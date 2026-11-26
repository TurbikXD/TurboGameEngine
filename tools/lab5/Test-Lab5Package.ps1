#requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$GameZip,
    [Parameter(Mandatory)]
    [string]$SymbolsZip,
    [string]$EvidenceDirectory,
    [string]$ExpectedCrashReportUrl,
    [ValidateRange(5, 300)]
    [int]$TimeoutSeconds = 90,
    [switch]$CheckStack,
    [string]$Cdb
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Lab5PackageFiles.ps1')

Assert-Lab5File $GameZip
Assert-Lab5File $SymbolsZip
$work = Join-Path ([IO.Path]::GetTempPath()) "tge-lab5-$([guid]::NewGuid().ToString('N'))"
$repository = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
if (![IO.Path]::GetRelativePath($repository, $work).StartsWith('..\') -and
    [IO.Path]::GetPathRoot($repository) -eq [IO.Path]::GetPathRoot($work)) {
    throw 'Package verification needs TEMP outside the repository.'
}
$game = Join-Path $work 'Beacon game'
$symbols = Join-Path $work 'symbols'
$reports = Join-Path $work 'crash-reports'
[void][IO.Directory]::CreateDirectory($reports)
Write-Host "Extracted packages and test reports: $work"
[IO.Compression.ZipFile]::ExtractToDirectory((Resolve-Path -LiteralPath $GameZip).Path, $game)
[IO.Compression.ZipFile]::ExtractToDirectory((Resolve-Path -LiteralPath $SymbolsZip).Path, $symbols)
foreach ($entry in $Lab5GameFiles) { Assert-Lab5File (Join-Path $game $entry) }
Assert-Lab5File (Join-Path $symbols 'app.pdb')
if (Get-ChildItem -LiteralPath $game -Filter '*.pdb' -Recurse -File) {
    throw 'Symbol files were included in the game ZIP.'
}
if ($ExpectedCrashReportUrl) {
    $reportUrlPath = Join-Path $game 'crash-report-url.txt'
    Assert-Lab5File $reportUrlPath
    if ((Get-Content -Raw -LiteralPath $reportUrlPath).Trim() -cne ([uri]$ExpectedCrashReportUrl).AbsoluteUri) {
        throw 'The game ZIP contains a different crash report URL.'
    }
}
$buildInfoPath = Join-Path $game 'build.json'
$build = Get-Lab5BuildInfo $buildInfoPath
Assert-Lab5SameBuild $build (Get-Lab5BuildInfo (Join-Path $symbols 'build.json')) $SymbolsZip

function Invoke-Lab5App([string[]]$AppArguments, [string]$LogName) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = Join-Path $game 'app.exe'
    $start.WorkingDirectory = $game
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [Text.Encoding]::UTF8
    $start.StandardErrorEncoding = [Text.Encoding]::UTF8
    foreach ($key in @($start.Environment.Keys | Where-Object { $_ -like 'TGE_*' })) {
        [void]$start.Environment.Remove($key)
    }
    $start.Environment['TGE_CRASH_REPORT_DIR'] = $reports
    foreach ($argument in $AppArguments) { $start.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::Start($start)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $timedOut = !$process.WaitForExit($TimeoutSeconds * 1000)
        if ($timedOut) {
            $process.Kill($true)
            if (!$process.WaitForExit(5000)) { throw "Timed-out process $($process.Id) did not exit" }
        }
        $output = $stdout.GetAwaiter().GetResult()
        $errors = $stderr.GetAwaiter().GetResult()
        [IO.File]::WriteAllText((Join-Path $work "$LogName.log"), "$output`n$errors")
        if ($timedOut) { throw "App timed out during $LogName. See $work." }
        return [pscustomobject]@{
            Id = $process.Id
            ExitCode = $process.ExitCode
            Output = $output
            Transcript = "$output`n$errors"
        }
    } finally {
        $process.Dispose()
    }
}

$embedded = Invoke-Lab5App @('--build-info') 'build-info'
if ($embedded.ExitCode -ne 0) { throw "app.exe --build-info failed. See $work." }
Assert-Lab5SameBuild $build (Read-Lab5BuildInfo $embedded.Output 'app.exe --build-info') $GameZip
$launch = @('--lab5', 'game/Lab5Beacon.tgeproj', '--verify')
$smoke = Invoke-Lab5App ($launch + 'smoke') 'smoke'
if ($smoke.ExitCode -ne 0 -or !$smoke.Transcript.Contains('LAB5_READY') -or
    !$smoke.Transcript.Contains('LAB5_CALIBRATED channels=3') -or
    !$smoke.Transcript.Contains('LAB5_SMOKE_OK')) {
    throw "The packaged scene did not reach the calibrated state and exit successfully. See $work."
}
if (Get-ChildItem -LiteralPath $reports -Force) { throw 'A normal exit generated a crash report.' }
Write-Host 'PASS: packaged scene loaded, calibrated three channels and exited without a report.'

