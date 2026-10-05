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
#include <cstring>

#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "ole32.lib")

// ============================================================================
// Constants
// ============================================================================
static const int ENGINE_SAMPLE_RATE = 48000;
static const int ENGINE_CHANNELS    = 2;

// Maximum WASAPI packet size we'll handle.  4096 frames at any sample rate
// is far above what any real driver delivers (typically 480-960 frames).
static const size_t MAX_WASAPI_FRAMES = 4096;

// Internal error codes (negative = failure, matching HRESULT convention)
static const int32_t E_NOT_INITIALIZED   = -1;
static const int32_t E_ALREADY_CAPTURING = -2;
static const int32_t E_EVENT_FAILED      = -3;
static const int32_t E_DEVICE_NOT_FOUND  = -4;

// ============================================================================
// Streaming Resampler — fixes packet-boundary bug
// ============================================================================
// The old DynamicResampler lost the final sample of each WASAPI packet,
// causing interpolation discontinuities across packet boundaries.
//
// This version maintains an internal history buffer that retains
// unconsumed samples (including the overlap needed for interpolation)
// between Process() calls.
//
class StreamingResampler {
    std::vector<float> history_;       // persistent input history
    size_t             history_len_;   // valid samples in history_
    size_t             channels_;
    float              fractional_pos_;

public:
    explicit StreamingResampler(size_t channels = ENGINE_CHANNELS,
                                size_t max_input_frames = MAX_WASAPI_FRAMES * 2)
        : channels_(channels)
        , history_len_(0)
        , fractional_pos_(0.0f)
    {
        // Pre-allocate for the largest conceivable input burst.
        history_.resize((max_input_frames + 2) * channels, 0.0f);
    }

    // Append new input, produce resampled output.
    // Returns the number of output frames actually written.
    size_t Process(const float* in, size_t in_frames,
                   float* out, size_t out_frames_cap,
                   float ratio)
    {
        const size_t in_samples = in_frames * channels_;

        // --- Append new input to history ---
        // Ensure capacity (should never resize after first call if MAX_WASAPI_FRAMES is correct)
        size_t needed = history_len_ + in_samples;
        if (needed > history_.size()) {
            history_.resize(needed);
        }
        std::memcpy(&history_[history_len_], in, in_samples * sizeof(float));
        history_len_ += in_samples;

        const size_t total_frames = history_len_ / channels_;

        // --- Resample from history ---
        size_t frames_out = 0;
        while (frames_out < out_frames_cap) {
            size_t idx0 = static_cast<size_t>(fractional_pos_);
            size_t idx1 = idx0 + 1;
            if (idx1 >= total_frames) break;

            float frac = fractional_pos_ - static_cast<float>(idx0);
            for (size_t c = 0; c < channels_; ++c) {
                float v0 = history_[idx0 * channels_ + c];
                float v1 = history_[idx1 * channels_ + c];
                out[frames_out * channels_ + c] = v0 + (v1 - v0) * frac;
            }

            fractional_pos_ += ratio;
            frames_out++;
        }

        // --- Compact history: keep unconsumed frames ---
        size_t frames_consumed = static_cast<size_t>(fractional_pos_);
        fractional_pos_ -= static_cast<float>(frames_consumed);

        size_t samples_consumed = frames_consumed * channels_;
        if (samples_consumed > 0 && samples_consumed < history_len_) {
            size_t remaining = history_len_ - samples_consumed;
            std::memmove(&history_[0], &history_[samples_consumed],
                         remaining * sizeof(float));
            history_len_ = remaining;
        } else if (samples_consumed >= history_len_) {
            history_len_ = 0;
        }

        return frames_out;
    }

    void Reset() {
        fractional_pos_ = 0.0f;
        history_len_ = 0;
    }
};

// ============================================================================
// Per-render-device resampler (no packet-boundary state needed — it reads
// from a ring buffer, not discrete packets)
// ============================================================================
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

// ============================================================================
// Output device state
// ============================================================================
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

