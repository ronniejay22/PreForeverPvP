# Copies a finished build into the server folder, plus the runtime DLLs the
# exes need. Never touches the .conf files there: ops/configure.ps1 owns them.
param(
    [string]$Root = 'D:\WoWServer',
    [string]$Config = 'RelWithDebInfo'
)
$ErrorActionPreference = 'Stop'

$bin = Join-Path $Root "build\bin\$Config"
$server = Join-Path $Root 'server'
if (-not (Test-Path (Join-Path $bin 'worldserver.exe'))) { throw "no build at $bin" }
New-Item -ItemType Directory -Force $server | Out-Null

robocopy $bin $server /E /XF *.conf /R:1 /W:1 /NP /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }

$openssl = 'C:\Program Files\OpenSSL-Win64'
# RC4 lives in OpenSSL's legacy provider: worldserver loads legacy.dll from its
# own folder and crashes on the first client connection without it.
$legacy = "$openssl\bin\legacy.dll", "$openssl\lib\ossl-modules\legacy.dll" | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $legacy) { throw "legacy.dll not found under $openssl" }

# libmysql.dll has to be the exact version the build was compiled against
foreach ($dll in 'C:\Program Files\MySQL\MySQL Server 8.4\lib\libmysql.dll',
                 "$openssl\bin\libcrypto-3-x64.dll",
                 "$openssl\bin\libssl-3-x64.dll",
                 $legacy) {
    Copy-Item $dll $server -Force
}
Write-Host "deployed $bin -> $server"
exit 0
