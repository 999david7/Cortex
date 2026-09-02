@echo off
rem Build AI IDE using the bundled portable toolchain.
setlocal

set ROOT=%~dp0
set TOOLCHAIN=%ROOT%tools\w64devkit\bin

if not exist "%TOOLCHAIN%\g++.exe" (
    echo.
    echo   The toolchain was not found at:
    echo     %TOOLCHAIN%
    echo.
    echo   Download w64devkit from https://github.com/skeeto/w64devkit/releases
    echo   and extract it into  %ROOT%tools\  so that
    echo     tools\w64devkit\bin\g++.exe
    echo   exists. Any other MinGW-w64 g++ on PATH works too.
    echo.
    exit /b 1
)

set PATH=%TOOLCHAIN%;%PATH%

echo Building AI IDE...
make -j%NUMBER_OF_PROCESSORS% %*
if errorlevel 1 (
    echo.
    echo Build FAILED.
    exit /b 1
)

echo.
echo Done:  build\AI-IDE.exe
endlocal
