// main.mm - iRate macOS shell (AppKit + ImageIO), Milestone 1.
//
// A REWRITE of the Win32 shell (source/irate.cpp), not a translation. It shares
// the platform-clean parser + XMP logic in source/core.h verbatim. This first
// milestone covers the culling loop that matters most:
//   * macOS folder-permission flow (NSOpenPanel grant + security-scoped bookmark
//     so a re-launch keeps access to the same card/drive without re-picking),
//   * recursive scan of the chosen folder (skips *.lrdata like the Win32 build),
//   * ImageIO decode of the embedded preview core.h locates, with EXIF rotation
//     applied to MATERIALIZED pixels (the WIC-TRAP lesson from CLAUDE.md),
//   * fullscreen display + info bar, arrow/Home/End navigation,
//   * rate / label / reject writing XMP sidecars through core.h, all sidecar I/O
//     on a dedicated serial queue so the UI thread never touches the photo drive,
//   * held-arrow repeats swallowed while the current preview decodes (never black).
//
// Still to port (later milestones): grid mode, 100% zoom, filter, prefs/rebind
// UI, help overlay, thumb disk cache, drive-aware multi-worker scheduling, INI.
// The action table below is already the single source of truth for keybinds so
// the help/rebind UI can be generated from it (design principle #3).

#import <Cocoa/Cocoa.h>
#import <ImageIO/ImageIO.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <algorithm>
#include <atomic>
#include "../source/core.h"

// ---------------------------------------------------------------- byte source
// core.h reads through this; pread keeps it thread-safe for background decodes.
class MacByteSource : public ByteSource {
public:
    explicit MacByteSource(const std::string& path) {
        fd_ = open(path.c_str(), O_RDONLY);
        if (fd_ >= 0) { struct stat st; if (!fstat(fd_, &st)) sz_ = (uint64_t)st.st_size; }
    }
    ~MacByteSource() override { if (fd_ >= 0) close(fd_); }
    bool ok() const { return fd_ >= 0; }
    bool read(uint64_t off, void* dst, uint32_t n) override {
        if (fd_ < 0 || off + n > sz_) return false;
        return pread(fd_, dst, n, (off_t)off) == (ssize_t)n;
    }
    uint64_t size() override { return sz_; }
private:
    int fd_ = -1; uint64_t sz_ = 0;
};

// ------------------------------------------------------------------ items
struct Item {
    std::string path;
    uint64_t mtime = 0, fsize = 0;
    int rating = -2;   // -2 = sidecar not read yet, -1 = rejected, 0..5 stars
    int label = -1;
    bool jpgTwin = false;   // a same-name JPG sits beside this raw
    std::vector<std::string> keywords;   // dc:subject from the sidecar
};

// ------------------------------------------------------------------ actions
// Single table drives keybinding + (later) help overlay and rebind UI.
enum Action {
    A_NONE = 0, A_NEXT, A_PREV, A_FIRST, A_LAST,
    A_RATE0, A_RATE1, A_RATE2, A_RATE3, A_RATE4, A_RATE5,
    A_LABEL_RED, A_LABEL_YELLOW, A_LABEL_GREEN, A_LABEL_BLUE, A_LABEL_PURPLE,
    A_REJECT, A_SORTNEXT, A_SORTPREV, A_SORTDIR, A_GRID, A_ZOOM, A_FILTER,
    A_KEYWORDS, A_TOGGLE_INFO, A_INFOPOS, A_HELP, A_PREFS, A_QUIT,
};

// Single source of truth for keybinds: drives the loaded keymap AND the help
// overlay (design principle #3 — never hardcode a shortcut list twice). Defaults
// mirror source/irate.cpp's kDefaultIni; the ini can rebind any of them.
struct ActRow { Action a; const char* label; const char* iniName; const char* defKey; };
static const ActRow kActRows[] = {
    { A_NEXT,        "Next image",        "next",        "RIGHT,SPACE" },
    { A_PREV,        "Previous image",    "prev",        "LEFT" },
    { A_FIRST,       "First image",       "first",       "HOME" },
    { A_LAST,        "Last image",        "last",        "END" },
    { A_RATE1,       "Rate 1",            "rate1",       "1" },
    { A_RATE2,       "Rate 2",            "rate2",       "2" },
    { A_RATE3,       "Rate 3",            "rate3",       "3" },
    { A_RATE4,       "Rate 4",            "rate4",       "4" },
    { A_RATE5,       "Rate 5",            "rate5",       "5" },
    { A_RATE0,       "Clear rating",      "rate0",       "0" },
    { A_REJECT,      "Reject toggle",     "reject",      "X" },
    { A_LABEL_RED,   "Red label",         "labelred",    "6" },
    { A_LABEL_YELLOW,"Yellow label",      "labelyellow", "7" },
    { A_LABEL_GREEN, "Green label",       "labelgreen",  "8" },
    { A_LABEL_BLUE,  "Blue label",        "labelblue",   "9" },
    { A_LABEL_PURPLE,"Purple label",      "labelpurple", "P" },
    { A_SORTNEXT,    "Next sort mode",    "sortnext",    "RBRACKET" },
    { A_SORTPREV,    "Prev sort mode",    "sortprev",    "LBRACKET" },
    { A_SORTDIR,     "Sort direction",    "sortdir",     "SEMICOLON" },
    { A_GRID,        "Grid / fullscreen", "grid",        "G" },
    { A_ZOOM,        "Zoom 100% / back",  "zoom",        "SLASH" },
    { A_FILTER,      "Filter panel",      "filter",      "F" },
    { A_KEYWORDS,    "Keyword sets",      "keywordsets", "K" },
    { A_TOGGLE_INFO, "Info bar on/off",   "toggleinfo",  "I" },
    { A_INFOPOS,     "Info bar top/bottom","infopos",    "Z" },
    { A_HELP,        "Shortcut help",     "help",        "H" },
    { A_PREFS,       "Preferences",       "prefs",       "SHIFT+P" },
    { A_QUIT,        "Quit",              "quit",        "ESC" },
};
static const int kNumActRows = (int)(sizeof(kActRows) / sizeof(kActRows[0]));

// ------------------------------------------------------------------ globals
static std::vector<Item> g_items;
static std::vector<int> g_view;             // view position -> index into g_items
static int g_cur = 0;                       // position in g_view (NOT g_items)
// Filter: rating bits 0..5 = ratings 0..5, bit 6 = rejected (default all set);
// label bits 0..5 = none/red/yellow/green/blue/purple; plus a filename substring.
static unsigned g_ratingMask = 0x7F;
static unsigned g_labelMask = 0x3F;
static std::string g_filterText;
static bool g_filterOpen = false;           // filter panel visible
static bool g_showInfo = true;
static bool g_infoTop = false;              // info bar at top instead of bottom
static bool g_helpOpen = false;             // shortcut overlay visible
static bool g_prefsOpen = false;            // preferences / rebind overlay visible
static int g_prefsCaptureAction = -1;       // Action being rebound (mouse-picked), or -1
static NSString* g_prefsMsg = nil;          // transient rebind conflict message
static int g_advanceMode = 0;               // 0=capslock 1=always 2=never
static int g_sortType = 0;                  // 0=name 1=date 2=size
static bool g_sortDesc = false;
static bool g_pairRawJpg = true;            // collapse RAW+JPG twins to one entry
static bool g_sessionInFolder = true;       // resume file lives in the folder root
static std::string g_folderPath;            // UTF-8 path of the chosen folder
static std::atomic<int> g_generation{0};    // bumped on re-sort: discards stale decodes
static std::atomic<bool> g_decoding{false}; // gates held-arrow repeats
static NSURL* g_folderURL = nil;            // security-scoped, access held for life
static NSView* g_contentView = nil;         // the IRView, for background repaints
                                            // (the borderless window is never "key",
                                            // so [NSApp keyWindow] is nil — don't use it)
static dispatch_queue_t g_sidecarQ = nullptr;   // all sidecar reads+writes
static RawExif g_curExif;                        // EXIF of the on-screen frame
static int g_maxPix = 2048;                      // decode cap = longest screen edge px
static int g_lastDir = 1;                        // nav direction: prefetch bias

// Decode cache keyed by ITEM index. SPEED (principle #1): a visited frame stays
// decoded so back-nav is instant, and neighbours are prefetched so forward-nav
// lands on an already-decoded image. Big screen-sized bitmaps, so the window is
// bounded and evicted around the cursor.
struct Cached { CGImageRef img = nullptr; RawExif exif; bool failed = false; };
static const int CACHE_RADIUS = 3;              // prefetch/keep this many each side
static std::map<int, Cached> g_cache;           // idx -> decoded frame
static std::set<int> g_inFlight;                // decodes running right now
static std::mutex g_cacheMx;                     // guards g_cache + g_inFlight

// Grid mode: a scrollable contact sheet of small thumbnails (separate cache,
// keyed by item index, decoded at THUMB_PIX). g_cur doubles as the grid cursor.
static bool g_gridMode = false;
static bool g_zoom = false;                      // 100% loupe (Milestone 6)
static CGPoint g_zoomCenter = {0.5f, 0.5f};      // pan center as a fraction of image
static CGImageRef g_zoomImage = nullptr;         // full-res preview for pixel-peeping
static const int THUMB_PIX = 400;
static std::map<int, CGImageRef> g_thumbCache;
static std::set<int> g_thumbInFlight;
static std::mutex g_thumbMx;
static int g_gridCols = 1;                        // recomputed each layout
static CGFloat g_gridScrollY = 0;                 // px from the top of the sheet
static bool g_sbDragging = false;                 // dragging the grid scrollbar
// The view-position window currently worth decoding thumbnails for (visible ±
// margin, updated each grid draw). A queued thumb decode for a position that has
// scrolled far outside this window bails instead of blocking the visible ones.
static std::atomic<int> g_visLo{0}, g_visHi{1 << 30};

// Drive-aware scheduling: on a seek-bound drive (spinning disk / slow card),
// serialize background reads through a 1-permit semaphore so thumbnails and
// prefetch don't seek-storm the on-screen decode. Fast drives get real parallelism.
static bool g_slowDrive = false;
static dispatch_semaphore_t g_bgSem = nullptr;    // gates BACKGROUND decodes only

// Persistent thumbnail cache: <folder>/irate_thumbs_mac.jrc, an append-log keyed
// by FNV-1a of the relative lowercased path (+mtime/fsize guard). Makes grid and
// cold restart instant — no re-parsing every raw. Distinct filename from the
// Windows irate_thumbs.jrc so a shared drive can't cross-corrupt (path hashes
// differ by separator anyway, so nothing would cross-match).
struct TcEnt { uint64_t mtime, fsize, off; uint32_t len; uint16_t w, h; };
static std::map<uint64_t, TcEnt> g_tcIndex;       // pathHash -> newest record
static std::mutex g_tcMx;
static int g_tcFd = -1;
static uint64_t g_tcEnd = 0;                       // append offset

static std::vector<std::string> g_exts;   // active extensions (lowercase, with dot)
static bool extEnabled(const std::string& e) {
    for (auto& x : g_exts) if (x == e) return true; return false;
}
static bool hasWantedExt(const std::string& name) {
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot);
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    if (ext == ".jpeg") ext = ".jpg";     // the .jpg toggle covers both
    return extEnabled(ext);
}

// ---------------------------------------------------------------- keymap
// A key is identified either by macOS hardware keyCode (arrows, Home, Esc, …) or
// by its character (letters / digits / punctuation), plus a Shift flag. Named
// specials round-trip through this small table, mirroring the Win32 parseKeyName.
struct KeySpec { int keyCode = -1; unichar ch = 0;
    bool shift = false, ctrl = false, alt = false, cmd = false;  // alt = Option
    bool operator==(const KeySpec& o) const {
        return keyCode == o.keyCode && ch == o.ch && shift == o.shift
            && ctrl == o.ctrl && alt == o.alt && cmd == o.cmd; } };
struct NamedKey { const char* name; int keyCode; };
static const NamedKey kNamedKeys[] = {
    {"LEFT",123},{"RIGHT",124},{"DOWN",125},{"UP",126},{"SPACE",49},
    {"ESC",53},{"ESCAPE",53},{"ENTER",36},{"RETURN",36},{"TAB",48},
    {"HOME",115},{"END",119},{"PGUP",116},{"PAGEUP",116},{"PGDN",121},{"PAGEDOWN",121},
    {"DELETE",117},{"BACKSPACE",51},
    {"F1",122},{"F2",120},{"F3",99},{"F4",118},{"F5",96},{"F6",97},
    {"F7",98},{"F8",100},{"F9",101},{"F10",109},{"F11",103},{"F12",111},
};
static std::vector<std::pair<KeySpec, Action>> g_keymap;

// Keyword sets: APP-LEVEL (saved in the ini, not per folder) slots pairing a key
// with a comma-separated keyword list. Pressing a slot's key while culling
// toggles those keywords on the current image (all present -> remove; else add).
// Written to the sidecar as a dc:subject bag via core.h's xmpApplyKeywords.
struct KwSlot { KeySpec key; bool bound = false; std::string words; };
static const int kKwSlots = 10;
static KwSlot g_kwSlots[kKwSlots];
static bool g_kwOpen = false;               // keyword-sets editor visible (K)
static int g_kwCaptureSlot = -1;            // slot waiting for a key press
static int g_kwEditSlot = -1;               // slot whose words are being typed
static NSString* g_kwMsg = nil;             // transient editor message
// Split a slot's text into trimmed keywords (comma or semicolon separated).
static std::vector<std::string> splitKeywords(const std::string& s) {
    std::vector<std::string> out; std::string cur;
    auto flush = [&]() {
        while (!cur.empty() && isspace((unsigned char)cur.front())) cur.erase(cur.begin());
        while (!cur.empty() && isspace((unsigned char)cur.back()))  cur.pop_back();
        if (!cur.empty()) out.push_back(cur);
        cur.clear();
    };
    for (char c : s) { if (c == ',' || c == ';') flush(); else cur += c; }
    flush();
    return out;
}

