// The launcher: checks the game files, edits the settings file the game reads, and starts the game.
//
// It sits beside Castlevania.exe and writes the same castlevania.toml, keeping any keys it does not know. The game
// files are checked as the game checks them: assets beside the exe or in a folder above it, default.xbe there must
// have the SHA-1 the exe was recompiled from; a disc image (.iso) is checked by the game when it starts.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <commdlg.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#include <toml++/toml.hpp>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
extern const char kImageSha1[];  // the xbe the game exe was recompiled from, written by CMake

namespace {

namespace fs = std::filesystem;

constexpr wchar_t kGameExe[] = L"Castlevania.exe";
constexpr char kSettings[] = "castlevania.toml";
constexpr char kXbe[] = "default.xbe";
constexpr wchar_t kSaves[] = L"hdd\\E\\UDATA\\4b4e002d";

// D3D11 and the window

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
HWND g_hwnd = nullptr;

void MakeTarget() {
    ID3D11Texture2D* bb = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
    bb->Release();
}

bool CreateDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL got;
    const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 2, D3D11_SDK_VERSION,
                                               &sd, &g_swap, &g_dev, &got, &g_ctx);
    // no GPU driver: the software rasterizer still draws a launcher
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, want, 2, D3D11_SDK_VERSION, &sd,
                                           &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) return false;
    MakeTarget();
    return true;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    switch (msg) {
        case WM_SIZE:
            if (g_dev && wp != SIZE_MINIMIZED) {
                if (g_rtv) g_rtv->Release(), g_rtv = nullptr;
                g_swap->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
                MakeTarget();
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_KEYMENU) return 0;  // Alt alone
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// files

fs::path ExeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return fs::path(p).parent_path();
}

// the game's own search: assets beside the exe or in any folder above it
fs::path FindAssets() {
    fs::path dir = ExeDir();
    std::error_code ec;
    for (int i = 0; i < 6; i++) {
        if (fs::is_directory(dir / "assets", ec)) return dir / "assets";
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    return {};
}

std::string Sha1(const fs::path& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return {};
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
    uint8_t digest[20];
    if (BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, data.data(), ULONG(data.size()), digest, 20) != 0) return {};
    std::string hex;
    for (uint8_t b : digest) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}

std::string Utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(std::max(n, 1)) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

void Open(const fs::path& p) { ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

// what the game will find
struct Files {
    fs::path assets;
    enum class Xbe { Missing, Match, Other } xbe = Xbe::Missing;
    std::vector<fs::path> isos;
    bool exe = false;

    void Scan() {
        assets = FindAssets();
        xbe = Xbe::Missing;
        isos.clear();
        exe = fs::exists(ExeDir() / kGameExe);
        if (assets.empty()) return;
        std::error_code ec;
        if (fs::exists(assets / kXbe, ec)) xbe = Sha1(assets / kXbe) == kImageSha1 ? Xbe::Match : Xbe::Other;
        for (auto& e : fs::directory_iterator(assets, ec)) {
            std::wstring ext = e.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), towlower);
            if (ext == L".iso" && e.is_regular_file(ec)) isos.push_back(e.path());
        }
    }
    bool Playable() const { return exe && (xbe == Xbe::Match || !isos.empty()); }
};

// a disc image into assets: a hard link on the same drive, a copy with progress otherwise
struct Import {
    std::atomic<bool> running{false};
    std::atomic<float> progress{0};
    std::string result;
    std::thread worker;

    static DWORD CALLBACK Progress(LARGE_INTEGER total, LARGE_INTEGER done, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD,
                                  HANDLE, HANDLE, LPVOID self) {
        static_cast<Import*>(self)->progress = total.QuadPart ? float(double(done.QuadPart) / double(total.QuadPart)) : 0;
        return PROGRESS_CONTINUE;
    }

    void Start(fs::path from, fs::path to) {
        if (running) return;
        if (worker.joinable()) worker.join();
        running = true;
        progress = 0;
        result.clear();
        worker = std::thread([this, from, to] {
            std::error_code ec;
            fs::create_directories(to.parent_path(), ec);
            if (CreateHardLinkW(to.c_str(), from.c_str(), nullptr)) {
                result = "Linked " + Utf8(to.filename().wstring()) + " into assets.";
            } else if (CopyFileExW(from.c_str(), to.c_str(), Progress, this, nullptr, COPY_FILE_FAIL_IF_EXISTS)) {
                result = "Copied " + Utf8(to.filename().wstring()) + " into assets.";
            } else {
                result = "Could not copy the disc image (error " + std::to_string(GetLastError()) + ").";
            }
            running = false;
        });
    }
    ~Import() {
        if (worker.joinable()) worker.join();
    }
};

// settings: the keys the game reads, the rest of the file kept

struct Settings {
    std::string display_mode = "borderless";
    uint32_t width = 0, height = 0;
    uint32_t render_percent = 100;
    uint32_t aniso = 0;
    bool vsync = true;
    bool widescreen = true;
    bool fps_counter = false;
    bool pause_on_focus_loss = true;
    uint32_t menu_key = VK_F10;
    bool keep_open = false;  // the launcher's own
    toml::table rest;

    fs::path File() const { return ExeDir() / kSettings; }

