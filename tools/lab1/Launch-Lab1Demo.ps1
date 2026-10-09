[CmdletBinding()]
param(
    [ValidateSet('ecs','loading')][string]$Scene = 'loading',
    [ValidateSet('before','after','legacy-pump')][string]$Mode = 'after',
    [ValidateRange(1,3600)][double]$Seconds = 60,
    [string]$App = 'C:\tge\profile\Release\app.exe',
    [string]$AssetDirectory = 'C:\tge\lab1-assets',
    [switch]$Tracy,
    [switch]$GpuUpload,
    [ValidateRange(0,256)][int]$Workers = 0,
    [ValidateRange(0,16)][int]$AssetInFlight = 0
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$App = (Resolve-Path -LiteralPath $App).Path
if ($GpuUpload -and ($Scene -ne 'loading' -or $Mode -eq 'legacy-pump')) { throw 'GpuUpload requires loading / before or after.' }
if ($Scene -eq 'ecs' -and $Mode -eq 'legacy-pump') { throw 'legacy-pump applies only to the loading scene.' }
if ($Scene -eq 'loading' -and -not (Test-Path -LiteralPath (Join-Path $AssetDirectory 'manifest.json'))) {
    throw 'Generate assets first with tools\lab1\New-BenchmarkAssets.ps1, or pass the prepared -AssetDirectory.'
}
if (Get-Process -Name app,tracy-capture -ErrorAction SilentlyContinue) {
    throw 'An engine or measurement capture is already running. Wait for it to finish before starting the live demo.'
}
$names = @('TGE_LAB_SCENE','TGE_JOBS','TGE_ASYNC_LOADING','TGE_GPU_UPLOAD','TGE_BENCHMARK_ASSET_DIR','TGE_DEMO_SECONDS','TGE_LOAD_AT_SECONDS','TGE_WAIT_FOR_TRACY','TGE_UPLOADS_PER_FRAME','TGE_UPLOAD_BUDGET_MS','TGE_ECS_ENTITIES','TGE_MISSING_ASSET','TGE_JOB_WORKERS','TGE_ASSET_IN_FLIGHT')
$previous = @{}
foreach ($name in $names) { $previous[$name] = [Environment]::GetEnvironmentVariable($name,'Process') }
try {
    $env:TGE_LAB_SCENE = $Scene
    $env:TGE_JOB_WORKERS = "$Workers"
    $env:TGE_ASSET_IN_FLIGHT = "$AssetInFlight"
    $env:TGE_JOBS = if ($Scene -eq 'ecs' -and $Mode -eq 'before') { '0' } else { '1' }
    $env:TGE_ASYNC_LOADING = if (-not $GpuUpload -and $Scene -eq 'loading' -and $Mode -eq 'before') { '0' } else { '1' }
    $env:TGE_GPU_UPLOAD = if ($GpuUpload -and $Mode -eq 'after') { '1' } else { '0' }
    $env:TGE_BENCHMARK_ASSET_DIR = [IO.Path]::GetFullPath($AssetDirectory)
    $env:TGE_DEMO_SECONDS = $Seconds.ToString([Globalization.CultureInfo]::InvariantCulture)
    $env:TGE_LOAD_AT_SECONDS = '6'
    $env:TGE_WAIT_FOR_TRACY = if ($Tracy) { '1' } else { '0' }
    $env:TGE_UPLOADS_PER_FRAME = if ($Mode -eq 'legacy-pump') { '65536' } else { '1' }
    $env:TGE_UPLOAD_BUDGET_MS = if ($Mode -eq 'legacy-pump') { '60000' } else { '2' }
    $env:TGE_ECS_ENTITIES = '4096'; $env:TGE_MISSING_ASSET = '0'
    if ($Tracy) {
        Write-Host 'Open the existing Tracy 0.13.1 GUI and Connect to 127.0.0.1 immediately. The app waits up to 10 seconds after initialization.'
    }
    # A visible application is intentional for the requested live defense demo.
    $process = Start-Process -FilePath $App -WorkingDirectory (Split-Path $App) -WindowStyle Normal -PassThru
    $null = $process.Handle
    Write-Host "Live demo: $Scene / $Mode; PID=$($process.Id); automatic exit after $Seconds seconds of the run."
    if ($Scene -eq 'loading') {
        if ($GpuUpload) { Write-Host 'GPU upload showcase: CPU decode is async in both modes; before = main upload, after = dedicated transfer + GPU fence. Check Upload mode in the overlay.' }
        else { Write-Host 'Loading starts at +6 seconds. before = controlled synchronous ablation; after = async with bounded GPU pump; legacy-pump = unbounded-drain emulation.' }
    }
    [pscustomobject]@{ ProcessId=$process.Id; Scene=$Scene; Mode=$Mode; Seconds=$Seconds; TracyGate=[bool]$Tracy }
} finally {
    # Child environment was copied at launch; restore this shell immediately.
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name,$previous[$name],'Process') }
}
