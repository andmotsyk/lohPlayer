#include "audio.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <algorithm>
#include <chrono>

// Declared locally so we don't drag in ks.h / ksmedia.h.
static const GUID PA_SUBTYPE_PCM =
{ 0x00000001, 0x0000, 0x0010, { 0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71 } };
static const GUID PA_SUBTYPE_FLOAT =
{ 0x00000003, 0x0000, 0x0010, { 0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71 } };

static DWORD ChannelMask(int ch) {
    switch (ch) {
    case 1: return 0x4;          // FC
    case 2: return 0x3;          // FL|FR
    case 4: return 0x33;         // quad
    case 6: return 0x3F;         // 5.1
    case 8: return 0x63F;        // 7.1
    default: return 0;
    }
}

static void BuildWfx(WAVEFORMATEXTENSIBLE& w, int rate, int ch, int validBits, int containerBytes, bool isFloat) {
    ZeroMemory(&w, sizeof(w));
    w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w.Format.nChannels = (WORD)ch;
    w.Format.nSamplesPerSec = (DWORD)rate;
    w.Format.wBitsPerSample = (WORD)(containerBytes * 8);
    w.Format.nBlockAlign = (WORD)(ch * containerBytes);
    w.Format.nAvgBytesPerSec = (DWORD)rate * w.Format.nBlockAlign;
    w.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    w.Samples.wValidBitsPerSample = (WORD)validBits;
    w.dwChannelMask = ChannelMask(ch);
    w.SubFormat = isFloat ? PA_SUBTYPE_FLOAT : PA_SUBTYPE_PCM;
}

// Interleaved channel map / down-mix.
static void MapChannels(const float* src, int sc, float* dst, int dc, int frames) {
    if (sc == dc) { memcpy(dst, src, (size_t)frames * sc * sizeof(float)); return; }

    if (sc == 1) {
        for (int n = 0; n < frames; ++n) {
            float v = src[n];
            float* d = dst + (size_t)n * dc;
            d[0] = v;
            if (dc > 1) d[1] = v;
            for (int c = 2; c < dc; ++c) d[c] = 0.0f;
        }
        return;
    }
    if (dc == 1) {
        for (int n = 0; n < frames; ++n)
            dst[n] = 0.5f * (src[(size_t)n * sc] + src[(size_t)n * sc + 1]);
        return;
    }
    if (sc == 2) {                                   // stereo -> multichannel
        for (int n = 0; n < frames; ++n) {
            float* d = dst + (size_t)n * dc;
            d[0] = src[(size_t)n * 2];
            d[1] = src[(size_t)n * 2 + 1];
            for (int c = 2; c < dc; ++c) d[c] = 0.0f;
        }
        return;
    }
    if (sc >= 6 && dc == 2) {                        // 5.1 / 7.1 -> stereo (ITU-ish)
        const float k = 0.7071f, norm = 1.0f / (1.0f + 0.7071f);
        for (int n = 0; n < frames; ++n) {
            const float* s = src + (size_t)n * sc;
            float c = s[2], bl = s[4], br = s[5];
            float sl = sc >= 8 ? s[6] : 0.0f, sr = sc >= 8 ? s[7] : 0.0f;
            dst[(size_t)n * 2] = (s[0] + k * c + k * bl + k * sl) * norm;
            dst[(size_t)n * 2 + 1] = (s[1] + k * c + k * br + k * sr) * norm;
        }
        return;
    }
    // generic: copy what fits, zero the rest
    for (int n = 0; n < frames; ++n) {
        const float* s = src + (size_t)n * sc;
        float* d = dst + (size_t)n * dc;
        int m = std::min(sc, dc);
        for (int c = 0; c < m; ++c) d[c] = s[c];
        for (int c = m; c < dc; ++c) d[c] = 0.0f;
    }
}

// ---------------------------------------------------------------------------

bool Player::init() {
    if (!Decoder::globalInit()) return false;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&enumr);
    return SUCCEEDED(hr) && enumr;
}

void Player::shutdown() {
    unwatchDevices();
    stopThreads();
    closeDevice();
    decoder.close();
    if (enumr) { enumr->Release(); enumr = nullptr; }
    Decoder::globalShutdown();
}

void Player::closeDevice() {
    if (client) client->Stop();
    if (render) { render->Release(); render = nullptr; }
    if (client) { client->Release(); client = nullptr; }
    if (device) { device->Release(); device = nullptr; }
    if (devFmt) { CoTaskMemFree(devFmt); devFmt = nullptr; }
    if (hEvent) { CloseHandle(hEvent); hEvent = nullptr; }
    streamRunning.store(false);
    bufferFrames = 0;
}

