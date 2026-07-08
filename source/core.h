// core.h - iRate: raw preview+EXIF parser and XMP sidecar logic.
// Supports: Sony ARW / Canon CR2 / Nikon NEF+NRW / Pentax PEF / DNG (TIFF, both
//           endians), Canon CR3 (ISO boxes), Fuji RAF, Panasonic RW2, Olympus ORF,
//           plain JPEG.
// Pure C++ (no Windows deps) so it can be unit-tested on any platform.
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <set>
#include <algorithm>

// ---------------------------------------------------------------- byte source
class ByteSource {
public:
    virtual bool read(uint64_t off, void* dst, uint32_t n) = 0;
    virtual uint64_t size() = 0;
    virtual ~ByteSource() {}
};

// A windowed view into another source (used for TIFF blobs inside CR3 boxes).
class SubSource : public ByteSource {
public:
    SubSource(ByteSource& p, uint64_t off, uint64_t len) : p_(p), off_(off), len_(len) {}
    bool read(uint64_t off, void* dst, uint32_t n) override {
        if (off + n > len_) return false;
        return p_.read(off_ + off, dst, n);
    }
    uint64_t size() override { return len_; }
private:
    ByteSource& p_; uint64_t off_, len_;
};

// ---------------------------------------------------------------- exif result
struct RawExif {
    std::string model, lens, dateTime;      // dateTime = "YYYY:MM:DD HH:MM:SS"
    uint32_t expNum = 0, expDen = 0;        // ExposureTime
    uint32_t fnNum = 0, fnDen = 0;          // FNumber
    uint32_t flNum = 0, flDen = 0;          // FocalLength
    int32_t  evNum = 0; int32_t evDen = 0; bool hasEv = false;
    uint32_t iso = 0;
    int orientation = 1;
    uint32_t width = 0, height = 0;         // filled by decoder later
};

struct ArwResult {
    bool ok = false;
    std::string err;
    uint64_t jpegOff = 0;
    uint32_t jpegLen = 0;
    RawExif exif;
};

// ---------------------------------------------------------------- parser
class ArwParser {
public:
    explicit ArwParser(ByteSource& s) : src(s), fsize(s.size()) {}

    ArwResult parse() {
        ArwResult r;
        uint8_t hdr[12];
        if (fsize < 16 || !src.read(0, hdr, 12)) { r.err = "unreadable file"; return r; }
        if (hdr[0] == 0xFF && hdr[1] == 0xD8) return parseJpeg();
        if (!memcmp(hdr + 4, "ftyp", 4)) return parseCr3();
        if (!memcmp(hdr, "FUJIFILM", 8)) return parseRaf();
        if (hdr[0]=='I' && hdr[1]=='I' &&
            ((hdr[2]==42 && hdr[3]==0) ||          // TIFF / NEF / CR2 / PEF / DNG
             (hdr[2]==0x55 && hdr[3]==0) ||        // Panasonic RW2
             (hdr[2]=='R' && (hdr[3]=='O' || hdr[3]=='S')))) be = false;   // Olympus ORF
        else if (hdr[0]=='M' && hdr[1]=='M' &&
                 ((hdr[2]==0 && hdr[3]==42) || (hdr[2]=='O' && hdr[3]=='R'))) be = true;
        else { r.err = "unsupported file format"; return r; }
        base = 0;
        walkIFD(rd32at(4), KN_NORMAL, 0);
        return finish();
    }

private:
    enum Kind { KN_NORMAL, KN_EXIF, KN_OLY_MN, KN_OLY_CS };
    ByteSource& src;
    uint64_t fsize;
    bool be = false;                         // big-endian TIFF (Nikon NEF)
    uint64_t base = 0;                       // TIFF origin (inside JPEG/CR3 wrappers)
    std::set<uint64_t> visited;
    struct Cand { uint64_t off; uint32_t len; };
    std::vector<Cand> cands;
    RawExif exif;
    bool haveOrient = false;
    bool sawFirstIfd = false;

