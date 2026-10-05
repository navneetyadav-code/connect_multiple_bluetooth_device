#pragma once
#include <windows.h>

extern "C" {
    __declspec(dllexport) void InitializeEngine();
    __declspec(dllexport) void ShutdownEngine();
    
    __declspec(dllexport) int GetDeviceCount();
    __declspec(dllexport) void GetDeviceName(int index, wchar_t* nameBuffer, int bufferSize);
    __declspec(dllexport) void GetDeviceId(int index, wchar_t* idBuffer, int bufferSize);
    
    __declspec(dllexport) void StartRouting();
    __declspec(dllexport) void StopRouting();
    
    __declspec(dllexport) void SetOutputEnabled(const wchar_t* deviceId, bool enabled);
    __declspec(dllexport) void SetOutputVolume(const wchar_t* deviceId, float volume);
    __declspec(dllexport) void SetOutputDelay(const wchar_t* deviceId, int delayMs);
}