// Endpoint add/remove and default-device changes: another app (or the driver's
// own control panel) reconfiguring the card lands here.
class DeviceNotify : public IMMNotificationClient {
public:
    explicit DeviceNotify(HWND h) : hwnd(h) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref);
        if (r == 0) delete this;
        return (ULONG)r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (!pp) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *pp = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) post();      // one role only, or we fire 3x
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { post(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { post(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { post(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
private:
    void post() { if (hwnd) PostMessageW(hwnd, WM_APP_DEV_CHANGED, 0, 0); }
    LONG ref = 1;
    HWND hwnd;
};

void Player::watchDevices(HWND notify) {
    unwatchDevices();
    if (!enumr) return;
    DeviceNotify* n = new DeviceNotify(notify);
    if (SUCCEEDED(enumr->RegisterEndpointNotificationCallback(n))) devNotify = n;
    else n->Release();
}

void Player::unwatchDevices() {
    if (!devNotify) return;
    IMMNotificationClient* n = (IMMNotificationClient*)devNotify;
    if (enumr) enumr->UnregisterEndpointNotificationCallback(n);
    n->Release();
    devNotify = nullptr;
}

std::vector<AudioDevice> Player::listDevices() const {
    std::vector<AudioDevice> out;
    if (!enumr) return out;

    wstr defId;
    IMMDevice* def = nullptr;
    if (SUCCEEDED(enumr->GetDefaultAudioEndpoint(eRender, eConsole, &def)) && def) {
        LPWSTR p = nullptr;
        if (SUCCEEDED(def->GetId(&p)) && p) { defId = p; CoTaskMemFree(p); }
        def->Release();
    }

    IMMDeviceCollection* coll = nullptr;
    if (FAILED(enumr->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll)) || !coll) return out;
    UINT n = 0;
    coll->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        IMMDevice* d = nullptr;
        if (FAILED(coll->Item(i, &d)) || !d) continue;
        AudioDevice a;
        LPWSTR p = nullptr;
        if (SUCCEEDED(d->GetId(&p)) && p) { a.id = p; CoTaskMemFree(p); }
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)) && ps) {
            PROPVARIANT v; PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
                a.name = v.pwszVal;
            PropVariantClear(&v);
            ps->Release();
        }
        if (a.name.empty()) a.name = L"Unknown device";
        a.isDefault = (!a.id.empty() && a.id == defId);
        out.push_back(a);
        d->Release();
    }
    coll->Release();
    return out;
}

