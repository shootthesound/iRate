// iRate - blazing fast fullscreen Sony RAW culling/rating tool.
// Native Win32. Shows the embedded JPEG preview (no raw processing).
// Writes Lightroom-compatible XMP sidecars.
//
// Build: zig c++ -target x86_64-windows-gnu -O2 irate.cpp -o iRate.exe ...
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <initguid.h>
#include <wincodec.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <cstdarg>
#include <string>
#include <vector>
#include <map>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <algorithm>
#include "core.h"

#pragma comment(lib, "windowscodecs.lib")

// mingw headers lack IWICBitmapSourceTransform (JPEG decode-time downscale).
// Declaration matches the Windows SDK wincodec.h exactly.
#ifndef __IWICBitmapSourceTransform_INTERFACE_DEFINED__
#define __IWICBitmapSourceTransform_INTERFACE_DEFINED__
DEFINE_GUID(IID_IWICBitmapSourceTransform, 0x3B16811B, 0x6A43, 0x4ec9,
            0xB7, 0x13, 0x3D, 0x5A, 0x0C, 0x13, 0xB9, 0x40);
struct IWICBitmapSourceTransform : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CopyPixels(const WICRect* prc, UINT uiWidth, UINT uiHeight,
        WICPixelFormatGUID* pguidDstFormat, WICBitmapTransformOptions dstTransform,
        UINT nStride, UINT cbBufferSize, BYTE* pbPixels) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetClosestSize(UINT* puiWidth, UINT* puiHeight) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetClosestPixelFormat(WICPixelFormatGUID* pguidDstFormat) = 0;
    virtual HRESULT STDMETHODCALLTYPE DoesSupportTransform(WICBitmapTransformOptions dstTransform,
        BOOL* pfIsSupported) = 0;
};
#endif

// ------------------------------------------------------------------ messages
#define WM_APP_DECODED  (WM_APP + 1)
#define WM_APP_SCANDONE (WM_APP + 2)

// ------------------------------------------------------------------ actions
enum Action {
    A_NONE = 0, A_NEXT, A_PREV, A_FIRST, A_LAST,
    A_RATE0, A_RATE1, A_RATE2, A_RATE3, A_RATE4, A_RATE5,
    A_LABEL_RED, A_LABEL_YELLOW, A_LABEL_GREEN, A_LABEL_BLUE, A_LABEL_PURPLE,
    A_TOGGLE_INFO, A_QUIT,
    A_ZOOM, A_SORTNEXT, A_SORTPREV, A_SORTDIR, A_FILTER, A_GRID, A_INFOPOS, A_HELP, A_PREFS,
    A_REJECT
};

// ------------------------------------------------------------------ items
struct Item {
    std::wstring path;
    uint64_t mtime = 0;
    uint64_t fsize = 0;
    int rating = -2;   // -2 = sidecar not read yet, -1 = rejected, 0..5 stars
    int label = -1;
    bool jpgTwin = false;   // paired RAW+JPG
};

struct Slot {
    HBITMAP bmp = nullptr;
    int w = 0, h = 0;
    RawExif exif;
    bool failed = false;
    std::wstring err;
    DWORD tick = 0;       // last access (thumb LRU)
};

// ------------------------------------------------------------------ globals
static HWND g_hwnd;
static int g_scrW = 1920, g_scrH = 1080;
static int g_dpi = 96;
static std::vector<Item> g_items;
static std::mutex g_itemsMx;               // guards g_items during scan only
static std::atomic<bool> g_scanning{true};
static int g_cur = 0;
static bool g_showInfo = true;
static bool g_infoTop = false;
static bool g_helpOpen = false;
static bool g_prefsOpen = false;
static int g_prefsCapture = -1;            // Action being rebound, -1 = none
static int g_advanceMode = 0;              // 0=capslock 1=always 2=never
static int g_lastDir = 1;
static std::wstring g_folder;
static std::vector<std::wstring> g_exts = { L".arw" };
static std::map<UINT, Action> g_keymap;
static std::wstring g_iniPath;
static bool g_sessionInFolder = true;
static bool g_slowDrive = false;           // browsed folder is on a seek-bound drive
static bool g_pairRawJpg = true;           // collapse RAW+JPG pairs to one entry      // per-folder resume file in the dataset root
static int g_sortType = 0;                 // 0=name 1=date 2=size
static bool g_sortDesc = false;
static std::atomic<int> g_generation{0};
// zoom state
static bool g_zoomWanted = false;
static int  g_zoomIdx = -1;
static HBITMAP g_zoomBmp = nullptr;
static int  g_zoomW = 0, g_zoomH = 0;
static std::mutex g_zoomMx;
static int  g_mouseX = 0, g_mouseY = 0;
static int g_lastPaintedItem = -1;
static const int ZOOM_BASE = 1 << 28;
static const int THUMB_BASE = 1 << 27;
#define WM_APP_ZOOMED   (WM_APP + 3)
#define WM_APP_THUMB    (WM_APP + 4)
#define WM_APP_SIDECARS (WM_APP + 5)
#define WM_APP_SIDECAR1 (WM_APP + 6)   // one item's sidecar loaded/patched async

// filtered view: view positions -> item indices; g_cur is a VIEW position
static std::vector<int> g_view;
struct Filter {
    unsigned ratingMask = 0x7F;   // bits 0..5 = rating values, bit 6 = rejected
    unsigned labelMask  = 0x3F;   // bits 0..5 = none/red/yellow/green/blue/purple
    std::wstring text;            // path substring, case-insensitive
    bool active() const { return ratingMask != 0x7F || labelMask != 0x3F || !text.empty(); }
};
static Filter g_filter;
static bool g_filterOpen = false;
static HWND g_filterEdit = nullptr;
static WNDPROC g_editOldProc = nullptr;
static bool g_sidecarsLoaded = false;
static std::atomic<bool> g_sidecarLoading{false};

// grid view
static bool g_gridMode = false;
static int g_gridScroll = 0;               // first visible row
static int g_gridCols = 1, g_gridRowsVis = 1;
static int g_cellW = 230, g_cellH = 200, g_gridX0 = 0, g_gridTbH = 46;
static int g_thumbBoxW = 214, g_thumbBoxH = 150;

// decode cache + worker
static std::map<int, Slot> g_cache;
static std::map<int, Slot> g_thumbCache;
static std::mutex g_thumbMx;
static std::mutex g_cacheMx;
static std::deque<int> g_queue;      // high priority: view + zoom decodes
static std::deque<int> g_lowQueue;   // low priority: thumbnails
static std::set<int> g_lowSet;       // dedupe for bulk preload
static std::set<int> g_inFlight;     // keys being decoded right now (guarded by g_queueMx)
static std::mutex g_queueMx;
static std::condition_variable g_queueCv;
static std::atomic<bool> g_quit{false};
static const int CACHE_RADIUS = 4;

// ------------------------------------------------------------------ helpers
static std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
// ---- debug log: ini [options] debuglog=1 -> %LOCALAPPDATA%\iRate\irate.log.
// No-op (one branch) when disabled; never enabled by default.
static HANDLE g_logFile = INVALID_HANDLE_VALUE;
static std::mutex g_logMx;
static ULONGLONG g_logT0 = 0;
static void logLine(const char* fmt, ...) {
    if (g_logFile == INVALID_HANDLE_VALUE) return;
    char buf[600];
    ULONGLONG t = GetTickCount64() - g_logT0;
    int n = snprintf(buf, 40, "%6llu.%03llu [%5lu] ",
                     t / 1000, t % 1000, GetCurrentThreadId());
    va_list ap; va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
    va_end(ap);
    if (m > 0) n += m;
    buf[n++] = '\r'; buf[n++] = '\n';
    std::lock_guard<std::mutex> lk(g_logMx);
    DWORD wr;
    WriteFile(g_logFile, buf, (DWORD)n, &wr, nullptr);
}
static const wchar_t* pathTail(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return p.c_str() + (s == std::wstring::npos ? 0 : s + 1);
}

static bool readWholeFile(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    if (sz.QuadPart > 32 * 1024 * 1024) { CloseHandle(h); return false; }
    out.resize((size_t)sz.QuadPart);
    DWORD rd = 0;
    BOOL ok = sz.QuadPart == 0 || ReadFile(h, &out[0], (DWORD)sz.QuadPart, &rd, nullptr);
    CloseHandle(h);
    if (!ok || rd != sz.QuadPart) { out.clear(); return false; }
    return true;
}
static bool writeWholeFileAtomic(const std::wstring& path, const std::string& data) {
    std::wstring tmp = path + L".jrtmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &wr, nullptr);
    CloseHandle(h);
    if (!ok || wr != data.size()) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
static std::wstring sidecarPath(const std::wstring& raw) {
    size_t dot = raw.find_last_of(L'.');
    size_t slash = raw.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return raw + L".xmp";
    return raw.substr(0, dot) + L".xmp";
}

// ------------------------------------------------------------------ sidecars
static void loadSidecar(Item& it) {
    if (it.rating != -2) return;
    std::string content;
    int r = 0, l = 0;
    if (readWholeFile(sidecarPath(it.path), content)) xmpParse(content, r, l);
    it.rating = r; it.label = l;
}
// async sidecar I/O: reads and writes happen on a dedicated thread so the UI
// thread never touches the (possibly slow) photo drive. Writes are FIFO and
// flushed before exit. rating/label of kXmpKeep = field not yet known here;
// xmpApply then leaves whatever the sidecar already says untouched.
struct ScWrite { int idx; std::wstring path; int rating; int label; };
static std::deque<int> g_scReadQ;
static std::set<int> g_scReadSet;
static std::deque<ScWrite> g_scWriteQ;
static std::mutex g_scMx;
static std::condition_variable g_scCv;

static void requestSidecar(int idx) {
    {
        std::lock_guard<std::mutex> lk(g_scMx);
        if (g_scReadSet.count(idx)) return;
        g_scReadQ.push_back(idx);
        g_scReadSet.insert(idx);
    }
    g_scCv.notify_one();
}
static void queueSidecarSave(int idx, const std::wstring& path, int rating, int label) {
    {
        std::lock_guard<std::mutex> lk(g_scMx);
        g_scWriteQ.push_back({ idx, path, rating, label });
    }
    g_scCv.notify_one();
}
static void sidecarThread() {
    for (;;) {
        ScWrite wr; int rdIdx = -1; bool haveWr = false;
        {
            std::unique_lock<std::mutex> lk(g_scMx);
            g_scCv.wait(lk, [] { return g_quit.load() || !g_scWriteQ.empty() || !g_scReadQ.empty(); });
            if (g_quit && g_scWriteQ.empty()) break;      // flush writes, drop reads
            if (!g_scWriteQ.empty()) {                    // writes always drain first
                wr = std::move(g_scWriteQ.front()); g_scWriteQ.pop_front(); haveWr = true;
            } else {
                rdIdx = g_scReadQ.front(); g_scReadQ.pop_front(); g_scReadSet.erase(rdIdx);
            }
        }
        if (haveWr) {
            ULONGLONG t0 = GetTickCount64();
            std::wstring sp = sidecarPath(wr.path);
            std::string content;
            readWholeFile(sp, content);
            std::string out = xmpApply(content, wr.rating, wr.label);
            writeWholeFileAtomic(sp, out);
            logLine("xmp write %llums  %ls", (unsigned long long)(GetTickCount64() - t0), pathTail(sp));
            if (wr.rating == kXmpKeep || wr.label == kXmpKeep) {
                // rated before the sidecar loaded: adopt what the patched
                // sidecar actually says for the still-unknown field
                int r = 0, l = 0; xmpParse(out, r, l);
                {
                    std::lock_guard<std::mutex> lk(g_itemsMx);
                    if (wr.idx >= 0 && wr.idx < (int)g_items.size()) {
                        if (wr.rating == kXmpKeep && g_items[wr.idx].rating == -2) g_items[wr.idx].rating = r;
                        if (wr.label == kXmpKeep && g_items[wr.idx].label < 0) g_items[wr.idx].label = l;
                    }
                }
                PostMessageW(g_hwnd, WM_APP_SIDECAR1, (WPARAM)wr.idx, 0);
            }
            continue;
        }
        std::wstring path;
        {
            std::lock_guard<std::mutex> lk(g_itemsMx);
            if (rdIdx < 0 || rdIdx >= (int)g_items.size() || g_items[rdIdx].rating != -2) continue;
            path = g_items[rdIdx].path;
        }
        ULONGLONG t0 = GetTickCount64();
        std::string content; int r = 0, l = 0;
        if (readWholeFile(sidecarPath(path), content)) xmpParse(content, r, l);
        {
            std::lock_guard<std::mutex> lk(g_itemsMx);
            if (rdIdx < (int)g_items.size() && g_items[rdIdx].rating == -2) {
                g_items[rdIdx].rating = r; g_items[rdIdx].label = l;
            }
        }
        ULONGLONG ms = GetTickCount64() - t0;
        if (ms > 50) logLine("SLOW xmp read item=%d %llums", rdIdx, (unsigned long long)ms);
        PostMessageW(g_hwnd, WM_APP_SIDECAR1, (WPARAM)rdIdx, 0);
    }
}

static std::wstring sessionFile() { return g_folder + L"\\irate.session"; }
static std::wstring sesGet(const wchar_t* key, const wchar_t* def) {
    wchar_t buf[512];
    GetPrivateProfileStringW(L"session", key, def, buf, 512, sessionFile().c_str());
    return buf;
}
static void sesSet(const wchar_t* key, const std::wstring& val) {
    WritePrivateProfileStringW(L"session", key, val.c_str(), sessionFile().c_str());
}

// ------------------------------------------------------------------ view
static int curItem() { return (g_cur >= 0 && g_cur < (int)g_view.size()) ? g_view[g_cur] : -1; }
static int viewItemAt(int pos) { return (pos >= 0 && pos < (int)g_view.size()) ? g_view[pos] : -1; }
static std::wstring lowerW(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }

static bool passesFilter(const Item& it) {
    if (!g_filter.active()) return true;
    int r = it.rating;
    int rb = (r == -1) ? 6 : (r < 0 ? 0 : r);
    int l = it.label < 0 ? 0 : it.label;
    if (!(g_filter.ratingMask & (1u << rb))) return false;
    if (!(g_filter.labelMask & (1u << l))) return false;
    if (!g_filter.text.empty() &&
        lowerW(it.path).find(lowerW(g_filter.text)) == std::wstring::npos) return false;
    return true;
}

static void rebuildView(bool keepCurrent) {
    int curIt = curItem();
    g_view.clear();
    g_view.reserve(g_items.size());
    for (int i = 0; i < (int)g_items.size(); i++)
        if (passesFilter(g_items[i])) g_view.push_back(i);
    int pos = 0;
    if (keepCurrent && curIt >= 0) {
        int best = 0, bestDist = 0x7FFFFFFF;
        for (int q = 0; q < (int)g_view.size(); q++) {
            int d = std::abs(g_view[q] - curIt);
            if (d < bestDist) { bestDist = d; best = q; }
            if (d == 0) break;
        }
        pos = best;
    }
    g_cur = g_view.empty() ? 0 : std::min(pos, (int)g_view.size() - 1);
}

// ------------------------------------------------------------------ config
static const UINT KM_SHIFT = 0x20000;      // modifier flag in keymap keys
struct KeyName { const wchar_t* name; UINT vk; };
static const KeyName kKeyNames[] = {
    {L"LEFT", VK_LEFT}, {L"RIGHT", VK_RIGHT}, {L"UP", VK_UP}, {L"DOWN", VK_DOWN},
    {L"SPACE", VK_SPACE}, {L"ESCAPE", VK_ESCAPE}, {L"ESC", VK_ESCAPE},
    {L"ENTER", VK_RETURN}, {L"RETURN", VK_RETURN}, {L"TAB", VK_TAB},
    {L"BACKSPACE", VK_BACK}, {L"DELETE", VK_DELETE}, {L"INSERT", VK_INSERT},
    {L"HOME", VK_HOME}, {L"END", VK_END},
    {L"PGUP", VK_PRIOR}, {L"PAGEUP", VK_PRIOR}, {L"PGDN", VK_NEXT}, {L"PAGEDOWN", VK_NEXT},
    {L"GRAVE", VK_OEM_3}, {L"MINUS", VK_OEM_MINUS}, {L"PLUS", VK_OEM_PLUS},
    {L"COMMA", VK_OEM_COMMA}, {L"PERIOD", VK_OEM_PERIOD},
    {L"NUMPAD0", VK_NUMPAD0}, {L"NUMPAD1", VK_NUMPAD1}, {L"NUMPAD2", VK_NUMPAD2},
    {L"NUMPAD3", VK_NUMPAD3}, {L"NUMPAD4", VK_NUMPAD4}, {L"NUMPAD5", VK_NUMPAD5},
    {L"NUMPAD6", VK_NUMPAD6}, {L"NUMPAD7", VK_NUMPAD7}, {L"NUMPAD8", VK_NUMPAD8},
    {L"NUMPAD9", VK_NUMPAD9},
    {L"F1", VK_F1}, {L"F2", VK_F2}, {L"F3", VK_F3}, {L"F4", VK_F4}, {L"F5", VK_F5},
    {L"F6", VK_F6}, {L"F7", VK_F7}, {L"F8", VK_F8}, {L"F9", VK_F9}, {L"F10", VK_F10},
    {L"F11", VK_F11}, {L"F12", VK_F12},
    {L"SLASH", VK_OEM_2}, {L"SEMICOLON", VK_OEM_1}, {L"LBRACKET", VK_OEM_4},
    {L"RBRACKET", VK_OEM_6}, {L"QUOTE", VK_OEM_7}, {L"BACKSLASH", VK_OEM_5},
};
static UINT parseKeyName(std::wstring s) {
    for (auto& c : s) c = towupper(c);
    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    if (s.empty()) return 0;
    UINT mod = 0;
    if (s.rfind(L"SHIFT+", 0) == 0) { mod = KM_SHIFT; s = s.substr(6); }
    if (!s.empty() && mod) {
        UINT base = parseKeyName(s);
        return base ? (base | mod) : 0;
    }
    if (s.empty()) return 0;
    if (s.size() == 1) {
        wchar_t c = s[0];
        if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) return (UINT)c;
        switch (c) {
        case L'/': return VK_OEM_2;  case L';': return VK_OEM_1;
        case L'[': return VK_OEM_4;  case L']': return VK_OEM_6;
        case L'\'': return VK_OEM_7; case L'\\': return VK_OEM_5;
        case L',': return VK_OEM_COMMA;  case L'.': return VK_OEM_PERIOD;
        case L'-': return VK_OEM_MINUS;  case L'=': return VK_OEM_PLUS;
        case L'`': return VK_OEM_3;
        }
    }
    for (auto& k : kKeyNames) if (s == k.name) return k.vk;
    return 0;
}
static void bindKey(const std::wstring& names, Action a) {
    // comma separated list of keys
    size_t start = 0;
    while (start <= names.size()) {
        size_t comma = names.find(L',', start);
        std::wstring one = names.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
        UINT vk = parseKeyName(one);
        if (vk) {
            g_keymap[vk] = a;
            // digits also bind their numpad twin automatically
            if (vk >= '0' && vk <= '9') g_keymap[VK_NUMPAD0 + (vk - '0')] = a;
        }
        if (comma == std::wstring::npos) break;
        start = comma + 1;
    }
}
static std::wstring iniGet(const wchar_t* sec, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[512];
    GetPrivateProfileStringW(sec, key, def, buf, 512, g_iniPath.c_str());
    return buf;
}
static const wchar_t* kDefaultIni =
L"; iRate configuration - edit and restart to apply.\r\n"
L"; Keys: single letters/digits, or LEFT RIGHT UP DOWN SPACE ESC ENTER TAB\r\n"
L";       BACKSPACE DELETE HOME END PGUP PGDN F1-F12 NUMPAD0-9 GRAVE COMMA PERIOD\r\n"
L"; Multiple keys per action: comma separated (e.g. next=RIGHT,SPACE)\r\n"
L"; Digits automatically also bind their numpad equivalent.\r\n"
L"[keys]\r\n"
L"next=RIGHT\r\n"
L"prev=LEFT\r\n"
L"first=HOME\r\n"
L"last=END\r\n"
L"rate1=1\r\n"
L"rate2=2\r\n"
L"rate3=3\r\n"
L"rate4=4\r\n"
L"rate5=5\r\n"
L"rate0=0\r\n"
L"reject=X\r\n"
L"labelred=6\r\n"
L"labelyellow=7\r\n"
L"labelgreen=8\r\n"
L"labelblue=9\r\n"
L"labelpurple=P\r\n"
L"toggleinfo=I\r\n"
L"zoom=SLASH\r\n"
L"sortnext=RBRACKET\r\n"
L"sortprev=LBRACKET\r\n"
L"sortdir=SEMICOLON\r\n"
L"filter=F\r\n"
L"grid=G\r\n"
L"infopos=Z\r\n"
L"help=H\r\n"
L"prefs=SHIFT+P\r\n"
L"quit=ESC\r\n"
L"[options]\r\n"
L"; jump to next image after rating: capslock (only when Caps Lock is on),\r\n"
L"; always, or never\r\n"
L"autoadvance=capslock\r\n"
L"; 1 = resume point/sort/view per folder, saved as irate.session in the browsed\r\n"
L"; folder root. 0 = keep a single global resume in this ini instead.\r\n"
L"sessioninfolder=1\r\n"
L"; treat RAW+JPG with the same name as one image (shown as +JPG)\r\n"
L"pairrawjpg=1\r\n"
L"; file extensions to include (semicolon separated). JPEGs also supported.\r\n"
L"extensions=.arw;.cr2;.cr3;.nef;.nrw;.raf;.rw2;.pef;.dng;.orf\r\n"
L"; show the info bar at startup\r\n"
L"showinfo=1\r\n"
L"; debuglog=1 writes timing diagnostics to irate.log next to this ini\r\n"
L"debuglog=0\r\n"
L"[state]\r\n"
L"lastfolder=\r\n";

