# Tree Lab — standalone tree-growth sandbox (MinGW-w64 g++).
# Run from this folder:
#   .\build.ps1
# Output: tree_lab.exe in this folder (tools/tree_lab/)
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$root = Resolve-Path "..\.."
$sources = @(
    (Resolve-Path "src\main.cpp").Path,
    (Resolve-Path "src\tree_sim.cpp").Path,
    (Resolve-Path "$root\src\core\gl_loader.cpp").Path
)
Write-Output "Sources (tree_lab.exe):"
$sources | ForEach-Object { Write-Output "  $_" }

& g++ -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type `
    -finput-charset=UTF-8 -fexec-charset=UTF-8 `
    -I"$root\src" `
    @sources -o tree_lab.exe -lopengl32 -lgdi32 -luser32 -lgdiplus
if ($LASTEXITCODE -ne 0) {
    Write-Output "BUILD FAILED (tree_lab.exe)"
    exit $LASTEXITCODE
}
Write-Output "BUILD OK -> $PSScriptRoot\tree_lab.exe"