$reportCheck = Join-Path $PSScriptRoot 'Test-Lab5Report.ps1'
$stackCheck = Join-Path $PSScriptRoot 'Test-Lab5Stack.ps1'
$scenarios = @('divide-by-zero', 'divide-by-zero', 'access-violation')
if ($build['demoRevision'] -eq 2) {
    $scenarios += 'assertion-failure'
} else {
    $unsupported = Invoke-Lab5App ($launch + 'assertion-failure') 'unsupported-probe'
    if ($unsupported.ExitCode -ne 2 -or !$unsupported.Transcript.Contains('Unsupported Lab 5 verification')) {
        throw "The first release accepted an unavailable assertion probe. See $work."
    }
}
$observed = @()
foreach ($scenario in $scenarios) {
    $previous = @(Get-ChildItem -LiteralPath $reports -Directory | Select-Object -ExpandProperty Name)
    $result = Invoke-Lab5App ($launch + $scenario) ("probe-$scenario-" + ($observed.Count + 1))
    $newReport = @(Get-ChildItem -LiteralPath $reports -Directory | Where-Object { $_.Name -notin $previous })
    if ($newReport.Count -ne 1) { throw "Expected one new report from $scenario. See $work." }
    $check = & $reportCheck -ReportDirectory $newReport[0].FullName -BuildInfoPath $buildInfoPath -Scenario $scenario
    $exitCode = '0x{0:X8}' -f $result.ExitCode
    if ($exitCode -ine $check.ExceptionCode -or $result.Id -ne $check.ProcessId) {
        throw "Process exit code or PID differs from the $scenario report. See $work."
    }
    if ($CheckStack -and !($observed | Where-Object Scenario -EQ $scenario)) {
        $arguments = @{
            ReportDirectory = $newReport[0].FullName
            GameDirectory = $game
            SymbolsDirectory = $symbols
        }
        if ($Cdb) { $arguments['Cdb'] = $Cdb }
        $stack = & $stackCheck @arguments
        if (!$? -or $stack -notmatch '^LAB5_STACK_OK .+ line=[1-9][0-9]*$') {
            throw "Stack verification failed for $scenario. See $work."
        }
        Write-Host $stack
    }
    $observed += $check
    Write-Host "PASS: $scenario $($check.ExceptionCode), thread $($check.ThreadId), report $($check.ReportId)."
}
if (@(Get-ChildItem -LiteralPath $reports -Directory).Count -ne $scenarios.Count) {
    throw 'An earlier report was overwritten.'
}

$missingTexture = Join-Path $game 'game/assets/textures/beacon.ppm'
[IO.File]::Move($missingTexture, "$missingTexture.missing")
try {
    $missing = Invoke-Lab5App ($launch + 'smoke') 'missing-texture'
    if ($missing.ExitCode -eq 0 -or !$missing.Transcript.Contains('beacon.ppm')) {
        throw "Expected a failure naming beacon.ppm after removing it from the extracted game. See $work."
    }
} finally {
    [IO.File]::Move("$missingTexture.missing", $missingTexture)
}
Write-Host 'PASS: game check rejected a package missing beacon.ppm.'

$first = $observed[0].ReportId
$incomplete = Join-Path $work "missing-dump/$first"
[void][IO.Directory]::CreateDirectory($incomplete)
foreach ($name in 'report.json', 'engine.log') {
    [IO.File]::Copy((Join-Path $reports "$first/$name"), (Join-Path $incomplete $name))
}
$missingDumpRejected = $false
try {
    & $reportCheck -ReportDirectory $incomplete -BuildInfoPath $buildInfoPath -Scenario 'divide-by-zero'
} catch {
    if ($_.Exception.Message -notmatch 'Missing or empty required file: .*crash\.dmp') { throw }
    $missingDumpRejected = $true
}
if (!$missingDumpRejected) { throw 'Report verification accepted an incomplete report.' }
Write-Host 'PASS: report check rejected a report missing crash.dmp.'

if ($EvidenceDirectory) {
    $evidence = [IO.Directory]::CreateDirectory($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath(
        $EvidenceDirectory)).FullName
    [ordered]@{
        build = $build
        smoke = 'passed'
        calibratedChannels = 3
        crashReports = $observed
        missingTextureRejected = $true
        missingDumpRejected = $true
        stackChecked = [bool]$CheckStack
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $evidence 'verification.json') -Encoding utf8NoBOM
}