    void Load() {
        try {
            rest = toml::parse_file(File().string());
        } catch (...) {
            rest = {};
        }
        display_mode = rest["display_mode"].value_or(display_mode);
        width = rest["width"].value_or(width);
        height = rest["height"].value_or(height);
        render_percent = rest["render_percent"].value_or(render_percent);
        aniso = rest["anisotropic_filtering"].value_or(aniso);
        vsync = rest["vsync"].value_or(vsync);
        widescreen = rest["widescreen"].value_or(widescreen);
        fps_counter = rest["fps_counter"].value_or(fps_counter);
        pause_on_focus_loss = rest["pause_on_focus_loss"].value_or(pause_on_focus_loss);
        menu_key = rest["menu_key"].value_or(menu_key);
        keep_open = rest["launcher_keep_open"].value_or(keep_open);
    }

    bool Save() {
        rest.insert_or_assign("display_mode", display_mode);
        rest.insert_or_assign("width", int64_t(width));
        rest.insert_or_assign("height", int64_t(height));
        rest.insert_or_assign("render_percent", int64_t(render_percent));
        rest.insert_or_assign("anisotropic_filtering", int64_t(aniso));
        rest.insert_or_assign("vsync", vsync);
        rest.insert_or_assign("widescreen", widescreen);
        rest.insert_or_assign("fps_counter", fps_counter);
        rest.insert_or_assign("pause_on_focus_loss", pause_on_focus_loss);
        rest.insert_or_assign("menu_key", int64_t(menu_key));
        rest.insert_or_assign("launcher_keep_open", keep_open);
        std::ofstream f(File());
        f << rest << "\n";
        return bool(f);
    }

    void Defaults() {
        Settings d;
        d.rest = std::move(rest);
        d.keep_open = keep_open;
        *this = std::move(d);
    }
};

// UI pieces

struct Res {
    uint32_t w, h;
};

std::vector<Res> Resolutions() {
    std::vector<Res> list{{0, 0}};
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); i++) {
        if (dm.dmPelsWidth < 640 || dm.dmPelsHeight < 480) continue;
        bool seen = false;
        for (const Res& r : list) seen |= r.w == dm.dmPelsWidth && r.h == dm.dmPelsHeight;
        if (!seen) list.push_back({dm.dmPelsWidth, dm.dmPelsHeight});
    }
    std::sort(list.begin() + 1, list.end(), [](Res a, Res b) { return a.w != b.w ? a.w > b.w : a.h > b.h; });
    return list;
}

std::string ResName(Res r) {
    if (!r.w) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        EnumDisplaySettingsW(nullptr, ENUM_REGISTRY_SETTINGS, &dm);
        return "Desktop (" + std::to_string(dm.dmPelsWidth) + "x" + std::to_string(dm.dmPelsHeight) + ")";
    }
    return std::to_string(r.w) + "x" + std::to_string(r.h);
}

std::string KeyName(uint32_t vk) {
    wchar_t name[64] = {};
    LONG scan = LONG(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)) << 16;
    if ((vk >= VK_PRIOR && vk <= VK_DOWN) || vk == VK_INSERT || vk == VK_DELETE) scan |= 1 << 24;
    if (!GetKeyNameTextW(scan, name, 64)) return "Key " + std::to_string(vk);
    return Utf8(name);
}

template <typename T>
void Combo(const char* id, T& value, const std::vector<T>& values, const std::vector<std::string>& names) {
    size_t cur = 0;
    for (size_t i = 0; i < values.size(); i++)
        if (values[i] == value) cur = i;
    if (ImGui::BeginCombo(id, names[cur].c_str())) {
        for (size_t i = 0; i < values.size(); i++) {
            if (ImGui::Selectable(names[i].c_str(), i == cur)) value = values[i];
            if (i == cur) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

void Row(const char* name, const char* help) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(name);
    if (help) {
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24);
            ImGui::TextUnformatted(help);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
}

bool BeginRows(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) return false;
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.45f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.55f);
    return true;
}

// the look: the PS2 games' menus. Navy panels in pale blue and gold rules, orange to crimson brush banners over the
// sections, bronze bars for buttons, Palatino, and a castle under the moon in the mist. All drawn here, no game art.

float g_scale = 1;
ImFont* g_body = nullptr;    // Palatino
ImFont* g_strong = nullptr;  // Palatino bold, buttons
ImFont* g_banner = nullptr;  // Georgia bold italic, section banners
ImFont* g_title = nullptr;   // Palatino bold, the logo
ImFont* g_sub = nullptr;     // Palatino bold italic, the logo's second line
ImFont* g_play = nullptr;

float S(float v) { return v * g_scale; }