// ============================================================================
// IMMNotificationClient — device hotplug support  (Issue 3A)
// ============================================================================
class DeviceNotificationClient : public IMMNotificationClient {
    LONG _refCount = 1;
    DeviceChangeCallback _callback = nullptr;

public:
    void SetCallback(DeviceChangeCallback cb) { _callback = cb; }

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&_refCount);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&_refCount);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    // IMMNotificationClient
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
        if (_callback) _callback();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override {
        if (_callback) _callback();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override {
        if (_callback) _callback();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override {
        if (_callback) _callback();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
        return S_OK;  // Not interesting for device enumeration
    }
};

// ============================================================================
// Globals
// ============================================================================
static std::vector<std::shared_ptr<OutputDevice>> g_devices;
static IMMDeviceEnumerator* g_pEnumerator = nullptr;
static std::mutex g_engineMutex;
static std::unique_ptr<RingBuffer> g_masterBuffer;
static HANDLE g_hRouterEvent = NULL;
static std::atomic<bool> g_isRouting{false};
static std::atomic<bool> g_isInitialized{false};

static IMMDevice*           g_pCaptureDevice       = nullptr;
static IAudioClient*        g_pCaptureClient        = nullptr;
static IAudioCaptureClient* g_pCaptureCaptureClient = nullptr;
static WAVEFORMATEX*        g_pCaptureMixFormat     = nullptr;
static HANDLE               g_hCaptureEvent         = NULL;
static std::thread          g_captureThread;
static std::atomic<bool>    g_isCapturing           { false };

static DeviceNotificationClient* g_pNotifyClient = nullptr;

// Forward declarations
static void CaptureThreadProc();
static void RenderThreadProc(std::shared_ptr<OutputDevice> pDev);
static bool TryInitRenderDevice(std::shared_ptr<OutputDevice> pDev);
static void CleanupRenderDevice(std::shared_ptr<OutputDevice> pDev);
static void CleanupCapture();

// ============================================================================
// Format helpers
// ============================================================================
static bool IsIEEEFloat(const WAVEFORMATEX* pwfx) {
    if (!pwfx) return false;
    if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE* pEx = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(pwfx);
        return (pEx->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }
    return false;
}

// Issue 2G fix: validate PCM subformat before treating as integer PCM
static bool IsIntegerPCM(const WAVEFORMATEX* pwfx) {
    if (!pwfx) return false;
    if (pwfx->wFormatTag == WAVE_FORMAT_PCM) return true;
    if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE* pEx = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(pwfx);
        return (pEx->SubFormat == KSDATAFORMAT_SUBTYPE_PCM);
    }
    return false;
}

// ============================================================================
// Format conversion — capture input → engine format (48kHz stereo float)
// ============================================================================
// Pre-allocated buffers for the capture thread.  Sized for MAX_WASAPI_FRAMES.
// These are allocated once in CaptureThreadProc() before the hot loop.
struct CaptureBuffers {
    std::vector<float> intermediate;  // raw-converted float samples
    std::vector<float> stereo;        // after channel conversion
    std::vector<float> resampled;     // after sample-rate conversion
    std::vector<float> silence;       // for silent packets

    void Allocate(size_t maxFrames, size_t maxChannels) {
        size_t maxSamples = maxFrames * maxChannels;
        intermediate.resize(maxSamples, 0.0f);
        stereo.resize(maxFrames * ENGINE_CHANNELS, 0.0f);
        // Resampled output can be up to 4x larger (e.g. 192kHz→48kHz is 0.25x,
        // but 8kHz→48kHz is 6x — use generous upper bound).
        resampled.resize(maxFrames * ENGINE_CHANNELS * 8, 0.0f);
        silence.resize(maxFrames * ENGINE_CHANNELS, 0.0f);
    }
};

static StreamingResampler g_captureResampler(ENGINE_CHANNELS, MAX_WASAPI_FRAMES * 2);