static void loadConfig() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    size_t sl = dir.find_last_of(L"\\/");
    dir = dir.substr(0, sl + 1);
    std::wstring exeIni = dir + L"irate.ini";
    wchar_t lad[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH)) {
        std::wstring cfgDir = std::wstring(lad) + L"\\iRate";
        CreateDirectoryW(cfgDir.c_str(), nullptr);
        g_iniPath = cfgDir + L"\\irate.ini";
        // migrate: JustRate-era LOCALAPPDATA ini, then any ini next to the exe
        if (GetFileAttributesW(g_iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::wstring oldIni = std::wstring(lad) + L"\\JustRate\\justrate.ini";
            std::wstring exeIniOld = dir + L"justrate.ini";
            if (GetFileAttributesW(oldIni.c_str()) != INVALID_FILE_ATTRIBUTES)
                CopyFileW(oldIni.c_str(), g_iniPath.c_str(), TRUE);
            else if (GetFileAttributesW(exeIni.c_str()) != INVALID_FILE_ATTRIBUTES)
                CopyFileW(exeIni.c_str(), g_iniPath.c_str(), TRUE);
            else if (GetFileAttributesW(exeIniOld.c_str()) != INVALID_FILE_ATTRIBUTES)
                CopyFileW(exeIniOld.c_str(), g_iniPath.c_str(), TRUE);
        }
    } else {
        g_iniPath = exeIni;   // no LOCALAPPDATA: fall back to exe folder
    }
    if (GetFileAttributesW(g_iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileW(g_iniPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            std::string utf8 = wideToUtf8(kDefaultIni);
            DWORD wr; WriteFile(h, utf8.data(), (DWORD)utf8.size(), &wr, nullptr);
            CloseHandle(h);
        }
    }
    bindKey(iniGet(L"keys", L"next", L"RIGHT"), A_NEXT);
    bindKey(iniGet(L"keys", L"prev", L"LEFT"), A_PREV);
    bindKey(iniGet(L"keys", L"first", L"HOME"), A_FIRST);
    bindKey(iniGet(L"keys", L"last", L"END"), A_LAST);
    bindKey(iniGet(L"keys", L"rate1", L"1"), A_RATE1);
    bindKey(iniGet(L"keys", L"rate2", L"2"), A_RATE2);
    bindKey(iniGet(L"keys", L"rate3", L"3"), A_RATE3);
    bindKey(iniGet(L"keys", L"rate4", L"4"), A_RATE4);
    bindKey(iniGet(L"keys", L"rate5", L"5"), A_RATE5);
    bindKey(iniGet(L"keys", L"rate0", L"0"), A_RATE0);
    bindKey(iniGet(L"keys", L"labelred", L"6"), A_LABEL_RED);
    bindKey(iniGet(L"keys", L"labelyellow", L"7"), A_LABEL_YELLOW);
    bindKey(iniGet(L"keys", L"labelgreen", L"8"), A_LABEL_GREEN);
    bindKey(iniGet(L"keys", L"labelblue", L"9"), A_LABEL_BLUE);
    bindKey(iniGet(L"keys", L"labelpurple", L"P"), A_LABEL_PURPLE);
    bindKey(iniGet(L"keys", L"toggleinfo", L"I"), A_TOGGLE_INFO);
    bindKey(iniGet(L"keys", L"quit", L"ESC"), A_QUIT);
    bindKey(iniGet(L"keys", L"zoom", L"SLASH"), A_ZOOM);
    bindKey(iniGet(L"keys", L"sortnext", L"RBRACKET"), A_SORTNEXT);
    bindKey(iniGet(L"keys", L"sortprev", L"LBRACKET"), A_SORTPREV);
    bindKey(iniGet(L"keys", L"sortdir", L"SEMICOLON"), A_SORTDIR);
    bindKey(iniGet(L"keys", L"filter", L"F"), A_FILTER);
    bindKey(iniGet(L"keys", L"grid", L"G"), A_GRID);
    bindKey(iniGet(L"keys", L"infopos", L"Z"), A_INFOPOS);
    bindKey(iniGet(L"keys", L"help", L"H"), A_HELP);
    bindKey(iniGet(L"keys", L"prefs", L"SHIFT+P"), A_PREFS);
    bindKey(iniGet(L"keys", L"reject", L"X"), A_REJECT);
    g_infoTop = iniGet(L"state", L"infotop", L"0") == L"1";
    g_sessionInFolder = iniGet(L"options", L"sessioninfolder", L"1") != L"0";
    g_pairRawJpg = iniGet(L"options", L"pairrawjpg", L"1") != L"0";
    std::wstring adv = iniGet(L"options", L"autoadvance", L"capslock");
    if (adv == L"always") g_advanceMode = 1;
    else if (adv == L"never" || adv == L"0") g_advanceMode = 2;
    else g_advanceMode = 0;   // "capslock" (legacy "1" maps here too)
    g_showInfo = iniGet(L"options", L"showinfo", L"1") != L"0";
    g_sortType = _wtoi(iniGet(L"state", L"sorttype", L"0").c_str());
    if (g_sortType < 0 || g_sortType > 2) g_sortType = 0;
    g_sortDesc = iniGet(L"state", L"sortdesc", L"0") == L"1";
    g_gridMode = iniGet(L"state", L"gridmode", L"0") == L"1";
    std::wstring ex = iniGet(L"options", L"extensions", L".arw;.cr2;.cr3;.nef;.nrw;.raf;.rw2;.pef;.dng;.orf");
    if (ex == L".arw" && iniGet(L"state", L"version", L"1") == L"1") {
        ex = L".arw;.cr2;.cr3;.nef;.nrw";   // v1 ini migration: new formats
        WritePrivateProfileStringW(L"options", L"extensions", ex.c_str(), g_iniPath.c_str());
    }
    if (_wtoi(iniGet(L"state", L"version", L"1").c_str()) < 4) {
        // v4: new raw formats join the default extension list
        if (ex == L".arw;.cr2;.cr3;.nef;.nrw" || ex == L".arw") {
            ex = L".arw;.cr2;.cr3;.nef;.nrw;.raf;.rw2;.pef;.dng;.orf";
            WritePrivateProfileStringW(L"options", L"extensions", ex.c_str(), g_iniPath.c_str());
        }
    }
    if (_wtoi(iniGet(L"state", L"version", L"1").c_str()) < 3) {
        // v3: P = purple label, prefs moved to Shift+P
        if (iniGet(L"keys", L"labelpurple", L"__d") != L"__d" &&
            iniGet(L"keys", L"labelpurple", L"").empty())
            WritePrivateProfileStringW(L"keys", L"labelpurple", L"P", g_iniPath.c_str());
        std::wstring pk = iniGet(L"keys", L"prefs", L"");
        if (pk.empty() || pk == L"P")
            WritePrivateProfileStringW(L"keys", L"prefs", L"SHIFT+P", g_iniPath.c_str());
    }
    WritePrivateProfileStringW(L"state", L"version", L"4", g_iniPath.c_str());
    if (iniGet(L"options", L"debuglog", L"0") == L"1") {
        std::wstring lp = g_iniPath.substr(0, g_iniPath.find_last_of(L"\\/") + 1) + L"irate.log";
        g_logFile = CreateFileW(lp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        g_logT0 = GetTickCount64();
    }
    g_exts.clear();
    size_t start = 0;
    while (start <= ex.size()) {
        size_t semi = ex.find(L';', start);
        std::wstring one = ex.substr(start, semi == std::wstring::npos ? std::wstring::npos : semi - start);
        while (!one.empty() && iswspace(one.front())) one.erase(one.begin());
        while (!one.empty() && iswspace(one.back())) one.pop_back();
        if (!one.empty()) {
            if (one[0] != L'.') one = L"." + one;
            for (auto& c : one) c = towlower(c);
            g_exts.push_back(one);
        }
        if (semi == std::wstring::npos) break;
        start = semi + 1;
    }
    if (g_exts.empty()) g_exts.push_back(L".arw");
}

// is the folder on a seek-bound drive? Policy verified against a real 11-drive
// fleet: the seek-penalty query answers correctly on NVMe/SATA/most USB (incl.
// USB SSDs); some USB HDD enclosures fail it (ERROR_INVALID_FUNCTION), so a
// failed query on a USB bus means "assume spinning". Network is always slow.
static bool driveIsSlow(const std::wstring& folder) {
    if (folder.size() < 2 || folder[1] != L':') return true;      // UNC path
    wchar_t root[4] = { folder[0], L':', L'\\', 0 };
    if (GetDriveTypeW(root) == DRIVE_REMOTE) return true;
    wchar_t vol[8] = { L'\\', L'\\', L'.', L'\\', folder[0], L':', 0 };
    HANDLE h = CreateFileW(vol, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;                  // can't tell: assume fast
    bool slow = false;
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceSeekPenaltyProperty;
    q.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR d{};
    DWORD br = 0;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &d, sizeof(d), &br, nullptr) &&
        br >= sizeof(d)) {
        slow = d.IncursSeekPenalty != FALSE;
    } else {
        STORAGE_PROPERTY_QUERY q2{};
        q2.PropertyId = StorageDeviceProperty;
        q2.QueryType = PropertyStandardQuery;
        BYTE buf[1024] = {};
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q2, sizeof(q2), buf, sizeof(buf), &br, nullptr) &&
            br >= sizeof(STORAGE_DEVICE_DESCRIPTOR))
            slow = ((STORAGE_DEVICE_DESCRIPTOR*)buf)->BusType == BusTypeUsb;
    }
    CloseHandle(h);
    return slow;
}

// ------------------------------------------------------------------ scanning
static bool hasWantedExt(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = name.substr(dot);
    for (auto& c : ext) c = towlower(c);
    for (auto& e : g_exts) if (ext == e) return true;
    return false;
}
static void scanDir(const std::wstring& dir, std::vector<Item>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            size_t nl = wcslen(fd.cFileName);      // skip Lightroom preview trees:
            if (nl >= 7 && !_wcsicmp(fd.cFileName + nl - 7, L".lrdata")) continue;
            scanDir(dir + L"\\" + fd.cFileName, out);
        } else if (hasWantedExt(fd.cFileName)) {
            Item it;
            it.path = dir + L"\\" + fd.cFileName;
            it.mtime = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
            it.fsize = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            out.push_back(std::move(it));
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static bool itemLess(const Item& a, const Item& b) {
    int c = 0;
    if (g_sortType == 1) c = a.mtime < b.mtime ? -1 : a.mtime > b.mtime ? 1 : 0;
    else if (g_sortType == 2) c = a.fsize < b.fsize ? -1 : a.fsize > b.fsize ? 1 : 0;
    if (c == 0) {
        int pc = StrCmpLogicalW(a.path.c_str(), b.path.c_str());
        c = pc < 0 ? -1 : pc > 0 ? 1 : 0;
    }
    if (c == 0) return false;
    return g_sortDesc ? c > 0 : c < 0;
}
static bool extIs(const std::wstring& path, const wchar_t* const* list, int nlist) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = path.substr(dot);
    for (auto& c : ext) c = towlower(c);
    for (int i = 0; i < nlist; i++) if (ext == list[i]) return true;
    return false;
}
static bool isRawFile(const std::wstring& p2) {
    static const wchar_t* kRaw[] = { L".arw", L".cr2", L".cr3", L".nef", L".nrw",
                                     L".raf", L".rw2", L".pef", L".dng", L".orf" };
    return extIs(p2, kRaw, 10);
}
static bool isJpgFile(const std::wstring& p2) {
    static const wchar_t* kJpg[] = { L".jpg", L".jpeg" };
    return extIs(p2, kJpg, 2);
}
static std::wstring pairKey(const std::wstring& p2) {
    size_t dot = p2.find_last_of(L'.');
    std::wstring k = (dot == std::wstring::npos) ? p2 : p2.substr(0, dot);
    for (auto& c : k) c = towlower(c);
    return k;
}

