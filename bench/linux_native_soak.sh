#!/usr/bin/env bash
# Native Linux 10k-connection soak. Requires the real host kernel; WSL2
# is explicitly rejected unless ALLOW_WSL=1 (development-only results).
set -euo pipefail

DURATION_SECONDS="${1:-86400}"
HEARTBEAT_SECONDS=$(( DURATION_SECONDS / 2 ))
if (( HEARTBEAT_SECONDS > 20 )); then HEARTBEAT_SECONDS=20; fi
if (( HEARTBEAT_SECONDS < 1 )); then HEARTBEAT_SECONDS=1; fi
CONNECTIONS="${2:-10000}"
PORT="${3:-50524}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build-native-linux}"
LOG_DIR="${LOG_DIR:-$ROOT/bench/results/linux-$(date +%Y%m%d-%H%M%S)}"
THREADS="${IO_THREADS:-8}"

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "This test requires Linux." >&2
    exit 2
fi
if grep -Eqi '(microsoft|wsl)' /proc/sys/kernel/osrelease && [[ "${ALLOW_WSL:-0}" != "1" ]]; then
    echo "WSL2 is not native Linux. Use ALLOW_WSL=1 for explicitly labeled development tests." >&2
    exit 3
fi
for value in "$DURATION_SECONDS" "$CONNECTIONS" "$PORT" "$THREADS"; do
    if [[ ! "$value" =~ ^[0-9]+$ ]]; then
        echo "Invalid numeric argument: $value" >&2
        exit 2
    fi
done
if (( CONNECTIONS < 1 || CONNECTIONS > 10000 || DURATION_SECONDS < 1 || DURATION_SECONDS > 86400 )); then
    echo "Duration must be 1..86400 and connections 1..10000." >&2
    exit 2
fi
ulimit -n 65535
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" --parallel "${JOBS:-16}"
mkdir -p "$LOG_DIR"

server_pid=""
monitor_pid=""
cleanup()
{
    if [[ -n "$monitor_pid" ]]; then
        kill "$monitor_pid" 2>/dev/null || true
        wait "$monitor_pid" 2>/dev/null || true
    fi
    if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
        kill -INT "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT

"$BUILD/net_server" --host 127.0.0.1 --port "$PORT" --io-threads "$THREADS" \
    --max-clients 10000 --duration 0 >"$LOG_DIR/server.log" 2>&1 &
server_pid=$!
for attempt in {1..50}; do
    if [[ -f "$LOG_DIR/server.log" ]] && grep -q '^READY ' "$LOG_DIR/server.log"; then
        break
    fi
    if ! kill -0 "$server_pid" 2>/dev/null; then
        echo "Server exited during startup." >&2
        cat "$LOG_DIR/server.log" >&2
        exit 4
    fi
    sleep 0.2
done
if ! grep -q '^READY ' "$LOG_DIR/server.log"; then
    echo "Server readiness timeout." >&2
    exit 4
fi

(
    while kill -0 "$server_pid" 2>/dev/null; do
        printf '%s ' "$(date -Is)"
        ps -o pid=,etime=,%cpu=,rss=,vsz= -p "$server_pid" || true
        sleep 60
    done
) >"$LOG_DIR/resource-samples.log" 2>&1 &
monitor_pid=$!

echo "Test running on $(uname -a) ; logs: $LOG_DIR"
client_exit=0
"$BUILD/net_load_test" --mode hold --host 127.0.0.1 --port "$PORT" \
    --connections "$CONNECTIONS" --threads 16 --duration "$DURATION_SECONDS" \
    --ramp-ms 8000 --source-ips 16 \
    --heartbeat-seconds "$HEARTBEAT_SECONDS" --payload 128 >"$LOG_DIR/client.log" 2>&1 || client_exit=$?

kill -INT "$server_pid" 2>/dev/null || true
server_exit=0
wait "$server_pid" || server_exit=$?
server_pid=""
echo "--- client ---"
tail -n 3 "$LOG_DIR/client.log"
echo "--- server ---"
tail -n 3 "$LOG_DIR/server.log"
if (( client_exit != 0 )); then
    echo "FAIL: client exited $client_exit" >&2
    exit "$client_exit"
fi
echoed="$(grep -oE 'echo_messages=[0-9]+' "$LOG_DIR/client.log" | tail -n 1 | cut -d= -f2)"
if [[ -z "$echoed" ]] || (( echoed < CONNECTIONS )); then
    echo "FAIL: no validated echo for every connected client" >&2
    exit 7
fi
if (( server_exit != 0 )); then
    echo "FAIL: server exited $server_exit" >&2
    exit "$server_exit"
fi
if grep -Eq 'SERVER_FINAL .*io_errors=[1-9]' "$LOG_DIR/server.log"; then
    echo "FAIL: server reported I/O errors" >&2
    exit 5
fi
if ! grep -q 'SERVER_FINAL .*active=0' "$LOG_DIR/server.log"; then
    echo "FAIL: no clean SERVER_FINAL with active=0" >&2
    exit 5
fi
if ! grep -q "peak_connections=$CONNECTIONS" "$LOG_DIR/server.log"; then
    echo "FAIL: did not reach $CONNECTIONS simultaneous connections" >&2
    exit 6
fi
if grep -Eqi '(microsoft|wsl)' /proc/sys/kernel/osrelease; then
    echo "PASS: WSL2 Linux development smoke only (NOT native Linux certification)"
else
    echo "PASS: native Linux network-layer connection soak"
fi