bool Player::openDevice(int prefRate, int prefCh) {
    closeDevice();
    if (!enumr) return false;

    // Explicit choice first, falling back to the default if it is gone/disabled.
    if (!wantDeviceId.empty()) {
        if (SUCCEEDED(enumr->GetDevice(wantDeviceId.c_str(), &device)) && device) {
            DWORD state = 0;
            if (FAILED(device->GetState(&state)) || state != DEVICE_STATE_ACTIVE) {
                device->Release();
                device = nullptr;
            }
        } else {
            device = nullptr;
        }
    }
    if (!device) {
        if (FAILED(enumr->GetDefaultAudioEndpoint(eRender, eConsole, &device)) || !device) return false;
    }

    curDeviceId.clear();
    {
        LPWSTR p = nullptr;
        if (SUCCEEDED(device->GetId(&p)) && p) { curDeviceId = p; CoTaskMemFree(p); }
    }

    devName = L"Default output";
    IPropertyStore* props = nullptr;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
            devName = v.pwszVal;
        PropVariantClear(&v);
        props->Release();
    }

    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client)) || !client)
        return false;

    hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!hEvent) return false;

    isExclusive = false;

    // ---- exclusive (bit-perfect) attempt --------------------------------
    if (wantExclusive && prefRate > 0) {
        struct Cand { int bits, cont; bool flt; };
        const Cand cands[] = { {32,4,true},{32,4,false},{24,4,false},{24,3,false},{16,2,false} };
        const int chTry[2] = { prefCh > 0 ? prefCh : 2, 2 };

        WAVEFORMATEXTENSIBLE chosen; bool found = false;
        for (int ci = 0; ci < 2 && !found; ++ci) {
            if (ci == 1 && chTry[1] == chTry[0]) break;
            for (const Cand& c : cands) {
                WAVEFORMATEXTENSIBLE w;
                BuildWfx(w, prefRate, chTry[ci], c.bits, c.cont, c.flt);
                if (client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &w.Format, nullptr) == S_OK) {
                    chosen = w; found = true; break;
                }
            }
        }

        if (found) {
            REFERENCE_TIME defPer = 0, minPer = 0;
            client->GetDevicePeriod(&defPer, &minPer);
            REFERENCE_TIME per = defPer;

            HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK, per, per, &chosen.Format, nullptr);

            if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                UINT32 fr = 0;
                client->GetBufferSize(&fr);
                client->Release(); client = nullptr;
                if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client)) && client) {
                    per = (REFERENCE_TIME)(10000.0 * 1000.0 / chosen.Format.nSamplesPerSec * fr + 0.5);
                    hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK, per, per, &chosen.Format, nullptr);
                }
            }

            if (SUCCEEDED(hr)) {
                devFmt = (WAVEFORMATEX*)CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE));
                memcpy(devFmt, &chosen, sizeof(WAVEFORMATEXTENSIBLE));
                isExclusive = true;
            } else {
                // fall through to shared: the client may be half-initialised, rebuild it
                if (client) { client->Release(); client = nullptr; }
                if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client)) || !client)
                    return false;
            }
        }
    }

    // ---- shared mode -----------------------------------------------------
    if (!isExclusive) {
        if (FAILED(client->GetMixFormat(&devFmt)) || !devFmt) return false;
        HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, devFmt, nullptr);
        if (FAILED(hr)) return false;
    }

    // ---- interpret the negotiated format --------------------------------
    devRate = (int)devFmt->nSamplesPerSec;
    devCh = (int)devFmt->nChannels;
    outContainer = devFmt->wBitsPerSample / 8;
    if (devFmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && devFmt->cbSize >= 22) {
        WAVEFORMATEXTENSIBLE* ex = (WAVEFORMATEXTENSIBLE*)devFmt;
        outFloat = (ex->SubFormat == PA_SUBTYPE_FLOAT);
        outBits = ex->Samples.wValidBitsPerSample ? ex->Samples.wValidBitsPerSample : devFmt->wBitsPerSample;
    } else {
        outFloat = (devFmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
        outBits = devFmt->wBitsPerSample;
    }

    if (FAILED(client->SetEventHandle(hEvent))) return false;
    if (FAILED(client->GetBufferSize(&bufferFrames))) return false;
    if (FAILED(client->GetService(__uuidof(IAudioRenderClient), (void**)&render)) || !render) return false;

    return true;
}

// ---------------------------------------------------------------------------

bool Player::open(const wstr& path) {
    stopThreads();
    if (client) { client->Stop(); client->Reset(); }
    streamRunning.store(false);
    closeDevice();
    decoder.close();
    loaded = false;
    st = PlayState::Stopped;

    if (!decoder.open(path)) return false;
    srcFmt = decoder.format();
    trackDuration = srcFmt.duration;
    curPath = path;

    if (!openDevice(srcFmt.sampleRate, srcFmt.channels)) { decoder.close(); return false; }

    resampler.init(srcFmt.sampleRate, devRate, srcFmt.channels);
    equalizer.configure(devRate, devCh);

    size_t ringFrames = (size_t)(devRate * 0.40);
    if (ringFrames < (size_t)bufferFrames * 4) ringFrames = (size_t)bufferFrames * 4;
    ring.init(ringFrames, devCh);

    baseSec.store(0.0);
    framesOut.store(0);
    seekReq.store(0); seekDone.store(0);
    flushReq.store(0); flushAck.store(0);
    decodeEOF.store(false);
    endPosted.store(false);
    underrunCount.store(0);
    curGain = 0.0f;
    paused.store(false);

    loaded = true;
    startThreads();
    return true;
}

void Player::play() {
    if (!loaded || !client) return;
    paused.store(false);
    if (!streamRunning.load()) {
        HRESULT hr = client->Start();
        if (FAILED(hr) && hr != AUDCLNT_E_NOT_STOPPED) {
            // Device was reconfigured or taken away while we held it.
            if (notifyHwnd) PostMessageW(notifyHwnd, WM_APP_DEV_LOST, 0, 0);
            return;
        }
        streamRunning.store(true);
    }
    st = PlayState::Playing;
    cv.notify_all();
}

void Player::pause() {
    if (!loaded || st == PlayState::Stopped) return;
    paused.store(true);
    st = PlayState::Paused;
}