static void scanThread() {
    std::vector<Item> files;
    scanDir(g_folder, files);
    if (g_pairRawJpg) {
        std::set<std::wstring> rawKeys, jpgKeys;
        for (auto& it : files) {
            if (isRawFile(it.path)) rawKeys.insert(pairKey(it.path));
            else if (isJpgFile(it.path)) jpgKeys.insert(pairKey(it.path));
        }
        std::vector<Item> merged;
        merged.reserve(files.size());
        for (auto& it : files) {
            if (isJpgFile(it.path) && rawKeys.count(pairKey(it.path))) continue; // twin of a raw
            if (isRawFile(it.path) && jpgKeys.count(pairKey(it.path))) it.jpgTwin = true;
            merged.push_back(std::move(it));
        }
        files.swap(merged);
    }
    std::sort(files.begin(), files.end(), itemLess);
    {
        std::lock_guard<std::mutex> lk(g_itemsMx);
        g_items = std::move(files);
    }
    g_scanning = false;
    logLine("scan done  items=%d", (int)g_items.size());
    PostMessageW(g_hwnd, WM_APP_SCANDONE, 0, 0);
}

// ------------------------------------------------------------------ thumb disk cache
// irate_thumbs.jrc : "JRTC"+ver, then entries of
// { u64 pathHash, u64 mtime, u64 fsize, u16 w, u16 h, u32 len, jpeg[len] }
static std::mutex g_tcMx;
static HANDLE g_tcFile = INVALID_HANDLE_VALUE;
struct TcEnt { uint64_t off; uint32_t len; uint64_t mtime, fsize; uint16_t w, h; };
static std::map<uint64_t, TcEnt> g_tcIndex;
static std::wstring g_tcPath;   // set by tcInit

static uint64_t fnv64(const std::wstring& in) {
    uint64_t h = 1469598103934665603ULL;
    for (wchar_t c : in) { h ^= (uint64_t)towlower(c); h *= 1099511628211ULL; }
    return h;
}
static uint64_t tcKeyFor(const std::wstring& path) {
    if (g_sessionInFolder && path.size() > g_folder.size() + 1 &&
        _wcsnicmp(path.c_str(), g_folder.c_str(), g_folder.size()) == 0)
        return fnv64(path.substr(g_folder.size() + 1));
    return fnv64(path);
}
static bool tcPread(uint64_t off, void* dst, uint32_t n) {
    OVERLAPPED ov{};
    ov.Offset = (DWORD)(off & 0xFFFFFFFF);
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD rd = 0;
    return ReadFile(g_tcFile, dst, n, &rd, &ov) && rd == n;
}
static void tcInit() {
    if (g_tcFile != INVALID_HANDLE_VALUE) { CloseHandle(g_tcFile); g_tcFile = INVALID_HANDLE_VALUE; }
    g_tcIndex.clear();
    std::wstring f;
    if (g_sessionInFolder && !g_folder.empty()) {
        // per-job: cache lives with the dataset, like irate.session
        f = g_folder + L"\\irate_thumbs.jrc";
        std::wstring oldF = g_folder + L"\\justrate_thumbs.jrc";   // JustRate-era name
        if (GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(oldF.c_str()) != INVALID_FILE_ATTRIBUTES)
            MoveFileW(oldF.c_str(), f.c_str());
    } else {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring exeSide = exe;
        size_t sl = exeSide.find_last_of(L"\\/");
        exeSide = exeSide.substr(0, sl + 1) + L"justrate_thumbs.jrc";
        f = exeSide;
        wchar_t lad[MAX_PATH];
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH)) {
            std::wstring cfgDir = std::wstring(lad) + L"\\iRate";
            CreateDirectoryW(cfgDir.c_str(), nullptr);
            f = cfgDir + L"\\irate_thumbs.jrc";
            if (GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES) {
                std::wstring oldF = std::wstring(lad) + L"\\JustRate\\justrate_thumbs.jrc";
                if (GetFileAttributesW(oldF.c_str()) != INVALID_FILE_ATTRIBUTES)
                    MoveFileExW(oldF.c_str(), f.c_str(), MOVEFILE_COPY_ALLOWED);
                else if (GetFileAttributesW(exeSide.c_str()) != INVALID_FILE_ATTRIBUTES)
                    MoveFileExW(exeSide.c_str(), f.c_str(), MOVEFILE_COPY_ALLOWED);
            }
        }
    }
    g_tcPath = f;
    g_tcFile = CreateFileW(f.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_tcFile == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz; GetFileSizeEx(g_tcFile, &sz);
    uint8_t hdr[8];
    if (sz.QuadPart < 8) {
        static const uint8_t kHdr[8] = { 'J','R','T','C',1,0,0,0 };
        memcpy(hdr, kHdr, 8);
        OVERLAPPED ov{}; DWORD wr;
        WriteFile(g_tcFile, hdr, 8, &wr, &ov);
        return;
    }
    if (!tcPread(0, hdr, 8) || memcmp(hdr, "JRTC", 4)) { CloseHandle(g_tcFile); g_tcFile = INVALID_HANDLE_VALUE; return; }
    uint64_t pos = 8;
    uint8_t eh[32];
    while (pos + 32 <= (uint64_t)sz.QuadPart) {
        if (!tcPread(pos, eh, 32)) break;
        uint64_t hash, mt, fs; uint16_t w, h; uint32_t len;
        memcpy(&hash, eh, 8); memcpy(&mt, eh + 8, 8); memcpy(&fs, eh + 16, 8);
        memcpy(&w, eh + 24, 2); memcpy(&h, eh + 26, 2); memcpy(&len, eh + 28, 4);
        if (len == 0 || len > 4 * 1024 * 1024 || pos + 32 + len > (uint64_t)sz.QuadPart) break;
        g_tcIndex[hash] = { pos + 32, len, mt, fs, w, h };   // later entries win
        pos += 32 + len;
    }
    // no size cap - instead, compact away superseded/orphaned bytes when they
    // exceed ~40% of the file (keeps every live thumbnail, loses nothing)
    uint64_t liveBytes = 8;
    for (auto& kv : g_tcIndex) liveBytes += 32 + kv.second.len;
    if ((uint64_t)sz.QuadPart > 64ull * 1024 * 1024 &&
        liveBytes < (uint64_t)sz.QuadPart * 3 / 5) {
        std::wstring tmp = f + L".compact";
        HANDLE ht = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (ht != INVALID_HANDLE_VALUE) {
            bool ok = true;
            DWORD wr = 0;
            static const uint8_t kHdr2[8] = { 'J','R','T','C',1,0,0,0 };
            ok = WriteFile(ht, kHdr2, 8, &wr, nullptr) && wr == 8;
            std::map<uint64_t, TcEnt> newIndex;
            uint64_t npos = 8;
            std::vector<uint8_t> blob;
            for (auto& kv : g_tcIndex) {
                if (!ok) break;
                blob.resize(kv.second.len);
                if (!tcPread(kv.second.off, blob.data(), kv.second.len)) continue;
                uint8_t neh[32];
                memcpy(neh, &kv.first, 8);
                memcpy(neh + 8, &kv.second.mtime, 8);
                memcpy(neh + 16, &kv.second.fsize, 8);
                memcpy(neh + 24, &kv.second.w, 2);
                memcpy(neh + 26, &kv.second.h, 2);
                memcpy(neh + 28, &kv.second.len, 4);
                ok = WriteFile(ht, neh, 32, &wr, nullptr) && wr == 32 &&
                     WriteFile(ht, blob.data(), kv.second.len, &wr, nullptr) && wr == kv.second.len;
                if (ok) {
                    newIndex[kv.first] = { npos + 32, kv.second.len, kv.second.mtime,
                                           kv.second.fsize, kv.second.w, kv.second.h };
                    npos += 32 + kv.second.len;
                }
            }
            CloseHandle(ht);
            if (ok) {
                CloseHandle(g_tcFile);
                if (MoveFileExW(tmp.c_str(), f.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                    g_tcIndex.swap(newIndex);
                }
                g_tcFile = CreateFileW(f.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                                       nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            } else DeleteFileW(tmp.c_str());
        }
    }
}
static bool tcLookup(uint64_t hash, uint64_t mtime, uint64_t fsize, std::vector<uint8_t>& blob, int& w, int& h) {
    ULONGLONG t0 = GetTickCount64();
    std::lock_guard<std::mutex> lk(g_tcMx);
    if (g_logFile != INVALID_HANDLE_VALUE) {
        ULONGLONG w8 = GetTickCount64() - t0;
        if (w8 > 50) logLine("SLOW tc lock wait %llums", (unsigned long long)w8);
    }
    if (g_tcFile == INVALID_HANDLE_VALUE) return false;
    auto it = g_tcIndex.find(hash);
    if (it == g_tcIndex.end() || it->second.mtime != mtime || it->second.fsize != fsize) return false;
    // reject size-mismatched thumbs (screen/DPI changed a lot)
    if ((int)it->second.w > g_thumbBoxW + 8 || (int)it->second.h > g_thumbBoxH + 8) return false;
    if ((int)it->second.w < g_thumbBoxW * 3 / 4 && (int)it->second.h < g_thumbBoxH * 3 / 4) return false;
    blob.resize(it->second.len);
    if (!tcPread(it->second.off, blob.data(), it->second.len)) return false;
    w = it->second.w; h = it->second.h;
    return true;
}
static void tcAppend(uint64_t hash, uint64_t mtime, uint64_t fsize, uint16_t w, uint16_t h,
                     const void* data, uint32_t len) {
    ULONGLONG t0app = GetTickCount64();
    std::lock_guard<std::mutex> lk(g_tcMx);
    if (g_tcFile == INVALID_HANDLE_VALUE || !len) return;
    LARGE_INTEGER sz; GetFileSizeEx(g_tcFile, &sz);
    uint8_t eh[32];
    memcpy(eh, &hash, 8); memcpy(eh + 8, &mtime, 8); memcpy(eh + 16, &fsize, 8);
    memcpy(eh + 24, &w, 2); memcpy(eh + 26, &h, 2); memcpy(eh + 28, &len, 4);
    OVERLAPPED ov{};
    ov.Offset = (DWORD)(sz.QuadPart & 0xFFFFFFFF);
    ov.OffsetHigh = (DWORD)(sz.QuadPart >> 32);
    DWORD wr = 0;
    if (!WriteFile(g_tcFile, eh, 32, &wr, &ov) || wr != 32) return;
    OVERLAPPED ov2{};
    uint64_t off2 = sz.QuadPart + 32;
    ov2.Offset = (DWORD)(off2 & 0xFFFFFFFF);
    ov2.OffsetHigh = (DWORD)(off2 >> 32);
    if (!WriteFile(g_tcFile, data, len, &wr, &ov2) || wr != len) return;
    g_tcIndex[hash] = { off2, len, mtime, fsize, w, h };
    ULONGLONG ms = GetTickCount64() - t0app;
    if (ms > 100) logLine("SLOW tcAppend %llums len=%u", (unsigned long long)ms, len);
}
static uint64_t tcSizeBytes() {
    std::lock_guard<std::mutex> lk(g_tcMx);
    if (g_tcFile == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz;
    return GetFileSizeEx(g_tcFile, &sz) ? (uint64_t)sz.QuadPart : 0;
}
static void tcClear() {
    std::lock_guard<std::mutex> lk(g_tcMx);
    if (g_tcFile != INVALID_HANDLE_VALUE) CloseHandle(g_tcFile);
    g_tcIndex.clear();
    if (!g_tcPath.empty()) {
        DeleteFileW(g_tcPath.c_str());
        g_tcFile = CreateFileW(g_tcPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                               nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_tcFile != INVALID_HANDLE_VALUE) {
            static const uint8_t kHdr3[8] = { 'J','R','T','C',1,0,0,0 };
            DWORD wr;
            WriteFile(g_tcFile, kHdr3, 8, &wr, nullptr);
        }
    } else g_tcFile = INVALID_HANDLE_VALUE;
}

// encode a 32bppBGR DIB to in-memory JPEG
static bool jpegEncode(IWICImagingFactory* fac, int w, int h, const void* bits, std::vector<uint8_t>& out) {
    IStream* ms = SHCreateMemStream(nullptr, 0);
    if (!ms) return false;
    bool ok = false;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    IPropertyBag2* props = nullptr;
    IWICBitmap* bmp = nullptr;
    HRESULT hr = fac->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(ms, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&fr, &props);
    if (SUCCEEDED(hr)) hr = fr->Initialize(props);
    if (SUCCEEDED(hr)) hr = fac->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGR,
                                                        w * 4, w * 4 * h, (BYTE*)bits, &bmp);
    if (SUCCEEDED(hr)) hr = fr->WriteSource(bmp, nullptr);
    if (SUCCEEDED(hr)) hr = fr->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    if (SUCCEEDED(hr)) {
        LARGE_INTEGER zero{}; ULARGE_INTEGER end{};
        ms->Seek(zero, STREAM_SEEK_END, &end);
        if (end.QuadPart > 0 && end.QuadPart < 8 * 1024 * 1024) {
            out.resize((size_t)end.QuadPart);
            ms->Seek(zero, STREAM_SEEK_SET, nullptr);
            ULONG rd = 0;
            ok = SUCCEEDED(ms->Read(out.data(), (ULONG)out.size(), &rd)) && rd == out.size();
        }
    }
    if (bmp) bmp->Release();
    if (props) props->Release();
    if (fr) fr->Release();
    if (enc) enc->Release();
    ms->Release();
    return ok;
}
// decode a small jpeg blob straight to a DIB
static bool jpegToDib(IWICImagingFactory* fac, const uint8_t* data, uint32_t len,
                      HBITMAP& outBmp, int& outW, int& outH) {
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    bool ok = false;
    HRESULT hr = fac->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromMemory((BYTE*)data, len);
    if (SUCCEEDED(hr)) hr = fac->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = fac->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr)) hr = conv->Initialize(frame, GUID_WICPixelFormat32bppBGR,
                                             WICBitmapDitherTypeNone, nullptr, 0.0,
                                             WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) {
        UINT fw = 0, fh = 0;
        conv->GetSize(&fw, &fh);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = (LONG)fw;
        bi.bmiHeader.biHeight = -(LONG)fh;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP hb = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (hb && SUCCEEDED(conv->CopyPixels(nullptr, fw * 4, fw * 4 * fh, (BYTE*)bits))) {
            outBmp = hb; outW = (int)fw; outH = (int)fh; ok = true;
        } else if (hb) DeleteObject(hb);
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (stream) stream->Release();
    return ok;
}

static void rescanFolder() {
    g_scanning = true;
    {
        std::lock_guard<std::mutex> lk(g_itemsMx);
        g_items.clear();
    }
    g_view.clear();
    g_cur = 0;
    g_sidecarsLoaded = false;
    g_sidecarLoading = false;
    std::thread(scanThread).detach();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ------------------------------------------------------------------ decoding
class Win32ByteSource : public ByteSource {
public:
    explicit Win32ByteSource(HANDLE h) : h_(h) {
        LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
        size_ = (uint64_t)sz.QuadPart;
    }
    bool read(uint64_t off, void* dst, uint32_t n) override {
        // serve small reads from a 256KB block cache
        if (n <= kBlock) {
            uint64_t b0 = off & ~(uint64_t)(kBlock - 1);
            if (!cacheValid_ || b0 != cacheOff_) {
                cacheLen_ = (uint32_t)std::min<uint64_t>(kBlock, size_ > b0 ? size_ - b0 : 0);
                if (!cacheLen_ || !pread(b0, cache_, cacheLen_)) { cacheValid_ = false; return pread(off, dst, n); }
                cacheOff_ = b0; cacheValid_ = true;
            }
            if (off - cacheOff_ + n <= cacheLen_) {
                memcpy(dst, cache_ + (off - cacheOff_), n);
                return true;
            }
        }
        return pread(off, dst, n);
    }
    uint64_t size() override { return size_; }
private:
    static const uint32_t kBlock = 256 * 1024;
    bool pread(uint64_t off, void* dst, uint32_t n) {
        uint8_t* p = (uint8_t*)dst;
        while (n) {
            OVERLAPPED ov{};
            ov.Offset = (DWORD)(off & 0xFFFFFFFF);
            ov.OffsetHigh = (DWORD)(off >> 32);
            DWORD rd = 0;
            if (!ReadFile(h_, p, n, &rd, &ov) || rd == 0) return false;
            p += rd; off += rd; n -= rd;
        }
        return true;
    }
    HANDLE h_;
    uint64_t size_ = 0;
    uint8_t cache_[kBlock];
    uint64_t cacheOff_ = 0;
    uint32_t cacheLen_ = 0;
    bool cacheValid_ = false;
};

static void decodeOne(IWICImagingFactory* fac, int idx, int mode) {
    ULONGLONG t0 = GetTickCount64(), tRead = 0;
    int gen = g_generation.load();
    std::wstring path;
    uint64_t mtimeV = 0, fsizeV = 0;
    {
        std::lock_guard<std::mutex> lk(g_itemsMx);
        if (idx < 0 || idx >= (int)g_items.size()) return;
        path = g_items[idx].path;
        mtimeV = g_items[idx].mtime;
        fsizeV = g_items[idx].fsize;
    }
    Slot slot;
    uint64_t hash = 0;
    bool cached = false;
    if (mode == 2) {                      // thumbnail: try the disk cache first
        hash = tcKeyFor(path);
        std::vector<uint8_t> blob;
        int tw = 0, th = 0;
        if (tcLookup(hash, mtimeV, fsizeV, blob, tw, th)) {
            HBITMAP hb; int w2, h2;
            if (jpegToDib(fac, blob.data(), (uint32_t)blob.size(), hb, w2, h2)) {
                slot.bmp = hb; slot.w = w2; slot.h = h2;
                cached = true;
            }
        }
    }
    if (!cached) {
    HANDLE hf = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    ArwResult res;
    std::vector<uint8_t> blob;
    if (hf == INVALID_HANDLE_VALUE) {
        slot.failed = true; slot.err = L"cannot open file";
    } else {
        Win32ByteSource src(hf);
        ArwParser parser(src);
        res = parser.parse();
        if (!res.ok) {
            slot.failed = true; slot.err = utf8ToWide(res.err);
        } else {
            blob.resize(res.jpegLen);
            if (!src.read(res.jpegOff, blob.data(), res.jpegLen)) {
                slot.failed = true; slot.err = L"preview read failed";
            }
        }
        CloseHandle(hf);
    }
    tRead = GetTickCount64();

    if (!slot.failed) {
        slot.exif = res.exif;
        IWICStream* stream = nullptr;
        IWICBitmapDecoder* dec = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        HRESULT hr = fac->CreateStream(&stream);
        if (SUCCEEDED(hr)) hr = stream->InitializeFromMemory(blob.data(), (DWORD)blob.size());
        if (SUCCEEDED(hr)) hr = fac->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &dec);
        if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &frame);
        UINT ow = 0, oh = 0;
        if (SUCCEEDED(hr)) hr = frame->GetSize(&ow, &oh);
        if (SUCCEEDED(hr) && ow && oh) {
            int orient = slot.exif.orientation;
            bool rot90 = (orient == 6 || orient == 8);
            // fit rotated dims into screen
            double rw = rot90 ? (double)oh : (double)ow;
            double rh = rot90 ? (double)ow : (double)oh;
            double maxW = (mode == 2) ? (double)g_thumbBoxW : (double)g_scrW;
            double maxH = (mode == 2) ? (double)g_thumbBoxH : (double)g_scrH;
            double scale = (mode == 1) ? 1.0 : std::min(maxW / rw, maxH / rh);
            if (mode == 2 && scale > 1.0) scale = 1.0;
            int tw = std::max(1, (int)(rw * scale + 0.5));
            int th = std::max(1, (int)(rh * scale + 0.5));
            // pre-rotation scale target
            UINT sw = rot90 ? th : tw, sh = rot90 ? tw : th;

            IWICBitmapSource* cur = frame; cur->AddRef();

            // Fast path: JPEG DCT-domain downscale during decode
            IWICBitmapSourceTransform* xf = nullptr;
            if (SUCCEEDED(frame->QueryInterface(IID_IWICBitmapSourceTransform, (void**)&xf))) {
                UINT cw = sw, ch = sh;
                if (SUCCEEDED(xf->GetClosestSize(&cw, &ch)) && cw && ch && (cw < ow || ch < oh)) {
                    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGR;
                    if (SUCCEEDED(xf->GetClosestPixelFormat(&fmt))) {
                        UINT bpp = 32;
                        if (fmt == GUID_WICPixelFormat24bppBGR) bpp = 24;
                        else if (fmt != GUID_WICPixelFormat32bppBGR) bpp = 0;
                        if (bpp) {
                            UINT stride = ((cw * bpp + 31) / 32) * 4;
                            std::vector<BYTE> px((size_t)stride * ch);
                            if (SUCCEEDED(xf->CopyPixels(nullptr, cw, ch, &fmt,
                                    WICBitmapTransformRotate0, stride, (UINT)px.size(), px.data()))) {
                                IWICBitmap* mem = nullptr;
                                if (SUCCEEDED(fac->CreateBitmapFromMemory(cw, ch, fmt, stride,
                                        (UINT)px.size(), px.data(), &mem))) {
                                    cur->Release();
                                    cur = mem; // takes ownership of copied pixels
                                }
                            }
                        }
                    }
                }
                xf->Release();
            }

            UINT curW = 0, curH = 0;
            cur->GetSize(&curW, &curH);
            IWICBitmapScaler* scaler = nullptr;
            IWICBitmapSource* afterScale = cur;
            if (curW != sw || curH != sh) {
                hr = fac->CreateBitmapScaler(&scaler);
                if (SUCCEEDED(hr)) hr = scaler->Initialize(cur, sw, sh, WICBitmapInterpolationModeFant);
                if (SUCCEEDED(hr)) afterScale = scaler;
            }

            IWICBitmapFlipRotator* rot = nullptr;
            IWICBitmap* rotCache = nullptr;
            if (SUCCEEDED(hr) && (orient == 3 || orient == 6 || orient == 8)) {
                // materialize the scaled pixels BEFORE rotating: the rotator
                // pulls source columns per output line, re-running the whole
                // upstream decode/scale chain each time — measured 9.5s vs
                // 16ms on one portrait a7R3 preview (classic WIC O(n^2) trap)
                if (SUCCEEDED(fac->CreateBitmapFromSource(afterScale, WICBitmapCacheOnLoad, &rotCache)))
                    afterScale = rotCache;
                if (SUCCEEDED(fac->CreateBitmapFlipRotator(&rot))) {
                    WICBitmapTransformOptions o =
                        orient == 3 ? WICBitmapTransformRotate180 :
                        orient == 6 ? WICBitmapTransformRotate90 : WICBitmapTransformRotate270;
                    if (SUCCEEDED(rot->Initialize(afterScale, o))) afterScale = rot;
                }
            }

            IWICFormatConverter* conv = nullptr;
            if (SUCCEEDED(hr)) hr = fac->CreateFormatConverter(&conv);
            if (SUCCEEDED(hr)) hr = conv->Initialize(afterScale, GUID_WICPixelFormat32bppBGR,
                                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                                     WICBitmapPaletteTypeCustom);
            if (SUCCEEDED(hr)) {
                UINT fw = 0, fh = 0;
                conv->GetSize(&fw, &fh);
                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = (LONG)fw;
                bi.bmiHeader.biHeight = -(LONG)fh;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                void* bits = nullptr;
                HBITMAP hbmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
                if (hbmp && SUCCEEDED(conv->CopyPixels(nullptr, fw * 4, fw * 4 * fh, (BYTE*)bits))) {
                    slot.bmp = hbmp; slot.w = (int)fw; slot.h = (int)fh;
                    slot.exif.width = ow; slot.exif.height = oh;
                    if (mode == 2) {
                        std::vector<uint8_t> jout;
                        if (jpegEncode(fac, (int)fw, (int)fh, bits, jout))
                            tcAppend(hash, mtimeV, fsizeV, (uint16_t)fw, (uint16_t)fh,
                                     jout.data(), (uint32_t)jout.size());
                    }
                } else if (hbmp) DeleteObject(hbmp);
            }
            if (conv) conv->Release();
            if (rot) rot->Release();
            if (rotCache) rotCache->Release();
            if (scaler) scaler->Release();
            cur->Release();
        }
        if (!slot.bmp && !slot.failed) { slot.failed = true; slot.err = L"preview decode failed"; }
        if (frame) frame->Release();
        if (dec) dec->Release();
        if (stream) stream->Release();
    }

    }   // !cached

    logLine("decoded item=%d mode=%d cached=%d fail=%d read=%llums total=%llums  %ls",
            idx, mode, cached ? 1 : 0, slot.failed ? 1 : 0,
            tRead ? (unsigned long long)(tRead - t0) : 0ull,
            (unsigned long long)(GetTickCount64() - t0), pathTail(path));
    if (gen != g_generation.load()) {                 // sort order changed mid-decode
        if (slot.bmp) DeleteObject(slot.bmp);
        return;
    }
    if (mode == 2) {
        slot.tick = GetTickCount();
        {
            std::lock_guard<std::mutex> lk(g_thumbMx);
            auto old = g_thumbCache.find(idx);
            if (old != g_thumbCache.end() && old->second.bmp) DeleteObject(old->second.bmp);
            g_thumbCache[idx] = slot;
            if (g_thumbCache.size() > 900) {          // LRU: keep the 700 most recent
                std::vector<std::pair<DWORD, int>> byAge;
                byAge.reserve(g_thumbCache.size());
                for (auto& kv : g_thumbCache) byAge.push_back({ kv.second.tick, kv.first });
                std::sort(byAge.begin(), byAge.end());
                size_t rm = g_thumbCache.size() - 700;
                for (size_t q = 0; q < rm && q < byAge.size(); q++) {
                    auto f = g_thumbCache.find(byAge[q].second);
                    if (f != g_thumbCache.end()) {
                        if (f->second.bmp) DeleteObject(f->second.bmp);
                        g_thumbCache.erase(f);
                    }
                }
            }
        }
        PostMessageW(g_hwnd, WM_APP_THUMB, (WPARAM)idx, 0);
        return;
    }
    if (mode == 1) {
        std::lock_guard<std::mutex> lk(g_zoomMx);
        if (g_zoomBmp) DeleteObject(g_zoomBmp);
        g_zoomBmp = slot.bmp; g_zoomW = slot.w; g_zoomH = slot.h;
        g_zoomIdx = slot.bmp ? idx : -1;
        PostMessageW(g_hwnd, WM_APP_ZOOMED, (WPARAM)idx, 0);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_cacheMx);
        auto old = g_cache.find(idx);
        if (old != g_cache.end() && old->second.bmp) DeleteObject(old->second.bmp);
        g_cache[idx] = slot;
    }
    PostMessageW(g_hwnd, WM_APP_DECODED, (WPARAM)idx, 0);
}