ImU32 Rgb(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
const ImU32 kNavyTop = Rgb(22, 36, 79, 236), kNavyBottom = Rgb(9, 16, 44, 236);
const ImU32 kSteel = Rgb(143, 182, 216), kSteelDim = Rgb(70, 98, 140);
const ImU32 kGoldHi = Rgb(250, 226, 150), kGoldC = Rgb(201, 162, 74), kGoldLo = Rgb(122, 86, 30);
const ImU32 kCrimson = Rgb(179, 18, 42), kOrange = Rgb(240, 134, 30), kBlood = Rgb(70, 6, 14);
const ImU32 kBronzeTop = Rgb(122, 74, 34), kBronzeBottom = Rgb(52, 28, 12);
const ImU32 kParchment = Rgb(232, 220, 192);
const ImVec4 kGold{0.98f, 0.86f, 0.55f, 1};
const ImVec4 kGood{0.56f, 0.82f, 0.70f, 1};
const ImVec4 kBad{0.90f, 0.32f, 0.26f, 1};
const ImVec4 kDim{0.62f, 0.60f, 0.56f, 1};

ImVec2 operator+(ImVec2 a, ImVec2 b) { return {a.x + b.x, a.y + b.y}; }
ImVec2 operator-(ImVec2 a, ImVec2 b) { return {a.x - b.x, a.y - b.y}; }

// text with a shadow, coloured top to bottom
void ShadedText(ImDrawList* dl, ImFont* f, ImVec2 p, const char* s, ImU32 top, ImU32 bottom, float shadow = 2) {
    float size = f->FontSize;
    dl->AddText(f, size, p + ImVec2(S(shadow), S(shadow) * 1.3f), Rgb(0, 0, 0, 210), s);
    int v0 = dl->VtxBuffer.Size;
    dl->AddText(f, size, p, IM_COL32_WHITE, s);
    ImVec2 sz = f->CalcTextSizeA(size, FLT_MAX, 0, s);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, p, p + ImVec2(0, sz.y), top, bottom);
}

void Diamond(ImDrawList* dl, ImVec2 c, float r, ImU32 fill, ImU32 edge) {
    ImVec2 pts[4] = {{c.x, c.y - r}, {c.x + r, c.y}, {c.x, c.y + r}, {c.x - r, c.y}};
    dl->AddConvexPolyFilled(pts, 4, fill);
    dl->AddPolyline(pts, 4, edge, ImDrawFlags_Closed, S(1));
}

// pale blue rule inside a gold one, a gem on each corner
void Frame(ImDrawList* dl, ImVec2 a, ImVec2 b, bool gems = true) {
    dl->AddRect(a, b, kGoldLo, 0, 0, S(3));
    dl->AddRect(a, b, kGoldC, 0, 0, S(1));
    float i = S(4);
    dl->AddRect(a + ImVec2(i, i), b - ImVec2(i, i), kSteelDim, 0, 0, S(1));
    if (!gems) return;
    for (ImVec2 c : {a, ImVec2(b.x, a.y), b, ImVec2(a.x, b.y)}) Diamond(dl, c, S(5), kCrimson, kGoldHi);
}

// a panel: navy fill and frame behind the child's contents
bool BeginPanel(const char* id, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {S(18), S(16)});
    bool open = ImGui::BeginChild(id, size, ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetWindowPos(), b = a + ImGui::GetWindowSize();
    dl->PushClipRectFullScreen();
    dl->AddRectFilledMultiColor(a, b, kNavyTop, kNavyTop, kNavyBottom, kNavyBottom);
    Frame(dl, a, b);
    dl->PopClipRect();
    return open;
}

// the brush banner the games put over a menu: orange into crimson, trailing off to the right
void Banner(const char* text) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 ts = g_banner->CalcTextSizeA(g_banner->FontSize, FLT_MAX, 0, text);
    float h = ts.y + S(8), w = std::min(ts.x + S(90), ImGui::GetContentRegionAvail().x);
    float skew = S(8), solid = w * 0.55f;
    // a slanted solid head, orange into crimson, then the stroke fading out, a darker streak through it like a brush
    ImVec2 q[4] = {{p.x + skew, p.y}, {p.x + solid, p.y}, {p.x + solid, p.y + h}, {p.x, p.y + h}};
    int v0 = dl->VtxBuffer.Size;
    dl->AddConvexPolyFilled(q, 4, IM_COL32_WHITE);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, p, p + ImVec2(solid, 0), kOrange, kCrimson);
    dl->AddRectFilledMultiColor({p.x + solid, p.y}, {p.x + w, p.y + h}, kCrimson, Rgb(179, 18, 42, 0),
                                Rgb(179, 18, 42, 0), kCrimson);
    dl->AddLine({p.x, p.y + h * 0.7f}, {p.x + w * 0.6f, p.y + h * 0.66f}, Rgb(120, 10, 24, 90), S(3));
    dl->AddLine({p.x + S(4), p.y + h + S(2)}, {p.x + w * 0.85f, p.y + h + S(2)}, Rgb(250, 226, 150, 150), S(1));
    ShadedText(dl, g_banner, p + ImVec2(S(14), S(3)), text, IM_COL32_WHITE, Rgb(255, 228, 200), 1.5f);
    ImGui::Dummy({w, h + S(8)});
}

// the games' menu cursor: a gem pointing at the choice
void Pointer(ImDrawList* dl, ImVec2 tip) {
    float r = S(7);
    ImVec2 pts[3] = {{tip.x, tip.y}, {tip.x - r * 1.8f, tip.y - r}, {tip.x - r * 1.8f, tip.y + r}};
    dl->AddTriangleFilled(pts[0], pts[1], pts[2], Rgb(60, 170, 150));
    dl->AddTriangle(pts[0], pts[1], pts[2], kGoldHi, S(1));
    Diamond(dl, {tip.x - r * 2.4f, tip.y}, S(3), kGoldC, kGoldHi);
}

