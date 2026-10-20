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
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
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

ImFont* g_title = nullptr;
ImFont* g_subtitle = nullptr;
const ImVec4 kGold{0.95f, 0.80f, 0.45f, 1};
const ImVec4 kGood{0.55f, 0.85f, 0.45f, 1};
const ImVec4 kBad{0.95f, 0.45f, 0.40f, 1};

void Style(float scale) {
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 0;
    st.FrameRounding = 4;
    st.TabRounding = 4;
    st.ChildRounding = 6;
    st.WindowPadding = {18, 16};
    st.FramePadding = {10, 6};
    st.ItemSpacing = {10, 9};
    ImVec4* c = st.Colors;
    ImVec4 red{0.55f, 0.08f, 0.10f, 1}, red_hi{0.72f, 0.12f, 0.14f, 1}, red_lo{0.40f, 0.06f, 0.08f, 1};
    c[ImGuiCol_WindowBg] = {0.05f, 0.04f, 0.05f, 1};
    c[ImGuiCol_ChildBg] = {0.09f, 0.07f, 0.08f, 1};
    c[ImGuiCol_PopupBg] = {0.09f, 0.07f, 0.08f, 0.98f};
    c[ImGuiCol_Border] = {0.30f, 0.12f, 0.14f, 1};
    c[ImGuiCol_Tab] = red_lo;
    c[ImGuiCol_TabHovered] = red_hi;
    c[ImGuiCol_TabSelected] = red;
    c[ImGuiCol_Button] = red_lo;
    c[ImGuiCol_ButtonHovered] = red_hi;
    c[ImGuiCol_ButtonActive] = red;
    c[ImGuiCol_FrameBg] = {0.16f, 0.13f, 0.14f, 1};
    c[ImGuiCol_FrameBgHovered] = {0.26f, 0.16f, 0.18f, 1};
    c[ImGuiCol_FrameBgActive] = {0.32f, 0.18f, 0.20f, 1};
    c[ImGuiCol_Header] = red_lo;
    c[ImGuiCol_HeaderHovered] = red_hi;
    c[ImGuiCol_HeaderActive] = red;
    c[ImGuiCol_CheckMark] = kGold;
    c[ImGuiCol_PlotHistogram] = red_hi;
    c[ImGuiCol_NavHighlight] = kGold;
    c[ImGuiCol_Separator] = {0.30f, 0.12f, 0.14f, 1};
    st.ScaleAllSizes(scale);
}

void Fonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    const char* ui = "C:\\Windows\\Fonts\\segoeui.ttf";
    const char* serif = "C:\\Windows\\Fonts\\georgiab.ttf";
    std::error_code ec;
    if (fs::exists(ui, ec)) io.Fonts->AddFontFromFileTTF(ui, 18.0f * scale);
    else io.Fonts->AddFontDefault();
    if (fs::exists(serif, ec)) {
        g_title = io.Fonts->AddFontFromFileTTF(serif, 40.0f * scale);
        g_subtitle = io.Fonts->AddFontFromFileTTF(serif, 22.0f * scale);
    }
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
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("launcher", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        Header();
        float footer = ImGui::GetFrameHeight() * 1.6f + ImGui::GetStyle().ItemSpacing.y * 2 + ImGui::GetTextLineHeightWithSpacing();
        float left = ImGui::GetContentRegionAvail().x * 0.36f;
        ImGui::BeginChild("files", {left, -footer}, ImGuiChildFlags_Border);
        GamePanel();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("settings", {0, -footer}, ImGuiChildFlags_Border);
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Display")) DisplayTab(), ImGui::EndTabItem();
            if (ImGui::BeginTabItem("Graphics")) GraphicsTab(), ImGui::EndTabItem();
            if (ImGui::BeginTabItem("Game")) GameTab(), ImGui::EndTabItem();
            if (ImGui::BeginTabItem("Controls")) ControlsTab(), ImGui::EndTabItem();
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
        bool keep = Footer();
        ImGui::End();
        return keep;
    }

