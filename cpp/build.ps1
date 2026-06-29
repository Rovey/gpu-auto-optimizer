# Build + test wrapper for the C++ project.
# Activates the VS 2026 dev environment, configures with the bundled CMake + Ninja,
# builds, and runs ctest. Usage (from anywhere):  powershell -File cpp\build.ps1
$ErrorActionPreference = "Stop"

$vs    = "C:\Program Files\Microsoft Visual Studio\18\Community"
$cmake = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ctest = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
$ninja = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

# Import the MSVC environment (sets cl, INCLUDE, LIB, PATH) into this session.
& "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null

$src = $PSScriptRoot
Push-Location $src
try {
    & $cmake -S . -B build -G Ninja -DCMAKE_MAKE_PROGRAM="$ninja" -DCMAKE_BUILD_TYPE=Debug
    if ($LASTEXITCODE -ne 0) { throw "configure failed" }
    & $cmake --build build
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
    & $ctest --test-dir build --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "tests failed" }
    Write-Output "BUILD+TESTS OK"
}
finally {
    Pop-Location
}
