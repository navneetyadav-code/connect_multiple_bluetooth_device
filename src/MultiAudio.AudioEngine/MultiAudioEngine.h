#pragma once
#include <windows.h>
#include <cstdint>

// ============================================================================
// MultiAudioEngine — Native API
// ============================================================================
//
// All functions return an int32_t status code:
//   0  = S_OK (success)
//  <0  = HRESULT failure code from WASAPI or internal error
//
// THREADING CONTRACT:
//   - All functions must be called from the managed EngineController thread
//     (single-threaded command queue), EXCEPT GetDeviceCount/GetDeviceName/
//     GetDeviceId which are safe after InitializeEngine() completes.
//   - The engine internally manages its own capture/render threads.
//
// LIFECYCLE:
//   InitializeEngine()  →  [Start/Stop/Set*]  →  ShutdownEngine()
//
// DEVICE HOTPLUG:
//   The engine registers IMMNotificationClient.  When devices change,
//   the callback set via SetDeviceChangeCallback() is invoked (on an
//   arbitrary COM thread).  The managed layer should re-enumerate.
//

extern "C" {
    __declspec(dllexport) int32_t InitializeEngine();
    __declspec(dllexport) int32_t ShutdownEngine();
    
    __declspec(dllexport) int32_t GetDeviceCount();
    __declspec(dllexport) int32_t GetDeviceName(int index, wchar_t* nameBuffer, int bufferSize);
    __declspec(dllexport) int32_t GetDeviceId(int index, wchar_t* idBuffer, int bufferSize);
    
    __declspec(dllexport) int32_t StartRouting();
    __declspec(dllexport) int32_t StopRouting();
    
    __declspec(dllexport) int32_t SetOutputEnabled(const wchar_t* deviceId, bool enabled);
    __declspec(dllexport) int32_t SetOutputVolume(const wchar_t* deviceId, float volume);
    __declspec(dllexport) int32_t SetOutputDelay(const wchar_t* deviceId, int delayMs);

    // Callback signature for device-change notifications.
    // Called on an arbitrary COM thread — the managed layer must marshal.
    typedef void(__stdcall* DeviceChangeCallback)();
    __declspec(dllexport) void SetDeviceChangeCallback(DeviceChangeCallback callback);

    // Re-enumerate devices.  Safe to call while routing is active.
    // Returns the new device count, or <0 on failure.
    __declspec(dllexport) int32_t RefreshDevices();
}
