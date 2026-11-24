$ErrorActionPreference = "Stop"
$srcDir = Join-Path $PSScriptRoot "dist\ARZ"
$dstDir = "C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\ARZ"
$legacyDirs = @(
    "C:\Program Files\Adobe\Adobe Premiere Pro 2027\PlugIns\Common\ARZ",
    "C:\Program Files\Adobe\Adobe Premiere Pro 2027\PlugIns\Common\AER",
    "C:\Program Files\Adobe\Adobe Premiere Pro 2026\PlugIns\Common\ARZ",
    "C:\Program Files\Adobe\Adobe Media Encoder 2027\PlugIns\Common\ARZ",
    "C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\AER"
)

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "Requesting Administrator privileges to install ARZ v1.3.0 into shared Adobe MediaCore..." -ForegroundColor Yellow
    Start-Process powershell.exe -Verb RunAs -Wait -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    if ((Test-Path (Join-Path $dstDir "ARZ.prm")) -and (Test-Path (Join-Path $dstDir "ARZ_Exporter.prm"))) {
        Write-Host "[OK] Verified ARZ v1.3.0 is installed in shared MediaCore (Premiere Pro + Media Encoder + After Effects): $dstDir" -ForegroundColor Green
        Get-ChildItem $dstDir | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
    } else {
        Write-Host "[ERROR] Installation did not complete or UAC prompt was cancelled." -ForegroundColor Red
    }
    exit 0
}

$adobeRunning = Get-Process -Name "Adobe Premiere Pro*", "Adobe Media Encoder*", "AfterFX*" -ErrorAction SilentlyContinue
if ($adobeRunning) {
    Write-Host "[WARNING] Premiere Pro / Media Encoder / After Effects is currently running! Please close it first." -ForegroundColor Red
    Read-Host "Press Enter after closing Adobe video apps to continue"
}

foreach ($ld in $legacyDirs) {
    if (Test-Path $ld) {
        Write-Host "Removing legacy folder to prevent duplicate plugin loading: $ld" -ForegroundColor Yellow
        Remove-Item -Path $ld -Recurse -Force -ErrorAction SilentlyContinue
    }
}

New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
Copy-Item -Path (Join-Path $srcDir "*") -Destination $dstDir -Force

foreach ($adobeRoot in @("HKCU:\Software\Adobe\Premiere Pro", "HKCU:\Software\Adobe\Adobe Media Encoder", "HKCU:\Software\Adobe\After Effects")) {
    if (Test-Path $adobeRoot) {
        Get-ChildItem -Path $adobeRoot -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.PSChildName -match "PluginCache" } |
            ForEach-Object {
                Write-Host "Clearing Adobe PluginCache: $($_.Name)" -ForegroundColor Cyan
                Remove-Item -Path $_.PSPath -Recurse -Force -ErrorAction SilentlyContinue
            }
    }
}

Write-Host "Successfully installed ARZ v1.3.0 into shared MediaCore and refreshed PluginCache: $dstDir" -ForegroundColor Green
Get-ChildItem $dstDir | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