// a bronze bar with gold edges; crimson for the big one
bool GothicButton(const char* label, ImVec2 size, bool play = false) {
    ImFont* f = play ? g_play : g_strong;
    ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, label);
    if (size.x <= 0) size.x = size.x < 0 ? ImGui::GetContentRegionAvail().x : ts.x + S(36);
    if (size.y <= 0) size.y = ts.y + S(12);
    ImVec2 a = ImGui::GetCursorScreenPos(), b = a + size;
    bool pressed = ImGui::InvisibleButton(label, size);
    bool hover = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    bool disabled = ImGui::GetItemFlags() & ImGuiItemFlags_Disabled;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 top = play ? Rgb(176, 24, 40) : kBronzeTop, bottom = play ? kBlood : kBronzeBottom;
    if (hover && !disabled) top = play ? Rgb(214, 40, 56) : Rgb(156, 98, 46);
    if (held) std::swap(top, bottom);
    dl->AddRectFilledMultiColor(a, b, top, top, bottom, bottom);
    // a sheen along the top
    dl->AddRectFilledMultiColor(a, {b.x, a.y + size.y * 0.45f}, Rgb(255, 230, 180, 40), Rgb(255, 230, 180, 40), 0, 0);
    if (play) {
        // breathes while it waits
        float pulse = 0.5f + 0.5f * std::sin(float(ImGui::GetTime()) * 2.4f);
        for (int i = 1; i <= 3; i++)
            dl->AddRect(a - ImVec2(S(i * 2.0f), S(i * 2.0f)), b + ImVec2(S(i * 2.0f), S(i * 2.0f)),
                        Rgb(250, 200, 120, int((disabled ? 0 : 60 * pulse) / i)), 0, 0, S(2));
        Frame(dl, a, b);
    } else {
        dl->AddRect(a, b, hover && !disabled ? kGoldHi : kGoldC, 0, 0, S(1.2f));
        dl->AddRect(a + ImVec2(S(3), S(3)), b - ImVec2(S(3), S(3)), Rgb(0, 0, 0, 90), 0, 0, S(1));
    }
    ImVec2 tp = a + ImVec2((size.x - ts.x) * 0.5f, (size.y - ts.y) * 0.5f);
    if (play) ShadedText(dl, f, tp, label, kGoldHi, Rgb(214, 160, 70), 2);
    else ShadedText(dl, f, tp, label, kParchment, Rgb(214, 196, 160), 1.5f);
    if (disabled) dl->AddRectFilled(a, b, Rgb(5, 6, 14, 140));
    if (hover && !disabled) Pointer(dl, {a.x - S(4), a.y + size.y * 0.5f});
    return pressed && !disabled;
}

// a tab: the brush banner when chosen, a navy plate when not
bool GothicTab(const char* label, bool selected) {
    ImVec2 ts = g_strong->CalcTextSizeA(g_strong->FontSize, FLT_MAX, 0, label);
    ImVec2 size{ts.x + S(34), ts.y + S(12)};
    ImVec2 a = ImGui::GetCursorScreenPos(), b = a + size;
    bool pressed = ImGui::InvisibleButton(label, size);
    bool hover = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected) {
        dl->AddRectFilledMultiColor(a, b, kOrange, kCrimson, kCrimson, kOrange);
        dl->AddRect(a, b, kGoldHi, 0, 0, S(1.2f));
    } else {
        ImU32 t = hover ? Rgb(40, 60, 112) : Rgb(20, 32, 70);
        dl->AddRectFilledMultiColor(a, b, t, t, kNavyBottom, kNavyBottom);
        dl->AddRect(a, b, hover ? kSteel : kSteelDim, 0, 0, S(1));
    }
    ShadedText(dl, g_strong, a + ImVec2(S(17), S(6)), label, selected ? IM_COL32_WHITE : kParchment,
               selected ? Rgb(255, 230, 200) : Rgb(190, 180, 160), 1.5f);
    ImGui::SameLine(0, S(6));
    return pressed;
}

