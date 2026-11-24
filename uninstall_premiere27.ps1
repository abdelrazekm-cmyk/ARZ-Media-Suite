$ErrorActionPreference = "Stop"
$dirs = @(
    "C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\ARZ",
    "C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\AER",
    "C:\Program Files\Adobe\Adobe Premiere Pro 2027\PlugIns\Common\ARZ",
    "C:\Program Files\Adobe\Adobe Premiere Pro 2027\PlugIns\Common\AER",
    "C:\Program Files\Adobe\Adobe Media Encoder 2027\PlugIns\Common\ARZ"
)

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Start-Process powershell.exe -Verb RunAs -Wait -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    exit 0
}

foreach ($d in $dirs) {
    if (Test-Path $d) {
        Remove-Item -Path $d -Recurse -Force
        Write-Host "Removed: $d" -ForegroundColor Green
    }
}