static void ConvertToEngineFormat(const BYTE* pData, const WAVEFORMATEX* pwfx,
                                   UINT32 numFrames,
                                   CaptureBuffers& bufs)
{
    const UINT32 numChannels = pwfx->nChannels;
    const UINT32 numSamples  = numFrames * numChannels;

    // 1. Convert to Float
    float* intermediate = bufs.intermediate.data();
    if (IsIEEEFloat(pwfx)) {
        const float* src = reinterpret_cast<const float*>(pData);
        std::memcpy(intermediate, src, numSamples * sizeof(float));
    } else if (IsIntegerPCM(pwfx)) {
        // Issue 2G: only convert when subformat is validated as PCM
        const WORD bitsPerSample = pwfx->wBitsPerSample;
        const WORD blockAlign    = pwfx->nBlockAlign;
        if (bitsPerSample == 16) {
            for (UINT32 i = 0; i < numSamples; ++i) {
                const int16_t s = *reinterpret_cast<const int16_t*>(
                    pData + (i / numChannels) * blockAlign + (i % numChannels) * sizeof(int16_t));
                intermediate[i] = s / 32768.0f;
            }
        } else if (bitsPerSample == 24) {
            for (UINT32 i = 0; i < numSamples; ++i) {
                size_t byteOff = (i / numChannels) * blockAlign + (i % numChannels) * 3;
                int32_t s = ((int32_t)pData[byteOff + 2] << 16) |
                            ((int32_t)pData[byteOff + 1] << 8)  |
                            (int32_t)pData[byteOff];
                if (s & 0x800000) s |= 0xFF000000;
                intermediate[i] = s / 8388608.0f;
            }
        } else if (bitsPerSample == 32) {
            for (UINT32 i = 0; i < numSamples; ++i) {
                const int32_t s = *reinterpret_cast<const int32_t*>(
                    pData + (i / numChannels) * blockAlign + (i % numChannels) * sizeof(int32_t));
                intermediate[i] = s / 2147483648.0f;
            }
        }
        // else: unsupported bit depth — intermediate stays zeroed, producing silence
    }
    // else: unsupported format — produce silence (intermediate is pre-zeroed)

    // 2. Mixdown / Upmix to ENGINE_CHANNELS (Stereo)
    float* stereoData = nullptr;
    if (numChannels == ENGINE_CHANNELS) {
        stereoData = intermediate;
    } else {
        stereoData = bufs.stereo.data();
        for (UINT32 i = 0; i < numFrames; ++i) {
            float left = 0.0f, right = 0.0f;
            if (numChannels == 1) { // Mono to Stereo
                left = intermediate[i];
                right = left;
            } else if (numChannels > 2) {
                // Issue 2H: Simplified downmix — FL + 0.7*FC, FR + 0.7*FC
                // NOTE: This ignores the WAVEFORMATEXTENSIBLE channel mask.
                // For a professional product, use proper channel-mask-aware downmixing.
                left  = intermediate[i * numChannels + 0] +
                        0.7f * intermediate[i * numChannels + 2];
                right = intermediate[i * numChannels + 1] +
                        0.7f * intermediate[i * numChannels + 2];
                if (left  >  1.0f) left  =  1.0f;
                if (left  < -1.0f) left  = -1.0f;
                if (right >  1.0f) right =  1.0f;
                if (right < -1.0f) right = -1.0f;
            }
            stereoData[i * ENGINE_CHANNELS + 0] = left;
            stereoData[i * ENGINE_CHANNELS + 1] = right;
        }
    }

    // 3. Resample to ENGINE_SAMPLE_RATE using streaming resampler
    if (pwfx->nSamplesPerSec == ENGINE_SAMPLE_RATE) {
        g_masterBuffer->Write(stereoData, numFrames * ENGINE_CHANNELS);
    } else {
        float ratio = (float)pwfx->nSamplesPerSec / (float)ENGINE_SAMPLE_RATE;
        size_t maxOutputFrames = (size_t)(numFrames / ratio) + 16;
        // Clamp to buffer size (pre-allocated)
        if (maxOutputFrames * ENGINE_CHANNELS > bufs.resampled.size()) {
            maxOutputFrames = bufs.resampled.size() / ENGINE_CHANNELS;
        }

        size_t framesWritten = g_captureResampler.Process(
            stereoData, numFrames,
            bufs.resampled.data(), maxOutputFrames,
            ratio);

        if (framesWritten > 0) {
            g_masterBuffer->Write(bufs.resampled.data(), framesWritten * ENGINE_CHANNELS);
        }
    }
}

