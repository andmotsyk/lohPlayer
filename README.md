# lohPlayer

A small, fast, **completely offline** audio player for Windows. One 367 KB `.exe`,
no installer, no runtime, no DLLs to ship, no network code of any kind.

Built with nothing but the Win32 API, WASAPI and Media Foundation — the codecs
already in Windows. No Electron, no .NET, no bundled libraries.

---

## Build

Needs Visual Studio 2022 (or Build Tools) with **Desktop development with C++**.

```bash
build.bat
```

Output: `build\lohPlayer.exe`. That single file *is* the program — copy it anywhere.

```bash
test.bat
```

Runs the resampler quality checks (SNR, gain flatness, bit-exact bypass).

```bash
bench.bat "some\song.flac"
```

Reports what one second of audio costs in decode and resample time.

---

## Formats

MP3 · FLAC · WAV · ALAC · AAC / M4A · WMA · AIFF, and anything else a codec is
installed for. Decoding is done by Windows' own Media Foundation, so there are no
third-party decoders to trust, update, or get exploited.

## Audio quality

- Decoded to **32-bit float at the file's native sample rate** — nothing is
  converted behind your back.
- **Exclusive mode** (the `Exclusive` button) opens the device at the track's own
  rate and bit depth. When the rates match, samples reach the DAC untouched —
  no resampling, no Windows mixer, no volume curve applied twice.
- When shared mode forces a rate change, a **32-tap × 512-phase Kaiser-windowed
  sinc polyphase resampler** does the conversion. Measured 101–142 dB SNR with
  under 0.01 dB passband gain error — well below the noise floor of any 16- or
  24-bit source. `test.bat` reproduces these numbers.
- TPDF dither on 16-bit output; 24-bit and float outputs are written directly.
- 10-band equalizer (RBJ peaking biquads) with preamp, off by default and fully
  bypassed when off.

## Output device

The **Device** button lists every active output endpoint. Pick one to pin
playback to it, or leave it on *Default device* to follow whatever Windows is
using. The choice is remembered.

Shared mode is the default, so lohPlayer coexists with browsers and everything
else. If another program (or the interface's own control panel) reconfigures the
card, lohPlayer notices, rebuilds the stream and carries on from the same
position instead of going quiet. Plugging in headphones or changing the Windows
default output moves playback automatically.

## No glitches when seeking

Decode and audio-render run on separate threads joined by a lock-free ring buffer.
A seek is a three-step handshake — the decoder repositions, the render thread
drops every stale sample and acknowledges, then playback resumes behind an 8 ms
cosine fade. No clicks, no stutter, no stale audio, however fast you scrub.
The render thread runs under MMCSS "Pro Audio" scheduling.

If the machine ever *does* fail to keep up, the status bar says `glitches!`
rather than hiding it.

## Cost

Measured on a 30 s 44.1 kHz stereo file playing to a 48 kHz device
(`showStats=1` in the ini shows this live):

| stage | CPU |
|---|---|
| decode (Media Foundation) | 0.08 % of one core |
| resample 44.1 → 48 kHz | 0.81 % |
| channel map + EQ | 0.02 % |
| render / format convert | 0.05 % |

Under 1 % of a single core; **zero** for resampling when the rates already match.
Resident memory ~7 MB. The UI redraws only the region that changed, and drops
from 30 fps to 4 fps when nothing is moving.

---

## Offline guarantee

The build **fails** if a network-capable import is ever linked in:

```
findstr WS2_32 WSOCK32 WININET WINHTTP URLMON DNSAPI IPHLPAPI
```

The shipped binary imports exactly these, all local:

```
KERNEL32  USER32  GDI32  SHELL32  COMDLG32  ole32
PROPSYS   MFPlat  MFReadWrite  AVRT  dwmapi
```

No sockets, no HTTP, no telemetry, no update check, no accounts, no cookies, no
album-art lookup, no scrobbling. Settings live in `lohPlayer.ini` next to the exe
(or `%APPDATA%\lohPlayer\` if that folder is read-only). Nothing else is written.

## About SmartScreen

An unsigned executable downloaded from the internet gets a
"Windows protected your PC" prompt — that is reputation, not malware detection,
and it applies to every new program that isn't signed by an established
publisher. This build already does what it can:

- `asInvoker` in the manifest — it never asks for admin
- statically linked CRT, no packing, no obfuscation
- an embedded version resource with a real description
- `/guard:cf`, `/DYNAMICBASE`, `/NXCOMPAT`, `/HIGHENTROPYVA`
- no network imports at all

Since you built it yourself, it never crosses the internet, so Mark-of-the-Web is
never applied and SmartScreen will not prompt at all. The only way to remove the
prompt for *other* people is an Authenticode certificate:

```bash
signtool sign /fd SHA256 /a /tr http://timestamp.digicert.com /td SHA256 build\lohPlayer.exe
```

An OV certificate still needs to build reputation; an EV certificate is trusted
immediately. Nothing in the code can substitute for that.

---

## Controls

| | |
|---|---|
| Space | play / pause |
| ← → | seek ∓5 s (Ctrl: ∓30 s) |
| ↑ ↓ | volume |
| Z X C V B | prev, play, pause, stop, next |
| Enter | play the selected row |
| Delete | remove selected rows |
| S / R / T | shuffle, repeat, theme |
| Ctrl+O / Ctrl+L | add files / add folder |
| Ctrl+A | select all |
| Ctrl + mouse wheel | scale the whole UI (0.75×–3×) |
| Ctrl+0 | reset scale to 100 % |
| Media keys | play/pause, stop, next, prev — work while minimised |

Drag files or folders onto the window to add them; hold **Shift** while dropping
to replace the playlist. Folders are scanned recursively. Right-click a slider or
an EQ band to reset it. Click the total time to switch to time remaining.
Drag the window edge to resize; the playlist takes the extra space.

## Sessions

The playlist and all settings come back the next time you start the app, and so
does your place in the track you were listening to — cued and ready, but not
playing until you press Play.

It is autosaved 1.5 seconds after any change and every 15 seconds during
playback, so killing the app from Task Manager doesn't lose it either. Opening a
file from Explorer *adds* it to the playlist and plays it rather than wiping what
was there; use **Clear**, or hold **Shift** while dropping files, to start fresh.

## Files

```
src/main.cpp       window, custom-drawn UI, input
src/audio.*        WASAPI engine, threads, seek handshake, format conversion
src/decoder.*      Media Foundation source reader -> float32
src/resampler.*    polyphase sinc resampler
src/dsp.*          equalizer biquads + FFT for the analyser
src/playlist.*     playlist, m3u, background metadata reader
src/config.*       ini load/save
tests/             resampler quality check, decode benchmark
```

Public domain / MIT — do whatever you want with it.
