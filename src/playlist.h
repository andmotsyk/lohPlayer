#pragma once
#include "common.h"
#include <map>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>

#define WM_APP_META_READY (WM_APP + 13)

struct Meta {
    wstr   title, artist, album;
    double duration = 0.0;
    int    track = 0;
    bool   resolved = false;
};

// Background metadata reader (Windows Property System - offline, no codecs opened).
class MetaCache {
public:
    void start(HWND notify);
    void stop();

    // Returns cached metadata; queues a background lookup on first miss.
    Meta get(const wstr& path);
    bool peek(const wstr& path, Meta& out);
    void clearQueue();

private:
    void worker();

    HWND hwnd = nullptr;
    std::thread th;
    std::atomic<bool> quit{ false };
    std::mutex mx;
    std::condition_variable cv;
    std::map<wstr, Meta> cache;
    std::deque<wstr> jobs;
};

struct Track {
    wstr   path;
    wstr   cachedLabel;
    double duration = 0.0;
    bool   labelled = false;
};

class Playlist {
public:
    std::vector<Track> items;
    int  current = -1;
    bool shuffle = false;
    int  repeat = 0;                     // 0 = off, 1 = all, 2 = one

    void addPath(const wstr& p);         // file or folder (recursive)
    void clear();
    void removeAt(int i);
    void removeSelection(const std::vector<int>& sorted);
    int  size() const { return (int)items.size(); }

    int  nextIndex(bool manual);
    int  prevIndex();
    void reshuffle();

    bool loadM3U(const wstr& file);
    bool saveM3U(const wstr& file) const;

    double totalDuration(MetaCache& mc) const;

private:
    std::vector<int> order;              // shuffle order
    int orderPos = -1;
};

void ScanFolder(const wstr& dir, std::vector<wstr>& out);