private:
    void Header() {
        if (g_title) ImGui::PushFont(g_title);
        ImGui::TextColored(kGold, "Castlevania");
        if (g_title) ImGui::PopFont();
        ImGui::SameLine();
        if (g_subtitle) ImGui::PushFont(g_subtitle);
        float drop = (g_title ? g_title->FontSize : 0) - (g_subtitle ? g_subtitle->FontSize : 0);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + drop * 0.62f);
        ImGui::TextColored({0.80f, 0.20f, 0.22f, 1}, "Curse of Darkness");
        if (g_subtitle) ImGui::PopFont();
        ImGui::TextDisabled("Native PC port of the original Xbox version");
        ImGui::Spacing();
    }

    void GamePanel() {
        ImGui::TextColored(kGold, "Game files");
        ImGui::Separator();
        if (files_.assets.empty()) {
            ImGui::TextColored(kBad, "No assets folder found.");
            ImGui::TextWrapped("Make an assets folder beside the game and put your game files in it.");
        } else {
            switch (files_.xbe) {
                case Files::Xbe::Match: ImGui::TextColored(kGood, "default.xbe: USA release, ready"); break;
                case Files::Xbe::Other:
                    ImGui::TextColored(kBad, "default.xbe: another release");
                    ImGui::TextWrapped("Only the USA (NTSC-U) release works with this build.");
                    break;
                case Files::Xbe::Missing: ImGui::TextDisabled("default.xbe: not extracted"); break;
            }
            for (const fs::path& iso : files_.isos) ImGui::TextColored(kGood, "Disc image: %s", Utf8(iso.filename().wstring()).c_str());
            if (files_.xbe == Files::Xbe::Missing && files_.isos.empty())
                ImGui::TextWrapped("Add your disc image (.iso), or extract the disc into the assets folder: default.xbe "
                                   "with its data, movie and sound folders.");
        }
        if (!files_.exe) ImGui::TextColored(kBad, "Castlevania.exe is not beside the launcher.");
        ImGui::Spacing();
        if (import_.running) {
            ImGui::ProgressBar(import_.progress, {-FLT_MIN, 0}, "Copying disc image...");
        } else {
            if (ImGui::Button("Add disc image...", {-FLT_MIN, 0})) AddImage();
        }
        if (ImGui::Button("Open assets folder", {-FLT_MIN, 0})) {
            fs::path a = files_.assets.empty() ? ExeDir() / "assets" : files_.assets;
            std::error_code ec;
            fs::create_directories(a, ec);
            Open(a);
            rescan_ = true;
        }
        if (ImGui::Button("Check again", {-FLT_MIN, 0})) rescan_ = true;

        ImGui::Spacing();
        ImGui::TextColored(kGold, "Folders");
        ImGui::Separator();
        if (ImGui::Button("Saves", {-FLT_MIN, 0})) {
            fs::path s = ExeDir() / kSaves;
            std::error_code ec;
            Open(fs::exists(s, ec) ? s : ExeDir());
        }
        if (ImGui::Button("Logs", {-FLT_MIN, 0})) {
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
        ImGui::Spacing();
        ImGui::TextDisabled("Quick presets");
        auto preset = [&](const char* name, uint32_t scale, uint32_t aniso) {
            if (ImGui::Button(name)) {
                settings_.render_percent = scale;
                settings_.aniso = aniso;
            }
            ImGui::SameLine();
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
        ImGui::TextWrapped("Xbox, PlayStation and Switch controllers work as player 1, with rumble. The keyboard plays "
                           "alongside:");
        static const char* kKeys[][2] = {
            {"W A S D", "Move"},         {"Arrow keys", "Camera"},           {"K / Space", "A (jump)"},
            {"J", "X (attack)"},         {"L", "B"},                         {"I", "Y"},
            {"Q / E", "Left / right trigger"}, {"Z / C", "White / black"},   {"Enter", "Start"},
            {"Backspace", "Back"},       {"Numpad 8 2 4 6", "D-pad"},        {"L3 + R3", "Settings menu"},
        };
        if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(KeyName(settings_.menu_key).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Settings menu");
            for (auto& k : kKeys) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(k[0]);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(k[1]);
            }
            ImGui::EndTable();
        }
    }

    bool Footer() {
        if (!status_.empty()) ImGui::TextDisabled("%s", status_.c_str());
        else ImGui::TextDisabled("Settings are saved to %s and can also be changed in game.", kSettings);
        float h = ImGui::GetFrameHeight() * 1.6f;
        if (ImGui::Button("Reset to defaults", {0, h})) {
            settings_.Defaults();
            status_ = "Defaults restored; Save or Play keeps them.";
        }
        ImGui::SameLine();
        if (ImGui::Button("Save", {0, h})) status_ = settings_.Save() ? "Settings saved." : "Could not write the settings file.";
        ImGui::SameLine();
        float play_w = ImGui::GetFontSize() * 9;
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - play_w);
        bool can = files_.Playable() && !import_.running;
        ImGui::BeginDisabled(!can);
        ImGui::PushStyleColor(ImGuiCol_Button, {0.62f, 0.10f, 0.12f, 1});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.80f, 0.16f, 0.18f, 1});
        bool play = ImGui::Button("PLAY", {play_w, h});
        ImGui::PopStyleColor(2);
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
        status_ = "Game started.";
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
    RECT r{0, 0, LONG(960 * scale), LONG(600 * scale)};
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
    Style(scale);
    Fonts(scale);
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
        const float clear[4] = {0.05f, 0.04f, 0.05f, 1};
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
