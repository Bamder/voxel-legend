# Orthographic six-view sheets for entity .model files.
# Run from this folder, or from the repo root via:
#   .\tools\model_views\build.ps1
$ErrorActionPreference = "Stop"

$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$sources = @(
    (Join-Path $PSScriptRoot "src\main.cpp"),
    (Join-Path $root "src\material\image.cpp")
)
Write-Output "Sources (model_views.exe):"
$sources | ForEach-Object { Write-Output "  $_" }

$rt = @("-static-libgcc", "-static-libstdc++")
$pthread = & g++ -print-file-name=libwinpthread.a
if ($pthread -and (Test-Path -LiteralPath $pthread)) {
    $rt += @("-Wl,-Bstatic,--whole-archive", "-lwinpthread", "-Wl,--no-whole-archive,-Bdynamic")
}

& g++ -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type `
    -finput-charset=UTF-8 -fexec-charset=UTF-8 `
    -I"$root\src" `
    @sources -o (Join-Path $PSScriptRoot "model_views.exe") @rt -lgdiplus -lgdi32 -luser32
if ($LASTEXITCODE -ne 0) {
    Write-Output "BUILD FAILED (model_views.exe)"
    exit $LASTEXITCODE
}
Write-Output "BUILD OK -> $PSScriptRoot\model_views.exe"
