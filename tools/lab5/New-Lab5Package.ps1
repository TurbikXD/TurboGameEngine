#requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$BuildDirectory,
    [ValidateSet('Release', 'RelWithDebInfo')]
    [string]$Configuration = 'RelWithDebInfo',
    [Parameter(Mandatory)]
    [string]$OutputDirectory,
    [string]$ExpectedBuildId,
    [string]$ExpectedCommitSha,
    [string]$CrashReportUrl
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Lab5PackageFiles.ps1')

if ($CrashReportUrl) {
    [uri]$endpoint = $null
    if ($CrashReportUrl.Length -gt 2048 -or ![uri]::TryCreate($CrashReportUrl, [UriKind]::Absolute, [ref]$endpoint) -or
        !$endpoint.IsWellFormedOriginalString() -or !$endpoint.Host -or $endpoint.Query -or $endpoint.Fragment -or
        $CrashReportUrl -match '^https?://[^/]*@' -or
        ($endpoint.Scheme -ne 'https' -and $CrashReportUrl -notmatch '^http://127\.0\.0\.1(?::[0-9]+)?(?:/|$)')) {
        throw 'CrashReportUrl must use HTTPS or local HTTP at 127.0.0.1 and omit credentials, query and fragment.'
    }
    $CrashReportUrl = $endpoint.AbsoluteUri
    if ($CrashReportUrl.Length -gt 2048 -or $CrashReportUrl -match '[^\x21-\x7E]') {
        throw 'CrashReportUrl must be an escaped ASCII URL of at most 2048 characters.'
    }
}

$source = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$buildDirectory = (Resolve-Path -LiteralPath $BuildDirectory).Path
$runtime = Join-Path $buildDirectory $Configuration
$app = Join-Path $runtime 'app.exe'
$pdb = Join-Path $runtime 'app.pdb'
Assert-Lab5File $app
Assert-Lab5File $pdb
$buildInfoJson = (& $app --build-info) -join "`n"
if ($LASTEXITCODE -ne 0) {
    throw "Reading embedded build identity from $app failed with exit code $LASTEXITCODE"
}
$buildInfo = Read-Lab5BuildInfo $buildInfoJson $app
if (($ExpectedBuildId -and $buildInfo['buildId'] -cne $ExpectedBuildId) -or
    ($ExpectedCommitSha -and $buildInfo['commitSha'] -cne $ExpectedCommitSha)) {
    throw "Embedded build identity does not match the requested build in $app"
}

function Add-Lab5Tree($Files, [string]$Directory, [string]$EntryPrefix) {
    if (!(Test-Path -LiteralPath $Directory -PathType Container)) {
        throw "Missing required directory: $Directory"
    }
    foreach ($file in Get-ChildItem -LiteralPath $Directory -Recurse -File) {
        $relative = [IO.Path]::GetRelativePath($Directory, $file.FullName).Replace('\', '/')
        $Files.Add("$EntryPrefix/$relative", $file.FullName)
    }
}

function Write-Lab5Zip([string]$Path, $Files, [string]$ReportUrl) {
    $archive = [IO.Compression.ZipFile]::Open($Path, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($entry in $Files.GetEnumerator()) {
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $archive, $entry.Value, $entry.Key, [IO.Compression.CompressionLevel]::Optimal)
        }
        $metadata = [ordered]@{ 'build.json' = $buildInfoJson }
        if ($ReportUrl) { $metadata['crash-report-url.txt'] = $ReportUrl }
        foreach ($entry in $metadata.GetEnumerator()) {
            $writer = [IO.StreamWriter]::new($archive.CreateEntry($entry.Key).Open(), [Text.UTF8Encoding]::new($false))
            try { $writer.WriteLine($entry.Value) } finally { $writer.Dispose() }
        }
    } finally {
        $archive.Dispose()
    }
}

$project = Join-Path $source 'demos/lab5-beacon'
$diligent = Join-Path $source 'third_party/DiligentEngine'
$cache = Get-Content -LiteralPath (Join-Path $buildDirectory 'CMakeCache.txt')
$fetchContentBase = @(($cache -cmatch '^FETCHCONTENT_BASE_DIR:PATH=') -creplace '^[^=]*=', '')
if (!$fetchContentBase) { $fetchContentBase = @(Join-Path $buildDirectory '_deps') }