// Parse one key token ("RIGHT", "SHIFT+P", "/", "1") into a KeySpec (keyCode<0
// and ch==0 means "unparsed").
static KeySpec parseKeyToken(std::string s) {
    KeySpec k;
    // trim + uppercase-compare for names, but keep original for single chars
    while (!s.empty() && isspace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && isspace((unsigned char)s.back()))  s.pop_back();
    // strip modifier prefixes in any order: SHIFT+ CTRL+ ALT+/OPT+ CMD+
    for (;;) {
        std::string up = s; for (auto& c : up) c = (char)toupper((unsigned char)c);
        if      (up.rfind("SHIFT+", 0) == 0)   { k.shift = true; s = s.substr(6); }
        else if (up.rfind("CTRL+", 0) == 0)    { k.ctrl = true;  s = s.substr(5); }
        else if (up.rfind("CONTROL+", 0) == 0) { k.ctrl = true;  s = s.substr(8); }
        else if (up.rfind("ALT+", 0) == 0)     { k.alt = true;   s = s.substr(4); }
        else if (up.rfind("OPT+", 0) == 0)     { k.alt = true;   s = s.substr(4); }
        else if (up.rfind("OPTION+", 0) == 0)  { k.alt = true;   s = s.substr(7); }
        else if (up.rfind("CMD+", 0) == 0)     { k.cmd = true;   s = s.substr(4); }
        else if (up.rfind("COMMAND+", 0) == 0) { k.cmd = true;   s = s.substr(8); }
        else break;
    }
    std::string up = s; for (auto& c : up) c = (char)toupper((unsigned char)c);
    if (s.empty()) return k;
    for (auto& nk : kNamedKeys) if (up == nk.name) { k.keyCode = nk.keyCode; return k; }
    // Punctuation names -> characters (matched by char, so layout-tolerant).
    struct { const char* n; char c; } kPunct[] = {
        {"LBRACKET",'['},{"RBRACKET",']'},{"SEMICOLON",';'},{"SLASH",'/'},
        {"BACKSLASH",'\\'},{"QUOTE",'\''},{"COMMA",','},{"PERIOD",'.'},
        {"MINUS",'-'},{"PLUS",'='},{"GRAVE",'`'},
    };
    for (auto& p : kPunct) if (up == p.n) { k.ch = (unichar)p.c; return k; }
    if (s.size() == 1) { k.ch = (unichar)tolower((unsigned char)s[0]); }
    return k;
}
// Bind a comma-separated key list to an action. onlyIfFree = for DEFAULT bindings
// of an action the ini didn't mention (an upgrade adding a new action, e.g. K for
// keyword sets): don't steal a key the user already assigned to something else.
static void bindKeys(const std::string& list, Action a, bool onlyIfFree = false) {
    size_t start = 0;
    while (start <= list.size()) {
        size_t comma = list.find(',', start);
        std::string one = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        KeySpec k = parseKeyToken(one);
        if (k.keyCode >= 0 || k.ch != 0) {
            bool taken = false;
            if (onlyIfFree) for (auto& kv : g_keymap) if (kv.first == k) { taken = true; break; }
            if (!taken) g_keymap.push_back({ k, a });
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
}
// Build the KeySpec that identifies an incoming key event.
static KeySpec specForEvent(NSEvent* e) {
    KeySpec k;
    NSEventModifierFlags m = e.modifierFlags;
    k.shift = (m & NSEventModifierFlagShift)   != 0;
    k.ctrl  = (m & NSEventModifierFlagControl) != 0;
    k.alt   = (m & NSEventModifierFlagOption)  != 0;
    k.cmd   = (m & NSEventModifierFlagCommand) != 0;
    int kc = e.keyCode;
    for (auto& nk : kNamedKeys) if (kc == nk.keyCode) { k.keyCode = kc; return k; }
    NSString* chars = [e.charactersIgnoringModifiers lowercaseString];
    if (chars.length == 1) k.ch = [chars characterAtIndex:0];
    return k;
}
static Action actionForEvent(NSEvent* e) {
    KeySpec want = specForEvent(e);
    for (auto& kv : g_keymap)               // full match incl. all modifiers
        if (kv.first == want) return kv.second;
    return A_NONE;
}
// Human-readable name for a bound key (Mac modifier symbols: ⌃⌥⇧⌘).
static std::string keyDisplayName(const KeySpec& k) {
    std::string s;
    if (k.keyCode >= 0) {
        for (auto& nk : kNamedKeys) if (nk.keyCode == k.keyCode) { s = nk.name; break; }
        if (s.empty()) s = "?";
    } else if (k.ch) {
        s = std::string(1, (char)toupper((int)k.ch));
    }
    std::string mod;
    if (k.ctrl)  mod += "⌃";   // ⌃
    if (k.alt)   mod += "⌥";   // ⌥
    if (k.shift) mod += "⇧";   // ⇧
    if (k.cmd)   mod += "⌘";   // ⌘
    return mod + s;
}
// Comma-joined list of the keys currently bound to an action.
static std::string keysOfAction(Action a) {
    std::string out;
    for (auto& kv : g_keymap) {
        if (kv.second != a) continue;
        if (!out.empty()) out += ", ";
        out += keyDisplayName(kv.first);
    }
    return out;
}

// ---------------------------------------------------------------- ini
// Minimal INI (section/key=value). macOS has no GetPrivateProfileString; this
// covers what iRate needs. Stored at ~/Library/Application Support/iRate/irate.ini.
static std::map<std::string, std::map<std::string, std::string>> g_ini;
static std::string g_iniPath;

static void iniLoadFile(const std::string& path) {
    g_ini.clear();
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return;
    char* buf = nullptr; size_t cap = 0; ssize_t len; std::string sec;   // getline: no line-length cap
    while ((len = getline(&buf, &cap, f)) != -1) {                        // (long keyword lists must not truncate)
        std::string s(buf, (size_t)len);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        size_t a = s.find_first_not_of(" \t"); if (a == std::string::npos) continue;
        if (s[a] == ';' || s[a] == '#') continue;
        if (s[a] == '[') { size_t b = s.find(']', a); if (b != std::string::npos)
            sec = s.substr(a + 1, b - a - 1); continue; }
        size_t eq = s.find('=', a); if (eq == std::string::npos) continue;
        std::string key = s.substr(a, eq - a);
        while (!key.empty() && isspace((unsigned char)key.back())) key.pop_back();
        std::string val = s.substr(eq + 1);
        size_t v0 = val.find_first_not_of(" \t");
        val = (v0 == std::string::npos) ? "" : val.substr(v0);
        for (auto& c : key) c = (char)tolower((unsigned char)c);
        for (auto& c : sec) c = (char)tolower((unsigned char)c);
        g_ini[sec][key] = val;
    }
    free(buf);
    fclose(f);
}
static std::string iniGet(const char* sec, const char* key, const char* def) {
    auto s = g_ini.find(sec); if (s == g_ini.end()) return def;
    auto k = s->second.find(key); if (k == s->second.end()) return def;
    return k->second;
}
static bool iniHas(const char* sec, const char* key) {
    auto s = g_ini.find(sec); if (s == g_ini.end()) return false;
    return s->second.find(key) != s->second.end();
}

// The default ini written on first run so the owner has something to edit.
static const char* kDefaultIni =
"; iRate configuration (macOS). Edit and relaunch to apply.\n"
"; Keys: single letters/digits, or LEFT RIGHT UP DOWN SPACE ESC ENTER TAB HOME\n"
";       END PGUP PGDN DELETE F1-F12. Prefix SHIFT+ for shifted. Comma-separate\n"
";       multiple keys per action (e.g. next=RIGHT,SPACE).\n"
"[keys]\n"
"next=RIGHT,SPACE\nprev=LEFT\nfirst=HOME\nlast=END\n"
"rate1=1\nrate2=2\nrate3=3\nrate4=4\nrate5=5\nrate0=0\nreject=X\n"
"labelred=6\nlabelyellow=7\nlabelgreen=8\nlabelblue=9\nlabelpurple=P\n"
"sortnext=RBRACKET\nsortprev=LBRACKET\nsortdir=SEMICOLON\n"
"grid=G\nzoom=SLASH\nfilter=F\nkeywordsets=K\ntoggleinfo=I\ninfopos=Z\nhelp=H\nprefs=SHIFT+P\nquit=ESC\n"
"[options]\n"
"; autoadvance after rating: capslock (only when Caps Lock is on), always, never\n"
"autoadvance=capslock\n"
"; show the info bar at startup\n"
"showinfo=1\n"
"; treat RAW+JPG with the same name as one image (shown as +JPG)\n"
"pairrawjpg=1\n"
"; 1 = resume point/sort per folder (irate.session in the folder root)\n"
"sessioninfolder=1\n";

static void loadConfig(const std::string& supportDir) {
    g_iniPath = supportDir + "/irate.ini";
    struct stat st;
    if (stat(g_iniPath.c_str(), &st) != 0) {           // first run: write default
        FILE* f = fopen(g_iniPath.c_str(), "wb");
        if (f) { fwrite(kDefaultIni, 1, strlen(kDefaultIni), f); fclose(f); }
    }
    iniLoadFile(g_iniPath);
    g_keymap.clear();
    for (auto& r : kActRows) {
        if (iniHas("keys", r.iniName)) bindKeys(iniGet("keys", r.iniName, ""), r.a);
        else bindKeys(r.defKey, r.a, /*onlyIfFree=*/true);   // new action on an old ini: don't steal
    }
    g_showInfo = iniGet("options", "showinfo", "1") != "0";
    g_pairRawJpg = iniGet("options", "pairrawjpg", "1") != "0";
    g_sessionInFolder = iniGet("options", "sessioninfolder", "1") != "0";
    // extensions: semicolon-separated; default = all supported
    g_exts.clear();
    std::string ex = iniGet("options", "extensions", ".arw;.cr2;.cr3;.nef;.nrw;.raf;.rw2;.pef;.dng;.orf;.jpg");
    size_t sp = 0;
    while (sp <= ex.size()) {
        size_t semi = ex.find(';', sp);
        std::string one = ex.substr(sp, semi == std::string::npos ? std::string::npos : semi - sp);
        while (!one.empty() && isspace((unsigned char)one.front())) one.erase(one.begin());
        while (!one.empty() && isspace((unsigned char)one.back()))  one.pop_back();
        for (auto& c : one) c = (char)tolower((unsigned char)c);
        if (!one.empty()) g_exts.push_back(one);
        if (semi == std::string::npos) break;
        sp = semi + 1;
    }
    std::string adv = iniGet("options", "autoadvance", "capslock");
    if (adv == "always") g_advanceMode = 1;
    else if (adv == "never" || adv == "0") g_advanceMode = 2;
    else g_advanceMode = 0;
    // keyword sets: [keywords] setNkey= (key token or empty) / setNwords= (comma list)
    for (int i = 0; i < kKwSlots; i++) {
        char kk[16], kw[16];
        snprintf(kk, sizeof kk, "set%dkey", i + 1);
        snprintf(kw, sizeof kw, "set%dwords", i + 1);
        g_kwSlots[i].words = iniGet("keywords", kw, "");
        g_kwSlots[i].bound = false;
        std::string tok = iniGet("keywords", kk, "");
        if (!tok.empty()) {
            KeySpec k = parseKeyToken(tok);
            if (k.keyCode >= 0 || k.ch) { g_kwSlots[i].key = k; g_kwSlots[i].bound = true; }
        }
    }
}

// ---------------------------------------------------------------- rebind + write
// The ini token for a key spec ("RIGHT", "SHIFT+P", "1", "/"): specials by name,
// punctuation/letters/digits by their character. Round-trips through parseKeyToken.
static std::string iniTokenForKey(const KeySpec& k) {
    std::string s;
    if (k.keyCode >= 0) {
        for (auto& nk : kNamedKeys) if (nk.keyCode == k.keyCode) { s = nk.name; break; }
        if (s.empty()) s = "?";
    } else if (k.ch) {
        s = std::string(1, (char)toupper((int)k.ch));   // letters upper; punctuation as-is
    }
    std::string pre;                                     // canonical order CTRL ALT SHIFT CMD
    if (k.ctrl)  pre += "CTRL+";
    if (k.alt)   pre += "ALT+";
    if (k.shift) pre += "SHIFT+";
    if (k.cmd)   pre += "CMD+";
    return pre + s;
}
static std::string keysIniString(Action a) {
    std::string out;
    for (auto& kv : g_keymap) {
        if (kv.second != a) continue;
        if (!out.empty()) out += ",";
        out += iniTokenForKey(kv.first);
    }
    return out;
}
// Rewrite the whole ini from current state (keeps the helpful header comments).
static void writeIni() {
    FILE* f = fopen(g_iniPath.c_str(), "wb");
    if (!f) return;
    fputs("; iRate configuration (macOS). Edit here or rebind live in Preferences (Shift+P).\n"
          "; Keys: single letters/digits, or LEFT RIGHT UP DOWN SPACE ESC ENTER TAB HOME\n"
          ";       END PGUP PGDN DELETE F1-F12. Prefix SHIFT+ for shifted. Comma-separate\n"
          ";       multiple keys per action (e.g. next=RIGHT,SPACE).\n[keys]\n", f);
    for (auto& r : kActRows) {
        std::string ks = keysIniString(r.a);
        fprintf(f, "%s=%s\n", r.iniName, ks.c_str());
    }
    fputs("[options]\n"
          "; autoadvance after rating: capslock (only when Caps Lock is on), always, never\n", f);
    fprintf(f, "autoadvance=%s\n", g_advanceMode == 1 ? "always" : g_advanceMode == 2 ? "never" : "capslock");
    fprintf(f, "showinfo=%d\n", g_showInfo ? 1 : 0);
    fprintf(f, "pairrawjpg=%d\n", g_pairRawJpg ? 1 : 0);
    fprintf(f, "sessioninfolder=%d\n", g_sessionInFolder ? 1 : 0);
    std::string ex; for (auto& e : g_exts) { if (!ex.empty()) ex += ";"; ex += e; }
    fprintf(f, "extensions=%s\n", ex.c_str());
    fputs("[keywords]\n"
          "; keyword sets: setNkey = the key that toggles set N while culling,\n"
          "; setNwords = its comma-separated keywords. Edit in-app with K.\n", f);
    for (int i = 0; i < kKwSlots; i++) {
        std::string tok = g_kwSlots[i].bound ? iniTokenForKey(g_kwSlots[i].key) : "";
        fprintf(f, "set%dkey=%s\nset%dwords=%s\n", i + 1, tok.c_str(), i + 1, g_kwSlots[i].words.c_str());
    }
    fclose(f);
}
// Bind a key to an action: steal it from any other action, add it here, persist.
// Rebind = REPLACE: the action's previous key(s) are dropped, and the new key is
// stolen from whatever other action held it. One action ends up with one key.
static void bindActionToKey(Action a, const KeySpec& k) {
    for (auto it = g_keymap.begin(); it != g_keymap.end(); )
        if (it->second == a || it->first == k) it = g_keymap.erase(it); else ++it;
    g_keymap.push_back({ k, a });
    writeIni();
}
// Describe what ELSE currently uses key k — an action or a keyword set — ignoring
// the action / keyword slot being edited. Returns nil if the key is free.
static NSString* keyUsedBy(const KeySpec& k, Action ignoreAction, int ignoreKwSlot) {
    for (auto& kv : g_keymap) {
        if (!(kv.first == k) || kv.second == ignoreAction) continue;
        for (auto& r : kActRows) if (r.a == kv.second)
            return [NSString stringWithFormat:@"“%s”", r.label];
        return @"an app control";
    }
    for (int i = 0; i < kKwSlots; i++)
        if (i != ignoreKwSlot && g_kwSlots[i].bound && g_kwSlots[i].key == k)
            return [NSString stringWithFormat:@"keyword set %d", i + 1];
    return nil;
}

// ---------------------------------------------------------------- decode
// Apply an EXIF orientation (1..8) to a decoded CGImage, materializing pixels.
// This is the macOS answer to the WIC TRAP: never leave orientation as a lazy
// transform on top of the decode chain — bake it into a fresh bitmap once.
static CGImageRef makeOriented(CGImageRef src, int orient) CF_RETURNS_RETAINED {
    if (!src) return nullptr;
    if (orient < 1 || orient > 8) orient = 1;
    size_t w = CGImageGetWidth(src), h = CGImageGetHeight(src);
    bool swap = (orient >= 5);                 // 5..8 swap width/height
    size_t ow = swap ? h : w, oh = swap ? w : h;
    if (orient == 1) { CGImageRetain(src); return src; }

    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(nullptr, ow, oh, 8, 0, cs,
                          kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);
    if (!ctx) { CGImageRetain(src); return src; }

    // Map each EXIF orientation to an affine transform in the output space.
    CGContextTranslateCTM(ctx, ow / 2.0, oh / 2.0);
    switch (orient) {
        case 2: CGContextScaleCTM(ctx, -1, 1); break;                       // flip H
        case 3: CGContextRotateCTM(ctx, M_PI); break;                       // 180
        case 4: CGContextScaleCTM(ctx, 1, -1); break;                       // flip V
        case 5: CGContextRotateCTM(ctx, -M_PI_2); CGContextScaleCTM(ctx, -1, 1); break;
        case 6: CGContextRotateCTM(ctx, -M_PI_2); break;                    // 90 CW
        case 7: CGContextRotateCTM(ctx,  M_PI_2); CGContextScaleCTM(ctx, -1, 1); break;
        case 8: CGContextRotateCTM(ctx,  M_PI_2); break;                    // 90 CCW
        default: break;
    }
    // After rotation the drawing box is always the ORIGINAL w x h, centered.
    CGContextDrawImage(ctx, CGRectMake(-(CGFloat)w / 2.0, -(CGFloat)h / 2.0,
                                       (CGFloat)w, (CGFloat)h), src);
    CGImageRef out = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    return out ? out : (CGImageRetain(src), src);
}

// Decode the embedded preview for a file to an oriented CGImage capped at maxPix.
// Returns nullptr on failure (out params carry EXIF for the info bar regardless).
static CGImageRef decodePreview(const std::string& path, int maxPix,
                                RawExif& exifOut) CF_RETURNS_RETAINED {
    MacByteSource src(path);
    if (!src.ok()) return nullptr;
    ArwParser parser(src);
    ArwResult r = parser.parse();
    exifOut = r.exif;
    if (!r.ok) return nullptr;

    // Pull just the embedded JPEG blob (a few MB) into memory — never the raw.
    NSMutableData* blob = [NSMutableData dataWithLength:r.jpegLen];
    if (!blob || !src.read(r.jpegOff, blob.mutableBytes, r.jpegLen)) return nullptr;

    CGImageSourceRef is = CGImageSourceCreateWithData((__bridge CFDataRef)blob, nullptr);
    if (!is) return nullptr;
    // Downscale in the DCT domain via ImageIO; do NOT let it apply the transform
    // here — core.h's container orientation is authoritative across all formats,
    // and we bake it in ourselves against the materialized pixels.
    NSDictionary* opt = @{
        (id)kCGImageSourceCreateThumbnailFromImageAlways: @YES,
        (id)kCGImageSourceThumbnailMaxPixelSize: @(maxPix),
        (id)kCGImageSourceCreateThumbnailWithTransform: @NO,
    };
    CGImageRef raw = CGImageSourceCreateThumbnailAtIndex(is, 0, (__bridge CFDictionaryRef)opt);
    CFRelease(is);
    if (!raw) return nullptr;
    CGImageRef oriented = makeOriented(raw, r.exif.orientation);
    CGImageRelease(raw);
    return oriented;
}

// ---------------------------------------------------------------- drive probe
// macOS has no seek-penalty IOCTL, so measure it: network volumes are slow by
// definition; otherwise time a handful of scattered uncached reads. Spinning
// disks / slow card readers cost ~10ms per seek and blow past the threshold.
static bool probeSlowDrive(const std::string& sampleFile) {
    NSURL* u = [NSURL fileURLWithPath:[NSString stringWithUTF8String:sampleFile.c_str()]];
    NSNumber* isLocal = nil;
    if ([u getResourceValue:&isLocal forKey:NSURLVolumeIsLocalKey error:nil]
        && isLocal && !isLocal.boolValue)
        return true;   // network share
    int fd = open(sampleFile.c_str(), O_RDONLY);
    if (fd < 0) return false;
    fcntl(fd, F_NOCACHE, 1);                        // force real device reads
    struct stat st;
    if (fstat(fd, &st) || (uint64_t)st.st_size < 65536) { close(fd); return false; }
    uint64_t sz = (uint64_t)st.st_size;
    const int N = 6; char buf[32768];
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < N; i++) {
        off_t off = (off_t)((sz - sizeof buf) * ((double)i / (N - 1)));
        if (pread(fd, buf, sizeof buf, off) < 0) break;
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    close(fd);
    double ms = (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1e6;
    return ms > 60.0;
}

// ---------------------------------------------------------------- thumb disk cache
static uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}
static std::string relLowerPath(const std::string& full) {
    std::string rel = full, pre = g_folderPath + "/";
    if (full.rfind(pre, 0) == 0) rel = full.substr(pre.size());
    for (auto& c : rel) c = (char)tolower((unsigned char)c);
    return rel;
}
// Open + index the per-folder thumb cache (called once, off the main thread).
static void tcInit(const std::string& folder) {
    std::string path = folder + "/irate_thumbs_mac.jrc";
    std::lock_guard<std::mutex> lk(g_tcMx);
    g_tcFd = open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (g_tcFd < 0) return;
    struct stat st; if (fstat(g_tcFd, &st)) { close(g_tcFd); g_tcFd = -1; return; }
    uint64_t sz = (uint64_t)st.st_size;
    if (sz == 0) { pwrite(g_tcFd, "JRTC", 4, 0); g_tcEnd = 4; return; }
    char magic[4];
    if (pread(g_tcFd, magic, 4, 0) != 4 || memcmp(magic, "JRTC", 4)) {
        close(g_tcFd); g_tcFd = -1; return;   // foreign/corrupt: don't touch it
    }
    // scan records: [hash u64][mtime u64][fsize u64][w u16][h u16][len u32][jpeg len]
    uint64_t pos = 4;
    while (pos + 32 <= sz) {
        uint8_t h[32];
        if (pread(g_tcFd, h, 32, pos) != 32) break;
        TcEnt e;
        memcpy(&e.mtime, h + 8, 8); memcpy(&e.fsize, h + 16, 8);
        memcpy(&e.w, h + 24, 2); memcpy(&e.h, h + 26, 2); memcpy(&e.len, h + 28, 4);
        uint64_t hash; memcpy(&hash, h, 8);
        e.off = pos + 32;
        if (e.off + e.len > sz) break;         // truncated tail
        g_tcIndex[hash] = e;                    // later record wins
        pos = e.off + e.len;
    }
    g_tcEnd = pos;

    // Compaction: append-only means superseded records (a re-decoded file) leave
    // dead bytes. If >40% of a >32MB file is dead, rewrite keeping only the live
    // record per hash (mirrors the Windows build's startup compaction).
    uint64_t live = 4;
    for (auto& kv : g_tcIndex) live += 32 + kv.second.len;
    if (g_tcEnd > 32u * 1024 * 1024 && (g_tcEnd - live) * 10 > g_tcEnd * 4) {
        std::string tmp = path + ".tmp";
        int nf = open(tmp.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (nf >= 0) {
            pwrite(nf, "JRTC", 4, 0);
            uint64_t w = 4; bool ok = true;
            std::vector<std::pair<uint64_t, uint64_t>> newOffs;   // hash -> new payload off
            for (auto& kv : g_tcIndex) {
                const TcEnt& e = kv.second;
                uint32_t full = 32 + e.len;
                std::vector<uint8_t> buf(full);
                if (pread(g_tcFd, buf.data(), full, e.off - 32) != (ssize_t)full) { ok = false; break; }
                if (pwrite(nf, buf.data(), full, w) != (ssize_t)full) { ok = false; break; }
                newOffs.push_back({ kv.first, w + 32 });
                w += full;
            }
            if (ok && rename(tmp.c_str(), path.c_str()) == 0) {
                for (auto& p : newOffs) g_tcIndex[p.first].off = p.second;  // commit offsets
                close(g_tcFd); g_tcFd = nf; g_tcEnd = w;
            } else { close(nf); unlink(tmp.c_str()); }   // keep the old file on failure
        }
    }
}
// Return the cached thumbnail JPEG bytes for an item, if fresh; else nil.
static NSData* tcLookup(uint64_t hash, uint64_t mtime, uint64_t fsize) {
    std::lock_guard<std::mutex> lk(g_tcMx);
    if (g_tcFd < 0) return nil;
    auto f = g_tcIndex.find(hash);
    if (f == g_tcIndex.end()) return nil;
    if (f->second.mtime != mtime || f->second.fsize != fsize) return nil;  // stale
    NSMutableData* d = [NSMutableData dataWithLength:f->second.len];
    if (!d || pread(g_tcFd, d.mutableBytes, f->second.len, f->second.off) != (ssize_t)f->second.len)
        return nil;
    return d;
}
// Append a freshly-decoded thumbnail JPEG to the cache.
static void tcAppend(uint64_t hash, uint64_t mtime, uint64_t fsize,
                     uint16_t w, uint16_t h, NSData* jpeg) {
    if (!jpeg.length || jpeg.length > 0xFFFFFFFF) return;
    std::lock_guard<std::mutex> lk(g_tcMx);
    if (g_tcFd < 0) return;
    uint8_t hdr[32]; uint32_t len = (uint32_t)jpeg.length;
    memcpy(hdr, &hash, 8); memcpy(hdr + 8, &mtime, 8); memcpy(hdr + 16, &fsize, 8);
    memcpy(hdr + 24, &w, 2); memcpy(hdr + 26, &h, 2); memcpy(hdr + 28, &len, 4);
    if (pwrite(g_tcFd, hdr, 32, g_tcEnd) != 32) return;
    if (pwrite(g_tcFd, jpeg.bytes, len, g_tcEnd + 32) != (ssize_t)len) return;
    TcEnt e{ mtime, fsize, g_tcEnd + 32, len, w, h };
    g_tcIndex[hash] = e;
    g_tcEnd += 32 + len;
}
static uint64_t tcSizeBytes() { std::lock_guard<std::mutex> lk(g_tcMx); return g_tcEnd; }
// Wipe the on-disk thumb cache (truncate to just the magic).
static void tcClear() {
    std::lock_guard<std::mutex> lk(g_tcMx);
    if (g_tcFd < 0) return;
    ftruncate(g_tcFd, 4);
    pwrite(g_tcFd, "JRTC", 4, 0);
    g_tcEnd = 4;
    g_tcIndex.clear();
}
// Encode a CGImage to JPEG bytes for the disk cache.
static NSData* cgImageToJPEG(CGImageRef img, CGFloat quality) {
    NSMutableData* out = [NSMutableData data];
    CGImageDestinationRef dst = CGImageDestinationCreateWithData(
        (__bridge CFMutableDataRef)out, (CFStringRef)@"public.jpeg", 1, nullptr);
    if (!dst) return nil;
    NSDictionary* opt = @{ (id)kCGImageDestinationLossyCompressionQuality: @(quality) };
    CGImageDestinationAddImage(dst, img, (__bridge CFDictionaryRef)opt);
    bool ok = CGImageDestinationFinalize(dst);
    CFRelease(dst);
    return ok ? out : nil;
}
// Decode JPEG bytes (from the disk cache) to a CGImage.
static CGImageRef cgImageFromJPEG(NSData* data) CF_RETURNS_RETAINED {
    CGImageSourceRef is = CGImageSourceCreateWithData((__bridge CFDataRef)data, nullptr);
    if (!is) return nullptr;
    CGImageRef img = CGImageSourceCreateImageAtIndex(is, 0, nullptr);
    CFRelease(is);
    return img;
}

// Decode a small grid thumbnail for one item (path captured on the main thread).
// Fast path: read the pre-decoded JPEG from the disk cache. Slow path: parse the
// raw, then append the result so next time (and next launch) is instant. The
// generation guard mirrors requestDecode; a re-sort discards stale-index thumbs.
static void requestThumb(int idx, const std::string& path,
                         uint64_t mtime, uint64_t fsize, int gen, int viewPos) {
    {
        std::lock_guard<std::mutex> lk(g_thumbMx);
        if (g_thumbCache.count(idx) || g_thumbInFlight.count(idx)) return;
        g_thumbInFlight.insert(idx);
    }
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        uint64_t hash = fnv1a(relLowerPath(path));
        CGImageRef img = nullptr;
        NSData* cached = tcLookup(hash, mtime, fsize);
        if (cached) {
            img = cgImageFromJPEG(cached);                 // fast: no raw parse
        } else {
            if (g_bgSem) dispatch_semaphore_wait(g_bgSem, DISPATCH_TIME_FOREVER);
            // By the time we hold the semaphore the user may have scrolled far away;
            // if this cell is well outside the viewport now, drop it so the visible
            // ones aren't stuck behind it. It re-requests if scrolled back to.
            if (viewPos < g_visLo.load() || viewPos > g_visHi.load()) {
                if (g_bgSem) dispatch_semaphore_signal(g_bgSem);
                std::lock_guard<std::mutex> lk(g_thumbMx); g_thumbInFlight.erase(idx);
                return;
            }
            RawExif exif;
            img = decodePreview(path, THUMB_PIX, exif);    // slow: parse the raw
            if (g_bgSem) dispatch_semaphore_signal(g_bgSem);
            if (img) {
                NSData* jpeg = cgImageToJPEG(img, 0.72);
                if (jpeg) tcAppend(hash, mtime, fsize,
                    (uint16_t)CGImageGetWidth(img), (uint16_t)CGImageGetHeight(img), jpeg);
            }
        }
        bool stale = false;
        {
            std::lock_guard<std::mutex> lk(g_thumbMx);
            g_thumbInFlight.erase(idx);
            if (gen != g_generation.load()) stale = true;
            else if (img) g_thumbCache[idx] = img;
        }
        if (stale || !img) { if (img) CGImageRelease(img); }
        if (!stale) dispatch_async(dispatch_get_main_queue(), ^{
            if (g_gridMode) [g_contentView setNeedsDisplay:YES];   // show it as it lands
        });
    });
}

// ---------------------------------------------------------------- scan
static void scanDir(NSString* dir, std::vector<Item>& out) {
    NSFileManager* fm = [NSFileManager defaultManager];
    NSError* err = nil;
    NSArray<NSString*>* names = [fm contentsOfDirectoryAtPath:dir error:&err];
    if (!names) return;
    for (NSString* name in names) {
        NSString* full = [dir stringByAppendingPathComponent:name];
        BOOL isDir = NO;
        [fm fileExistsAtPath:full isDirectory:&isDir];
        if (isDir) {
            if ([[name lowercaseString] hasSuffix:@".lrdata"]) continue;  // LR previews
            scanDir(full, out);
        } else {
            std::string n = name.UTF8String;
            if (!hasWantedExt(n)) continue;
            NSDictionary* a = [fm attributesOfItemAtPath:full error:nil];
            Item it;
            it.path = full.UTF8String;
            it.fsize = (uint64_t)[a fileSize];
            it.mtime = (uint64_t)[[a fileModificationDate] timeIntervalSince1970];
            out.push_back(std::move(it));
        }
    }
}

// ---------------------------------------------------------------- sort + pairing
static const char* kRawExts[] = { ".arw",".cr2",".cr3",".nef",".nrw",
                                  ".raf",".rw2",".pef",".dng",".orf" };
static bool extIn(const std::string& p, const char* const* list, int n) {
    size_t dot = p.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string e = p.substr(dot);
    for (auto& c : e) c = (char)tolower((unsigned char)c);
    for (int i = 0; i < n; i++) if (e == list[i]) return true;
    return false;
}
static bool isRawFile(const std::string& p) { return extIn(p, kRawExts, 10); }
static bool isJpgFile(const std::string& p) {
    static const char* j[] = { ".jpg", ".jpeg" }; return extIn(p, j, 2);
}
// Key that a RAW and its JPG twin share: path minus extension, lowercased.
static std::string pairKey(const std::string& p) {
    size_t dot = p.find_last_of('.');
    std::string k = (dot == std::string::npos) ? p : p.substr(0, dot);
    for (auto& c : k) c = (char)tolower((unsigned char)c);
    return k;
}
// Natural (Finder-style) path compare so "IMG_2" precedes "IMG_10".
static int naturalCompare(const std::string& a, const std::string& b) {
    NSString* sa = [NSString stringWithUTF8String:a.c_str()];
    NSString* sb = [NSString stringWithUTF8String:b.c_str()];
    return (int)[sa localizedStandardCompare:sb];
}
static bool itemLess(const Item& a, const Item& b) {
    int c = 0;
    if (g_sortType == 1) c = a.mtime < b.mtime ? -1 : a.mtime > b.mtime ? 1 : 0;
    else if (g_sortType == 2) c = a.fsize < b.fsize ? -1 : a.fsize > b.fsize ? 1 : 0;
    if (c == 0) c = naturalCompare(a.path, b.path);       // tiebreak / name sort
    if (c == 0) return false;
    return g_sortDesc ? c > 0 : c < 0;
}
// Collapse RAW+JPG pairs to the RAW (flagged +JPG); drop the standalone JPG entry.
static void pairRawJpg(std::vector<Item>& items) {
    std::set<std::string> rawKeys, jpgKeys;
    for (auto& it : items) {
        if (isRawFile(it.path)) rawKeys.insert(pairKey(it.path));
        else if (isJpgFile(it.path)) jpgKeys.insert(pairKey(it.path));
    }
    std::vector<Item> out;
    out.reserve(items.size());
    for (auto& it : items) {
        std::string k = pairKey(it.path);
        if (isJpgFile(it.path) && rawKeys.count(k)) continue;   // twin of a raw: drop
        if (isRawFile(it.path) && jpgKeys.count(k)) it.jpgTwin = true;
        out.push_back(it);
    }
    items.swap(out);
}

// ---------------------------------------------------------------- session
// Per-folder resume: irate.session in the folder root (relative paths so the
// folder can move). Falls back to the ini's [state] when sessioninfolder=0.
struct KV { std::map<std::string, std::string> m;
    std::string get(const char* k, const char* d) const {
        auto f = m.find(k); return f == m.end() ? d : f->second; } };
static std::string sessionFilePath() { return g_folderPath + "/irate.session"; }
static KV sessionRead() {
    KV kv;
    FILE* f = fopen(sessionFilePath().c_str(), "rb");
    if (!f) return kv;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        std::string s(line);
        while (!s.empty() && (s.back()=='\n'||s.back()=='\r')) s.pop_back();
        size_t eq = s.find('='); if (eq == std::string::npos) continue;
        kv.m[s.substr(0, eq)] = s.substr(eq + 1);
    }
    fclose(f);
    return kv;
}
static void sessionWrite(const std::string& lastRel) {
    if (!g_sessionInFolder) return;   // (ini-based resume path not needed yet)
    FILE* f = fopen(sessionFilePath().c_str(), "wb");
    if (!f) return;
    fprintf(f, "lastfile=%s\n", lastRel.c_str());
    fprintf(f, "sorttype=%d\n", g_sortType);
    fprintf(f, "sortdesc=%d\n", g_sortDesc ? 1 : 0);
    fprintf(f, "gridmode=%d\n", g_gridMode ? 1 : 0);
    fclose(f);
}