    bool rd(uint64_t off, void* dst, uint32_t n) {
        if (off + n > fsize) return false;
        return src.read(off, dst, n);
    }
    static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
    static uint32_t le32(const uint8_t* p) { return (uint32_t)(p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24)); }
    static uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0]<<8) | p[1]); }
    static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0]<<24) | (p[1]<<16) | (p[2]<<8) | p[3]; }
    static uint64_t be64(const uint8_t* p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }
    uint16_t rd16(const uint8_t* p) const { return be ? be16(p) : le16(p); }
    uint32_t rd32(const uint8_t* p) const { return be ? be32(p) : le32(p); }
    uint32_t rd32at(uint64_t off) { uint8_t b[4]; return rd(off, b, 4) ? rd32(b) : 0; }
    uint16_t rd16at(uint64_t off) { uint8_t b[2]; return rd(off, b, 2) ? rd16(b) : 0; }

    static uint32_t typeSize(uint16_t t) {
        switch (t) { case 1: case 2: case 6: case 7: return 1;
                     case 3: case 8: return 2;
                     case 4: case 9: case 11: return 4;
                     case 5: case 10: case 12: return 8; }
        return 1;
    }

    // scan a JPEG stream at [off, off+len) for an Exif APP1 and harvest metadata
    void scanJpegExif(uint64_t joff, uint64_t jlen) {
        uint64_t lim = joff + jlen;
        if (lim > fsize) lim = fsize;
        uint64_t pos = joff + 2;
        uint8_t b[4];
        for (int guard = 0; guard < 64 && pos + 4 < lim; guard++) {
            if (!rd(pos, b, 4)) break;
            if (b[0] != 0xFF) break;
            uint8_t m = b[1];
            if (m == 0xDA || m == 0xD9) break;
            uint32_t seglen = be16(b + 2);
            if (seglen < 2) break;
            if (m == 0xE1) {
                uint8_t sig[6];
                if (rd(pos + 4, sig, 6) && !memcmp(sig, "Exif\0\0", 6)) {
                    uint64_t tiff = pos + 10;
                    uint8_t th[8];
                    if (rd(tiff, th, 8)) {
                        bool sb = be; uint64_t sbase = base;
                        if (th[0]=='I' && th[1]=='I' && th[2]==42) { be = false; base = tiff; walkIFD(rd32at(tiff + 4), KN_NORMAL, 0); }
                        else if (th[0]=='M' && th[1]=='M' && th[3]==42) { be = true; base = tiff; walkIFD(rd32at(tiff + 4), KN_NORMAL, 0); }
                        be = sb; base = sbase;
                    }
                    break;
                }
            }
            pos += 2 + seglen;
        }
    }

    // ---------------- Fujifilm RAF: fixed header, full-size JPEG inside
    ArwResult parseRaf() {
        ArwResult r;
        uint8_t b[8];
        if (!rd(84, b, 8)) { r.err = "truncated RAF"; return r; }
        uint64_t joff = be32(b);
        uint32_t jlen = be32(b + 4);
        if (!joff || !jlen || joff + jlen > fsize) { r.err = "no RAF preview"; return r; }
        cands.push_back({ joff, jlen });
        scanJpegExif(joff, jlen);
        return finish();
    }

    // ---------------- plain JPEG (with optional Exif APP1, either endian)
    ArwResult parseJpeg() {
        ArwResult r;
        uint64_t pos = 2;
        uint8_t b[4];
        for (int guard = 0; guard < 64 && pos + 4 < fsize; guard++) {
            if (!rd(pos, b, 4)) break;
            if (b[0] != 0xFF) break;
            uint8_t m = b[1];
            if (m == 0xDA || m == 0xD9) break;
            uint32_t seglen = be16(b + 2);
            if (seglen < 2) break;
            if (m == 0xE1) {
                uint8_t sig[6];
                if (rd(pos + 4, sig, 6) && !memcmp(sig, "Exif\0\0", 6)) {
                    uint64_t tiff = pos + 10;
                    uint8_t th[8];
                    if (rd(tiff, th, 8)) {
                        if (th[0]=='I' && th[1]=='I' && th[2]==42) { be = false; base = tiff; walkIFD(rd32at(tiff + 4), KN_NORMAL, 0); }
                        else if (th[0]=='M' && th[1]=='M' && th[3]==42) { be = true; base = tiff; walkIFD(rd32at(tiff + 4), KN_NORMAL, 0); }
                    }
                    break;
                }
            }
            pos += 2 + seglen;
        }
        r.ok = true;
        r.jpegOff = 0;
        r.jpegLen = (uint32_t)std::min<uint64_t>(fsize, 0xFFFFFFFFu);
        r.exif = exif;
        return r;
    }

    // ---------------- Canon CR3 (ISO base media file)
    struct Trak { uint64_t off = 0; uint32_t len = 0; };

    ArwResult parseCr3() {
        walkBoxes(0, fsize, 0, nullptr);
        return finish();
    }

    void parseEmbeddedTiff(uint64_t off, uint64_t len, Kind firstKind) {
        if (!len || off + len > fsize) return;
        uint8_t th[8];
        if (!rd(off, th, 8)) return;
        bool save_be = be; uint64_t save_base = base;
        if (th[0]=='I' && th[1]=='I' && th[2]==42) be = false;
        else if (th[0]=='M' && th[1]=='M' && th[3]==42) be = true;
        else { return; }
        base = off;
        walkIFD(rd32at(off + 4), firstKind, 0);
        be = save_be; base = save_base;
    }

    void walkBoxes(uint64_t start, uint64_t end, int depth, Trak* trak) {
        static const uint8_t kCanonMetaUuid[16] = { 0x85,0xc0,0xb6,0x87,0x82,0x0f,0x11,0xe0,0x81,0x11,0xf4,0xce,0x46,0x2b,0x6a,0x48 };
        static const uint8_t kCanonPrvwUuid[16] = { 0xea,0xf4,0x2b,0x5e,0x1c,0x98,0x4b,0x88,0xb9,0xfb,0xb7,0xdc,0x40,0x6e,0x4d,0x16 };
        if (depth > 10) return;
        uint64_t pos = start;
        uint8_t h[16];
        int guard = 0;
        while (pos + 8 <= end && guard++ < 4096) {
            if (!rd(pos, h, 8)) return;
            uint64_t boxsz = be32(h);
            uint32_t hdrsz = 8;
            char type[5] = { (char)h[4], (char)h[5], (char)h[6], (char)h[7], 0 };
            if (boxsz == 1) {
                uint8_t l[8];
                if (!rd(pos + 8, l, 8)) return;
                boxsz = be64(l); hdrsz = 16;
            } else if (boxsz == 0) boxsz = end - pos;
            if (boxsz < hdrsz || pos + boxsz > end) return;
            uint64_t pay = pos + hdrsz, payEnd = pos + boxsz;

            if (!strcmp(type, "uuid") && payEnd - pay >= 16) {
                uint8_t u[16];
                if (rd(pay, u, 16)) {
                    if (!memcmp(u, kCanonMetaUuid, 16)) {
                        walkBoxes(pay + 16, payEnd, depth + 1, nullptr);
                    } else if (!memcmp(u, kCanonPrvwUuid, 16)) {
                        scanForPrvw(pay + 16, payEnd);
                    }
                }
            } else if (!strcmp(type, "moov") || !strcmp(type, "mdia") ||
                       !strcmp(type, "minf") || !strcmp(type, "stbl")) {
                walkBoxes(pay, payEnd, depth + 1, trak);
            } else if (!strcmp(type, "trak")) {
                Trak t;
                walkBoxes(pay, payEnd, depth + 1, &t);
                if (t.off && t.len) cands.push_back({ t.off, t.len });
            } else if (!strcmp(type, "CMT1")) {
                parseEmbeddedTiff(pay, payEnd - pay, KN_NORMAL);
            } else if (!strcmp(type, "CMT2")) {
                parseEmbeddedTiff(pay, payEnd - pay, KN_EXIF);
            } else if (trak && !strcmp(type, "stsz") && payEnd - pay >= 12) {
                uint8_t b[16];
                if (rd(pay, b, 16)) {
                    uint32_t fixed = be32(b + 4), count = be32(b + 8);
                    trak->len = fixed ? fixed : (count ? be32(b + 12) : 0);
                }
            } else if (trak && !strcmp(type, "stco") && payEnd - pay >= 12) {
                uint8_t b[12];
                if (rd(pay, b, 12) && be32(b + 4)) trak->off = be32(b + 8);
            } else if (trak && !strcmp(type, "co64") && payEnd - pay >= 16) {
                uint8_t b[16];
                if (rd(pay, b, 16) && be32(b + 4)) trak->off = be64(b + 8);
            }
            pos += boxsz;
        }
    }

    // PRVW box: header fields then a JPEG; locate FFD8 in the first bytes.
    void scanForPrvw(uint64_t start, uint64_t end) {
        uint8_t buf[96];
        uint32_t n = (uint32_t)std::min<uint64_t>(sizeof(buf), end > start ? end - start : 0);
        if (n < 16 || !rd(start, buf, n)) return;
        // find the PRVW child box then the JPEG inside it
        for (uint32_t i = 0; i + 4 <= n; i++) {
            if (!memcmp(buf + i, "PRVW", 4)) {
                uint64_t boxStart = start + i - 4;          // size precedes type
                uint8_t bh[4];
                uint64_t boxEnd = end;
                if (i >= 4) { boxEnd = boxStart + be32(buf + i - 4); if (boxEnd > end) boxEnd = end; }
                (void)bh;
                uint8_t j[64];
                uint32_t m = (uint32_t)std::min<uint64_t>(sizeof(j), boxEnd - (start + i));
                if (rd(start + i, j, m)) {
                    for (uint32_t k = 0; k + 3 <= m; k++) {
                        if (j[k] == 0xFF && j[k+1] == 0xD8 && j[k+2] == 0xFF) {
                            uint64_t joff = start + i + k;
                            if (boxEnd > joff) cands.push_back({ joff, (uint32_t)(boxEnd - joff) });
                            return;
                        }
                    }
                }
                return;
            }
        }
    }

    // ---------------- shared TIFF machinery
    ArwResult finish() {
        ArwResult r;
        r.exif = exif;
        Cand best{0, 0};
        for (auto& c : cands) {
            if (c.len <= best.len) continue;
            if (c.off + c.len > fsize) continue;
            uint8_t sig[3];
            if (!rd(c.off, sig, 3)) continue;
            if (sig[0] == 0xFF && sig[1] == 0xD8 && sig[2] == 0xFF) best = c;
        }
        if (!best.len) { r.err = "no embedded JPEG preview found"; return r; }
        if (!exif.expNum && !exif.iso)                 // e.g. RAF/ORF: exif rides in the preview
            scanJpegExif(best.off, best.len);
        r.ok = true; r.jpegOff = best.off; r.jpegLen = best.len;
        r.exif = exif;
        return r;
    }

    void walkIFD(uint64_t off, Kind kind, int depth) {
        if (off == 0 || depth > 8) return;
        uint64_t abs = base + off;
        if (visited.count(abs)) return;
        visited.insert(abs);
        uint16_t n = rd16at(abs);
        if (n == 0 || n > 1024) return;
        std::vector<uint8_t> ents(n * 12u);
        if (!rd(abs + 2, ents.data(), n * 12u)) return;

        uint64_t jOff = 0; uint32_t jLen = 0;
        // CR2: IFD0 carries the big preview via strips with old-JPEG compression
        bool atIfd0 = (kind == KN_NORMAL) && !sawFirstIfd;
        if (atIfd0) sawFirstIfd = true;
        uint32_t comp = 0, stripOff = 0, stripLen = 0;
        uint32_t subType = 0xFFFF;             // NewSubfileType (DNG preview IFDs = 1)
        uint64_t rw2Off = 0; uint32_t rw2Len = 0;
        uint32_t olyOff = 0, olyLen = 0;       // Olympus CameraSettings preview

        for (uint16_t i = 0; i < n; i++) {
            const uint8_t* e = ents.data() + i * 12;
            uint16_t tag = rd16(e), typ = rd16(e + 2);
            uint32_t cnt = rd32(e + 4);
            uint32_t val = rd32(e + 8);
            uint64_t byteLen = (uint64_t)typeSize(typ) * cnt;
            uint64_t dpos = (byteLen <= 4) ? (abs + 2 + i * 12u + 8) : (base + val);

            if (kind == KN_NORMAL) {
                switch (tag) {
                case 0x0110: if (exif.model.empty()) exif.model = readAscii(dpos, cnt); break;
                case 0x0112: if (!haveOrient) { exif.orientation = (int)readUint(typ, dpos); haveOrient = true; } break;
                case 0x00FE: subType = readUint(typ, dpos); break;
                case 0x002E: if (typ == 7 && cnt > 4) { rw2Off = base + val; rw2Len = cnt; } break;
                case 0x0103: comp = readUint(typ, dpos); break;
                case 0x0111: if (cnt == 1) stripOff = readUint(typ, dpos); break;
                case 0x0117: if (cnt == 1) stripLen = readUint(typ, dpos); break;
                case 0x0201: jOff = base + val; break;
                case 0x0202: jLen = readUint(typ, dpos); break;
                case 0x014A: {
                    uint32_t m = std::min<uint32_t>(cnt, 16);
                    for (uint32_t k = 0; k < m; k++)
                        walkIFD(rd32at(dpos + 4ull * k), KN_NORMAL, depth + 1);
                    break; }
                case 0x8769: walkIFD(val, KN_EXIF, depth + 1); break;
                }
            } else if (kind == KN_OLY_MN) {
                // Olympus makernote IFD: 0x2020 = CameraSettings sub-IFD (rel. offsets)
                if (tag == 0x2020) walkIFD(val, KN_OLY_CS, depth + 1);
            } else if (kind == KN_OLY_CS) {
                if (tag == 0x0101) olyOff = readUint(typ, dpos);
                else if (tag == 0x0102) olyLen = readUint(typ, dpos);
            } else { // KN_EXIF
                switch (tag) {
                case 0x829A: readRational(dpos, exif.expNum, exif.expDen); break;
                case 0x829D: readRational(dpos, exif.fnNum, exif.fnDen); break;
                case 0x8827: if (!exif.iso) exif.iso = readUint(typ, dpos); break;
                case 0x9003: if (exif.dateTime.empty()) exif.dateTime = readAscii(dpos, cnt); break;
                case 0x9204: {
                    uint32_t a, b2;
                    if (readRational(dpos, a, b2)) { exif.evNum = (int32_t)a; exif.evDen = (int32_t)b2; exif.hasEv = true; }
                    break; }
                case 0x920A: readRational(dpos, exif.flNum, exif.flDen); break;
                case 0xA434: if (exif.lens.empty()) exif.lens = readAscii(dpos, cnt); break;
                case 0x927C: {
                    // Olympus makernote: IFD at +12, offsets relative to makernote start
                    uint8_t sig[12];
                    uint64_t mnAbs = base + val;
                    if (cnt > 16 && rd(mnAbs, sig, 12) && !memcmp(sig, "OLYMPUS\0", 8)) {
                        uint64_t sbase = base;
                        base = mnAbs;
                        walkIFD(12, KN_OLY_MN, depth + 1);
                        base = sbase;
                    }
                    break; }
                }
            }
        }
        if (jOff && jLen) cands.push_back({jOff, jLen});
        if (atIfd0 && comp == 6 && stripOff && stripLen)
            cands.push_back({ base + stripOff, stripLen });      // CR2 big preview
        if (subType == 1 && (comp == 6 || comp == 7) && stripOff && stripLen)
            cands.push_back({ base + stripOff, stripLen });      // DNG preview IFD
        if (rw2Len) {                                            // Panasonic JpgFromRaw
            cands.push_back({ rw2Off, rw2Len });
            scanJpegExif(rw2Off, rw2Len);
        }
        if (olyOff && olyLen)
            cands.push_back({ base + olyOff, olyLen });          // Olympus preview

        if (kind == KN_NORMAL)
            walkIFD(rd32at(abs + 2 + n * 12u), KN_NORMAL, depth);
    }

    uint32_t readUint(uint16_t typ, uint64_t dpos) {
        if (typ == 3) return rd16at(dpos);
        return rd32at(dpos);
    }
    bool readRational(uint64_t dpos, uint32_t& a, uint32_t& b) {
        uint8_t buf[8];
        if (!rd(dpos, buf, 8)) return false;
        a = rd32(buf); b = rd32(buf + 4);
        return true;
    }
    std::string readAscii(uint64_t dpos, uint32_t cnt) {
        cnt = std::min<uint32_t>(cnt, 256);
        std::vector<char> buf(cnt + 1, 0);
        if (!rd(dpos, buf.data(), cnt)) return "";
        std::string s(buf.data());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
        return s;
    }
};

