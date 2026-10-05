#include "MultiAudioEngine.h"
#include "RingBuffer.h"
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "ole32.lib")

static const int ENGINE_SAMPLE_RATE = 48000;
static const int ENGINE_CHANNELS    = 2;

class DynamicResampler {
    float fractional_pos = 0.0f;
public:
    size_t Process(const float* in,  size_t in_frames,
                   float*       out, size_t out_frames_cap,
                   float ratio, size_t channels,
                   size_t& input_frames_consumed)
    {
        size_t frames_out = 0;

        while (frames_out < out_frames_cap) {
            size_t idx0 = (size_t)fractional_pos;
            size_t idx1 = idx0 + 1;

            if (idx1 >= in_frames) break;

            float frac = fractional_pos - (float)idx0;

            for (size_t c = 0; c < channels; ++c) {
                float v0 = in[idx0 * channels + c];
                float v1 = in[idx1 * channels + c];
                out[frames_out * channels + c] = v0 + (v1 - v0) * frac;
            }

            fractional_pos += ratio;
            frames_out++;
        }

        input_frames_consumed = (size_t)fractional_pos;
        fractional_pos -= (float)input_frames_consumed;   
        return frames_out;
    }
    void Reset() { fractional_pos = 0.0f; }
};

struct OutputDevice {
    std::wstring id;
    std::wstring name;
    std::atomic<bool>  enabled    { false };
    std::atomic<float> volume     { 1.0f  };
    std::atomic<int>   syncDelayMs   { 0  };  
    std::atomic<int>   targetBufferMs{ 100 }; 
    IMMDevice*         pDevice       = nullptr;
    IAudioClient*      pAudioClient  = nullptr;
    IAudioRenderClient* pRenderClient= nullptr;
    WAVEFORMATEX*      pDeviceFormat = nullptr; 
    HANDLE             hEvent        = NULL;
    std::unique_ptr<RingBuffer> ringBuffer;
    DynamicResampler resampler;
    std::thread        renderThread;
    std::atomic<bool>  isRendering  { false };
    std::atomic<bool>  deviceFailed { false };
};

static std::vector<std::shared_ptr<OutputDevice>> g_devices;
static IMMDeviceEnumerator* g_pEnumerator = nullptr;
static std::mutex g_engineMutex;
static std::unique_ptr<RingBuffer> g_masterBuffer;
static HANDLE g_hRouterEvent = NULL;
static std::atomic<bool> g_isRouting{false};

static IMMDevice*         g_pCaptureDevice       = nullptr;
static IAudioClient*      g_pCaptureClient        = nullptr;
static IAudioCaptureClient* g_pCaptureCaptureClient = nullptr;
static WAVEFORMATEX*      g_pCaptureMixFormat     = nullptr; 
static HANDLE             g_hCaptureEvent         = NULL;
static std::thread        g_captureThread;
static std::atomic<bool>  g_isCapturing           { false };

static void CaptureThreadProc();
static void RenderThreadProc(std::shared_ptr<OutputDevice> pDev);
static bool TryInitRenderDevice(std::shared_ptr<OutputDevice> pDev);
static void CleanupRenderDevice(std::shared_ptr<OutputDevice> pDev);
static void CleanupCapture();

static bool IsIEEEFloat(const WAVEFORMATEX* pwfx) {
    if (!pwfx) return false;
    if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE* pEx = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(pwfx);
        return (pEx->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }
    return false;
}

// Issue #3 Fix: Use pre-allocated buffers, no allocations in realtime thread.
// Issue #2 Fix: Convert to strict 48kHz Stereo float.
static DynamicResampler g_captureResampler;

