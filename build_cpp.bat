@echo off
set VSCMD_START_DIR=%CD%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cd src\MultiAudio.AudioEngine
cl /LD /EHsc /W4 /std:c++20 /I. /Fe:MultiAudioEngine.dll *.cpp mmdevapi.lib mfuuid.lib wmcodecdspuuid.lib ole32.lib oleaut32.lib
copy MultiAudioEngine.dll ..\MultiAudio.UI\bin\Debug\net10.0-windows\ /Y
copy MultiAudioEngine.dll ..\MultiAudio.UI\bin\Release\net10.0-windows\ /Y
