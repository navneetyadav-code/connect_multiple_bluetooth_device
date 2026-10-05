@echo off
setlocal

:: Find vcvarsall.bat dynamically using vswhere
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
  set "InstallDir=%%i"
)

if not defined InstallDir (
    echo Error: Visual Studio with C++ build tools not found.
    exit /b 1
)

set "vcvarsall=%InstallDir%\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%vcvarsall%" (
    echo Error: vcvarsall.bat not found at %vcvarsall%
    exit /b 1
)

set VSCMD_START_DIR=%CD%
call "%vcvarsall%" x64

cd src\MultiAudio.AudioEngine
cl /LD /EHsc /W4 /std:c++20 /I. /Fe:MultiAudioEngine.dll *.cpp mmdevapi.lib mfuuid.lib wmcodecdspuuid.lib ole32.lib oleaut32.lib

if exist ..\MultiAudio.UI\bin\Debug\net10.0-windows\ (
    copy MultiAudioEngine.dll ..\MultiAudio.UI\bin\Debug\net10.0-windows\ /Y
)
if exist ..\MultiAudio.UI\bin\Release\net10.0-windows\ (
    copy MultiAudioEngine.dll ..\MultiAudio.UI\bin\Release\net10.0-windows\ /Y
)
