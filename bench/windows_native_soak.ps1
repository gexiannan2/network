# Native Windows / VS2026 IOCP 10k connection soak.
# Run in Windows PowerShell 5.1+ after compiling the MSVC Release binaries.
param(
    [ValidateRange(1, 86400)][int]$DurationSeconds = 86400,
    [ValidateRange(1, 10000)][int]$Connections = 10000,
    [ValidateRange(1024, 65535)][int]$Port = 50525
)

$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT')
{
    throw 'This script must run on native Windows.'
}

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build-iocp-win3'
$serverExe = Join-Path $build 'net_server.exe'
$clientExe = Join-Path $build 'net_load_test.exe'
if (!(Test-Path $serverExe) -or !(Test-Path $clientExe))
{
    throw 'Build the native Windows MSVC Release executables first.'
}

$logDir = Join-Path (Join-Path $root 'bench/results') ('windows-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$serverStdout = Join-Path $logDir 'server.log'
$serverStderr = Join-Path $logDir 'server-stderr.log'
$clientStdout = Join-Path $logDir 'client.log'
$clientStderr = Join-Path $logDir 'client-stderr.log'
$shutdownMargin = [Math]::Min(90, [Math]::Max(15, [Math]::Ceiling($Connections / 120.0) + 15))
$serverDuration = [Math]::Min(172800, $DurationSeconds + $shutdownMargin)
$heartbeat = [Math]::Max(1, [Math]::Min(20, [Math]::Floor($DurationSeconds / 2)))
$serverArgs = @('--host','127.0.0.1','--port',"$Port",
                '--max-clients','10000','--io-threads','8',
                '--duration',"$serverDuration")
$clientArgs = @('--mode','hold','--host','127.0.0.1','--port',"$Port",
                '--connections',"$Connections",'--threads','16',
                '--duration',"$DurationSeconds",'--ramp-ms','8000',
                '--source-ips','16','--heartbeat-seconds',"$heartbeat",'--payload','128')

Write-Host "Native Windows IOCP soak logs: $logDir"
$server = Start-Process -FilePath $serverExe -ArgumentList $serverArgs -PassThru -NoNewWindow `
    -RedirectStandardOutput $serverStdout -RedirectStandardError $serverStderr
try
{
    Start-Sleep -Seconds 2
    if ($server.HasExited)
    {
        throw "Server exited during startup: $($server.ExitCode)"
    }

    $client = Start-Process -FilePath $clientExe -ArgumentList $clientArgs -PassThru -Wait -NoNewWindow `
        -RedirectStandardOutput $clientStdout -RedirectStandardError $clientStderr
    Get-Content $clientStdout -Tail 3
    $clientSummary = Get-Content $clientStdout -Tail 3 |
        Where-Object { $_ -match '^LOAD ' } | Select-Object -Last 1
    $echoMatch = [regex]::Match([string]$clientSummary, 'echo_messages=([0-9]+)')
    if (!$echoMatch.Success -or [int64]$echoMatch.Groups[1].Value -lt $Connections)
    {
        throw 'Heartbeat test was not exercised for every connection.'
    }
    if ($client.ExitCode -ne 0)
    {
        throw "Client validation failed with exit code $($client.ExitCode)"
    }

    # The server exits on its configured duration and prints SERVER_FINAL.
    if (!$server.WaitForExit(180000))
    {
        throw 'Server did not finish within the bounded shutdown interval.'
    }

    $final = Get-Content $serverStdout -Tail 8 | Where-Object { $_ -match '^SERVER_FINAL ' } | Select-Object -Last 1
    if (!$final)
    {
        throw 'Missing SERVER_FINAL statistics.'
    }
    Write-Host $final
    if ($final -notmatch 'active=0' -or !$final.Contains("peak_connections=$Connections ") -or $final -notmatch 'io_errors=0')
    {
        throw 'Soak acceptance check failed (active/peak/errors).'
    }

    Write-Host 'PASS: native Windows IOCP connection soak'
}
finally
{
    $server.Refresh()
    if (!$server.HasExited)
    {
        Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
    }
}
