# Serve the WSL staging folder to the phone over the Windows LAN address.
# The phone reaches 192.168.0.104 (this PC's Wi-Fi address); a server inside WSL
# listens on 172.x, which the phone cannot route to, so the server runs on Windows
# with --directory pointing at the WSL folder.
$ErrorActionPreference = 'SilentlyContinue'
Get-NetTCPConnection -LocalPort 8000 -State Listen | ForEach-Object {
    Stop-Process -Id $_.OwningProcess -Force
}
Start-Sleep -Milliseconds 500
Start-Process -FilePath 'python' -ArgumentList @(
    '-m', 'http.server', '8000', '--bind', '0.0.0.0',
    '--directory', '\\wsl.localhost\Ubuntu\home\darling\ipas-lan'
) -WindowStyle Hidden
Start-Sleep -Seconds 1
try {
    (Invoke-WebRequest -UseBasicParsing -TimeoutSec 3 http://127.0.0.1:8000/).StatusCode
} catch {
    "server did not answer: $($_.Exception.Message)"
}
