# One-time toolchain install for the server. Run elevated (one UAC prompt):
#   OpenSSL 3.5.9 LTS (full), MySQL 8.4.9 (all features, so the headers and
#   libmysql.lib are present), Boost 1.78 (msvc-14.3-64), MySQL initialized as
#   service MySQL84 with its data on D:, then Visual Studio 2022 Community with
#   the C++ desktop workload (which includes CMake).
# Safe to re-run: every step checks whether it is already done.
param([string]$Root = 'D:\WoWServer')

$ErrorActionPreference = 'Stop'

# A click in a console with QuickEdit on pauses every write until Esc is
# pressed, which silently stalls the whole install. Turn it off for this window.
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

$logDir = Join-Path $Root 'setup\logs'
$status = Join-Path $logDir 'admin-install-toolchain.status'
Start-Transcript -Path (Join-Path $logDir 'admin-install-toolchain.log') -Append | Out-Null

function Set-Status([string]$text) {
    Set-Content -Path $status -Value $text -Encoding ascii
    Write-Host "== $text"
}

# Success is judged by what ends up on disk, not by winget's exit code
# (it reports "already installed" and "reboot required" as non-zero).
function Invoke-Winget([string[]]$wingetArgs) {
    & winget @wingetArgs --accept-package-agreements --accept-source-agreements --disable-interactivity
    Write-Host "winget exit code: $LASTEXITCODE"
}

$mysqlBase = 'C:\Program Files\MySQL\MySQL Server 8.4'

try {
    Set-Status 'RUNNING openssl'
    if (-not (Test-Path 'C:\Program Files\OpenSSL-Win64\bin\libcrypto-3-x64.dll')) {
        # 3.x LTS from slproweb, downloaded and checked against their published
        # SHA-256 (6e6eb6fb...bfcc2). winget's default is 4.x, which this code was
        # never built against, and its pinned 3.6.2 URL now returns 404.
        $msi = Join-Path $Root 'downloads\openssl\Win64OpenSSL-3_5_9.msi'
        $p = Start-Process -FilePath 'msiexec.exe' -ArgumentList '/i', "`"$msi`"", '/qn', '/norestart', '/l*v', "`"$logDir\openssl-msi.log`"" -Wait -PassThru
        Write-Host "openssl msi exit code: $($p.ExitCode)"
    }
    if (-not (Test-Path 'C:\Program Files\OpenSSL-Win64\bin\libcrypto-3-x64.dll')) { throw 'OpenSSL 3 not found after install' }

    Set-Status 'RUNNING mysql-install'
    if (-not (Test-Path "$mysqlBase\bin\mysqld.exe")) {
        Invoke-Winget @('install', '-e', '--id', 'Oracle.MySQL', '--version', '8.4.9', '--override', '/qn /norestart ADDLOCAL=ALL')
    }
    if (-not (Test-Path "$mysqlBase\bin\mysqld.exe")) { throw 'MySQL 8.4 not found after install' }

    Set-Status 'RUNNING boost'
    if (-not (Test-Path 'C:\local\boost_1_78_0\lib64-msvc-14.3')) {
        # downloaded and md5-checked beforehand (47928a37822dbb09f25012576e056a0e)
        $boostExe = Join-Path $Root 'downloads\boost\boost_1_78_0-msvc-14.3-64.exe'
        $p = Start-Process -FilePath $boostExe -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', '/DIR=C:\local\boost_1_78_0' -Wait -PassThru
        Write-Host "boost installer exit code: $($p.ExitCode)"
    }
    if (-not (Test-Path 'C:\local\boost_1_78_0\lib64-msvc-14.3')) { throw 'Boost 1.78 not found after install' }

    Set-Status 'RUNNING mysql-init'
    $mysqlDir = Join-Path $Root 'mysql'
    $ini = Join-Path $mysqlDir 'my.ini'
    if (-not (Get-Service MySQL84 -ErrorAction SilentlyContinue)) {
        # localhost only; no X protocol (port 33060); no binary log (the bots write constantly)
        $iniText = @"
[mysqld]
basedir="C:/Program Files/MySQL/MySQL Server 8.4"
datadir="$($Root -replace '\\','/')/mysql/data"
port=3306
bind-address=127.0.0.1
mysqlx=OFF
skip-log-bin
innodb_buffer_pool_size=4G
max_allowed_packet=256M
log-error="$($Root -replace '\\','/')/mysql/mysqld.err"

[client]
port=3306
"@
        [IO.File]::WriteAllText($ini, $iniText, [Text.Encoding]::ASCII)
        if (-not (Test-Path (Join-Path $mysqlDir 'data'))) {
            & "$mysqlBase\bin\mysqld.exe" --defaults-file="$ini" --initialize-insecure
            if ($LASTEXITCODE -ne 0) { throw "mysqld --initialize-insecure failed ($LASTEXITCODE); see $mysqlDir\mysqld.err" }
        }
        & "$mysqlBase\bin\mysqld.exe" --install MySQL84 --defaults-file="$ini"
        if ($LASTEXITCODE -ne 0) { throw "mysqld --install failed ($LASTEXITCODE)" }
    }
    Start-Service MySQL84

    # root starts with an empty password; set it from the secrets file.
    # PowerShell 5.1 turns a native command's stderr into a terminating error
    # under ErrorActionPreference=Stop, and the first probe is expected to fail,
    # so mysql runs with Continue and only its exit code counts.
    $rootCnf = Join-Path $Root 'secrets\mysql-root.cnf'
    function Invoke-MySql([string[]]$mysqlArgs, [string]$sql) {
        $old = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            if ($sql) { $sql | & "$mysqlBase\bin\mysql.exe" @mysqlArgs *> $null }
            else { & "$mysqlBase\bin\mysql.exe" @mysqlArgs *> $null }
            return $LASTEXITCODE
        }
        finally { $ErrorActionPreference = $old }
    }
    if ((Invoke-MySql @("--defaults-extra-file=$rootCnf", '-e', 'SELECT 1')) -ne 0) {
        $rootPw = (Select-String -Path $rootCnf -Pattern '^password=(.+)$').Matches[0].Groups[1].Value
        $sql = "ALTER USER 'root'@'localhost' IDENTIFIED BY '$rootPw';"
        if ((Invoke-MySql @('-u', 'root', '--skip-password', '-h', '127.0.0.1') $sql) -ne 0) { throw 'could not set the MySQL root password' }
    }
    if ((Invoke-MySql @("--defaults-extra-file=$rootCnf", '-e', 'SELECT 1')) -ne 0) { throw 'MySQL root login with the secrets file failed' }

    Set-Status 'RUNNING visualstudio'
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsReady = { (Test-Path $vswhere) -and (& $vswhere -products * -requires Microsoft.VisualStudio.Workload.NativeDesktop -property installationPath) }
    if (-not (& $vsReady)) {
        Invoke-Winget @('install', '-e', '--id', 'Microsoft.VisualStudio.2022.Community', '--override', '--add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended --passive --wait --norestart')
    }
    if (-not (& $vsReady)) { throw 'Visual Studio 2022 with the C++ workload not found after install' }

    Set-Status 'DONE'
}
catch {
    Set-Status "FAILED $($_.Exception.Message)"
    throw
}
finally {
    Stop-Transcript | Out-Null
}
