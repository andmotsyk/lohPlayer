#pragma once
#include "common.h"
#include "dsp.h"

struct Config {
    bool  dark = true;
    float scale = 1.0f;              // UI scale, 0.75 .. 3.0
    float volume = 0.70f;
    float balance = 0.0f;
    bool  shuffle = false;
    int   repeat = 0;
    bool  exclusive = false;
    bool  eqOn = false;
    float preamp = 0.0f;
    float eqGain[EQ_BANDS] = { 0 };
    bool  showEq = false;
    bool  showVis = true;
    bool  mediaKeys = true;
    bool  elapsed = true;            // false = show remaining
    bool  showStats = false;         // ini-only: per-stage DSP cost in the status bar
    wstr  deviceId;                  // "" = follow the Windows default output
    int   resumeIndex = -1;          // playlist row that was current last session
    double resumePos = 0.0;          // and how far into it
    int   winX = CW_USEDEFAULT, winY = CW_USEDEFAULT, winW = 0, winH = 0;
    bool  maximized = false;

    void load();
    void save() const;

    static const wstr& dataDir();          // ends with '\'
    static wstr iniPath();
    static wstr playlistPath();
};
