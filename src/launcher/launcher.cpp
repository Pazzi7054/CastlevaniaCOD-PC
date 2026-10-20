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
    sd.BufferDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;  // 10 bits: no bands in the gradients
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

// the look: dark and flat, one crimson accent, Segoe UI with the Segoe Fluent icons, a sidebar of pages. Settings
// save as they change.

float g_scale = 1;
ImFont* g_body = nullptr;   // Segoe UI 16, icons merged
ImFont* g_small = nullptr;  // Segoe UI 13.5, descriptions
ImFont* g_semi = nullptr;   // Segoe UI Semibold 16, icons merged
ImFont* g_head = nullptr;   // Segoe UI Semibold 22, page titles
ImFont* g_hero = nullptr;   // Segoe UI Bold 46, the title on the home page
ImFont* g_heroSub = nullptr;
ImFont* g_button = nullptr;  // Segoe UI Semibold 20, Play

float S(float v) { return v * g_scale; }

ImU32 Rgb(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
const ImU32 kBg = Rgb(13, 14, 18), kSide = Rgb(18, 19, 24), kCard = Rgb(24, 25, 31), kCardHi = Rgb(31, 33, 40);
const ImU32 kLine = Rgb(42, 44, 53), kText = Rgb(236, 236, 241), kMuted = Rgb(140, 142, 154);
const ImU32 kAccent = Rgb(224, 41, 74), kAccentHi = Rgb(255, 66, 98), kAccentLo = Rgb(176, 26, 54);
const ImU32 kGreen = Rgb(60, 207, 145), kRed = Rgb(255, 92, 92), kAmber = Rgb(255, 184, 76);
ImVec4 V(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

ImVec2 operator+(ImVec2 a, ImVec2 b) { return {a.x + b.x, a.y + b.y}; }
ImVec2 operator-(ImVec2 a, ImVec2 b) { return {a.x - b.x, a.y - b.y}; }

// Segoe Fluent / MDL2 icons
enum : uint32_t {
    kIconHome = 0xE80F, kIconDisplay = 0xE7F4, kIconGraphics = 0xE790, kIconGame = 0xE713, kIconKeys = 0xE765,
    kIconFolder = 0xE8B7, kIconPlay = 0xE768, kIconCheck = 0xE73E, kIconWarn = 0xE7BA, kIconAdd = 0xE710,
    kIconRefresh = 0xE72C, kIconSave = 0xE74E, kIconLog = 0xE9F9, kIconReset = 0xE777, kIconChevron = 0xE76C,
    kIconDisc = 0xE958,
};
const ImWchar kIconRanges[] = {0xE710, 0xE710, 0xE713, 0xE713, 0xE72C, 0xE72C, 0xE73E, 0xE73E, 0xE74E, 0xE74E,
                               0xE765, 0xE765, 0xE768, 0xE768, 0xE76C, 0xE76C, 0xE777, 0xE777, 0xE790, 0xE790,
                               0xE7BA, 0xE7BA, 0xE7F4, 0xE7F4, 0xE80F, 0xE80F, 0xE8B7, 0xE8B7, 0xE958, 0xE958,
                               0xE9F9, 0xE9F9, 0};

std::string Icon(uint32_t cp) {
    char s[5] = {};
    ImTextCharToUtf8(s, cp);
    return s;
}

void Text(ImFont* f, ImU32 col, const char* s) {
    ImGui::PushFont(f);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void Wrapped(ImFont* f, ImU32 col, const char* s) {
    ImGui::PushFont(f);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextWrapped("%s", s);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// an animated 0..1 per id, easing toward on
float Ease(ImGuiID id, bool on, float speed = 12) {
    ImGuiStorage* st = ImGui::GetStateStorage();
    float t = st->GetFloat(id, on ? 1.0f : 0.0f);
    float dt = ImGui::GetIO().DeltaTime;
    t += ((on ? 1.0f : 0.0f) - t) * std::min(1.0f, dt * speed);
    st->SetFloat(id, t);
    return t;
}

ImU32 Mix(ImU32 a, ImU32 b, float t) {
    ImVec4 x = V(a), y = V(b);
    return ImGui::ColorConvertFloat4ToU32({x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t});
}

// an on/off switch; true when flipped
bool Toggle(const char* id, bool& v) {
    ImVec2 size{S(42), S(22)};
    ImVec2 a = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(id, size);
    if (pressed) v = !v;
    float t = Ease(ImGui::GetItemID(), v);
    bool hover = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = size.y * 0.5f;
    dl->AddRectFilled(a, a + size, Mix(hover ? Rgb(58, 60, 70) : Rgb(46, 48, 57), hover ? kAccentHi : kAccent, t), r);
    dl->AddCircleFilled({a.x + r + t * (size.x - 2 * r), a.y + r}, r - S(3.5f), kText, 24);
    return pressed;
}

// joined buttons, one chosen
template <typename T>
bool Segmented(const char* id, T& value, const std::vector<T>& values, const std::vector<std::string>& names) {
    ImGui::PushID(id);
    ImGui::PushFont(g_semi);
    float avail = ImGui::GetContentRegionAvail().x;
    float h = S(32), w = avail / float(values.size());
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, a + ImVec2(avail, h), Rgb(16, 17, 22), S(7));
    dl->AddRect(a, a + ImVec2(avail, h), kLine, S(7));
    bool changed = false;
    for (size_t i = 0; i < values.size(); i++) {
        ImVec2 p{a.x + w * float(i), a.y};
        ImGui::SetCursorScreenPos(p);
        ImGui::PushID(int(i));
        if (ImGui::InvisibleButton("seg", {w, h}) && !(values[i] == value)) {
            value = values[i];
            changed = true;
        }
        bool on = values[i] == value, hover = ImGui::IsItemHovered();
        float t = Ease(ImGui::GetItemID(), on, 16);
        ImVec2 in{S(3), S(3)};
        if (t > 0.01f) dl->AddRectFilled(p + in, p + ImVec2(w, h) - in, Mix(kCard, kAccent, t), S(5));
        else if (hover) dl->AddRectFilled(p + in, p + ImVec2(w, h) - in, kCardHi, S(5));
        ImVec2 ts = ImGui::CalcTextSize(names[i].c_str());
        dl->AddText(p + ImVec2((w - ts.x) * 0.5f, (h - ts.y) * 0.5f), on ? kText : (hover ? kText : kMuted), names[i].c_str());
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(a);
    ImGui::Dummy({avail, h});
    ImGui::PopFont();
    ImGui::PopID();
    return changed;
}

template <typename T>
void Combo(const char* id, T& value, const std::vector<T>& values, const std::vector<std::string>& names) {
    size_t cur = 0;
    for (size_t i = 0; i < values.size(); i++)
        if (values[i] == value) cur = i;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo(id, names[cur].c_str())) {
        for (size_t i = 0; i < values.size(); i++) {
            if (ImGui::Selectable(names[i].c_str(), i == cur)) value = values[i];
            if (i == cur) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

enum class Kind { Primary, Secondary, Ghost };

bool Button(const char* label, ImVec2 size, Kind kind = Kind::Secondary, uint32_t icon = 0, ImFont* font = nullptr) {
    font = font ? font : g_semi;
    std::string text = icon ? Icon(icon) + "   " + label : label;
    ImVec2 ts = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, text.c_str());
    if (size.x == 0) size.x = ts.x + S(32);
    if (size.x < 0) size.x = ImGui::GetContentRegionAvail().x;
    if (size.y == 0) size.y = S(38);
    ImVec2 a = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(label, size);
    bool disabled = ImGui::GetItemFlags() & ImGuiItemFlags_Disabled;
    float hv = Ease(ImGui::GetItemID(), ImGui::IsItemHovered() && !disabled, 14);
    bool held = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = S(8);
    ImU32 fill, text_col = kText;
    switch (kind) {
        case Kind::Primary: fill = Mix(kAccent, kAccentHi, hv); break;
        case Kind::Secondary: fill = Mix(kCardHi, Rgb(44, 46, 56), hv); break;
        default: fill = Mix(Rgb(0, 0, 0, 0), kCardHi, hv); text_col = Mix(kMuted, kText, hv); break;
    }
    if (held) fill = kind == Kind::Primary ? kAccentLo : kCard;
    if (disabled) {
        fill = kind == Kind::Ghost ? 0 : Rgb(36, 37, 44);
        text_col = Rgb(90, 92, 102);
    }
    if (kind == Kind::Primary && !disabled) {
        // a soft glow under the main action
        for (int i = 1; i <= 4; i++)
            dl->AddRectFilled(a + ImVec2(-S(i * 1.5f), S(i * 1.2f)), a + size + ImVec2(S(i * 1.5f), S(i * 2.5f)),
                              Rgb(224, 41, 74, int((10 + 14 * hv) / i)), r + S(i * 2.0f));
    }
    dl->AddRectFilled(a, a + size, fill, r);
    if (kind == Kind::Secondary) dl->AddRect(a, a + size, Mix(kLine, Rgb(70, 72, 84), hv), r);
    dl->AddText(font, font->FontSize, a + ImVec2((size.x - ts.x) * 0.5f, (size.y - ts.y) * 0.5f), text_col, text.c_str());
    return pressed && !disabled;
}

// a rounded card; its contents laid out inside
void BeginCard(const char* id) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, V(kCard));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {S(20), S(16)});
    ImGui::BeginChild(id, {0, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Border);
}
void EndCard() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::Dummy({0, S(4)});
}

// a setting: title and description at the left, its control in the column at the right
bool BeginSetting(const char* title, const char* desc, bool first = false) {
    if (!first) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        ImGui::GetWindowDrawList()->AddLine({p.x, p.y}, {p.x + w, p.y}, kLine);
        ImGui::Dummy({0, S(6)});
    }
    float control = S(300);
    float w = ImGui::GetContentRegionAvail().x - control - S(24);
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    Text(g_semi, kText, title);
    if (desc) Wrapped(g_small, kMuted, desc);
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    float text_h = ImGui::GetItemRectSize().y;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - control);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, (text_h - S(32)) * 0.5f));
    ImGui::BeginGroup();
    return true;
}
void EndSetting() {
    ImGui::EndGroup();
    ImGui::Dummy({0, S(4)});
}

