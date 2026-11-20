// DH-S1b premise: what a dependency-free zero-suppression packer makes of
// the images DH-S0 sampled. Not engine code: the engine's packer is
// `EncodePackedPageImage` (`src/wal/payload.cpp`), which this prototyped.
//
//   g++ -O2 -std=c++20 -o zs_premise zs_premise.cpp
//   ./zs_premise <dir>/k1 <dir>/k2 ...      (reads <label>.pages, <label>.index)
//
// <label>.pages is concatenated 8,192-byte pages, <label>.index one
// "lsn,page_id,type" line a page (`tools/dh_image_census.py`). **The sample
// is the first images by LSN of each page type, capped a type, not an even
// stride over the cell's images**, so a label's "all" row over-weights the
// rarer types; the per-type rows are what to read.
//
// Encoding measured ("Z1b"): a 128-byte bitmap of the page's nonzero 8-byte
// words, then per nonzero word a mask byte (bit i = byte i nonzero) and its
// nonzero bytes. Sizes reported beside it, not timed: Z1 (a mask byte for
// every word, no bitmap) and Z2 (byte runs: zero_len u16, lit_len u16,
// literals; zero runs of 4 or more).
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static constexpr std::size_t kPage = 8192;
static constexpr std::size_t kWords = kPage / 8;
static constexpr std::size_t kSlot = kPage + kWords + 200;

static std::size_t PackZ1b(const unsigned char* in, unsigned char* out) {
    unsigned char* bitmap = out;
    std::memset(bitmap, 0, kWords / 8);
    unsigned char* p = out + kWords / 8;
    for (std::size_t w = 0; w < kWords; ++w) {
        std::uint64_t v;
        std::memcpy(&v, in + w * 8, 8);
        if (v == 0) continue;
        bitmap[w >> 3] |= static_cast<unsigned char>(1u << (w & 7));
        unsigned char* mask = p++;
        unsigned m = 0;
        for (unsigned b = 0; b < 8; ++b) {
            const unsigned char c = in[w * 8 + b];
            *p = c;
            const unsigned nz = c != 0;
            p += nz;
            m |= nz << b;
        }
        *mask = static_cast<unsigned char>(m);
    }
    return static_cast<std::size_t>(p - out);
}

static bool UnpackZ1b(const unsigned char* in, std::size_t n, unsigned char* out) {
    if (n < kWords / 8) return false;
    const unsigned char* bitmap = in;
    const unsigned char* p = in + kWords / 8;
    const unsigned char* end = in + n;
    std::memset(out, 0, kPage);
    for (std::size_t w = 0; w < kWords; ++w) {
        if ((bitmap[w >> 3] & (1u << (w & 7))) == 0) continue;
        if (p == end) return false;
        const unsigned m = *p++;
        if (m == 0) return false;
        for (unsigned b = 0; b < 8; ++b) {
            if (m & (1u << b)) {
                if (p == end) return false;
                out[w * 8 + b] = *p++;
            }
        }
    }
    return p == end;
}

static std::size_t SizeZ1(const unsigned char* in) {
    std::size_t n = kWords;
    for (std::size_t i = 0; i < kPage; ++i) n += in[i] != 0;
    return n;
}

static std::size_t SizeZ2(const unsigned char* in) {
    std::size_t n = 0, i = 0;
    while (i < kPage) {
        std::size_t z = i;
        while (z < kPage && in[z] == 0) ++z;
        std::size_t l = z;  // the literal run: until a zero run of 4 or more, or the end
        while (l < kPage) {
            if (in[l] == 0) {
                std::size_t k = l;
                while (k < kPage && in[k] == 0) ++k;
                if (k - l >= 4 || k == kPage) break;
                l = k;
            } else {
                ++l;
            }
        }
        n += 4 + (l - z);
        i = l;
    }
    return n;
}

static const char* TypeName(int t) {
    switch (t) {
        case 1: return "heap";
        case 2: return "btree internal";
        case 3: return "btree leaf";
        case 4: return "undo";
        case 10: return "var-heap";
        case 11: return "index internal";
        case 12: return "index leaf";
        case 13: return "bound cabin";
        case 14: return "anchor";
        default: return "other";
    }
}

