# Build script for VOXEL LEGEND (MinGW-w64 g++).
# Builds two separate executables:
#   voxel-legend.exe - the game
#   editor.exe    - the standalone material/model editor (all modes)
#
# Run from this folder in Windows PowerShell:
#   .\build.ps1
$ErrorActionPreference = "Stop"

function Test-ExeLocked($path) {
    if (-not (Test-Path $path)) { return $false }
    try {
        $fs = [System.IO.File]::Open($path, 'Open', 'ReadWrite', 'None')
        $fs.Close()
        return $false
    } catch {
        return $true
    }
}

function Fail-IfLocked($name) {
    if (Test-ExeLocked $name) {
        Write-Output "BUILD FAILED ($name): file is in use and cannot be overwritten."
        Write-Output "Close $name, then run .\build.ps1 again."
        exit 1
    }
}

function Explain-LinkFailure($name, $code) {
    if (Test-ExeLocked $name) {
        Write-Output "BUILD FAILED ($name): linker could not overwrite $name (file in use)."
        Write-Output "Close $name, then run .\build.ps1 again."
    } else {
        Write-Output "BUILD FAILED ($name)"
    }
    exit $code
}

function Build($name, $sources) {
    Fail-IfLocked $name
    Write-Output "Sources ($name):"
    $sources | ForEach-Object { Write-Output "  $_" }
    & g++ -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -finput-charset=UTF-8 -fexec-charset=UTF-8 @sources -o $name -lopengl32 -lgdi32 -luser32 -lgdiplus -lcomdlg32 -lws2_32
    if ($LASTEXITCODE -ne 0) { Explain-LinkFailure $name $LASTEXITCODE }
    Write-Output "BUILD OK -> $name"
}

# Game: every source except the standalone editor.
$gameSrc = Get-ChildItem -Path "src" -Filter "*.cpp" -Recurse |
    Where-Object { $_.FullName -notmatch '\\editor\\' } |
    ForEach-Object { $_.FullName }
Build "voxel-legend.exe" $gameSrc

# Editor: standalone material/model editor (window + GL + material system only).
# -mwindows makes it a GUI app (no console window). It shows a mode chooser on
# startup; --texture / --item / --model / --animation skip straight to a mode.
Fail-IfLocked "editor.exe"
$editorSrc = @(
    "src/editor/editor_main.cpp",
    "src/editor/data_editor.cpp",
    "src/core/gl_loader.cpp",
    "src/material/image.cpp",
    "src/material/material.cpp",
    "src/material/registry.cpp",
    "src/material/blocks/grass_tuft_mat.cpp",
    "src/plugin/plugin.cpp",
    "src/plugin/modules/BasicConstruction/basic_construction.cpp",
    "src/render/textures.cpp",
    "src/world/loot.cpp",
    "src/world/data_pack.cpp"
) | ForEach-Object { (Resolve-Path $_).Path }
Write-Output "Sources (editor.exe):"
$editorSrc | ForEach-Object { Write-Output "  $_" }
& g++ -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -finput-charset=UTF-8 -fexec-charset=UTF-8 @editorSrc -o editor.exe -lopengl32 -lgdi32 -luser32 -lgdiplus -lcomdlg32 -mwindows
if ($LASTEXITCODE -ne 0) { Explain-LinkFailure "editor.exe" $LASTEXITCODE }
Write-Output "BUILD OK -> editor.exe"

# Tree growth sandbox (step playback + live sliders).
Write-Output ""
& "$PSScriptRoot\tools\tree_lab\build.ps1"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