// a switch at the right end of the control column
void ToggleRight(const char* id, bool& v) {
    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - S(42));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(5));
    Toggle(id, v);
}

void PageTitle(const char* title, const char* desc) {
    Text(g_head, kText, title);
    if (desc) Wrapped(g_body, kMuted, desc);
    ImGui::Dummy({0, S(10)});
}

// a sidebar entry: icon and label, the chosen one with an accent bar
bool NavItem(uint32_t icon, const char* label, bool selected) {
    ImVec2 size{ImGui::GetContentRegionAvail().x, S(40)};
    ImVec2 a = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(label, size);
    float hv = Ease(ImGui::GetItemID(), ImGui::IsItemHovered(), 14);
    float on = Ease(ImGui::GetItemID() + 1, selected, 14);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (on > 0.01f || hv > 0.01f) dl->AddRectFilled(a, a + size, Rgb(255, 255, 255, int(8 * hv + 10 * on)), S(7));
    if (on > 0.01f) {
        float bar = S(16) * on;
        dl->AddRectFilled({a.x, a.y + (size.y - bar) * 0.5f}, {a.x + S(3), a.y + (size.y + bar) * 0.5f}, kAccent, S(2));
    }
    ImU32 col = selected ? kText : Mix(kMuted, kText, hv);
    std::string ic = Icon(icon);
    float y = a.y + (size.y - g_semi->FontSize) * 0.5f;
    dl->AddText(g_semi, g_semi->FontSize, {a.x + S(16), y + S(1)}, selected ? kAccentHi : col, ic.c_str());
    dl->AddText(g_semi, g_semi->FontSize, {a.x + S(46), y}, col, label);
    return pressed;
}

