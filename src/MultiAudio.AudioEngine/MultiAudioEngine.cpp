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
#include <algorithm>

#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "ole32.lib")

const int SAMPLE_RATE = 48000;
const int CHANNELS = 2;
const int BYTES_PER_SAMPLE = sizeof(float);

class DynamicResampler {
    float fractional_pos = 0.0f;
public:
    size_t Process(const float* in, size_t in_frames, float* out, size_t out_frames_cap, float ratio, size_t channels, size_t& input_frames_consumed) {
        size_t frames_written = 0;
        
        while (frames_written < out_frames_cap) {
            int index = (int)fractional_pos;
            if (index + 1 >= in_frames) break; 
            
            float frac = fractional_pos - index;
            
            for (size_t c = 0; c < channels; ++c) {
                float v0 = in[index * channels + c];
                float v1 = in[(index + 1) * channels + c];
                out[frames_written * channels + c] = v0 + (v1 - v0) * frac;
            }
            
            fractional_pos += ratio;
            frames_written++;
        }
        
        input_frames_consumed = (size_t)fractional_pos;
        fractional_pos -= input_frames_consumed;
        return frames_written;
    }
    
    void Reset() { fractional_pos = 0.0f; }
};

struct OutputDevice {
    std::wstring id;
    std::wstring name;
    std::atomic<bool> enabled{false};
    std::atomic<float> volume{1.0f};
    std::atomic<int> delayMs{0};
    
    // Core engine state
    IMMDevice* pDevice = nullptr;
    IAudioClient* pAudioClient = nullptr;
    IAudioRenderClient* pRenderClient = nullptr;
    std::unique_ptr<RingBuffer> ringBuffer;
    DynamicResampler resampler;
    
    std::thread renderThread;
    std::atomic<bool> isRendering{false};
    std::atomic<bool> deviceFailed{false};
};

std::vector<std::shared_ptr<OutputDevice>> g_devices;
IMMDeviceEnumerator* g_pEnumerator = nullptr;
std::mutex g_engineMutex;

// Master Pipeline
std::unique_ptr<RingBuffer> g_masterBuffer;
std::thread g_routerThread;
std::atomic<bool> g_isRouting{false};

// Capture
IMMDevice* g_pCaptureDevice = nullptr;
IAudioClient* g_pCaptureClient = nullptr;
IAudioCaptureClient* g_pCaptureRenderClient = nullptr; 
std::thread g_captureThread;
std::atomic<bool> g_isCapturing{false};

void RenderThreadProc(std::shared_ptr<OutputDevice> pDev);
void CaptureThreadProc();
void RouterThreadProc();
bool TryInitializeDevice(std::shared_ptr<OutputDevice> pDev);

void InitializeEngine() {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), NULL,
        CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
        (void**)&g_pEnumerator);
        
    if (FAILED(hr)) return;
    
    g_masterBuffer = std::make_unique<RingBuffer>(SAMPLE_RATE * CHANNELS * 2); 
    
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
                            dev->ringBuffer = std::make_unique<RingBuffer>(SAMPLE_RATE * CHANNELS * 2); 
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
    g_masterBuffer.reset();
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

    g_masterBuffer->Clear();
    
    g_isRouting = true;
    g_isCapturing = true;
    
    g_captureThread = std::thread(CaptureThreadProc);
    g_routerThread = std::thread(RouterThreadProc);
    
    for (auto& dev : g_devices) {
        if (dev->enabled && !dev->isRendering) {
            dev->isRendering = true;
            dev->deviceFailed = false;
            dev->ringBuffer->Clear();
            dev->resampler.Reset();
            
            // Baseline 100ms safety buffer + configured delay
            int delaySamples = ((dev->delayMs + 100) * SAMPLE_RATE / 1000) * CHANNELS;
            if (delaySamples > 0) {
                std::vector<float> zeros(delaySamples, 0.0f);
                dev->ringBuffer->Write(zeros.data(), delaySamples);
            }
            
            dev->renderThread = std::thread(RenderThreadProc, dev);
        }
    }
}

