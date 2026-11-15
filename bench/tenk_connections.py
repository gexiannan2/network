#!/usr/bin/env python3
"""Standard-library TCP Echo load test with scalable asynchronous clients.

All clients are connected before traffic begins. Unlike net_client, this does
not require one OS thread per connection. This is NOT an MMO simulation.
"""

import argparse
import asyncio
import errno
import math
import resource
import struct
import sys
import time


def percentile(values, ratio):
    if not values:
        return 0.0
    ordered = sorted(values)
    position = (len(ordered) - 1) * ratio
    lo = math.floor(position)
    hi = math.ceil(position)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (position - lo)


async def connect_one(index, args, semaphore):
    async with semaphore:
        attempts = 3 if args.source_start_port > 0 else 1
        for attempt in range(attempts):
            local_address = None
            if args.source_start_port > 0:
                local_address = (
                    "127.0.0.1",
                    args.source_start_port + index + attempt * args.connections,
                )
            elif args.source_ips > 0:
                local_address = (f"127.0.0.{2 + index % args.source_ips}", 0)

            try:
                reader, writer = await asyncio.wait_for(
                    asyncio.open_connection(
                        args.host, args.port, local_addr=local_address,
                    ),
                    timeout=args.connect_timeout,
                )
                return index, reader, writer, None
            except OSError as error:
                if (
                    args.source_start_port > 0
                    and error.errno == errno.EADDRINUSE
                    and attempt + 1 < attempts
                ):
                    continue
                return index, None, None, str(error)
            except asyncio.TimeoutError as error:
                return index, None, None, str(error)

        return index, None, None, "all source ports unavailable"


async def simulate_client(index, reader, writer, args, start, end):
    loop = asyncio.get_running_loop()
    period = 1.0 / args.rate
    next_send = start + ((index % 997) / 997.0) * period
    body_tail = bytes(max(0, args.payload - 16))
    messages = 0
    samples = []
    errors = 0
    while next_send < end:
        await asyncio.sleep(max(0.0, next_send - loop.time()))
        if loop.time() >= end:
            break
        body = struct.pack("!QQ", index, messages) + body_tail
        wire = struct.pack("!I", len(body)) + body
        sent_at = time.monotonic_ns()
        try:
            writer.write(wire)
            await asyncio.wait_for(writer.drain(), timeout=args.timeout)
            header = await asyncio.wait_for(reader.readexactly(4), timeout=args.timeout)
            length = struct.unpack("!I", header)[0]
            if length != len(body):
                raise ValueError("invalid Echo response length")
            echo = await asyncio.wait_for(reader.readexactly(length), timeout=args.timeout)
            if echo != body:
                raise ValueError("Echo payload mismatch")
        except (OSError, asyncio.TimeoutError, asyncio.IncompleteReadError, ValueError):
            errors += 1
            break
        messages += 1
        if messages % args.sample_every == 0:
            samples.append((time.monotonic_ns() - sent_at) / 1e6)
        next_send += period
    return messages, errors, samples


async def run(args):
    limit_soft, limit_hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    needed = args.connections + 128
    if limit_soft < needed and limit_hard >= needed:
        resource.setrlimit(resource.RLIMIT_NOFILE, (needed, limit_hard))

    loop = asyncio.get_running_loop()
    semaphore = asyncio.Semaphore(args.connect_parallel)
    started = time.monotonic()
    pending = [
        asyncio.create_task(connect_one(i, args, semaphore))
        for i in range(args.connections)
    ]
    connections = await asyncio.gather(*pending)
    connected = [(i, reader, writer) for i, reader, writer, error in connections
                 if error is None]
    failures = [(i, error) for i, reader, writer, error in connections
                if error is not None]
    connect_seconds = time.monotonic() - started
    print(
        f"CONNECT requested={args.connections} established={len(connected)} "
        f"failed={len(failures)} connect_seconds={connect_seconds:.3f} "
        f"fd_soft={resource.getrlimit(resource.RLIMIT_NOFILE)[0]}",
        flush=True,
    )
    if failures:
        print(f"CONNECT_FAILURE_SAMPLES {failures[:5]}", flush=True)
    # Hold every successful socket open for the entire traffic interval.
    await asyncio.sleep(args.settle)
    start = loop.time() + 0.1
    end = start + args.duration
    results = []
    if args.rate > 0:
        workers = [
            asyncio.create_task(simulate_client(i, reader, writer, args, start, end))
            for i, reader, writer in connected
        ]
        results = await asyncio.gather(*workers)
    else:
        await asyncio.sleep(args.duration)

    elapsed = max(0.001, loop.time() - start)
    completed = sum(count for count, errors, samples in results)
    errors = sum(errors for count, errors, samples in results)
    latencies = [s for count, errors, samples in results for s in samples]
    print(
        f"RESULT connected={len(connected)} requested={args.connections} "
        f"duration_s={args.duration} measured_s={elapsed:.3f} "
        f"target_msg_per_sec={len(connected) * args.rate:.0f} "
        f"completed_messages={completed} req_per_sec={completed / elapsed:.1f} "
        f"io_errors={errors} connect_errors={len(failures)} "
        f"p50_ms={percentile(latencies, 0.50):.3f} "
        f"p95_ms={percentile(latencies, 0.95):.3f} "
        f"p99_ms={percentile(latencies, 0.99):.3f} "
        f"latency_samples={len(latencies)}",
        flush=True,
    )
    for i, reader, writer in connected:
        writer.close()
    await asyncio.gather(*(writer.wait_closed() for _, _, writer in connected),
                         return_exceptions=True)
    return 0 if len(connected) == args.connections and errors == 0 else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=50114)
    parser.add_argument("--connections", type=int, default=10000)
    parser.add_argument("--duration", type=float, default=10)
    parser.add_argument("--rate", type=float, default=1,
                        help="messages per second per connection (0=idle only)")
    parser.add_argument("--payload", type=int, default=128)
    parser.add_argument("--connect-parallel", type=int, default=128)
    parser.add_argument("--source-ips", type=int, default=0,
                        help="loopback IP pool size; e.g. 8 for 2048 ephemeral ports")
    parser.add_argument("--source-start-port", type=int, default=0,
                        help="bind one loopback IP to sequential local ports, e.g. 22000")
    parser.add_argument("--connect-timeout", type=float, default=10)
    parser.add_argument("--timeout", type=float, default=10)
    parser.add_argument("--settle", type=float, default=1.0)
    parser.add_argument("--sample-every", type=int, default=1)
    args = parser.parse_args()
    if not 16 <= args.payload <= 65536 or args.rate < 0 or args.connections < 1 or args.source_ips > 200 or (
        args.source_start_port > 0 and
        args.source_start_port + args.connections * 3 - 1 > 65535
    ):
        parser.error("payload must be 16..65536, rate >= 0, connections >= 1")
    return asyncio.run(run(args))


if __name__ == "__main__":
    sys.exit(main())