// a status line: coloured dot or icon and text
void Status(uint32_t icon, ImU32 col, const std::string& text) {
    ImGui::PushFont(g_semi);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(Icon(icon).c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine(0, S(10));
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopFont();
}

// a keyboard key drawn as a keycap
void Keycap(const char* key) {
    ImGui::PushFont(g_semi);
    ImVec2 ts = ImGui::CalcTextSize(key);
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImVec2 size{ts.x + S(16), ts.y + S(6)};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a + ImVec2(0, S(2)), a + size + ImVec2(0, S(2)), Rgb(10, 10, 14), S(5));
    dl->AddRectFilled(a, a + size, kCardHi, S(5));
    dl->AddRect(a, a + size, kLine, S(5));
    dl->AddText(a + ImVec2(S(8), S(3)), kText, key);
    ImGui::Dummy(size + ImVec2(0, S(2)));
    ImGui::PopFont();
}

void Style() {
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 0;
    st.ChildRounding = 10;
    st.FrameRounding = 7;
    st.PopupRounding = 8;
    st.GrabRounding = 7;
    st.ScrollbarRounding = 8;
    st.WindowBorderSize = 0;
    st.ChildBorderSize = 1;
    st.FrameBorderSize = 1;
    st.PopupBorderSize = 1;
    st.WindowPadding = {0, 0};
    st.FramePadding = {12, 7};
    st.ItemSpacing = {10, 8};
    st.ScrollbarSize = 10;
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = {0, 0, 0, 0};
    c[ImGuiCol_Text] = V(kText);
    c[ImGuiCol_TextDisabled] = V(kMuted);
    c[ImGuiCol_PopupBg] = V(Rgb(30, 31, 38, 252));
    c[ImGuiCol_Border] = V(kLine);
    c[ImGuiCol_FrameBg] = V(Rgb(16, 17, 22));
    c[ImGuiCol_FrameBgHovered] = V(kCardHi);
    c[ImGuiCol_FrameBgActive] = V(kCardHi);
    c[ImGuiCol_Button] = V(kCardHi);
    c[ImGuiCol_ButtonHovered] = V(Rgb(44, 46, 56));
    c[ImGuiCol_ButtonActive] = V(kCard);
    c[ImGuiCol_Header] = V(Rgb(224, 41, 74, 70));
    c[ImGuiCol_HeaderHovered] = V(Rgb(255, 255, 255, 18));
    c[ImGuiCol_HeaderActive] = V(Rgb(224, 41, 74, 110));
    c[ImGuiCol_CheckMark] = V(kAccent);
    c[ImGuiCol_PlotHistogram] = V(kAccent);
    c[ImGuiCol_NavHighlight] = V(kAccentHi);
    c[ImGuiCol_ScrollbarBg] = {0, 0, 0, 0};
    c[ImGuiCol_ScrollbarGrab] = V(Rgb(60, 62, 72));
    c[ImGuiCol_ScrollbarGrabHovered] = V(Rgb(80, 82, 94));
    c[ImGuiCol_TableRowBg] = {0, 0, 0, 0};
    c[ImGuiCol_TableRowBgAlt] = {0, 0, 0, 0};
    st.ScaleAllSizes(g_scale);
}

