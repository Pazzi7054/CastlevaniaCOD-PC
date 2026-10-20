// HD texture packs made for PCSX2, drawn in place of the game's textures.
//
// The Xbox release kept the PS2's textures: the same palette indices and colours. A PCSX2 pack names each picture
// by PCSX2's hashes of the PS2 texture, so each P8 texture the game uploads is hashed the way PCSX2 hashes it in GS
// memory, and a picture with that name replaces it:
//
//   <texture hash>-<palette hash>-<TEX0 bits>.png
//
//   texture hash   XXH3-64 of the texture's 256 byte GS blocks, row by row, each block in the GS's own layout of
//                  8 bit (16x16 texels) or 4 bit (32x16) indices; a texture smaller than a block, of its indices
//                  row by row instead
//   palette hash   XXH3-64 of its 256 (8 bit) or 16 (4 bit) colours as the GS holds them: R G B A bytes, alpha
//                  0..128 where the Xbox's is 0..255
//
// A 4 bit texture is 8 bit on the Xbox, its indices under 16. Pictures load in the background, scaled to the size
// set, their PS2 alpha doubled, with every mip level; the game's own texture is drawn until they are ready. They go
// in the textures folder beside the game (any folders inside it), which is looked for like the assets folder.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "app/app.h"
#include "core/log.h"
#include "gpu/ext.h"
#include "gpu/texture.h"

bool g_texture_pack = true;         // settings file: texture_pack
uint32_t g_texture_pack_size = 2048;  // texture_pack_size: the largest side a picture is loaded at, 0 as it is

namespace {

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

// where a texel of a 16x16 8 bit block is in the block's bytes, and of a 32x16 4 bit block in its nibbles: four
// columns of four rows, each column's two halves of rows swapped on odd columns
uint32_t Block8(uint32_t x, uint32_t y) {
    uint32_t c = y >> 2, r = y & 3;
    return c * 64 + ((((x >> 2) & 1) ^ (r >> 1) ^ (c & 1)) << 5) + (((x >> 1) & 1) << 4) + ((r & 1) << 3) +
           ((x & 1) << 2) + (((x >> 3) & 1) << 1) + (r >> 1);
}
uint32_t Block4(uint32_t x, uint32_t y) {
    uint32_t c = y >> 2, r = y & 3;
    return c * 128 + ((((x >> 2) & 1) ^ (r >> 1) ^ (c & 1)) << 6) + (((x >> 1) & 1) << 5) + ((r & 1) << 4) +
           ((x & 1) << 3) + (((x >> 3) & 3) << 1) + (r >> 1);
}

uint64_t TextureHash(const std::vector<uint8_t>& idx, uint32_t w, uint32_t h, bool four) {
    uint32_t bw = four ? 32 : 16, bh = 16;
    if (w < bw || h < bh || w % bw || h % bh) return XXH3_64bits(idx.data(), idx.size());
    XXH3_state_t st;
    XXH3_64bits_reset(&st);
    uint8_t block[256];
    for (uint32_t by = 0; by < h; by += bh)
        for (uint32_t bx = 0; bx < w; bx += bw) {
            std::fill(std::begin(block), std::end(block), uint8_t(0));
            for (uint32_t y = 0; y < bh; y++) {
                const uint8_t* row = &idx[size_t(by + y) * w + bx];
                for (uint32_t x = 0; x < bw; x++) {
                    if (four) {
                        uint32_t n = Block4(x, y);
                        block[n >> 1] |= uint8_t((row[x] & 15) << ((n & 1) * 4));
                    } else {
                        block[Block8(x, y)] = row[x];
                    }
                }
            }
            XXH3_64bits_update(&st, block, sizeof(block));
        }
    return XXH3_64bits_digest(&st);
}

uint64_t PaletteHash(const uint32_t* palette, uint32_t n) {
    uint32_t gs[256];
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = palette[i];  // A R G B
        uint32_t a = ((c >> 24) + 1) >> 1;
        gs[i] = a << 24 | (c & 0xFF) << 16 | (c & 0xFF00) | ((c >> 16) & 0xFF);
    }
    return XXH3_64bits(gs, n * sizeof(uint32_t));
}

