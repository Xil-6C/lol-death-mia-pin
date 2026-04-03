# LoL Death MIA Pin - OBS Plugin Installer
# Run as Administrator: powershell -ExecutionPolicy Bypass -File install.ps1

param(
    [string]$OBSPath = "C:\Program Files\obs-studio"
)

$ErrorActionPreference = "Stop"

$PluginDir = Join-Path $OBSPath "obs-plugins\64bit"
$DataDir   = Join-Path $OBSPath "data\obs-plugins\lol-death-mia-pin"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$DLL       = Join-Path $ScriptDir "build\Release\lol-death-mia-pin.dll"
$LocaleDir = Join-Path $ScriptDir "data\locale"

# Verify source files exist
if (-not (Test-Path $DLL)) {
    Write-Error "DLL not found: $DLL`nPlease build the project first."
    exit 1
}
if (-not (Test-Path $LocaleDir)) {
    Write-Error "Locale directory not found: $LocaleDir"
    exit 1
}

# Verify OBS installation
if (-not (Test-Path $PluginDir)) {
    Write-Error "OBS plugins directory not found: $PluginDir`nSpecify OBS path with -OBSPath parameter."
    exit 1
}

# Copy DLL
Write-Host "Installing plugin DLL..." -ForegroundColor Cyan
Copy-Item $DLL (Join-Path $PluginDir "lol-death-mia-pin.dll") -Force
Write-Host "  -> $PluginDir\lol-death-mia-pin.dll" -ForegroundColor Green

# Copy locale data
Write-Host "Installing locale files..." -ForegroundColor Cyan
$DestLocale = Join-Path $DataDir "locale"
New-Item -ItemType Directory -Path $DestLocale -Force | Out-Null
Copy-Item (Join-Path $LocaleDir "*.ini") $DestLocale -Force
Write-Host "  -> $DestLocale" -ForegroundColor Green

Write-Host "`nInstallation complete! Restart OBS to load the plugin." -ForegroundColor Yellow