// night sky, moon, the castle on its rock, mist and bats; behind everything
void Backdrop(ImDrawList* dl, ImVec2 size, float t) {
    dl->AddRectFilledMultiColor({0, 0}, size, Rgb(14, 22, 52), Rgb(14, 22, 52), Rgb(3, 4, 10), Rgb(3, 4, 10));
    // moon and its glow, upper right
    ImVec2 moon{size.x - S(150), S(64)};
    for (int i = 6; i >= 1; i--) dl->AddCircleFilled(moon, S(30 + i * 10.0f), Rgb(150, 170, 210, 7));
    dl->AddCircleFilled(moon, S(30), Rgb(226, 228, 210));
    dl->AddCircleFilled(moon + ImVec2(S(9), S(-5)), S(7), Rgb(200, 202, 186));
    dl->AddCircleFilled(moon + ImVec2(S(-10), S(8)), S(5), Rgb(206, 208, 192));
    // the castle: towers with spires along the header's right side
    struct Tower {
        float x, w, h, spire;
    };
    static const Tower kTowers[] = {{-330, 26, 46, 22}, {-300, 18, 70, 30}, {-276, 40, 54, 0},   {-232, 22, 96, 44},
                                    {-206, 30, 64, 18}, {-172, 16, 110, 52}, {-152, 46, 72, 0},  {-102, 20, 88, 36},
                                    {-78, 34, 58, 20},  {-40, 14, 76, 30},   {-22, 30, 44, 14}};
    float base = S(128);
    ImU32 dark = Rgb(5, 7, 16);
    dl->AddRectFilled({size.x - S(345), base - S(30)}, {size.x, base + S(4)}, dark);
    for (const Tower& tw : kTowers) {
        float x = size.x + S(tw.x), w = S(tw.w), top = base - S(tw.h);
        dl->AddRectFilled({x, top}, {x + w, base}, dark);
        if (tw.spire > 0) dl->AddTriangleFilled({x - S(3), top}, {x + w + S(3), top}, {x + w * 0.5f, top - S(tw.spire)}, dark);
        else
            for (float c = x; c < x + w - S(3); c += S(7)) dl->AddRectFilled({c, top - S(5)}, {c + S(4), top}, dark);
        // a lit window or two
        if (int(tw.h) % 3 == 0) dl->AddRectFilled({x + w * 0.4f, top + S(14)}, {x + w * 0.4f + S(3), top + S(20)}, Rgb(240, 170, 70, 200));
    }
    // mist drifting across
    for (int i = 0; i < 6; i++) {
        float y = size.y * (0.25f + 0.13f * i);
        float x = std::fmod(t * (8.0f + i * 3) + i * 230.0f, size.x + S(600)) - S(300);
        dl->AddCircleFilled({x, y}, S(140 + i * 25.0f), Rgb(110, 130, 170, 9), 48);
        dl->AddCircleFilled({size.x - x, y + S(40)}, S(110 + i * 20.0f), Rgb(110, 130, 170, 7), 48);
    }
    // bats crossing the moon
    for (int i = 0; i < 3; i++) {
        float bt = t * (0.05f + 0.013f * i) + i * 0.37f;
        float x = size.x - S(420) + std::fmod(bt, 1.0f) * S(440);
        float y = S(40 + i * 18.0f) + std::sin(t * 1.7f + i) * S(6);
        float flap = std::sin(t * 12 + i * 2) * S(3);
        dl->AddTriangleFilled({x, y}, {x - S(9), y - S(3) + flap}, {x - S(4), y + S(2)}, dark);
        dl->AddTriangleFilled({x, y}, {x + S(9), y - S(3) + flap}, {x + S(4), y + S(2)}, dark);
    }
    // the ornate band along the top
    dl->AddRectFilledMultiColor({0, 0}, {size.x, S(10)}, Rgb(38, 110, 118), Rgb(38, 110, 118), Rgb(12, 44, 52), Rgb(12, 44, 52));
    for (float x = S(10); x < size.x; x += S(22)) Diamond(dl, {x, S(5)}, S(3), Rgb(120, 200, 196, 160), Rgb(12, 44, 52));
    dl->AddLine({0, S(10)}, {size.x, S(10)}, kGoldC, S(1));
    // darker edges
    ImU32 clear = 0, edge = Rgb(0, 0, 0, 150);
    dl->AddRectFilledMultiColor({0, 0}, {S(80), size.y}, edge, clear, clear, edge);
    dl->AddRectFilledMultiColor({size.x - S(80), 0}, size, clear, edge, edge, clear);
    dl->AddRectFilledMultiColor({0, size.y - S(70)}, size, clear, clear, edge, edge);
}

void Style() {
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = st.ChildRounding = st.FrameRounding = st.PopupRounding = st.GrabRounding = 0;
    st.WindowBorderSize = 0;
    st.ChildBorderSize = 0;
    st.FrameBorderSize = 1;
    st.PopupBorderSize = 1;
    st.WindowPadding = {18, 14};
    st.FramePadding = {10, 5};
    st.ItemSpacing = {10, 9};
    ImVec4* c = st.Colors;
    auto v = [](ImU32 u) { return ImGui::ColorConvertU32ToFloat4(u); };
    c[ImGuiCol_WindowBg] = {0, 0, 0, 0};
    c[ImGuiCol_Text] = v(kParchment);
    c[ImGuiCol_TextDisabled] = kDim;
    c[ImGuiCol_PopupBg] = v(Rgb(10, 18, 46, 250));
    c[ImGuiCol_Border] = v(kSteelDim);
    c[ImGuiCol_FrameBg] = v(Rgb(8, 13, 34, 230));
    c[ImGuiCol_FrameBgHovered] = v(Rgb(26, 40, 86, 240));
    c[ImGuiCol_FrameBgActive] = v(Rgb(36, 52, 104, 250));
    c[ImGuiCol_Header] = v(Rgb(150, 20, 36));
    c[ImGuiCol_HeaderHovered] = v(Rgb(190, 40, 52));
    c[ImGuiCol_HeaderActive] = v(kCrimson);
    c[ImGuiCol_Button] = v(kBronzeBottom);
    c[ImGuiCol_ButtonHovered] = v(kBronzeTop);
    c[ImGuiCol_ButtonActive] = v(kBronzeTop);
    c[ImGuiCol_CheckMark] = kGold;
    c[ImGuiCol_PlotHistogram] = v(kCrimson);
    c[ImGuiCol_NavHighlight] = kGold;
    c[ImGuiCol_Separator] = v(kGoldLo);
    c[ImGuiCol_TableRowBg] = {0, 0, 0, 0};
    c[ImGuiCol_TableRowBgAlt] = v(Rgb(255, 255, 255, 10));
    c[ImGuiCol_TableBorderLight] = v(kSteelDim);
    c[ImGuiCol_ScrollbarBg] = {0, 0, 0, 0};
    c[ImGuiCol_ScrollbarGrab] = v(kBronzeTop);
    st.ScaleAllSizes(g_scale);
}