// ---------------------------------------------------------------- exif format
inline std::string fmtShutter(uint32_t n, uint32_t d) {
    if (!n || !d) return "";
    double v = (double)n / d;
    char b[32];
    if (v >= 0.25) snprintf(b, 32, "%.1fs", v);
    else snprintf(b, 32, "1/%d", (int)(d / (double)n + 0.5));
    return b;
}
inline std::string fmtAperture(uint32_t n, uint32_t d) {
    if (!n || !d) return "";
    char b[32]; double v = (double)n / d;
    snprintf(b, 32, (v == (int)v) ? "f/%.0f" : "f/%.1f", v);
    return b;
}
inline std::string fmtFocal(uint32_t n, uint32_t d) {
    if (!n || !d) return "";
    char b[32]; double v = (double)n / d;
    snprintf(b, 32, (v == (int)v) ? "%.0fmm" : "%.1fmm", v);
    return b;
}
inline std::string fmtEv(int32_t n, int32_t d, bool has) {
    if (!has || !d || !n) return "";
    char b[32]; snprintf(b, 32, "%+.1fEV", (double)n / d);
    return b;
}

// ---------------------------------------------------------------- XMP sidecar
static const char* kLabelNames[6] = { "", "Red", "Yellow", "Green", "Blue", "Purple" };
const int kXmpKeep = -1000;   // sentinel: leave this field unchanged
// rating -1 = rejected (Bridge / Photo Mechanic convention: xmp:Rating="-1")