// ---------------------------------------------------------------- filtered view
// g_view maps view positions -> item indices; g_cur is a VIEW position. Rating
// filters let the owner cull down to picks/rejects/unrated fast.
static int viewCount() { return (int)g_view.size(); }
static int curItem()   { return (g_cur >= 0 && g_cur < (int)g_view.size()) ? g_view[g_cur] : -1; }
static bool filterActive() {
    return g_ratingMask != 0x7F || g_labelMask != 0x3F || !g_filterText.empty();
}
static bool filterPass(const Item& it) {
    int r = (it.rating == -2) ? 0 : it.rating;   // -2 = sidecar not yet read
    unsigned rbit = (r == -1) ? 0x40u : (1u << r);            // reject = bit 6
    if (!(g_ratingMask & rbit)) return false;
    int lb = (it.label < 0) ? 0 : it.label;                  // 0..5
    if (!(g_labelMask & (1u << lb))) return false;
    if (!g_filterText.empty()) {
        std::string name = it.path;
        size_t sl = name.find_last_of('/'); if (sl != std::string::npos) name = name.substr(sl + 1);
        for (auto& c : name) c = (char)tolower((unsigned char)c);
        std::string t = g_filterText;
        for (auto& c : t) c = (char)tolower((unsigned char)c);
        if (name.find(t) == std::string::npos) return false;
    }
    return true;
}
// Rebuild g_view for the current filter, keeping the cursor on keepItem if it
// still passes (else clamp to a valid position).
static void rebuildView(int keepItem) {
    g_view.clear();
    for (int i = 0; i < (int)g_items.size(); i++)
        if (filterPass(g_items[i])) g_view.push_back(i);
    int pos = 0;
    if (keepItem >= 0)
        for (int p = 0; p < (int)g_view.size(); p++)
            if (g_view[p] == keepItem) { pos = p; break; }
    g_cur = g_view.empty() ? 0 : std::min(pos, (int)g_view.size() - 1);
}