void StopRouting() {
    g_isCapturing = false;
    g_isRouting = false;
    
    if (g_captureThread.joinable()) g_captureThread.join();
    if (g_routerThread.joinable()) g_routerThread.join();
    
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
                
                if (g_isRouting) {
                    if (enabled && !dev->isRendering) {
                        dev->isRendering = true;
                        dev->deviceFailed = false;
                        dev->ringBuffer->Clear();
                        dev->resampler.Reset();
                        
                        int delaySamples = ((dev->delayMs + 100) * SAMPLE_RATE / 1000) * CHANNELS;
                        if (delaySamples > 0) {
                            std::vector<float> zeros(delaySamples, 0.0f);
                            dev->ringBuffer->Write(zeros.data(), delaySamples);
                        }
                        
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
    for (auto& dev : g_devices) {
        if (dev->id == deviceId) {
            dev->volume = volume;
            break;
        }
    }
}

void SetOutputDelay(const wchar_t* deviceId, int delayMs) {
    for (auto& dev : g_devices) {
        if (dev->id == deviceId) {
            dev->delayMs = delayMs;
            break;
        }
    }
}

void CaptureThreadProc() {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    
    while (g_isCapturing) {
        HRESULT hr = g_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &g_pCaptureDevice);
        if (FAILED(hr)) { Sleep(1000); continue; }

        hr = g_pCaptureDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&g_pCaptureClient);
        if (FAILED(hr)) { g_pCaptureDevice->Release(); g_pCaptureDevice = nullptr; Sleep(1000); continue; }

        WAVEFORMATEX* pwfx = nullptr;
        hr = g_pCaptureClient->GetMixFormat(&pwfx);
        
        hr = g_pCaptureClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000, 0, pwfx, NULL);
        if (FAILED(hr)) {
            CoTaskMemFree(pwfx); g_pCaptureClient->Release(); g_pCaptureClient = nullptr;
            g_pCaptureDevice->Release(); g_pCaptureDevice = nullptr; Sleep(1000); continue;
        }

        hr = g_pCaptureClient->GetService(__uuidof(IAudioCaptureClient), (void**)&g_pCaptureRenderClient);
        g_pCaptureClient->Start();
        
        UINT32 packetLength = 0;
        bool deviceInvalidated = false;
        
        while (g_isCapturing && !deviceInvalidated) {
            Sleep(5);
            hr = g_pCaptureRenderClient->GetNextPacketSize(&packetLength);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { deviceInvalidated = true; break; }
            if (FAILED(hr)) continue;
            
            while (packetLength != 0) {
                BYTE* pData;
                UINT32 numFramesAvailable;
                DWORD flags;
                hr = g_pCaptureRenderClient->GetBuffer(&pData, &numFramesAvailable, &flags, NULL, NULL);
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { deviceInvalidated = true; break; }
                    
                if (SUCCEEDED(hr)) {
                    if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                        float* pFloatData = (float*)pData;
                        size_t sampleCount = numFramesAvailable * pwfx->nChannels;
                        
                        std::vector<float> zeros(sampleCount, 0.0f);
                        float* pWriteData = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? zeros.data() : pFloatData;
                        g_masterBuffer->Write(pWriteData, sampleCount);
                    }
                    g_pCaptureRenderClient->ReleaseBuffer(numFramesAvailable);
                }
                hr = g_pCaptureRenderClient->GetNextPacketSize(&packetLength);
            }
        }
        
        g_pCaptureClient->Stop();
        if (g_pCaptureRenderClient) { g_pCaptureRenderClient->Release(); g_pCaptureRenderClient = nullptr; }
        if (g_pCaptureClient) { g_pCaptureClient->Release(); g_pCaptureClient = nullptr; }
        if (g_pCaptureDevice) { g_pCaptureDevice->Release(); g_pCaptureDevice = nullptr; }
        CoTaskMemFree(pwfx);
    }
    CoUninitialize();
}

void RouterThreadProc() {
    std::vector<float> mixBuffer(9600); // Max 100ms
    
    while (g_isRouting) {
        Sleep(5);
        size_t readCount = g_masterBuffer->Read(mixBuffer.data(), mixBuffer.size());
        if (readCount > 0) {
            std::lock_guard<std::mutex> lock(g_engineMutex);
            for (auto& dev : g_devices) {
                if (dev->enabled && dev->isRendering && !dev->deviceFailed) {
                    dev->ringBuffer->Write(mixBuffer.data(), readCount);
                }
            }
        }
    }
}

bool TryInitializeDevice(std::shared_ptr<OutputDevice> pDev) {
    if (pDev->pRenderClient) { pDev->pRenderClient->Release(); pDev->pRenderClient = nullptr; }
    if (pDev->pAudioClient) { pDev->pAudioClient->Release(); pDev->pAudioClient = nullptr; }
    if (pDev->pDevice) { pDev->pDevice->Release(); pDev->pDevice = nullptr; }

    HRESULT hr = g_pEnumerator->GetDevice(pDev->id.c_str(), &pDev->pDevice);
    if (FAILED(hr)) return false;
    
    hr = pDev->pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pDev->pAudioClient);
    if (FAILED(hr)) return false;

    WAVEFORMATEX* pMixFormat = nullptr;
    hr = pDev->pAudioClient->GetMixFormat(&pMixFormat);
    if (FAILED(hr)) return false;
    
    hr = pDev->pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        10000000, 0, pMixFormat, NULL);
        
    if (FAILED(hr)) { CoTaskMemFree(pMixFormat); return false; }
    
    hr = pDev->pAudioClient->GetService(__uuidof(IAudioRenderClient), (void**)&pDev->pRenderClient);
    CoTaskMemFree(pMixFormat);
    return SUCCEEDED(hr);
}