static void ConvertToEngineFormat(const BYTE* pData, const WAVEFORMATEX* pwfx, UINT32 numFrames, std::vector<float>& intermediateBuffer, std::vector<float>& engineBuffer) 
{
    const UINT32 numChannels = pwfx->nChannels;
    const UINT32 numSamples  = numFrames * numChannels;
    
    // Ensure intermediate fits the raw converted data
    if (intermediateBuffer.size() < numSamples) {
        intermediateBuffer.resize(numSamples); // Will only allocate on first few frames if size changes
    }
    
    // 1. Convert to Float
    if (IsIEEEFloat(pwfx)) {
        const float* src = reinterpret_cast<const float*>(pData);
        for (UINT32 i = 0; i < numSamples; ++i) intermediateBuffer[i] = src[i];
    } else {
        const WORD bitsPerSample = pwfx->wBitsPerSample;
        const WORD blockAlign    = pwfx->nBlockAlign;
        if (bitsPerSample == 16) {
            for (UINT32 i = 0; i < numSamples; ++i) {
                const int16_t s = *reinterpret_cast<const int16_t*>(pData + (i / numChannels) * blockAlign + (i % numChannels) * sizeof(int16_t));
                intermediateBuffer[i] = s / 32768.0f;
            }
        } else if (bitsPerSample == 24) {
            for (UINT32 i = 0; i < numSamples; ++i) {
                size_t byteOff = (i / numChannels) * blockAlign + (i % numChannels) * 3;
                int32_t s = ((int32_t)pData[byteOff + 2] << 16) | ((int32_t)pData[byteOff + 1] << 8) | (int32_t)pData[byteOff];
                if (s & 0x800000) s |= 0xFF000000;
                intermediateBuffer[i] = s / 8388608.0f;
            }
        } else if (bitsPerSample == 32) {
            for (UINT32 i = 0; i < numSamples; ++i) {
                const int32_t s = *reinterpret_cast<const int32_t*>(pData + (i / numChannels) * blockAlign + (i % numChannels) * sizeof(int32_t));
                intermediateBuffer[i] = s / 2147483648.0f;
            }
        }
    }

    // 2. Mixdown / Upmix to ENGINE_CHANNELS (Stereo)
    std::vector<float>& stereoBuffer = (numChannels == ENGINE_CHANNELS) ? intermediateBuffer : engineBuffer;
    
    if (numChannels != ENGINE_CHANNELS) {
        if (engineBuffer.size() < numFrames * ENGINE_CHANNELS) {
            engineBuffer.resize(numFrames * ENGINE_CHANNELS);
        }
        for (UINT32 i = 0; i < numFrames; ++i) {
            float left = 0.0f, right = 0.0f;
            if (numChannels == 1) { // Mono to Stereo
                left = intermediateBuffer[i];
                right = left;
            } else if (numChannels > 2) { // 5.1/7.1 to Stereo (naive downmix)
                left = intermediateBuffer[i * numChannels + 0] + 0.7f * intermediateBuffer[i * numChannels + 2];
                right = intermediateBuffer[i * numChannels + 1] + 0.7f * intermediateBuffer[i * numChannels + 2];
                // Clamp
                if (left > 1.0f) left = 1.0f; if (left < -1.0f) left = -1.0f;
                if (right > 1.0f) right = 1.0f; if (right < -1.0f) right = -1.0f;
            }
            stereoBuffer[i * ENGINE_CHANNELS + 0] = left;
            stereoBuffer[i * ENGINE_CHANNELS + 1] = right;
        }
    }
    
    // 3. Resample to ENGINE_SAMPLE_RATE
    if (pwfx->nSamplesPerSec == ENGINE_SAMPLE_RATE) {
        g_masterBuffer->Write(stereoBuffer.data(), numFrames * ENGINE_CHANNELS);
    } else {
        float ratio = (float)pwfx->nSamplesPerSec / (float)ENGINE_SAMPLE_RATE;
        size_t maxOutputFrames = (size_t)(numFrames / ratio) + 10;
        
        static std::vector<float> resampledBuffer;
        if (resampledBuffer.size() < maxOutputFrames * ENGINE_CHANNELS) {
            resampledBuffer.resize(maxOutputFrames * ENGINE_CHANNELS);
        }
        
        size_t framesConsumed = 0;
        size_t framesWritten = g_captureResampler.Process(
            stereoBuffer.data(), numFrames,
            resampledBuffer.data(), maxOutputFrames,
            ratio, ENGINE_CHANNELS, framesConsumed);
            
        if (framesWritten > 0) {
            g_masterBuffer->Write(resampledBuffer.data(), framesWritten * ENGINE_CHANNELS);
        }
    }
}