void Fonts() {
    ImGuiIO& io = ImGui::GetIO();
    auto load = [&](const char* file, float size, const char* fallback = nullptr) -> ImFont* {
        std::string path = std::string("C:\\Windows\\Fonts\\") + file;
        std::error_code ec;
        if (!fs::exists(path, ec) && fallback) path = std::string("C:\\Windows\\Fonts\\") + fallback;
        if (!fs::exists(path, ec)) return nullptr;
        return io.Fonts->AddFontFromFileTTF(path.c_str(), size * g_scale);
    };
    g_body = load("pala.ttf", 19, "georgia.ttf");
    if (!g_body) g_body = io.Fonts->AddFontDefault();
    g_strong = load("palab.ttf", 19, "georgiab.ttf");
    g_banner = load("georgiaz.ttf", 21, "palabi.ttf");
    g_title = load("palab.ttf", 54, "georgiab.ttf");
    g_sub = load("palabi.ttf", 27, "georgiaz.ttf");
    g_play = load("palab.ttf", 30, "georgiab.ttf");
    for (ImFont** f : {&g_strong, &g_banner, &g_title, &g_sub, &g_play})
        if (!*f) *f = g_body;
}

// the launcher's state and its one screen
class Launcher {
public:
    Launcher() {
        settings_.Load();
        files_.Scan();
        modes_ = Resolutions();
    }

    // false: the launcher closes
    bool Frame() {
        if (rescan_ && !import_.running) {
            rescan_ = false;
            files_.Scan();
            if (!import_.result.empty()) status_ = import_.result;
        }
        ImGuiIO& io = ImGui::GetIO();
        Backdrop(ImGui::GetBackgroundDrawList(), io.DisplaySize, float(ImGui::GetTime()));
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("launcher", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground);
        Header();
        float footer = S(78);
        float left = ImGui::GetContentRegionAvail().x * 0.35f;
        if (BeginPanel("files", {left, -footer})) GamePanel();
        ImGui::EndChild();
        ImGui::SameLine(0, S(16));
        if (BeginPanel("settings", {0, -footer})) {
            for (int i = 0; i < 4; i++) {
                static const char* kTabs[] = {"Display", "Graphics", "Game", "Controls"};
                if (GothicTab(kTabs[i], tab_ == i)) tab_ = i;
            }
            ImGui::NewLine();
            ImGui::Dummy({0, S(4)});
            switch (tab_) {
                case 0: DisplayTab(); break;
                case 1: GraphicsTab(); break;
                case 2: GameTab(); break;
                default: ControlsTab(); break;
            }
        }
        ImGui::EndChild();
        bool keep = Footer();
        ImGui::End();
        return keep;
    }

private:
    void Header() {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos() + ImVec2(S(4), S(4));
        ShadedText(dl, g_title, p, "Castlevania", kGoldHi, Rgb(170, 110, 36), 3);
        ImVec2 ts = g_title->CalcTextSizeA(g_title->FontSize, FLT_MAX, 0, "Castlevania");
        ShadedText(dl, g_sub, p + ImVec2(ts.x * 0.42f, ts.y * 0.80f), "Curse of Darkness", Rgb(236, 70, 70),
                   Rgb(140, 14, 28), 2);
        ImGui::Dummy({ts.x, ts.y + g_sub->FontSize * 0.75f});
        ImGui::PushFont(g_body);
        ImGui::TextColored(kDim, "Native PC port of the original Xbox release");
        ImGui::PopFont();
        ImGui::Dummy({0, S(2)});
    }