// ---------------------------------------------------------------- sidecars
static std::string sidecarPath(const std::string& raw) {
    size_t dot = raw.find_last_of('.');
    return (dot == std::string::npos ? raw : raw.substr(0, dot)) + ".xmp";
}
// Read a sidecar for patching, distinguishing "missing" from "unreadable" to
// avoid the DATA-LOSS clobber: a transient read failure on an EXISTING sidecar
// (Lightroom/AV/Dropbox briefly holding it, short read) would otherwise hand ""
// to xmpApply/xmpApplyKeywords, which build a fresh minimal doc that replaces the
// user's real crs:*/rating/label/keywords. Returns:
//   file missing        -> out="", true   (a fresh sidecar is correct)
//   read OK             -> out=data, true
//   exists but unreadable after retries -> false  (caller MUST NOT write)
static bool readSidecarForPatch(const std::string& p, std::string& out) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0) { out.clear(); return true; }   // missing -> fresh OK
    for (int attempt = 0; attempt < 3; attempt++) {
        FILE* f = fopen(p.c_str(), "rb");
        if (f) {
            fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
            if (n >= 0) {
                std::string s((size_t)(n > 0 ? n : 0), 0);
                size_t rd = n > 0 ? fread(&s[0], 1, (size_t)n, f) : 0;
                fclose(f);
                if (rd == (size_t)n) { out.swap(s); return true; }   // 0-byte counts as readable
            } else fclose(f);
        }
        usleep(50000);   // 50ms, then retry
    }
    return false;   // exists but unreadable -> drop the op, never clobber
}
// Read a sidecar (if any) and reflect its rating/label/keywords into the item.
static void loadSidecar(int idx) {
    if (idx < 0 || idx >= (int)g_items.size()) return;
    std::string path = g_items[idx].path;
    dispatch_async(g_sidecarQ, ^{
        std::string xmp;
        if (!readSidecarForPatch(sidecarPath(path), xmp)) return;   // unreadable: retry on next visit (stays -2)
        int rating = 0, label = 0;
        if (!xmp.empty()) xmpParse(xmp, rating, label);
        std::vector<std::string> kws = xmp.empty() ? std::vector<std::string>{}
                                                   : xmpGetKeywords(xmp);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (idx < (int)g_items.size() && g_items[idx].path == path &&
                g_items[idx].rating == -2) {       // don't clobber a fresh user rating
                g_items[idx].rating = xmp.empty() ? 0 : rating;
                g_items[idx].label  = xmp.empty() ? -1 : label;
                g_items[idx].keywords = kws;
                [g_contentView setNeedsDisplay:YES];
            }
        });
    });
}
// Patch (or create) the sidecar for one item. kXmpKeep leaves a field alone.
static void writeSidecar(const std::string& raw, int rating, int label) {
    dispatch_async(g_sidecarQ, ^{
        std::string sc = sidecarPath(raw), existing;
        if (!readSidecarForPatch(sc, existing)) return;   // unreadable -> don't clobber
        std::string patched = xmpApply(existing, rating, label);
        FILE* f = fopen(sc.c_str(), "wb");
        if (f) { fwrite(patched.data(), 1, patched.size(), f); fclose(f); }
    });
}
// Write the image's FULL keyword list (replaces the sidecar's dc:subject bag).
// Same serial queue as rating/label writes, so ordering is preserved.
static void writeSidecarKeywords(const std::string& raw, std::vector<std::string> kws) {
    dispatch_async(g_sidecarQ, ^{
        std::string sc = sidecarPath(raw), existing;
        if (!readSidecarForPatch(sc, existing)) return;   // unreadable -> don't clobber
        std::string patched = xmpApplyKeywords(existing, kws);
        FILE* f = fopen(sc.c_str(), "wb");
        if (f) { fwrite(patched.data(), 1, patched.size(), f); fclose(f); }
    });
}

// ============================================================ folder access
// The macOS permission flow the owner asked for: let the user grant a folder
// (or a whole card/drive) via NSOpenPanel, persist a security-scoped bookmark so
// re-launch keeps that grant, and hold access open for the process lifetime.
@interface FolderAccess : NSObject
+ (NSString*)supportDir;
+ (NSURL*)promptForFolder;              // always asks; opens at the last folder
@end

@implementation FolderAccess
+ (NSString*)supportDir {
    NSArray* dirs = NSSearchPathForDirectoriesInDomains(
        NSApplicationSupportDirectory, NSUserDomainMask, YES);
    NSString* base = dirs.firstObject ?: NSTemporaryDirectory();
    NSString* dir = [base stringByAppendingPathComponent:@"iRate"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir
        withIntermediateDirectories:YES attributes:nil error:nil];
    return dir;
}
+ (NSString*)bookmarkPath {
    return [[self supportDir] stringByAppendingPathComponent:@"folder.bookmark"];
}
+ (void)saveBookmarkFor:(NSURL*)url {
    NSError* err = nil;
    NSData* bm = [url bookmarkDataWithOptions:NSURLBookmarkCreationWithSecurityScope
                  includingResourceValuesForKeys:nil relativeToURL:nil error:&err];
    if (bm) [bm writeToFile:[self bookmarkPath] atomically:YES];
}
// Resolve the last-used folder from the saved bookmark WITHOUT taking access —
// used only to point the picker at where you last worked.
+ (NSURL*)lastFolderURL {
    NSData* bm = [NSData dataWithContentsOfFile:[self bookmarkPath]];
    if (!bm) return nil;
    BOOL stale = NO; NSError* err = nil;
    return [NSURL URLByResolvingBookmarkData:bm
              options:NSURLBookmarkResolutionWithSecurityScope
              relativeToURL:nil bookmarkDataIsStale:&stale error:&err];
}
// Always ask which folder to cull (like the Windows build); the picker just opens
// at the last-used folder for convenience. Picking it grants access and re-saves
// the bookmark so next launch defaults here again.
+ (NSURL*)promptForFolder {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = NO;
    panel.canChooseDirectories = YES;
    panel.allowsMultipleSelection = NO;
    panel.prompt = @"Cull";
    panel.message = @"Choose the folder (or card / drive) of raws to cull.";
    NSURL* last = [self lastFolderURL];
    if (last) panel.directoryURL = last;
    if ([panel runModal] != NSModalResponseOK) return nil;
    NSURL* url = panel.URLs.firstObject;
    if (!url) return nil;
    [url startAccessingSecurityScopedResource];
    [self saveBookmarkFor:url];
    return url;
}
@end

// ============================================================ shared UI bits
// Match the Windows build's rating/label styling exactly.
static NSColor* nsLabelColor(int label) {
    switch (label) {
        case 1: return [NSColor colorWithSRGBRed:235/255.0 green:70/255.0  blue:60/255.0  alpha:1];
        case 2: return [NSColor colorWithSRGBRed:245/255.0 green:210/255.0 blue:60/255.0  alpha:1];
        case 3: return [NSColor colorWithSRGBRed:90/255.0  green:200/255.0 blue:90/255.0  alpha:1];
        case 4: return [NSColor colorWithSRGBRed:80/255.0  green:150/255.0 blue:245/255.0 alpha:1];
        case 5: return [NSColor colorWithSRGBRed:190/255.0 green:100/255.0 blue:230/255.0 alpha:1];
    }
    return [NSColor whiteColor];
}
static NSString* labelNameNS(int label) {
    switch (label) { case 1: return @"Red"; case 2: return @"Yellow"; case 3: return @"Green";
        case 4: return @"Blue"; case 5: return @"Purple"; }
    return @"";
}
static NSColor* kStarGold(void)  { return [NSColor colorWithSRGBRed:1 green:0.78 blue:0.16 alpha:1]; }
static NSColor* kRejectRed(void) { return [NSColor colorWithSRGBRed:235/255.0 green:70/255.0 blue:60/255.0 alpha:1]; }
// Always five glyphs: filled ★ up to the rating, hollow ☆ after (like Windows).
static NSString* starsString(int rating) {
    if (rating < 0) rating = 0;
    NSMutableString* s = [NSMutableString string];
    for (int i = 0; i < 5; i++) [s appendString:(i < rating) ? @"★" : @"☆"];
    return s;
}

// ============================================================ image view
@protocol IRGridController <NSObject>
- (void)toggleGrid;
- (void)openPrefs;
- (void)openFilter;
- (void)applyFilter;
- (void)setSortType:(int)t;
- (void)toggleSortDir;
- (void)jumpFirst;
- (void)jumpLast;
@end

// Grid top-toolbar hit rects (built each draw, tested on click).
struct TbBtn { CGRect rc; int id; };   // ids: 1 Prefs 2 Filter 3 Name 4 Date 5 Size 6 Dir 7 Full 8 Start 9 End
static std::vector<TbBtn> g_tbBtns;
static const CGFloat kGridTbH = 44;    // grid top toolbar height

// Filter panel chip hit rects (built each draw, tested on click).
struct FiltChip { CGRect rc; int kind; int val; };   // kind 0=rating 1=label 2=clear 3=close
static std::vector<FiltChip> g_filterChips;

// Prefs panel chip hit rects. kind: 0 advance 2 rebind 3 close 4 infoshown
// 5 infopos 6 session 7 clearcache 8 filetype 9 pairjpg
struct PChip { CGRect rc; int kind; int val; };
static std::vector<PChip> g_pchips;

// Keyword-sets editor hit rects. kind: 0 key chip 1 words field 2 close 3 clear key
struct KwChip { CGRect rc; int kind; int val; };
static std::vector<KwChip> g_kwChips;
static const char* kAllExts[] = { ".arw",".cr2",".cr3",".nef",".nrw",
                                  ".raf",".rw2",".pef",".dng",".orf",".jpg" };

@interface IRView : NSView
@property (nonatomic, assign) CGImageRef image;   // manually retained
@property (nonatomic, copy)   NSString* infoText;
@end

@implementation IRView
- (BOOL)isFlipped { return NO; }
- (BOOL)isOpaque  { return YES; }
- (void)setImage:(CGImageRef)image {
    if (image == _image) return;
    if (image) CGImageRetain(image);
    if (_image) CGImageRelease(_image);
    _image = image;
    [self setNeedsDisplay:YES];
}
- (void)dealloc { if (_image) CGImageRelease(_image); }

// ---- grid mouse: click selects, double-click opens; wheel scrolls the sheet ----
// Map a pointer y within the content area to a scroll offset (knob-centred).
- (void)scrollGridToPointerY:(CGFloat)ptY {
    CGFloat vw = self.bounds.size.width, availH = self.bounds.size.height - kGridTbH;
    int cols = std::max(1, (int)(vw / 240.0));
    CGFloat cellH = (vw / cols) * 0.72;
    int n = viewCount(), rows = (n + cols - 1) / cols;
    CGFloat contentH = rows * cellH, maxScroll = std::max((CGFloat)0, contentH - availH);
    if (maxScroll <= 0) return;
    CGFloat t = 1 - (ptY / availH);                    // top of track = scroll 0
    t = std::max((CGFloat)0, std::min((CGFloat)1, t));
    g_gridScrollY = t * maxScroll;
    [self setNeedsDisplay:YES];
}
- (BOOL)pointInScrollbar:(NSPoint)pt {
    CGFloat availH = self.bounds.size.height - kGridTbH;
    return pt.x >= self.bounds.size.width - 16 && pt.y >= 0 && pt.y <= availH;
}
- (void)mouseDragged:(NSEvent*)e {
    if (g_sbDragging) {
        NSPoint pt = [self convertPoint:e.locationInWindow fromView:nil];
        [self scrollGridToPointerY:pt.y];
    }
}
- (void)mouseUp:(NSEvent*)e { g_sbDragging = false; }