struct Key {
    uint64_t texture, palette;
    bool operator<(const Key& o) const { return texture != o.texture ? texture < o.texture : palette < o.palette; }
};

class Pack {
public:
    void Init() {
        if (!g_texture_pack) return;
        dir_ = Find();
        if (dir_.empty()) return;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(dir_, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec) || it->path().extension() != ".png") continue;
            // texture-palette-bits, no region
            std::string name = it->path().stem().string();
            unsigned long long t = 0, p = 0;
            unsigned bits = 0;
            char tail = 0;
            if (std::sscanf(name.c_str(), "%llx-%llx-%8x%c", &t, &p, &bits, &tail) != 3) continue;
            uint32_t psm = bits & 0x3F;
            if (psm != 0x13 && psm != 0x14) continue;  // PSMT8, PSMT4
            if (index_.emplace(Key{t, p}, uint32_t(files_.size())).second) files_.push_back(it->path());
        }
        LOGR("texpack: %zu pictures in %s, up to %u px", files_.size(), dir_.string().c_str(), g_texture_pack_size);
        if (files_.empty()) return;
        state_.resize(files_.size());
        unsigned n = std::clamp(std::thread::hardware_concurrency() / 2, 2u, 6u);
        for (unsigned i = 0; i < n; i++) std::thread([this] { Work(); }).detach();
    }

    uint64_t Replace(const gpu::TextureDesc& d, const uint8_t* guest, const uint32_t* palette) {
        if (files_.empty() || !palette) return 0;
        std::vector<uint8_t> idx;
        if (!gpu::PaletteIndices(d, guest, idx)) return 0;
        auto it = index_.find(Key{TextureHash(idx, d.width, d.height, false), PaletteHash(palette, 256)});
        if (it == index_.end() && *std::max_element(idx.begin(), idx.end()) < 16)
            it = index_.find(Key{TextureHash(idx, d.width, d.height, true), PaletteHash(palette, 16)});
        if (it == index_.end()) return 0;
        uint32_t i = it->second;
        std::lock_guard lock(m_);
        if (state_[i].stage == Stage::Idle) {
            state_[i].stage = Stage::Queued;
            queue_.push_back(i);
            cv_.notify_one();
        }
        return uint64_t(i) + 1;
    }

    std::shared_ptr<const gpu::Replacement> Replaced(uint64_t id) {
        std::lock_guard lock(m_);
        Entry& e = state_[size_t(id - 1)];
        if (e.stage == Stage::Failed) return std::make_shared<gpu::Replacement>();  // empty: drawn as it was
        if (e.stage != Stage::Ready) return nullptr;
        e.stage = Stage::Idle;  // the renderer keeps it now; loaded again should it drop it
        LOG_FIRST(40, "texpack: %s at %ux%u", files_[size_t(id - 1)].filename().string().c_str(), e.pixels->width, e.pixels->height);
        return std::move(e.pixels);
    }

