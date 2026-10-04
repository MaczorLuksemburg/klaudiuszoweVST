# Builds the Windows installer (installer\windows\output\MakiPlugins-Setup-<version>.exe) from built plugins.
#   .\build-installer.ps1 [-PluginDir "C:\gen plugins"] [-Version 1.0.0]
# Needs Inno Setup 6 (https://jrsoftware.org/isinfo.php, or: winget install JRSoftware.InnoSetup).
param (
    [string] $PluginDir = "C:\gen plugins",
    [string] $Version = "1.0.0"
)

$candidates = @(@(
    (Get-Command ISCC.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source),
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
) | Where-Object { $_ -and (Test-Path $_) })

if (-not $candidates) {
    Write-Error "Inno Setup 6 (ISCC.exe) not found. Install it: winget install JRSoftware.InnoSetup"
    exit 1
}

& $candidates[0] "/DPluginDir=$PluginDir" "/DAppVersion=$Version" "$PSScriptRoot\MakiPlugins.iss"
exit $LASTEXITCODE
