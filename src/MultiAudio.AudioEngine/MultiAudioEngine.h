#pragma once
#include <windows.h>

extern "C" {
    __declspec(dllexport) void InitializeEngine();
    __declspec(dllexport) void ShutdownEngine();
    __declspec(dllexport) int GetDeviceCount();
    __declspec(dllexport) void GetDeviceName(int index, wchar_t* nameBuffer, int bufferSize);
    __declspec(dllexport) void GetDeviceId(int index, wchar_t* idBuffer, int bufferSize);
    __declspec(dllexport) void StartCapture();
    __declspec(dllexport) void StopCapture();
    __declspec(dllexport) void AddOutputDevice(const wchar_t* deviceId);
    __declspec(dllexport) void RemoveOutputDevice(const wchar_t* deviceId);
    __declspec(dllexport) void SetDeviceVolume(const wchar_t* deviceId, float volume);
    __declspec(dllexport) void SetDeviceDelay(const wchar_t* deviceId, int delayMs);
}