    void GamePanel() {
        Banner("Game Files");
        if (files_.assets.empty()) {
            ImGui::TextColored(kBad, "No assets folder found.");
            ImGui::TextWrapped("Make an assets folder beside the game and put your game files in it.");
        } else {
            switch (files_.xbe) {
                case Files::Xbe::Match: ImGui::TextColored(kGood, "default.xbe - USA release, ready"); break;
                case Files::Xbe::Other:
                    ImGui::TextColored(kBad, "default.xbe - another release");
                    ImGui::TextWrapped("Only the USA (NTSC-U) release works with this build.");
                    break;
                case Files::Xbe::Missing: ImGui::TextColored(kDim, "default.xbe - not extracted"); break;
            }
            for (const fs::path& iso : files_.isos) ImGui::TextColored(kGood, "Disc image - %s", Utf8(iso.filename().wstring()).c_str());
            if (files_.xbe == Files::Xbe::Missing && files_.isos.empty())
                ImGui::TextWrapped("Add your disc image (.iso), or extract the disc into the assets folder: default.xbe "
                                   "with its data, movie and sound folders.");
        }
        if (!files_.exe) ImGui::TextColored(kBad, "Castlevania.exe is not beside the launcher.");
        ImGui::Dummy({0, S(2)});
        if (import_.running) ImGui::ProgressBar(import_.progress, {-FLT_MIN, 0}, "Copying disc image...");
        else if (GothicButton("Add Disc Image", {-1, 0})) AddImage();
        if (GothicButton("Open Assets Folder", {-1, 0})) {
            fs::path a = files_.assets.empty() ? ExeDir() / "assets" : files_.assets;
            std::error_code ec;
            fs::create_directories(a, ec);
            Open(a);
            rescan_ = true;
        }
        if (GothicButton("Check Again", {-1, 0})) rescan_ = true;

        ImGui::Dummy({0, S(6)});
        Banner("Records");
        float half = (ImGui::GetContentRegionAvail().x - S(10)) * 0.5f;
        if (GothicButton("Saves", {half, 0})) {
            fs::path s = ExeDir() / kSaves;
            std::error_code ec;
            Open(fs::exists(s, ec) ? s : ExeDir());
        }
        ImGui::SameLine(0, S(10));
        if (GothicButton("Logs", {half, 0})) {
            std::error_code ec;
            Open(fs::exists(ExeDir() / "logs", ec) ? ExeDir() / "logs" : ExeDir());
        }
    }

    void DisplayTab() {
        if (!BeginRows("display")) return;
        Row("Display mode", "Borderless fills the monitor without changing its mode, and switches away instantly.");
        Combo<std::string>("##mode", settings_.display_mode, {"fullscreen", "borderless", "windowed"},
                           {"Fullscreen", "Borderless", "Windowed"});
        Row("Resolution", "The output size. Fullscreen switches the monitor to it; borderless always fills the monitor.");
        {
            size_t cur = 0;
            for (size_t i = 0; i < modes_.size(); i++)
                if (modes_[i].w == settings_.width && modes_[i].h == settings_.height) cur = i;
            if (ImGui::BeginCombo("##res", ResName(modes_[cur]).c_str())) {
                for (size_t i = 0; i < modes_.size(); i++)
                    if (ImGui::Selectable(ResName(modes_[i]).c_str(), i == cur)) {
                        settings_.width = modes_[i].w;
                        settings_.height = modes_[i].h;
                    }
                ImGui::EndCombo();
            }
        }
        Row("V-Sync", "Off can tear, and lowers input latency a little. The game runs at 60 FPS either way.");
        ImGui::Checkbox("##vsync", &settings_.vsync);
        ImGui::EndTable();
    }

    void GraphicsTab() {
        if (!BeginRows("graphics")) return;
        Row("Widescreen", "16:9 with more of the world at the sides, nothing stretched. The HUD, menus and movies stay "
                          "4:3 in the middle. Off shows the original 4:3 picture. Needs a 16:9 or wider resolution.");
        ImGui::Checkbox("##wide", &settings_.widescreen);
        Row("Render scale", "Of the output height. Above 100% supersamples for a sharper, smoother picture; below "
                            "100% is faster on weak GPUs.");
        Combo<uint32_t>("##scale", settings_.render_percent, {50, 75, 100, 150, 200},
                        {"50%", "75%", "100% (native)", "150%", "200%"});
        Row("Anisotropic filtering", "Sharper textures on floors and walls seen at an angle.");
        Combo<uint32_t>("##aniso", settings_.aniso, {0, 2, 4, 8, 16}, {"Off (as the game asks)", "2x", "4x", "8x", "16x"});
        ImGui::EndTable();
        ImGui::Dummy({0, S(4)});
        ImGui::TextColored(kDim, "Presets");
        auto preset = [&](const char* name, uint32_t scale, uint32_t aniso) {
            bool on = settings_.render_percent == scale && settings_.aniso == aniso;
            if (GothicTab(name, on)) {
                settings_.render_percent = scale;
                settings_.aniso = aniso;
            }
        };
        preset("Performance", 75, 0);
        preset("Original", 100, 0);
        preset("High", 150, 8);
        preset("Ultra", 200, 16);
        ImGui::NewLine();
    }

    void GameTab() {
        if (!BeginRows("game")) return;
        Row("FPS counter", nullptr);
        ImGui::Checkbox("##fps", &settings_.fps_counter);
        Row("Pause on focus loss", "Stops the game while another window is in front.");
        ImGui::Checkbox("##pause", &settings_.pause_on_focus_loss);
        Row("Settings menu key", "The key that opens the in-game settings menu. Pick another if F10 does nothing on "
                                 "your keyboard (laptops often need Fn for it). L3 + R3 on a controller always works.");
        static const std::vector<uint32_t> keys = {VK_F10, VK_F1, VK_F2, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9,
                                                   VK_F11, VK_F12, VK_INSERT, VK_HOME, VK_END, VK_PRIOR, VK_NEXT,
                                                   VK_PAUSE, VK_SCROLL, VK_OEM_3};
        static std::vector<std::string> names = [] {
            std::vector<std::string> n;
            for (uint32_t k : keys) n.push_back(KeyName(k));
            return n;
        }();
        Combo<uint32_t>("##key", settings_.menu_key, keys, names);
        Row("Keep launcher open", "Leave this window open while the game runs.");
        ImGui::Checkbox("##keep", &settings_.keep_open);
        ImGui::EndTable();
    }