void InitializeEngine() {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&g_pEnumerator);
    if (FAILED(hr)) return;

    g_masterBuffer = std::make_unique<RingBuffer>(ENGINE_SAMPLE_RATE * ENGINE_CHANNELS * 2);
    g_hRouterEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    IMMDeviceCollection* pCollection = nullptr;
    hr = g_pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
    if (FAILED(hr)) return;

    UINT count = 0;
    pCollection->GetCount(&count);

    for (UINT i = 0; i < count; ++i) {
        IMMDevice* pEndpoint = nullptr;
        hr = pCollection->Item(i, &pEndpoint);
        if (FAILED(hr)) continue;

        LPWSTR pwszID = nullptr;
        hr = pEndpoint->GetId(&pwszID);
        if (FAILED(hr)) { pEndpoint->Release(); continue; }

        IPropertyStore* pProps = nullptr;
        hr = pEndpoint->OpenPropertyStore(STGM_READ, &pProps);
        if (FAILED(hr)) { CoTaskMemFree(pwszID); pEndpoint->Release(); continue; }

        PROPVARIANT varName;
        PropVariantInit(&varName);
        hr = pProps->GetValue(PKEY_Device_FriendlyName, &varName);
        if (SUCCEEDED(hr)) {
            auto dev = std::make_shared<OutputDevice>();
            dev->id   = pwszID;
            dev->name = varName.pwszVal;
            dev->ringBuffer = std::make_unique<RingBuffer>(ENGINE_SAMPLE_RATE * ENGINE_CHANNELS * 2);
            g_devices.push_back(dev);
            PropVariantClear(&varName);
        }
        pProps->Release();
        CoTaskMemFree(pwszID);
        pEndpoint->Release();
    }
    pCollection->Release();
}

void ShutdownEngine() {
    StopRouting();
    std::lock_guard<std::mutex> lock(g_engineMutex);
    g_devices.clear();
    if (g_pEnumerator) { g_pEnumerator->Release(); g_pEnumerator = nullptr; }
    if (g_hRouterEvent) { CloseHandle(g_hRouterEvent); g_hRouterEvent = NULL; }
    g_masterBuffer.reset();
    CoUninitialize();
}

int GetDeviceCount() {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    return (int)g_devices.size();
}

void GetDeviceName(int index, wchar_t* nameBuffer, int bufferSize) {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    if (index >= 0 && index < (int)g_devices.size())
        wcsncpy_s(nameBuffer, bufferSize, g_devices[index]->name.c_str(), _TRUNCATE);
}

void GetDeviceId(int index, wchar_t* idBuffer, int bufferSize) {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    if (index >= 0 && index < (int)g_devices.size())
        wcsncpy_s(idBuffer, bufferSize, g_devices[index]->id.c_str(), _TRUNCATE);
}

static void ActivateDevice(std::shared_ptr<OutputDevice>& dev) {
    dev->isRendering = true;
    dev->deviceFailed = false;
    dev->ringBuffer->Clear();
    dev->resampler.Reset();

    int delaySamples = (dev->syncDelayMs.load() * ENGINE_SAMPLE_RATE / 1000) * ENGINE_CHANNELS;
    if (delaySamples > 0) {
        std::vector<float> zeros(delaySamples, 0.0f);
        dev->ringBuffer->Write(zeros.data(), delaySamples);
    }
    dev->renderThread = std::thread(RenderThreadProc, dev);
}

void StartRouting() {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    if (g_isCapturing.load()) return;

    g_masterBuffer->Clear();
    g_captureResampler.Reset();
    g_isRouting   = true;
    g_isCapturing = true;

    g_captureThread = std::thread(CaptureThreadProc);
    
    for (auto& dev : g_devices) {
        if (dev->enabled && !dev->isRendering) ActivateDevice(dev);
    }
}

void StopRouting() {
    g_isCapturing = false;
    g_isRouting   = false;
    if (g_captureThread.joinable()) g_captureThread.join();

    std::vector<std::thread> threadsToJoin;
    {
        std::lock_guard<std::mutex> lock(g_engineMutex);
        for (auto& dev : g_devices) {
            dev->isRendering = false;
            if (dev->renderThread.joinable()) {
                threadsToJoin.push_back(std::move(dev->renderThread));
            }
        }
    }
    for (auto& t : threadsToJoin) { if (t.joinable()) t.join(); }
    CleanupCapture();
}

