#requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$ReportDirectory,
    [Parameter(Mandatory)]
    [string]$BuildInfoPath,
    [ValidateSet('divide-by-zero', 'access-violation', 'assertion-failure')]
    [string]$Scenario
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Lab5PackageFiles.ps1')

function Read-Lab5MiniDump([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($stream.Length -lt 32 -or $reader.ReadUInt32() -ne 0x504D444D -or
            ($reader.ReadUInt32() -band 0xFFFF) -ne 0xA793) {
            throw "Invalid minidump header in $Path"
        }
        $count = $reader.ReadUInt32()
        $directory = $reader.ReadUInt32()
        if ($count -eq 0 -or $count -gt 4096 -or $directory -lt 32 -or
            ($directory + 12L * $count) -gt $stream.Length) {
            throw "Invalid minidump directory in $Path"
        }
        $stream.Position = $directory
        $streams = @{}
        for ($i = 0; $i -lt $count; $i++) {
            $type = $reader.ReadUInt32()
            $size = $reader.ReadUInt32()
            $rva = $reader.ReadUInt32()
            if (([long]$rva + $size) -gt $stream.Length) {
                throw "Truncated minidump stream $type in $Path"
            }
            if ($type -in 3, 6, 7) { $streams.Add([int]$type, @{ Size = $size; Rva = $rva }) }
        }

        $threads = $streams[3]
        $exception = $streams[6]
        $systemInfo = $streams[7]
        if (!$threads -or $threads.Size -lt 4 -or !$exception -or $exception.Size -lt 168 -or
            !$systemInfo -or $systemInfo.Size -lt 2) {
            throw "Missing minidump thread, exception or system information in $Path"
        }
        $stream.Position = $systemInfo.Rva
        if ($reader.ReadUInt16() -ne 9) { throw "Expected an x64 minidump in $Path" }

        $stream.Position = $exception.Rva
        $threadId = $reader.ReadUInt32()
        $stream.Position += 4
        $exceptionCode = '0x{0:X8}' -f $reader.ReadUInt32()
        $stream.Position = $exception.Rva + 160
        $contextSize = $reader.ReadUInt32()
        $contextRva = $reader.ReadUInt32()
        if ($contextSize -lt 256 -or $contextRva -lt 32 -or ([long]$contextRva + $contextSize) -gt $stream.Length) {
            throw "Invalid AMD64 exception context in $Path"
        }
        $stream.Position = $contextRva + 48
        $contextFlags = $reader.ReadUInt32()
        $stream.Position = $contextRva + 248
        if (($contextFlags -band 0x00100001) -ne 0x00100001 -or $reader.ReadUInt64() -eq 0) {
            throw "Missing AMD64 control registers in the exception context in $Path"
        }

        $stream.Position = $threads.Rva
        $threadCount = $reader.ReadUInt32()
        if ($threadCount -gt 65536 -or (4L + 48L * $threadCount) -gt $threads.Size) {
            throw "Invalid minidump thread list in $Path"
        }
        for ($i = 0; $i -lt $threadCount; $i++) {
            $stream.Position = $threads.Rva + 4L + 48L * $i
            if ($reader.ReadUInt32() -eq $threadId -and $threadId -gt 0) {
                return @{ ThreadId = $threadId; ExceptionCode = $exceptionCode }
            }
        }
        throw "Minidump exception thread $threadId is absent from the thread list in $Path"
    } finally {
        $reader.Dispose()
    }
}

$directory = Get-Item -LiteralPath $ReportDirectory
if ($directory -isnot [IO.DirectoryInfo]) { throw "Expected a report directory: $ReportDirectory" }
$metadataPath = Join-Path $directory.FullName 'report.json'
$dumpPath = Join-Path $directory.FullName 'crash.dmp'
$logPath = Join-Path $directory.FullName 'engine.log'
foreach ($path in $metadataPath, $dumpPath, $logPath) { Assert-Lab5File $path }

$expectedBuild = Get-Lab5BuildInfo $BuildInfoPath
$report = Get-Content -Raw -LiteralPath $metadataPath | ConvertFrom-Json -AsHashtable
if ($report -isnot [System.Collections.IDictionary]) { throw "Expected a report object in $metadataPath" }
$utc = $report['utc']
$time = [DateTimeOffset]::MinValue
$validUtc = if ($utc -is [DateTime]) {
    $utc.Kind -eq [DateTimeKind]::Utc
} else {
    $utc -match '(Z|\+00:00)$' -and [DateTimeOffset]::TryParse(
        $utc, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::None, [ref]$time)
}
if ($report['schemaVersion'] -ne 1 -or
    $report['reportId'] -cne $directory.Name -or $report['buildId'] -cne $expectedBuild['buildId'] -or
    $report['commitSha'] -cne $expectedBuild['commitSha'] -or $report['dumpWritten'] -ne $true -or
    $report['threadId'] -le 0 -or $report['processId'] -le 0 -or $report['scene'] -cne 'beacon.scene.json' -or !$validUtc) {
    throw "Incomplete report or build identity mismatch in $metadataPath"
}

$codes = @{
    'divide-by-zero' = '0xC0000094'
    'access-violation' = '0xC0000005'
    'assertion-failure' = '0xC0000420'
}
if (!$Scenario -and $report['lastAction'] -match '^probe-(.+)$') { $Scenario = $Matches[1] }
if (!$Scenario -or !$codes.ContainsKey($Scenario) -or $report['lastAction'] -cne "probe-$Scenario") {
    throw "Unexpected Lab 5 probe in $metadataPath"
}
$dump = Read-Lab5MiniDump $dumpPath
if ($dump.ExceptionCode -ine $codes[$Scenario] -or $dump.ExceptionCode -ine $report['exceptionCode'] -or
    $dump.ThreadId -ne $report['threadId']) {
    throw "Minidump exception/thread does not match $Scenario and $metadataPath"
}
$log = Get-Content -Raw -LiteralPath $logPath
foreach ($marker in 'LAB5_READY', 'LAB5_CALIBRATED channels=3', "LAB5_PROBE scenario=$Scenario") {
    if (!$log.Contains($marker)) { throw "Missing log marker '$marker' in $logPath" }
}

[pscustomobject]@{
    ReportId = $report['reportId']
    Scenario = $Scenario
    ProcessId = $report['processId']
    ThreadId = $dump.ThreadId
    ExceptionCode = $dump.ExceptionCode
}