void RenderThreadProc(std::shared_ptr<OutputDevice> pDev) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    
    // Internal buffers for drift correction resampler
    std::vector<float> inBuffer(9600, 0.0f);
    
    while (pDev->isRendering) {
        if (!TryInitializeDevice(pDev)) {
            pDev->deviceFailed = true;
            Sleep(2000);
            continue;
        }
        
        pDev->deviceFailed = false;
        UINT32 bufferFrameCount;
        pDev->pAudioClient->GetBufferSize(&bufferFrameCount);
        pDev->pAudioClient->Start();
        
        bool invalidated = false;
        while (pDev->isRendering && !invalidated) {
            Sleep(5); 
            
            UINT32 numFramesPadding;
            HRESULT hr = pDev->pAudioClient->GetCurrentPadding(&numFramesPadding);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { invalidated = true; break; }
            if (FAILED(hr)) continue;
            
            UINT32 numFramesAvailable = bufferFrameCount - numFramesPadding;
            if (numFramesAvailable == 0) continue;
            
            BYTE* pData;
            hr = pDev->pRenderClient->GetBuffer(numFramesAvailable, &pData);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { invalidated = true; break; }
            
            if (SUCCEEDED(hr)) {
                float* pFloatOut = (float*)pData;
                
                // Drift Correction Feedback Controller
                size_t targetFrames = ((pDev->delayMs + 100) * SAMPLE_RATE / 1000);
                size_t availableFrames = pDev->ringBuffer->GetAvailableRead() / CHANNELS;
                
                long error = (long)availableFrames - (long)targetFrames;
                
                // P-Controller: Proportional gain
                // E.g., 480 frames error (10ms) * 0.00002 = 0.0096 ratio change
                float P = 0.00002f;
                float ratio = 1.0f + error * P;
                
                // Clamp ratio to prevent audible pitch shifts
                if (ratio > 1.05f) ratio = 1.05f;
                if (ratio < 0.95f) ratio = 0.95f;
                
                // We need to read slightly more input than output if ratio > 1
                // We peek enough input to satisfy the resampler
                size_t maxInputFramesNeeded = (size_t)(numFramesAvailable * ratio) + 2; 
                size_t inSamplesNeeded = maxInputFramesNeeded * CHANNELS;
                
                if (inSamplesNeeded > inBuffer.size()) inBuffer.resize(inSamplesNeeded);
                
                size_t samplesPeeked = pDev->ringBuffer->Peek(inBuffer.data(), inSamplesNeeded);
                size_t framesPeeked = samplesPeeked / CHANNELS;
                
                size_t framesConsumed = 0;
                size_t framesWritten = pDev->resampler.Process(
                    inBuffer.data(), framesPeeked, 
                    pFloatOut, numFramesAvailable, 
                    ratio, CHANNELS, framesConsumed);
                    
                // Advance buffer by strictly what was consumed
                pDev->ringBuffer->Advance(framesConsumed * CHANNELS);
                
                // Fill any remaining output with silence if resampler starved
                if (framesWritten < numFramesAvailable) {
                    for(size_t i = framesWritten * CHANNELS; i < numFramesAvailable * CHANNELS; ++i) {
                        pFloatOut[i] = 0.0f;
                    }
                }
                
                float vol = pDev->volume.load();
                if (vol != 1.0f) {
                    for (size_t i = 0; i < numFramesAvailable * CHANNELS; ++i) {
                        pFloatOut[i] *= vol;
                    }
                }
                
                hr = pDev->pRenderClient->ReleaseBuffer(numFramesAvailable, 0);
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { invalidated = true; break; }
            }
        }
        
        pDev->pAudioClient->Stop();
    }
    
    if (pDev->pRenderClient) { pDev->pRenderClient->Release(); pDev->pRenderClient = nullptr; }
    if (pDev->pAudioClient) { pDev->pAudioClient->Release(); pDev->pAudioClient = nullptr; }
    if (pDev->pDevice) { pDev->pDevice->Release(); pDev->pDevice = nullptr; }
    
    CoUninitialize();
}
