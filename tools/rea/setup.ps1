param(
    [string]$JavaHome = $env:JAVA_HOME
)
$ErrorActionPreference = 'Stop'
$toolRoot = $PSScriptRoot
$repoRoot = Split-Path (Split-Path $toolRoot -Parent) -Parent
$nodePath = (Get-Command node.exe -ErrorAction Stop).Source
if (-not $JavaHome) {
    $JavaHome = Split-Path (Split-Path (Get-Command javac.exe -ErrorAction Stop).Source -Parent) -Parent
}
if (-not (Test-Path -LiteralPath (Join-Path $JavaHome 'bin/javac.exe'))) {
    throw 'A full JDK is required. Pass -JavaHome with its installation directory.'
}

$configDir = Join-Path $repoRoot '.codex'
$configPath = Join-Path $configDir 'config.toml'
$runner = Join-Path $toolRoot 'rea.mjs'
function ConvertTo-TomlString([string]$Value) {
    # JSON quoted strings are valid TOML basic strings for these Windows paths.
    return ConvertTo-Json -InputObject $Value -Compress
}
$block = @"
[mcp_servers.rea]
command = $(ConvertTo-TomlString $nodePath)
args = [$(ConvertTo-TomlString $runner), "mcp"]
cwd = $(ConvertTo-TomlString $repoRoot)
enabled = true
startup_timeout_sec = 60
tool_timeout_sec = 2147544

[mcp_servers.rea.env]
JAVA_HOME = $(ConvertTo-TomlString $JavaHome)
"@
$current = if (Test-Path -LiteralPath $configPath) { [IO.File]::ReadAllText($configPath) } else { '' }
if ($current -match '(?m)^\s*\[mcp_servers\.rea(?:\]|\.)' -and -not $current.Contains($block)) {
    throw 'An existing REA registration differs. Review .codex/config.toml before changing it.'
}

Push-Location $toolRoot
try {
    & npm.cmd ci --ignore-scripts --no-audit --no-fund
    if ($LASTEXITCODE -ne 0) { throw 'REA dependency installation failed' }
} finally {
    Pop-Location
}

$localDir = Join-Path $toolRoot 'local'
$ghidra = Join-Path $localDir 'ghidra_12.1.4_PUBLIC'
if (-not (Test-Path -LiteralPath (Join-Path $ghidra 'support/analyzeHeadless.bat'))) {
    New-Item -ItemType Directory -Path $localDir -Force | Out-Null
    $archive = Join-Path $localDir 'ghidra_12.1.4_PUBLIC_20260921.zip'
    if (-not (Test-Path -LiteralPath $archive)) {
        & curl.exe --fail --location --silent --show-error --output $archive 'https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1.4_build/ghidra_12.1.4_PUBLIC_20260921.zip'
        if ($LASTEXITCODE -ne 0) { throw 'Ghidra download failed' }
    }
    $expectedHash = 'ddac49f903da9d5bac833e5cc79395098b9c33cfd3279be5f31bd00387d2d4db'
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expectedHash) {
        throw 'Ghidra archive SHA256 mismatch; extraction refused'
    }
    Expand-Archive -LiteralPath $archive -DestinationPath $localDir
}

if (-not $current.Contains($block)) {
    New-Item -ItemType Directory -Path $configDir -Force | Out-Null
    [IO.File]::WriteAllText($configPath, $current + "`n" + $block + "`n", [Text.UTF8Encoding]::new($false))
}
Write-Output 'REA installed; project MCP registration saved. Restart Codex to load it.'
