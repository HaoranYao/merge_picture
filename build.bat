@echo off
setlocal

call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 (
    echo [error] VsDevCmd.bat failed
    exit /b 1
)

cd /d "%~dp0"

if not exist build (
    mkdir build
)

cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo [warn] Ninja generator failed, falling back to NMake Makefiles
    cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
)
if errorlevel 1 (
    echo [error] cmake configure failed
    exit /b 1
)

cmake --build build --config Release
if errorlevel 1 (
    echo [error] cmake build failed
    exit /b 1
)

echo [ok] build succeeded
