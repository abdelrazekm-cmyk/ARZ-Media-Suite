$ErrorActionPreference = "Stop"
$srcDir = Join-Path $PSScriptRoot "dist\ARZ"
$dstDir = "C:\Program Files\Adobe\Adobe Premiere Pro 2027\PlugIns\Common\ARZ"
$legacyDir = "C:\Program Files\Adobe\Adobe Premiere Pro 2027\PlugIns\Common\AER"

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "Requesting Administrator privileges to install ARZ v1.0.0 into Premiere Pro 2027..." -ForegroundColor Yellow
    Start-Process powershell.exe -Verb RunAs -Wait -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    if ((Test-Path (Join-Path $dstDir "ARZ.prm")) -and (Test-Path (Join-Path $dstDir "ARZ_Exporter.prm"))) {
        Write-Host "[OK] Verified ARZ v1.0.0 (ARZ.prm, ARZ_HP.prm, ARZ_Exporter.prm, ARZDesktop.exe) is installed in: $dstDir" -ForegroundColor Green
        Get-ChildItem $dstDir | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
    } else {
        Write-Host "[ERROR] Installation did not complete or UAC prompt was cancelled." -ForegroundColor Red
    }
    exit 0
}

$prRunning = Get-Process -Name "Adobe Premiere Pro*", "Adobe Media Encoder*" -ErrorAction SilentlyContinue
if ($prRunning) {
    Write-Host "[WARNING] Adobe Premiere Pro / Media Encoder is currently running! Please close it first." -ForegroundColor Red
    Read-Host "Press Enter after closing Premiere Pro to continue"
}

if (Test-Path $legacyDir) {
    Write-Host "Removing previous AER folder to complete clean migration to ARZ..." -ForegroundColor Yellow
    Remove-Item -Path $legacyDir -Recurse -Force -ErrorAction SilentlyContinue
}

New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
Copy-Item -Path (Join-Path $srcDir "*") -Destination $dstDir -Force

foreach ($adobeRoot in @("HKCU:\Software\Adobe\Premiere Pro", "HKCU:\Software\Adobe\Adobe Media Encoder")) {
    if (Test-Path $adobeRoot) {
        Get-ChildItem -Path $adobeRoot -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.PSChildName -match "PluginCache" } |
            ForEach-Object {
                Write-Host "Clearing Adobe PluginCache: $($_.Name)" -ForegroundColor Cyan
                Remove-Item -Path $_.PSPath -Recurse -Force -ErrorAction SilentlyContinue
            }
    }
}

Write-Host "Successfully installed ARZ v1.0.0 and refreshed PluginCache: $dstDir" -ForegroundColor Green
Get-ChildItem $dstDir | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
