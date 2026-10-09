[CmdletBinding()]
param(
    [ValidateSet('ecs','loading','all')][string]$Scene = 'all',
    [ValidateRange(3,30)][int]$Runs = 3,
    [string]$App = 'C:\tge\profile\Release\app.exe',
    [string]$TracyDirectory = 'C:\tge\tracy-0.13.1',
    [string]$AssetDirectory = 'C:\tge\lab1-assets',
    [string]$OutputDirectory = '',
    [switch]$IncludeUnboundedPump,
    [switch]$CompareGpuUpload,
    [switch]$CompareWorkers,
    [ValidateRange(1,256)][int]$BeforeWorkers = 4,
    [ValidateRange(1,256)][int]$AfterWorkers = 24,
    [ValidateRange(1,16)][int]$WorkerComparisonInFlight = 16,
    [string]$Node = 'node'
)
$ErrorActionPreference = 'Stop'
if ($CompareGpuUpload -and ($Scene -ne 'loading' -or $IncludeUnboundedPump)) {
    throw 'CompareGpuUpload requires -Scene loading and cannot combine with IncludeUnboundedPump.'
}
if ($CompareWorkers -and ($CompareGpuUpload -or $IncludeUnboundedPump -or $BeforeWorkers -eq $AfterWorkers)) {
    throw 'CompareWorkers needs distinct worker counts and cannot combine with feature-toggle experiments.'
}
Set-StrictMode -Version Latest
$project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $project ('reports\lab1\' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$App = (Resolve-Path -LiteralPath $App).Path
$capture = (Resolve-Path -LiteralPath (Join-Path $TracyDirectory 'tracy-capture.exe')).Path
$exporter = (Resolve-Path -LiteralPath (Join-Path $TracyDirectory 'tracy-csvexport.exe')).Path
if ((Test-Path -LiteralPath $OutputDirectory) -and (Get-ChildItem -LiteralPath $OutputDirectory | Select-Object -First 1)) {
    throw "Output directory must be new or empty: $OutputDirectory"
}
if ($Scene -ne 'ecs' -and -not (Test-Path -LiteralPath (Join-Path $AssetDirectory 'manifest.json'))) {
    throw 'Generate the assets first: tools\lab1\New-BenchmarkAssets.ps1'
}
if (Get-Process -Name app,tracy-capture -ErrorAction SilentlyContinue) {
    throw 'An app or tracy-capture process is already running. Close it before the reproducible run.'
}
$null = New-Item -ItemType Directory -Path $OutputDirectory -Force
$machineContext = [ordered]@{ collectionStatus='complete'; cpu=@(); detectedGpus=@(); os=$null; ramBytes=$null }
try {
    $machineContext.cpu = @(Get-CimInstance Win32_Processor | ForEach-Object {
        [ordered]@{ model=$_.Name.Trim(); physicalCores=[int]$_.NumberOfCores; logicalProcessors=[int]$_.NumberOfLogicalProcessors }
    })
    # Measure-Object treats OrderedDictionary entries differently in Windows PowerShell.
    $machineContext.physicalCores = 0
    $machineContext.logicalProcessors = 0
    foreach ($processor in $machineContext.cpu) {
        $machineContext.physicalCores += $processor.physicalCores
        $machineContext.logicalProcessors += $processor.logicalProcessors
    }
    $machineContext.ramBytes = [long](Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory
    $osInfo = Get-CimInstance Win32_OperatingSystem
    $machineContext.os = [ordered]@{ name=$osInfo.Caption; version=$osInfo.Version; build=$osInfo.BuildNumber; architecture=$osInfo.OSArchitecture }
    $machineContext.detectedGpus = @(Get-CimInstance Win32_VideoController | ForEach-Object {
        [ordered]@{ name=$_.Name; driverVersion=$_.DriverVersion }
    })
} catch {
    $machineContext.collectionStatus = 'partial / UNKNOWN fields'
    $machineContext.collectionError = $_.Exception.Message
}
$buildDirectory = Split-Path (Split-Path $App)
$cachePath = Join-Path $buildDirectory 'CMakeCache.txt'
$buildContext = [ordered]@{
    executableLastWriteUtc = (Get-Item -LiteralPath $App).LastWriteTimeUtc.ToString('o')
    executableBytes = (Get-Item -LiteralPath $App).Length
    cmakeCachePath = $cachePath; cmakeCacheValues = [ordered]@{}
    provenanceNote = 'Adjacent build cache and current source revision are context, not proof of which source bytes produced the executable. Executable SHA-256 identifies the measured binary.'
}
if (Test-Path -LiteralPath $cachePath) {
    # Only selected non-secret build settings, never environment/cache dumps.
    $selectedSettings = '^(CMAKE_GENERATOR|CMAKE_CXX_COMPILER|CMAKE_CXX_FLAGS_RELEASE|CMAKE_CXX_FLAGS_RELWITHDEBINFO|CMAKE_CONFIGURATION_TYPES|CMAKE_BUILD_TYPE|ENGINE_ENABLE_TRACY|TRACY_ENABLE|TRACY_ON_DEMAND):[^=]*=(.*)$'
    foreach ($line in (Get-Content -LiteralPath $cachePath | Where-Object { $_ -match $selectedSettings })) {
        if ($line -match $selectedSettings) { $buildContext.cmakeCacheValues[$Matches[1]] = $Matches[2] }
    }
}
if (Get-Command git -ErrorAction SilentlyContinue) {
    $revision = & git -C $project rev-parse HEAD 2>$null
    if ($LASTEXITCODE -eq 0) {
        $buildContext.observedSourceRevision = $revision
        $buildContext.observedSourceDirty = [bool](& git -C $project status --porcelain --untracked-files=normal)
    }
}
$manifest = [ordered]@{
    schema = 1; createdUtc = [DateTime]::UtcNow.ToString('o'); app = $App
    executableSha256 = (Get-FileHash -LiteralPath $App -Algorithm SHA256).Hash
    configuration = 'Release'; tracyVersion = '0.13.1'; scene = $Scene; runsPerMode = $Runs
    warmupSeconds = 3; captureSeconds = 15; windowSeconds = @(3,14)
    loadingWindowRelativeToEventSeconds = @(-1,5); loadAtSeconds = 6; autoExitSeconds = 18
    quantile = 'linear interpolation (R-7)'; order = 'AB on odd repetitions, BA on even repetitions'
    assetDirectory = [IO.Path]::GetFullPath($AssetDirectory); runs = @()
    includeUnboundedPump = [bool]$IncludeUnboundedPump
    compareGpuUpload = [bool]$CompareGpuUpload
    compareWorkers = [bool]$CompareWorkers
    beforeWorkers = $BeforeWorkers; afterWorkers = $AfterWorkers
    workerComparisonInFlight = $WorkerComparisonInFlight
    boundedUploadsPerFrame = 1; boundedUploadBudgetMs = 2
    ecsEntities = 4096; missingAsset = 0
    hardware = $machineContext; buildContext = $buildContext
}
if (Test-Path -LiteralPath (Join-Path $AssetDirectory 'manifest.json')) {
    $manifest.assetManifest = Get-Content -LiteralPath (Join-Path $AssetDirectory 'manifest.json') -Raw | ConvertFrom-Json
}
$manifestPath = Join-Path $OutputDirectory 'runs.json'
$environmentNames = @('TGE_LAB_SCENE','TGE_JOBS','TGE_ASYNC_LOADING','TGE_GPU_UPLOAD','TGE_BENCHMARK_ASSET_DIR','TGE_DEMO_SECONDS','TGE_LOAD_AT_SECONDS','TGE_WAIT_FOR_TRACY','TGE_UPLOADS_PER_FRAME','TGE_UPLOAD_BUDGET_MS','TGE_ECS_ENTITIES','TGE_MISSING_ASSET','TGE_JOB_WORKERS','TGE_ASSET_IN_FLIGHT')
$previous = @{}
foreach ($name in $environmentNames) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
function Save-Manifest { $manifest | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $manifestPath -Encoding UTF8 }
function Native-Argument([string]$Value) { return '"' + $Value + '"' }
function Export-Trace([string[]]$Arguments, [string]$Destination) {
    $process = Start-Process -FilePath $exporter -ArgumentList $Arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput $Destination -RedirectStandardError ($Destination + '.stderr.log')
    # Hold the native process handle before waiting; Windows PowerShell can otherwise lose ExitCode.
    $null = $process.Handle
    if (-not $process.WaitForExit(30000)) { throw "Exporter timed out (PID $($process.Id)); it was not forcibly terminated." }
    if ($null -eq $process.ExitCode -or $process.ExitCode -ne 0) { throw "CSV export failed or exit code unavailable: $Destination" }
}
try {
    $scenes = if ($Scene -eq 'all') { @('ecs','loading') } else { @($Scene) }
    foreach ($currentScene in $scenes) {
        for ($repeat = 1; $repeat -le $Runs; ++$repeat) {
            $modes = if ($repeat % 2 -eq 1) { @('before','after') } else { @('after','before') }
            if ($currentScene -eq 'loading' -and $IncludeUnboundedPump) {
                # Rotate the third mode's position to avoid always testing it last.
                $modes = switch ($repeat % 3) {
                    1 { @('before','after','legacy-pump') }
                    2 { @('after','legacy-pump','before') }
                    0 { @('legacy-pump','before','after') }
                }
            }
            foreach ($mode in $modes) {
                $id = '{0}-{1}-{2:00}' -f $currentScene,$mode,$repeat
                $runDirectory = Join-Path $OutputDirectory $id
                $null = New-Item -ItemType Directory -Path $runDirectory
                $trace = Join-Path $runDirectory 'trace.tracy'
                $env:TGE_LAB_SCENE = $currentScene
                # Each experiment changes only its own feature toggle.
                $env:TGE_JOBS = if (-not $CompareWorkers -and $currentScene -eq 'ecs' -and $mode -eq 'before') { '0' } else { '1' }
                $env:TGE_ASYNC_LOADING = if (-not $CompareWorkers -and -not $CompareGpuUpload -and $currentScene -eq 'loading' -and $mode -eq 'before') { '0' } else { '1' }
                # Preserve the original L1 experiment; copy queue is its own A/B.
                $env:TGE_GPU_UPLOAD = if ($CompareWorkers -or ($CompareGpuUpload -and $mode -eq 'after')) { '1' } else { '0' }
                $env:TGE_JOB_WORKERS = if ($CompareWorkers) { if ($mode -eq 'before') { "$BeforeWorkers" } else { "$AfterWorkers" } } else { '0' }
                $env:TGE_ASSET_IN_FLIGHT = if ($CompareWorkers) { "$WorkerComparisonInFlight" } else { '0' }
                $env:TGE_BENCHMARK_ASSET_DIR = $manifest.assetDirectory
                $env:TGE_DEMO_SECONDS = '18'
                $env:TGE_LOAD_AT_SECONDS = '6'
                $env:TGE_WAIT_FOR_TRACY = '1'
                $env:TGE_ECS_ENTITIES = '4096'; $env:TGE_MISSING_ASSET = '0'
                $env:TGE_UPLOADS_PER_FRAME = if ($mode -eq 'legacy-pump') { '65536' } else { '1' }
                $env:TGE_UPLOAD_BUDGET_MS = if ($mode -eq 'legacy-pump') { '60000' } else { '2' }
                $record = [ordered]@{ id=$id; scene=$currentScene; mode=$mode; repeat=$repeat; jobs=$env:TGE_JOBS; asyncLoading=$env:TGE_ASYNC_LOADING; gpuUpload=$env:TGE_GPU_UPLOAD; requestedWorkers=[int]$env:TGE_JOB_WORKERS; assetInFlight=[int]$env:TGE_ASSET_IN_FLIGHT; uploadsPerFrame=$env:TGE_UPLOADS_PER_FRAME; uploadBudgetMs=$env:TGE_UPLOAD_BUDGET_MS; startedUtc=[DateTime]::UtcNow.ToString('o'); trace="$id/trace.tracy"; zones="$id/zones.csv"; messages="$id/messages.csv"; status='running' }
                $manifest.runs += $record
                Save-Manifest
                Write-Host "Capturing $id workers=$env:TGE_JOB_WORKERS (3 s warmup inside a 15 s trace; application exits itself)."
                if ((Get-FileHash -LiteralPath $App -Algorithm SHA256).Hash -ne $manifest.executableSha256) { throw 'Measured executable changed between runs. Start a fresh experiment.' }
                $appProcess = Start-Process -FilePath $App -WorkingDirectory (Split-Path $App) -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $runDirectory 'app.stdout.log') -RedirectStandardError (Join-Path $runDirectory 'app.stderr.log')
                $null = $appProcess.Handle
                $record.appPid = $appProcess.Id
                $readyTimer = [Diagnostics.Stopwatch]::StartNew()
                $ready = $false
                while ($readyTimer.Elapsed.TotalSeconds -lt 20) {
                    $appProcess.Refresh()
                    if ($appProcess.HasExited) { throw "Application exited before profiler readiness: $id." }
                    $startupLog = Get-Content -LiteralPath (Join-Path $runDirectory 'app.stdout.log') -Raw -ErrorAction SilentlyContinue
                    if ($startupLog -match 'LAB_READY_FOR_TRACY') { $ready = $true; break }
                    Start-Sleep -Milliseconds 50
                }
                if (-not $ready) { throw "Profiler readiness timeout in ${id}; app PID $($appProcess.Id) was not forcibly terminated." }
                $captureProcess = Start-Process -FilePath $capture -ArgumentList @('-a','127.0.0.1','-s','15','-o',(Native-Argument $trace)) -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $runDirectory 'capture.stdout.log') -RedirectStandardError (Join-Path $runDirectory 'capture.stderr.log')
                $null = $captureProcess.Handle
                $record.capturePid = $captureProcess.Id
                if (-not $captureProcess.WaitForExit(30000)) { throw "Capture timeout: PID $($captureProcess.Id). Process was not killed." }
                if (-not $appProcess.WaitForExit(30000)) { throw "Graceful shutdown timeout: PID $($appProcess.Id). Process was not killed." }
                $record.appExitCode = $appProcess.ExitCode
                $record.captureExitCode = $captureProcess.ExitCode
                if ($null -eq $record.appExitCode -or $null -eq $record.captureExitCode -or $record.appExitCode -ne 0 -or $record.captureExitCode -ne 0) { throw "Run failed or exit code unavailable: $id (app=$($record.appExitCode), capture=$($record.captureExitCode))." }
                $logs = (Get-Content -LiteralPath (Join-Path $runDirectory 'app.stdout.log'),(Join-Path $runDirectory 'app.stderr.log') -Raw -ErrorAction SilentlyContinue) -join "`n"
                if ($logs -match 'LAB_RUN_START scene=(\S+) parallel_ecs=(\S+) async_loading=(\S+) entities=(\d+) workers=(\d+)') {
                    $record.runtimeConfig = [ordered]@{ scene=$Matches[1]; parallelEcs=$Matches[2]; asyncLoading=$Matches[3]; entities=[int]$Matches[4]; workers=[int]$Matches[5] }
                } else { throw "Actual worker/configuration log missing in $id." }
                if ($CompareWorkers) {
                    if ($record.runtimeConfig.workers -ne $record.requestedWorkers -or
                        $record.runtimeConfig.parallelEcs -ne 'true' -or $record.runtimeConfig.asyncLoading -ne 'true') {
                        throw "Requested worker configuration was not applied in $id."
                    }
                    if ($logs -match 'LAB_RUN_START[^\r\n]+asset_in_flight=(\d+)') {
                        $record.runtimeConfig['assetInFlight'] = [int]$Matches[1]
                    } else { throw "Asset in-flight configuration log missing in $id." }
                    if ($record.runtimeConfig.assetInFlight -ne $WorkerComparisonInFlight) {
                        throw "Fixed asset in-flight limit was not applied in $id."
                    }
                }
                if ($logs -notmatch 'Application shutdown complete') { throw "Graceful shutdown marker missing in $id." }
                if ($logs -match '\[error\]|Diligent Engine:\s*(ERROR|Error|Fatal)|Debug assertion failed') { throw "Engine errors found in $id." }
                if (($CompareGpuUpload -and $mode -eq 'after') -or $CompareWorkers) {
                    if ($logs -notmatch 'GPU_UPLOAD_QUEUE enabled' -or
                        ($currentScene -eq 'loading' -and [regex]::Matches($logs, 'via GPU transfer:').Count -lt 16)) {
                        throw "Dedicated GPU transfer path was not actually used for all 16 assets in $id."
                    }
                }
                if ($currentScene -eq 'loading') {
                    if ($logs -notmatch 'LAB_LOAD_COMPLETE elapsed=\S+ loaded=(\d+) failed=(\d+) cancelled=(\d+)') { throw "Loading did not complete in $id." }
                    if ([int]$Matches[1] -lt 16 -or [int]$Matches[2] -ne 0 -or [int]$Matches[3] -ne 0) { throw "Unexpected resource counts in ${id}: $($Matches[0])" }
                }
                if (-not (Test-Path -LiteralPath $trace)) { throw "Trace missing: $trace" }
                Export-Trace @('-u',(Native-Argument $trace)) (Join-Path $runDirectory 'zones.csv')
                Export-Trace @('-m',(Native-Argument $trace)) (Join-Path $runDirectory 'messages.csv')
                $record.status = 'complete'
                $record.finishedUtc = [DateTime]::UtcNow.ToString('o')
                Save-Manifest
            }
        }
    }
    & $Node (Join-Path $PSScriptRoot 'analyze-traces.mjs') $manifestPath
    if ($LASTEXITCODE -ne 0) { throw "Trace analysis failed ($LASTEXITCODE). Raw traces remain in $OutputDirectory." }
    Write-Host "Ready: $(Join-Path $OutputDirectory 'measurements.md')"
} catch {
    $manifest.failure = $_.Exception.Message
    if ($manifest.runs.Count -gt 0 -and $manifest.runs[-1].status -eq 'running') { $manifest.runs[-1].status = 'failed' }
    Save-Manifest
    throw
} finally {
    foreach ($name in $environmentNames) { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') }
}
