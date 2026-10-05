#include "MultiAudioEngine.h"
#include <mmdeviceapi.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <vector>
#include <string>
#include <iostream>

#pragma comment(lib, "mmdevapi.lib")

struct AudioDevice {
    std::wstring id;
    std::wstring name;
};

std::vector<AudioDevice> g_devices;
IMMDeviceEnumerator* g_pEnumerator = nullptr;

void InitializeEngine() {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), NULL,
        CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
        (void**)&g_pEnumerator);
        
    if (FAILED(hr)) return;
    
    IMMDeviceCollection* pCollection = nullptr;
    hr = g_pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
    if (SUCCEEDED(hr)) {
        UINT count;
        pCollection->GetCount(&count);
        for (UINT i = 0; i < count; i++) {
            IMMDevice* pEndpoint = nullptr;
            hr = pCollection->Item(i, &pEndpoint);
            if (SUCCEEDED(hr)) {
                LPWSTR pwszID = nullptr;
                hr = pEndpoint->GetId(&pwszID);
                if (SUCCEEDED(hr)) {
                    IPropertyStore* pProps = nullptr;
                    hr = pEndpoint->OpenPropertyStore(STGM_READ, &pProps);
                    if (SUCCEEDED(hr)) {
                        PROPVARIANT varName;
                        PropVariantInit(&varName);
                        hr = pProps->GetValue(PKEY_Device_FriendlyName, &varName);
                        if (SUCCEEDED(hr)) {
                            g_devices.push_back({ pwszID, varName.pwszVal });
                            PropVariantClear(&varName);
                        }
                        pProps->Release();
                    }
                    CoTaskMemFree(pwszID);
                }
                pEndpoint->Release();
            }
        }
        pCollection->Release();
    }
}

void ShutdownEngine() {
    if (g_pEnumerator) {
        g_pEnumerator->Release();
        g_pEnumerator = nullptr;
    }
    g_devices.clear();
    CoUninitialize();
}

int GetDeviceCount() {
    return (int)g_devices.size();
}

void GetDeviceName(int index, wchar_t* nameBuffer, int bufferSize) {
    if (index >= 0 && index < g_devices.size()) {
        wcsncpy_s(nameBuffer, bufferSize, g_devices[index].name.c_str(), _TRUNCATE);
    }
}

void GetDeviceId(int index, wchar_t* idBuffer, int bufferSize) {
    if (index >= 0 && index < g_devices.size()) {
        wcsncpy_s(idBuffer, bufferSize, g_devices[index].id.c_str(), _TRUNCATE);
    }
}

void StartCapture() {
    // TODO: Implement WASAPI Loopback Capture
}

void StopCapture() {
    // TODO: Stop Capture
}

void AddOutputDevice(const wchar_t* deviceId) {
    // TODO: Initialize WASAPI render for this device and add to mixer
}

void RemoveOutputDevice(const wchar_t* deviceId) {
    // TODO: Stop and remove
}

void SetDeviceVolume(const wchar_t* deviceId, float volume) {
    // TODO: Set per-device software volume in mixer
}

void SetDeviceDelay(const wchar_t* deviceId, int delayMs) {
    // TODO: Set delay buffer size
}