inline std::string xmpFresh(int rating, int label) {
    std::string s;
    s += "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n";
    s += "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"iRate 1.0\">\n";
    s += " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n";
    s += "  <rdf:Description rdf:about=\"\"\n";
    s += "    xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\"\n";
    s += "    xmp:Rating=\"" + std::to_string(rating < -1 ? 0 : rating) + "\"\n";
    s += "    xmp:Label=\"" + std::string(kLabelNames[(label >= 0 && label <= 5) ? label : 0]) + "\"/>\n";
    s += " </rdf:RDF>\n";
    s += "</x:xmpmeta>\n";
    s += "<?xpacket end=\"w\"?>";
    return s;
}

inline bool xmpReplace(std::string& s, const std::string& name, const std::string& val) {
    size_t pos = 0;
    while ((pos = s.find(name, pos)) != std::string::npos) {
        size_t p = pos + name.size();
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) p++;
        if (p < s.size() && s[p] == '=') {
            p++;
            while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) p++;
            if (p < s.size() && (s[p] == '"' || s[p] == '\'')) {
                size_t e = s.find(s[p], p + 1);
                if (e != std::string::npos) { s.replace(p + 1, e - p - 1, val); return true; }
            }
        }
        pos += name.size();
    }
    std::string open = "<" + name, close = "</" + name + ">";
    size_t o = s.find(open);
    if (o != std::string::npos) {
        size_t gt = s.find('>', o);
        if (gt != std::string::npos && s[gt - 1] != '/') {
            size_t c = s.find(close, gt);
            if (c != std::string::npos) { s.replace(gt + 1, c - gt - 1, val); return true; }
        }
    }
    return false;
}

