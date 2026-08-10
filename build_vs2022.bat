@echo off
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found at "%VSWHERE%".
    echo Install Visual Studio 2022 with the "Desktop development with C++" workload.
    exit /b 1
)

set "VSINSTALL="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -version [17.0^,18.0^) -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
    echo ERROR: Visual Studio 2022 with the x64 C++ toolset was not found.
    echo Install the "Desktop development with C++" workload.
    exit /b 1
)

call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b %errorlevel%

echo [configure] cmake --preset windows-msvc-debug
cmake --preset windows-msvc-debug
if errorlevel 1 exit /b %errorlevel%

echo [build] cmake --build --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
if errorlevel 1 exit /b %errorlevel%

echo.
echo Built: build\bin\sample_cpp_project.exe
exit /b 0
