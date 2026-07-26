#pragma once
#include "common.h"
#include "decoder.h"
#include "resampler.h"
#include "dsp.h"
#include "ringbuffer.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>

struct IAudioClient;
struct IAudioRenderClient;
struct IMMDevice;
struct IMMDeviceEnumerator;
struct tWAVEFORMATEX;

enum class PlayState { Stopped, Playing, Paused };

// Posted to the owner window (wParam/lParam unused).
#define WM_APP_TRACK_END   (WM_APP + 11)
#define WM_APP_DEV_LOST    (WM_APP + 12)
#define WM_APP_DEV_CHANGED (WM_APP + 14)

struct AudioDevice {
    wstr id;
    wstr name;
    bool isDefault = false;
};

class Player {
public:
    bool init();
    void shutdown();

    bool open(const wstr& path);        // loads and prepares; does not start
    void play();
    void pause();
    void togglePause();
    void stop();
    void seek(double seconds);
    void nudge(double deltaSeconds);    // FF / RW

    PlayState state() const { return st; }
    double position() const;
    double duration() const { return trackDuration; }
    const TrackFormat& format() const { return srcFmt; }
    bool   hasTrack() const { return loaded; }

    void  setVolume(float v01);
    float volume() const { return volLinear; }
    void  setBalance(float b);          // -1 = left, +1 = right
    float balance() const { return bal; }

    Equalizer& eq() { return equalizer; }
    void eqChanged() { eqDirty.store(true); }

    void setExclusive(bool on);         // applied on next open()
    bool exclusive() const { return wantExclusive; }
    bool exclusiveActive() const { return isExclusive; }

    wstr deviceName() const { return devName; }
    wstr outputDesc() const;

    // Output device selection. An empty id means "follow the Windows default".
    std::vector<AudioDevice> listDevices() const;
    void setDeviceId(const wstr& id) { wantDeviceId = id; }
    const wstr& deviceId() const { return wantDeviceId; }
    const wstr& activeDeviceId() const { return curDeviceId; }
    bool  followsDefault() const { return wantDeviceId.empty(); }
    void  watchDevices(HWND notify);          // default-device / hotplug notifications
    void  unwatchDevices();
    int  underruns() const { return underrunCount.load(); }

    void getVis(float* dst, int count);

    // Diagnostics: counters and per-stage microseconds since the last call.
    struct Stats {
        unsigned decodeIters, decodeWorks, renderCbs;
        double usDecode, usResample, usMixEq, usRender;
    };
    Stats takeStats() {
        Stats s;
        s.decodeIters = statDecIter.exchange(0);
        s.decodeWorks = statDecWork.exchange(0);
        s.renderCbs = statRender.exchange(0);
        s.usDecode = (double)statUsDecode.exchange(0);
        s.usResample = (double)statUsResample.exchange(0);
        s.usMixEq = (double)statUsMixEq.exchange(0);
        s.usRender = (double)statUsRender.exchange(0);
        return s;
    }

    HWND notifyHwnd = nullptr;

private:
    bool  openDevice(int preferredRate, int preferredChannels);
    void  closeDevice();
    void  startThreads();
    void  stopThreads();
    void  decodeLoop();
    void  renderLoop();
    void  fillDevice(unsigned char* dst, unsigned frames);
    void  convertOut(unsigned char* dst, const float* src, unsigned frames);
    void  pushVis(const float* src, unsigned frames);

    // ---- COM / WASAPI ----
    IMMDeviceEnumerator* enumr = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    tWAVEFORMATEX* devFmt = nullptr;
    HANDLE  hEvent = nullptr;
    unsigned bufferFrames = 0;
    wstr devName;
    wstr wantDeviceId;                        // "" = follow default
    wstr curDeviceId;                         // what we actually opened
    void* devNotify = nullptr;                // IMMNotificationClient*

    int  devRate = 48000, devCh = 2;
    int  outBits = 32, outContainer = 4;
    bool outFloat = true;
    bool isExclusive = false;
    bool wantExclusive = false;

    // ---- pipeline ----
    Decoder     decoder;
    Resampler   resampler;
    Equalizer   equalizer;
    RingBuffer  ring;
    TrackFormat srcFmt;
    double      trackDuration = 0.0;
    bool        loaded = false;
    wstr        curPath;
    std::vector<float> mixTmp;      // render-thread scratch (render thread only)
    uint32_t    rngState = 0x9E3779B9u;

    std::thread decodeThread, renderThread;
    std::atomic<bool> quit{ false };
    std::atomic<bool> decodeEOF{ false };
    std::atomic<bool> streamRunning{ false };
    std::atomic<bool> endPosted{ false };
    std::atomic<bool> eqDirty{ false };

    std::mutex              mx;
    std::condition_variable cv;

    // seek handshake
    std::atomic<uint32_t> seekReq{ 0 }, seekDone{ 0 };
    std::atomic<uint32_t> flushReq{ 0 }, flushAck{ 0 };
    std::atomic<double>   seekTarget{ 0.0 };
    std::atomic<double>   flushBaseSec{ 0.0 };

    // position
    std::atomic<double>   baseSec{ 0.0 };
    std::atomic<uint64_t> framesOut{ 0 };

    // gain (render thread)
    PlayState st = PlayState::Stopped;
    std::atomic<float> volLinear{ 0.7f };
    std::atomic<float> bal{ 0.0f };
    std::atomic<bool>  paused{ false };
    float curGain = 0.0f;
    float ditherState[2] = { 0, 0 };

    std::atomic<int> underrunCount{ 0 };
    std::atomic<unsigned> statDecIter{ 0 }, statDecWork{ 0 }, statRender{ 0 };
    std::atomic<uint64_t> statUsDecode{ 0 }, statUsResample{ 0 }, statUsMixEq{ 0 }, statUsRender{ 0 };

    // visualiser tap
    float visBuf[VIS_FFT * 2] = { 0 };
    std::atomic<unsigned> visWrite{ 0 };
};
