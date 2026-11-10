# =============================================================================
# make-zip.ps1 — The Windows receiver for people without Visual Studio
# =============================================================================
# In "x64 Native Tools Command Prompt for VS" (for a 64-bit build), from the
# repository folder:
#
#   powershell -ExecutionPolicy Bypass -File packaging\windows\make-zip.ps1
#
# → dist\ExternalDisplayReceiver-<version>-win64.zip: a folder with the .exe and
# the install guide (docs/INSTALL.md). It builds Release in its own folder
# (windows-receiver\build-release), so none of your pairing files end up in it.
# The C++ runtime is linked into the .exe: nothing else needs installing.
# =============================================================================
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$src = Join-Path $root 'windows-receiver'
$build = Join-Path $src 'build-release'

cmake -S $src -B $build -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw 'CMake could not configure the build (is this a Visual Studio command prompt?)' }
cmake --build $build --config Release
if ($LASTEXITCODE -ne 0) { throw 'The build failed (see above)' }
$exe = Join-Path $build 'ExternalDisplayReceiver.exe'
if (-not (Test-Path $exe)) { throw "No exe at $exe" }

# 64- or 32-bit: the machine field of the exe's PE header
$bytes = [IO.File]::ReadAllBytes($exe)
$machine = [BitConverter]::ToUInt16($bytes, [BitConverter]::ToInt32($bytes, 60) + 4)
$arch = switch ($machine) { 0x8664 { 'win64' } 0x14c { 'win32' } default { 'win-' + $machine.ToString('x') } }
if ($arch -ne 'win64') {
    Write-Warning "This is a $arch build. For 64-bit, run it in the x64 Native Tools Command Prompt."
}

$version = (Select-String -Path (Join-Path $src 'CMakeLists.txt') `
    -Pattern 'project\(ExternalDisplayReceiver VERSION ([0-9.]+)').Matches[0].Groups[1].Value
$name = "ExternalDisplayReceiver-$version-$arch"
$dist = Join-Path $root 'dist'
$stage = Join-Path $dist $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item $exe $stage
Copy-Item (Join-Path $root 'docs\INSTALL.md') (Join-Path $stage 'Read me first.txt')
$zip = Join-Path $dist "$name.zip"
Compress-Archive -Force -Path $stage -DestinationPath $zip
Remove-Item -Recurse -Force $stage
Write-Host "Done: $zip"