void Player::togglePause() {
    // Stopped must resume too, otherwise the play button is dead after Stop
    // or after the playlist runs out.
    if (st == PlayState::Playing) pause();
    else play();
}

void Player::stop() {
    if (!loaded) return;
    st = PlayState::Stopped;
    paused.store(true);
    Sleep(20);                                   // let the gain ramp reach zero
    if (client) { client->Stop(); client->Reset(); }
    streamRunning.store(false);
    curGain = 0.0f;
    seekTarget.store(0.0);
    seekReq.fetch_add(1);
    cv.notify_all();
}

void Player::seek(double seconds) {
    if (!loaded) return;
    seconds = Clampd(seconds, 0.0, trackDuration > 0 ? trackDuration : seconds);
    endPosted.store(false);
    seekTarget.store(seconds);
    seekReq.fetch_add(1);
    cv.notify_all();
}

void Player::nudge(double delta) { seek(position() + delta); }

double Player::position() const {
    if (!loaded) return 0.0;
    double p = baseSec.load() + (double)framesOut.load() / (double)(devRate > 0 ? devRate : 48000);
    if (trackDuration > 0 && p > trackDuration) p = trackDuration;
    return p;
}

void Player::setVolume(float v) { volLinear.store(Clampf(v, 0.0f, 1.0f)); }
void Player::setBalance(float b) { bal.store(Clampf(b, -1.0f, 1.0f)); }

void Player::setExclusive(bool on) {
    if (wantExclusive == on) return;
    wantExclusive = on;
    if (!loaded) return;
    double pos = position();
    bool wasPlaying = (st == PlayState::Playing);
    wstr p = curPath;
    if (open(p)) {
        if (pos > 0.5) seek(pos);
        if (wasPlaying) play();
    }
}

wstr Player::outputDesc() const {
    if (!loaded || !devFmt) return L"-";
    wchar_t buf[160];
    swprintf(buf, 160, L"%d Hz · %d-bit %s · %d ch · %s",
        devRate, outBits, outFloat ? L"float" : L"int", devCh,
        isExclusive ? L"exclusive" : L"shared");
    return buf;
}

// ---------------------------------------------------------------------------

void Player::startThreads() {
    quit.store(false);
    decodeThread = std::thread(&Player::decodeLoop, this);
    renderThread = std::thread(&Player::renderLoop, this);
}

void Player::stopThreads() {
    quit.store(true);
    cv.notify_all();
    if (hEvent) SetEvent(hEvent);
    if (decodeThread.joinable()) decodeThread.join();
    if (renderThread.joinable()) renderThread.join();
    quit.store(false);
}

void Player::decodeLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    const int BLK = 2048;
    const int sc = std::max(1, srcFmt.channels);
    std::vector<float> src((size_t)BLK * sc);
    std::vector<float> rs, mix;

    while (!quit.load()) {
        statDecIter.fetch_add(1, std::memory_order_relaxed);
        // ---- seek / flush handshake --------------------------------------
        uint32_t rq = seekReq.load();
        if (rq != seekDone.load()) {
            double t = seekTarget.load();
            decoder.seek(t);
            resampler.reset();
            equalizer.clearState();
            rs.clear();
            decodeEOF.store(false);
            flushBaseSec.store(t);

            if (streamRunning.load()) {
                uint32_t want = flushReq.fetch_add(1) + 1;
                for (int i = 0; i < 150 && flushAck.load() != want && !quit.load(); ++i) Sleep(1);
                if (flushAck.load() != want) {          // render stalled: do it ourselves
                    ring.dropAll();
                    baseSec.store(t);
                    framesOut.store(0);
                    flushAck.store(want);
                }
            } else {
                ring.hardReset();
                baseSec.store(t);
                framesOut.store(0);
                uint32_t want = flushReq.fetch_add(1) + 1;
                flushAck.store(want);
            }
            seekDone.store(rq);
            continue;
        }

        if (decodeEOF.load() || ring.writeAvail() < (size_t)BLK) {
            std::unique_lock<std::mutex> lk(mx);
            cv.wait_for(lk, std::chrono::milliseconds(30));
            continue;
        }

        statDecWork.fetch_add(1, std::memory_order_relaxed);

        LARGE_INTEGER qf, q0, q1, q2, q3;
        QueryPerformanceFrequency(&qf);
        QueryPerformanceCounter(&q0);

        int got = decoder.read(src.data(), BLK);
        if (got <= 0) { decodeEOF.store(true); continue; }
        QueryPerformanceCounter(&q1);

        rs.clear();
        resampler.process(src.data(), got, rs);
        int rf = (int)(rs.size() / sc);
        QueryPerformanceCounter(&q2);
        if (rf <= 0) continue;

        mix.resize((size_t)rf * devCh);
        MapChannels(rs.data(), sc, mix.data(), devCh, rf);
        equalizer.process(mix.data(), rf);
        QueryPerformanceCounter(&q3);

        const double toUs = 1e6 / (double)qf.QuadPart;
        statUsDecode.fetch_add((uint64_t)((q1.QuadPart - q0.QuadPart) * toUs), std::memory_order_relaxed);
        statUsResample.fetch_add((uint64_t)((q2.QuadPart - q1.QuadPart) * toUs), std::memory_order_relaxed);
        statUsMixEq.fetch_add((uint64_t)((q3.QuadPart - q2.QuadPart) * toUs), std::memory_order_relaxed);

        size_t done = 0;
        while (done < (size_t)rf && !quit.load()) {
            done += ring.write(mix.data() + done * devCh, (size_t)rf - done);
            if (done < (size_t)rf) {
                if (seekReq.load() != seekDone.load()) break;    // abandon: a seek arrived
                std::unique_lock<std::mutex> lk(mx);
                cv.wait_for(lk, std::chrono::milliseconds(30));
            }
        }
    }
    CoUninitialize();
}

