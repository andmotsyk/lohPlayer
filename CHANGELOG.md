# Changelog

## 1.0.3 — 2026-07-26

### Renamed to lohPlayer
Everything the user or the system sees now says `lohPlayer`:

- executable is `build\lohPlayer.exe`
- window title, window class (`LohPlayerWnd`) and the error dialog caption
- settings file `lohPlayer.ini`, and the fallback folder `%APPDATA%\lohPlayer\`
- manifest assembly identity, and every string in the version resource
  (ProductName, InternalName, OriginalFilename, FileDescription, CompanyName)
- README and build script output

Existing `PlainAmp.ini` / `playlist.m3u8` files are not migrated — the new build
starts from defaults. Rename them by hand if you want the old session back.

## 1.0.2 — 2026-07-26

### Playlist now really survives a session
- The saved playlist is **always** restored on launch. Previously, starting the
  app with a file (double-clicking a track, or any command-line argument) skipped
  the restore entirely and then overwrote the saved playlist on exit. Files given
  on the command line are now **appended** to the restored playlist, and the first
  of them starts playing.
- The playlist is **autosaved 1.5 s after any change** (add, remove, clear) rather
  than only on a clean exit, so closing from Task Manager or a crash no longer
  loses it.
- **Resume point.** The track that was current and how far into it you were are
  stored (`[play] index` / `position`) and written every 15 s while playing as
  well as on exit. On the next launch that track is cued up at the same position
  — the seek bar and track info are already populated — but playback does **not**
  start on its own; press Play. Status bar says "Resumed playlist — press Play".

## 1.0.1 — 2026-07-26

### Fixed
- **Play button did nothing after Stop, or after the playlist reached the end.**
  `togglePause()` only handled Playing→Paused and Paused→Playing, so from the
  Stopped state both branches fell through and no-one called `play()`. Stopped
  now resumes.
- **Playback could stay silent after another app (a browser, the interface's own
  control panel) reconfigured the output device.** The render thread treated a
  failing `GetCurrentPadding` as "try again" and spun forever without audio, and
  only `GetBuffer` checked for `AUDCLNT_E_DEVICE_INVALIDATED`. Every WASAPI call
  in the loop is now checked, and a 2-second silence watchdog catches the case
  where the device simply stops delivering callbacks. Any of these rebuilds the
  stream and resumes at the same position.
- `client->Start()` failing no longer leaves the UI showing "playing" with no
  sound — it reports device loss and reconnects.

### Added
- **Output device selection.** New `Device` button lists every active render
  endpoint; pick one to pin it, or "Default device (follow Windows)". The choice
  is saved to the ini (`[audio] device=`) and survives restarts. Pinning a device
  that later disappears falls back to the default instead of going silent.
- Registered an `IMMNotificationClient`, so when the Windows default output
  changes or a device is plugged/unplugged, playback follows it automatically
  (debounced to one rebuild per burst).

## 1.0.0 — 2026-07-26

Initial version. Win32 + WASAPI + Media Foundation audio player, no third-party
libraries, no network code.

### Playback
- Media Foundation source reader decoding to 32-bit float at the file's native
  sample rate and channel count (MP3, FLAC, WAV, ALAC, AAC/M4A, WMA, AIFF).
- WASAPI shared-mode output, plus an exclusive-mode option that opens the device
  at the source rate/depth for a bit-perfect path.
- Polyphase Kaiser-windowed sinc resampler (32 taps × 512 phases, per-phase unity
  DC gain) used only when the device rate differs from the source. Verified at
  101–142 dB SNR, ≤0.081 dB gain error, and bit-exact when bypassed.
- TPDF dither for 16-bit output; direct write for 24-bit, 32-bit int and float.
- 5.1/7.1 → stereo ITU downmix, mono → stereo, and generic channel mapping.
- Lock-free SPSC ring buffer between the decode and render threads; render thread
  runs under MMCSS "Pro Audio".
- Glitch-free seeking: generation-counter flush handshake between the two threads
  plus an 8 ms cosine gain ramp, so scrubbing never clicks or plays stale audio.
- 10-band peaking-biquad equalizer with preamp, bypassed entirely when disabled.
- Underruns are counted and surfaced in the status bar instead of being hidden.

### Interface
- Fully custom-drawn Win32 UI, dark and light themes, DWM dark title bar.
- Per-monitor DPI aware; UI scales 0.75×–3× with Ctrl+wheel, persisted.
- Spectrum analyser (1024-point FFT, log-spaced bars, asymmetric smoothing).
- Playlist with background metadata reading via the Windows Property System,
  multi-select, drag & drop, recursive folder scan, m3u/m3u8 load and save.
- Media-key support, portable ini next to the exe, playlist restored on launch.

### Build
- `build.bat` — MSVC, `/MT` static CRT, `/GL /LTCG`, `/guard:cf`; single ~355 KB
  exe with no runtime dependency.
- The build **fails** if any network-capable DLL (WS2_32, WININET, WINHTTP,
  URLMON, DNSAPI, IPHLPAPI, WSOCK32) appears in the import table.
- `test.bat` — resampler SNR / gain / bypass checks.
- `bench.bat` — per-stage decode and resample cost.

### Fixed during bring-up
- `CW_USEDEFAULT` as the window x-position makes Windows ignore the requested
  width/height; the window is now sized explicitly via `AdjustWindowRectExForDpi`
  once the monitor DPI is known, and centred on the work area.
- Saved window positions on a monitor that no longer exists now fall back to
  centring instead of opening off-screen.
- 30 fps repaint redrew the whole window including the playlist; each section is
  now culled with `RectVisible`, and the timer drops to 4 fps when idle.
  Measured DSP cost is now under 1 % of one core.
- Render thread woke the decoder on every callback; it now only signals when a
  full block of ring space is free (~4× fewer context switches).
- Status bar left and right text could overlap on narrow windows.
- Sources are UTF-8 but were compiled as the ANSI codepage, so `·` rendered as
  `Â·`; `/utf-8` added to the compiler flags.
- `build.bat`: `%ProgramFiles(x86)%` expanded inside `if (` and `for /f (` blocks,
  whose `)` closed the block at parse time — switched to delayed expansion.
