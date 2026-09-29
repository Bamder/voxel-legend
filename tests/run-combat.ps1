param([string]$Compiler = "g++")
$ErrorActionPreference = "Stop"
$repoDir = Split-Path -Parent $PSScriptRoot
$testDir = Join-Path ([System.IO.Path]::GetTempPath()) ("voxel-combat-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $testDir | Out-Null
$testExe = Join-Path $testDir "combat-test.exe"
& $Compiler -std=c++20 -Wall -Wextra -Werror -static-libgcc -static-libstdc++ `
    (Join-Path $PSScriptRoot "combat_test.cpp") `
    (Join-Path $repoDir "src/world/combat.cpp") `
    (Join-Path $repoDir "src/world/arcane.cpp") `
    (Join-Path $repoDir "src/world/guardian_ai.cpp") -o $testExe
if ($LASTEXITCODE -ne 0) { throw "Combat test compilation failed ($LASTEXITCODE)." }
& $testExe
if ($LASTEXITCODE -ne 0) { throw "Combat tests failed ($LASTEXITCODE)." }
Write-Output "Test executable: $testExe"
$bodyExe = Join-Path $testDir "room-body-test.exe"
# Link production physics/world code: do not replace the authority path with mocks.
$sources = Get-ChildItem (Join-Path $repoDir "src") -Filter *.cpp -Recurse |
    Where-Object { $_.FullName -notmatch '\\editor\\' -and $_.Name -ne 'main.cpp' } |
    ForEach-Object { $_.FullName }
& $Compiler -std=c++20 -O0 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type `
    -static-libgcc -static-libstdc++ (Join-Path $PSScriptRoot "room_body_test.cpp") `
    @sources -o $bodyExe -lopengl32 -lgdi32 -luser32 -lgdiplus -lcomdlg32 -lws2_32
if ($LASTEXITCODE -ne 0) { throw "Room body test compilation failed ($LASTEXITCODE)." }
& $bodyExe
if ($LASTEXITCODE -ne 0) { throw "Room body tests failed ($LASTEXITCODE)." }