void SetOutputEnabled(const wchar_t* deviceId, bool enabled) {
    std::thread threadToJoin;
    {
        std::lock_guard<std::mutex> lock(g_engineMutex);
        for (auto& dev : g_devices) {
            if (dev->id != deviceId) continue;
            if (dev->enabled == enabled) break;

            dev->enabled = enabled;
            if (g_isRouting.load()) {
                if (enabled && !dev->isRendering) {
                    ActivateDevice(dev);
                } else if (!enabled && dev->isRendering) {
                    dev->isRendering = false;
                    if (dev->renderThread.joinable()) {
                        threadToJoin = std::move(dev->renderThread);
                    }
                }
            }
            break;
        }
    }
    if (threadToJoin.joinable()) threadToJoin.join();
}

void SetOutputVolume(const wchar_t* deviceId, float volume) {
    for (auto& dev : g_devices) { if (dev->id == deviceId) { dev->volume = volume; break; } }
}

void SetOutputDelay(const wchar_t* deviceId, int delayMs) {
    for (auto& dev : g_devices) { if (dev->id == deviceId) { dev->syncDelayMs = delayMs; break; } }
}

static void CleanupCapture() {
    if (g_pCaptureCaptureClient) { g_pCaptureCaptureClient->Release(); g_pCaptureCaptureClient = nullptr; }
    if (g_pCaptureClient)        { g_pCaptureClient->Stop(); g_pCaptureClient->Release(); g_pCaptureClient = nullptr; }
    if (g_pCaptureDevice)        { g_pCaptureDevice->Release(); g_pCaptureDevice = nullptr; }
    if (g_pCaptureMixFormat)     { CoTaskMemFree(g_pCaptureMixFormat); g_pCaptureMixFormat = nullptr; }
    if (g_hCaptureEvent)         { CloseHandle(g_hCaptureEvent); g_hCaptureEvent = NULL; }
}