inline bool xmpGet(const std::string& s, const std::string& name, std::string& out) {
    size_t pos = 0;
    while ((pos = s.find(name, pos)) != std::string::npos) {
        size_t p = pos + name.size();
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) p++;
        if (p < s.size() && s[p] == '=') {
            p++;
            while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) p++;
            if (p < s.size() && (s[p] == '"' || s[p] == '\'')) {
                size_t e = s.find(s[p], p + 1);
                if (e != std::string::npos) { out = s.substr(p + 1, e - p - 1); return true; }
            }
        }
        pos += name.size();
    }
    std::string open = "<" + name, close = "</" + name + ">";
    size_t o = s.find(open);
    if (o != std::string::npos) {
        size_t gt = s.find('>', o);
        if (gt != std::string::npos && s[gt - 1] != '/') {
            size_t c = s.find(close, gt);
            if (c != std::string::npos) { out = s.substr(gt + 1, c - gt - 1); return true; }
        }
    }
    return false;
}

inline std::string xmpApply(const std::string& existing, int rating, int label) {
    if (existing.empty() || existing.find("<rdf:Description") == std::string::npos)
        return xmpFresh(rating == kXmpKeep ? 0 : rating, label == kXmpKeep ? 0 : label);
    std::string s = existing;
    auto setOne = [&](const std::string& name, const std::string& val) {
        if (xmpReplace(s, name, val)) return;
        size_t d = s.find("<rdf:Description");
        if (d == std::string::npos) return;
        std::string ins = " " + name + "=\"" + val + "\"";
        if (s.find("xmlns:xmp") == std::string::npos)
            ins = " xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\"" + ins;
        s.insert(d + strlen("<rdf:Description"), ins);
    };
    if (rating != kXmpKeep && rating >= -1) setOne("xmp:Rating", std::to_string(rating));
    if (label != kXmpKeep && label >= 0)  setOne("xmp:Label", kLabelNames[label <= 5 ? label : 0]);
    return s;
}

