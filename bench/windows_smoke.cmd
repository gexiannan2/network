@echo off
setlocal EnableExtensions
rem Windows WSAPoll regression: Echo/protocol smoke only, not 10K MMO proof.
set "ROOT=%~dp0.."
set "BIN=%ROOT%\build-original-network"
set "PORT=42024"
set "LOG=%TEMP%\memorydeal-windows-smoke-42024.log"
if not exist "%BIN%\net_server.exe" exit /b 2
if not exist "%BIN%\net_protocol_test.exe" exit /b 2
if not exist "%BIN%\net_client.exe" exit /b 2
start "" /B "%BIN%\net_server.exe" --host 127.0.0.1 --port %PORT% --io-threads 4 --max-clients 1024 --duration 9 > "%LOG%" 2>&1
"%SystemRoot%\System32\PING.EXE" -n 3 127.0.0.1 > nul
"%SystemRoot%\System32\FINDSTR.EXE" /B "READY" "%LOG%" > nul
if errorlevel 1 (
    type "%LOG%"
    exit /b 2
)
"%BIN%\net_protocol_test.exe" --port %PORT%
if errorlevel 1 exit /b 1
"%BIN%\net_client.exe" --host 127.0.0.1 --port %PORT% --connections 64 --duration 3 --payload 256 --pipeline 4
if errorlevel 1 exit /b 1
"%SystemRoot%\System32\PING.EXE" -n 7 127.0.0.1 > nul
type "%LOG%"
exit /b 0