- (void)mouseDown:(NSEvent*)e {
    NSPoint pt = [self convertPoint:e.locationInWindow fromView:nil];
    id<IRGridController> d = (id<IRGridController>)[NSApp delegate];

    // modal panels (can be open in loupe or grid) take clicks first
    if (g_prefsOpen)  { [self prefsClickAt:pt];  return; }
    if (g_filterOpen) { [self filterClickAt:pt]; return; }
    if (g_kwOpen)     { [self kwClickAt:pt];     return; }
    if (!g_gridMode) return;

    // scrollbar grab (right edge, content area) — click-to-jump then drag
    if ([self pointInScrollbar:pt]) { g_sbDragging = true; [self scrollGridToPointerY:pt.y]; return; }

    // toolbar hit?
    for (auto& tb : g_tbBtns) {
        if (!CGRectContainsPoint(tb.rc, pt)) continue;
        switch (tb.id) {
            case 1: [d openPrefs]; break;
            case 2: [d openFilter]; break;
            case 7: [d toggleGrid]; break;
            case 8: [d jumpFirst]; break;
            case 9: [d jumpLast]; break;
            case 3: [d setSortType:0]; break;
            case 4: [d setSortType:1]; break;
            case 5: [d setSortType:2]; break;
            case 6: [d toggleSortDir]; break;
        }
        return;
    }

    // cell hit (content area is below the toolbar)
    if (viewCount() == 0) return;
    CGFloat vw = self.bounds.size.width, vh = self.bounds.size.height;
    int cols = std::max(1, (int)(vw / 240.0));
    CGFloat cellW = vw / cols, cellH = cellW * 0.72;
    CGFloat topInContent = (vh - kGridTbH - pt.y) + g_gridScrollY;
    if (topInContent < 0) return;                       // clicked in the toolbar gap
    int row = (int)(topInContent / cellH), col = (int)(pt.x / cellW);
    if (col < 0 || col >= cols || row < 0) return;
    int p = row * cols + col;
    if (p < 0 || p >= viewCount()) return;
    g_cur = p;
    if (e.clickCount >= 2) [d toggleGrid];              // double-click opens in loupe
    else [self setNeedsDisplay:YES];
}
- (void)scrollWheel:(NSEvent*)e {
    if (!g_gridMode) return;
    g_gridScrollY -= e.scrollingDeltaY;
    [self setNeedsDisplay:YES];
}
// Mouse-follow panning for the 100% zoom (matches the Windows build).
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    for (NSTrackingArea* ta in [self.trackingAreas copy]) [self removeTrackingArea:ta];
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:self.bounds
        options:NSTrackingMouseMoved | NSTrackingActiveAlways | NSTrackingInVisibleRect
        owner:self userInfo:nil]];
}
- (void)mouseMoved:(NSEvent*)e {
    if (!g_zoom) return;
    NSPoint pt = [self convertPoint:e.locationInWindow fromView:nil];
    CGFloat w = self.bounds.size.width, h = self.bounds.size.height;
    g_zoomCenter.x = std::max((CGFloat)0, std::min((CGFloat)1, pt.x / w));
    g_zoomCenter.y = std::max((CGFloat)0, std::min((CGFloat)1, 1 - pt.y / h));  // view y-up → image y-down
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty {
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    NSRect b = self.bounds;
    CGContextSetGrayFillColor(ctx, 0, 1);
    CGContextFillRect(ctx, b);

    if (g_gridMode) { [self drawGrid:ctx bounds:b];
        if (g_helpOpen) [self drawHelpOverlay:ctx bounds:b];
        if (g_prefsOpen) [self drawPrefs:ctx bounds:b];
        if (g_filterOpen) [self drawFilter:ctx bounds:b];
        if (g_kwOpen) [self drawKeywords:ctx bounds:b]; return; }

    if (_image) {
        if (g_zoom && g_zoomImage) {
            // True 100%: 1 full-res preview pixel per backing pixel, panned around
            // g_zoomCenter (follows the mouse). A single zoom level, like Windows.
            CGImageRef zi = g_zoomImage;
            CGFloat iw = CGImageGetWidth(zi), ih = CGImageGetHeight(zi);
            CGFloat scale = self.window.backingScaleFactor ?: 1;
            CGFloat vw = b.size.width * scale, vh = b.size.height * scale;   // image px shown
            CGFloat sx = std::min(iw, vw), sy = std::min(ih, vh);
            CGFloat ox = std::max((CGFloat)0, std::min(iw - sx, g_zoomCenter.x * iw - sx / 2));
            CGFloat oy = std::max((CGFloat)0, std::min(ih - sy, g_zoomCenter.y * ih - sy / 2));
            CGImageRef crop = CGImageCreateWithImageInRect(zi, CGRectMake(ox, oy, sx, sy));
            if (crop) {
                CGFloat dw = sx / scale, dh = sy / scale;
                CGRect dst = CGRectMake((b.size.width - dw) / 2, (b.size.height - dh) / 2, dw, dh);
                CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
                CGContextDrawImage(ctx, dst, crop);
                CGImageRelease(crop);
            }
        } else {
            // Fit view (also the zoom "loading" state — Windows keeps the fit image
            // up with a "100% loading…" label until the full-res decode lands).
            CGFloat iw = CGImageGetWidth(_image), ih = CGImageGetHeight(_image);
            CGFloat s = std::min(b.size.width / iw, b.size.height / ih);
            CGFloat dw = iw * s, dh = ih * s;
            CGRect dst = CGRectMake((b.size.width - dw) / 2, (b.size.height - dh) / 2, dw, dh);
            CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
            CGContextDrawImage(ctx, dst, _image);
        }
        if (g_zoom) {   // "100%" / "100% loading…" indicator, like Windows
            NSString* zt = g_zoomImage ? @"100%" : @"100% loading…";
            CGFloat zy = g_infoTop && g_showInfo ? b.size.height - 34 - 22 : b.size.height - 26;
            [zt drawAtPoint:NSMakePoint(12, zy) withAttributes:@{
                NSFontAttributeName: [NSFont systemFontOfSize:13],
                NSForegroundColorAttributeName: [NSColor whiteColor] }];
        }
    }

    if (g_showInfo && _infoText.length) {
        NSFont* f = [NSFont monospacedSystemFontOfSize:13 weight:NSFontWeightRegular];
        CGFloat pad = 12, barH = 30;
        CGFloat barY = g_infoTop ? (b.size.height - barH) : 0;   // top or bottom
        [[NSColor colorWithWhite:0 alpha:0.68] setFill];        // matches Windows 175/255
        NSRectFillUsingOperation(NSMakeRect(0, barY, b.size.width, barH),
                                 NSCompositingOperationSourceOver);
        CGFloat th = [@"Xg" sizeWithAttributes:@{NSFontAttributeName: f}].height;
        CGFloat ty = barY + (barH - th) / 2;

        // right side (drawn first so the left can truncate before it): always five
        // stars filled/hollow (gold), or ✖ Rejected (red); label ● Name in its colour.
        CGFloat rightStart = b.size.width - pad;
        int item = curItem();
        if (item >= 0) {
            Item& it = g_items[item];
            NSString* stars; NSColor* sc;
            if (it.rating == -1) { stars = @"✖ Rejected"; sc = kRejectRed(); }
            else { stars = starsString(it.rating); sc = kStarGold(); }
            NSDictionary* sa = @{ NSFontAttributeName: f, NSForegroundColorAttributeName: sc };
            NSSize ssz = [stars sizeWithAttributes:sa];
            CGFloat sx = b.size.width - pad - ssz.width;
            [stars drawAtPoint:NSMakePoint(sx, ty) withAttributes:sa];
            rightStart = sx;
            if (it.label > 0) {
                NSString* lt = [NSString stringWithFormat:@"● %@   ", labelNameNS(it.label)];
                NSDictionary* la = @{ NSFontAttributeName: f, NSForegroundColorAttributeName: nsLabelColor(it.label) };
                NSSize lsz = [lt sizeWithAttributes:la];
                [lt drawAtPoint:NSMakePoint(sx - lsz.width, ty) withAttributes:la];
                rightStart = sx - lsz.width;
            }
        }
        // left side: count · filename · [sort] · EXIF, truncated before the stars.
        NSMutableParagraphStyle* ps = [NSMutableParagraphStyle new];
        ps.lineBreakMode = NSLineBreakByTruncatingTail;
        NSDictionary* la = @{ NSFontAttributeName: f,
            NSForegroundColorAttributeName: [NSColor colorWithWhite:0.92 alpha:1],
            NSParagraphStyleAttributeName: ps };
        CGFloat leftW = std::max((CGFloat)0, rightStart - pad - 16);
        [_infoText drawInRect:NSMakeRect(pad, ty, leftW, th + 2) withAttributes:la];
    }

    if (g_helpOpen) [self drawHelpOverlay:ctx bounds:b];
    if (g_prefsOpen) [self drawPrefs:ctx bounds:b];
    if (g_filterOpen) [self drawFilter:ctx bounds:b];
    if (g_kwOpen) [self drawKeywords:ctx bounds:b];
}

// Shortcut help (Windows layout): title, a two-column grid of gold keys + gray
// labels generated from kActRows, and the reject/XMP footnote.
- (void)drawHelpOverlay:(CGContextRef)ctx bounds:(NSRect)b {
    int nRows = kNumActRows, perCol = (nRows + 1) / 2;
    CGFloat colW = 340, rowH = 26, pad = 24;
    CGFloat panelW = colW * 2 + pad * 2;
    CGFloat panelH = perCol * rowH + pad * 2 + rowH * 5;   // + title + footer + donation line
    CGFloat px = (b.size.width - panelW) / 2, py = (b.size.height - panelH) / 2;
    [[NSColor colorWithSRGBRed:0.094 green:0.094 blue:0.102 alpha:0.98] setFill];
    NSRectFill(NSMakeRect(px, py, panelW, panelH));
    [[NSColor colorWithWhite:0.43 alpha:1] setStroke];
    NSBezierPath* frame = [NSBezierPath bezierPathWithRect:NSMakeRect(px, py, panelW, panelH)];
    frame.lineWidth = 1; [frame stroke];

    NSFont* f = [NSFont systemFontOfSize:14];
    NSDictionary* keyA = @{ NSFontAttributeName: f, NSForegroundColorAttributeName: kStarGold() };
    NSDictionary* labA = @{ NSFontAttributeName: f, NSForegroundColorAttributeName: [NSColor colorWithWhite:0.82 alpha:1] };

    // title (centred)
    std::string pk = keysOfAction(A_PREFS);
    NSString* title = [NSString stringWithFormat:@"Keyboard shortcuts — rebind in Preferences (%@)",
        pk.empty() ? @"toolbar" : [NSString stringWithUTF8String:pk.c_str()]];
    NSDictionary* tA = @{ NSFontAttributeName: [NSFont systemFontOfSize:15],
        NSForegroundColorAttributeName: [NSColor colorWithWhite:0.92 alpha:1] };
    NSSize tsz = [title sizeWithAttributes:tA];
    CGFloat titleY = py + panelH - pad - tsz.height;
    [title drawAtPoint:NSMakePoint(px + (panelW - tsz.width) / 2, titleY) withAttributes:tA];

    CGFloat gridTop = titleY - rowH * 0.8;
    for (int i = 0; i < nRows; i++) {
        int col = i / perCol, rw = i % perCol;
        CGFloat x = px + pad + col * colW;
        CGFloat y = gridTop - rowH - rw * rowH;
        std::string keys = keysOfAction(kActRows[i].a);
        NSString* ks = keys.empty() ? @"—" : [NSString stringWithUTF8String:keys.c_str()];
        [ks drawAtPoint:NSMakePoint(x, y) withAttributes:keyA];
        [[NSString stringWithUTF8String:kActRows[i].label]
            drawAtPoint:NSMakePoint(x + 116, y) withAttributes:labA];
    }
    NSMutableParagraphStyle* ps = [NSMutableParagraphStyle new]; ps.alignment = NSTextAlignmentCenter;
    NSString* foot = @"Reject (✖) = xmp:Rating \"-1\" — Bridge / Photo Mechanic only; "
                      "Lightroom ignores XMP rejects (its flags are catalog-only)";
    [foot drawInRect:NSMakeRect(px + pad, py + pad * 0.5 + rowH * 1.3, panelW - pad * 2, rowH * 2)
        withAttributes:@{ NSFontAttributeName: [NSFont systemFontOfSize:12],
            NSForegroundColorAttributeName: [NSColor colorWithWhite:0.6 alpha:1],
            NSParagraphStyleAttributeName: ps }];
    // donationware ask (gold), mirrors the Windows help footer
    NSString* dona = @"iRate is donationware — if it saves your evenings: buymeacoffee.com/lorasandlenses";
    [dona drawInRect:NSMakeRect(px + pad, py + pad * 0.4, panelW - pad * 2, rowH)
        withAttributes:@{ NSFontAttributeName: [NSFont systemFontOfSize:12.5],
            NSForegroundColorAttributeName: kStarGold(),
            NSParagraphStyleAttributeName: ps }];
}

// Preferences (Windows layout): option chips at the top (after-rating mode, file
// types, info bar, RAW+JPG, clear cache) then a two-column key-rebind grid — click
// an action, press its new key. Everything applies + saves to the ini immediately.
- (void)drawPrefs:(CGContextRef)ctx bounds:(NSRect)b {
    g_pchips.clear();
    int perCol = (kNumActRows + 1) / 2;
    CGFloat colW = 430, pad = 20, optH = 38, chipH = 28, rowH = 26, labW = 108;
    CGFloat panelW = colW * 2 + pad * 2;                    // ~920, fits the 11 ext chips
    CGFloat panelH = pad * 2 + optH * 6 + rowH * perCol + 44;
    CGFloat px = (b.size.width - panelW) / 2;
    CGFloat py = std::max((CGFloat)16, (b.size.height - panelH) / 2);
    [[NSColor colorWithSRGBRed:0.094 green:0.094 blue:0.102 alpha:0.98] setFill];
    NSRectFill(NSMakeRect(px, py, panelW, panelH));
    [[NSColor colorWithWhite:0.43 alpha:1] setStroke];
    NSBezierPath* fr = [NSBezierPath bezierPathWithRect:NSMakeRect(px, py, panelW, panelH)];
    fr.lineWidth = 1; [fr stroke];

    NSDictionary* labA = @{ NSFontAttributeName: [NSFont systemFontOfSize:13],
        NSForegroundColorAttributeName: [NSColor colorWithWhite:0.72 alpha:1] };
    NSColor* gold = kStarGold();

    CGFloat top = py + panelH - 14;                         // running top-anchored y
    // title (centred)
    NSString* title = @"Preferences — changes apply and save immediately";
    NSDictionary* tA = @{ NSFontAttributeName: [NSFont systemFontOfSize:15],
        NSForegroundColorAttributeName: [NSColor colorWithWhite:0.92 alpha:1] };
    NSSize tsz = [title sizeWithAttributes:tA];
    [title drawAtPoint:NSMakePoint(px + (panelW - tsz.width) / 2, top - tsz.height) withAttributes:tA];
    __block CGFloat cy = top - optH;   // band top; __block so the chip helpers see each row's y

    // helper to draw a chip at (x, band) and record its hit rect
    __block CGFloat bx;
    void (^label)(NSString*) = ^(NSString* t) {
        [t drawAtPoint:NSMakePoint(px + pad, cy - chipH + (chipH - 16) / 2) withAttributes:labA];
        bx = px + pad + labW;
    };
    CGFloat (^chip)(NSString*, CGFloat, BOOL, int, int) =
        ^CGFloat(NSString* t, CGFloat w, BOOL on, int kind, int val) {
        CGRect rc = CGRectMake(bx, cy - chipH, w, chipH);
        [self drawChip:rc text:t on:on dot:nil];
        g_pchips.push_back({ rc, kind, val });
        bx = rc.origin.x + w + 8;
        return bx;
    };

    // After rating
    label(@"After rating");
    NSString* adv[3] = { @"Caps Lock gated", @"Always advance", @"Never" };
    for (int i = 0; i < 3; i++) chip(adv[i], 140, g_advanceMode == i, 0, i);
    cy -= optH;
    // File types
    label(@"File types");
    for (int i = 0; i < 11; i++) chip([NSString stringWithUTF8String:kAllExts[i]], 52,
        extEnabled(kAllExts[i]), 8, i);
    cy -= optH;
    // Info bar
    label(@"Info bar");
    chip(g_showInfo ? @"Shown" : @"Hidden", 90, g_showInfo, 4, 0);
    chip(g_infoTop ? @"Top" : @"Bottom", 90, NO, 5, 0);
    bx += 16;
    chip(g_sessionInFolder ? @"Resume: in folder" : @"Resume: central", 170, g_sessionInFolder, 6, 0);
    cy -= optH;
    // RAW+JPG + clear cache
    label(@"RAW+JPG");
    chip(g_pairRawJpg ? @"Paired (+JPG)" : @"Separate", 130, g_pairRawJpg, 9, 0);
    bx += 16;
    double mb = tcSizeBytes() / (1024.0 * 1024.0);
    NSString* cc = mb >= 1024 ? [NSString stringWithFormat:@"Clear thumb cache (%.1f GB)", mb / 1024]
                              : [NSString stringWithFormat:@"Clear thumb cache (%.0f MB)", mb];
    chip(cc, 220, NO, 7, 0);
    cy -= optH;

    // key rebind header (or a conflict notice after a reassignment)
    NSString* kh = g_prefsCaptureAction >= 0 ? @"Press the new key…  (Esc cancels)"
                 : g_prefsMsg ? g_prefsMsg
                              : @"Keys — click an action, then press its new key:";
    [kh drawAtPoint:NSMakePoint(px + pad, cy - 18) withAttributes:@{
        NSFontAttributeName: [NSFont systemFontOfSize:13],
        NSForegroundColorAttributeName: g_prefsMsg && g_prefsCaptureAction < 0
            ? kStarGold() : [NSColor colorWithWhite:0.72 alpha:1] }];
    cy -= rowH;

    // two-column rebind grid
    NSDictionary* keyA = @{ NSFontAttributeName: [NSFont systemFontOfSize:13], NSForegroundColorAttributeName: gold };
    NSDictionary* rowLab = @{ NSFontAttributeName: [NSFont systemFontOfSize:13],
        NSForegroundColorAttributeName: [NSColor colorWithWhite:0.82 alpha:1] };
    for (int i = 0; i < kNumActRows; i++) {
        int col = i / perCol, rw = i % perCol;
        CGFloat rx = px + pad + col * colW;
        CGFloat ry = cy - rowH - rw * rowH;
        CGRect rowRc = CGRectMake(rx, ry, colW - 14, rowH - 2);
        if (g_prefsCaptureAction == (int)kActRows[i].a) {
            [[NSColor colorWithSRGBRed:0.27 green:0.43 blue:0.70 alpha:1] setFill];
            [[NSBezierPath bezierPathWithRoundedRect:rowRc xRadius:4 yRadius:4] fill];
        }
        std::string keys = keysOfAction(kActRows[i].a);
        NSString* ks = keys.empty() ? @"—" : [NSString stringWithUTF8String:keys.c_str()];
        [ks drawAtPoint:NSMakePoint(rx + 6, ry + 3) withAttributes:keyA];
        [[NSString stringWithUTF8String:kActRows[i].label]
            drawAtPoint:NSMakePoint(rx + 120, ry + 3) withAttributes:rowLab];
        g_pchips.push_back({ rowRc, 2, (int)kActRows[i].a });
    }
    // close
    CGRect closeR = CGRectMake(px + panelW - pad - 100, py + pad * 0.6, 100, chipH);
    [self drawChip:closeR text:@"Close (Esc)" on:NO dot:nil];
    g_pchips.push_back({ closeR, 3, 0 });
}
// Handle a click in the prefs panel. Returns YES if it hit a chip/row.
- (BOOL)prefsClickAt:(NSPoint)pt {
    for (auto& c : g_pchips) {
        if (!CGRectContainsPoint(c.rc, pt)) continue;
        switch (c.kind) {
            case 0: g_advanceMode = c.val; break;
            case 4: g_showInfo = !g_showInfo; break;
            case 5: g_infoTop = !g_infoTop; break;
            case 6: g_sessionInFolder = !g_sessionInFolder; break;
            case 7: tcClear(); break;
            case 8: {                                       // toggle a file type
                std::string e = kAllExts[c.val];
                auto it = std::find(g_exts.begin(), g_exts.end(), e);
                if (it != g_exts.end()) g_exts.erase(it); else g_exts.push_back(e);
                break; }
            case 9: g_pairRawJpg = !g_pairRawJpg; break;
            case 2: g_prefsCaptureAction = c.val; g_prefsMsg = nil; [self setNeedsDisplay:YES]; return YES;  // no ini write
            case 3: g_prefsOpen = false; [self setNeedsDisplay:YES]; return YES;
        }
        writeIni();
        [self setNeedsDisplay:YES];
        return YES;
    }
    return NO;
}

// Keyword-sets editor (K). One dedicated screen: 10 slots, each = a key chip
// (click, then press the key that will toggle this set while culling), a ✕ to
// unbind, and a keywords field (click, type; comma-separated). Saved to the
// APP-LEVEL ini ([keywords] section) on every change — deliberately NOT
// per-folder, so your sets follow you across shoots.
- (void)drawKeywords:(CGContextRef)ctx bounds:(NSRect)b {
    g_kwChips.clear();
    CGFloat panelW = 760, rowH = 36, chipH = 28, pad = 20;
    CGFloat panelH = kKwSlots * rowH + rowH * 4 + pad * 2;
    CGFloat px = (b.size.width - panelW) / 2;
    CGFloat py = std::max((CGFloat)16, (b.size.height - panelH) / 2);
    [[NSColor colorWithSRGBRed:0.094 green:0.094 blue:0.102 alpha:0.98] setFill];
    NSRectFill(NSMakeRect(px, py, panelW, panelH));
    [[NSColor colorWithWhite:0.43 alpha:1] setStroke];
    NSBezierPath* fr = [NSBezierPath bezierPathWithRect:NSMakeRect(px, py, panelW, panelH)];
    fr.lineWidth = 1; [fr stroke];

    NSFont* f13 = [NSFont systemFontOfSize:13];
    NSDictionary* labA = @{ NSFontAttributeName: f13,
        NSForegroundColorAttributeName: [NSColor colorWithWhite:0.72 alpha:1] };

    // title (centred)
    NSString* title = @"Keyword sets — press a set's key while culling to toggle its keywords";
    NSDictionary* tA = @{ NSFontAttributeName: [NSFont systemFontOfSize:15],
        NSForegroundColorAttributeName: [NSColor colorWithWhite:0.92 alpha:1] };
    NSSize tsz = [title sizeWithAttributes:tA];
    CGFloat top = py + panelH - 14;
    [title drawAtPoint:NSMakePoint(px + (panelW - tsz.width) / 2, top - tsz.height) withAttributes:tA];

    CGFloat y = top - rowH * 1.6;
    for (int i = 0; i < kKwSlots; i++) {
        CGFloat ry = y - chipH;
        // slot number
        [[NSString stringWithFormat:@"Set %d", i + 1]
            drawAtPoint:NSMakePoint(px + pad, ry + (chipH - 16) / 2) withAttributes:labA];
        // key chip
        NSString* kt;
        if (g_kwCaptureSlot == i) kt = @"press key…";
        else if (g_kwSlots[i].bound) kt = [NSString stringWithUTF8String:keyDisplayName(g_kwSlots[i].key).c_str()];
        else kt = @"(no key)";
        CGRect keyR = CGRectMake(px + pad + 58, ry, 110, chipH);
        [self drawChip:keyR text:kt on:(g_kwCaptureSlot == i) dot:nil];
        g_kwChips.push_back({ keyR, 0, i });
        // clear-key ✕
        CGRect clrR = CGRectMake(keyR.origin.x + 116, ry, 30, chipH);
        [self drawChip:clrR text:@"✕" on:NO dot:nil];
        g_kwChips.push_back({ clrR, 3, i });
        // words field
        CGRect tf = CGRectMake(clrR.origin.x + 38, ry, px + panelW - pad - (clrR.origin.x + 38), chipH);
        [[NSColor colorWithWhite:(g_kwEditSlot == i ? 0.20 : 0.16) alpha:1] setFill]; NSRectFill(tf);
        [(g_kwEditSlot == i ? [NSColor colorWithSRGBRed:0.27 green:0.43 blue:0.70 alpha:1]
                            : [NSColor colorWithWhite:0.45 alpha:1]) setStroke];
        NSBezierPath* tb = [NSBezierPath bezierPathWithRect:tf]; tb.lineWidth = 1; [tb stroke];
        NSString* wt;
        if (!g_kwSlots[i].words.empty())
            wt = [[NSString stringWithUTF8String:g_kwSlots[i].words.c_str()] ?: @""
                  stringByAppendingString:(g_kwEditSlot == i ? @"▏" : @"")];
        else wt = (g_kwEditSlot == i) ? @"▏" : @"(keywords, comma separated)";
        NSDictionary* wa = @{ NSFontAttributeName: f13, NSForegroundColorAttributeName:
            g_kwSlots[i].words.empty() && g_kwEditSlot != i ? [NSColor colorWithWhite:0.5 alpha:1] : [NSColor whiteColor] };
        [wt drawAtPoint:NSMakePoint(tf.origin.x + 8, ry + (chipH - 16) / 2) withAttributes:wa];
        g_kwChips.push_back({ tf, 1, i });
        y -= rowH;
    }

    // footer: message / hint + Close
    NSString* hint = g_kwMsg ?: @"Click a key chip then press a key · click a field to type · saved in app settings";
    [hint drawAtPoint:NSMakePoint(px + pad, py + pad * 0.5)
        withAttributes:@{ NSFontAttributeName: [NSFont systemFontOfSize:12],
            NSForegroundColorAttributeName: g_kwMsg ? kStarGold() : [NSColor colorWithWhite:0.6 alpha:1] }];
    CGRect closeR = CGRectMake(px + panelW - pad - 96, py + pad * 0.4, 96, chipH);
    [self drawChip:closeR text:@"Close (Esc)" on:NO dot:nil];
    g_kwChips.push_back({ closeR, 2, 0 });
}
// Click routing for the keyword editor. Returns YES if it hit anything.
- (BOOL)kwClickAt:(NSPoint)pt {
    for (auto& c : g_kwChips) {
        if (!CGRectContainsPoint(c.rc, pt)) continue;
        if (c.kind == 0)      { g_kwCaptureSlot = c.val; g_kwEditSlot = -1; g_kwMsg = nil; }
        else if (c.kind == 1) { g_kwEditSlot = c.val; g_kwCaptureSlot = -1; g_kwMsg = nil; }
        else if (c.kind == 3) { g_kwSlots[c.val].bound = false; writeIni(); }
        else if (c.kind == 2) { g_kwOpen = false; g_kwEditSlot = g_kwCaptureSlot = -1; writeIni(); }
        [self setNeedsDisplay:YES];
        return YES;
    }
    if (g_kwEditSlot >= 0) { g_kwEditSlot = -1; writeIni(); [self setNeedsDisplay:YES]; }
    return NO;
}

// Windows-style filter panel: a "Rating" row of toggle chips (0 1★…5★ ✖), a
// "Label" row (None + 5 colours), a "Text" field, and a bottom row with the
// shown/total count + Clear + Close. Chips highlight when that value is SHOWN.
// Mouse-driven (click chips); type for the text field. Applies live.
- (void)drawChip:(CGRect)rc text:(NSString*)text on:(BOOL)on dot:(NSColor*)dot {
    [(on ? [NSColor colorWithSRGBRed:0.27 green:0.43 blue:0.70 alpha:1]
         : [NSColor colorWithWhite:0.24 alpha:1]) setFill];
    [[NSBezierPath bezierPathWithRoundedRect:rc xRadius:5 yRadius:5] fill];
    if (!on) { [[NSColor colorWithWhite:0.42 alpha:1] setStroke];
        NSBezierPath* p = [NSBezierPath bezierPathWithRoundedRect:rc xRadius:5 yRadius:5];
        p.lineWidth = 1; [p stroke]; }
    NSDictionary* ta = @{ NSFontAttributeName: [NSFont systemFontOfSize:13],
        NSForegroundColorAttributeName: on ? [NSColor whiteColor] : [NSColor colorWithWhite:0.85 alpha:1] };
    CGFloat tx = rc.origin.x + 10;
    if (dot) {
        NSDictionary* da = @{ NSFontAttributeName: [NSFont systemFontOfSize:13], NSForegroundColorAttributeName: dot };
        [@"●" drawAtPoint:NSMakePoint(tx, rc.origin.y + (rc.size.height - 16) / 2) withAttributes:da];
        tx += 16;
    }
    NSSize ts = [text sizeWithAttributes:ta];
    [text drawAtPoint:NSMakePoint(tx, rc.origin.y + (rc.size.height - ts.height) / 2) withAttributes:ta];
}
- (void)drawFilter:(CGContextRef)ctx bounds:(NSRect)b {
    g_filterChips.clear();
    CGFloat panelW = 720, rowH = 40, chipH = 28, padX = 20, padY = 18;
    CGFloat panelH = rowH * 4 + padY * 2 + 8;
    CGFloat px = (b.size.width - panelW) / 2, py = (b.size.height - panelH) / 2;
    [[NSColor colorWithWhite:0.09 alpha:0.98] setFill];
    NSRectFillUsingOperation(NSMakeRect(px, py, panelW, panelH), NSCompositingOperationSourceOver);
    [[NSColor colorWithWhite:0.4 alpha:1] setStroke];
    NSBezierPath* fr = [NSBezierPath bezierPathWithRect:NSMakeRect(px, py, panelW, panelH)];
    fr.lineWidth = 1; [fr stroke];

    NSDictionary* lab = @{ NSFontAttributeName: [NSFont systemFontOfSize:14],
        NSForegroundColorAttributeName: [NSColor colorWithWhite:0.78 alpha:1] };
    CGFloat labW = 60;

    // rows are laid out top-down; convert to bottom-left y per row
    CGFloat topY = py + panelH - padY;
    auto rowY = [&](int r) { return topY - chipH - r * rowH; };

    // --- Rating row ---
    [@"Rating" drawAtPoint:NSMakePoint(px + padX, rowY(0) + (chipH - 16) / 2) withAttributes:lab];
    CGFloat x = px + padX + labW;
    NSString* rlabels[7] = { @"0", @"1★", @"2★", @"3★", @"4★", @"5★", @"✖" };
    for (int i = 0; i <= 6; i++) {
        CGFloat w = (i == 6) ? 42 : 50;
        CGRect rc = CGRectMake(x, rowY(0), w, chipH);
        [self drawChip:rc text:rlabels[i] on:((g_ratingMask >> i) & 1) dot:nil];
        g_filterChips.push_back({ rc, 0, i });
        x = rc.origin.x + w + 6;
    }
    // --- Label row ---
    [@"Label" drawAtPoint:NSMakePoint(px + padX, rowY(1) + (chipH - 16) / 2) withAttributes:lab];
    x = px + padX + labW;
    NSString* llabels[6] = { @"None", @"Red", @"Yellow", @"Green", @"Blue", @"Purple" };
    for (int i = 0; i <= 5; i++) {
        CGFloat w = (i == 0) ? 58 : 78;
        CGRect rc = CGRectMake(x, rowY(1), w, chipH);
        [self drawChip:rc text:llabels[i] on:((g_labelMask >> i) & 1) dot:(i > 0 ? nsLabelColor(i) : nil)];
        g_filterChips.push_back({ rc, 1, i });
        x = rc.origin.x + w + 6;
    }
    // --- Text field ---
    [@"Text" drawAtPoint:NSMakePoint(px + padX, rowY(2) + (chipH - 16) / 2) withAttributes:lab];
    CGRect tf = CGRectMake(px + padX + labW, rowY(2), panelW - padX * 2 - labW, chipH);
    [[NSColor colorWithWhite:0.16 alpha:1] setFill]; NSRectFill(tf);
    [[NSColor colorWithWhite:0.45 alpha:1] setStroke];
    NSBezierPath* tfb = [NSBezierPath bezierPathWithRect:tf]; tfb.lineWidth = 1; [tfb stroke];
    NSString* ttext = g_filterText.empty() ? @"(filename contains…)"
        : [[NSString stringWithUTF8String:g_filterText.c_str()] ?: @"" stringByAppendingString:@"▏"];
    NSDictionary* ta = @{ NSFontAttributeName: [NSFont systemFontOfSize:13],
        NSForegroundColorAttributeName: g_filterText.empty() ? [NSColor colorWithWhite:0.5 alpha:1] : [NSColor whiteColor] };
    [ttext drawAtPoint:NSMakePoint(tf.origin.x + 8, tf.origin.y + (chipH - 16) / 2) withAttributes:ta];

    // --- bottom row: status + Clear + Close ---
    NSString* status = filterActive()
        ? [NSString stringWithFormat:@"%d of %d shown", viewCount(), (int)g_items.size()]
        : @"showing all";
    [status drawAtPoint:NSMakePoint(px + padX, rowY(3) + (chipH - 16) / 2)
        withAttributes:@{ NSFontAttributeName: [NSFont systemFontOfSize:13],
            NSForegroundColorAttributeName: [NSColor colorWithWhite:0.66 alpha:1] }];
    CGRect closeR = CGRectMake(px + panelW - padX - 96, rowY(3), 96, chipH);
    [self drawChip:closeR text:@"Close (F)" on:NO dot:nil];
    g_filterChips.push_back({ closeR, 3, 0 });
    CGRect clearR = CGRectMake(closeR.origin.x - 78, rowY(3), 72, chipH);
    [self drawChip:clearR text:@"Clear" on:NO dot:nil];
    g_filterChips.push_back({ clearR, 2, 0 });
}
// Toggle/clear/close from a filter-panel click. Returns YES if it hit a chip.
- (BOOL)filterClickAt:(NSPoint)pt {
    for (auto& c : g_filterChips) {
        if (!CGRectContainsPoint(c.rc, pt)) continue;
        if (c.kind == 0)      g_ratingMask ^= (1u << c.val);
        else if (c.kind == 1) g_labelMask  ^= (1u << c.val);
        else if (c.kind == 2) { g_ratingMask = 0x7F; g_labelMask = 0x3F; g_filterText.clear(); }
        else if (c.kind == 3) { g_filterOpen = false; [self setNeedsDisplay:YES]; return YES; }
        [(id<IRGridController>)[NSApp delegate] applyFilter];
        return YES;
    }
    return NO;
}

// Contact-sheet grid. View is non-flipped (origin bottom-left); the sheet is laid
// out top-down and flipped per cell. Visible cells request their thumbnail on the
// fly; g_cur is the selected cell.
- (CGFloat)gridCellH { return [self gridCellW] * 0.72; }
- (CGFloat)gridCellW {
    CGFloat vw = self.bounds.size.width;
    int cols = std::max(1, (int)(vw / 240.0));   // ~240px target cell
    g_gridCols = cols;
    return vw / cols;
}
// Windows-style top toolbar: Prefs / Filter / Fullscreen / Start / End, then the
// sort buttons, then count + filename on the right. Records hit rects in g_tbBtns.
- (void)drawGridToolbar:(CGContextRef)ctx bounds:(NSRect)b {
    g_tbBtns.clear();
    CGFloat barY = b.size.height - kGridTbH;
    [[NSColor colorWithWhite:0.11 alpha:1] setFill];
    NSRectFill(NSMakeRect(0, barY, b.size.width, kGridTbH));
    NSFont* f = [NSFont systemFontOfSize:13];
    __block CGFloat bx = 10;
    void (^addBtn)(NSString*, int, BOOL) = ^(NSString* title, int bid, BOOL active) {
        NSDictionary* ta = @{ NSFontAttributeName: f, NSForegroundColorAttributeName:
            active ? [NSColor whiteColor] : [NSColor colorWithWhite:0.82 alpha:1] };
        NSSize ts = [title sizeWithAttributes:ta];
        CGFloat w = ts.width + 20;
        CGRect r = CGRectMake(bx, barY + 6, w, kGridTbH - 12);
        [(active ? [NSColor colorWithSRGBRed:0.27 green:0.43 blue:0.70 alpha:1]
                 : [NSColor colorWithWhite:0.22 alpha:1]) setFill];
        [[NSBezierPath bezierPathWithRoundedRect:r xRadius:5 yRadius:5] fill];
        [title drawAtPoint:NSMakePoint(bx + 10, barY + (kGridTbH - ts.height) / 2) withAttributes:ta];
        g_tbBtns.push_back({ r, bid });
        bx += w + 8;
    };
    addBtn(@"⚙ Prefs", 1, NO);
    addBtn(filterActive() ? @"▼ Filter ●" : @"▼ Filter", 2, g_filterOpen || filterActive());
    addBtn(@"⛶ Fullscreen", 7, NO);
    addBtn(@"⇤ Start", 8, NO);
    addBtn(@"End ⇥", 9, NO);
    bx += 12;
    addBtn(@"Name", 3, g_sortType == 0);
    addBtn(@"Date", 4, g_sortType == 1);
    addBtn(@"Size", 5, g_sortType == 2);
    addBtn(g_sortDesc ? @"↓ desc" : @"↑ asc", 6, NO);

    int n = viewCount();
    NSString* fname = @"";
    if (n > 0) { int it = curItem(); if (it >= 0)
        fname = [[NSString stringWithUTF8String:g_items[it].path.c_str()] lastPathComponent]; }
    NSString* info = filterActive()
        ? [NSString stringWithFormat:@"%@   %d / %d (of %d)", fname, n ? g_cur + 1 : 0, n, (int)g_items.size()]
        : [NSString stringWithFormat:@"%@   %d / %d", fname, n ? g_cur + 1 : 0, n];
    NSDictionary* ia = @{ NSFontAttributeName: f, NSForegroundColorAttributeName: [NSColor colorWithWhite:0.82 alpha:1] };
    NSSize is = [info sizeWithAttributes:ia];
    [info drawAtPoint:NSMakePoint(b.size.width - 12 - is.width, barY + (kGridTbH - is.height) / 2) withAttributes:ia];
}
- (void)drawGrid:(CGContextRef)ctx bounds:(NSRect)b {
    [self drawGridToolbar:ctx bounds:b];
    CGFloat availH = b.size.height - kGridTbH;             // cells live below the bar
    if (viewCount() == 0) {
        NSString* msg = filterActive() ? @"No images match the filter — press F to adjust" : @"No images";
        NSDictionary* ma = @{ NSFontAttributeName: [NSFont systemFontOfSize:16],
            NSForegroundColorAttributeName: [NSColor colorWithWhite:0.66 alpha:1] };
        NSSize ms = [msg sizeWithAttributes:ma];
        [msg drawAtPoint:NSMakePoint((b.size.width - ms.width) / 2, availH / 2) withAttributes:ma];
        return;
    }
    CGFloat cellW = [self gridCellW], cellH = [self gridCellH];
    int cols = g_gridCols, n = viewCount();
    int rows = (n + cols - 1) / cols;
    CGFloat contentH = rows * cellH;
    CGFloat maxScroll = std::max((CGFloat)0, contentH - availH);
    if (g_gridScrollY > maxScroll) g_gridScrollY = maxScroll;
    if (g_gridScrollY < 0) g_gridScrollY = 0;
    int gen = g_generation.load();

    // Publish the "worth decoding" window (visible rows ± 3) for requestThumb.
    int firstRow = std::max(0, (int)(g_gridScrollY / cellH) - 3);
    int lastRow  = (int)((g_gridScrollY + availH) / cellH) + 3;
    g_visLo.store(firstRow * cols);
    g_visHi.store((lastRow + 1) * cols - 1);

    NSDictionary* capAttr = @{ NSFontAttributeName: [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightBold] };

    for (int p = 0; p < n; p++) {
        int item = g_view[p];
        int row = p / cols, col = p % cols;
        CGFloat topInContent = row * cellH - g_gridScrollY;
        if (topInContent + cellH <= 0 || topInContent >= availH) continue;   // offscreen
        CGFloat x = col * cellW;
        CGFloat y = availH - topInContent - cellH;         // bottom-left, below toolbar
        CGFloat pad = 6;
        CGRect cell = CGRectMake(x + pad, y + pad, cellW - pad * 2, cellH - pad * 2);

        if (p == g_cur) {
            [[NSColor colorWithSRGBRed:0.27 green:0.43 blue:0.70 alpha:0.55] setFill];
            NSRectFill(NSMakeRect(x + 2, y + 2, cellW - 4, cellH - 4));
        }

        CGFloat strip = 20;                                // caption strip (stars/label)
        CGRect ibox = CGRectMake(cell.origin.x, cell.origin.y + strip,
                                 cell.size.width, cell.size.height - strip);
        [[NSColor colorWithWhite:(p == g_cur ? 0.24 : 0.16) alpha:1] setFill];
        NSRectFill(ibox);

        CGImageRef th = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_thumbMx);
            auto f = g_thumbCache.find(item);
            if (f != g_thumbCache.end() && f->second) th = CGImageRetain(f->second);
        }
        if (!th) { requestThumb(item, g_items[item].path,
                                g_items[item].mtime, g_items[item].fsize, gen, p); }
        else {
            CGFloat iw = CGImageGetWidth(th), ih = CGImageGetHeight(th);
            CGFloat s = std::min(ibox.size.width / iw, ibox.size.height / ih);
            CGFloat dw = iw * s, dh = ih * s;
            CGRect dst = CGRectMake(ibox.origin.x + (ibox.size.width - dw) / 2,
                                    ibox.origin.y + (ibox.size.height - dh) / 2, dw, dh);
            CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
            CGContextDrawImage(ctx, dst, th);
            CGImageRelease(th);
        }

        // caption: always five stars (gold, filled/hollow) or ✖ (red); label ● dot.
        Item& it = g_items[item];
        NSString* cap; NSColor* cc;
        if (it.rating == -1) { cap = @"✖"; cc = kRejectRed(); }
        else { cap = starsString(it.rating); cc = kStarGold(); }
        NSMutableDictionary* ca = [capAttr mutableCopy]; ca[NSForegroundColorAttributeName] = cc;
        [cap drawAtPoint:NSMakePoint(cell.origin.x + 3, cell.origin.y + 2) withAttributes:ca];
        if (it.label > 0) {
            NSMutableDictionary* da = [capAttr mutableCopy]; da[NSForegroundColorAttributeName] = nsLabelColor(it.label);
            NSString* dot = @"●"; NSSize dz = [dot sizeWithAttributes:da];
            [dot drawAtPoint:NSMakePoint(cell.origin.x + cell.size.width - dz.width - 3, cell.origin.y + 2) withAttributes:da];
        }
    }

    // proportional scrollbar within the content area (below the toolbar)
    if (maxScroll > 0) {
        CGFloat sbW = 6, sbX = b.size.width - sbW - 2;
        [[NSColor colorWithWhite:1 alpha:0.08] setFill];
        NSRectFill(NSMakeRect(sbX, 0, sbW, availH));
        CGFloat knobH = std::max((CGFloat)40, availH * (availH / contentH));
        CGFloat t = g_gridScrollY / maxScroll;
        CGFloat knobY = (availH - knobH) * (1 - t);
        [[NSColor colorWithWhite:1 alpha:0.3] setFill];
        NSRectFill(NSMakeRect(sbX, knobY, sbW, knobH));
    }
}
@end

