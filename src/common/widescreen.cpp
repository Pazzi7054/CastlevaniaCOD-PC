// Widescreen, and the frame pass's overscan, through a renderer extension.
//
// Widescreen: on a 16:9 output the console frame is shown 16:9, and every draw into the back buffer is squeezed to
// 3/4 of its width around the centre, so nothing is stretched. The game projects with a PS2 style view screen matrix
// (c68..c71, or c85..c88 with the world matrix in it) into PS2 screen space, which c94 and c95 then scale to console
// pixels. Squeezing a projected draw is the same as narrowing the camera's focal length, so the 3D view shows more
// at the sides (Hor+). 2D draws (HUD, menus, text, backdrops, movies) keep their 4:3 shape in the middle. Draws over
// the whole width that are no picture of their own stay whole: fades and tints, and passes that draw the frame
// itself back (which already holds the squeezed scene).
//
// Overscan: the last pass of each frame draws a copy of the frame back over it with rows -16 to 496 of the copy
// across rows 0 to 480, the PS2's 448 lines and their border shrunk into the NTSC frame. A TV's overscan hid the 16
// rows at the top and bottom, which read past the copy and smear its first and last rows into streaks. The pass is
// drawn that much taller instead, so the copy fills the frame row for row, as square pixels as the scene was drawn.
#include <cmath>
#include <memory>
#include <unordered_map>

#include "app/app.h"
#include "gpu/ext.h"

uint64_t TexturePackReplace(const gpu::TextureDesc& d, const uint8_t* guest, const uint32_t* palette);  // texpack.cpp
std::shared_ptr<const gpu::Replacement> TexturePackReplaced(uint64_t id);                              // texpack.cpp

namespace {

constexpr float kSqueeze = (4.0f / 3.0f) / (16.0f / 9.0f);

// hardware constant order: D3D register + 96
constexpr uint32_t kCamera = 68 + 96;       // view screen matrix
constexpr uint32_t kCameraWorld = 85 + 96;  // world view screen matrix
constexpr uint32_t kScreenScale = 94 + 96;  // PS2 screen to console pixels
constexpr uint32_t kScreenOffset = 95 + 96;
constexpr int kFrameTexCoord = 2;  // the frame pass's texture coordinates, in texels

struct Program {
    bool camera = false;  // projects with the game's camera
    bool screen = false;  // takes PS2 screen coordinates: x * c94.x / w + c95.x
};

struct Span {
    float lo = 1e30f, hi = -1e30f;
    void Add(float v) {
        lo = std::fmin(lo, v);
        hi = std::fmax(hi, v);
    }
    bool Empty() const { return lo > hi; }
};

class Hooks : public gpu::ShaderHooks {
public:
    std::string Position() const override { return "    o.pos.xy *= ext.xy;\n"; }
};

class Widescreen : public gpu::Extension {
public:
    const gpu::ShaderHooks* Shaders() const override { return &hooks_; }

    void BeginDraw(const gpu::State&) override {
        squeeze_ = whole_ = textured_ = target_ = false;
        stretch_ = 1;
    }

    void Vertices(const gpu::State& s, const gpu::Assembled& a) override {
        squeeze_ = true;
        bool prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
        // fixed function transform: a camera of its own
        if (!prog && !a.pretransformed) return;
        Program p = prog ? Classify(*s.vs) : Program{};
        if (p.camera) return;
        textured_ = s.textures[0] != 0;
        // x on the console frame
        Span x = Read(a, 0, [&](const float* v, uint32_t comps) {
            if (!p.screen) return v[0];
            uint32_t decode = a.decode & 0xF;
            float w = comps == 4 && decode != gpu::kDecodeW1 ? v[3] : decode == gpu::kDecodeFloat2H ? v[2] : 1.0f;
            return v[0] * s.vsc[kScreenScale][0] * (w != 0 ? 1 / w : 1) + s.vsc[kScreenOffset][0];
        });
        float gw = float(s.viewport.w ? s.viewport.w : 640);
        whole_ = !x.Empty() && x.lo <= 1 && x.hi >= gw - 1;
        // the frame pass: screen positions, its copy's rows read over a taller span than it covers
        if (prog && !p.screen && whole_ && a.vertex_count == 4) {
            Span y = Read(a, 0, [](const float* v, uint32_t) { return v[1]; });
            Span t = Read(a, kFrameTexCoord, [](const float* v, uint32_t) { return v[1]; });
            if (!t.Empty() && y.hi > y.lo && t.lo < y.lo - 0.5f && t.hi > y.hi + 0.5f)
                stretch_ = (t.hi - t.lo) / (y.hi - y.lo);
        }
    }

