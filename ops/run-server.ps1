# Runs authserver or worldserver in this console window and restarts it when it
# exits with anything but 0: a crash, or ".server restart" (exit code 2).
# ".server shutdown" exits with 0 and ends the loop.
#   powershell -ExecutionPolicy Bypass -File ops\run-server.ps1 -Name worldserver
param(
    [ValidateSet('worldserver', 'authserver')][string]$Name = 'worldserver',
    [string]$Root = 'D:\WoWServer'
)

# With QuickEdit on, one click in this window pauses every console write until
# Esc is pressed. The server logs to the console from its worker threads, so a
# stray click would freeze the realm. Turn QuickEdit off for this window.
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

$Host.UI.RawUI.WindowTitle = $Name
$server = Join-Path $Root 'server'
$log = Join-Path $Root "logs\$Name-restarts.log"
Set-Location $server   # both servers read their .conf (and DataDir ".") relative to this

while ($true) {
    $started = Get-Date
    & (Join-Path $server "$Name.exe")
    $code = $LASTEXITCODE
    $line = '{0:yyyy-MM-dd HH:mm:ss}  {1} exited with {2} after {3:n1} min' -f (Get-Date), $Name, $code, ((Get-Date) - $started).TotalMinutes
    Add-Content -Path $log -Value $line
    Write-Host $line
    if ($code -eq 0) { break }
    Write-Host 'restarting in 10 s (close this window to stop)'
    Start-Sleep -Seconds 10
}