void Player::renderLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIdx = 0;
    HANDLE hTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);

    // Any WASAPI failure, or the device going quiet while we think we are
    // playing, means another app or the driver reconfigured the endpoint.
    // Report it once and let the UI rebuild the stream.
    int silentTicks = 0;
    auto deviceLost = [&]() {
        if (notifyHwnd) PostMessageW(notifyHwnd, WM_APP_DEV_LOST, 0, 0);
        };

    while (!quit.load()) {
        DWORD w = WaitForSingleObject(hEvent, 200);
        if (quit.load()) break;

        if (w != WAIT_OBJECT_0) {
            if (w == WAIT_TIMEOUT && streamRunning.load() && !paused.load()) {
                if (++silentTicks >= 10) { deviceLost(); break; }   // ~2 s with no callback
            } else {
                silentTicks = 0;
            }
            continue;
        }
        silentTicks = 0;
        if (!streamRunning.load() || !client || !render) continue;

        UINT32 avail = bufferFrames;
        if (!isExclusive) {
            UINT32 pad = 0;
            HRESULT hp = client->GetCurrentPadding(&pad);
            if (FAILED(hp)) { deviceLost(); break; }
            if (pad >= bufferFrames) continue;
            avail = bufferFrames - pad;
        }
        if (avail == 0) continue;

        BYTE* data = nullptr;
        HRESULT hr = render->GetBuffer(avail, &data);
        if (FAILED(hr)) { deviceLost(); break; }
        if (!data) continue;

        statRender.fetch_add(1, std::memory_order_relaxed);
        LARGE_INTEGER qf, r0, r1;
        QueryPerformanceFrequency(&qf);
        QueryPerformanceCounter(&r0);
        fillDevice(data, avail);
        QueryPerformanceCounter(&r1);
        statUsRender.fetch_add((uint64_t)((r1.QuadPart - r0.QuadPart) * 1e6 / (double)qf.QuadPart),
            std::memory_order_relaxed);
        if (FAILED(render->ReleaseBuffer(avail, 0))) { deviceLost(); break; }
        // Only wake the decoder once there is a full block of room, instead of
        // on every callback - roughly a 4x cut in context switches.
        if (ring.writeAvail() >= 2048) cv.notify_one();
    }

    if (hTask) AvRevertMmThreadCharacteristics(hTask);
    CoUninitialize();
}

