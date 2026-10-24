# Configures (first time, or with -Reconfigure) and builds the server and the
# map tools with Visual Studio 2022. Output: build\bin\<Config>; then run
# ops\deploy.ps1 and ops\configure.ps1.
param(
    [string]$Root = 'D:\WoWServer',
    [string]$Config = 'RelWithDebInfo',
    [switch]$Reconfigure
)
# cmake and MSBuild print warnings on stderr, which PowerShell 5.1 would turn
# into terminating errors under Stop; exit codes are checked instead.
$ErrorActionPreference = 'Continue'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio with the C++ tools not found' }
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'

$src = Join-Path $Root 'src'
$build = Join-Path $Root 'build'
$mysql = 'C:/Program Files/MySQL/MySQL Server 8.4'

if ($Reconfigure -or -not (Test-Path (Join-Path $build 'CMakeCache.txt'))) {
    # FindMySQL.cmake only searches the 8.0 and 5.7 install folders, hence the
    # explicit paths. SCRIPTS_PLAYERBOT=static guards against a cache that once
    # saw PLAYERBOT=OFF (it would keep the bots disabled).
    & $cmake -S $src -B $build -G 'Visual Studio 17 2022' -A x64 `
        -DPLAYERBOT=ON -DSCRIPTS_PLAYERBOT=static -DTOOLS=ON `
        -DBOOST_ROOT=C:/local/boost_1_78_0 `
        "-DMYSQL_INCLUDE_DIR=$mysql/include" "-DMYSQL_LIBRARY=$mysql/lib/libmysql.lib" `
        "-DOPENSSL_ROOT_DIR=C:/Program Files/OpenSSL-Win64"
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
}

# /MP already compiles each project's files on every core, so projects build
# one at a time (no --parallel): that keeps memory use in check.
& $cmake --build $build --config $Config
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
Write-Host "build ok: $build\bin\$Config"
