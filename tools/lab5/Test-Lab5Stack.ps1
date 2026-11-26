#requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ReportDirectory,
    [Parameter(Mandatory)][string]$GameDirectory,
    [Parameter(Mandatory)][string]$SymbolsDirectory,
    [string]$Cdb,
    [string]$SystemSymbolCache
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Read-Lab5Json([string]$Path) {
    Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
}

try {
    $report = Read-Lab5Json (Join-Path $ReportDirectory 'report.json')
    $game = Read-Lab5Json (Join-Path $GameDirectory 'build.json')
    $symbols = Read-Lab5Json (Join-Path $SymbolsDirectory 'build.json')
    if ($report.schemaVersion -ne 1 -or $game.schemaVersion -ne 1 -or $symbols.schemaVersion -ne 1) {
        throw 'Unsupported build or report metadata schema.'
    }
    if ($report.buildId -cne $game.buildId -or $report.commitSha -cne $game.commitSha) {
        throw "The report belongs to build $($report.buildId), game package is $($game.buildId)."
    }
    if ($symbols.buildId -cne $game.buildId -or $symbols.commitSha -cne $game.commitSha) {
        throw "The game package belongs to build $($game.buildId), symbol archive is $($symbols.buildId)."
    }

    $expected = @{
        'probe-divide-by-zero' = @{ Code = 'C0000094'; Frame = 'lab5IntegerDivideByZero' }
        'probe-access-violation' = @{ Code = 'C0000005'; Frame = 'lab5AccessViolation' }
        'probe-assertion-failure' = @{ Code = 'C0000420'; Frame = 'lab5AssertionFailure' }
    }[$report.lastAction]
    if (!$expected -or $report.exceptionCode -ine "0x$($expected.Code)") {
        throw "Unrecognized Lab 5 scenario and code: $($report.lastAction), $($report.exceptionCode)."
    }
    $dump = (Get-Item -LiteralPath (Join-Path $ReportDirectory 'crash.dmp')).FullName
    $imagePath = (Get-Item -LiteralPath (Join-Path $GameDirectory 'app.exe')).DirectoryName
    $symbolPath = (Get-Item -LiteralPath (Join-Path $SymbolsDirectory 'app.pdb')).DirectoryName
    if ($imagePath -match '[;*]' -or $symbolPath -match '[;*]') {
        throw 'Image and symbol directories must omit debugger path separators.'
    }

    if (!$Cdb) {
        $command = Get-Command cdb.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($command) { $Cdb = $command.Source }
        else { $Cdb = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/Debuggers/x64/cdb.exe' }
    }
    if (!(Test-Path -LiteralPath $Cdb -PathType Leaf)) {
        throw 'cdb.exe is required. Install Debugging Tools for Windows or use -Cdb.'
    }
    $symbolNetwork = '-netsyms:no'
    if ($SystemSymbolCache) {
        $cache = [IO.Path]::GetFullPath($SystemSymbolCache)
        if ($cache -match '[;*]') { throw 'The cache path must omit debugger path separators.' }
        $symbolPath += ";srv*$cache*https://msdl.microsoft.com/download/symbols"
        $symbolNetwork = '-netsyms:yes'
    }

    $start = [Diagnostics.ProcessStartInfo]::new((Get-Item -LiteralPath $Cdb).FullName)
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.WorkingDirectory = $imagePath
    foreach ($argument in @('-nosqm', '-noshell', '-sins', '-lines', $symbolNetwork,
            '-z', $dump, '-i', $imagePath, '-y', $symbolPath,
            '-c', '.symopt+ 0x400; .reload /f app.exe; .exr -1; .ecxr; kn 32; q')) {
        $start.ArgumentList.Add($argument)
    }
    $process = [Diagnostics.Process]::Start($start)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit(90000)) {
            $process.Kill()
            throw 'CDB exceeded its time limit.'
        }
        $output = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
        $frame = [regex]::Match($output, '(?im)\bapp![^\r\n]*\b' + [regex]::Escape($expected.Frame) +
            '(?:\+0x[0-9a-f]+)?\s*\[(?<file>[^\]\r\n]*Lab5Session\.cpp)\s*@\s*(?<line>[1-9][0-9]*)\]')
        if ($process.ExitCode -ne 0 -or $output -notmatch "(?im)^\s*ExceptionCode:\s*$($expected.Code)\b" -or !$frame.Success) {
            Write-Host $output
            throw "CDB did not resolve $($expected.Frame) and its source line for build $($game.buildId)."
        }
        Write-Output ('LAB5_STACK_OK buildId={0} threadId={1} exception=0x{2} function={3} file={4} line={5}' -f
            $game.buildId, $report.threadId, $expected.Code, $expected.Frame, $frame.Groups['file'].Value.Trim(),
            $frame.Groups['line'].Value)
    } finally {
        $process.Dispose()
    }
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
