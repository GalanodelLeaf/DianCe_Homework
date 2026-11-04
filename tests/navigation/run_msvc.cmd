@echo off
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio Installer or a C++ build toolchain was not found.
    exit /b 1
)
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "CMAKE_BIN=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "NINJA=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
set "LVGL=%~dp0..\..\managed_components\lvgl__lvgl"
if not "%~1"=="" set "LVGL=%~1"
pushd "%~dp0"
if not exist build-msvc mkdir build-msvc
set "TMP=%CD%\build-msvc"
set "TEMP=%TMP%"
"%CMAKE_BIN%\cmake.exe" -S . -B build-msvc -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_MAKE_PROGRAM=%NINJA%" "-DLVGL_ROOT=%LVGL%"
if errorlevel 1 goto fail
"%CMAKE_BIN%\cmake.exe" --build build-msvc --parallel 2
if errorlevel 1 goto fail
"%CMAKE_BIN%\ctest.exe" --test-dir build-msvc --output-on-failure
if errorlevel 1 goto fail
popd
exit /b 0
:fail
popd
exit /b 1
