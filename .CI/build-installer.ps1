if (Test-Path -PathType Container Leafyrino) {
    $stagingDirName = "Leafyrino"
}
elseif (Test-Path -PathType Container Chatterino2) {
    $stagingDirName = "Chatterino2"
}
else {
    Write-Error "Couldn't find a staging folder called 'Leafyrino' or 'Chatterino2' in the current directory.";
    exit 1
}

# Check if we're on a tag
$OldErrorActionPref = $ErrorActionPreference;
$ErrorActionPreference = 'Continue';
git describe --exact-match --match 'v*' *> $null;
$isTagged = $?;
$ErrorActionPreference = $OldErrorActionPref;

$defines = $null;
if ($isTagged) {
    # This is a release.
    # Make sure, any existing `modes` file is overwritten for the user,
    # for example when updating from nightly to stable.
    Write-Output "" | Out-File "$stagingDirName/modes" -Encoding ASCII;
    $installerBaseName = "Leafyrino.Installer";
}
else {
    Write-Output nightly | Out-File "$stagingDirName/modes" -Encoding ASCII;
    $defines = "/DIS_NIGHTLY=1";
    $installerBaseName = "Leafyrino.Nightly.Installer";
}

$architecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLower()
if ($architecture -eq 'arm64') {
    $installerBaseName = "Experimental-ARM64-$installerBaseName"
    $defines = "$defines /DIS_ARM=1".Trim()
}

if ($Env:GITHUB_OUTPUT) {
    # This is used in CI when creating the artifact
    "C2_INSTALLER_BASE_NAME=$installerBaseName" >> "$Env:GITHUB_OUTPUT"
}

# Copy vc_redist.x64.exe
if ($null -eq $Env:VCToolsRedistDir) {
    Write-Error "VCToolsRedistDir is not set. Forgot to set Visual Studio environment variables?";
    exit 1
}
Copy-Item "$Env:VCToolsRedistDir\vc_redist.$architecture.exe" .;

$VCRTVersion = (Get-Item "$Env:VCToolsRedistDir\vc_redist.$architecture.exe").VersionInfo;
$setupIconFile = (Resolve-Path "$PSScriptRoot\..\resources\icon.ico").Path;

# Build the installer
ISCC `
    /DWORKING_DIR="$($pwd.Path)\" `
    /DSTAGING_DIR_NAME="$stagingDirName" `
    /DINSTALLER_BASE_NAME="$installerBaseName" `
    /DSETUP_ICON_FILE="$setupIconFile" `
    /DSHIPPED_VCRT_MINOR="$($VCRTVersion.FileMinorPart)" `
    /DSHIPPED_VCRT_VERSION="$($VCRTVersion.FileDescription)" `
    /DVCRT_ARCH="$architecture" `
    $defines `
    /O. `
    "$PSScriptRoot\chatterino-installer.iss";