inline void xmpParse(const std::string& s, int& rating, int& label) {
    rating = 0; label = 0;
    std::string v;
    if (xmpGet(s, "xmp:Rating", v) && !v.empty()) rating = atoi(v.c_str());
    if (rating < -1) rating = 0; if (rating > 5) rating = 5;   // -1 = rejected
    if (xmpGet(s, "xmp:Label", v)) {
        for (int i = 1; i <= 5; i++)
            if (v == kLabelNames[i]) { label = i; break; }
    }
}

// ------------------------------------------------- keywords (dc:subject bag)
// Lightroom/Bridge keywords live in the sidecar as an rdf:Bag of rdf:li under
// dc:subject, element-form inside rdf:Description:
//   <rdf:Description ... xmlns:dc="http://purl.org/dc/elements/1.1/">
//     <dc:subject><rdf:Bag><rdf:li>ceremony</rdf:li>...</rdf:Bag></dc:subject>
//   </rdf:Description>
// Same string-patching philosophy as rating/label: the WHOLE dc:subject block is
// replaced on write (callers pass the image's full keyword list each time), all
// other sidecar content (crs:* develop settings etc.) is preserved byte-for-byte.
// Shared by both shells — keep this section platform-clean.

inline std::string xmlEscape(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (char c : s) switch (c) {
        case '&': o += "&amp;"; break;  case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;   case '"': o += "&quot;"; break;
        default:  o += c;
    }
    return o;
}
inline std::string xmlUnescape(std::string s) {
    auto rep = [&](const char* a, const char* b) {
        size_t p = 0, la = strlen(a), lb = strlen(b);
        while ((p = s.find(a, p)) != std::string::npos) { s.replace(p, la, b); p += lb; }
    };
    rep("&lt;", "<"); rep("&gt;", ">"); rep("&quot;", "\""); rep("&#39;", "'");
    rep("&amp;", "&");   // must be last (avoids double-decoding &amp;lt;)
    return s;
}

