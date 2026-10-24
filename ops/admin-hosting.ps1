# One-time setup for opening the realm to the internet. Run elevated (one UAC
# prompt). Safe to re-run:
#   - Windows Firewall: inbound TCP 3724 (authserver), 8085 (worldserver) and
#     80/443 (Caddy), each tied to its program; nothing else is opened.
#   - DuckDNS updater: every 5 minutes and at startup, as SYSTEM.
#   - Caddy (HTTPS for patches, launcher and sign-up) as an auto-start service.
#   - The sign-up page as a startup task, as SYSTEM.
param([string]$Root = 'D:\WoWServer')

$ErrorActionPreference = 'Stop'

# A click in a console with QuickEdit on pauses every write until Esc is
# pressed, which silently stalls the script. Turn it off for this window.
Add-Type -Namespace Win32 -Name ConsoleMode -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern IntPtr GetStdHandle(int nStdHandle);
[DllImport("kernel32.dll")] public static extern bool GetConsoleMode(IntPtr h, out uint mode);
[DllImport("kernel32.dll")] public static extern bool SetConsoleMode(IntPtr h, uint mode);
'@
$stdin = [Win32.ConsoleMode]::GetStdHandle(-10)
$mode = 0
if ([Win32.ConsoleMode]::GetConsoleMode($stdin, [ref]$mode)) {
    [void][Win32.ConsoleMode]::SetConsoleMode($stdin, ($mode -band (-bnot 0x40)) -bor 0x80)
}

$logDir = Join-Path $Root 'logs'
$status = Join-Path $Root 'setup\logs\admin-hosting.status'
Start-Transcript -Path (Join-Path $Root 'setup\logs\admin-hosting.log') -Append | Out-Null
function Set-Status([string]$text) { Set-Content -Path $status -Value $text -Encoding ascii; Write-Host "== $text" }

$repo = Join-Path $Root 'src'
$server = Join-Path $Root 'server'
$caddy = Join-Path $Root 'caddy\caddy.exe'
$taskPath = '\PreForeverPvP\'

try {
    Set-Status 'RUNNING firewall'
    $rules = @(
        @{ Name = 'PreForeverPvP authserver'; Program = "$server\authserver.exe"; Ports = @('3724') },
        @{ Name = 'PreForeverPvP worldserver'; Program = "$server\worldserver.exe"; Ports = @('8085') },
        @{ Name = 'PreForeverPvP caddy'; Program = $caddy; Ports = @('80', '443') }
    )
    $programs = $rules | ForEach-Object { $_.Program }
    # Cancelling Windows' "allow access?" prompt creates block rules for a
    # program, and block rules beat allow rules. Remove any for these programs.
    Get-NetFirewallApplicationFilter | Where-Object { $programs -contains $_.Program } | Get-NetFirewallRule |
        Where-Object { $_.Action -eq 'Block' } | ForEach-Object { Write-Host "removing block rule: $($_.DisplayName)"; Remove-NetFirewallRule -Name $_.Name }
    foreach ($r in $rules) {
        Get-NetFirewallRule -DisplayName $r.Name -ErrorAction SilentlyContinue | Remove-NetFirewallRule
        New-NetFirewallRule -DisplayName $r.Name -Direction Inbound -Action Allow -Protocol TCP `
            -LocalPort $r.Ports -Program $r.Program -Profile Any | Out-Null
        Write-Host "allow inbound TCP $($r.Ports -join ',') -> $($r.Program)"
    }

    Set-Status 'RUNNING duckdns-task'
    $ps = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
    $action = New-ScheduledTaskAction -Execute $ps -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$repo\ops\duckdns-update.ps1`""
    $every5 = New-ScheduledTaskTrigger -Once -At (Get-Date) -RepetitionInterval (New-TimeSpan -Minutes 5)
    $boot = New-ScheduledTaskTrigger -AtStartup
    $settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -ExecutionTimeLimit (New-TimeSpan -Minutes 2)
    Register-ScheduledTask -TaskName 'DuckDNS' -TaskPath $taskPath -Action $action -Trigger $every5, $boot `
        -Settings $settings -User 'SYSTEM' -RunLevel Highest -Force | Out-Null
    Start-ScheduledTask -TaskName 'DuckDNS' -TaskPath $taskPath

    Set-Status 'RUNNING signup-task'
    $py = Join-Path $Root 'signup\venv\Scripts\python.exe'
    $action = New-ScheduledTaskAction -Execute $py -Argument "`"$repo\ops\signup\app.py`"" -WorkingDirectory "$repo\ops\signup"
    $settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) `
        -ExecutionTimeLimit ([TimeSpan]::Zero)
    Register-ScheduledTask -TaskName 'Signup' -TaskPath $taskPath -Action $action -Trigger (New-ScheduledTaskTrigger -AtStartup) `
        -Settings $settings -User 'SYSTEM' -RunLevel Highest -Force | Out-Null
    Start-ScheduledTask -TaskName 'Signup' -TaskPath $taskPath

    Set-Status 'RUNNING caddy-service'
    $bin = "$caddy run --config $repo\ops\Caddyfile --adapter caddyfile"
    if (-not (Get-Service caddy -ErrorAction SilentlyContinue)) {
        & sc.exe create caddy start= auto binPath= $bin DisplayName= 'Caddy (PreForeverPvP web)' | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "sc create caddy failed ($LASTEXITCODE)" }
    }
    else {
        & sc.exe config caddy start= auto binPath= $bin | Out-Host
    }
    # restart Caddy if it ever stops
    & sc.exe failure caddy reset= 86400 actions= restart/5000/restart/5000/restart/60000 | Out-Host
    Start-Service caddy -ErrorAction SilentlyContinue
    if ((Get-Service caddy).Status -ne 'Running') { throw 'Caddy did not start; see D:\WoWServer\logs and the Windows event log' }

    Set-Status 'DONE'
}
catch {
    Set-Status "FAILED $($_.Exception.Message)"
    throw
}
finally {
    Stop-Transcript | Out-Null
}
