#pragma once
#include "common.h"

struct Theme {
    COLORREF bg, panel, panelAlt, border;
    COLORREF text, textDim, textFaint;
    COLORREF accent, accentDim, ok;
    COLORREF track, fill, knob;
    COLORREF sel, selBar, hover;
    COLORREF visLo, visHi;
};

inline const Theme& ThemeFor(bool dark) {
    static const Theme D = {
        RGB(13,13,15), RGB(21,21,25), RGB(28,28,33), RGB(46,46,53),
        RGB(236,236,240), RGB(148,148,160), RGB(96,96,106),
        RGB(251,191,36), RGB(146,110,22), RGB(74,201,126),
        RGB(44,44,52), RGB(251,191,36), RGB(250,250,252),
        RGB(38,38,46), RGB(251,191,36), RGB(52,52,62),
        RGB(120,90,26), RGB(253,214,110)
    };
    static const Theme L = {
        RGB(245,245,247), RGB(255,255,255), RGB(238,238,242), RGB(206,206,214),
        RGB(24,24,27), RGB(106,106,118), RGB(150,150,162),
        RGB(202,110,4), RGB(240,196,130), RGB(22,146,86),
        RGB(222,222,228), RGB(202,110,4), RGB(255,255,255),
        RGB(224,230,241), RGB(202,110,4), RGB(233,233,239),
        RGB(240,206,150), RGB(202,110,4)
    };
    return dark ? D : L;
}