// ============================================================ app / window
@interface AppDelegate : NSObject <NSApplicationDelegate>
@property (nonatomic, strong) NSWindow* window;
@property (nonatomic, strong) IRView* view;
@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)n {
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    // ---- config (keybinds + options) before anything reads them ----
    loadConfig([FolderAccess supportDir].UTF8String);

    // ---- resolve folder access (saved grant, else prompt) ----
    g_folderURL = [FolderAccess promptForFolder];   // always ask which folder
    if (!g_folderURL) { [NSApp terminate:nil]; return; }

    g_sidecarQ = dispatch_queue_create("com.shootthesound.irate.sidecar", DISPATCH_QUEUE_SERIAL);
    NSScreen* mainScr = [NSScreen mainScreen];
    g_maxPix = (int)(std::max(mainScr.frame.size.width, mainScr.frame.size.height)
                     * mainScr.backingScaleFactor);

    // ---- restore per-folder session (sort + last file) before the scan ----
    g_folderPath = g_folderURL.path.UTF8String;
    std::string resumeRel;
    if (g_sessionInFolder) {
        KV s = sessionRead();
        g_sortType = atoi(s.get("sorttype", std::to_string(g_sortType).c_str()).c_str());
        if (g_sortType < 0 || g_sortType > 2) g_sortType = 0;
        g_sortDesc = s.get("sortdesc", g_sortDesc ? "1" : "0") == "1";
        g_gridMode = s.get("gridmode", "0") == "1";
        resumeRel = s.get("lastfile", "");
    }

    // ---- scan (off the main thread; folder may be a slow card) ----
    NSString* folder = g_folderURL.path;
    std::string folderPath = g_folderPath;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        std::vector<Item> found;
        scanDir(folder, found);
        if (g_pairRawJpg) pairRawJpg(found);
        std::sort(found.begin(), found.end(), itemLess);
        bool slow = !found.empty() && probeSlowDrive(found[0].path);
        tcInit(folderPath);                        // open/index the disk thumb cache
        dispatch_async(dispatch_get_main_queue(), ^{
            g_items = std::move(found);
            g_slowDrive = slow;
            // Background decode concurrency: 1 on a seek-bound drive, else a small
            // pool. The on-screen decode bypasses this and always runs at once.
            unsigned cores = std::max(2u, (unsigned)std::thread::hardware_concurrency());
            int bg = slow ? 1 : (int)std::min(6u, cores - 1);
            g_bgSem = dispatch_semaphore_create(bg);
            int startItem = 0;
            if (!resumeRel.empty()) {              // jump back to where we left off
                std::string want = g_folderPath + "/" + resumeRel;
                for (int i = 0; i < (int)g_items.size(); i++)
                    if (g_items[i].path == want) { startItem = i; break; }
            }
            rebuildView(startItem);                // no filter yet -> view == all items
            if (g_items.empty()) {
                self.view.infoText = @"No supported raws found in this folder.";
                [self.view setNeedsDisplay:YES];
            } else {
                [self showCurrent];
                [self loadAllSidecars];            // ratings for grid badges + filters
            }
        });
    });

    // ---- fullscreen borderless window ----
    NSRect frame = [NSScreen mainScreen].frame;
    self.window = [[NSWindow alloc] initWithContentRect:frame
        styleMask:NSWindowStyleMaskBorderless
        backing:NSBackingStoreBuffered defer:NO];
    self.window.level = NSMainMenuWindowLevel + 1;
    self.window.backgroundColor = [NSColor blackColor];
    self.window.opaque = YES;
    self.window.acceptsMouseMovedEvents = YES;   // for zoom mouse-follow panning
    [self.window setFrame:frame display:YES];

    self.view = [[IRView alloc] initWithFrame:frame];
    self.view.infoText = @"Scanning…";
    // Guaranteed-opaque black backing: the borderless window is not "key" and can
    // otherwise show the desktop through any pixel a draw pass hasn't covered.
    self.view.wantsLayer = YES;
    self.view.layer.backgroundColor = [NSColor blackColor].CGColor;
    self.window.contentView = self.view;
    g_contentView = self.view;              // background threads repaint via this

    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    NSApp.presentationOptions = NSApplicationPresentationHideDock |
                                NSApplicationPresentationHideMenuBar;
}

