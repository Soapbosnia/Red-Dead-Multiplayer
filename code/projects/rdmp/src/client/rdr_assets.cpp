/*
 * The game's own title art for the menu: see rdr_assets.h. Formats in re/NOTES.md "Game fonts and title art".
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
        g_loaded = artCount > 0;
        Log()->info("menu: title art {}/5 read from {}", artCount, game.string());
        return g_loaded;
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
