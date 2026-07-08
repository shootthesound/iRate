// Linux test harness for core.h
#include "core.h"
#include <cstdio>
#include <cassert>

class FileSource : public ByteSource {
public:
    explicit FileSource(const char* p) { f = fopen(p, "rb"); if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); } }
    ~FileSource() { if (f) fclose(f); }
    bool ok() const { return f != nullptr; }
    bool read(uint64_t off, void* dst, uint32_t n) override {
        if (!f || fseek(f, (long)off, SEEK_SET)) return false;
        return fread(dst, 1, n, f) == n;
    }
    uint64_t size() override { return sz; }
private:
    FILE* f = nullptr; uint64_t sz = 0;
};

int main(int argc, char** argv) {
    int fails = 0;
    // ---- parse every file given on cmdline
    for (int i = 1; i < argc; i++) {
        FileSource src(argv[i]);
        if (!src.ok()) { printf("FAIL open %s\n", argv[i]); fails++; continue; }
        ArwParser p(src);
        ArwResult r = p.parse();
        if (!r.ok) { printf("FAIL parse %s: %s\n", argv[i], r.err.c_str()); fails++; continue; }
        // preview must be a JPEG
        uint8_t sig[3] = {0,0,0};
        src.read(r.jpegOff, sig, 3);
        bool jok = sig[0]==0xFF && sig[1]==0xD8;
        uint8_t tail[2] = {0,0};
        src.read(r.jpegOff + r.jpegLen - 2, tail, 2);
        printf("%s\n  preview off=%llu len=%u sigOK=%d endOK=%d\n", argv[i],
               (unsigned long long)r.jpegOff, r.jpegLen, jok, tail[0]==0xFF && tail[1]==0xD9);
        printf("  model='%s' lens='%s' dt='%s' orient=%d\n", r.exif.model.c_str(),
               r.exif.lens.c_str(), r.exif.dateTime.c_str(), r.exif.orientation);
        printf("  shutter=%s aperture=%s iso=%u focal=%s ev=%s\n",
               fmtShutter(r.exif.expNum, r.exif.expDen).c_str(),
               fmtAperture(r.exif.fnNum, r.exif.fnDen).c_str(), r.exif.iso,
               fmtFocal(r.exif.flNum, r.exif.flDen).c_str(),
               fmtEv(r.exif.evNum, r.exif.evDen, r.exif.hasEv).c_str());
        if (!jok) fails++;
        if (r.exif.model.empty() || !r.exif.expNum || !r.exif.iso) { printf("  FAIL exif missing\n"); fails++; }
        // preview must be the BIG one for a7R5 files (>1MB)
        if (r.exif.model == "ILCE-7RM5" && r.jpegLen < 1000000) { printf("  FAIL picked small preview\n"); fails++; }
    }

    // ---- XMP tests
    {
        std::string fresh = xmpFresh(3, 1);
        int rr, ll; xmpParse(fresh, rr, ll);
        if (rr != 3 || ll != 1) { printf("FAIL xmp fresh roundtrip r=%d l=%d\n", rr, ll); fails++; }

        std::string p1 = xmpApply(fresh, 5, kXmpKeep);     // change rating only
        xmpParse(p1, rr, ll);
        if (rr != 5 || ll != 1) { printf("FAIL xmp patch rating r=%d l=%d\n", rr, ll); fails++; }

        std::string p2 = xmpApply(p1, kXmpKeep, 4);        // change label only
        xmpParse(p2, rr, ll);
        if (rr != 5 || ll != 4) { printf("FAIL xmp patch label r=%d l=%d\n", rr, ll); fails++; }

        std::string p3 = xmpApply(p2, 0, 0);               // clear both
        xmpParse(p3, rr, ll);
        if (rr != 0 || ll != 0) { printf("FAIL xmp clear r=%d l=%d\n", rr, ll); fails++; }

        // Lightroom-style sidecar with element form and other data preserved
        std::string lr =
            "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
            "<rdf:Description rdf:about=\"\" xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\" "
            "xmlns:crs=\"http://ns.adobe.com/camera-raw-settings/1.0/\" crs:Temperature=\"5500\">"
            "<xmp:Rating>2</xmp:Rating></rdf:Description></rdf:RDF></x:xmpmeta>";
        std::string p4 = xmpApply(lr, 4, 2);
        xmpParse(p4, rr, ll);
        if (rr != 4 || ll != 2) { printf("FAIL xmp LR-style patch r=%d l=%d\n%s\n", rr, ll, p4.c_str()); fails++; }
        if (p4.find("crs:Temperature=\"5500\"") == std::string::npos) { printf("FAIL xmp lost other data\n"); fails++; }

        // sidecar with NO xmp ns declared -> insertion must add xmlns
        std::string bare = "<x:xmpmeta><rdf:RDF><rdf:Description rdf:about=\"\"/></rdf:RDF></x:xmpmeta>";
        std::string p5 = xmpApply(bare, 1, 3);
        xmpParse(p5, rr, ll);
        if (rr != 1 || ll != 3) { printf("FAIL xmp bare insert r=%d l=%d\n%s\n", rr, ll, p5.c_str()); fails++; }
        if (p5.find("xmlns:xmp") == std::string::npos) { printf("FAIL missing xmlns\n"); fails++; }

        // reject roundtrip (xmp:Rating="-1")
        std::string p7 = xmpApply(p3, -1, kXmpKeep);
        xmpParse(p7, rr, ll);
        if (rr != -1 || ll != 0) { printf("FAIL reject roundtrip r=%d l=%d\n", rr, ll); fails++; }
        std::string p8 = xmpApply(p7, 3, kXmpKeep);        // un-reject by rating
        xmpParse(p8, rr, ll);
        if (rr != 3) { printf("FAIL unreject r=%d\n", rr); fails++; }

        // garbage in -> fresh doc out
        std::string p6 = xmpApply("random garbage", 2, 0);
        xmpParse(p6, rr, ll);
        if (rr != 2) { printf("FAIL xmp garbage fallback\n"); fails++; }
    }

    // ---- keyword (dc:subject) tests
    {
        // fresh create + read back
        std::string k1 = xmpApplyKeywords("", { "ceremony", "top table" });
        auto kws = xmpGetKeywords(k1);
        if (kws.size() != 2 || kws[0] != "ceremony" || kws[1] != "top table") {
            printf("FAIL kw fresh create (%zu)\n", kws.size()); fails++;
        }
        int rr, ll; xmpParse(k1, rr, ll);
        if (rr != 0 || ll != 0) { printf("FAIL kw fresh doc rating/label\n"); fails++; }

        // escape round-trip
        std::string k2 = xmpApplyKeywords(k1, { "R&D <crew>", "say \"cheese\"" });
        kws = xmpGetKeywords(k2);
        if (kws.size() != 2 || kws[0] != "R&D <crew>" || kws[1] != "say \"cheese\"") {
            printf("FAIL kw escape roundtrip\n"); fails++;
        }

        // rating patch preserves keywords, keyword patch preserves rating
        std::string k3 = xmpApply(k2, 4, 2);
        kws = xmpGetKeywords(k3);
        if (kws.size() != 2 || kws[0] != "R&D <crew>") { printf("FAIL kw lost on rating patch\n"); fails++; }
        std::string k4 = xmpApplyKeywords(k3, { "solo" });
        xmpParse(k4, rr, ll);
        if (rr != 4 || ll != 2) { printf("FAIL rating lost on kw patch r=%d l=%d\n", rr, ll); fails++; }
        kws = xmpGetKeywords(k4);
        if (kws.size() != 1 || kws[0] != "solo") { printf("FAIL kw replace\n"); fails++; }

        // empty list removes the block entirely
        std::string k5 = xmpApplyKeywords(k4, {});
        if (k5.find("dc:subject") != std::string::npos) { printf("FAIL kw clear leaves block\n"); fails++; }
        if (!xmpGetKeywords(k5).empty()) { printf("FAIL kw clear readback\n"); fails++; }

        // LR-style self-closing Description with crs settings: keywords added,
        // crs preserved, xmlns:dc inserted
        std::string lr2 =
            "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
            "<rdf:Description rdf:about=\"\" xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\" "
            "xmlns:crs=\"http://ns.adobe.com/camera-raw-settings/1.0/\" xmp:Rating=\"3\" crs:Temperature=\"5500\"/>"
            "</rdf:RDF></x:xmpmeta>";
        std::string k6 = xmpApplyKeywords(lr2, { "podium" });
        kws = xmpGetKeywords(k6);
        if (kws.size() != 1 || kws[0] != "podium") { printf("FAIL kw self-closing insert\n%s\n", k6.c_str()); fails++; }
        if (k6.find("crs:Temperature=\"5500\"") == std::string::npos) { printf("FAIL kw lost crs\n"); fails++; }
        if (k6.find("xmlns:dc") == std::string::npos) { printf("FAIL kw missing dc ns\n"); fails++; }
        xmpParse(k6, rr, ll);
        if (rr != 3) { printf("FAIL kw self-closing rating r=%d\n", rr); fails++; }
    }
    printf(fails ? "\n%d FAILURES\n" : "\nALL TESTS PASSED\n", fails);
    return fails ? 1 : 0;
}
