# Writes the server's .conf files from the repo's copies plus this machine's
# paths and secrets. Re-run after every deploy or config change. It overwrites
# the generated files, so permanent settings belong here, not in server\.
# -LocalOnly binds both servers to 127.0.0.1: nothing is reachable from other
# machines, and Windows Firewall never asks about them (cancelling that dialog
# creates block rules that would later override the explicit allow rules).
param(
    [string]$Root = 'D:\WoWServer',
    [switch]$LocalOnly
)
$ErrorActionPreference = 'Stop'

$repo = Join-Path $Root 'src'
$server = Join-Path $Root 'server'
$rootFwd = $Root -replace '\\', '/'
$trinityPw = (Get-Content (Join-Path $Root 'secrets\mysql-trinity.txt') -Raw).Trim()

# Replaces "Key = ..." lines; a key that isn't there is an error, so a renamed
# key upstream can't be silently skipped.
function Write-Conf([string]$source, [string]$target, [hashtable]$set) {
    $text = [IO.File]::ReadAllText($source)
    foreach ($key in $set.Keys) {
        $pattern = '(?m)^' + [regex]::Escape($key) + '[ \t]*=[^\r\n]*'
        if (-not [regex]::IsMatch($text, $pattern)) { throw "$key not found in $source" }
        $text = [regex]::Replace($text, $pattern, ("$key = " + $set[$key]).Replace('$', '$$'))
    }
    [IO.File]::WriteAllText($target, $text, (New-Object Text.UTF8Encoding($false)))
    Write-Host "wrote $target"
}

function Db([string]$name) { "`"127.0.0.1;3306;trinity;$trinityPw;$name`"" }
$bindIp = if ($LocalOnly) { '"127.0.0.1"' } else { '"0.0.0.0"' }

Write-Conf (Join-Path $repo 'centurion\conf\worldserver.conf') (Join-Path $server 'worldserver.conf') @{
    'DataDir'               = "`"$rootFwd/data`""
    'LogsDir'               = "`"$rootFwd/logs`""
    'LoginDatabaseInfo'     = Db 'auth'
    'WorldDatabaseInfo'     = Db 'world'
    'CharacterDatabaseInfo' = Db 'characters'
    'BindIP'                = $bindIp
    # the live file runs without a console; ours needs it for GM commands
    'Console.Enable'        = '1'
    # The bots and Centurion's spell data generate a steady stream of aura,
    # spell and script diagnostics that bury the console. Those loggers write
    # to Server.log only; the console keeps startup progress (Logger.server),
    # GM commands, SQL errors and command output.
    'Logger.root'            = '5,Server'
    'Logger.scripts'         = '3,Server'
    'Logger.scripts.hotswap' = '3,Server'
    'Logger.spells'          = '3,Server'
    'Logger.custom'          = '3,Server'
    'Logger.arena.replay'    = '3,Server'
    'Logger.bg.spiritguide'  = '1,Server'
}

Write-Conf (Join-Path $repo 'src\server\authserver\authserver.conf.dist') (Join-Path $server 'authserver.conf') @{
    'LogsDir'            = "`"$rootFwd/logs`""
    'LoginDatabaseInfo'  = Db 'auth'
    'BindIP'             = $bindIp
    # the .dist default of 0 means no lockout on a realm open to the internet
    'WrongPass.MaxCount' = '5'
    'WrongPass.BanTime'  = '600'
    'WrongPass.Logging'  = '1'
}

foreach ($f in 'playerbots.conf', 'AutoBalance.conf') {
    Copy-Item (Join-Path $repo "centurion\conf\$f") (Join-Path $server $f) -Force
    Write-Host "wrote $(Join-Path $server $f)"
}

# these two hold the database password: owner and SYSTEM (services) only
foreach ($f in 'worldserver.conf', 'authserver.conf') {
    icacls (Join-Path $server $f) /inheritance:r /grant:r "${env:USERNAME}:F" 'SYSTEM:F' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "icacls failed on $f" }
}