- (void)applicationWillTerminate:(NSNotification*)n {
    // persist resume point (relative path so the folder can move)
    int item = curItem();
    if (item >= 0) {
        std::string p = g_items[item].path, rel = p;
        std::string prefix = g_folderPath + "/";
        if (p.rfind(prefix, 0) == 0) rel = p.substr(prefix.size());
        sessionWrite(rel);
    }
    if (g_sidecarQ) dispatch_sync(g_sidecarQ, ^{});   // flush pending writes
    if (g_folderURL) [g_folderURL stopAccessingSecurityScopedResource];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)a { return YES; }

// ---- decode + display the current image ----
- (void)showCurrent {
    if (viewCount() == 0) { self.view.image = nullptr;
        self.view.infoText = @"No images match this filter."; [self.view setNeedsDisplay:YES]; return; }
    if (g_cur < 0) g_cur = 0;
    if (g_cur >= viewCount()) g_cur = viewCount() - 1;
    if (g_zoom) { g_zoom = false;              // a new frame exits the 100% loupe
        if (g_zoomImage) { CGImageRelease(g_zoomImage); g_zoomImage = nullptr; } }
    int idx = curItem();                       // real item index
    if (g_items[idx].rating == -2) loadSidecar(idx);

    // Cache hit -> paint instantly, no decode, no held-arrow swallow.
    CGImageRef cachedImg = nullptr; RawExif cachedExif; bool haveDone = false, haveFail = false;
    {
        std::lock_guard<std::mutex> lk(g_cacheMx);
        auto f = g_cache.find(idx);
        if (f != g_cache.end()) {
            haveDone = true; haveFail = f->second.failed; cachedExif = f->second.exif;
            if (f->second.img) cachedImg = CGImageRetain(f->second.img);
        }
    }
    if (haveDone) {
        g_decoding = false;
        g_curExif = cachedExif;
        if (cachedImg) { self.view.image = cachedImg; CGImageRelease(cachedImg); }
        self.view.infoText = [self infoStringForView:g_cur exif:cachedExif decoded:(!haveFail)];
        [self.view setNeedsDisplay:YES];
    } else {
        // Miss: keep the last frame up, swallow held-arrow repeats until it lands.
        g_decoding = true;
        [self requestDecode:idx priority:YES];
    }
    [self prefetchAround:g_cur];
    [self evictFarFrom:g_cur];
}

// Read every sidecar once (one sequential reader), so grid badges and rating
// filters reflect what's on disk without visiting each frame first.
- (void)loadAllSidecars {
    __weak AppDelegate* weakSelf = self;
    dispatch_async(g_sidecarQ, ^{
        int n = (int)g_items.size();
        for (int i = 0; i < n; i++) {
            std::string sc = sidecarPath(g_items[i].path), xmp;
            if (!readSidecarForPatch(sc, xmp)) continue;   // unreadable: leave -2, retry on visit
            int rating = 0, label = 0;
            if (!xmp.empty()) xmpParse(xmp, rating, label);
            int fr = xmp.empty() ? 0 : rating, fl = xmp.empty() ? -1 : label;
            std::vector<std::string> kws = xmp.empty() ? std::vector<std::string>{}
                                                       : xmpGetKeywords(xmp);
            dispatch_async(dispatch_get_main_queue(), ^{
                if (i < (int)g_items.size() && g_items[i].rating == -2) {
                    g_items[i].rating = fr; g_items[i].label = fl;
                    g_items[i].keywords = kws;
                    if (g_gridMode) [g_contentView setNeedsDisplay:YES];   // badges live
                }
            });
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            AppDelegate* s = weakSelf; if (!s) return;
            [s.view setNeedsDisplay:YES];   // final refresh once all are in
        });
    });
}

// Queue a decode for one item (unless cached or already in flight). On finish it
// populates the cache and, if that item is still the current one, paints it.
- (void)requestDecode:(int)idx priority:(BOOL)high {
    if (idx < 0 || idx >= (int)g_items.size()) return;
    {
        std::lock_guard<std::mutex> lk(g_cacheMx);
        if (g_cache.count(idx) || g_inFlight.count(idx)) return;
        g_inFlight.insert(idx);
    }
    std::string path = g_items[idx].path;
    int gen = g_generation.load();
    dispatch_queue_t q = dispatch_get_global_queue(
        high ? QOS_CLASS_USER_INITIATED : QOS_CLASS_UTILITY, 0);
    __weak AppDelegate* weakSelf = self;
    dispatch_async(q, ^{
        // Background (prefetch) decodes wait on the drive semaphore so a slow disk
        // serves one at a time; the on-screen decode (high) never waits.
        bool bg = !high && g_bgSem;
        if (bg) dispatch_semaphore_wait(g_bgSem, DISPATCH_TIME_FOREVER);
        RawExif exif;
        CGImageRef img = decodePreview(path, g_maxPix, exif);   // +1 retained
        if (bg) dispatch_semaphore_signal(g_bgSem);
        bool stale = false;
        {
            std::lock_guard<std::mutex> lk(g_cacheMx);
            g_inFlight.erase(idx);
            if (gen != g_generation.load()) stale = true;   // a re-sort remapped indices
            else { Cached c; c.img = img; c.exif = exif; c.failed = (img == nullptr);
                   g_cache[idx] = c; }
        }
        if (stale) { if (img) CGImageRelease(img); return; }
        dispatch_async(dispatch_get_main_queue(), ^{
            AppDelegate* s = weakSelf; if (!s) { return; }
            if (gen == g_generation.load() && idx == curItem()) {   // on-screen image
                g_decoding = false;
                g_curExif = exif;
                if (img) s.view.image = img;
                s.view.infoText = [s infoStringForView:g_cur exif:exif decoded:(img != nullptr)];
                [s.view setNeedsDisplay:YES];
            }
        });
    });
}

// Prefetch a window of neighbours (by VIEW position, biased in the direction of
// travel) so a run of forward-presses keeps landing on decoded frames.
- (void)prefetchAround:(int)viewPos {
    for (int d = 1; d <= CACHE_RADIUS; d++) {
        int fwd = viewPos + d * g_lastDir, back = viewPos - d * g_lastDir;
        if (fwd >= 0 && fwd < viewCount())  [self requestDecode:g_view[fwd]  priority:NO];
        if (back >= 0 && back < viewCount()) [self requestDecode:g_view[back] priority:NO];
    }
}

// Drop decoded frames for items outside the keep window (by VIEW distance).
- (void)evictFarFrom:(int)viewPos {
    int keep = CACHE_RADIUS + 2;
    std::set<int> keepItems;
    for (int d = -keep; d <= keep; d++) {
        int p = viewPos + d;
        if (p >= 0 && p < viewCount()) keepItems.insert(g_view[p]);
    }
    std::vector<CGImageRef> toRelease;
    {
        std::lock_guard<std::mutex> lk(g_cacheMx);
        for (auto it = g_cache.begin(); it != g_cache.end(); ) {
            if (!keepItems.count(it->first) && !g_inFlight.count(it->first)) {
                if (it->second.img) toRelease.push_back(it->second.img);
                it = g_cache.erase(it);
            } else ++it;
        }
    }
    for (CGImageRef r : toRelease) CGImageRelease(r);   // release outside the lock
}

// Wipe both caches (on re-sort: item indices are about to remap).
- (void)clearCache {
    std::vector<CGImageRef> toRelease;
    {
        std::lock_guard<std::mutex> lk(g_cacheMx);
        for (auto& kv : g_cache) if (kv.second.img) toRelease.push_back(kv.second.img);
        g_cache.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_thumbMx);
        for (auto& kv : g_thumbCache) if (kv.second) toRelease.push_back(kv.second);
        g_thumbCache.clear();
    }
    for (CGImageRef r : toRelease) CGImageRelease(r);
}

// Re-sort g_items, preserving the on-screen image across the index remap. The
// current frame stays shown (the view retains its own copy), so no black flash.
- (void)resortKeepingCurrent {
    int keepItem = curItem();
    std::string keepPath = keepItem >= 0 ? g_items[keepItem].path : "";
    g_generation++;                                // discard in-flight decodes
    [self clearCache];
    std::stable_sort(g_items.begin(), g_items.end(), itemLess);
    int newItem = -1;
    for (int i = 0; i < (int)g_items.size(); i++)
        if (g_items[i].path == keepPath) { newItem = i; break; }
    rebuildView(newItem);
    g_decoding = false;
    if (g_gridMode) { [self ensureGridVisible]; [self.view setNeedsDisplay:YES]; }
    else [self showCurrent];
}
- (void)changeSort:(Action)a {
    if (g_items.empty()) return;
    if (a == A_SORTNEXT)      g_sortType = (g_sortType + 1) % 3;
    else if (a == A_SORTPREV) g_sortType = (g_sortType + 2) % 3;
    else if (a == A_SORTDIR)  g_sortDesc = !g_sortDesc;
    [self resortKeepingCurrent];
}
- (void)setSortType:(int)t {                        // toolbar Name/Date/Size
    if (t < 0 || t > 2 || g_items.empty()) return;
    g_sortType = t; [self resortKeepingCurrent];
}
- (void)toggleSortDir { if (!g_items.empty()) { g_sortDesc = !g_sortDesc; [self resortKeepingCurrent]; } }
- (void)jumpFirst {
    if (!viewCount()) return;
    g_lastDir = 1; g_cur = 0;
    if (g_gridMode) { [self ensureGridVisible]; [self.view setNeedsDisplay:YES]; } else [self showCurrent];
}
- (void)jumpLast {
    if (!viewCount()) return;
    g_lastDir = -1; g_cur = viewCount() - 1;
    if (g_gridMode) { [self ensureGridVisible]; [self.view setNeedsDisplay:YES]; } else [self showCurrent];
}

// Re-apply the filter live (after any panel change): rebuild the view, keeping
// the current frame if it survives, and repaint.
- (void)applyFilter {
    int keepItem = curItem();
    rebuildView(keepItem);
    g_decoding = false;
    if (g_gridMode) { g_gridScrollY = 0; [self ensureGridVisible]; [self.view setNeedsDisplay:YES]; }
    else [self showCurrent];
}
- (void)openFilter {
    g_filterOpen = true; g_helpOpen = false; g_prefsOpen = false;
    [self.view setNeedsDisplay:YES];
}
// While the filter panel is open: click chips with the mouse; the keyboard just
// edits the filename text field, and Esc / F close.
- (void)handleFilterKey:(NSEvent*)e {
    unsigned short kc = e.keyCode;
    if (kc == 53 || actionForEvent(e) == A_FILTER) {    // Esc or F closes
        g_filterOpen = false; [self.view setNeedsDisplay:YES]; return;
    }
    if (kc == 51) {                                     // Backspace
        if (!g_filterText.empty()) { g_filterText.pop_back(); [self applyFilter]; }
    } else {
        NSString* ch = e.characters;
        if (ch.length == 1) { unichar c = [ch characterAtIndex:0];
            if (c >= 32 && c < 127) { g_filterText.push_back((char)c); [self applyFilter]; } }
    }
    [self.view setNeedsDisplay:YES];
}

// ---- preferences / rebind ----
- (void)openPrefs {
    g_prefsOpen = true; g_prefsCaptureAction = -1; g_prefsMsg = nil;
    g_helpOpen = false; g_filterOpen = false;
    [self.view setNeedsDisplay:YES];
}
// Keyboard while prefs is open: if an action is armed for rebinding (clicked), the
// next key binds it (Esc cancels); otherwise Esc / Shift+P closes the panel.
- (void)handlePrefsKey:(NSEvent*)e {
    unsigned short kc = e.keyCode;
    if (g_prefsCaptureAction >= 0) {
        if (kc != 53) {                                   // Esc cancels; else bind it
            KeySpec k = specForEvent(e);
            if (k.keyCode >= 0 || k.ch) {
                Action a = (Action)g_prefsCaptureAction;
                NSString* prior = keyUsedBy(k, a, -1);    // what did this key do before?
                // A keyword set on this key would go silently dead (actions win in
                // dispatch), so unbind it and say so.
                for (int i = 0; i < kKwSlots; i++)
                    if (g_kwSlots[i].bound && g_kwSlots[i].key == k) g_kwSlots[i].bound = false;
                bindActionToKey(a, k);                    // also steals from another action
                g_prefsMsg = prior ? [NSString stringWithFormat:@"Reassigned that key from %@.", prior] : nil;
            }
        }
        g_prefsCaptureAction = -1;
        [self.view setNeedsDisplay:YES];
        return;
    }
    if (kc == 53 || actionForEvent(e) == A_PREFS) g_prefsOpen = false;   // close
    [self.view setNeedsDisplay:YES];
}

