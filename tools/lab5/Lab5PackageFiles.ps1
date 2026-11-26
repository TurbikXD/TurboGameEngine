$Lab5GameFiles = @(
    'app.exe'
    'build.json'
    'config.json'
    'Play-Lab5.cmd'
    'TESTER.md'
    'game/Lab5Beacon.tgeproj'
    'game/assets/scenes/beacon.scene.json'
    'game/assets/scripts/Beacon.luau'
    'game/assets/textures/beacon.ppm'
    'assets/fonts/SourceSansPro-Regular.ttf'
    'assets/fonts/SourceSansPro-Semibold.ttf'
    'assets/fonts/FiraMono-Regular.ttf'
    'assets/shaders_hlsl/basic.vert.hlsl'
    'assets/shaders_hlsl/basic.frag.hlsl'
    'assets/shaders_hlsl/textured.vert.hlsl'
    'assets/shaders_hlsl/textured.frag.hlsl'
    'assets/shaders_hlsl/textured.shader.json'
    'assets/scripts/react-luau/manifest.tsv'
    'assets/scripts/react-luau/adapters/Compatibility.luau'
    'assets/scripts/react-luau/adapters/RobloxUI.luau'
    'assets/scripts/react-luau/adapters/TweenService.luau'
    'assets/scripts/react-luau/adapters/ReactTurbo.luau'
)

function Assert-Lab5File([string]$Path) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf) -or (Get-Item -LiteralPath $Path).Length -eq 0) {
        throw "Missing or empty required file: $Path"
    }
}

function Read-Lab5BuildInfo([string]$Json, [string]$Source) {
    $info = ConvertFrom-Json -InputObject $Json -AsHashtable
    if ($info -isnot [System.Collections.IDictionary] -or $info['schemaVersion'] -ne 1 -or
        $info['buildId'] -cnotmatch '\A[A-Za-z0-9][A-Za-z0-9._-]{0,79}\z' -or
        $info['commitSha'] -cnotmatch '\A[a-f0-9]{40}\z' -or $info['demoRevision'] -notin 1, 2) {
        throw "Invalid Lab 5 build identity in $Source"
    }
    return $info
}

function Assert-Lab5SameBuild($Expected, $Actual, [string]$Source) {
    if ($Actual['buildId'] -cne $Expected['buildId'] -or
        $Actual['commitSha'] -cne $Expected['commitSha'] -or
        $Actual['demoRevision'] -ne $Expected['demoRevision']) {
        throw "Build identity mismatch in $Source"
    }
}

function Get-Lab5BuildInfo([string]$Path) {
    Assert-Lab5File $Path
    return Read-Lab5BuildInfo (Get-Content -Raw -LiteralPath $Path) $Path
}
