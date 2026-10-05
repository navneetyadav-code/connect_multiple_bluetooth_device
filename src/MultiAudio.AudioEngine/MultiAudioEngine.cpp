#include "MultiAudioEngine.h"
#include "RingBuffer.h"
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <vector>
#include <string>
#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>
#include <memory>

#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "ole32.lib")

// Internal audio format (48kHz, 32-bit float, Stereo)
const int SAMPLE_RATE = 48000;
const int CHANNELS = 2;
const int BYTES_PER_SAMPLE = sizeof(float);

struct OutputDevice {
    std::wstring id;
    std::wstring name;
    bool enabled = false;
    float volume = 1.0f;
    int delayMs = 0;
    
    // Audio engine components
    IMMDevice* pDevice = nullptr;
    IAudioClient* pAudioClient = nullptr;
    IAudioRenderClient* pRenderClient = nullptr;
    std::unique_ptr<RingBuffer> ringBuffer;
    
    std::thread renderThread;
    std::atomic<bool> isRendering{false};
};

// Globals
std::vector<std::shared_ptr<OutputDevice>> g_devices;
IMMDeviceEnumerator* g_pEnumerator = nullptr;
std::mutex g_engineMutex;

// Capture Globals
IMMDevice* g_pCaptureDevice = nullptr;
IAudioClient* g_pCaptureClient = nullptr;
IAudioCaptureClient* g_pCaptureRenderClient = nullptr; 
std::thread g_captureThread;
std::atomic<bool> g_isCapturing{false};

void RenderThreadProc(std::shared_ptr<OutputDevice> pDev);
void CaptureThreadProc();

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
                            auto dev = std::make_shared<OutputDevice>();
                            dev->id = pwszID;
                            dev->name = varName.pwszVal;
                            
                            // Initialize ring buffer for 1 second of audio (48000 samples * 2 channels)
                            dev->ringBuffer = std::make_unique<RingBuffer>(SAMPLE_RATE * CHANNELS);
                            
                            g_devices.push_back(dev);
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
    StopRouting();
    
    std::lock_guard<std::mutex> lock(g_engineMutex);
    g_devices.clear();
    
    if (g_pEnumerator) {
        g_pEnumerator->Release();
        g_pEnumerator = nullptr;
    }
    CoUninitialize();
}

int GetDeviceCount() {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    return (int)g_devices.size();
}

void GetDeviceName(int index, wchar_t* nameBuffer, int bufferSize) {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    if (index >= 0 && index < g_devices.size()) {
        wcsncpy_s(nameBuffer, bufferSize, g_devices[index]->name.c_str(), _TRUNCATE);
    }
}

void GetDeviceId(int index, wchar_t* idBuffer, int bufferSize) {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    if (index >= 0 && index < g_devices.size()) {
        wcsncpy_s(idBuffer, bufferSize, g_devices[index]->id.c_str(), _TRUNCATE);
    }
}

void StartRouting() {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    if (g_isCapturing) return;

    // Start Capture
    HRESULT hr = g_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &g_pCaptureDevice);
    if (FAILED(hr)) return;

    hr = g_pCaptureDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&g_pCaptureClient);
    if (FAILED(hr)) return;

    WAVEFORMATEX* pwfx = nullptr;
    hr = g_pCaptureClient->GetMixFormat(&pwfx);
    if (FAILED(hr)) return;
    
    hr = g_pCaptureClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        10000000, 
        0,
        pwfx,
        NULL);
        
    if (FAILED(hr)) {
        CoTaskMemFree(pwfx);
        return;
    }

    hr = g_pCaptureClient->GetService(__uuidof(IAudioCaptureClient), (void**)&g_pCaptureRenderClient);
    if (FAILED(hr)) return;

    g_isCapturing = true;
    g_captureThread = std::thread(CaptureThreadProc);
    
    // Start Outputs
    for (auto& dev : g_devices) {
        if (dev->enabled && !dev->isRendering) {
            dev->isRendering = true;
            dev->ringBuffer->Clear();
            dev->renderThread = std::thread(RenderThreadProc, dev);
        }
    }
    
    CoTaskMemFree(pwfx);
}

void StopRouting() {
    g_isCapturing = false;
    
    if (g_captureThread.joinable()) {
        g_captureThread.join();
    }
    
    std::lock_guard<std::mutex> lock(g_engineMutex);
    for (auto& dev : g_devices) {
        dev->isRendering = false;
        if (dev->renderThread.joinable()) {
            dev->renderThread.join();
        }
    }
    
    if (g_pCaptureRenderClient) {
        g_pCaptureRenderClient->Release();
        g_pCaptureRenderClient = nullptr;
    }
    if (g_pCaptureClient) {
        g_pCaptureClient->Release();
        g_pCaptureClient = nullptr;
    }
    if (g_pCaptureDevice) {
        g_pCaptureDevice->Release();
        g_pCaptureDevice = nullptr;
    }
}