static void workerThread(bool lowAllowed) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* fac = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_IWICImagingFactory, (void**)&fac);
    while (!g_quit) {
        int idx = -1, hiDepth = 0, loDepth = 0;
        {
            std::unique_lock<std::mutex> lk(g_queueMx);
            g_queueCv.wait(lk, [lowAllowed] { return g_quit.load() || !g_queue.empty() ||
                                                     (lowAllowed && !g_lowQueue.empty()); });
            if (g_quit) break;
            if (!g_queue.empty()) {
                idx = g_queue.front();
                g_queue.pop_front();
            } else if (lowAllowed && !g_lowQueue.empty()) {
                idx = g_lowQueue.front();
                g_lowQueue.pop_front();
                g_lowSet.erase(idx);
            } else continue;
            g_inFlight.insert(idx);
            hiDepth = (int)g_queue.size(); loDepth = (int)g_lowQueue.size();
        }
        logLine("pop %d  high=%d low=%d", idx, hiDepth, loDepth);
        bool skip = false;
        if (idx < THUMB_BASE) {
            std::lock_guard<std::mutex> lk(g_cacheMx);
            skip = g_cache.count(idx) != 0;        // already done
        } else if (idx < ZOOM_BASE) {
            std::lock_guard<std::mutex> lk(g_thumbMx);
            skip = g_thumbCache.count(idx - THUMB_BASE) != 0;
        }
        if (!skip && fac) {
            if (idx >= ZOOM_BASE) decodeOne(fac, idx - ZOOM_BASE, 1);
            else if (idx >= THUMB_BASE) decodeOne(fac, idx - THUMB_BASE, 2);
            else decodeOne(fac, idx, 0);
        }
        {
            std::lock_guard<std::mutex> lk(g_queueMx);
            g_inFlight.erase(idx);
        }
    }
    if (fac) fac->Release();
    CoUninitialize();
}

static void requestDecode(int idx, bool front) {
    if (idx < 0) return;
    {
        std::lock_guard<std::mutex> lk(g_itemsMx);
        if (idx >= (int)g_items.size()) return;
    }
    {
        std::lock_guard<std::mutex> lk(g_cacheMx);
        if (g_cache.count(idx)) return;
    }
    {
        std::lock_guard<std::mutex> lk(g_queueMx);
        if (front) {
            // the image on screen must be next in line: if an old read-ahead
            // request for it is buried in the backlog, PROMOTE it instead of
            // returning — otherwise held-arrow stalls until the whole backlog
            // drains (was a real pause after long browses on slow drives)
            for (auto qi = g_queue.begin(); qi != g_queue.end(); ++qi)
                if (*qi == idx) {
                    logLine("bump item=%d from queue depth %d", idx, (int)(qi - g_queue.begin()));
                    g_queue.erase(qi);
                    break;
                }
            g_queue.push_front(idx);
        } else {
            if (g_inFlight.count(idx)) return;   // a worker is already on it
            for (int q : g_queue) if (q == idx) return;
            g_queue.push_back(idx);
        }
    }
    g_queueCv.notify_one();
}

static void requestZoom(int idx) {
    {
        std::lock_guard<std::mutex> lk(g_queueMx);
        g_queue.push_front(ZOOM_BASE + idx);
    }
    g_queueCv.notify_one();
}

static void requestThumb(int idx, bool front) {
    if (idx < 0) return;
    {
        std::lock_guard<std::mutex> lk(g_thumbMx);
        if (g_thumbCache.count(idx)) return;
    }
    {
        std::lock_guard<std::mutex> lk(g_queueMx);
        int key = THUMB_BASE + idx;
        if (front && !g_slowDrive) {
            g_lowQueue.push_front(key);          // duplicates fine: worker re-checks cache
        } else if (front) {
            // slow drive: background read-ahead is confined to the single
            // low-priority worker (no seek storm), so visible grid cells ride
            // the high-priority queue instead — decoded in parallel by all
            // workers, behind pending fullscreen decodes.
            for (int q : g_queue) if (q == key) return;
            g_queue.push_back(key);
        } else {
            if (g_lowSet.count(key)) return;
            g_lowQueue.push_back(key);
            g_lowSet.insert(key);
        }
    }
    g_queueCv.notify_all();   // _all: only worker 0 may take low-priority jobs
}

// queue thumbnails for the whole (filtered) set in the background, nearest first
static void preloadThumbs() {
    int n = (int)g_view.size();
    if (n == 0) return;
    const int MAXPRE = 4000;
    std::lock_guard<std::mutex> lkq(g_queueMx);
    std::lock_guard<std::mutex> lkt(g_thumbMx);
    int added = 0;
    for (int d = 0; d < n && added < MAXPRE; d++) {          // ripple out from current
        int candidates[2] = { g_cur + d, d ? g_cur - d : -1 };
        for (int pos : candidates) {
            if (pos < 0 || pos >= n) continue;
            int key = THUMB_BASE + g_view[pos];
            if (g_thumbCache.count(g_view[pos]) || g_lowSet.count(key)) continue;
            g_lowQueue.push_back(key);
            g_lowSet.insert(key);
            added++;
        }
    }
    logLine("preload queued %d thumbs", added);
    g_queueCv.notify_all();
}

static void clearThumbs() {
    std::lock_guard<std::mutex> lk(g_thumbMx);
    for (auto& kv : g_thumbCache) if (kv.second.bmp) DeleteObject(kv.second.bmp);
    g_thumbCache.clear();
}

static void clearDecodes() {
    {
        std::lock_guard<std::mutex> lk(g_queueMx);
        g_queue.clear();
        g_lowQueue.clear();
        g_lowSet.clear();
    }
    g_generation++;
    std::lock_guard<std::mutex> lk(g_cacheMx);
    for (auto& kv : g_cache) if (kv.second.bmp) DeleteObject(kv.second.bmp);
    g_cache.clear();
}

