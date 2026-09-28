# Tree Lab — standalone tree-growth sandbox (MinGW-w64 g++).
# Run from this folder:
#   .\build.ps1
# Output: tree_lab.exe in this folder (tools/tree_lab/)
$ErrorActionPreference = "Stop"

$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$sources = @(
    (Join-Path $PSScriptRoot "src\main.cpp"),
    (Join-Path $PSScriptRoot "src\tree_sim.cpp"),
    (Join-Path $root "src\core\gl_loader.cpp")
)
Write-Output "Sources (tree_lab.exe):"
$sources | ForEach-Object { Write-Output "  $_" }

# Same runtime fold-in as the root build: no libgcc / libstdc++ / winpthread DLLs.
$rt = @("-static-libgcc", "-static-libstdc++")
$pthread = & g++ -print-file-name=libwinpthread.a
if ($pthread -and (Test-Path -LiteralPath $pthread)) {
    $rt += @("-Wl,-Bstatic,--whole-archive", "-lwinpthread", "-Wl,--no-whole-archive,-Bdynamic")
}

& g++ -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type `
    -finput-charset=UTF-8 -fexec-charset=UTF-8 `
    -I"$root\src" `
    @sources -o (Join-Path $PSScriptRoot "tree_lab.exe") @rt -lopengl32 -lgdi32 -luser32 -lgdiplus
if ($LASTEXITCODE -ne 0) {
    Write-Output "BUILD FAILED (tree_lab.exe)"
    exit $LASTEXITCODE
}
Write-Output "BUILD OK -> $PSScriptRoot\tree_lab.exe"
