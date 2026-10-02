[CmdletBinding()]
param(
    [string]$OutputDirectory = 'C:\tge\lab1-assets',
    [ValidateRange(16,4096)][int]$TextureSize = 2048,
    [ValidateRange(4,512)][int]$MeshGrid = 256,
    [string]$Node = 'node'
)
$ErrorActionPreference = 'Stop'
& $Node (Join-Path $PSScriptRoot 'generate-assets.mjs') --output $OutputDirectory --texture-size $TextureSize --mesh-grid $MeshGrid
if ($LASTEXITCODE -ne 0) { throw "Fixture generation failed ($LASTEXITCODE)." }
