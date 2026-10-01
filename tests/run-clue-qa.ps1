param([string]$Compiler = 'C:\msys64\ucrt64\bin\g++.exe')
$ErrorActionPreference = 'Stop'
$repoDir = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path -LiteralPath $Compiler)) { throw "Compiler not found: $Compiler" }

# MinGW's linker may misdecode a Chinese output directory. Keep the QA exe in an
# ASCII-only temporary path, while the game runs with the repository as its cwd.
$qaDir = Join-Path ([System.IO.Path]::GetTempPath()) ('voxel-clue-qa-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $qaDir | Out-Null
$qaExe = Join-Path $qaDir 'voxel-clue-qa.exe'
$sources = Get-ChildItem (Join-Path $repoDir 'src') -Filter '*.cpp' -Recurse |
    Where-Object { $_.FullName -notmatch '\\editor\\' } |
    ForEach-Object { $_.FullName }

& $Compiler -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type `
    -finput-charset=UTF-8 -fexec-charset=UTF-8 @sources -o $qaExe `
    -static-libgcc -static-libstdc++ `
    '-Wl,-Bstatic,--whole-archive' -lwinpthread '-Wl,--no-whole-archive,-Bdynamic' `
    -lopengl32 -lgdi32 -luser32 -lgdiplus -lcomdlg32 -lws2_32
if ($LASTEXITCODE -ne 0) { throw "QA compilation failed ($LASTEXITCODE)." }

Write-Host "QA game: $qaExe"
Write-Host 'Create a room, join a combat team, start, deploy, then press F8 near each clue objective.'
Push-Location $repoDir
try { & $qaExe --qa-clue }
finally { Pop-Location }
