# Builds the server's terrain data from the client copy and assembles data\:
#   maps, vmaps, mmaps  via tools/mapdata/regen_map_data.ps1 (never -Deploy:
#                       that pushes to the original author's servers)
#   Cameras             mapextractor -e 4, which that script doesn't run
#   dbc                 the repo's server DBCs, not the extracted ones (the
#                       client's copies differ in places)
#   mmaps\1690*         Summoner's Rift's navmesh, built from a server-only
#                       version of the map that the client doesn't carry
# Takes hours (mmaps_generator). -Resume keeps finished mmaps tiles.
param(
    [string]$Root = 'D:\WoWServer',
    [switch]$Resume
)
$ErrorActionPreference = 'Stop'

$repo = Join-Path $Root 'src'
$client = Join-Path $Root 'client'
$work = Join-Path $Root 'mapwork'
$tools = Join-Path $Root 'server'
$data = Join-Path $Root 'data'

& (Join-Path $repo 'tools\mapdata\regen_map_data.ps1') -Client $client -Work $work -Tools $tools -Resume:$Resume

Push-Location $work
try {
    & (Join-Path $tools 'mapextractor.exe') -i $client -o $work -e 4
    if ($LASTEXITCODE -ne 0) { throw "mapextractor -e 4 exited with $LASTEXITCODE" }
}
finally { Pop-Location }

function Mirror([string]$from, [string]$to, [string[]]$files = @('*')) {
    robocopy $from $to @files /MIR /R:1 /W:1 /NP /NFL /NDL /NJH /NJS | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy $from -> $to failed ($LASTEXITCODE)" }
}

foreach ($d in 'maps', 'vmaps', 'mmaps', 'Cameras') { Mirror (Join-Path $work $d) (Join-Path $data $d) }
Mirror (Join-Path $repo 'centurion\dbc') (Join-Path $data 'dbc') @('*.dbc')
Copy-Item (Join-Path $repo 'centurion\mmaps\1690*') (Join-Path $data 'mmaps') -Force

foreach ($d in 'dbc', 'maps', 'vmaps', 'mmaps', 'Cameras') {
    $n = (Get-ChildItem (Join-Path $data $d) -File).Count
    Write-Host ("{0,-8} {1,6} files" -f $d, $n)
}
exit 0
