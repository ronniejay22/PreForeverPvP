# Points <Domain>.duckdns.org at this connection's current public IP. Runs every
# 5 minutes as a SYSTEM scheduled task (registered by admin-hosting.ps1).
param(
    [string]$Root = 'D:\WoWServer',
    [string]$Domain = 'preforeverpvp'
)
$ErrorActionPreference = 'Stop'

$token = (Get-Content (Join-Path $Root 'secrets\duckdns.token') -Raw).Trim()
# an empty ip= makes DuckDNS use the address the request comes from
$result = Invoke-RestMethod -Uri "https://www.duckdns.org/update?domains=$Domain&token=$token&ip=" -TimeoutSec 30
# log the answer only, never the URL: it contains the token
'{0:yyyy-MM-dd HH:mm:ss}  {1}' -f (Get-Date), $result | Add-Content (Join-Path $Root 'logs\duckdns.log')
if ($result -ne 'OK') { exit 1 }