// ============================================================================
// Convert engine float → render device format
// ============================================================================
// Issue 2B fix: do not blindly reinterpret render buffer as float.
// If the device format is integer PCM, convert from float.
static void WriteToRenderBuffer(BYTE* pRenderData, const float* engineData,
                                 size_t totalSamples, const WAVEFORMATEX* pFormat)
{
    if (IsIEEEFloat(pFormat)) {
        // Direct copy — render buffer is float
        std::memcpy(pRenderData, engineData, totalSamples * sizeof(float));
    } else if (IsIntegerPCM(pFormat)) {
        const WORD bps = pFormat->wBitsPerSample;
        if (bps == 16) {
            int16_t* dst = reinterpret_cast<int16_t*>(pRenderData);
            for (size_t i = 0; i < totalSamples; ++i) {
                float clamped = engineData[i];
                if (clamped >  1.0f) clamped =  1.0f;
                if (clamped < -1.0f) clamped = -1.0f;
                dst[i] = static_cast<int16_t>(clamped * 32767.0f);
            }
        } else if (bps == 24) {
            for (size_t i = 0; i < totalSamples; ++i) {
                float clamped = engineData[i];
                if (clamped >  1.0f) clamped =  1.0f;
                if (clamped < -1.0f) clamped = -1.0f;
                int32_t s = static_cast<int32_t>(clamped * 8388607.0f);
                pRenderData[i * 3 + 0] = (BYTE)(s & 0xFF);
                pRenderData[i * 3 + 1] = (BYTE)((s >> 8) & 0xFF);
                pRenderData[i * 3 + 2] = (BYTE)((s >> 16) & 0xFF);
            }
        } else if (bps == 32) {
            int32_t* dst = reinterpret_cast<int32_t*>(pRenderData);
            for (size_t i = 0; i < totalSamples; ++i) {
                float clamped = engineData[i];
                if (clamped >  1.0f) clamped =  1.0f;
                if (clamped < -1.0f) clamped = -1.0f;
                dst[i] = static_cast<int32_t>(clamped * 2147483647.0f);
            }
        } else {
            // Unsupported — fill with silence
            std::memset(pRenderData, 0, totalSamples * (bps / 8));
        }
    } else {
        // Unknown format — silence
        std::memset(pRenderData, 0,
                    totalSamples * (pFormat->wBitsPerSample / 8));
    }
}

// ============================================================================
// Helper: check if an HRESULT is a "device gone" error
// ============================================================================
static bool IsDeviceError(HRESULT hr) {
    return hr == AUDCLNT_E_DEVICE_INVALIDATED ||
           hr == AUDCLNT_E_RESOURCES_INVALIDATED ||
           hr == AUDCLNT_E_SERVICE_NOT_RUNNING;
}

// ============================================================================
// Engine initialization
// ============================================================================
static void CleanupGlobalState() {
    g_devices.clear();
    if (g_pNotifyClient && g_pEnumerator) {
        g_pEnumerator->UnregisterEndpointNotificationCallback(g_pNotifyClient);
    }
    if (g_pNotifyClient) { g_pNotifyClient->Release(); g_pNotifyClient = nullptr; }
    if (g_pEnumerator) { g_pEnumerator->Release(); g_pEnumerator = nullptr; }
    if (g_hRouterEvent) { CloseHandle(g_hRouterEvent); g_hRouterEvent = NULL; }
    g_masterBuffer.reset();
}

int32_t InitializeEngine() {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    // RPC_E_CHANGED_MODE is acceptable (already initialized with compatible mode)
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return hr;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&g_pEnumerator);
    if (FAILED(hr)) return hr;

    g_masterBuffer = std::make_unique<RingBuffer>(ENGINE_SAMPLE_RATE * ENGINE_CHANNELS * 2);
    g_hRouterEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_hRouterEvent) {
        CleanupGlobalState();
        return E_EVENT_FAILED;
    }

    // Register device notification client (Issue 3A)
    g_pNotifyClient = new DeviceNotificationClient();
    hr = g_pEnumerator->RegisterEndpointNotificationCallback(g_pNotifyClient);
    if (FAILED(hr)) {
        // Non-fatal: hotplug won't work but engine still functions
        g_pNotifyClient->Release();
        g_pNotifyClient = nullptr;
    }

    // Enumerate devices
    IMMDeviceCollection* pCollection = nullptr;
    hr = g_pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
    if (FAILED(hr)) {
        CleanupGlobalState();
        return hr;
    }

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

    g_isInitialized = true;
    return S_OK;
}

