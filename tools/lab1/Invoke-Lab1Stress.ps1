[CmdletBinding()]
param(
    [string]$App = 'C:\tge\profile\Release\app.exe',
    [string]$AssetDirectory = 'C:\tge\lab1-assets',
    [string]$OutputDirectory = '',
    [ValidateRange(30,3600)][int]$LongSessionSeconds = 60,
    [ValidateRange(2,20)][int]$RepeatedStarts = 3,
    [switch]$GpuUpload,
    [ValidateRange(0,256)][int]$Workers = 0
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$App = (Resolve-Path -LiteralPath $App).Path
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $project ('reports\lab1\stress-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if ((Test-Path -LiteralPath $OutputDirectory) -and (Get-ChildItem -LiteralPath $OutputDirectory | Select-Object -First 1)) { throw 'Output directory must be new or empty.' }
if (Get-Process -Name app -ErrorAction SilentlyContinue) { throw 'An app is already running. Close it before stress checks.' }
if (-not (Test-Path -LiteralPath (Join-Path $AssetDirectory 'manifest.json'))) { throw 'Generate benchmark assets first.' }
$null = New-Item -ItemType Directory -Path $OutputDirectory -Force
$missing = Join-Path $OutputDirectory 'intentionally-absent-assets'
$names = @('TGE_LAB_SCENE','TGE_JOBS','TGE_ASYNC_LOADING','TGE_GPU_UPLOAD','TGE_BENCHMARK_ASSET_DIR','TGE_DEMO_SECONDS','TGE_LOAD_AT_SECONDS','TGE_WAIT_FOR_TRACY','TGE_UPLOADS_PER_FRAME','TGE_UPLOAD_BUDGET_MS','TGE_ECS_ENTITIES','TGE_MISSING_ASSET','TGE_JOB_WORKERS','TGE_ASSET_IN_FLIGHT')
$previous = @{}
foreach ($name in $names) { $previous[$name] = [Environment]::GetEnvironmentVariable($name,'Process') }
$scenarios = @(
    @{ name='missing-resources'; duration=5; loadAt=1; assets=$missing; expectFailureLog=$true },
    @{ name='exit-with-live-loading'; duration=1.01; loadAt=1; assets=$AssetDirectory; expectFailureLog=$false },
    @{ name='long-session'; duration=$LongSessionSeconds; loadAt=2; assets=$AssetDirectory; expectFailureLog=$false }
)
for ($i=1; $i -le $RepeatedStarts; ++$i) { $scenarios += @{ name=('repeated-start-{0:00}' -f $i); duration=8; loadAt=1; assets=$AssetDirectory; expectFailureLog=$false } }
$results = @()
try {
    foreach ($scenario in $scenarios) {
        $env:TGE_LAB_SCENE='loading'; $env:TGE_JOBS='1'; $env:TGE_ASYNC_LOADING='1'; $env:TGE_WAIT_FOR_TRACY='0'
        $env:TGE_JOB_WORKERS="$Workers"; $env:TGE_ASSET_IN_FLIGHT='0'
        $env:TGE_GPU_UPLOAD = if ($GpuUpload) { '1' } else { '0' }
        $env:TGE_UPLOADS_PER_FRAME='1'; $env:TGE_UPLOAD_BUDGET_MS='2'
        $env:TGE_ECS_ENTITIES='4096'; $env:TGE_MISSING_ASSET='0'
        $env:TGE_BENCHMARK_ASSET_DIR=[IO.Path]::GetFullPath($scenario.assets)
        $env:TGE_DEMO_SECONDS=$scenario.duration.ToString([Globalization.CultureInfo]::InvariantCulture)
        $env:TGE_LOAD_AT_SECONDS=$scenario.loadAt.ToString([Globalization.CultureInfo]::InvariantCulture)
        $stdout = Join-Path $OutputDirectory ($scenario.name + '.stdout.log')
        $stderr = Join-Path $OutputDirectory ($scenario.name + '.stderr.log')
        Write-Host "Stress: $($scenario.name)"
        $timer = [Diagnostics.Stopwatch]::StartNew()
        $process = Start-Process -FilePath $App -WorkingDirectory (Split-Path $App) -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        $null = $process.Handle
        $exited = $process.WaitForExit([int](($scenario.duration + 30) * 1000))
        $timer.Stop()
        $record = [ordered]@{ name=$scenario.name; requestedRunSeconds=$scenario.duration; elapsedProcessSeconds=$timer.Elapsed.TotalSeconds; pid=$process.Id; gracefulExit=$exited; exitCode=$null; passed=$false; notes='' }
        if ($exited) {
            $record.exitCode=$process.ExitCode
            $logs=(Get-Content -LiteralPath $stdout,$stderr -Raw -ErrorAction SilentlyContinue) -join "`n"
            $record.passed=($null -ne $record.exitCode -and $record.exitCode -eq 0)
            $record.shutdownMarkerFound=($logs -match 'Application shutdown complete')
            $record.passed=$record.passed -and $record.shutdownMarkerFound
            $record.gpuUploadRequested = [bool]$GpuUpload
            if ($GpuUpload) {
                $record.transferQueueEnabled = ($logs -match 'GPU_UPLOAD_QUEUE enabled')
                $record.passed = $record.passed -and $record.transferQueueEnabled
            }
            $resourceErrors = @($logs -split "`n" | Where-Object {
                $_ -match '(Texture|Mesh) (load|transfer) failed' -and $_ -notmatch 'cancelled during shutdown'
            })
            if ($scenario.expectFailureLog) {
                $record.expectedMissingResourceLog=($resourceErrors.Count -gt 0)
                $record.passed=$record.passed -and $record.expectedMissingResourceLog
            } else {
                $record.unexpectedResourceErrors=($resourceErrors.Count -gt 0)
                $record.passed=$record.passed -and -not $record.unexpectedResourceErrors
            }
            if ($scenario.name -eq 'exit-with-live-loading') {
                $record.liveWorkAtShutdown=$false
                if ($logs -match 'LAB_SHUTDOWN pending_cpu=(\d+) pending_uploads=(\d+)') {
                    $record.pendingCpuAtShutdown=[int]$Matches[1]; $record.pendingUploadsAtShutdown=[int]$Matches[2]
                    $record.liveWorkAtShutdown=($record.pendingCpuAtShutdown + $record.pendingUploadsAtShutdown -gt 0)
                }
                $record.passed=$record.passed -and $record.liveWorkAtShutdown
            } elseif (-not $scenario.expectFailureLog) {
                $record.loadingCompletedCleanly=$false
                if ($logs -match 'LAB_LOAD_COMPLETE elapsed=\S+ loaded=(\d+) failed=(\d+) cancelled=(\d+)') {
                    $record.loadingCompletedCleanly=([int]$Matches[1] -ge 16 -and [int]$Matches[2] -eq 0 -and [int]$Matches[3] -eq 0)
                }
                $record.passed=$record.passed -and $record.loadingCompletedCleanly
            }
            $record.notes='Checks process exit, explicit completed shutdown, resource errors/completion; early-exit additionally requires logged pending work > 0.'
        } else { $record.notes='Timeout: process was NOT forcibly terminated. Inspect this PID before continuing.' }
        $results += $record
        $results | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'stress-results.json') -Encoding UTF8
        if (-not $record.passed) { throw "Stress scenario failed: $($scenario.name). $($record.notes)" }
    }
    Write-Host "Passed $($results.Count) smoke scenarios. Results: $OutputDirectory"
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name,$previous[$name],'Process') }
}