function Get-Lab5DependencyFile([string]$Dependency, [string]$RelativePath) {
    $key = "FETCHCONTENT_SOURCE_DIR_$($Dependency.ToUpperInvariant()):PATH="
    $overrides = @(($cache -cmatch "^$key.") -creplace '^[^=]*=', '')
    $directory = if ($overrides) { $overrides[0] } else { Join-Path $fetchContentBase[0] "$Dependency-src" }
    return Join-Path $directory $RelativePath
}

$gameFiles = [ordered]@{
    'app.exe' = $app
    'config.json' = Join-Path $PSScriptRoot 'game-config.json'
    'Play-Lab5.cmd' = Join-Path $PSScriptRoot 'Play-Lab5.cmd'
    'TESTER.md' = Join-Path $source 'docs/lab5/TESTER.md'
    'game/Lab5Beacon.tgeproj' = Join-Path $project 'Lab5Beacon.tgeproj'
    'licenses/DiligentCore.txt' = Join-Path $diligent 'DiligentCore/License.txt'
    'licenses/DiligentTools.txt' = Join-Path $diligent 'DiligentTools/License.txt'
    'licenses/ImGui.txt' = Join-Path $diligent 'DiligentTools/ThirdParty/imgui/LICENSE.txt'
    'licenses/ImGuiColorTextEdit.txt' = Join-Path $source 'third_party/ImGuiColorTextEdit/LICENSE'
    'licenses/xxHash.txt' = Join-Path $diligent 'DiligentCore/ThirdParty/xxHash/LICENSE'
    'licenses/Assimp.txt' = Get-Lab5DependencyFile 'assimp' 'LICENSE'
    'licenses/zlib.txt' = Get-Lab5DependencyFile 'assimp' 'contrib/zlib/LICENSE'
    'licenses/enkiTS.txt' = Get-Lab5DependencyFile 'enkits' 'License.txt'
    'licenses/EnTT.txt' = Get-Lab5DependencyFile 'entt' 'LICENSE'
    'licenses/GLFW.md' = Get-Lab5DependencyFile 'glfw' 'LICENSE.md'
    'licenses/GLM.txt' = Get-Lab5DependencyFile 'glm' 'copying.txt'
    'licenses/ImGuizmo.txt' = Get-Lab5DependencyFile 'imguizmo' 'LICENSE'
    'licenses/Luau.txt' = Get-Lab5DependencyFile 'luau' 'LICENSE.txt'
    'licenses/nlohmann-json.txt' = Get-Lab5DependencyFile 'nlohmann_json' 'LICENSE.MIT'
    'licenses/spdlog.txt' = Get-Lab5DependencyFile 'spdlog' 'LICENSE'
    'licenses/stb.txt' = Get-Lab5DependencyFile 'stb' 'LICENSE'
}
foreach ($subdirectory in 'fonts', 'shaders_hlsl', 'textures', 'scripts/react-luau') {
    Add-Lab5Tree $gameFiles (Join-Path $runtime "assets/$subdirectory") "assets/$subdirectory"
}
Add-Lab5Tree $gameFiles (Join-Path $project 'assets') 'game/assets'
foreach ($entry in $Lab5GameFiles) {
    if ($entry -eq 'build.json') { continue }
    if (!$gameFiles.Contains($entry)) { throw "Missing required package entry: $entry" }
}
foreach ($file in $gameFiles.Values) { Assert-Lab5File $file }

$output = [IO.Directory]::CreateDirectory($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath(
    $OutputDirectory)).FullName
$archiveBase = "TurboGameEngine-Lab5-$($buildInfo['buildId'])"
$gameZip = Join-Path $output "$archiveBase-win64.zip"
$symbolsZip = Join-Path $output "$archiveBase-symbols.zip"
if ((Test-Path -LiteralPath $gameZip) -or (Test-Path -LiteralPath $symbolsZip)) {
    throw "Output for build $($buildInfo['buildId']) already exists in $output"
}
$suffix = ".partial.$([guid]::NewGuid().ToString('N'))"
Write-Lab5Zip ($gameZip + $suffix) $gameFiles $CrashReportUrl
Write-Lab5Zip ($symbolsZip + $suffix) ([ordered]@{ 'app.pdb' = $pdb })
[IO.File]::Move(($gameZip + $suffix), $gameZip)
[IO.File]::Move(($symbolsZip + $suffix), $symbolsZip)

[pscustomobject]@{
    GameZip = $gameZip
    SymbolsZip = $symbolsZip
    BuildId = $buildInfo['buildId']
    CommitSha = $buildInfo['commitSha']
}
