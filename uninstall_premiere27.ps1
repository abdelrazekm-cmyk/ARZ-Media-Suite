$ErrorActionPreference = "Stop"
$dstDir = "C:\Program Files\Adobe\Adobe Premiere Pro 2027\PlugIns\Common\ARZ"
$legacyDir = "C:\Program Files\Adobe\Adobe Premiere Pro 2027\PlugIns\Common\AER"

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Start-Process powershell.exe -Verb RunAs -Wait -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    exit 0
}

foreach ($d in @($dstDir, $legacyDir)) {
    if (Test-Path $d) {
        Remove-Item -Path $d -Recurse -Force
        Write-Host "Removed: $d" -ForegroundColor Green
    }
}