static void pruneCache() {
    std::vector<int> keep;
    for (int d = -CACHE_RADIUS; d <= CACHE_RADIUS; d++) {
        int it2 = viewItemAt(g_cur + d);
        if (it2 >= 0) keep.push_back(it2);
    }
    std::lock_guard<std::mutex> lk(g_cacheMx);
    for (auto it = g_cache.begin(); it != g_cache.end();) {
        if (std::find(keep.begin(), keep.end(), it->first) == keep.end()) {
            if (it->second.bmp) DeleteObject(it->second.bmp);
            it = g_cache.erase(it);
        } else ++it;
    }
}

static void prefetchAround() {
    requestDecode(viewItemAt(g_cur), true);
    requestDecode(viewItemAt(g_cur + g_lastDir), false);
    requestDecode(viewItemAt(g_cur + 2 * g_lastDir), false);
    requestDecode(viewItemAt(g_cur - g_lastDir), false);
}

// ------------------------------------------------------------------ UI
static void resetZoom() {
    g_zoomWanted = false;
    std::lock_guard<std::mutex> lk(g_zoomMx);
    if (g_zoomBmp) { DeleteObject(g_zoomBmp); g_zoomBmp = nullptr; }
    g_zoomIdx = -1;
}

static void gridEnsureVisible();

static void setCurrent(int pos) {
    int n = (int)g_view.size();
    if (n == 0) { InvalidateRect(g_hwnd, nullptr, FALSE); return; }
    if (g_zoomWanted || g_zoomIdx >= 0) resetZoom();
    pos = std::max(0, std::min(n - 1, pos));
    if (pos > g_cur) g_lastDir = 1; else if (pos < g_cur) g_lastDir = -1;
    g_cur = pos;
    logLine("nav view=%d item=%d", g_cur, g_view[g_cur]);
    if (g_items[g_view[g_cur]].rating == -2) requestSidecar(g_view[g_cur]);
    prefetchAround();
    pruneCache();
    if (g_gridMode) gridEnsureVisible();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void applySort() {
    if (g_scanning || g_items.empty()) return;
    int ci = curItem();
    std::wstring curPath = ci >= 0 ? g_items[ci].path : L"";
    {
        std::lock_guard<std::mutex> lk(g_itemsMx);
        std::sort(g_items.begin(), g_items.end(), itemLess);
    }
    clearDecodes();
    clearThumbs();
    rebuildView(false);
    int pos = 0;
    for (int q = 0; q < (int)g_view.size(); q++)
        if (g_items[g_view[q]].path == curPath) { pos = q; break; }
    g_cur = pos;
    setCurrent(pos);
    preloadThumbs();
}

static void computeGridMetrics() {
    g_gridTbH = MulDiv(48, g_dpi, 96);
    g_cellW = MulDiv(230, g_dpi, 96);
    g_cellH = MulDiv(200, g_dpi, 96);
    int cw = g_scrW;
    RECT cr;
    if (g_hwnd && GetClientRect(g_hwnd, &cr)) cw = cr.right;   // minus native scrollbar
    g_gridCols = std::max(1, cw / g_cellW);
    g_gridX0 = (cw - g_gridCols * g_cellW) / 2;
    g_gridRowsVis = std::max(1, (g_scrH - g_gridTbH) / g_cellH);
    g_thumbBoxW = g_cellW - MulDiv(16, g_dpi, 96);
    g_thumbBoxH = g_cellH - MulDiv(40, g_dpi, 96);
}

static void gridEnsureVisible() {
    computeGridMetrics();
    int row = g_gridCols > 0 ? g_cur / g_gridCols : 0;
    if (row < g_gridScroll) g_gridScroll = row;
    if (row >= g_gridScroll + g_gridRowsVis) g_gridScroll = row - g_gridRowsVis + 1;
    if (g_gridScroll < 0) g_gridScroll = 0;
}

// toolbar buttons (rebuilt each grid paint)
struct TbBtn { RECT rc; int id; };            // 1 prefs 2 filter 3 name 4 date 5 size 6 dir 7 loupe
static std::vector<TbBtn> g_tbBtns;

// filter panel chips (rebuilt each panel paint)
struct Chip { RECT rc; int kind; int val; };  // kind 0=rating 1=label 2=clear 3=close
static std::vector<Chip> g_chips;
static RECT g_panelRc{};

static void filterPanelLayout(RECT& panel, RECT& editRc) {
    int w = MulDiv(700, g_dpi, 96), h = MulDiv(196, g_dpi, 96);
    panel = { (g_scrW - w) / 2, MulDiv(64, g_dpi, 96), (g_scrW + w) / 2, MulDiv(64, g_dpi, 96) + h };
    int pad = MulDiv(14, g_dpi, 96);
    int rowH = MulDiv(34, g_dpi, 96);
    editRc = { panel.left + pad + MulDiv(52, g_dpi, 96), panel.top + pad + 2 * rowH + MulDiv(4, g_dpi, 96),
               panel.right - pad, panel.top + pad + 2 * rowH + MulDiv(30, g_dpi, 96) };
}

static LRESULT CALLBACK editSubProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN && (w == VK_ESCAPE || w == VK_RETURN)) { SetFocus(g_hwnd); return 0; }
    if (m == WM_CHAR && (w == VK_ESCAPE || w == VK_RETURN)) return 0;
    return CallWindowProcW(g_editOldProc, h, m, w, l);
}