void Fonts() {
    ImGuiIO& io = ImGui::GetIO();
    std::error_code ec;
    auto path = [&](std::initializer_list<const char*> names) -> std::string {
        for (const char* n : names) {
            std::string p = std::string("C:\\Windows\\Fonts\\") + n;
            if (fs::exists(p, ec)) return p;
        }
        return {};
    };
    std::string regular = path({"segoeui.ttf"}), semi = path({"seguisb.ttf", "segoeui.ttf"});
    std::string bold = path({"segoeuib.ttf", "seguisb.ttf"}), icons = path({"SegoeIcons.ttf", "segmdl2.ttf"});
    auto load = [&](const std::string& file, float size, bool with_icons) -> ImFont* {
        if (file.empty()) return io.Fonts->AddFontDefault();
        ImFont* f = io.Fonts->AddFontFromFileTTF(file.c_str(), size * g_scale);
        if (with_icons && !icons.empty()) {
            ImFontConfig cfg;
            cfg.MergeMode = true;
            cfg.GlyphOffset = {0, S(2)};
            io.Fonts->AddFontFromFileTTF(icons.c_str(), size * g_scale, &cfg, kIconRanges);
        }
        return f;
    };
    g_body = load(regular, 16, true);
    g_small = load(regular, 13.5f, false);
    g_semi = load(semi, 16, true);
    g_head = load(semi, 24, false);
    g_hero = load(bold, 48, false);
    g_heroSub = load(semi, 24, false);
    g_button = load(semi, 20, true);
}

// the launcher's state and its one screen
class Launcher {
public:
    Launcher() {
        settings_.Load();
        saved_ = Snapshot();
        files_.Scan();
        modes_ = Resolutions();
    }