int32_t ShutdownEngine() {
    if (!g_isInitialized.load()) return S_OK;  // Issue 6C: safe if not initialized

    StopRouting();

    std::lock_guard<std::mutex> lock(g_engineMutex);
    g_isInitialized = false;
    CleanupGlobalState();
    CoUninitialize();
    return S_OK;
}

// ============================================================================
// Device queries
// ============================================================================
int32_t GetDeviceCount() {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    return (int32_t)g_devices.size();
}

int32_t GetDeviceName(int index, wchar_t* nameBuffer, int bufferSize) {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    if (index < 0 || index >= (int)g_devices.size()) return E_DEVICE_NOT_FOUND;
    wcsncpy_s(nameBuffer, bufferSize, g_devices[index]->name.c_str(), _TRUNCATE);
    return S_OK;
}

int32_t GetDeviceId(int index, wchar_t* idBuffer, int bufferSize) {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    if (index < 0 || index >= (int)g_devices.size()) return E_DEVICE_NOT_FOUND;
    wcsncpy_s(idBuffer, bufferSize, g_devices[index]->id.c_str(), _TRUNCATE);
    return S_OK;
}

// ============================================================================
// Device hotplug support
// ============================================================================
void SetDeviceChangeCallback(DeviceChangeCallback callback) {
    if (g_pNotifyClient) {
        g_pNotifyClient->SetCallback(callback);
    }
}

int32_t RefreshDevices() {
    if (!g_pEnumerator) return E_NOT_INITIALIZED;

    IMMDeviceCollection* pCollection = nullptr;
    HRESULT hr = g_pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
    if (FAILED(hr)) return hr;

    UINT count = 0;
    pCollection->GetCount(&count);

    std::vector<std::shared_ptr<OutputDevice>> newDevices;

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
            std::wstring deviceId = pwszID;

            // Check if this device already exists — preserve its state
            std::shared_ptr<OutputDevice> existing = nullptr;
            {
                std::lock_guard<std::mutex> lock(g_engineMutex);
                for (auto& d : g_devices) {
                    if (d->id == deviceId) { existing = d; break; }
                }
            }

            if (existing) {
                // Preserve existing device with all its state
                existing->name = varName.pwszVal;
                newDevices.push_back(existing);
            } else {
                auto dev = std::make_shared<OutputDevice>();
                dev->id   = deviceId;
                dev->name = varName.pwszVal;
                dev->ringBuffer = std::make_unique<RingBuffer>(
                    ENGINE_SAMPLE_RATE * ENGINE_CHANNELS * 2);
                newDevices.push_back(dev);
            }
            PropVariantClear(&varName);
        }
        pProps->Release();
        CoTaskMemFree(pwszID);
        pEndpoint->Release();
    }
    pCollection->Release();

    {
        std::lock_guard<std::mutex> lock(g_engineMutex);
        g_devices = std::move(newDevices);
    }

    return (int32_t)g_devices.size();
}

// ============================================================================
// Routing control
// ============================================================================
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

int32_t StartRouting() {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    if (!g_isInitialized.load()) return E_NOT_INITIALIZED;
    if (g_isCapturing.load()) return E_ALREADY_CAPTURING;

    g_masterBuffer->Clear();
    g_captureResampler.Reset();
    g_isRouting   = true;
    g_isCapturing = true;

    g_captureThread = std::thread(CaptureThreadProc);

    for (auto& dev : g_devices) {
        if (dev->enabled && !dev->isRendering) ActivateDevice(dev);
    }

    return S_OK;
}

int32_t StopRouting() {
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
    return S_OK;
}

int32_t SetOutputEnabled(const wchar_t* deviceId, bool enabled) {
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
    return S_OK;
}

int32_t SetOutputVolume(const wchar_t* deviceId, float volume) {
    for (auto& dev : g_devices) {
        if (dev->id == deviceId) { dev->volume = volume; return S_OK; }
    }
    return E_DEVICE_NOT_FOUND;
}