// One group of pages: sizes, then pack and unpack-and-compare timed over
// five passes, the best kept.
static int Report(const std::string& label, const char* group, const std::vector<unsigned char>& buf) {
    const std::size_t pages = buf.size() / kPage;
    if (pages == 0) return 0;
    std::vector<unsigned char> out(pages * kSlot);
    std::vector<std::size_t> sizes(pages);
    double z1 = 0, z2 = 0, z1b = 0;
    for (std::size_t i = 0; i < pages; ++i) {
        z1 += SizeZ1(&buf[i * kPage]);
        z2 += SizeZ2(&buf[i * kPage]);
    }
    double best_pack = 1e30, best_unpack = 1e30;
    for (int rep = 0; rep < 5; ++rep) {
        const auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < pages; ++i) sizes[i] = PackZ1b(&buf[i * kPage], &out[i * kSlot]);
        const auto t1 = std::chrono::steady_clock::now();
        unsigned char back[kPage];
        bool ok = true;
        for (std::size_t i = 0; i < pages; ++i) {
            ok &= UnpackZ1b(&out[i * kSlot], sizes[i], back);
            ok &= std::memcmp(back, &buf[i * kPage], kPage) == 0;
        }
        const auto t2 = std::chrono::steady_clock::now();
        if (!ok) {
            std::printf("%s %s: ROUND TRIP FAILED\n", label.c_str(), group);
            return 2;
        }
        best_pack = std::min(best_pack, std::chrono::duration<double, std::micro>(t1 - t0).count() / pages);
        best_unpack = std::min(best_unpack, std::chrono::duration<double, std::micro>(t2 - t1).count() / pages);
    }
    std::size_t worst = 0, not_smaller = 0;
    for (std::size_t s : sizes) {
        z1b += s;
        worst = std::max(worst, s);
        not_smaller += s >= kPage;
    }
    std::vector<std::size_t> sorted = sizes;
    std::sort(sorted.begin(), sorted.end());
    std::printf("%-4s %-15s %5zu pages | Z1 %5.0f B | Z2 %5.0f B | Z1b mean %5.0f B (%.3f of a page), "
                "p50 %4zu B, p90 %4zu B, max %4zu B, %zu not smaller than a page | "
                "pack %5.2f us, unpack and compare %5.2f us a page\n",
                label.c_str(), group, pages, z1 / pages, z2 / pages, z1b / pages, z1b / pages / kPage,
                sorted[pages / 2], sorted[pages * 9 / 10], worst, not_smaller, best_pack, best_unpack);
    return 0;
}

int main(int argc, char** argv) {
    for (int a = 1; a < argc; ++a) {
        const std::string base = argv[a];
        const std::string label = base.substr(base.find_last_of('/') + 1);
        std::FILE* f = std::fopen((base + ".pages").c_str(), "rb");
        std::FILE* g = std::fopen((base + ".index").c_str(), "r");
        if (f == nullptr || g == nullptr) {
            std::perror(base.c_str());
            return 1;
        }
        std::vector<unsigned char> all;
        std::map<int, std::vector<unsigned char>> by_type;
        unsigned char page[kPage];
        char line[256];
        while (std::fread(page, 1, kPage, f) == kPage && std::fgets(line, sizeof(line), g) != nullptr) {
            unsigned long long lsn = 0, page_id = 0;
            int type = 0;
            if (std::sscanf(line, "%llu,%llu,%d", &lsn, &page_id, &type) != 3) {
                std::printf("%s: bad index line\n", label.c_str());
                return 1;
            }
            all.insert(all.end(), page, page + kPage);
            by_type[type].insert(by_type[type].end(), page, page + kPage);
        }
        std::fclose(f);
        std::fclose(g);
        if (int rc = Report(label, "all (sample)", all); rc != 0) return rc;
        for (const auto& [type, buf] : by_type) {
            if (int rc = Report(label, TypeName(type), buf); rc != 0) return rc;
        }
    }
    return 0;
}