static void CaptureThreadProc() {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    std::vector<float> intermediateBuffer(192000, 0.0f); // Pre-allocated 1 second buffer
    std::vector<float> engineBuffer(192000, 0.0f);
    std::vector<float> silenceBuffer(192000, 0.0f);

    while (g_isCapturing.load()) {
        HRESULT hr = g_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &g_pCaptureDevice);
        if (FAILED(hr)) { Sleep(1000); continue; }

        hr = g_pCaptureDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&g_pCaptureClient);
        if (FAILED(hr)) { CleanupCapture(); Sleep(1000); continue; }

        hr = g_pCaptureClient->GetMixFormat(&g_pCaptureMixFormat);
        if (FAILED(hr)) { CleanupCapture(); Sleep(1000); continue; }

        hr = g_pCaptureClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            0, 0, g_pCaptureMixFormat, NULL);
        if (FAILED(hr)) { CleanupCapture(); Sleep(1000); continue; }
        
        g_hCaptureEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        hr = g_pCaptureClient->SetEventHandle(g_hCaptureEvent);
        if (FAILED(hr)) { CleanupCapture(); Sleep(1000); continue; }

        hr = g_pCaptureClient->GetService(__uuidof(IAudioCaptureClient), (void**)&g_pCaptureCaptureClient);
        if (FAILED(hr)) { CleanupCapture(); Sleep(1000); continue; }

        hr = g_pCaptureClient->Start();
        if (FAILED(hr)) { CleanupCapture(); Sleep(1000); continue; }

        bool deviceInvalidated = false;
        while (g_isCapturing.load() && !deviceInvalidated) {
            DWORD waitResult = WaitForSingleObject(g_hCaptureEvent, 100);
            if (waitResult != WAIT_OBJECT_0) continue;

            UINT32 packetLength = 0;
            hr = g_pCaptureCaptureClient->GetNextPacketSize(&packetLength);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { deviceInvalidated = true; break; }
            if (FAILED(hr)) continue;

            while (packetLength != 0) {
                BYTE*  pData = nullptr;
                UINT32 numFramesAvailable = 0;
                DWORD  flags = 0;

                hr = g_pCaptureCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, NULL, NULL);
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { deviceInvalidated = true; break; }
                if (FAILED(hr)) break;

                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    size_t count = numFramesAvailable * ENGINE_CHANNELS;
                    if (count > silenceBuffer.size()) silenceBuffer.resize(count, 0.0f);
                    g_masterBuffer->Write(silenceBuffer.data(), count);
                } else {
                    ConvertToEngineFormat(pData, g_pCaptureMixFormat, numFramesAvailable, intermediateBuffer, engineBuffer);
                }

                hr = g_pCaptureCaptureClient->ReleaseBuffer(numFramesAvailable);
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { deviceInvalidated = true; break; }

                hr = g_pCaptureCaptureClient->GetNextPacketSize(&packetLength);
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { deviceInvalidated = true; break; }
                if (FAILED(hr)) break;
            }
            
            // Inline Routing (No separate Router Thread required)
            // Distribute master buffer to all active devices lock-free
            size_t availableFrames = g_masterBuffer->GetAvailableRead() / ENGINE_CHANNELS;
            if (availableFrames > 0) {
                size_t mixBufferFrames = availableFrames;
                if (mixBufferFrames * ENGINE_CHANNELS > engineBuffer.size()) engineBuffer.resize(mixBufferFrames * ENGINE_CHANNELS);
                
                size_t readCount = g_masterBuffer->Read(engineBuffer.data(), mixBufferFrames * ENGINE_CHANNELS);
                if (readCount > 0) {
                    for (auto& dev : g_devices) {
                        if (dev->enabled.load() && dev->isRendering.load() && !dev->deviceFailed.load()) {
                            dev->ringBuffer->Write(engineBuffer.data(), readCount);
                        }
                    }
                }
            }
        }

        CleanupCapture();
        if (deviceInvalidated) Sleep(500); 
    }
    CoUninitialize();
}

static void CleanupRenderDevice(std::shared_ptr<OutputDevice> pDev) {
    if (pDev->pRenderClient) { pDev->pRenderClient->Release(); pDev->pRenderClient = nullptr; }
    if (pDev->pAudioClient)  {
        pDev->pAudioClient->Stop();
        pDev->pAudioClient->Release();
        pDev->pAudioClient = nullptr;
    }
    if (pDev->pDevice)       { pDev->pDevice->Release();      pDev->pDevice       = nullptr; }
    if (pDev->pDeviceFormat) { CoTaskMemFree(pDev->pDeviceFormat); pDev->pDeviceFormat = nullptr; }
    if (pDev->hEvent)        { CloseHandle(pDev->hEvent);     pDev->hEvent        = NULL; }
}

static bool TryInitRenderDevice(std::shared_ptr<OutputDevice> pDev) {
    CleanupRenderDevice(pDev);
    HRESULT hr = g_pEnumerator->GetDevice(pDev->id.c_str(), &pDev->pDevice);
    if (FAILED(hr)) return false;

    hr = pDev->pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pDev->pAudioClient);
    if (FAILED(hr)) { CleanupRenderDevice(pDev); return false; }

    hr = pDev->pAudioClient->GetMixFormat(&pDev->pDeviceFormat);
    if (FAILED(hr)) { CleanupRenderDevice(pDev); return false; }

    hr = pDev->pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        0, 0, pDev->pDeviceFormat, NULL); 
    if (FAILED(hr)) { CleanupRenderDevice(pDev); return false; }
    
    pDev->hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    hr = pDev->pAudioClient->SetEventHandle(pDev->hEvent);
    if (FAILED(hr)) { CleanupRenderDevice(pDev); return false; }

    hr = pDev->pAudioClient->GetService(__uuidof(IAudioRenderClient), (void**)&pDev->pRenderClient);
    if (FAILED(hr)) { CleanupRenderDevice(pDev); return false; }

    hr = pDev->pAudioClient->Start();
    if (FAILED(hr)) { CleanupRenderDevice(pDev); return false; }
    return true;
}