void SetOutputEnabled(const wchar_t* deviceId, bool enabled) {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    for (auto& dev : g_devices) {
        if (dev->id == deviceId) {
            if (dev->enabled != enabled) {
                dev->enabled = enabled;
                
                if (g_isCapturing) {
                    if (enabled && !dev->isRendering) {
                        dev->isRendering = true;
                        dev->ringBuffer->Clear();
                        dev->renderThread = std::thread(RenderThreadProc, dev);
                    } else if (!enabled && dev->isRendering) {
                        dev->isRendering = false;
                        if (dev->renderThread.joinable()) {
                            dev->renderThread.join();
                        }
                    }
                }
            }
            break;
        }
    }
}

void SetOutputVolume(const wchar_t* deviceId, float volume) {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    for (auto& dev : g_devices) {
        if (dev->id == deviceId) {
            dev->volume = volume;
            break;
        }
    }
}

void SetOutputDelay(const wchar_t* deviceId, int delayMs) {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    for (auto& dev : g_devices) {
        if (dev->id == deviceId) {
            dev->delayMs = delayMs;
            break;
        }
    }
}

void CaptureThreadProc() {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    
    g_pCaptureClient->Start();
    
    WAVEFORMATEX* pwfx = nullptr;
    g_pCaptureClient->GetMixFormat(&pwfx);
    
    UINT32 packetLength = 0;
    while (g_isCapturing) {
        Sleep(5);
        
        HRESULT hr = g_pCaptureRenderClient->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) continue;
        
        while (packetLength != 0) {
            BYTE* pData;
            UINT32 numFramesAvailable;
            DWORD flags;
            
            hr = g_pCaptureRenderClient->GetBuffer(
                &pData,
                &numFramesAvailable,
                &flags,
                NULL,
                NULL);
                
            if (SUCCEEDED(hr)) {
                if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                    float* pFloatData = (float*)pData;
                    size_t sampleCount = numFramesAvailable * pwfx->nChannels;
                    
                    std::vector<float> zeros(sampleCount, 0.0f);
                    float* pWriteData = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? zeros.data() : pFloatData;
                    
                    std::lock_guard<std::mutex> lock(g_engineMutex);
                    for (auto& dev : g_devices) {
                        if (dev->enabled && dev->isRendering) {
                            dev->ringBuffer->Write(pWriteData, sampleCount);
                        }
                    }
                }
                
                g_pCaptureRenderClient->ReleaseBuffer(numFramesAvailable);
            }
            
            hr = g_pCaptureRenderClient->GetNextPacketSize(&packetLength);
        }
    }
    
    g_pCaptureClient->Stop();
    CoTaskMemFree(pwfx);
    CoUninitialize();
}

void RenderThreadProc(std::shared_ptr<OutputDevice> pDev) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    
    HRESULT hr = g_pEnumerator->GetDevice(pDev->id.c_str(), &pDev->pDevice);
    if (FAILED(hr)) return;
    
    hr = pDev->pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pDev->pAudioClient);
    if (FAILED(hr)) return;

    WAVEFORMATEX* pMixFormat = nullptr;
    g_pCaptureClient->GetMixFormat(&pMixFormat); 
    
    hr = pDev->pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        10000000,
        0,
        pMixFormat,
        NULL);
        
    if (FAILED(hr)) {
        CoTaskMemFree(pMixFormat);
        return;
    }
    
    hr = pDev->pAudioClient->GetService(__uuidof(IAudioRenderClient), (void**)&pDev->pRenderClient);
    if (FAILED(hr)) return;
    
    UINT32 bufferFrameCount;
    pDev->pAudioClient->GetBufferSize(&bufferFrameCount);
    
    pDev->pAudioClient->Start();
    
    while (pDev->isRendering) {
        Sleep(5); 
        
        UINT32 numFramesPadding;
        hr = pDev->pAudioClient->GetCurrentPadding(&numFramesPadding);
        if (FAILED(hr)) continue;
        
        UINT32 numFramesAvailable = bufferFrameCount - numFramesPadding;
        if (numFramesAvailable == 0) continue;
        
        BYTE* pData;
        hr = pDev->pRenderClient->GetBuffer(numFramesAvailable, &pData);
        if (SUCCEEDED(hr)) {
            size_t sampleCount = numFramesAvailable * pMixFormat->nChannels;
            float* pFloatData = (float*)pData;
            
            pDev->ringBuffer->Read(pFloatData, sampleCount);
            
            float vol = pDev->volume;
            if (vol != 1.0f) {
                for (size_t i = 0; i < sampleCount; ++i) {
                    pFloatData[i] *= vol;
                }
            }
            
            pDev->pRenderClient->ReleaseBuffer(numFramesAvailable, 0);
        }
    }
    
    pDev->pAudioClient->Stop();
    
    if (pDev->pRenderClient) {
        pDev->pRenderClient->Release();
        pDev->pRenderClient = nullptr;
    }
    if (pDev->pAudioClient) {
        pDev->pAudioClient->Release();
        pDev->pAudioClient = nullptr;
    }
    if (pDev->pDevice) {
        pDev->pDevice->Release();
        pDev->pDevice = nullptr;
    }
    
    CoTaskMemFree(pMixFormat);
    CoUninitialize();
}
