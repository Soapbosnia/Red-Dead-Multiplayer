/*
 * The game's own fonts and logo art for the menu: see rdr_assets.h. Formats in re/NOTES.md "Game fonts and title art".
 */

#include <utils/safe_win32.h>

#include <bcrypt.h>

#include <imgui.h>
#include <imgui_internal.h>

#include "client/fw_client.h"
#include "client/rdr_assets.h"
#include "shared/fw_api.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace RDMP::RdrAssets {
    namespace {
        const rdmp::fw::Host *g_host = nullptr;
        bool g_tried                 = false;
        bool g_loaded                = false;

        // ---- RPF6 ----
        // The PC archives' table of contents: AES-256-ECB, applied 16 times (the game's TOC routine loops it).
        constexpr uint8_t kRpfKey[32] = {0xB7, 0x62, 0xDF, 0xB6, 0xE2, 0xB2, 0xC6, 0xDE, 0xAF, 0x72, 0x2A, 0x32, 0xD2, 0xFB, 0x6F, 0x0C,
                                         0x98, 0xA3, 0x21, 0x74, 0x62, 0xC9, 0xC4, 0xED, 0xAD, 0xAA, 0x2E, 0xD0, 0xDD, 0xF9, 0x2F, 0x10};

        uint32_t BigEndian(const uint8_t *p) {
            return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        }

        bool AesDecrypt16(std::vector<uint8_t> &data) {
            BCRYPT_ALG_HANDLE alg = nullptr;
            BCRYPT_KEY_HANDLE key = nullptr;
            bool ok               = false;
            if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) == 0 &&
                BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_ECB)),
                                  sizeof(BCRYPT_CHAIN_MODE_ECB), 0) == 0 &&
                BCryptGenerateSymmetricKey(alg, &key, nullptr, 0, const_cast<PUCHAR>(kRpfKey), sizeof(kRpfKey), 0) == 0) {
                ok = true;
                for (int i = 0; i < 16 && ok; ++i) {
                    ULONG done = 0;
                    ok = BCryptDecrypt(key, data.data(), static_cast<ULONG>(data.size()), nullptr, nullptr, 0, data.data(), static_cast<ULONG>(data.size()),
                                       &done, 0) == 0;
                }
            }
            if (key) BCryptDestroyKey(key);
            if (alg) BCryptCloseAlgorithmProvider(alg, 0);
            return ok;
        }

        class Rpf {
          public:
            bool Open(const std::filesystem::path &path) {
                _file.open(path, std::ios::binary);
                uint8_t head[16];
                if (!_file.read(reinterpret_cast<char *>(head), 16) || BigEndian(head) != 0x52504636) { // "RPF6"
                    return false;
                }
                const uint32_t count = BigEndian(head + 4);
                const bool enc       = BigEndian(head + 12) != 0;
                std::vector<uint8_t> toc((((count * 5) << 2) + 15) & ~15u);
                if (!_file.read(reinterpret_cast<char *>(toc.data()), toc.size()) || (enc && !AesDecrypt16(toc))) {
                    return false;
                }
                _entries.resize(count);
                for (uint32_t i = 0; i < count; ++i) {
                    const uint8_t *e = toc.data() + i * 20;
                    Entry &out       = _entries[i];
                    out.hash         = BigEndian(e);
                    out.dir          = e[8] == 0x80;
                    out.size         = BigEndian(e + 4) & 0x0FFFFFFF;
                    out.offset       = BigEndian(e + 8);
                    out.flag1        = BigEndian(e + 12);
                }
                return true;
            }

            // The decoded payload of the first file entry with this name hash (RSC5 resource or plain zstd).
            bool Payload(uint32_t hash, std::vector<uint8_t> &out) {
                for (const Entry &e : _entries) {
                    if (e.dir || e.hash != hash || e.size == 0) {
                        continue;
                    }
                    const uint64_t at = (e.flag1 & 0x80000000) ? uint64_t(e.offset & 0x7FFFFF00) * 8 : uint64_t(e.offset & 0x7FFFFFFF) * 8;
                    std::vector<uint8_t> raw(e.size);
                    _file.clear();
                    _file.seekg(static_cast<std::streamoff>(at));
                    if (!_file.read(reinterpret_cast<char *>(raw.data()), raw.size())) {
                        return false;
                    }
                    size_t skip = 0;
                    if (raw.size() > 16 && std::memcmp(raw.data(), "RSC\x05", 4) == 0) {
                        skip = 12;
                    }
                    if (raw.size() < skip + 4 || BigEndian(raw.data() + skip) != 0x28B52FFD) {
                        out = std::move(raw); // stored as is
                        return true;
                    }
                    for (uint64_t cap = std::max<uint64_t>(raw.size() * 8, 1 << 20); cap <= (256ull << 20); cap *= 2) {
                        out.resize(cap);
                        uint64_t got = 0;
                        const int rc = g_host->inflate(raw.data() + skip, raw.size() - skip, out.data(), cap, &got);
                        if (rc == 1) {
                            out.resize(got);
                            return true;
                        }
                        if (rc != 2) {
                            return false;
                        }
                    }
                    return false;
                }
                return false;
            }

          private:
            struct Entry {
                uint32_t hash = 0, size = 0, offset = 0, flag1 = 0;
                bool dir = false;
            };
            std::ifstream _file;
            std::vector<Entry> _entries;
        };

        // ---- DXT ----
        void Color565(uint16_t c, uint8_t out[3]) {
            out[0] = static_cast<uint8_t>(((c >> 11) & 31) * 255 / 31);
            out[1] = static_cast<uint8_t>(((c >> 5) & 63) * 255 / 63);
            out[2] = static_cast<uint8_t>((c & 31) * 255 / 31);
        }

        // RGBA8 from DXT1 / DXT5 blocks.
        bool DecodeDxt(const uint8_t *src, size_t srcLen, int w, int h, bool dxt5, std::vector<uint8_t> &rgba) {
            const size_t blockBytes = dxt5 ? 16 : 8;
            const int bw = (w + 3) / 4, bh = (h + 3) / 4;
            if (srcLen < size_t(bw) * bh * blockBytes) {
                return false;
            }
            rgba.assign(size_t(w) * h * 4, 0);
            for (int by = 0; by < bh; ++by) {
                for (int bx = 0; bx < bw; ++bx) {
                    const uint8_t *b = src + (size_t(by) * bw + bx) * blockBytes;
                    uint8_t alpha[16];
                    std::fill(std::begin(alpha), std::end(alpha), uint8_t(255));
                    if (dxt5) {
                        const uint8_t a0 = b[0], a1 = b[1];
                        uint8_t table[8] = {a0, a1};
                        for (int i = 2; i < 8; ++i) {
                            table[i] = a0 > a1 ? uint8_t(((8 - i) * a0 + (i - 1) * a1) / 7)
                                               : (i < 6 ? uint8_t(((6 - i) * a0 + (i - 1) * a1) / 5) : (i == 6 ? 0 : 255));
                        }
                        uint64_t bits = 0;
                        for (int i = 0; i < 6; ++i) {
                            bits |= uint64_t(b[2 + i]) << (8 * i);
                        }
                        for (int i = 0; i < 16; ++i) {
                            alpha[i] = table[(bits >> (3 * i)) & 7];
                        }
                        b += 8;
                    }
                    const uint16_t c0 = uint16_t(b[0] | (b[1] << 8)), c1 = uint16_t(b[2] | (b[3] << 8));
                    uint8_t pal[4][4] = {};
                    Color565(c0, pal[0]);
                    Color565(c1, pal[1]);
                    pal[0][3] = pal[1][3] = 255;
                    for (int k = 0; k < 3; ++k) {
                        if (c0 > c1 || dxt5) {
                            pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
                            pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
                        }
                        else {
                            pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
                            pal[3][k] = 0;
                        }
                    }
                    pal[2][3] = 255;
                    pal[3][3] = (c0 > c1 || dxt5) ? 255 : 0;
                    const uint32_t idx = uint32_t(b[4]) | (uint32_t(b[5]) << 8) | (uint32_t(b[6]) << 16) | (uint32_t(b[7]) << 24);
                    for (int py = 0; py < 4; ++py) {
                        for (int px = 0; px < 4; ++px) {
                            const int x = bx * 4 + px, y = by * 4 + py;
                            if (x >= w || y >= h) {
                                continue;
                            }
                            const uint8_t *c = pal[(idx >> (2 * (py * 4 + px))) & 3];
                            uint8_t *o       = &rgba[(size_t(y) * w + x) * 4];
                            o[0] = c[0], o[1] = c[1], o[2] = c[2];
                            o[3] = dxt5 ? alpha[py * 4 + px] : c[3];
                        }
                    }
                }
            }
            return true;
        }

        // ---- fonts ----
        struct Glyph {
            uint32_t code = 0;
            int page = 0, x = 0, y = 0, w = 0, h = 0, top = 0, adjust = 0;
        };
        struct BitmapFont {
            std::string name;
            std::vector<Glyph> glyphs;
            std::unordered_map<uint32_t, int> index; // codepoint -> glyphs[]
            std::vector<std::vector<uint8_t>> pages; // 128 x 128 coverage
            float ascent = 0, descent = 0;
            ImFont *font = nullptr;
        };
        BitmapFont g_fonts[3];
        constexpr int kPage = 128;

        // Glyph table (rage::grcFont): header 0x30 bytes (+0x2C glyph count), 10-byte glyphs {u16 code, u16 0, u8 x, u8 y,
        // u8 w, u8 h, s8 top (from the baseline), s8 advance adjust}, then {u32 pages, u32 first glyph of the next page...}.
        bool ParseGlyphs(const std::vector<uint8_t> &d, BitmapFont &font) {
            if (d.size() < 0x30) {
                return false;
            }
            uint32_t count = 0;
            std::memcpy(&count, d.data() + 0x2C, 4);
            const size_t tail = 0x30 + size_t(count) * 10;
            if (count == 0 || count > 4096 || d.size() < tail + 4) {
                return false;
            }
            uint32_t pages = 0;
            std::memcpy(&pages, d.data() + tail, 4);
            if (pages == 0 || pages > 32 || d.size() < tail + 4 + pages * 4) {
                return false;
            }
            std::vector<uint32_t> ends(pages);
            std::memcpy(ends.data(), d.data() + tail + 4, pages * 4);
            uint32_t page = 0;
            for (uint32_t i = 0; i < count; ++i) {
                while (page + 1 < pages && i >= ends[page]) {
                    ++page;
                }
                const uint8_t *r = d.data() + 0x30 + size_t(i) * 10;
                Glyph g;
                g.code   = uint32_t(r[0] | (r[1] << 8));
                g.page   = int(page);
                g.x      = r[4];
                g.y      = r[5];
                g.w      = r[6];
                g.h      = r[7];
                g.top    = int8_t(r[8]);
                g.adjust = int8_t(r[9]);
                if (g.w == 0 || g.h == 0 || g.h > 64 || g.x + g.w > kPage || g.y + g.h > kPage || font.index.count(g.code)) {
                    continue; // padding records and duplicates
                }
                font.index[g.code] = int(font.glyphs.size());
                font.glyphs.push_back(g);
                // Line metrics from letters and digits only: a few symbols ({ } | and the like) reach far beyond them.
                if ((g.code >= 'A' && g.code <= 'Z') || (g.code >= 'a' && g.code <= 'z') || (g.code >= '0' && g.code <= '9')) {
                    font.ascent  = std::max(font.ascent, float(-g.top));
                    font.descent = std::max(font.descent, float(g.h + g.top));
                }
            }
            return !font.glyphs.empty() && font.ascent > 0;
        }

        // A 128 x 128 DXT5 page (DDS) -> coverage (its alpha).
        bool ParsePage(const std::vector<uint8_t> &d, std::vector<uint8_t> &coverage) {
            if (d.size() < 128 || std::memcmp(d.data(), "DDS ", 4) != 0 || std::memcmp(d.data() + 84, "DXT5", 4) != 0) {
                return false;
            }
            uint32_t h = 0, w = 0;
            std::memcpy(&h, d.data() + 12, 4);
            std::memcpy(&w, d.data() + 16, 4);
            std::vector<uint8_t> rgba;
            if (w != kPage || h != kPage || !DecodeDxt(d.data() + 128, d.size() - 128, kPage, kPage, true, rgba)) {
                return false;
            }
            coverage.resize(kPage * kPage);
            for (int i = 0; i < kPage * kPage; ++i) {
                coverage[i] = rgba[i * 4 + 3];
            }
            return true;
        }

        BitmapFont *FontOf(ImFontConfig *src) {
            return static_cast<BitmapFont *>(src->FontLoaderData);
        }

        bool LoaderFontSrcInit(ImFontAtlas *, ImFontConfig *) {
            return true;
        }
        int LoaderGlyphIndex(ImFontAtlas *, ImFontConfig *src, ImWchar codepoint) {
            const BitmapFont *f = FontOf(src);
            const auto it       = f->index.find(codepoint);
            return it == f->index.end() ? 0 : it->second + 1;
        }
        bool LoaderContains(ImFontAtlas *, ImFontConfig *src, int glyphIndex) {
            return glyphIndex > 0 && glyphIndex <= int(FontOf(src)->glyphs.size());
        }
        bool LoaderBakedInit(ImFontAtlas *, ImFontConfig *src, ImFontBaked *baked, void *) {
            const BitmapFont *f = FontOf(src);
            const float scale   = baked->Size / (f->ascent + f->descent);
            if (!src->MergeMode) {
                baked->Ascent  = std::ceil(f->ascent * scale);
                baked->Descent = -std::ceil(f->descent * scale);
            }
            return true;
        }
        void LoaderBakedDestroy(ImFontAtlas *, ImFontConfig *, ImFontBaked *, void *) {}

        // The glyph scaled from its page to the size asked for (area-weighted, so small sizes stay clean).
        bool LoaderLoadGlyph(ImFontAtlas *atlas, ImFontConfig *src, ImFontBaked *baked, void *, int glyphIndex, ImFontGlyph *out, float *outAdvance) {
            const BitmapFont *f = FontOf(src);
            if (glyphIndex <= 0 || glyphIndex > int(f->glyphs.size())) {
                return false;
            }
            const Glyph &g      = f->glyphs[glyphIndex - 1];
            const float scale   = baked->Size / (f->ascent + f->descent);
            const float density = baked->RasterizerDensity > 0 ? baked->RasterizerDensity : 1.0f;
            const float advance = std::max(1.0f, float(g.w + g.adjust)) * scale;
            if (outAdvance) {
                *outAdvance = advance;
            }
            if (!out) {
                return true;
            }
            out->AdvanceX  = advance;
            const int tw   = std::max(1, int(std::lround(g.w * scale * density)));
            const int th   = std::max(1, int(std::lround(g.h * scale * density)));
            const ImFontAtlasRectId pack = ImFontAtlasPackAddRect(atlas, tw, th);
            if (pack == ImFontAtlasRectId_Invalid) {
                return false;
            }
            ImTextureRect *r = ImFontAtlasPackGetRect(atlas, pack);
            std::vector<uint8_t> bitmap(size_t(tw) * th);
            const std::vector<uint8_t> &page = f->pages[g.page];
            const float sx = float(g.w) / tw, sy = float(g.h) / th;
            for (int y = 0; y < th; ++y) {
                const float y0 = y * sy, y1 = (y + 1) * sy;
                for (int x = 0; x < tw; ++x) {
                    const float x0 = x * sx, x1 = (x + 1) * sx;
                    float sum = 0, area = 0;
                    for (int yy = int(y0); yy < int(std::ceil(y1)); ++yy) {
                        const float wy = std::min(y1, float(yy + 1)) - std::max(y0, float(yy));
                        for (int xx = int(x0); xx < int(std::ceil(x1)); ++xx) {
                            const float wx = std::min(x1, float(xx + 1)) - std::max(x0, float(xx));
                            const int px = std::min(g.x + xx, kPage - 1), py = std::min(g.y + yy, kPage - 1);
                            sum += page[size_t(py) * kPage + px] * wx * wy;
                            area += wx * wy;
                        }
                    }
                    bitmap[size_t(y) * tw + x] = uint8_t(std::clamp(area > 0 ? sum / area : 0.0f, 0.0f, 255.0f));
                }
            }
            out->X0      = 0;
            out->Y0      = std::round((f->ascent + g.top) * scale);
            out->X1      = out->X0 + tw / density;
            out->Y1      = out->Y0 + th / density;
            out->Visible = true;
            out->PackId  = pack;
            ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, src, out, r, bitmap.data(), ImTextureFormat_Alpha8, tw);
            return true;
        }

        ImFontLoader MakeLoader() {
            ImFontLoader l;
            l.Name                              = "rdr-bitmap";
            l.FontSrcInit                       = &LoaderFontSrcInit;
            l.FontSrcGetGlyphIndexFromCodepoint = &LoaderGlyphIndex;
            l.FontSrcContainsGlyph              = &LoaderContains;
            l.FontBakedInit                     = &LoaderBakedInit;
            l.FontBakedDestroy                  = &LoaderBakedDestroy;
            l.FontBakedLoadGlyph                = &LoaderLoadGlyph;
            return l;
        }
        const ImFontLoader g_loader = MakeLoader();
        uint8_t g_dummyFontData     = 0;

        // ---- art ----
        struct ArtTexture {
            std::unique_ptr<ImTextureData> tex;
            int w = 0, h = 0;
        };
        ArtTexture g_art[5];

        // A texture record of the title movie: {+0 name ptr (0x5... virtual), +8 u16 w, +0xA u16 h, +0xC fourcc, +0x34 data
        // ptr (0x6... physical)}; the physical part is the last 0xA00000 bytes of the decoded resource.
        bool FindMovieTexture(const std::vector<uint8_t> &movie, const char *name, std::vector<uint8_t> &rgba, int &w, int &h) {
            constexpr size_t kPhysical = 0xA00000;
            if (movie.size() <= kPhysical) {
                return false;
            }
            const size_t phys = movie.size() - kPhysical;
            const std::string needle = std::string(name) + '\0';
            const auto at            = std::search(movie.begin(), movie.end(), needle.begin(), needle.end());
            if (at == movie.end()) {
                return false;
            }
            const uint32_t ptr = 0x50000000u + uint32_t(at - movie.begin());
            for (size_t i = 0; i + 0x38 <= phys; i += 4) {
                uint32_t v;
                std::memcpy(&v, movie.data() + i, 4);
                if (v != ptr) {
                    continue;
                }
                uint16_t tw, th;
                uint32_t data;
                std::memcpy(&tw, movie.data() + i + 8, 2);
                std::memcpy(&th, movie.data() + i + 0xA, 2);
                std::memcpy(&data, movie.data() + i + 0x34, 4);
                const bool dxt1 = std::memcmp(movie.data() + i + 0xC, "DXT1", 4) == 0;
                const bool dxt5 = std::memcmp(movie.data() + i + 0xC, "DXT5", 4) == 0;
                if ((!dxt1 && !dxt5) || (data >> 28) != 6 || tw == 0 || th == 0) {
                    continue;
                }
                const size_t off = phys + (data & 0x0FFFFFFF);
                if (off >= movie.size() || !DecodeDxt(movie.data() + off, movie.size() - off, tw, th, dxt5, rgba)) {
                    continue;
                }
                w = tw, h = th;
                return true;
            }
            return false;
        }

        // The title masks are white on black (no alpha): coverage = brightness, colour white, tinted when drawn.
        void MaskFromBrightness(std::vector<uint8_t> &rgba) {
            for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
                const uint8_t v = std::max({rgba[i], rgba[i + 1], rgba[i + 2]});
                rgba[i] = rgba[i + 1] = rgba[i + 2] = 255;
                rgba[i + 3]                         = v;
            }
        }

        void Register(Art art, std::vector<uint8_t> &rgba, int w, int h) {
            ArtTexture &a = g_art[int(art)];
            a.tex         = std::make_unique<ImTextureData>();
            a.tex->Create(ImTextureFormat_RGBA32, w, h);
            std::memcpy(a.tex->GetPixels(), rgba.data(), size_t(w) * h * 4);
            a.tex->UseColors = true;
            a.tex->SetStatus(ImTextureStatus_WantCreate);
            ImGui::RegisterUserTexture(a.tex.get());
            a.w = w, a.h = h;
        }

        std::filesystem::path GameDir() {
            wchar_t exe[MAX_PATH] {};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            return std::filesystem::path(exe).parent_path();
        }
    } // namespace

    bool Load(const rdmp::fw::Host *host) {
        if (g_tried) {
            return g_loaded;
        }
        g_tried = true;
        g_host  = host;
        if (!host || !host->inflate || !ImGui::GetCurrentContext()) {
            return false;
        }
        const std::filesystem::path game = GameDir() / "game";

        // Fonts: glyph table + its pages, in page order (fonts.rpf names are hashes; see re/NOTES.md).
        struct FontFiles {
            const char *name;
            uint32_t glyphs;
            std::vector<uint32_t> pages;
        };
        const FontFiles files[3] = {
            {"RDR Serif", 0x0411E993, {0x4EE9E283, 0x08AF780F, 0xC3220EEC, 0x501362D8, 0xEA6D86F7, 0xEF5113A7, 0x8CB5145E, 0x2EEC2FDC}},
            {"RDR Narrow", 0xBA922545, {0xC47FB29E, 0x1EBCC74D, 0xF7B13ADD, 0x7093E1D6, 0x84EDA8FA}},
            {"RDR Body", 0xEAE379F1, {0xD94CD6B0, 0xDD3CC5A2, 0x12513FF0, 0x31CAD755}},
        };
        Rpf fonts;
        int fontCount = 0;
        if (fonts.Open(game / "fonts.rpf")) {
            for (int i = 0; i < 3; ++i) {
                BitmapFont &f = g_fonts[i];
                f.name        = files[i].name;
                std::vector<uint8_t> data;
                bool ok = fonts.Payload(files[i].glyphs, data) && ParseGlyphs(data, f);
                for (uint32_t page : files[i].pages) {
                    std::vector<uint8_t> coverage;
                    ok = ok && fonts.Payload(page, data) && ParsePage(data, coverage);
                    f.pages.push_back(std::move(coverage));
                }
                for (const Glyph &g : f.glyphs) {
                    ok = ok && g.page < int(f.pages.size());
                }
                if (!ok) {
                    Log()->warn("menu: the game font {} could not be read", f.name);
                    continue;
                }
                ImFontConfig cfg;
                cfg.FontData             = &g_dummyFontData; // the loader reads its own pages
                cfg.FontDataSize         = 1;
                cfg.FontDataOwnedByAtlas = false;
                cfg.FontLoader           = &g_loader;
                cfg.FontLoaderData       = &f;
                cfg.SizePixels           = f.ascent + f.descent;
                std::snprintf(cfg.Name, sizeof(cfg.Name), "%s", f.name.c_str());
                f.font = ImGui::GetIO().Fonts->AddFont(&cfg);
                // Characters the game's fonts lack (other scripts) come from Windows' Bahnschrift.
                wchar_t windows[MAX_PATH] {};
                GetWindowsDirectoryW(windows, MAX_PATH);
                const std::filesystem::path fallback = std::filesystem::path(windows) / "Fonts" / "bahnschrift.ttf";
                if (f.font && std::filesystem::exists(fallback)) {
                    ImFontConfig merge;
                    merge.MergeMode = true;
                    merge.DstFont   = f.font;
                    ImGui::GetIO().Fonts->AddFontFromFileTTF(fallback.string().c_str(), 0.0f, &merge);
                }
                fontCount += f.font ? 1 : 0;
            }
        }

        // Title art.
        Rpf flash;
        std::vector<uint8_t> movie;
        int artCount = 0;
        if (flash.Open(game / "flash.rpf") && flash.Payload(0x4D8DB2A4, movie)) {
            const struct {
                Art art;
                const char *name;
            } wanted[] = {
                {Art::Red, "textures/_brs_startscreen_z_0111.dds"},
                {Art::Dead, "textures/_brs_startscreen_z_0110.dds"},
                {Art::Stroke, "textures/_brs_startscreen_z_0023.dds"},
                {Art::StrokeThin, "textures/_brs_startscreen_z_0020.dds"},
                {Art::Splatter, "textures/_brs_startscreen_z_0069.dds"},
            };
            for (const auto &item : wanted) {
                std::vector<uint8_t> rgba;
                int w = 0, h = 0;
                if (FindMovieTexture(movie, item.name, rgba, w, h)) {
                    MaskFromBrightness(rgba);
                    Register(item.art, rgba, w, h);
                    ++artCount;
                }
            }
        }
        g_loaded = fontCount == 3;
        Log()->info("menu: game fonts {}/3, title art {}/5 read from {}", fontCount, artCount, game.string());
        return g_loaded;
    }

    ImFont *Font(Face face) {
        return g_fonts[int(face)].font;
    }

    Texture Get(Art art) {
        const ArtTexture &a = g_art[int(art)];
        Texture t;
        if (a.tex) {
            t.ref    = a.tex->GetTexRef();
            t.width  = float(a.w);
            t.height = float(a.h);
            t.valid  = true;
        }
        return t;
    }
} // namespace RDMP::RdrAssets