static void RenderThreadProc(std::shared_ptr<OutputDevice> pDev) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    std::vector<float> inBuffer((ENGINE_SAMPLE_RATE / 5 + 2) * ENGINE_CHANNELS, 0.0f);

    while (pDev->isRendering.load()) {
        if (!TryInitRenderDevice(pDev)) {
            pDev->deviceFailed = true;
            Sleep(2000);
            continue;
        }
        pDev->deviceFailed = false;

        UINT32 bufferFrameCount = 0;
        pDev->pAudioClient->GetBufferSize(&bufferFrameCount);

        bool invalidated = false;
        while (pDev->isRendering.load() && !invalidated) {
            DWORD waitResult = WaitForSingleObject(pDev->hEvent, 100);
            if (waitResult != WAIT_OBJECT_0) continue;

            UINT32 padding = 0;
            HRESULT hr = pDev->pAudioClient->GetCurrentPadding(&padding);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { invalidated = true; break; }
            if (FAILED(hr)) continue;

            UINT32 framesWanted = bufferFrameCount - padding;
            if (framesWanted == 0) continue;

            BYTE* pData = nullptr;
            hr = pDev->pRenderClient->GetBuffer(framesWanted, &pData);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { invalidated = true; break; }
            if (FAILED(hr)) continue;

            float* pOut = reinterpret_cast<float*>(pData);

            size_t targetFrames  = (pDev->targetBufferMs.load() * ENGINE_SAMPLE_RATE / 1000);
            size_t availFrames   = pDev->ringBuffer->GetAvailableRead() / ENGINE_CHANNELS;
            long error = (long)availFrames - (long)targetFrames;

            const float P = 0.00002f;
            float ratio = 1.0f + (float)error * P;
            if (ratio > 1.05f) ratio = 1.05f;
            if (ratio < 0.95f) ratio = 0.95f;

            size_t maxInputFrames = (size_t)((float)framesWanted * ratio) + 2; 
            size_t peekSamples   = maxInputFrames * ENGINE_CHANNELS;

            if (peekSamples > inBuffer.size()) {
                peekSamples = inBuffer.size();
                maxInputFrames = peekSamples / ENGINE_CHANNELS;
            }

            // Issue #4 Fix: Prevent resampler from reading zero-filled fake data.
            // Peek only returns what's actually valid, without padding.
            size_t actualPeekedSamples = pDev->ringBuffer->Peek(inBuffer.data(), peekSamples);
            size_t actualPeekedFrames  = actualPeekedSamples / ENGINE_CHANNELS;

            size_t framesConsumed = 0;
            size_t framesWritten = 0;

            if (actualPeekedFrames > 1) {
                // We only ask the resampler to produce what it has valid input for
                size_t actualFramesWanted = framesWanted;
                if (actualPeekedFrames < maxInputFrames) {
                    actualFramesWanted = (size_t)((actualPeekedFrames - 1) / ratio);
                }
                
                framesWritten = pDev->resampler.Process(
                    inBuffer.data(), actualPeekedFrames,
                    pOut, actualFramesWanted,
                    ratio, ENGINE_CHANNELS, framesConsumed);

                pDev->ringBuffer->Advance(framesConsumed * ENGINE_CHANNELS);
            }

            // Fill remainder with absolute silence to avoid edge glitching
            if (framesWritten < framesWanted) {
                size_t startSample = framesWritten * ENGINE_CHANNELS;
                size_t endSample   = framesWanted  * ENGINE_CHANNELS;
                for (size_t i = startSample; i < endSample; ++i) pOut[i] = 0.0f;
            }

            float vol = pDev->volume.load();
            if (vol != 1.0f) {
                size_t total = framesWanted * ENGINE_CHANNELS;
                for (size_t i = 0; i < total; ++i) pOut[i] *= vol;
            }

            hr = pDev->pRenderClient->ReleaseBuffer(framesWanted, 0);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { invalidated = true; break; }
        }

        CleanupRenderDevice(pDev);
        pDev->deviceFailed = true;
        if (pDev->isRendering.load()) Sleep(500);
    }
    CleanupRenderDevice(pDev);
    CoUninitialize();
}