static void closeFilterPanel() {
    g_filterOpen = false;
    if (g_filterEdit) { DestroyWindow(g_filterEdit); g_filterEdit = nullptr; }
    SetFocus(g_hwnd);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void toggleFilterPanel() {
    if (g_filterOpen) { closeFilterPanel(); return; }
    g_filterOpen = true;
    if (!g_sidecarsLoaded && !g_sidecarLoading.exchange(true)) {
        std::thread([] {
            for (size_t i = 0; i < g_items.size() && !g_quit; i++) {
                std::lock_guard<std::mutex> lk(g_itemsMx);
                if (i < g_items.size()) loadSidecar(g_items[i]);
            }
            g_sidecarsLoaded = true;
            PostMessageW(g_hwnd, WM_APP_SIDECARS, 0, 0);
        }).detach();
    }
    RECT panel, er;
    filterPanelLayout(panel, er);
    g_filterEdit = CreateWindowExW(0, L"EDIT", g_filter.text.c_str(),
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        er.left, er.top, er.right - er.left, er.bottom - er.top,
        g_hwnd, (HMENU)(INT_PTR)100, GetModuleHandleW(nullptr), nullptr);
    if (g_filterEdit) {
        g_editOldProc = (WNDPROC)SetWindowLongPtrW(g_filterEdit, GWLP_WNDPROC, (LONG_PTR)editSubProc);
        HFONT f = CreateFontW(-MulDiv(16, g_dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Segoe UI");
        SendMessageW(g_filterEdit, WM_SETFONT, (WPARAM)f, TRUE);
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void filterChanged() {
    rebuildView(true);
    if (!g_view.empty()) setCurrent(g_cur);
    if (g_gridMode) gridEnsureVisible();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static COLORREF labelColor(int label) {
    switch (label) {
    case 1: return RGB(235, 70, 60);
    case 2: return RGB(245, 210, 60);
    case 3: return RGB(90, 200, 90);
    case 4: return RGB(80, 150, 245);
    case 5: return RGB(190, 100, 230);
    }
    return RGB(255, 255, 255);
}
static const wchar_t* labelNameW(int label) {
    switch (label) {
    case 1: return L"Red"; case 2: return L"Yellow"; case 3: return L"Green";
    case 4: return L"Blue"; case 5: return L"Purple";
    }
    return L"";
}

static void drawBtn(HDC dc, RECT rc, const wchar_t* txt, bool active) {
    HBRUSH br = CreateSolidBrush(active ? RGB(70, 110, 180) : RGB(45, 45, 48));
    FillRect(dc, &rc, br);
    DeleteObject(br);
    HBRUSH fr = CreateSolidBrush(RGB(90, 90, 95));
    FrameRect(dc, &rc, fr);
    DeleteObject(fr);
    SetTextColor(dc, RGB(235, 235, 235));
    RECT t = rc;
    DrawTextW(dc, txt, -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void paintGrid(HDC mem) {
    computeGridMetrics();
    int clw = g_scrW;                       // client width (scrollbar shaves the right edge)
    RECT crc;
    if (GetClientRect(g_hwnd, &crc)) clw = crc.right;
    int n = (int)g_view.size();
    int maxRow = g_gridCols ? std::max(0, (n + g_gridCols - 1) / g_gridCols - 1) : 0;
    if (g_gridScroll > maxRow) g_gridScroll = maxRow;
    if (g_gridScroll < 0) g_gridScroll = 0;
    {
        SCROLLINFO si{ sizeof(si) };
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        si.nMin = 0;
        si.nMax = maxRow;
        si.nPage = (UINT)g_gridRowsVis;
        si.nPos = g_gridScroll;
        SetScrollInfo(g_hwnd, SB_VERT, &si, TRUE);
    }

    // ---- toolbar
    RECT tb{ 0, 0, clw, g_gridTbH };
    HBRUSH tbr = CreateSolidBrush(RGB(28, 28, 30));
    FillRect(mem, &tb, tbr);
    DeleteObject(tbr);
    g_tbBtns.clear();
    int bx = MulDiv(10, g_dpi, 96), bh = MulDiv(30, g_dpi, 96);
    int by = (g_gridTbH - bh) / 2;
    auto addBtn = [&](const wchar_t* txt, int id, bool active, int wpx) {
        RECT rc{ bx, by, bx + MulDiv(wpx, g_dpi, 96), by + bh };
        drawBtn(mem, rc, txt, active);
        g_tbBtns.push_back({ rc, id });
        bx = rc.right + MulDiv(8, g_dpi, 96);
    };
    addBtn(L"\x2699 Prefs", 1, false, 84);
    addBtn(g_filter.active() ? L"\x25BC Filter \x25CF" : L"\x25BC Filter (F)", 2, g_filterOpen || g_filter.active(), 104);
    addBtn(L"\x26F6 Fullscreen (G)", 7, false, 128);
    addBtn(L"\x21E4 Start", 8, false, 78);
    addBtn(L"End \x21E5", 9, false, 70);
    bx += MulDiv(12, g_dpi, 96);
    addBtn(L"Name", 3, g_sortType == 0, 70);
    addBtn(L"Date", 4, g_sortType == 1, 70);
    addBtn(L"Size", 5, g_sortType == 2, 70);
    addBtn(g_sortDesc ? L"\x2193 desc (;)" : L"\x2191 asc (;)", 6, false, 96);

    // right: count + selected filename
    wchar_t info[320];
    std::wstring fname;
    if (n > 0) {
        fname = g_items[g_view[g_cur]].path;
        size_t sl = fname.find_last_of(L"\\/");
        if (sl != std::wstring::npos) fname = fname.substr(sl + 1);
    }
    if (g_filter.active())
        swprintf(info, 320, L"%ls   %d / %d (of %d)", fname.c_str(), n ? g_cur + 1 : 0, n, (int)g_items.size());
    else
        swprintf(info, 320, L"%ls   %d / %d", fname.c_str(), n ? g_cur + 1 : 0, n);
    RECT ir{ bx + MulDiv(10, g_dpi, 96), 0, clw - MulDiv(12, g_dpi, 96), g_gridTbH };
    SetTextColor(mem, RGB(200, 200, 200));
    DrawTextW(mem, info, -1, &ir, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (n == 0) {
        RECT r{ 0, g_gridTbH, clw, g_scrH };
        SetTextColor(mem, RGB(170, 170, 170));
        DrawTextW(mem, g_filter.active() ? L"No images match the filter \x2014 press F to adjust"
                                         : L"No images", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    // ---- cells
    int firstPos = g_gridScroll * g_gridCols;
    int lastPos = std::min(n - 1, (g_gridScroll + g_gridRowsVis + 1) * g_gridCols - 1);
    HFONT small = CreateFontW(-MulDiv(13, g_dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Segoe UI");
    HFONT oldF = (HFONT)SelectObject(mem, small);
    for (int pos = firstPos; pos <= lastPos; pos++) {
        int itemIdx = g_view[pos];
        int r = pos / g_gridCols - g_gridScroll, c = pos % g_gridCols;
        int cx = g_gridX0 + c * g_cellW, cy = g_gridTbH + r * g_cellH;
        RECT cell{ cx, cy, cx + g_cellW, cy + g_cellH };
        if (pos == g_cur) {
            HBRUSH sb = CreateSolidBrush(RGB(70, 110, 180));
            RECT hi = cell;
            FillRect(mem, &hi, sb);
            DeleteObject(sb);
        }
        Slot t; bool haveT = false;
        {
            std::lock_guard<std::mutex> lk(g_thumbMx);
            auto f = g_thumbCache.find(itemIdx);
            if (f != g_thumbCache.end()) { f->second.tick = GetTickCount(); t = f->second; haveT = true; }
        }
        int pad = MulDiv(8, g_dpi, 96);
        RECT inner{ cx + pad, cy + pad, cx + g_cellW - pad, cy + pad + g_thumbBoxH };
        if (haveT && t.bmp) {
            HDC img = CreateCompatibleDC(mem);
            HBITMAP ob = (HBITMAP)SelectObject(img, t.bmp);
            int ix = inner.left + (g_thumbBoxW - t.w) / 2;
            int iy = inner.top + (g_thumbBoxH - t.h) / 2;
            BitBlt(mem, ix, iy, t.w, t.h, img, 0, 0, SRCCOPY);
            SelectObject(img, ob);
            DeleteDC(img);
        } else {
            HBRUSH ph = CreateSolidBrush(RGB(38, 38, 40));
            FillRect(mem, &inner, ph);
            DeleteObject(ph);
            if (!haveT) requestThumb(itemIdx, true);
            SetTextColor(mem, RGB(120, 120, 120));
            RECT pr = inner;
            DrawTextW(mem, haveT ? L"\x26A0" : L"\x2026", -1, &pr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        // caption: stars + label dot
        Item& item = g_items[itemIdx];
        if (item.rating == -2) requestSidecar(itemIdx);   // async: paint never blocks on the drive
        RECT cap{ cx + pad, cy + pad + g_thumbBoxH, cx + g_cellW - pad, cy + g_cellH - MulDiv(4, g_dpi, 96) };
        std::wstring stars;
        if (item.rating == -1) {
            stars = L"\x2716";
            SetTextColor(mem, RGB(235, 70, 60));
        } else {
            int rating = item.rating < 0 ? 0 : item.rating;
            for (int i = 0; i < 5; i++) stars += (i < rating) ? L"\x2605" : L"\x2606";
            SetTextColor(mem, RGB(255, 200, 40));
        }
        DrawTextW(mem, stars.c_str(), -1, &cap, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        int lbl = item.label < 0 ? 0 : item.label;
        if (lbl) {
            SetTextColor(mem, labelColor(lbl));
            DrawTextW(mem, L"\x25CF", -1, &cap, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
    }
    SelectObject(mem, oldF);
    DeleteObject(small);

    // read ahead a few rows in both directions
    for (int q = lastPos + 1; q <= std::min(n - 1, lastPos + 4 * g_gridCols); q++)
        requestThumb(g_view[q], false);
    for (int q = firstPos - 1; q >= std::max(0, firstPos - 4 * g_gridCols); q--)
        requestThumb(g_view[q], false);
}

static void paintFilterPanel(HDC mem) {
    RECT panel, er;
    filterPanelLayout(panel, er);
    g_panelRc = panel;
    g_chips.clear();
    // dim + panel bg
    HBRUSH bg = CreateSolidBrush(RGB(24, 24, 26));
    FillRect(mem, &panel, bg);
    DeleteObject(bg);
    HBRUSH fr = CreateSolidBrush(RGB(110, 110, 115));
    FrameRect(mem, &panel, fr);
    DeleteObject(fr);

    HFONT small = CreateFontW(-MulDiv(15, g_dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Segoe UI");
    HFONT oldF = (HFONT)SelectObject(mem, small);
    int pad = MulDiv(14, g_dpi, 96);
    int rowH = MulDiv(34, g_dpi, 96);
    int labW = MulDiv(52, g_dpi, 96);
    int chipH = MulDiv(26, g_dpi, 96);

    SetTextColor(mem, RGB(200, 200, 200));
    RECT lr{ panel.left + pad, panel.top + pad, panel.left + pad + labW, panel.top + pad + chipH };
    DrawTextW(mem, L"Rating", -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    int x = lr.right + MulDiv(6, g_dpi, 96);
    const wchar_t* rlabels[7] = { L"0", L"1\x2605", L"2\x2605", L"3\x2605", L"4\x2605", L"5\x2605", L"\x2716" };
    for (int i = 0; i <= 6; i++) {
        RECT rc{ x, lr.top, x + MulDiv(i == 6 ? 46 : 56, g_dpi, 96), lr.top + chipH };
        drawBtn(mem, rc, rlabels[i], (g_filter.ratingMask >> i) & 1);
        g_chips.push_back({ rc, 0, i });
        x = rc.right + MulDiv(6, g_dpi, 96);
    }
    RECT lr2{ panel.left + pad, panel.top + pad + rowH, panel.left + pad + labW, panel.top + pad + rowH + chipH };
    SetTextColor(mem, RGB(200, 200, 200));
    DrawTextW(mem, L"Label", -1, &lr2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    x = lr2.right + MulDiv(6, g_dpi, 96);
    const wchar_t* llabels[6] = { L"None", L"Red", L"Yellow", L"Green", L"Blue", L"Purple" };
    for (int i = 0; i <= 5; i++) {
        RECT rc{ x, lr2.top, x + MulDiv(i == 0 ? 56 : 66, g_dpi, 96), lr2.top + chipH };
        bool on = (g_filter.labelMask >> i) & 1;
        drawBtn(mem, rc, llabels[i], on);
        if (i > 0) {
            SetTextColor(mem, labelColor(i));
            RECT dr{ rc.left + MulDiv(4, g_dpi, 96), rc.top, rc.right, rc.bottom };
            DrawTextW(mem, L"\x25CF", -1, &dr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        g_chips.push_back({ rc, 1, i });
        x = rc.right + MulDiv(6, g_dpi, 96);
    }
    SetTextColor(mem, RGB(200, 200, 200));
    RECT lr3{ panel.left + pad, er.top, panel.left + pad + labW, er.bottom };
    DrawTextW(mem, L"Text", -1, &lr3, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // bottom row: status + clear/close
    int byy = er.bottom + MulDiv(10, g_dpi, 96);
    wchar_t st[128];
    if (g_sidecarsLoaded)
        swprintf(st, 128, L"%d of %d shown", (int)g_view.size(), (int)g_items.size());
    else
        swprintf(st, 128, L"loading ratings\x2026");
    SetTextColor(mem, RGB(170, 170, 170));
    RECT sr{ panel.left + pad, byy, panel.left + MulDiv(300, g_dpi, 96), byy + chipH };
    DrawTextW(mem, st, -1, &sr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT cb{ panel.right - pad - MulDiv(150, g_dpi, 96), byy, panel.right - pad - MulDiv(84, g_dpi, 96), byy + chipH };
    drawBtn(mem, cb, L"Clear", false);
    g_chips.push_back({ cb, 2, 0 });
    RECT xb{ panel.right - pad - MulDiv(76, g_dpi, 96), byy, panel.right - pad, byy + chipH };
    drawBtn(mem, xb, L"Close (F)", false);
    g_chips.push_back({ xb, 3, 0 });
    SelectObject(mem, oldF);
    DeleteObject(small);
}

static std::wstring vkName(UINT vk) {
    std::wstring pre;
    if (vk & KM_SHIFT) { pre = L"Shift+"; vk &= ~KM_SHIFT; }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return pre + std::wstring(1, (wchar_t)vk);
    for (auto& k : kKeyNames) if (k.vk == vk) return pre + k.name;
    return L"?";
}

// shift-aware lookup: prefer Shift+key binding when Shift is held
static Action actionForKey(UINT vk) {
    if (GetKeyState(VK_SHIFT) & 0x8000) {
        auto it = g_keymap.find(vk | KM_SHIFT);
        if (it != g_keymap.end()) return it->second;
    }
    auto it = g_keymap.find(vk);
    return it != g_keymap.end() ? it->second : A_NONE;
}

struct ActRow { Action a; const wchar_t* label; const wchar_t* iniName; };
static const ActRow kActRows[] = {
    { A_NEXT, L"Next image", L"next" }, { A_PREV, L"Previous image", L"prev" },
    { A_FIRST, L"First image", L"first" }, { A_LAST, L"Last image", L"last" },
    { A_RATE1, L"Rate 1", L"rate1" }, { A_RATE2, L"Rate 2", L"rate2" },
    { A_RATE3, L"Rate 3", L"rate3" }, { A_RATE4, L"Rate 4", L"rate4" },
    { A_RATE5, L"Rate 5", L"rate5" }, { A_RATE0, L"Clear rating", L"rate0" },
    { A_REJECT, L"Reject toggle (Bridge/PM)", L"reject" },
    { A_LABEL_RED, L"Red label", L"labelred" }, { A_LABEL_YELLOW, L"Yellow label", L"labelyellow" },
    { A_LABEL_GREEN, L"Green label", L"labelgreen" }, { A_LABEL_BLUE, L"Blue label", L"labelblue" },
    { A_LABEL_PURPLE, L"Purple label", L"labelpurple" },
    { A_ZOOM, L"Zoom 100% / back", L"zoom" },
    { A_GRID, L"Grid \x2194 fullscreen", L"grid" }, { A_FILTER, L"Filter", L"filter" },
    { A_SORTNEXT, L"Next sort mode", L"sortnext" }, { A_SORTPREV, L"Prev sort mode", L"sortprev" },
    { A_SORTDIR, L"Sort direction", L"sortdir" },
    { A_TOGGLE_INFO, L"Info bar on/off", L"toggleinfo" }, { A_INFOPOS, L"Info bar top/bottom", L"infopos" },
    { A_HELP, L"Shortcut help", L"help" }, { A_PREFS, L"Preferences", L"prefs" },
    { A_QUIT, L"Quit", L"quit" },
};
static const int kNumActRows = (int)(sizeof(kActRows) / sizeof(kActRows[0]));

static std::wstring keysOfAction(Action a) {
    std::wstring keys;
    for (auto& kv : g_keymap) {
        if (kv.second != a) continue;
        if (kv.first >= VK_NUMPAD0 && kv.first <= VK_NUMPAD9) continue;
        if (!keys.empty()) keys += L", ";
        keys += vkName(kv.first);
    }
    return keys;
}

static void writeKeysToIni(Action a) {
    for (auto& r : kActRows) {
        if (r.a != a) continue;
        WritePrivateProfileStringW(L"keys", r.iniName, keysOfAction(a).c_str(), g_iniPath.c_str());
        return;
    }
}

static void rebindAction(Action a, UINT vk) {
    Action stolen = A_NONE;
    auto owner = g_keymap.find(vk);
    if (owner != g_keymap.end() && owner->second != a) stolen = owner->second;
    for (auto it2 = g_keymap.begin(); it2 != g_keymap.end();) {
        bool killA = it2->second == a;
        bool killVk = it2->first == vk ||
            (vk >= '0' && vk <= '9' && it2->first == (UINT)(VK_NUMPAD0 + vk - '0'));
        if (killA || killVk) it2 = g_keymap.erase(it2); else ++it2;
    }
    g_keymap[vk] = a;
    if (vk >= '0' && vk <= '9') g_keymap[VK_NUMPAD0 + (vk - '0')] = a;   // unshifted digits only
    writeKeysToIni(a);
    if (stolen != A_NONE) writeKeysToIni(stolen);
}

static void paintHelp(HDC mem) {
    const int nRows = kNumActRows;
    const int perCol = (nRows + 1) / 2;
    int rowH = MulDiv(26, g_dpi, 96);
    int colW = MulDiv(330, g_dpi, 96);
    int pad = MulDiv(20, g_dpi, 96);
    int w = colW * 2 + pad * 2, hgt = perCol * rowH + pad * 2 + rowH * 3;
    RECT panel{ (g_scrW - w) / 2, (g_scrH - hgt) / 2, (g_scrW + w) / 2, (g_scrH + hgt) / 2 };
    HBRUSH bg = CreateSolidBrush(RGB(24, 24, 26));
    FillRect(mem, &panel, bg);
    DeleteObject(bg);
    HBRUSH fr = CreateSolidBrush(RGB(110, 110, 115));
    FrameRect(mem, &panel, fr);
    DeleteObject(fr);
    SetTextColor(mem, RGB(235, 235, 235));
    RECT tr{ panel.left + pad, panel.top + MulDiv(8, g_dpi, 96), panel.right - pad, panel.top + pad + rowH };
    {
        wchar_t title[128];
        std::wstring pk = keysOfAction(A_PREFS);
        swprintf(title, 128, L"Keyboard shortcuts \x2014 rebind in Preferences (%ls)",
                 pk.empty() ? L"toolbar" : pk.c_str());
        DrawTextW(mem, title, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    for (int i = 0; i < nRows; i++) {
        int col = i / perCol, rw = i % perCol;
        int x = panel.left + pad + col * colW;
        int y = panel.top + pad + rowH + rw * rowH;
        std::wstring keys;
        for (auto& kv : g_keymap) {
            if (kv.second != kActRows[i].a) continue;
            if (kv.first >= VK_NUMPAD0 && kv.first <= VK_NUMPAD9) continue;  // auto twins
            if (!keys.empty()) keys += L", ";
            keys += vkName(kv.first);
        }
        if (keys.empty()) keys = L"\x2014";
        SetTextColor(mem, RGB(255, 200, 40));
        RECT kr{ x, y, x + MulDiv(110, g_dpi, 96), y + rowH };
        DrawTextW(mem, keys.c_str(), -1, &kr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SetTextColor(mem, RGB(210, 210, 210));
        RECT lr{ kr.right + MulDiv(6, g_dpi, 96), y, x + colW - MulDiv(6, g_dpi, 96), y + rowH };
        DrawTextW(mem, kActRows[i].label, -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    SetTextColor(mem, RGB(150, 150, 150));
    RECT fn{ panel.left + pad, panel.bottom - pad - rowH * 2, panel.right - pad, panel.bottom - pad };
    DrawTextW(mem, L"Reject (\x2716) = xmp:Rating \"-1\" \x2014 Bridge/Photo Mechanic only;\n"
                   L"Lightroom ignores XMP rejects (its flags are catalog-only)",
              -1, &fn, DT_CENTER | DT_WORDBREAK);
}

// prefs GUI: chips rebuilt each paint. kind 0=advance 1=ext 2=rebind 3=close 4=showinfo 5=barpos
struct PChip { RECT rc; int kind; int val; };
static std::vector<PChip> g_pchips;
static RECT g_prefsRc{};
static const wchar_t* kExtList[11] = { L".arw", L".cr2", L".cr3", L".nef", L".nrw",
                                       L".raf", L".rw2", L".pef", L".dng", L".orf", L".jpg" };

static bool extEnabled(const wchar_t* e) {
    for (auto& x : g_exts) if (x == e) return true;
    return false;
}

static void paintPrefs(HDC mem) {
    const int perCol = (kNumActRows + 1) / 2;
    int rowH = MulDiv(26, g_dpi, 96);
    int colW = MulDiv(360, g_dpi, 96);
    int pad = MulDiv(20, g_dpi, 96);
    int optH = MulDiv(36, g_dpi, 96);
    int w = colW * 2 + pad * 2;
    int hgt = pad * 2 + optH * 4 + rowH * (perCol + 3) + MulDiv(40, g_dpi, 96);
    RECT panel{ (g_scrW - w) / 2, std::max(MulDiv(20, g_dpi, 96), (g_scrH - hgt) / 2),
                (g_scrW + w) / 2, std::max(MulDiv(20, g_dpi, 96), (g_scrH - hgt) / 2) + hgt };
    g_prefsRc = panel;
    g_pchips.clear();
    HBRUSH bg = CreateSolidBrush(RGB(24, 24, 26));
    FillRect(mem, &panel, bg);
    DeleteObject(bg);
    HBRUSH fr = CreateSolidBrush(RGB(110, 110, 115));
    FrameRect(mem, &panel, fr);
    DeleteObject(fr);
    HFONT small = CreateFontW(-MulDiv(14, g_dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Segoe UI");
    HFONT oldF = (HFONT)SelectObject(mem, small);
    int chipH = MulDiv(26, g_dpi, 96);
    int y = panel.top + MulDiv(12, g_dpi, 96);

    SetTextColor(mem, RGB(235, 235, 235));
    RECT tr{ panel.left, y, panel.right, y + chipH };
    DrawTextW(mem, L"Preferences \x2014 changes apply and save immediately", -1, &tr,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    y += optH;

    auto rowLabel = [&](const wchar_t* txt, int yy) {
        SetTextColor(mem, RGB(180, 180, 180));
        RECT lr{ panel.left + pad, yy, panel.left + pad + MulDiv(120, g_dpi, 96), yy + chipH };
        DrawTextW(mem, txt, -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return lr.right + MulDiv(6, g_dpi, 96);
    };
    // advance mode
    int x = rowLabel(L"After rating", y);
    const wchar_t* advLabels[3] = { L"Caps Lock gated", L"Always advance", L"Never" };
    for (int i = 0; i < 3; i++) {
        RECT rc{ x, y, x + MulDiv(140, g_dpi, 96), y + chipH };
        drawBtn(mem, rc, advLabels[i], g_advanceMode == i);
        g_pchips.push_back({ rc, 0, i });
        x = rc.right + MulDiv(8, g_dpi, 96);
    }
    y += optH;
    // extensions
    x = rowLabel(L"File types", y);
    for (int i = 0; i < 11; i++) {
        RECT rc{ x, y, x + MulDiv(56, g_dpi, 96), y + chipH };
        drawBtn(mem, rc, kExtList[i], extEnabled(kExtList[i]));
        g_pchips.push_back({ rc, 1, i });
        x = rc.right + MulDiv(5, g_dpi, 96);
    }
    y += optH;
    // info bar
    x = rowLabel(L"Info bar", y);
    RECT ib1{ x, y, x + MulDiv(100, g_dpi, 96), y + chipH };
    drawBtn(mem, ib1, g_showInfo ? L"Shown" : L"Hidden", g_showInfo);
    g_pchips.push_back({ ib1, 4, 0 });
    RECT ib2{ ib1.right + MulDiv(8, g_dpi, 96), y, ib1.right + MulDiv(108, g_dpi, 96), y + chipH };
    drawBtn(mem, ib2, g_infoTop ? L"Top" : L"Bottom", false);
    g_pchips.push_back({ ib2, 5, 0 });
    SetTextColor(mem, RGB(180, 180, 180));
    RECT rl{ ib2.right + MulDiv(24, g_dpi, 96), y, ib2.right + MulDiv(110, g_dpi, 96), y + chipH };
    DrawTextW(mem, L"Resume info", -1, &rl, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT rb{ rl.right + MulDiv(6, g_dpi, 96), y, rl.right + MulDiv(150, g_dpi, 96), y + chipH };
    drawBtn(mem, rb, g_sessionInFolder ? L"In photo folder" : L"Central ini", g_sessionInFolder);
    g_pchips.push_back({ rb, 6, 0 });
    {
        wchar_t cc[64];
        double mb = tcSizeBytes() / (1024.0 * 1024.0);
        swprintf(cc, 64, mb >= 1024.0 ? L"Clear thumb cache (%.1f GB)" : L"Clear thumb cache (%.0f MB)",
                 mb >= 1024.0 ? mb / 1024.0 : mb);
        RECT cb2{ rb.right + MulDiv(24, g_dpi, 96), y, rb.right + MulDiv(24 + 210, g_dpi, 96), y + chipH };
        drawBtn(mem, cb2, cc, false);
        g_pchips.push_back({ cb2, 7, 0 });
    }
    y += optH;
    x = rowLabel(L"RAW+JPG", y);
    {
        RECT pj{ x, y, x + MulDiv(120, g_dpi, 96), y + chipH };
        drawBtn(mem, pj, g_pairRawJpg ? L"Paired (+JPG)" : L"Separate", g_pairRawJpg);
        g_pchips.push_back({ pj, 8, 0 });
        SetTextColor(mem, RGB(150, 150, 150));
        RECT nt{ pj.right + MulDiv(16, g_dpi, 96), y, panel.right - pad, y + chipH };
        DrawTextW(mem, L"Note: Reject (\x2716) writes xmp:Rating=\"-1\" \x2014 read by Bridge/Photo"
                       L" Mechanic; Lightroom ignores it (its flags are catalog-only)",
                  -1, &nt, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    y += optH;

    SetTextColor(mem, RGB(180, 180, 180));
    RECT kh{ panel.left + pad, y, panel.right - pad, y + rowH };
    DrawTextW(mem, g_prefsCapture >= 0 ? L"Press the new key\x2026  (Esc cancels)"
                                       : L"Keys \x2014 click an action, then press its new key:",
              -1, &kh, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    y += rowH + MulDiv(2, g_dpi, 96);

    for (int i = 0; i < kNumActRows; i++) {
        int col = i / perCol, rw = i % perCol;
        int rx = panel.left + pad + col * colW;
        int ry = y + rw * rowH;
        RECT rowRc{ rx, ry, rx + colW - MulDiv(12, g_dpi, 96), ry + rowH - 2 };
        bool capturing = g_prefsCapture == (int)kActRows[i].a;
        if (capturing) {
            HBRUSH cb = CreateSolidBrush(RGB(70, 110, 180));
            FillRect(mem, &rowRc, cb);
            DeleteObject(cb);
        }
        std::wstring keys = keysOfAction(kActRows[i].a);
        if (keys.empty()) keys = L"\x2014";
        SetTextColor(mem, RGB(255, 200, 40));
        RECT kr{ rx + MulDiv(4, g_dpi, 96), ry, rx + MulDiv(120, g_dpi, 96), ry + rowH };
        DrawTextW(mem, keys.c_str(), -1, &kr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SetTextColor(mem, RGB(210, 210, 210));
        RECT lr{ kr.right + MulDiv(6, g_dpi, 96), ry, rowRc.right, ry + rowH };
        DrawTextW(mem, kActRows[i].label, -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        g_pchips.push_back({ rowRc, 2, (int)kActRows[i].a });
    }
    y += perCol * rowH + MulDiv(8, g_dpi, 96);
    RECT xb{ panel.right - pad - MulDiv(90, g_dpi, 96), y, panel.right - pad, y + chipH };
    drawBtn(mem, xb, L"Close (Esc)", false);
    g_pchips.push_back({ xb, 3, 0 });
    SelectObject(mem, oldF);
    DeleteObject(small);
}

static void paint(HDC dc) {
    RECT rc{ 0, 0, g_scrW, g_scrH };
    // backbuffer
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP back = CreateCompatibleBitmap(dc, g_scrW, g_scrH);
    HBITMAP oldBack = (HBITMAP)SelectObject(mem, back);
    FillRect(mem, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));

    int fontH = MulDiv(17, g_dpi, 96);
    HFONT font = CreateFontW(-fontH, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Segoe UI");
    HFONT oldFont = (HFONT)SelectObject(mem, font);
    SetBkMode(mem, TRANSPARENT);

    int n = (int)g_view.size();
    if (g_scanning) {
        SetTextColor(mem, RGB(200, 200, 200));
        DrawTextW(mem, L"Scanning folder\x2026", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else if (g_gridMode) {
        paintGrid(mem);
    } else if (n == 0) {
        SetTextColor(mem, RGB(200, 200, 200));
        std::wstring msg = g_items.empty()
            ? L"No matching files found in\n" + g_folder
            : L"No images match the filter \x2014 press F to adjust";
        RECT r2 = rc;
        DrawTextW(mem, msg.c_str(), -1, &r2, DT_CENTER | DT_VCENTER);
    } else {
        Slot slot; bool have = false;
        {
            std::lock_guard<std::mutex> lk(g_cacheMx);
            auto it = g_cache.find(curItem());
            if (it != g_cache.end()) { slot = it->second; have = true; }
        }
        Item& item = g_items[g_view[g_cur]];
        std::wstring fname = item.path;
        size_t sl = fname.find_last_of(L"\\/");
        if (sl != std::wstring::npos) fname = fname.substr(sl + 1);
        if (item.jpgTwin) fname += L" +JPG";

        bool zoomDrawn = false;
        if (g_zoomWanted) {
            std::lock_guard<std::mutex> lk(g_zoomMx);
            if (g_zoomIdx == curItem() && g_zoomBmp) {
                HDC img = CreateCompatibleDC(mem);
                HBITMAP oldImg = (HBITMAP)SelectObject(img, g_zoomBmp);
                int zw = g_zoomW, zh = g_zoomH;
                int dx = 0, dy = 0, sx = 0, sy = 0, w2 = zw, h2 = zh;
                if (zw <= g_scrW) dx = (g_scrW - zw) / 2;
                else {
                    double fx = (double)g_mouseX / (g_scrW > 1 ? g_scrW - 1 : 1);
                    fx = fx < 0 ? 0 : fx > 1 ? 1 : fx;
                    sx = (int)(fx * (zw - g_scrW)); w2 = g_scrW;
                }
                if (zh <= g_scrH) dy = (g_scrH - zh) / 2;
                else {
                    double fy = (double)g_mouseY / (g_scrH > 1 ? g_scrH - 1 : 1);
                    fy = fy < 0 ? 0 : fy > 1 ? 1 : fy;
                    sy = (int)(fy * (zh - g_scrH)); h2 = g_scrH;
                }
                BitBlt(mem, dx, dy, w2, h2, img, sx, sy, SRCCOPY);
                SelectObject(img, oldImg);
                DeleteDC(img);
                zoomDrawn = true;
            }
        }
        bool drewPrev = false;
        if (zoomDrawn) {
        } else if (have && slot.bmp) {
            HDC img = CreateCompatibleDC(mem);
            HBITMAP oldImg = (HBITMAP)SelectObject(img, slot.bmp);
            int x = (g_scrW - slot.w) / 2, y = (g_scrH - slot.h) / 2;
            BitBlt(mem, x, y, slot.w, slot.h, img, 0, 0, SRCCOPY);
            SelectObject(img, oldImg);
            DeleteDC(img);
            g_lastPaintedItem = curItem();
        } else {
            if (!(have && slot.failed) && g_lastPaintedItem >= 0) {
                Slot prev;
                std::lock_guard<std::mutex> lk(g_cacheMx);
                auto pf = g_cache.find(g_lastPaintedItem);
                if (pf != g_cache.end() && pf->second.bmp) {
                    HDC img = CreateCompatibleDC(mem);
                    HBITMAP oldImg = (HBITMAP)SelectObject(img, pf->second.bmp);
                    int x = (g_scrW - pf->second.w) / 2, y = (g_scrH - pf->second.h) / 2;
                    BitBlt(mem, x, y, pf->second.w, pf->second.h, img, 0, 0, SRCCOPY);
                    SelectObject(img, oldImg);
                    DeleteDC(img);
                    drewPrev = true;
                }
            }
            if (!drewPrev) {
                SetTextColor(mem, RGB(150, 150, 150));
                std::wstring msg = have && slot.failed ? (fname + L"\n" + slot.err)
                                                       : (fname + L"\nloading\x2026");
                RECT r2 = rc;
                DrawTextW(mem, msg.c_str(), -1, &r2, DT_CENTER | DT_VCENTER);
            }
        }

        if (g_zoomWanted) {
            SetTextColor(mem, RGB(255, 255, 255));
            int zoff = g_infoTop && g_showInfo ? MulDiv(34, g_dpi, 96) : 0;
            RECT zr{ MulDiv(12, g_dpi, 96), zoff + MulDiv(10, g_dpi, 96), g_scrW, zoff + MulDiv(44, g_dpi, 96) };
            DrawTextW(mem, zoomDrawn ? L"100%" : L"100% loading\x2026", -1, &zr,
                      DT_LEFT | DT_TOP | DT_SINGLELINE);
        }

        if (g_showInfo) {
            int barH = MulDiv(34, g_dpi, 96);
            RECT bar = g_infoTop ? RECT{ 0, 0, g_scrW, barH }
                                 : RECT{ 0, g_scrH - barH, g_scrW, g_scrH };
            // translucent black strip
            HDC bdc = CreateCompatibleDC(mem);
            HBITMAP bbm = CreateCompatibleBitmap(mem, 1, 1);
            HBITMAP oldb = (HBITMAP)SelectObject(bdc, bbm);
            SetPixel(bdc, 0, 0, RGB(0, 0, 0));
            BLENDFUNCTION bf{ AC_SRC_OVER, 0, 175, 0 };
            AlphaBlend(mem, 0, bar.top, g_scrW, barH, bdc, 0, 0, 1, 1, bf);
            SelectObject(bdc, oldb);
            DeleteObject(bbm);
            DeleteDC(bdc);

            RawExif& x = slot.exif;
            wchar_t left[640];
            std::wstring sh = utf8ToWide(fmtShutter(x.expNum, x.expDen));
            std::wstring ap = utf8ToWide(fmtAperture(x.fnNum, x.fnDen));
            std::wstring fo = utf8ToWide(fmtFocal(x.flNum, x.flDen));
            std::wstring ev = utf8ToWide(fmtEv(x.evNum, x.evDen, x.hasEv));
            std::wstring dt = utf8ToWide(x.dateTime);
            std::wstring cam = utf8ToWide(x.model);
            std::wstring lens = utf8ToWide(x.lens);
            std::wstring exifStr;
            auto add = [&](const std::wstring& s) { if (!s.empty()) { if (!exifStr.empty()) exifStr += L"   "; exifStr += s; } };
            add(sh); add(ap);
            if (x.iso) add(L"ISO " + std::to_wstring(x.iso));
            add(fo); add(ev);
            if (have && !slot.failed) {
                std::wstring extra;
                if (!dt.empty()) extra = dt;
                if (!cam.empty()) extra += (extra.empty() ? L"" : L"   ") + cam;
                if (!lens.empty()) extra += (extra.empty() ? L"" : L"   ") + lens;
                if (!extra.empty()) add(L"|   " + extra);
            }
            const wchar_t* sortNames[3] = { L"Name", L"Date", L"Size" };
            wchar_t cnt[64];
            if (g_filter.active())
                swprintf(cnt, 64, L"%d / %d (of %d)", g_cur + 1, n, (int)g_items.size());
            else
                swprintf(cnt, 64, L"%d / %d", g_cur + 1, n);
            swprintf(left, 640, L"%ls   %ls   [%ls %ls]      %ls", cnt,
                     fname.c_str(), sortNames[g_sortType], g_sortDesc ? L"\x2193" : L"\x2191",
                     exifStr.c_str());

            int pad = MulDiv(12, g_dpi, 96);
            RECT tr{ pad, bar.top, g_scrW - pad, bar.bottom };
            SetTextColor(mem, RGB(235, 235, 235));
            DrawTextW(mem, left, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            // right side: stars (or reject cross) + label
            std::wstring right;
            bool rejected = item.rating == -1;
            if (rejected) right = L"\x2716 Rejected";
            else {
                int rating = item.rating < 0 ? 0 : item.rating;
                for (int i = 0; i < 5; i++) right += (i < rating) ? L"\x2605" : L"\x2606";
            }
            RECT rr{ pad, bar.top, g_scrW - pad, bar.bottom };
            int lbl = item.label < 0 ? 0 : item.label;
            if (lbl) {
                std::wstring ltxt = std::wstring(L"\x25CF ") + labelNameW(lbl) + L"   ";
                SIZE ssz;
                GetTextExtentPoint32W(mem, (right).c_str(), (int)right.size(), &ssz);
                RECT lr{ pad, bar.top, g_scrW - pad - ssz.cx, bar.bottom };
                SetTextColor(mem, labelColor(lbl));
                DrawTextW(mem, ltxt.c_str(), -1, &lr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            }
            SetTextColor(mem, rejected ? RGB(235, 70, 60) : RGB(255, 200, 40));
            DrawTextW(mem, right.c_str(), -1, &rr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
    }

    if (g_filterOpen && !g_scanning) paintFilterPanel(mem);
    if (g_prefsOpen && !g_scanning) paintPrefs(mem);
    if (g_helpOpen) paintHelp(mem);

    SelectObject(mem, oldFont);
    DeleteObject(font);
    BitBlt(dc, 0, 0, g_scrW, g_scrH, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBack);
    DeleteObject(back);
    DeleteDC(mem);
}

// ------------------------------------------------------------------ actions
static void doAction(Action a) {
    switch (a) {
    case A_QUIT: PostQuitMessage(0); return;
    case A_TOGGLE_INFO: g_showInfo = !g_showInfo; InvalidateRect(g_hwnd, nullptr, FALSE); return;
    case A_INFOPOS: g_infoTop = !g_infoTop; InvalidateRect(g_hwnd, nullptr, FALSE); return;
    case A_HELP: g_helpOpen = !g_helpOpen; InvalidateRect(g_hwnd, nullptr, FALSE); return;
    case A_PREFS:
        g_prefsOpen = !g_prefsOpen;
        g_prefsCapture = -1;
        if (g_prefsOpen && g_filterOpen) closeFilterPanel();
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    case A_FILTER:
        if (g_scanning) return;
        if (!g_gridMode) {
            g_gridMode = true;
            ShowScrollBar(g_hwnd, SB_VERT, TRUE);
            if (g_zoomWanted || g_zoomIdx >= 0) resetZoom();
            gridEnsureVisible();
            if (!g_filterOpen) toggleFilterPanel(); else InvalidateRect(g_hwnd, nullptr, FALSE);
        } else toggleFilterPanel();
        return;
    case A_GRID:
        if (g_scanning) return;
        g_gridMode = !g_gridMode;
        ShowScrollBar(g_hwnd, SB_VERT, g_gridMode);
        if (!g_gridMode) {
            if (g_filterOpen) closeFilterPanel();
            g_prefsOpen = false;
            g_prefsCapture = -1;
        }
        if (g_zoomWanted || g_zoomIdx >= 0) resetZoom();
        if (g_gridMode) gridEnsureVisible();
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    case A_SORTNEXT: case A_SORTPREV:
        if (g_scanning) return;
        g_sortType = (g_sortType + (a == A_SORTNEXT ? 1 : 2)) % 3;
        applySort();
        return;
    case A_SORTDIR:
        if (g_scanning) return;
        g_sortDesc = !g_sortDesc;
        applySort();
        return;
    default: break;
    }
    int n = (int)g_view.size();
    if (g_scanning || n == 0) return;
    Item& it = g_items[g_view[g_cur]];
    switch (a) {
    case A_NEXT: setCurrent(g_cur + 1); break;
    case A_PREV: setCurrent(g_cur - 1); break;
    case A_FIRST: setCurrent(0); break;
    case A_LAST: setCurrent(n - 1); break;
    case A_ZOOM: {
        if (g_gridMode) break;
        if (g_zoomWanted) {
            resetZoom();
        } else {
            g_zoomWanted = true;
            POINT cp;
            if (GetCursorPos(&cp)) { g_mouseX = cp.x; g_mouseY = cp.y; }
            bool ready;
            { std::lock_guard<std::mutex> lk(g_zoomMx); ready = (g_zoomIdx == curItem() && g_zoomBmp != nullptr); }
            if (!ready) requestZoom(curItem());
        }
        InvalidateRect(g_hwnd, nullptr, FALSE);
        break; }
    case A_RATE0: case A_RATE1: case A_RATE2: case A_RATE3: case A_RATE4: case A_RATE5: {
        it.rating = a - A_RATE0;
        queueSidecarSave(g_view[g_cur], it.path, it.rating,
                         it.label < 0 ? kXmpKeep : it.label);
        if (g_filter.active() && !passesFilter(it)) {
            rebuildView(true);
            setCurrent(g_cur);
            break;
        }
        bool adv = g_advanceMode == 1 ||
                   (g_advanceMode == 0 && (GetKeyState(VK_CAPITAL) & 1) != 0);
        if (adv && g_cur < n - 1) setCurrent(g_cur + 1);
        else InvalidateRect(g_hwnd, nullptr, FALSE);
        break; }
    case A_REJECT: {
        it.rating = (it.rating == -1) ? 0 : -1;
        queueSidecarSave(g_view[g_cur], it.path, it.rating,
                         it.label < 0 ? kXmpKeep : it.label);
        if (g_filter.active() && !passesFilter(it)) {
            rebuildView(true);
            setCurrent(g_cur);
            break;
        }
        bool advR = g_advanceMode == 1 ||
                    (g_advanceMode == 0 && (GetKeyState(VK_CAPITAL) & 1) != 0);
        if (advR && it.rating == -1 && g_cur < n - 1) setCurrent(g_cur + 1);
        else InvalidateRect(g_hwnd, nullptr, FALSE);
        break; }
    case A_LABEL_RED: case A_LABEL_YELLOW: case A_LABEL_GREEN:
    case A_LABEL_BLUE: case A_LABEL_PURPLE: {
        int l = a - A_LABEL_RED + 1;
        it.label = (it.label == l) ? 0 : l;      // toggle like Lightroom
        queueSidecarSave(g_view[g_cur], it.path,
                         it.rating == -2 ? kXmpKeep : it.rating, it.label);
        if (g_filter.active() && !passesFilter(it)) {
            rebuildView(true);
            setCurrent(g_cur);
            break;
        }
        InvalidateRect(g_hwnd, nullptr, FALSE);
        break; }
    default: break;
    }
}

// ------------------------------------------------------------------ wndproc
static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        ULONGLONG t0 = GetTickCount64();
        paint(dc);
        ULONGLONG ms = GetTickCount64() - t0;
        if (ms > 30) logLine("SLOW paint %llums grid=%d", (unsigned long long)ms, g_gridMode ? 1 : 0);
        EndPaint(h, &ps);
        return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_KEYDOWN: {
        if (g_prefsOpen) {
            if (g_prefsCapture >= 0) {
                if (w == VK_ESCAPE) { g_prefsCapture = -1; InvalidateRect(h, nullptr, FALSE); return 0; }
                if (w == VK_SHIFT || w == VK_CONTROL || w == VK_MENU) return 0;
                UINT cap = (UINT)w;
                if (GetKeyState(VK_SHIFT) & 0x8000) cap |= KM_SHIFT;
                if (vkName(cap) != L"?") {
                    rebindAction((Action)g_prefsCapture, cap);
                    g_prefsCapture = -1;
                    InvalidateRect(h, nullptr, FALSE);
                }
                return 0;
            }
            if (w == VK_ESCAPE) { g_prefsOpen = false; InvalidateRect(h, nullptr, FALSE); }
            return 0;
        }
        if (g_helpOpen) {
            if (w == VK_ESCAPE || actionForKey((UINT)w) == A_HELP) {
                g_helpOpen = false;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        if (g_filterOpen) {
            UINT vk = (UINT)w;
            if (vk == VK_ESCAPE || actionForKey(vk) == A_FILTER) { closeFilterPanel(); return 0; }
            int digit = -1;
            if (vk >= '0' && vk <= '9') digit = vk - '0';
            else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) digit = vk - VK_NUMPAD0;
            if (digit >= 0 && digit <= 5) { g_filter.ratingMask ^= 1u << digit; filterChanged(); return 0; }
            if (digit >= 6 && digit <= 9) { g_filter.labelMask ^= 1u << (digit - 5); filterChanged(); return 0; }
            if (vk == 'P') { g_filter.labelMask ^= 1u << 5; filterChanged(); return 0; }
            if (vk == 'X') { g_filter.ratingMask ^= 1u << 6; filterChanged(); return 0; }
            if (vk == 'N') { g_filter.labelMask ^= 1u; filterChanged(); return 0; }
            if (vk == 'C') { g_filter = Filter(); if (g_filterEdit) SetWindowTextW(g_filterEdit, L""); filterChanged(); return 0; }
            return 0;
        }
        if (g_gridMode) {
            switch (w) {
            case VK_UP:    setCurrent(g_cur - g_gridCols); return 0;
            case VK_DOWN:  setCurrent(g_cur + g_gridCols); return 0;
            case VK_PRIOR: setCurrent(g_cur - g_gridCols * g_gridRowsVis); return 0;
            case VK_NEXT:  setCurrent(g_cur + g_gridCols * g_gridRowsVis); return 0;
            case VK_RETURN:
                g_gridMode = false;
                ShowScrollBar(h, SB_VERT, FALSE);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
        }
        Action act = actionForKey((UINT)w);
        if (act != A_NONE) {
            // held-down arrows: dynamically match repeat rate to decode speed
            if ((l & (1 << 30)) && !g_gridMode && (act == A_NEXT || act == A_PREV)) {
                std::lock_guard<std::mutex> lk(g_cacheMx);
                auto c = g_cache.find(curItem());
                if (c == g_cache.end() || (!c->second.bmp && !c->second.failed)) {
                    logLine("swallow item=%d", curItem());
                    return 0;                      // swallow this repeat, keep frame up
                }
            }
            doAction(act);
        }
        return 0; }
    case WM_COMMAND:
        if (LOWORD(w) == 100 && HIWORD(w) == EN_CHANGE && g_filterEdit) {
            wchar_t buf[256];
            GetWindowTextW(g_filterEdit, buf, 256);
            g_filter.text = buf;
            filterChanged();
        }
        return 0;
    case WM_VSCROLL: {
        if (!g_gridMode) return 0;
        SCROLLINFO si{ sizeof(si) };
        si.fMask = SIF_ALL;
        GetScrollInfo(h, SB_VERT, &si);
        int pos = g_gridScroll;
        switch (LOWORD(w)) {
        case SB_LINEUP: pos--; break;
        case SB_LINEDOWN: pos++; break;
        case SB_PAGEUP: pos -= (int)si.nPage; break;
        case SB_PAGEDOWN: pos += (int)si.nPage; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = si.nMax; break;
        }
        g_gridScroll = std::max(0, pos);
        InvalidateRect(h, nullptr, FALSE);
        return 0; }
    case WM_MOUSEWHEEL:
        if (g_gridMode) {
            g_gridScroll += GET_WHEEL_DELTA_WPARAM(w) > 0 ? -1 : 1;
            if (g_gridScroll < 0) g_gridScroll = 0;
            InvalidateRect(h, nullptr, FALSE);
        } else {
            doAction(GET_WHEEL_DELTA_WPARAM(w) > 0 ? A_PREV : A_NEXT);
        }
        return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        int mx = (int)(short)LOWORD(l), my = (int)(short)HIWORD(l);
        POINT pt{ mx, my };
        if (g_prefsOpen) {
            if (PtInRect(&g_prefsRc, pt)) {
                for (auto& ch : g_pchips) {
                    if (!PtInRect(&ch.rc, pt)) continue;
                    switch (ch.kind) {
                    case 0:
                        g_advanceMode = ch.val;
                        WritePrivateProfileStringW(L"options", L"autoadvance",
                            ch.val == 1 ? L"always" : ch.val == 2 ? L"never" : L"capslock", g_iniPath.c_str());
                        break;
                    case 1: {
                        std::wstring e = kExtList[ch.val];
                        if (extEnabled(e.c_str())) {
                            if (g_exts.size() > 1)
                                g_exts.erase(std::find(g_exts.begin(), g_exts.end(), e));
                        } else g_exts.push_back(e);
                        std::wstring joined;
                        for (auto& x : g_exts) { if (!joined.empty()) joined += L";"; joined += x; }
                        WritePrivateProfileStringW(L"options", L"extensions", joined.c_str(), g_iniPath.c_str());
                        clearDecodes();
                        clearThumbs();
                        rescanFolder();
                        break; }
                    case 2: g_prefsCapture = ch.val; break;
                    case 3: g_prefsOpen = false; g_prefsCapture = -1; break;
                    case 4:
                        g_showInfo = !g_showInfo;
                        WritePrivateProfileStringW(L"options", L"showinfo", g_showInfo ? L"1" : L"0", g_iniPath.c_str());
                        break;
                    case 5:
                        g_infoTop = !g_infoTop;
                        WritePrivateProfileStringW(L"state", L"infotop", g_infoTop ? L"1" : L"0", g_iniPath.c_str());
                        break;
                    case 6:
                        g_sessionInFolder = !g_sessionInFolder;
                        WritePrivateProfileStringW(L"options", L"sessioninfolder",
                            g_sessionInFolder ? L"1" : L"0", g_iniPath.c_str());
                        tcInit();   // switch cache location live
                        break;
                    case 7:
                        tcClear();
                        clearThumbs();
                        preloadThumbs();   // start re-making thumbs immediately
                        break;
                    case 8:
                        g_pairRawJpg = !g_pairRawJpg;
                        WritePrivateProfileStringW(L"options", L"pairrawjpg",
                            g_pairRawJpg ? L"1" : L"0", g_iniPath.c_str());
                        clearDecodes();
                        clearThumbs();
                        rescanFolder();
                        break;
                    }
                    InvalidateRect(h, nullptr, FALSE);
                    return 0;
                }
            } else { g_prefsOpen = false; g_prefsCapture = -1; InvalidateRect(h, nullptr, FALSE); }
            return 0;
        }
        if (g_filterOpen && PtInRect(&g_panelRc, pt)) {
            for (auto& ch : g_chips) {
                if (PtInRect(&ch.rc, pt)) {
                    if (ch.kind == 0) g_filter.ratingMask ^= 1u << ch.val;
                    else if (ch.kind == 1) g_filter.labelMask ^= 1u << ch.val;
                    else if (ch.kind == 2) { g_filter = Filter(); if (g_filterEdit) SetWindowTextW(g_filterEdit, L""); }
                    else { closeFilterPanel(); return 0; }
                    filterChanged();
                    return 0;
                }
            }
            return 0;
        }
        if (g_gridMode) {
            for (auto& b : g_tbBtns) {
                if (PtInRect(&b.rc, pt)) {
                    switch (b.id) {
                    case 1:
                        if (g_filterOpen) closeFilterPanel();
                        g_prefsOpen = true;
                        break;
                    case 2: doAction(A_FILTER); break;
                    case 3: if (g_sortType != 0) { g_sortType = 0; applySort(); } break;
                    case 4: if (g_sortType != 1) { g_sortType = 1; applySort(); } break;
                    case 5: if (g_sortType != 2) { g_sortType = 2; applySort(); } break;
                    case 6: doAction(A_SORTDIR); break;
                    case 7: doAction(A_GRID); break;
                    case 8: doAction(A_FIRST); break;
                    case 9: doAction(A_LAST); break;
                    }
                    return 0;
                }
            }
            if (my > g_gridTbH && !g_view.empty()) {
                int c = (mx - g_gridX0) / g_cellW, r = (my - g_gridTbH) / g_cellH;
                if (c >= 0 && c < g_gridCols) {
                    int pos = (g_gridScroll + r) * g_gridCols + c;
                    if (pos >= 0 && pos < (int)g_view.size()) {
                        setCurrent(pos);
                        if (m == WM_LBUTTONDBLCLK) {
                            g_gridMode = false;
                            ShowScrollBar(h, SB_VERT, FALSE);
                            InvalidateRect(h, nullptr, FALSE);
                        }
                    }
                }
            }
        }
        return 0; }
    case WM_MOUSEMOVE:
        g_mouseX = (int)(short)LOWORD(l);
        g_mouseY = (int)(short)HIWORD(l);
        if (g_zoomWanted) InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_APP_ZOOMED:
        if (g_zoomWanted && (int)w == curItem()) InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_APP_DECODED:
        if ((int)w == curItem()) InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_APP_THUMB:
        if (g_gridMode) InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_APP_SIDECARS:
        rebuildView(true);
        if (g_filterOpen || g_filter.active()) InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_APP_SIDECAR1:
        if (g_gridMode || (int)w == curItem()) InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_APP_SCANDONE: {
        rebuildView(false);
        int start = 0;
        std::wstring lastFile;
        if (g_sessionInFolder) {
            std::wstring rel = sesGet(L"lastfile", L"");
            if (!rel.empty()) lastFile = g_folder + L"\\" + rel;
        }
        if (lastFile.empty()) lastFile = iniGet(L"state", L"lastfile", L"");
        if (!lastFile.empty()) {
            for (int q = 0; q < (int)g_view.size(); q++)
                if (g_items[g_view[q]].path == lastFile) { start = q; break; }
        }
        if (!g_view.empty()) { g_cur = start; setCurrent(start); }
        preloadThumbs();
        InvalidateRect(h, nullptr, FALSE);
        return 0; }
    case WM_CTLCOLOREDIT: {
        HDC edc = (HDC)w;
        SetTextColor(edc, RGB(235, 235, 235));
        SetBkColor(edc, RGB(45, 45, 48));
        static HBRUSH s_editBr = CreateSolidBrush(RGB(45, 45, 48));
        return (LRESULT)s_editBr; }
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ------------------------------------------------------------------ folder pick
static bool pickFolder(std::wstring& out) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IFileOpenDialog, (void**)&dlg))) return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"iRate \x2014 choose the folder with your raws");
    std::wstring last = iniGet(L"state", L"lastfolder", L"");
    if (!last.empty()) {
        IShellItem* si = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(last.c_str(), nullptr, IID_IShellItem, (void**)&si))) {
            dlg->SetDefaultFolder(si);
            si->Release();
        }
    }
    bool ok = false;
    if (SUCCEEDED(dlg->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR psz = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz))) {
                out = psz;
                CoTaskMemFree(psz);
                ok = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

// ------------------------------------------------------------------ main
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int) {
    // Per-monitor DPI awareness (dynamic - works on Win7 too)
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    typedef BOOL(WINAPI* SetCtxFn)(HANDLE);
    if (auto f = (SetCtxFn)GetProcAddress(u32, "SetProcessDpiAwarenessContext"))
        f((HANDLE)-4 /*PER_MONITOR_AWARE_V2*/);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    loadConfig();

    // folder: command line arg, else picker
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc > 1) {
        g_folder = argv[1];
        DWORD attr = GetFileAttributesW(g_folder.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) g_folder.clear();
    }
    if (argv) LocalFree(argv);
    if (g_folder.empty() && !pickFolder(g_folder)) return 0;
    while (!g_folder.empty() && (g_folder.back() == L'\\' || g_folder.back() == L'/'))
        g_folder.pop_back();
    g_slowDrive = driveIsSlow(g_folder);
    logLine("start  folder=%ls  slowdrive=%d", g_folder.c_str(), g_slowDrive ? 1 : 0);
    WritePrivateProfileStringW(L"state", L"lastfolder", g_folder.c_str(), g_iniPath.c_str());
    {   // migrate a JustRate-era per-folder session file to the new name
        std::wstring oldSes = g_folder + L"\\justrate.session";
        if (GetFileAttributesW(sessionFile().c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(oldSes.c_str()) != INVALID_FILE_ATTRIBUTES)
            MoveFileW(oldSes.c_str(), sessionFile().c_str());
    }
    if (g_sessionInFolder && GetFileAttributesW(sessionFile().c_str()) != INVALID_FILE_ATTRIBUTES) {
        g_sortType = _wtoi(sesGet(L"sorttype", std::to_wstring(g_sortType).c_str()).c_str());
        if (g_sortType < 0 || g_sortType > 2) g_sortType = 0;
        g_sortDesc = sesGet(L"sortdesc", g_sortDesc ? L"1" : L"0") == L"1";
        g_gridMode = sesGet(L"gridmode", g_gridMode ? L"1" : L"0") == L"1";
    }
    tcInit();   // per-job cache lives in the chosen folder

    g_scrW = GetSystemMetrics(SM_CXSCREEN);
    g_scrH = GetSystemMetrics(SM_CYSCREEN);

    WNDCLASSW wc{};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"iRateWnd";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"iRate", WS_POPUP | WS_CLIPCHILDREN,
                             0, 0, g_scrW, g_scrH, nullptr, nullptr, hInst, nullptr);
    HDC dc = GetDC(g_hwnd);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(g_hwnd, dc);
    ShowScrollBar(g_hwnd, SB_VERT, g_gridMode);
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);

    std::thread scanner(scanThread);
    std::thread scWorker(sidecarThread);
    unsigned nWorkers = std::max(2u, std::min(4u, std::thread::hardware_concurrency() / 2));
    std::vector<std::thread> workers;
    // slow drive: worker 0 is the only one allowed to take low-priority
    // (thumbnail preload) jobs — one sequential reader, no seek storm — and the
    // rest serve the high queue exclusively so a fresh arrow-press decode never
    // waits behind background thumbnails. Fast drives: all workers take both.
    for (unsigned i = 0; i < nWorkers; i++)
        workers.emplace_back(workerThread, !g_slowDrive || i == 0);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // persist session state for resume
    {
        std::wstring sortS = std::to_wstring(g_sortType);
        if (g_sessionInFolder) {
            if (!g_scanning && curItem() >= 0) {
                std::wstring fp = g_items[curItem()].path, rel = fp;
                if (fp.size() > g_folder.size() + 1 &&
                    _wcsnicmp(fp.c_str(), g_folder.c_str(), g_folder.size()) == 0)
                    rel = fp.substr(g_folder.size() + 1);
                sesSet(L"lastfile", rel);
            }
            sesSet(L"sorttype", sortS);
            sesSet(L"sortdesc", g_sortDesc ? L"1" : L"0");
            sesSet(L"gridmode", g_gridMode ? L"1" : L"0");
        } else if (!g_scanning && curItem() >= 0) {
            WritePrivateProfileStringW(L"state", L"lastfile", g_items[curItem()].path.c_str(), g_iniPath.c_str());
        }
        WritePrivateProfileStringW(L"state", L"sorttype", sortS.c_str(), g_iniPath.c_str());
        WritePrivateProfileStringW(L"state", L"sortdesc", g_sortDesc ? L"1" : L"0", g_iniPath.c_str());
        WritePrivateProfileStringW(L"state", L"gridmode", g_gridMode ? L"1" : L"0", g_iniPath.c_str());
        WritePrivateProfileStringW(L"state", L"infotop", g_infoTop ? L"1" : L"0", g_iniPath.c_str());
        WritePrivateProfileStringW(L"options", L"showinfo", g_showInfo ? L"1" : L"0", g_iniPath.c_str());
    }

    g_quit = true;
    g_queueCv.notify_all();
    g_scCv.notify_all();
    scanner.join();
    for (auto& t : workers) t.join();
    scWorker.join();                   // flushes any still-pending sidecar writes
    {
        std::lock_guard<std::mutex> lk(g_cacheMx);
        for (auto& kv : g_cache) if (kv.second.bmp) DeleteObject(kv.second.bmp);
    }
    CoUninitialize();
    return 0;
}