    void ControlsTab() {
        ImGui::TextWrapped("Xbox, PlayStation and Switch controllers play as player 1, with rumble. The keyboard plays "
                           "alongside.");
        static const char* kKeys[][2] = {
            {"W A S D", "Move"},         {"Arrow keys", "Camera"},           {"K / Space", "A (jump)"},
            {"J", "X (attack)"},         {"L", "B"},                         {"I", "Y"},
            {"Q / E", "Triggers"},     {"Z / C", "White / black"},   {"Enter", "Start"},
            {"Backspace", "Back"},       {"Numpad 8 2 4 6", "D-pad"},        {"L3 + R3", "Settings menu"},
        };
        if (ImGui::BeginTable("keys", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
            auto cell = [](const char* key, const char* what) {
                ImGui::TableNextColumn();
                ImGui::TextColored(kGold, "%s", key);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(what);
            };
            std::string menu = KeyName(settings_.menu_key);
            cell(menu.c_str(), "Settings menu");
            for (auto& k : kKeys) cell(k[0], k[1]);
            ImGui::EndTable();
        }
    }

    bool Footer() {
        ImGui::Dummy({0, S(4)});
        ImVec2 row = ImGui::GetCursorPos();
        float h = S(44);
        if (GothicButton("Defaults", {S(130), h})) {
            settings_.Defaults();
            status_ = "Defaults restored. Save or Play keeps them.";
        }
        ImGui::SameLine(0, S(12));
        if (GothicButton("Save", {S(110), h}))
            status_ = settings_.Save() ? "Settings saved." : "Could not write the settings file.";
        ImGui::SameLine(0, S(18));
        ImGui::SetCursorPosY(row.y + (h - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::PushFont(g_body);
        if (!status_.empty()) ImGui::TextColored(kGold, "%s", status_.c_str());
        else ImGui::TextColored(kDim, "Also changeable in game.");
        ImGui::PopFont();
        float play_w = S(230);
        ImGui::SetCursorPos({ImGui::GetWindowContentRegionMax().x - play_w - S(6), row.y - S(4)});
        bool can = files_.Playable() && !import_.running;
        ImGui::BeginDisabled(!can);
        bool play = GothicButton("PLAY", {play_w, h + S(8)}, true);
        ImGui::EndDisabled();
        if (!can && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(files_.exe ? "Add the game files first." : "Castlevania.exe is missing.");
        if (play) return Play();
        return true;
    }

    bool Play() {
        if (!settings_.Save()) {
            status_ = "Could not write the settings file.";
            return true;
        }
        fs::path exe = ExeDir() / kGameExe;
        std::wstring cmd = L"\"" + exe.wstring() + L"\"";
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, ExeDir().c_str(), &si, &pi)) {
            status_ = "Could not start the game (error " + std::to_string(GetLastError()) + ").";
            return true;
        }
        // the game may bring its window to the front
        AllowSetForegroundWindow(pi.dwProcessId);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        status_ = "The castle awaits.";
        return settings_.keep_open;
    }

    void AddImage() {
        wchar_t file[MAX_PATH] = {};
        OPENFILENAMEW ofn{sizeof(ofn)};
        ofn.hwndOwner = g_hwnd;
        ofn.lpstrFilter = L"Xbox disc image (*.iso)\0*.iso\0All files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrTitle = L"Your Castlevania: Curse of Darkness disc image";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&ofn)) return;
        fs::path from = file;
        fs::path assets = files_.assets.empty() ? ExeDir() / "assets" : files_.assets;
        fs::path to = assets / from.filename();
        std::error_code ec;
        if (fs::equivalent(from.parent_path(), assets, ec)) {
            status_ = "That image is already in the assets folder.";
            rescan_ = true;
            return;
        }
        import_.Start(from, to);
        rescan_ = true;
    }

    Settings settings_;
    Files files_;
    Import import_;
    std::vector<Res> modes_;
    std::string status_;
    bool rescan_ = false;
    int tab_ = 0;
};

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    float scale = float(GetDpiForSystem()) / 96.0f;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"CastlevaniaLauncher";
    RegisterClassExW(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{0, 0, LONG(1000 * scale), LONG(660 * scale)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, GetDpiForSystem());
    int w = r.right - r.left, h = r.bottom - r.top;
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Castlevania: Curse of Darkness - Launcher", style,
                             (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h,
                             nullptr, nullptr, inst, nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    if (!CreateDevice(g_hwnd)) {
        MessageBoxW(nullptr, L"Direct3D 11 is not available.", L"Castlevania Launcher", MB_ICONERROR);
        return 1;
    }
    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    g_scale = scale;
    Style();
    Fonts();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    Launcher launcher;
    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;
        if (IsIconic(g_hwnd)) {
            Sleep(50);
            continue;
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        if (!launcher.Frame()) running = false;
        ImGui::Render();
        const float clear[4] = {0.01f, 0.02f, 0.04f, 1};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_rtv) g_rtv->Release();
    g_swap->Release();
    g_ctx->Release();
    g_dev->Release();
    DestroyWindow(g_hwnd);
    return 0;
}