    bool Bound(gpu::Context& c, const gpu::State&, gpu::Target* rt, gpu::Target*, bool) override {
        uint32_t w = 0, h = 0;
        c.GuestSize(w, h);
        back_buffer_ = rt && rt->gw == w && rt->gh == h;
        return false;
    }

    void Sampled(const gpu::TextureDesc&, const gpu::Target* t, uint8_t, gpu::TexInfo&) override { target_ |= t != nullptr; }

    // HD texture packs (texpack.cpp)
    uint64_t Replace(const gpu::TextureDesc& d, const uint8_t* guest, const uint32_t* palette) override {
        return TexturePackReplace(d, guest, palette);
    }
    std::shared_ptr<const gpu::Replacement> Replaced(uint64_t id) override { return TexturePackReplaced(id); }

    void Constants(float (*)[4], float* ext, const gpu::Target*, uint32_t) override {
        bool squeeze = squeeze_ && back_buffer_ && app::Widescreen();
        // whole width: an untextured fade or tint, or the frame drawn back over itself
        if (whole_ && (!textured_ || target_)) squeeze = false;
        ext[0] = squeeze ? kSqueeze : 1.0f;
        ext[1] = back_buffer_ && target_ ? stretch_ : 1.0f;
    }

private:
    Program Classify(const gpu::VertexShader& vs) {
        uint64_t key = gpu::Hash(vs.program.data(), vs.program.size() * 4);
        auto it = programs_.find(key);
        if (it != programs_.end()) return it->second;
        Program p;
        for (uint32_t r = 0; r < 4; r++)
            p.camera |= gpu::ReadsConstant(vs.program, kCamera + r) || gpu::ReadsConstant(vs.program, kCameraWorld + r);
        p.screen = gpu::ReadsConstant(vs.program, kScreenScale) && gpu::ReadsConstant(vs.program, kScreenOffset);
        programs_.emplace(key, p);
        return p;
    }

    // f(element, components) over the draw's vertices of input register reg, when it is 2 to 4 floats
    template <typename F>
    static Span Read(const gpu::Assembled& a, int reg, F f) {
        Span span;
        const gpu::VertexElement& e = a.elements[size_t(reg)];
        if (!(a.regs >> reg & 1) || e.buffer >= a.buffer_count) return span;
        const gpu::VertexBuffer& b = a.buffers[e.buffer];
        uint32_t comps = e.format == DXGI_FORMAT_R32G32B32A32_FLOAT ? 4
                         : e.format == DXGI_FORMAT_R32G32B32_FLOAT  ? 3
                         : e.format == DXGI_FORMAT_R32G32_FLOAT     ? 2
                                                                    : 0;
        if (!comps || !b.src) return span;
        for (uint32_t v = 0; v < a.vertex_count; v++) {
            uint64_t at = uint64_t(v) * b.stride + e.offset;
            if (at + comps * 4 > b.bytes) break;
            span.Add(f(reinterpret_cast<const float*>(b.src + at), comps));
        }
        return span;
    }

    Hooks hooks_;
    std::unordered_map<uint64_t, Program> programs_;
    bool squeeze_ = false, whole_ = false, textured_ = false, target_ = false, back_buffer_ = false;
    float stretch_ = 1;
};

}  // namespace

// GameInfo::init: before the renderer is made
void WidescreenInit() { gpu::SetExtension(std::make_unique<Widescreen>()); }