private:
    enum class Stage { Idle, Queued, Ready, Failed };
    struct Entry {
        Stage stage = Stage::Idle;
        std::shared_ptr<gpu::Replacement> pixels;
    };

    static fs::path Find() {
        fs::path dir = app::ExeDir();
        std::error_code ec;
        for (int i = 0; i < 6; i++) {
            if (fs::is_directory(dir / "textures", ec)) return dir / "textures";
            if (!dir.has_parent_path() || dir.parent_path() == dir) break;
            dir = dir.parent_path();
        }
        return {};
    }

    void Work() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<IWICImagingFactory> wic;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        for (;;) {
            uint32_t i;
            {
                std::unique_lock lock(m_);
                cv_.wait(lock, [this] { return !queue_.empty(); });
                i = queue_.front();
                queue_.pop_front();
            }
            std::shared_ptr<gpu::Replacement> r = wic ? Load(wic.Get(), files_[i]) : nullptr;
            if (!r) LOGW("texpack: %s does not load", files_[i].filename().string().c_str());
            std::lock_guard lock(m_);
            state_[i].pixels = std::move(r);
            state_[i].stage = state_[i].pixels ? Stage::Ready : Stage::Failed;
        }
    }

    // BGRA, scaled by halves to the size set, PS2 alpha (128 opaque) to the Xbox's, then every level below
    static std::shared_ptr<gpu::Replacement> Load(IWICImagingFactory* wic, const fs::path& file) {
        ComPtr<IWICBitmapDecoder> dec;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> conv;
        if (FAILED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
            FAILED(dec->GetFrame(0, &frame)) || FAILED(wic->CreateFormatConverter(&conv)) ||
            FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                    WICBitmapPaletteTypeCustom)))
            return nullptr;
        UINT w = 0, h = 0;
        conv->GetSize(&w, &h);
        if (!w || !h) return nullptr;
        UINT cap = g_texture_pack_size ? g_texture_pack_size : 16384;
        UINT tw = w, th = h;
        while (std::max(tw, th) > cap && tw > 1 && th > 1) {
            tw /= 2;
            th /= 2;
        }
        ComPtr<IWICBitmapSource> src = conv;
        if (tw != w || th != h) {
            ComPtr<IWICBitmapScaler> sc;
            if (FAILED(wic->CreateBitmapScaler(&sc)) || FAILED(sc->Initialize(conv.Get(), tw, th, WICBitmapInterpolationModeFant)))
                return nullptr;
            src = sc;
        }
        auto r = std::make_shared<gpu::Replacement>();
        r->width = tw;
        r->height = th;
        std::vector<uint8_t> px(size_t(tw) * th * 4);
        if (FAILED(src->CopyPixels(nullptr, tw * 4, UINT(px.size()), px.data()))) return nullptr;
        for (size_t i = 3; i < px.size(); i += 4) px[i] = uint8_t(std::min(255u, (px[i] * 255u + 64) / 128));
        r->levels.push_back(std::move(px));
        for (uint32_t lw = tw, lh = th; lw > 1 || lh > 1;) {
            uint32_t nw = std::max(lw / 2, 1u), nh = std::max(lh / 2, 1u);
            const std::vector<uint8_t>& a = r->levels.back();
            std::vector<uint8_t> b(size_t(nw) * nh * 4);
            for (uint32_t y = 0; y < nh; y++)
                for (uint32_t x = 0; x < nw; x++)
                    for (uint32_t c = 0; c < 4; c++) {
                        uint32_t x0 = std::min(x * 2, lw - 1), x1 = std::min(x * 2 + 1, lw - 1);
                        uint32_t y0 = std::min(y * 2, lh - 1), y1 = std::min(y * 2 + 1, lh - 1);
                        uint32_t s = a[(size_t(y0) * lw + x0) * 4 + c] + a[(size_t(y0) * lw + x1) * 4 + c] +
                                     a[(size_t(y1) * lw + x0) * 4 + c] + a[(size_t(y1) * lw + x1) * 4 + c];
                        b[(size_t(y) * nw + x) * 4 + c] = uint8_t((s + 2) / 4);
                    }
            r->levels.push_back(std::move(b));
            lw = nw;
            lh = nh;
        }
        return r;
    }

    fs::path dir_;
    std::map<Key, uint32_t> index_;
    std::vector<fs::path> files_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<uint32_t> queue_;
    std::vector<Entry> state_;
};

Pack g_pack;

}  // namespace

void TexturePackInit() { g_pack.Init(); }
uint64_t TexturePackReplace(const gpu::TextureDesc& d, const uint8_t* guest, const uint32_t* palette) {
    return g_pack.Replace(d, guest, palette);
}
std::shared_ptr<const gpu::Replacement> TexturePackReplaced(uint64_t id) { return g_pack.Replaced(id); }