// Read the image's keywords from a sidecar. Empty vector = none / no dc:subject.
inline std::vector<std::string> xmpGetKeywords(const std::string& s) {
    std::vector<std::string> out;
    size_t p = s.find("<dc:subject");
    if (p == std::string::npos) return out;
    size_t end = s.find("</dc:subject>", p);
    if (end == std::string::npos) return out;
    size_t pos = p;
    while (true) {
        size_t li = s.find("<rdf:li", pos);
        if (li == std::string::npos || li > end) break;
        size_t gt = s.find('>', li);
        if (gt == std::string::npos || gt > end) break;
        size_t close = s.find("</rdf:li>", gt);
        if (close == std::string::npos || close > end) break;
        std::string v = xmlUnescape(s.substr(gt + 1, close - gt - 1));
        if (!v.empty()) out.push_back(v);
        pos = close + 9;
    }
    return out;
}

// Write the FULL keyword list into a sidecar (replaces any existing dc:subject;
// an empty list removes it). Creates a fresh sidecar (rating 0, no label) when
// existing is empty/garbage — call after xmpApply if you're also writing those.
inline std::string xmpApplyKeywords(const std::string& existing,
                                    const std::vector<std::string>& kws) {
    std::string s = existing;
    if (s.empty() || s.find("<rdf:Description") == std::string::npos)
        s = xmpFresh(0, 0);
    // drop any existing dc:subject block
    size_t p = s.find("<dc:subject");
    if (p != std::string::npos) {
        size_t end = s.find("</dc:subject>", p);
        if (end != std::string::npos) s.erase(p, end + 13 - p);
        else { size_t sc = s.find("/>", p); if (sc != std::string::npos) s.erase(p, sc + 2 - p); }
    }
    if (kws.empty()) return s;
    std::string bag = "<dc:subject><rdf:Bag>";
    for (auto& k : kws) bag += "<rdf:li>" + xmlEscape(k) + "</rdf:li>";
    bag += "</rdf:Bag></dc:subject>";
    // ensure the dc namespace is declared on rdf:Description
    size_t d = s.find("<rdf:Description");
    if (d == std::string::npos) return s;
    if (s.find("xmlns:dc") == std::string::npos) {
        s.insert(d + strlen("<rdf:Description"),
                 " xmlns:dc=\"http://purl.org/dc/elements/1.1/\"");
        d = s.find("<rdf:Description");
    }
    // insert the bag as element content (converting a self-closing tag if needed)
    size_t gt = s.find('>', d);
    if (gt == std::string::npos) return s;
    if (s[gt - 1] == '/') s.replace(gt - 1, 2, ">" + bag + "</rdf:Description>");
    else                  s.insert(gt + 1, bag);
    return s;
}