int32_t SetOutputDelay(const wchar_t* deviceId, int delayMs) {
    for (auto& dev : g_devices) {
        if (dev->id == deviceId) { dev->syncDelayMs = delayMs; return S_OK; }
    }
    return E_DEVICE_NOT_FOUND;
}

// ============================================================================
// Capture cleanup
// ============================================================================
static void CleanupCapture() {
    if (g_pCaptureCaptureClient) { g_pCaptureCaptureClient->Release(); g_pCaptureCaptureClient = nullptr; }
    if (g_pCaptureClient)        { g_pCaptureClient->Stop(); g_pCaptureClient->Release(); g_pCaptureClient = nullptr; }
    if (g_pCaptureDevice)        { g_pCaptureDevice->Release(); g_pCaptureDevice = nullptr; }
    if (g_pCaptureMixFormat)     { CoTaskMemFree(g_pCaptureMixFormat); g_pCaptureMixFormat = nullptr; }
    if (g_hCaptureEvent)         { CloseHandle(g_hCaptureEvent); g_hCaptureEvent = NULL; }
}

// ============================================================================
// Capture thread
// ============================================================================
static void CaptureThreadProc() {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return;  // Issue 6A

    // Issue 2F: Pre-allocate ALL buffers before the hot loop.
    // No resize() calls are permitted after this point.
    CaptureBuffers bufs;
    bufs.Allocate(MAX_WASAPI_FRAMES, 8);  // 8 channels max (7.1)

    // Pre-allocate routing buffer
    std::vector<float> routeBuffer(MAX_WASAPI_FRAMES * ENGINE_CHANNELS * 2, 0.0f);

    while (g_isCapturing.load()) {
        hr = g_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &g_pCaptureDevice);
        if (FAILED(hr)) { Sleep(1000); continue; }

        hr = g_pCaptureDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL,
                                         (void**)&g_pCaptureClient);
        if (FAILED(hr)) { CleanupCapture(); Sleep(1000); continue; }

        hr = g_pCaptureClient->GetMixFormat(&g_pCaptureMixFormat);
        if (FAILED(hr)) { CleanupCapture(); Sleep(1000); continue; }

        hr = g_pCaptureClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            0, 0, g_pCaptureMixFormat, NULL);
        if (FAILED(hr)) { CleanupCapture(); Sleep(1000); continue; }

        // Issue 2D: Check event creation
        g_hCaptureEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (!g_hCaptureEvent) { CleanupCapture(); Sleep(1000); continue; }

        hr = g_pCaptureClient->SetEventHandle(g_hCaptureEvent);
        if (FAILED(hr)) { CleanupCapture(); Sleep(1000); continue; }

        hr = g_pCaptureClient->GetService(__uuidof(IAudioCaptureClient),
                                           (void**)&g_pCaptureCaptureClient);
        if (FAILED(hr)) { CleanupCapture(); Sleep(1000); continue; }

        hr = g_pCaptureClient->Start();
        if (FAILED(hr)) { CleanupCapture(); Sleep(1000); continue; }

        bool deviceInvalidated = false;
        while (g_isCapturing.load() && !deviceInvalidated) {
            DWORD waitResult = WaitForSingleObject(g_hCaptureEvent, 100);
            if (waitResult != WAIT_OBJECT_0) continue;

            UINT32 packetLength = 0;
            hr = g_pCaptureCaptureClient->GetNextPacketSize(&packetLength);
            if (IsDeviceError(hr)) { deviceInvalidated = true; break; }
            if (FAILED(hr)) continue;

            while (packetLength != 0) {
                BYTE*  pData = nullptr;
                UINT32 numFramesAvailable = 0;
                DWORD  flags = 0;

                hr = g_pCaptureCaptureClient->GetBuffer(&pData, &numFramesAvailable,
                                                         &flags, NULL, NULL);
                if (IsDeviceError(hr)) { deviceInvalidated = true; break; }
                if (FAILED(hr)) break;

                // Clamp to our pre-allocated maximum
                if (numFramesAvailable > MAX_WASAPI_FRAMES) {
                    numFramesAvailable = MAX_WASAPI_FRAMES;
                }

                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    size_t count = numFramesAvailable * ENGINE_CHANNELS;
                    // Use pre-allocated silence buffer (already zeroed)
                    g_masterBuffer->Write(bufs.silence.data(), count);
                } else {
                    ConvertToEngineFormat(pData, g_pCaptureMixFormat,
                                          numFramesAvailable, bufs);
                }

                hr = g_pCaptureCaptureClient->ReleaseBuffer(numFramesAvailable);
                if (IsDeviceError(hr)) { deviceInvalidated = true; break; }
                if (FAILED(hr)) break;  // Issue 2E: don't ignore other failures

                hr = g_pCaptureCaptureClient->GetNextPacketSize(&packetLength);
                if (IsDeviceError(hr)) { deviceInvalidated = true; break; }
                if (FAILED(hr)) break;
            }

            // Inline Routing — distribute master buffer to all active devices
            size_t availableRead = g_masterBuffer->GetAvailableRead();
            if (availableRead > 0) {
                // Clamp to pre-allocated buffer
                if (availableRead > routeBuffer.size()) {
                    availableRead = routeBuffer.size();
                }

                size_t readCount = g_masterBuffer->Read(routeBuffer.data(), availableRead);
                if (readCount > 0) {
                    for (auto& dev : g_devices) {
                        if (dev->enabled.load() && dev->isRendering.load() &&
                            !dev->deviceFailed.load()) {
                            dev->ringBuffer->Write(routeBuffer.data(), readCount);
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

// ============================================================================
// Render device cleanup
// ============================================================================
static void CleanupRenderDevice(std::shared_ptr<OutputDevice> pDev) {
    if (pDev->pRenderClient) { pDev->pRenderClient->Release(); pDev->pRenderClient = nullptr; }
    if (pDev->pAudioClient)  {
        pDev->pAudioClient->Stop();
        pDev->pAudioClient->Release();
        pDev->pAudioClient = nullptr;
    }
    if (pDev->pDevice)       { pDev->pDevice->Release();       pDev->pDevice       = nullptr; }
    if (pDev->pDeviceFormat) { CoTaskMemFree(pDev->pDeviceFormat); pDev->pDeviceFormat = nullptr; }
    if (pDev->hEvent)        { CloseHandle(pDev->hEvent);      pDev->hEvent        = NULL; }
}

// ============================================================================
// Render device initialization
// ============================================================================
static bool TryInitRenderDevice(std::shared_ptr<OutputDevice> pDev) {
    CleanupRenderDevice(pDev);
    HRESULT hr = g_pEnumerator->GetDevice(pDev->id.c_str(), &pDev->pDevice);
    if (FAILED(hr)) return false;

    hr = pDev->pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL,
                                  (void**)&pDev->pAudioClient);
    if (FAILED(hr)) { CleanupRenderDevice(pDev); return false; }

    hr = pDev->pAudioClient->GetMixFormat(&pDev->pDeviceFormat);
    if (FAILED(hr)) { CleanupRenderDevice(pDev); return false; }

    // Issue 2B: Validate render format.  We support IEEE float and integer PCM.
    if (!IsIEEEFloat(pDev->pDeviceFormat) && !IsIntegerPCM(pDev->pDeviceFormat)) {
        // Unsupported format — cannot render to this device
        CleanupRenderDevice(pDev);
        return false;
    }

    hr = pDev->pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        0, 0, pDev->pDeviceFormat, NULL);
    if (FAILED(hr)) { CleanupRenderDevice(pDev); return false; }

    // Issue 2D: Check event creation
    pDev->hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!pDev->hEvent) { CleanupRenderDevice(pDev); return false; }

    hr = pDev->pAudioClient->SetEventHandle(pDev->hEvent);
    if (FAILED(hr)) { CleanupRenderDevice(pDev); return false; }

    hr = pDev->pAudioClient->GetService(__uuidof(IAudioRenderClient),
                                         (void**)&pDev->pRenderClient);
    if (FAILED(hr)) { CleanupRenderDevice(pDev); return false; }

    hr = pDev->pAudioClient->Start();
    if (FAILED(hr)) { CleanupRenderDevice(pDev); return false; }
    return true;
}

// ============================================================================
// Render thread
// ============================================================================
static void RenderThreadProc(std::shared_ptr<OutputDevice> pDev) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return;  // Issue 6A

    // Pre-allocate buffers before hot loop (Issue 2F)
    std::vector<float> inBuffer((ENGINE_SAMPLE_RATE / 5 + 2) * ENGINE_CHANNELS, 0.0f);
    std::vector<float> outBuffer;  // sized after GetBufferSize

    while (pDev->isRendering.load()) {
        if (!TryInitRenderDevice(pDev)) {
            pDev->deviceFailed = true;
            Sleep(2000);
            continue;
        }
        pDev->deviceFailed = false;

        // Issue 2C: Check GetBufferSize HRESULT
        UINT32 bufferFrameCount = 0;
        hr = pDev->pAudioClient->GetBufferSize(&bufferFrameCount);
        if (FAILED(hr)) {
            CleanupRenderDevice(pDev);
            pDev->deviceFailed = true;
            Sleep(2000);
            continue;
        }

        // Pre-allocate output conversion buffer for this device's format
        const UINT32 devChannels = pDev->pDeviceFormat->nChannels;
        outBuffer.resize(bufferFrameCount * devChannels, 0.0f);

        bool invalidated = false;
        while (pDev->isRendering.load() && !invalidated) {
            DWORD waitResult = WaitForSingleObject(pDev->hEvent, 100);
            if (waitResult != WAIT_OBJECT_0) continue;

            UINT32 padding = 0;
            hr = pDev->pAudioClient->GetCurrentPadding(&padding);
            if (IsDeviceError(hr)) { invalidated = true; break; }
            if (FAILED(hr)) continue;

            UINT32 framesWanted = bufferFrameCount - padding;
            if (framesWanted == 0) continue;

            BYTE* pData = nullptr;
            hr = pDev->pRenderClient->GetBuffer(framesWanted, &pData);
            if (IsDeviceError(hr)) { invalidated = true; break; }
            if (FAILED(hr)) continue;

            // -- Resampling from per-device ring buffer --
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

            size_t actualPeekedSamples = pDev->ringBuffer->Peek(inBuffer.data(), peekSamples);
            size_t actualPeekedFrames  = actualPeekedSamples / ENGINE_CHANNELS;

            size_t framesConsumed = 0;
            size_t framesWritten = 0;

            if (actualPeekedFrames > 1) {
                size_t actualFramesWanted = framesWanted;
                if (actualPeekedFrames < maxInputFrames) {
                    actualFramesWanted = (size_t)((actualPeekedFrames - 1) / ratio);
                }

                framesWritten = pDev->resampler.Process(
                    inBuffer.data(), actualPeekedFrames,
                    outBuffer.data(), actualFramesWanted,
                    ratio, ENGINE_CHANNELS, framesConsumed);

                pDev->ringBuffer->Advance(framesConsumed * ENGINE_CHANNELS);
            }

            // Fill remainder with silence
            if (framesWritten < framesWanted) {
                size_t startSample = framesWritten * ENGINE_CHANNELS;
                size_t endSample   = framesWanted  * ENGINE_CHANNELS;
                for (size_t i = startSample; i < endSample; ++i)
                    outBuffer[i] = 0.0f;
            }

            // Apply volume
            float vol = pDev->volume.load();
            if (vol != 1.0f) {
                size_t total = framesWanted * ENGINE_CHANNELS;
                for (size_t i = 0; i < total; ++i)
                    outBuffer[i] *= vol;
            }

            // Issue 2B: Convert engine float → render device format
            WriteToRenderBuffer(pData, outBuffer.data(),
                                framesWanted * devChannels,
                                pDev->pDeviceFormat);

            // Issue 2E: Handle ReleaseBuffer errors properly
            hr = pDev->pRenderClient->ReleaseBuffer(framesWanted, 0);
            if (IsDeviceError(hr)) { invalidated = true; break; }
            if (FAILED(hr)) { invalidated = true; break; }
        }

        CleanupRenderDevice(pDev);
        pDev->deviceFailed = true;
        if (pDev->isRendering.load()) Sleep(500);
    }
    CleanupRenderDevice(pDev);
    CoUninitialize();
}