// ---- keyword sets ----
- (void)openKeywords {
    g_kwOpen = true; g_kwCaptureSlot = -1; g_kwEditSlot = -1; g_kwMsg = nil;
    g_helpOpen = false; g_prefsOpen = false; g_filterOpen = false;
    [self.view setNeedsDisplay:YES];
}
// Keyboard while the keyword editor is open: key-capture for a slot, or typing
// into a slot's words field, or Esc/K to close.
- (void)handleKeywordsKey:(NSEvent*)e {
    unsigned short kc = e.keyCode;
    if (g_kwCaptureSlot >= 0) {                          // capture the toggle key
        if (kc != 53) {                                  // Esc cancels
            KeySpec k = specForEvent(e);
            // Grid mode consumes these keyCodes before slot dispatch (regardless of
            // modifiers), so a set bound to one would silently never fire in grid.
            bool gridKey = (kc == 123 || kc == 124 || kc == 125 || kc == 126 ||
                            kc == 36 || kc == 116 || kc == 121);
            if (k.cmd && k.ch == 'q') {                  // never let capture eat Cmd+Q
                g_kwMsg = @"⌘Q quits — pick another key.";
            } else if (gridKey) {
                g_kwMsg = @"That key navigates the grid — pick another.";
            } else if (k.keyCode >= 0 || k.ch) {
                if (actionForEvent(e) != A_NONE) {         // app controls win — refuse
                    g_kwMsg = @"That key runs an app control — pick another (or rebind that control in Preferences first).";
                } else {
                    int stole = -1;
                    for (int i = 0; i < kKwSlots; i++)   // steal from another slot (with notice)
                        if (i != g_kwCaptureSlot && g_kwSlots[i].bound && g_kwSlots[i].key == k)
                            { g_kwSlots[i].bound = false; stole = i; }
                    g_kwSlots[g_kwCaptureSlot].key = k;
                    g_kwSlots[g_kwCaptureSlot].bound = true;
                    g_kwMsg = stole >= 0
                        ? [NSString stringWithFormat:@"Moved that key here from Set %d.", stole + 1] : nil;
                    writeIni();
                }
            }
        }
        g_kwCaptureSlot = -1;
        [self.view setNeedsDisplay:YES];
        return;
    }
    if (g_kwEditSlot >= 0) {                             // typing keywords
        if (kc == 53 || kc == 36) { g_kwEditSlot = -1; writeIni(); }   // Esc/Enter done
        else if (kc == 51) { auto& w = g_kwSlots[g_kwEditSlot].words;  // Backspace
                             if (!w.empty()) w.pop_back(); }
        else {
            NSString* ch = e.characters;
            if (ch.length == 1) { unichar c = [ch characterAtIndex:0];
                if (c >= 32 && c < 127) g_kwSlots[g_kwEditSlot].words.push_back((char)c); }
        }
        [self.view setNeedsDisplay:YES];
        return;
    }
    if (kc == 53 || actionForEvent(e) == A_KEYWORDS) { g_kwOpen = false; writeIni(); }
    [self.view setNeedsDisplay:YES];
}
// Toggle a slot's keywords on the current image: if the image already has ALL of
// them they're removed, otherwise the missing ones are added. Full list written
// to the sidecar (async, same serial queue as rating writes).
- (void)toggleKeywordSlot:(int)slot {
    int idx = curItem(); if (idx < 0) return;
    auto kws = splitKeywords(g_kwSlots[slot].words);
    if (kws.empty()) return;
    Item& it = g_items[idx];
    bool hasAll = true;
    for (auto& k : kws)
        if (std::find(it.keywords.begin(), it.keywords.end(), k) == it.keywords.end())
            { hasAll = false; break; }
    if (hasAll) {
        for (auto& k : kws)
            it.keywords.erase(std::remove(it.keywords.begin(), it.keywords.end(), k),
                              it.keywords.end());
    } else {
        for (auto& k : kws)
            if (std::find(it.keywords.begin(), it.keywords.end(), k) == it.keywords.end())
                it.keywords.push_back(k);
    }
    writeSidecarKeywords(it.path, it.keywords);
    if (!g_gridMode) self.view.infoText = [self infoStringForView:g_cur exif:g_curExif decoded:YES];
    [self.view setNeedsDisplay:YES];
}

// ---- grid mode ----
- (void)toggleGrid {
    g_gridMode = !g_gridMode;
    g_zoom = false;
    if (g_gridMode) [self ensureGridVisible];
    else [self showCurrent];          // back to loupe on the selected cell
    [self.view setNeedsDisplay:YES];
}
// Scroll the sheet so the selected cell is fully on screen.
- (void)ensureGridVisible {
    if (g_items.empty()) return;
    CGFloat vw = self.view.bounds.size.width;
    CGFloat availH = self.view.bounds.size.height - kGridTbH;   // cells sit below the toolbar
    int cols = std::max(1, (int)(vw / 240.0));
    CGFloat cellW = vw / cols, cellH = cellW * 0.72;
    int row = g_cur / cols;
    CGFloat top = row * cellH, bot = top + cellH;
    if (top < g_gridScrollY) g_gridScrollY = top;
    else if (bot > g_gridScrollY + availH) g_gridScrollY = bot - availH;
}
// Move the grid selection (delta = ±1 for L/R, ±cols for U/D). Prefetches the
// destination frame so opening it into the loupe is instant.
- (void)gridMove:(int)delta {
    if (viewCount() == 0) return;
    int ni = g_cur + delta;
    if (ni < 0 || ni >= viewCount()) return;
    g_cur = ni;
    [self ensureGridVisible];
    [self.view setNeedsDisplay:YES];
}
- (void)gridScrollRows:(int)rows {
    CGFloat vw = self.view.bounds.size.width;
    int cols = std::max(1, (int)(vw / 240.0));
    CGFloat cellH = (vw / cols) * 0.72;
    g_gridScrollY += rows * cellH;
    [self.view setNeedsDisplay:YES];
}

// ---- 100% zoom (loupe only) ----
- (void)toggleZoom {
    if (g_gridMode || viewCount() == 0) return;
    g_zoom = !g_zoom;
    g_zoomCenter = CGPointMake(0.5f, 0.5f);
    if (g_zoomImage) { CGImageRelease(g_zoomImage); g_zoomImage = nullptr; }
    if (g_zoom) {
        // Decode the preview at full resolution so 1:1 shows real sensor pixels.
        int item = curItem(); if (item < 0) { g_zoom = NO; return; }
        std::string path = g_items[item].path;
        int gen = g_generation.load();
        __weak AppDelegate* weakSelf = self;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            RawExif ex;
            CGImageRef full = decodePreview(path, 100000, ex);   // uncapped
            dispatch_async(dispatch_get_main_queue(), ^{
                AppDelegate* s = weakSelf; if (!s) { if (full) CGImageRelease(full); return; }
                if (!g_zoom || gen != g_generation.load() || curItem() != item) {
                    if (full) CGImageRelease(full); return;      // moved on / cancelled
                }
                if (g_zoomImage) CGImageRelease(g_zoomImage);
                g_zoomImage = full;                              // may be nullptr
                [s.view setNeedsDisplay:YES];
            });
        });
    }
    [self.view setNeedsDisplay:YES];
}
- (void)panZoomDX:(CGFloat)dx dy:(CGFloat)dy {
    if (!g_zoom) return;
    g_zoomCenter.x = std::max((CGFloat)0, std::min((CGFloat)1, g_zoomCenter.x + dx));
    g_zoomCenter.y = std::max((CGFloat)0, std::min((CGFloat)1, g_zoomCenter.y + dy));
    [self.view setNeedsDisplay:YES];
}

// The LEFT half of the loupe info bar (Windows layout): count · filename ·
// [Sort ↑] · EXIF (shutter aperture ISO focal ev | date camera lens). The stars
// and label are drawn separately, coloured and right-aligned (see drawRect).
- (NSString*)infoStringForView:(int)viewPos exif:(const RawExif&)e decoded:(BOOL)ok {
    int idx = (viewPos >= 0 && viewPos < viewCount()) ? g_view[viewPos] : -1;
    if (idx < 0) return @"";
    Item& it = g_items[idx];
    NSString* name = [[NSString stringWithUTF8String:it.path.c_str()] lastPathComponent];
    // count (with "(of Total)" when filtered), then filename, then [sort]
    NSString* count = filterActive()
        ? [NSString stringWithFormat:@"%d / %d (of %d)", viewPos + 1, viewCount(), (int)g_items.size()]
        : [NSString stringWithFormat:@"%d / %d", viewPos + 1, viewCount()];
    NSString* sortName = g_sortType == 1 ? @"Date" : g_sortType == 2 ? @"Size" : @"Name";
    NSMutableString* m = [NSMutableString stringWithFormat:@"%@   %@%@   [%@ %@]",
        count, name, it.jpgTwin ? @"+JPG" : @"", sortName, g_sortDesc ? @"↓" : @"↑"];
    // EXIF group
    NSMutableArray* ex = [NSMutableArray array];
    std::string sh = fmtShutter(e.expNum, e.expDen), ap = fmtAperture(e.fnNum, e.fnDen);
    std::string fl = fmtFocal(e.flNum, e.flDen), ev = fmtEv(e.evNum, e.evDen, e.hasEv);
    if (!sh.empty()) [ex addObject:[NSString stringWithUTF8String:sh.c_str()]];
    if (!ap.empty()) [ex addObject:[NSString stringWithUTF8String:ap.c_str()]];
    if (e.iso)       [ex addObject:[NSString stringWithFormat:@"ISO %u", e.iso]];
    if (!fl.empty()) [ex addObject:[NSString stringWithUTF8String:fl.c_str()]];
    if (!ev.empty()) [ex addObject:[NSString stringWithUTF8String:ev.c_str()]];
    if (ex.count) [m appendFormat:@"      %@", [ex componentsJoinedByString:@"   "]];
    if (ok) {   // date / camera / lens after a separator
        NSMutableArray* extra = [NSMutableArray array];
        if (!e.dateTime.empty()) [extra addObject:[NSString stringWithUTF8String:e.dateTime.c_str()]];
        if (!e.model.empty())    [extra addObject:[NSString stringWithUTF8String:e.model.c_str()]];
        if (!e.lens.empty())     [extra addObject:[NSString stringWithUTF8String:e.lens.c_str()]];
        if (extra.count) [m appendFormat:@"   |   %@", [extra componentsJoinedByString:@"   "]];
    } else [m appendString:@"   (no preview)"];
    // keywords (first few, comma-joined, braced so they read as tags)
    if (!it.keywords.empty()) {
        std::string kj;
        for (size_t i = 0; i < it.keywords.size() && i < 4; i++)
            { if (!kj.empty()) kj += ", "; kj += it.keywords[i]; }
        if (it.keywords.size() > 4) kj += ", …";
        NSString* ks = [NSString stringWithUTF8String:kj.c_str()];
        if (ks) [m appendFormat:@"   {%@}", ks];
    }
    return m;
}

// ---- rating / labels ----
- (void)applyRating:(int)rating {
    int idx = curItem(); if (idx < 0) return;
    Item& it = g_items[idx];
    it.rating = rating;
    writeSidecar(it.path, rating, kXmpKeep);
    if (!g_gridMode) self.view.infoText = [self infoStringForView:g_cur exif:g_curExif decoded:YES];
    [self maybeAutoAdvance];
}
- (void)applyLabel:(int)label {
    int idx = curItem(); if (idx < 0) return;
    Item& it = g_items[idx];
    it.label = (it.label == label) ? 0 : label;   // toggle off if same
    writeSidecar(it.path, kXmpKeep, it.label);
    if (!g_gridMode) self.view.infoText = [self infoStringForView:g_cur exif:g_curExif decoded:YES];
    [self maybeAutoAdvance];
}
- (void)maybeAutoAdvance {
    BOOL go = NO;
    if (g_advanceMode == 1) go = YES;                              // always
    else if (g_advanceMode == 0)                                   // capslock
        go = ([NSEvent modifierFlags] & NSEventModifierFlagCapsLock) != 0;
    if (go && g_cur < viewCount() - 1) {
        g_cur++;
        if (g_gridMode) { [self ensureGridVisible]; [self.view setNeedsDisplay:YES]; }
        else [self showCurrent];
    } else [self.view setNeedsDisplay:YES];
}
@end

// ============================================================ key handling
// actionForEvent() (above, near the keymap) resolves keys through the ini-loaded
// g_keymap built from kActRows — one table drives binding and the help overlay.

@interface IRWindow : NSWindow @end
@implementation IRWindow
- (BOOL)canBecomeKeyMain { return YES; }
- (BOOL)canBecomeKeyWindow { return YES; }
@end

int main(int argc, const char* argv[]) {
    (void)argc; (void)argv;
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        AppDelegate* del = [[AppDelegate alloc] init];
        app.delegate = del;

        // Global key monitor: the borderless window has no responder chain menu,
        // so we route keys here. Held-arrow repeats are swallowed while a decode
        // is in flight (never-black guarantee); last frame stays up.
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                              handler:^NSEvent*(NSEvent* e) {
            Action a = actionForEvent(e);
            unsigned short kc = e.keyCode;

            // ⌘Q always quits (the borderless app has no menu to do it) — handled
            // before modals/capture so a rebind can never swallow or bind it.
            if ((e.modifierFlags & NSEventModifierFlagCommand) &&
                [[e.charactersIgnoringModifiers lowercaseString] isEqualToString:@"q"]) {
                [NSApp terminate:nil]; return nil;
            }

            // Modal overlays grab all keys while open.
            if (g_prefsOpen)  { [del handlePrefsKey:e];  return nil; }
            if (g_filterOpen) { [del handleFilterKey:e]; return nil; }
            if (g_kwOpen)     { [del handleKeywordsKey:e]; return nil; }

            // While help is open, any key just closes it (and is swallowed).
            if (g_helpOpen) { g_helpOpen = false; [del.view setNeedsDisplay:YES];
                              return nil; }

            // --- grid mode: arrows/Enter drive the contact sheet ---
            if (g_gridMode) {
                if (kc == 126) { [del gridMove:-g_gridCols]; return nil; }   // Up
                if (kc == 125) { [del gridMove:+g_gridCols]; return nil; }   // Down
                if (kc == 123) { [del gridMove:-1]; return nil; }            // Left
                if (kc == 124) { [del gridMove:+1]; return nil; }            // Right
                if (kc == 36)  { [del toggleGrid]; return nil; }             // Enter -> open
                if (kc == 116) { [del gridScrollRows:-3]; return nil; }      // PageUp
                if (kc == 121) { [del gridScrollRows:+3]; return nil; }      // PageDown
                if (a == A_GRID) { [del toggleGrid]; return nil; }
                // rating/label/sort/quit/etc fall through to the shared switch.
            }
            // --- zoom mode: arrows pan the 100% loupe ---
            else if (g_zoom && (kc == 123 || kc == 124 || kc == 125 || kc == 126)) {
                CGFloat step = 0.12f;
                if (kc == 123) [del panZoomDX:-step dy:0];
                else if (kc == 124) [del panZoomDX:step dy:0];
                else if (kc == 125) [del panZoomDX:0 dy:-step];
                else [del panZoomDX:0 dy:step];
                return nil;
            }

            if (a == A_NONE) {
                // keyword-set keys fire only when no app action claims the key
                KeySpec want = specForEvent(e);
                for (int i = 0; i < kKwSlots; i++)
                    if (g_kwSlots[i].bound && g_kwSlots[i].key == want &&
                        !g_kwSlots[i].words.empty()) {
                        // ignore auto-repeat — a toggle isn't idempotent, so a held
                        // key would flip the keywords on/off per typematic repeat
                        if (!e.isARepeat) [del toggleKeywordSlot:i];
                        return nil;
                    }
                return e;
            }
            bool isNav = (a == A_NEXT || a == A_PREV || a == A_FIRST || a == A_LAST);
            if (!g_gridMode && isNav && e.isARepeat && g_decoding.load()) return nil;  // swallow

            switch (a) {
                case A_NEXT:  if (g_gridMode) [del gridMove:+1];
                              else if (g_cur < viewCount()-1) { g_lastDir = 1; g_cur++; [del showCurrent]; } break;
                case A_PREV:  if (g_gridMode) [del gridMove:-1];
                              else if (g_cur > 0) { g_lastDir = -1; g_cur--; [del showCurrent]; } break;
                case A_FIRST: if (viewCount()) { g_lastDir = 1; g_cur = 0;
                                  if (g_gridMode) { [del ensureGridVisible]; [del.view setNeedsDisplay:YES]; } else [del showCurrent]; } break;
                case A_LAST:  if (viewCount()) { g_lastDir = -1; g_cur = viewCount()-1;
                                  if (g_gridMode) { [del ensureGridVisible]; [del.view setNeedsDisplay:YES]; } else [del showCurrent]; } break;
                case A_GRID:  [del toggleGrid]; break;
                case A_ZOOM:  [del toggleZoom]; break;
                case A_FILTER: [del openFilter]; break;
                case A_KEYWORDS: [del openKeywords]; break;
                case A_RATE0: [del applyRating:0]; break;
                case A_RATE1: [del applyRating:1]; break;
                case A_RATE2: [del applyRating:2]; break;
                case A_RATE3: [del applyRating:3]; break;
                case A_RATE4: [del applyRating:4]; break;
                case A_RATE5: [del applyRating:5]; break;
                case A_REJECT: {
                    int it = curItem();
                    if (it >= 0) [del applyRating:(g_items[it].rating == -1 ? 0 : -1)];
                    break; }
                case A_LABEL_RED:    [del applyLabel:1]; break;
                case A_LABEL_YELLOW: [del applyLabel:2]; break;
                case A_LABEL_GREEN:  [del applyLabel:3]; break;
                case A_LABEL_BLUE:   [del applyLabel:4]; break;
                case A_LABEL_PURPLE: [del applyLabel:5]; break;
                case A_SORTNEXT: case A_SORTPREV: case A_SORTDIR:
                                     [del changeSort:a]; break;
                case A_TOGGLE_INFO:  g_showInfo = !g_showInfo;
                                     [del.view setNeedsDisplay:YES]; break;
                case A_INFOPOS:      g_infoTop = !g_infoTop;
                                     [del.view setNeedsDisplay:YES]; break;
                case A_HELP:         g_helpOpen = !g_helpOpen; g_prefsOpen = false; g_filterOpen = false;
                                     [del.view setNeedsDisplay:YES]; break;
                case A_PREFS:        [del openPrefs]; break;
                case A_QUIT: [NSApp terminate:nil]; break;
                default: break;
            }
            return nil;   // consumed
        }];

        [app run];
    }
    return 0;
}
