param(
    [string]$Compiler = "C:\msys64\ucrt64\bin\g++.exe",
    [string]$Output = "voxel-legend-combat-test.exe"
)
$ErrorActionPreference = "Stop"
$repoDir = Split-Path -Parent $PSScriptRoot
Push-Location $repoDir
try {
    # Relative source/output paths avoid MinGW's locale conversion of a Chinese cwd.
    $sources = Get-ChildItem .\src -Filter "*.cpp" -Recurse |
        Where-Object { $_.FullName -notmatch "\\editor\\" } |
        ForEach-Object { Resolve-Path -LiteralPath $_.FullName -Relative }
    $runtime = @("-static-libgcc", "-static-libstdc++")
    $pthread = & $Compiler -print-file-name=libwinpthread.a
    if ($pthread -and (Test-Path -LiteralPath $pthread)) {
        $runtime += @("-Wl,-Bstatic,--whole-archive", "-lwinpthread", "-Wl,--no-whole-archive,-Bdynamic")
    }
    & $Compiler -std=c++20 -O0 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type `
        -finput-charset=UTF-8 -fexec-charset=UTF-8 @sources -o $Output @runtime `
        -lopengl32 -lgdi32 -luser32 -lgdiplus -lcomdlg32 -lws2_32
    if ($LASTEXITCODE -ne 0) { throw "Combat test build failed ($LASTEXITCODE)." }
    Write-Output "BUILD OK -> $repoDir\$Output"
} finally {
    Pop-Location
}