    // false: the launcher closes
    bool Frame() {
        if (rescan_ && !import_.running) {
            rescan_ = false;
            files_.Scan();
            if (!import_.result.empty()) Note(import_.result);
        }
        ImGuiIO& io = ImGui::GetIO();
        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        bg->AddRectFilled({0, 0}, io.DisplaySize, kBg);
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("launcher", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground);
        bool keep = true;
        Sidebar();
        ImGui::SameLine(0, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {S(36), S(28)});
        ImGui::BeginChild("page", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PushFont(g_body);
        switch (page_) {
            case 0: keep = HomePage(); break;
            case 1: DisplayPage(); break;
            case 2: GraphicsPage(); break;
            case 3: GamePage(); break;
            case 4: ControlsPage(); break;
            default: FilesPage(); break;
        }
        ImGui::PopFont();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::End();
        AutoSave();
        return keep;
    }

private:
    // every setting the launcher shows, to see when one changed
    std::string Snapshot() const {
        const Settings& s = settings_;
        return s.display_mode + "|" + std::to_string(s.width) + "x" + std::to_string(s.height) + "|" +
               std::to_string(s.render_percent) + "|" + std::to_string(s.aniso) + "|" + std::to_string(s.vsync) +
               std::to_string(s.widescreen) + std::to_string(s.fps_counter) + std::to_string(s.pause_on_focus_loss) +
               std::to_string(s.keep_open) + "|" + std::to_string(s.menu_key);
    }

    void AutoSave() {
        std::string now = Snapshot();
        if (now == saved_) return;
        saved_ = now;
        Note(settings_.Save() ? "Settings saved" : "Could not write the settings file");
    }

    void Note(const std::string& s) {
        status_ = s;
        status_time_ = ImGui::GetTime();
    }

    void Sidebar() {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 a = ImGui::GetCursorScreenPos();
        float w = S(232), h = ImGui::GetIO().DisplaySize.y;
        dl->AddRectFilled(a, a + ImVec2(w, h), kSide);
        dl->AddLine(a + ImVec2(w - 1, 0), a + ImVec2(w - 1, h), kLine);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {S(14), S(22)});
        ImGui::BeginChild("side", {w, h}, ImGuiChildFlags_AlwaysUseWindowPadding);
        // the wordmark
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(8));
        ImGui::BeginGroup();
        ImGui::PushFont(g_semi);
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(p + ImVec2(0, S(3)), p + ImVec2(S(4), S(37)), kAccent, S(2));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(14));
        ImGui::TextUnformatted("CASTLEVANIA");
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(14));
        ImGui::PopFont();
        Text(g_small, kMuted, "Curse of Darkness  |  PC");
        ImGui::EndGroup();
        ImGui::Dummy({0, S(22)});

        static const struct {
            uint32_t icon;
            const char* label;
        } kPages[] = {{kIconHome, "Home"},     {kIconDisplay, "Display"},  {kIconGraphics, "Graphics"},
                      {kIconGame, "Game"},     {kIconKeys, "Controls"},    {kIconFolder, "Game files"}};
        for (int i = 0; i < 6; i++) {
            if (i == 1) {
                ImGui::Dummy({0, S(6)});
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(16));
                Text(g_small, Rgb(100, 102, 114), "SETTINGS");
            }
            if (i == 5) ImGui::Dummy({0, S(6)});
            if (NavItem(kPages[i].icon, kPages[i].label, page_ == i)) page_ = i;
        }

        // at the bottom: the save note and a reset
        float bottom = ImGui::GetWindowHeight() - S(96);
        if (ImGui::GetCursorPosY() < bottom) ImGui::SetCursorPosY(bottom);
        float age = float(ImGui::GetTime() - status_time_);
        if (!status_.empty() && age < 4) {
            float alpha = std::clamp(4 - age, 0.0f, 1.0f);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(16));
            Text(g_small, Rgb(140, 142, 154, int(255 * alpha)), status_.c_str());
        } else {
            ImGui::Dummy({0, g_small->FontSize});
        }
        if (Button("Reset to defaults", {-1, S(36)}, Kind::Ghost, kIconReset)) {
            settings_.Defaults();
            Note("Defaults restored");
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    bool HomePage() {
        // the hero: a dark panel lit crimson from the top right corner
        float w = ImGui::GetContentRegionAvail().x, h = S(270);
        ImVec2 a = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        int v0 = dl->VtxBuffer.Size;
        dl->AddRectFilled(a, a + ImVec2(w, h), IM_COL32_WHITE, S(14));
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, a + ImVec2(w, 0), a + ImVec2(w * 0.35f, h),
                                                      Rgb(104, 20, 42), Rgb(18, 17, 23));
        dl->AddRect(a, a + ImVec2(w, h), Rgb(255, 255, 255, 14), S(14));

        ImGui::SetCursorScreenPos(a + ImVec2(S(34), S(30)));
        ImGui::BeginGroup();
        Text(g_small, kAccentHi, "ORIGINAL XBOX  |  NATIVE PC PORT");
        Text(g_hero, kText, "Castlevania");
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(10));
        Text(g_heroSub, Rgb(200, 200, 210), "Curse of Darkness");
        ImGui::Dummy({0, S(8)});
        bool ready = files_.Playable();
        if (ready) Status(kIconCheck, kGreen, "Ready to play");
        else if (!files_.exe) Status(kIconWarn, kRed, "Castlevania.exe is missing");
        else Status(kIconWarn, kAmber, "Game files needed");
        ImGui::EndGroup();

        ImGui::SetCursorScreenPos(a + ImVec2(S(34), h - S(30) - S(54)));
        bool keep = true;
        ImGui::BeginDisabled(!ready || import_.running);
        if (Button("PLAY", {S(220), S(54)}, Kind::Primary, kIconPlay, g_button)) keep = Play();
        ImGui::EndDisabled();
        if (!ready) {
            ImGui::SameLine(0, S(14));
            if (Button("Set up game files", {0, S(54)}, Kind::Secondary, kIconFolder)) page_ = 5;
        }
        ImGui::SetCursorScreenPos(a + ImVec2(0, h + S(20)));

        // what is set, at a glance; each opens its page
        Text(g_semi, kMuted, "Current setup");
        ImGui::Dummy({0, S(2)});
        Res r{settings_.width, settings_.height};
        std::string res = r.w ? std::to_string(r.w) + " x " + std::to_string(r.h) : "Desktop";
        std::string mode = settings_.display_mode == "fullscreen" ? "Fullscreen" : settings_.display_mode == "windowed" ? "Windowed" : "Borderless";
        struct Tile {
            const char* label;
            std::string value;
            int page;
        } tiles[] = {{"Display", mode + "  |  " + res, 1},
                     {"Widescreen", settings_.widescreen ? "On (16:9, Hor+)" : "Off (4:3)", 2},
                     {"Render scale", std::to_string(settings_.render_percent) + "%", 2},
                     {"Menu key", KeyName(settings_.menu_key) + "  |  L3 + R3", 3}};
        float gap = S(12), tw = (ImGui::GetContentRegionAvail().x - gap) / 2, th = S(68);
        for (int i = 0; i < 4; i++) {
            if (i % 2) ImGui::SameLine(0, gap);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("tile", {tw, th})) page_ = tiles[i].page;
            float hv = Ease(ImGui::GetItemID(), ImGui::IsItemHovered(), 14);
            ImGui::PopID();
            dl->AddRectFilled(p, p + ImVec2(tw, th), Mix(kCard, kCardHi, hv), S(10));
            dl->AddRect(p, p + ImVec2(tw, th), kLine, S(10));
            dl->AddText(g_small, g_small->FontSize, p + ImVec2(S(18), S(14)), kMuted, tiles[i].label);
            dl->AddText(g_semi, g_semi->FontSize, p + ImVec2(S(18), S(34)), kText, tiles[i].value.c_str());
            std::string chev = Icon(kIconChevron);
            dl->AddText(g_semi, g_semi->FontSize, p + ImVec2(tw - S(30), th * 0.5f - S(8)), Mix(kMuted, kText, hv), chev.c_str());
        }
        ImGui::Dummy({0, S(8)});
        std::string tip = "In game, press " + KeyName(settings_.menu_key) + " or click both sticks (L3 + R3) for these settings.";
        Wrapped(g_small, kMuted, tip.c_str());
        return keep;
    }

    void DisplayPage() {
        PageTitle("Display", "How the game window fills your screen.");
        BeginCard("display");
        BeginSetting("Display mode", "Borderless fills the monitor without changing its mode and switches away instantly.", true);
        Segmented<std::string>("mode", settings_.display_mode, {"fullscreen", "borderless", "windowed"},
                               {"Fullscreen", "Borderless", "Windowed"});
        EndSetting();
        BeginSetting("Resolution", "The output size. Fullscreen switches the monitor to it; borderless always fills the monitor.");
        {
            size_t cur = 0;
            for (size_t i = 0; i < modes_.size(); i++)
                if (modes_[i].w == settings_.width && modes_[i].h == settings_.height) cur = i;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##res", ResName(modes_[cur]).c_str())) {
                for (size_t i = 0; i < modes_.size(); i++)
                    if (ImGui::Selectable(ResName(modes_[i]).c_str(), i == cur)) {
                        settings_.width = modes_[i].w;
                        settings_.height = modes_[i].h;
                    }
                ImGui::EndCombo();
            }
        }
        EndSetting();
        BeginSetting("V-Sync", "Off can tear and lowers input latency a little. The game runs at 60 FPS either way.");
        ToggleRight("vsync", settings_.vsync);
        EndSetting();
        EndCard();
    }

    void GraphicsPage() {
        PageTitle("Graphics", "Picture quality and shape.");
        BeginCard("presets");
        BeginSetting("Quality preset", "Sets render scale and texture filtering together.", true);
        int preset = settings_.render_percent == 75 && settings_.aniso == 0      ? 0
                     : settings_.render_percent == 100 && settings_.aniso == 0   ? 1
                     : settings_.render_percent == 150 && settings_.aniso == 8   ? 2
                     : settings_.render_percent == 200 && settings_.aniso == 16  ? 3
                                                                                 : -1;
        if (Segmented<int>("preset", preset, {0, 1, 2, 3}, {"Low", "Original", "High", "Ultra"})) {
            static const uint32_t kScale[] = {75, 100, 150, 200}, kAniso[] = {0, 0, 8, 16};
            settings_.render_percent = kScale[preset];
            settings_.aniso = kAniso[preset];
        }
        EndSetting();
        EndCard();
        BeginCard("graphics");
        BeginSetting("Widescreen", "16:9 with more of the world at the sides, nothing stretched. The HUD, menus and "
                                   "movies stay 4:3 in the middle. Needs a 16:9 or wider resolution.", true);
        ToggleRight("wide", settings_.widescreen);
        EndSetting();
        BeginSetting("Render scale", "Of the output height. Above 100% supersamples for a sharper picture; below is "
                                     "faster on weak GPUs.");
        Segmented<uint32_t>("scale", settings_.render_percent, {50, 75, 100, 150, 200}, {"50%", "75%", "100%", "150%", "200%"});
        EndSetting();
        BeginSetting("Anisotropic filtering", "Sharper textures on floors and walls seen at an angle.");
        Segmented<uint32_t>("aniso", settings_.aniso, {0, 2, 4, 8, 16}, {"Off", "2x", "4x", "8x", "16x"});
        EndSetting();
        EndCard();
    }

    void GamePage() {
        PageTitle("Game", "In-game overlay and behaviour.");
        BeginCard("game");
        BeginSetting("FPS counter", "Shows the frame rate in the corner.", true);
        ToggleRight("fps", settings_.fps_counter);
        EndSetting();
        BeginSetting("Pause on focus loss", "Stops the game while another window is in front.");
        ToggleRight("pause", settings_.pause_on_focus_loss);
        EndSetting();
        BeginSetting("Settings menu key", "Opens the in-game settings. Pick another if F10 does nothing on your keyboard "
                                          "(laptops often need Fn for it). L3 + R3 on a controller always works.");
        static const std::vector<uint32_t> keys = {VK_F10, VK_F1, VK_F2, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9,
                                                   VK_F11, VK_F12, VK_INSERT, VK_HOME, VK_END, VK_PRIOR, VK_NEXT,
                                                   VK_PAUSE, VK_SCROLL, VK_OEM_3};
        static std::vector<std::string> names = [] {
            std::vector<std::string> n;
            for (uint32_t k : keys) n.push_back(KeyName(k));
            return n;
        }();
        Combo<uint32_t>("##key", settings_.menu_key, keys, names);
        EndSetting();
        EndCard();
        BeginCard("launcher");
        BeginSetting("Keep launcher open", "Leave this window open while the game runs.", true);
        ToggleRight("keep", settings_.keep_open);
        EndSetting();
        EndCard();
    }

    void ControlsPage() {
        PageTitle("Controls", "Xbox, PlayStation and Switch controllers play as player 1, with rumble. The keyboard "
                              "plays alongside.");
        BeginCard("keys");
        std::string menu = KeyName(settings_.menu_key);
        const char* kKeys[][2] = {
            {"W A S D", "Move"},  {"Arrows", "Camera"},     {"K / Space", "Jump (A)"}, {"J", "Attack (X)"},
            {"L", "B"},           {"I", "Y"},               {"Q / E", "Triggers"},     {"Z / C", "White / Black"},
            {"Enter", "Start"},   {"Backspace", "Back"},    {"Num 8 2 4 6", "D-pad"},  {menu.c_str(), "Settings menu"},
        };
        if (ImGui::BeginTable("keys", 4, ImGuiTableFlags_SizingStretchProp)) {
            for (auto& k : kKeys) {
                ImGui::TableNextColumn();
                Keycap(k[0]);
                ImGui::TableNextColumn();
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(3));
                ImGui::TextUnformatted(k[1]);
            }
            ImGui::EndTable();
        }
        EndCard();
    }

    void FilesPage() {
        PageTitle("Game files", "Your own copy of the game: the USA (NTSC-U) release, as a disc image or extracted.");
        BeginCard("status");
        if (files_.assets.empty()) {
            Status(kIconWarn, kRed, "No assets folder found");
        } else {
            switch (files_.xbe) {
                case Files::Xbe::Match: Status(kIconCheck, kGreen, "default.xbe  -  USA release, ready"); break;
                case Files::Xbe::Other: Status(kIconWarn, kRed, "default.xbe  -  another release, only the USA one works"); break;
                case Files::Xbe::Missing: Status(kIconWarn, kMuted, "default.xbe  -  not extracted"); break;
            }
            for (const fs::path& iso : files_.isos) Status(kIconDisc, kGreen, "Disc image  -  " + Utf8(iso.filename().wstring()));
        }
        if (!files_.exe) Status(kIconWarn, kRed, "Castlevania.exe is not beside the launcher");
        if (!files_.assets.empty() && files_.xbe == Files::Xbe::Missing && files_.isos.empty())
            Wrapped(g_small, kMuted, "Add your disc image (.iso), or extract the disc into the assets folder: default.xbe "
                                     "with its data, movie and sound folders.");
        if (!files_.assets.empty()) Wrapped(g_small, kMuted, Utf8(files_.assets.wstring()).c_str());
        ImGui::Dummy({0, S(6)});
        if (import_.running) {
            ImGui::ProgressBar(import_.progress, {-FLT_MIN, S(36)}, "Copying disc image...");
        } else {
            if (Button("Add disc image", {0, 0}, Kind::Primary, kIconAdd)) AddImage();
            ImGui::SameLine();
            if (Button("Open folder", {0, 0}, Kind::Secondary, kIconFolder)) {
                fs::path a = files_.assets.empty() ? ExeDir() / "assets" : files_.assets;
                std::error_code ec;
                fs::create_directories(a, ec);
                Open(a);
                rescan_ = true;
            }
            ImGui::SameLine();
            if (Button("Check again", {0, 0}, Kind::Secondary, kIconRefresh)) rescan_ = true;
        }
        EndCard();
        BeginCard("folders");
        Text(g_semi, kText, "Saves and logs");
        Wrapped(g_small, kMuted, "Saves are kept beside the game in hdd\\E\\UDATA\\4b4e002d.");
        ImGui::Dummy({0, S(4)});
        if (Button("Open saves", {0, 0}, Kind::Secondary, kIconSave)) {
            fs::path s = ExeDir() / kSaves;
            std::error_code ec;
            Open(fs::exists(s, ec) ? s : ExeDir());
        }
        ImGui::SameLine();
        if (Button("Open logs", {0, 0}, Kind::Secondary, kIconLog)) {
            std::error_code ec;
            Open(fs::exists(ExeDir() / "logs", ec) ? ExeDir() / "logs" : ExeDir());
        }
        EndCard();
    }

    bool Play() {
        saved_ = Snapshot();
        if (!settings_.Save()) {
            Note("Could not write the settings file");
            return true;
        }
        fs::path exe = ExeDir() / kGameExe;
        std::wstring cmd = L"\"" + exe.wstring() + L"\"";
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, ExeDir().c_str(), &si, &pi)) {
            Note("Could not start the game (error " + std::to_string(GetLastError()) + ")");
            return true;
        }
        // the game may bring its window to the front
        AllowSetForegroundWindow(pi.dwProcessId);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Note("Game started");
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
            Note("That image is already in the assets folder");
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
    std::string status_, saved_;
    double status_time_ = -100;
    bool rescan_ = false;
    int page_ = 0;
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
    RECT r{0, 0, LONG(1060 * scale), LONG(690 * scale)};
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
        const float clear[4] = {13 / 255.0f, 14 / 255.0f, 18 / 255.0f, 1};
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
