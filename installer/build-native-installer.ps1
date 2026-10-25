$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$releaseExe = Join-Path $repoRoot 'build\release\Release\webview_example.exe'
$outputDir = Join-Path $repoRoot 'dist'

& cmake --preset windows-x64-release
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
& cmake --build --preset release --parallel
if ($LASTEXITCODE -ne 0) { throw 'Native release build failed.' }
if (-not (Test-Path -LiteralPath $releaseExe)) { throw "Missing release executable: $releaseExe" }

$makeNsis = Get-Command makensis -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source
if (-not $makeNsis) {
  $cacheRoot = Join-Path $env:LOCALAPPDATA 'electron-builder\Cache'
  $makeNsis = Get-ChildItem -LiteralPath $cacheRoot -Recurse -Filter makensis.exe -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty FullName
}
if (-not $makeNsis) { throw 'makensis.exe was not found. Install NSIS or build the Electron installer once.' }

New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
Copy-Item -LiteralPath $releaseExe -Destination (Join-Path $outputDir 'WebView-Browser-1.3.1-portable.exe') -Force
Push-Location $PSScriptRoot
try {
  & $makeNsis 'native-installer.nsi'
  if ($LASTEXITCODE -ne 0) { throw 'Native installer build failed.' }
} finally {
  Pop-Location
}
