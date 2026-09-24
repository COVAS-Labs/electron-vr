param(
  [string]$Version = "latest"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

if ($env:OS -ne "Windows_NT" -or -not [Environment]::Is64BitOperatingSystem) {
  throw "The Electron VR OpenXR layer requires 64-bit Windows."
}

[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$asset = "electron-vr-openxr-layer-win32-x64.zip"
$release = if ($Version -eq "latest") {
  "https://github.com/COVAS-Labs/electron-vr/releases/latest/download"
} else {
  "https://github.com/COVAS-Labs/electron-vr/releases/download/$Version"
}
$temporaryDirectory = Join-Path ([IO.Path]::GetTempPath()) ("electron-vr-openxr-" + [IO.Path]::GetRandomFileName())
$archive = Join-Path $temporaryDirectory $asset
$checksumFile = "$archive.sha256"
$extracted = Join-Path $temporaryDirectory "layer"

try {
  New-Item -ItemType Directory -Path $temporaryDirectory | Out-Null
  Write-Host "Downloading Electron VR OpenXR integration..."
  Invoke-WebRequest -UseBasicParsing -Uri "$release/$asset" -OutFile $archive
  Invoke-WebRequest -UseBasicParsing -Uri "$release/$asset.sha256" -OutFile $checksumFile

  $expectedHash = ((Get-Content -Raw $checksumFile).Trim() -split "\s+")[0]
  $actualHash = (Get-FileHash -Algorithm SHA256 $archive).Hash
  if ($actualHash -ne $expectedHash) {
    throw "The downloaded OpenXR integration failed SHA-256 verification."
  }

  Expand-Archive -Path $archive -DestinationPath $extracted
  $installer = Join-Path $extracted "electron_vr_openxr_layer_cli.exe"
  if (-not (Test-Path $installer)) {
    throw "The downloaded archive does not contain the OpenXR integration installer."
  }

  & $installer install
  if ($LASTEXITCODE -ne 0) {
    throw "The OpenXR integration installer exited with code $LASTEXITCODE."
  }

  & $installer status
  if ($LASTEXITCODE -ne 0) {
    throw "The OpenXR integration status check exited with code $LASTEXITCODE."
  }

  Write-Host "OpenXR integration installed. Restart any running OpenXR applications."
} finally {
  if (Test-Path $temporaryDirectory) {
    Remove-Item -Recurse -Force $temporaryDirectory
  }
}
