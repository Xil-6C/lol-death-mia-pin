# LoL Death MIA Pin - Distribution Package Builder
# Usage: powershell -ExecutionPolicy Bypass -File package.ps1

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$DLL       = Join-Path $ScriptDir "build\Release\lol-death-mia-pin.dll"
$LocaleDir = Join-Path $ScriptDir "data\locale"
$Delivery  = Join-Path $ScriptDir "delivery"

# Verify build exists
if (-not (Test-Path $DLL)) {
    Write-Error "DLL not found: $DLL`nPlease build the project first (Release config)."
    exit 1
}

# Create staging directory
$StageDir = Join-Path $ScriptDir "dist\lol-death-mia-pin"
if (Test-Path $StageDir) { Remove-Item $StageDir -Recurse -Force }

# OBS plugin directory structure
$PluginDest = Join-Path $StageDir "obs-plugins\64bit"
$DataDest   = Join-Path $StageDir "data\obs-plugins\lol-death-mia-pin\locale"

New-Item -ItemType Directory -Path $PluginDest -Force | Out-Null
New-Item -ItemType Directory -Path $DataDest   -Force | Out-Null

# Copy plugin files
Copy-Item $DLL $PluginDest
Copy-Item (Join-Path $LocaleDir "*.ini") $DataDest

# Copy delivery files
foreach ($f in @("mia.webm", "README.txt")) {
    $src = Join-Path $Delivery $f
    if (Test-Path $src) {
        Copy-Item $src $StageDir
    }
}

# Create ZIP
$ZipPath = Join-Path $ScriptDir "dist\lol-death-mia-pin.zip"
if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
Compress-Archive -Path (Join-Path $StageDir "*") -DestinationPath $ZipPath

# Cleanup staging
Remove-Item $StageDir -Recurse -Force

$Size = [math]::Round((Get-Item $ZipPath).Length / 1KB, 1)
Write-Host ""
Write-Host "Package created: dist\lol-death-mia-pin.zip ($Size KB)" -ForegroundColor Green
Write-Host ""
Write-Host "ZIP contents:" -ForegroundColor Cyan
Write-Host "  mia.webm"
Write-Host "  README.txt"
Write-Host "  obs-plugins/64bit/lol-death-mia-pin.dll"
Write-Host "  data/obs-plugins/lol-death-mia-pin/locale/en-US.ini"
Write-Host "  data/obs-plugins/lol-death-mia-pin/locale/ja-JP.ini"
Write-Host ""
Write-Host "Users extract obs-plugins/ and data/ into their OBS install folder" -ForegroundColor Yellow
Write-Host "(e.g. C:\Program Files\obs-studio\)" -ForegroundColor Yellow