void Player::fillDevice(unsigned char* dst, unsigned frames) {
    // ---- honour a pending flush (seek) -----------------------------------
    uint32_t fr = flushReq.load();
    if (fr != flushAck.load()) {
        ring.dropAll();
        curGain = 0.0f;                          // silence, then ramp back in
        baseSec.store(flushBaseSec.load());
        framesOut.store(0);
        flushAck.store(fr);
    }

    const float target = paused.load() ? 0.0f : volLinear.load() * volLinear.load();

    if (target <= 0.0f && curGain <= 1e-5f) {
        curGain = 0.0f;
        memset(dst, 0, (size_t)frames * outContainer * devCh);
        return;
    }

    if (mixTmp.size() < (size_t)frames * devCh) mixTmp.resize((size_t)frames * devCh);
    float* tmp = mixTmp.data();

    size_t got = ring.read(tmp, frames);
    if (got < frames) {
        memset(tmp + got * devCh, 0, (frames - got) * devCh * sizeof(float));
        if (!decodeEOF.load() && !paused.load() && curGain > 0.0f)
            underrunCount.fetch_add(1);
    }
    if (got > 0) framesOut.fetch_add(got);

    const float b = bal.load();
    const float gl = b <= 0.0f ? 1.0f : 1.0f - b;
    const float gr = b >= 0.0f ? 1.0f : 1.0f + b;
    const float rampStep = 1.0f / (0.012f * (float)devRate);   // ~12 ms click-free ramp

    for (unsigned n = 0; n < frames; ++n) {
        if (curGain < target) { curGain += rampStep; if (curGain > target) curGain = target; }
        else if (curGain > target) { curGain -= rampStep; if (curGain < target) curGain = target; }
        float* f = tmp + (size_t)n * devCh;
        for (int c = 0; c < devCh; ++c)
            f[c] *= curGain * (c == 0 ? gl : (c == 1 ? gr : 1.0f));
    }

    pushVis(tmp, frames);
    convertOut(dst, tmp, frames);

    if (decodeEOF.load() && ring.readAvail() == 0 && !endPosted.exchange(true)) {
        if (notifyHwnd) PostMessageW(notifyHwnd, WM_APP_TRACK_END, 0, 0);
    }
}

void Player::convertOut(unsigned char* dst, const float* src, unsigned frames) {
    const size_t n = (size_t)frames * devCh;

    if (outFloat && outContainer == 4) { memcpy(dst, src, n * sizeof(float)); return; }

    if (outFloat && outContainer == 8) {
        double* d = (double*)dst;
        for (size_t i = 0; i < n; ++i) d[i] = (double)src[i];
        return;
    }

    if (outContainer == 4) {                                  // 24/32-bit int in a 32-bit container
        int32_t* d = (int32_t*)dst;
        for (size_t i = 0; i < n; ++i) {
            double v = (double)Clampf(src[i], -1.0f, 1.0f) * 2147483647.0;
            d[i] = (int32_t)(v < 0 ? v - 0.5 : v + 0.5);
        }
        return;
    }

    if (outContainer == 3) {                                  // packed 24-bit
        unsigned char* d = dst;
        for (size_t i = 0; i < n; ++i) {
            double v = (double)Clampf(src[i], -1.0f, 1.0f) * 8388607.0;
            int32_t s = (int32_t)(v < 0 ? v - 0.5 : v + 0.5);
            d[0] = (unsigned char)(s & 0xFF);
            d[1] = (unsigned char)((s >> 8) & 0xFF);
            d[2] = (unsigned char)((s >> 16) & 0xFF);
            d += 3;
        }
        return;
    }

    if (outContainer == 2) {                                  // 16-bit + TPDF dither
        int16_t* d = (int16_t*)dst;
        for (size_t i = 0; i < n; ++i) {
            rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
            float r1 = (float)(rngState >> 8) * (1.0f / 16777216.0f);
            rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
            float r2 = (float)(rngState >> 8) * (1.0f / 16777216.0f);
            float dither = (r1 - r2) * (1.0f / 32768.0f);      // 1 LSB peak-to-peak TPDF

            float v = Clampf(src[i] + dither, -1.0f, 1.0f) * 32767.0f;
            d[i] = (int16_t)(v < 0 ? v - 0.5f : v + 0.5f);
        }
        return;
    }

    memset(dst, 0, (size_t)frames * outContainer * devCh);
}

void Player::pushVis(const float* src, unsigned frames) {
    unsigned w = visWrite.load(std::memory_order_relaxed);
    const unsigned CAP = VIS_FFT * 2;
    for (unsigned n = 0; n < frames; ++n) {
        const float* f = src + (size_t)n * devCh;
        float m = devCh >= 2 ? 0.5f * (f[0] + f[1]) : f[0];
        visBuf[w % CAP] = m;
        ++w;
    }
    visWrite.store(w, std::memory_order_release);
}

void Player::getVis(float* dst, int count) {
    const unsigned CAP = VIS_FFT * 2;
    unsigned w = visWrite.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i) {
        unsigned idx = (w + CAP - (unsigned)(count - i)) % CAP;
        dst[i] = visBuf[idx];
    }
}
