// First start: the game files, and in a release the game code.
//
// No game found: the assets folder is made beside the exe and opened, and the player is told what goes in it.
//
// A release (CV_SPLIT) ships no game code. Castlevania.exe holds the runtime; the game code is CastlevaniaGame.dll,
// built here on the player's PC from their own default.xbe with what the compiler folder holds: the recompiler,
// Clang, LLD, the headers the recompiled code includes, the DLL's side of the split (glue.obj) and import libraries
// for the two Windows runtime DLLs it calls. A DLL from another release, or none, is built again; it takes about a
// minute and shows its progress.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"
#include "kernel/crypto.h"
#include "kernel/disc.h"

extern const char kImageSha1[];  // the xbe this release was made for

namespace {

namespace fs = std::filesystem;

void Fail(const std::wstring& text) {
    MessageBoxW(nullptr, text.c_str(), app::kGame.short_title, MB_ICONWARNING | MB_SETFOREGROUND);
    ExitProcess(1);
}

// the runtime's search: assets beside the exe or in a folder above it
fs::path FindAssets() {
    fs::path dir = app::ExeDir();
    std::error_code ec;
    for (int i = 0; i < 6; i++) {
        if (fs::is_directory(dir / "assets", ec)) return dir / "assets";
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    return {};
}

std::vector<fs::path> Images(const fs::path& assets) {
    std::vector<fs::path> isos;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(assets, ec)) {
        std::wstring ext = e.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), towlower);
        if (ext == L".iso" && e.is_regular_file(ec)) isos.push_back(e.path());
    }
    std::sort(isos.begin(), isos.end());
    return isos;
}

// nothing to play: an assets folder to put the game in, opened for the player
void AskForGame() {
    fs::path assets = FindAssets();
    std::error_code ec;
    if (!assets.empty() && (fs::exists(assets / IMAGE_XBE, ec) || !Images(assets).empty())) return;
    if (assets.empty()) {
        assets = app::ExeDir() / "assets";
        fs::create_directories(assets, ec);
    }
    LOGR("setup: no game in %s, asking for it", assets.string().c_str());
    ShellExecuteW(nullptr, L"open", assets.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    Fail(L"Put your copy of Castlevania: Curse of Darkness (USA, original Xbox) in the assets folder that just "
         L"opened:\n\n  - its disc image (.iso), or\n  - the extracted disc: default.xbe with its data, movie and "
         L"sound folders.\n\nThen start the game again.");
}

#ifdef CV_SPLIT

std::string Sha1(const std::vector<uint8_t>& data) {
    alignas(8) uint8_t ctx[128];
    uint8_t digest[20];
    crypto::ShaInit(ctx);
    crypto::ShaUpdate(ctx, data.data(), data.size());
    crypto::ShaFinal(ctx, digest);
    std::string hex;
    for (uint8_t b : digest) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}

// the player's default.xbe: extracted, or out of the first disc image that has this release's
bool ReadXbe(std::vector<uint8_t>& out) {
    fs::path assets = FindAssets();
    std::ifstream f(assets / IMAGE_XBE, std::ios::binary);
    if (f) {
        out.assign(std::istreambuf_iterator<char>(f), {});
        if (Sha1(out) == kImageSha1) return true;
    }
    for (const fs::path& iso : Images(assets)) {
        kernel::Disc disc;
        std::string error;
        if (disc.Open(iso, error) && disc.Load(IMAGE_XBE, out) && Sha1(out) == kImageSha1) return true;
    }
    return false;
}

// the progress window, on a thread of its own while the build runs on this one
struct Progress {
    std::atomic<float> done{0};
    std::atomic<int> stage{0};
    std::atomic<bool> finished{false};
    HWND hwnd = nullptr;
    std::thread thread;

    static constexpr const wchar_t* kStages[] = {L"Reading your game", L"Translating the game code",
                                                 L"Compiling the game code", L"Linking the game", L"Done"};

    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Progress*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        switch (msg) {
            case WM_TIMER:
                if (self && self->finished) DestroyWindow(h);
                else InvalidateRect(h, nullptr, FALSE);
                return 0;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(h, &ps);
                RECT r;
                GetClientRect(h, &r);
                // drawn into a bitmap first, so it does not flicker
                HDC mem = CreateCompatibleDC(dc);
                HBITMAP bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
                HGDIOBJ old = SelectObject(mem, bmp);
                if (self) self->Paint(mem, r);
                BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
                DeleteObject(bmp);
                DeleteDC(mem);
                EndPaint(h, &ps);
                return 0;
            }
            case WM_CLOSE: return 0;  // the build finishes first
            case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    void Paint(HDC dc, const RECT& r) {
        float s = float(GetDpiForWindow(hwnd)) / 96.0f;
        auto px = [&](float v) { return int(v * s); };
        HBRUSH bg = CreateSolidBrush(RGB(18, 19, 24));
        FillRect(dc, &r, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        auto font = [&](int size, int weight) {
            return CreateFontW(-px(float(size)), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        };
        HFONT title = font(20, FW_SEMIBOLD), body = font(14, FW_NORMAL);
        HGDIOBJ old = SelectObject(dc, title);
        SetTextColor(dc, RGB(236, 236, 241));
        RECT t{px(28), px(24), r.right - px(28), px(56)};
        DrawTextW(dc, L"Setting up Castlevania: Curse of Darkness", -1, &t, DT_SINGLELINE);
        SelectObject(dc, body);
        SetTextColor(dc, RGB(140, 142, 154));
        RECT sub{px(28), px(56), r.right - px(28), px(80)};
        DrawTextW(dc, L"Building the game from your copy. This happens on the first start only.", -1, &sub, DT_SINGLELINE);
        // bar
        float f = std::clamp(done.load(), 0.0f, 1.0f);
        RECT track{px(28), px(104), r.right - px(28), px(112)};
        HBRUSH tb = CreateSolidBrush(RGB(42, 44, 53)), fb = CreateSolidBrush(RGB(224, 41, 74));
        FillRect(dc, &track, tb);
        RECT fill = track;
        fill.right = track.left + LONG(float(track.right - track.left) * f);
        FillRect(dc, &fill, fb);
        DeleteObject(tb);
        DeleteObject(fb);
        SetTextColor(dc, RGB(236, 236, 241));
        RECT st{px(28), px(124), r.right - px(28), px(148)};
        DrawTextW(dc, kStages[std::clamp(stage.load(), 0, 4)], -1, &st, DT_SINGLELINE);
        wchar_t pct[16];
        swprintf(pct, 16, L"%d%%", int(f * 100));
        DrawTextW(dc, pct, -1, &st, DT_SINGLELINE | DT_RIGHT);
        SelectObject(dc, old);
        DeleteObject(title);
        DeleteObject(body);
    }

    void Start() {
        thread = std::thread([this] {
            WNDCLASSW wc{};
            wc.lpfnWndProc = Proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
            wc.lpszClassName = L"CastlevaniaSetup";
            RegisterClassW(&wc);
            UINT dpi = GetDpiForSystem();
            int w = MulDiv(560, int(dpi), 96), h = MulDiv(170, int(dpi), 96);
            RECT r{0, 0, w, h};
            DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_MINIMIZEBOX;
            AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
            hwnd = CreateWindowExW(0, wc.lpszClassName, app::kGame.title, style,
                                   (GetSystemMetrics(SM_CXSCREEN) - (r.right - r.left)) / 2,
                                   (GetSystemMetrics(SM_CYSCREEN) - (r.bottom - r.top)) / 2, r.right - r.left,
                                   r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
            BOOL dark = TRUE;
            DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            SetTimer(hwnd, 1, 33, nullptr);
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        });
    }

    void Finish() {
        finished = true;
        if (thread.joinable()) thread.join();
    }
};

// child processes, their output appended to logs\setup.log
class Runner {
public:
    explicit Runner(const fs::path& log) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        log_ = CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    ~Runner() {
        if (log_ != INVALID_HANDLE_VALUE) CloseHandle(log_);
    }

    HANDLE Start(const fs::path& exe, const std::wstring& args, const fs::path& cwd) {
        std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;
        Note(cmd);
        STARTUPINFOW si{sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = log_;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                            nullptr, cwd.c_str(), &si, &pi))
            return nullptr;
        CloseHandle(pi.hThread);
        return pi.hProcess;
    }

    static bool Succeeded(HANDLE p) {
        DWORD code = 1;
        GetExitCodeProcess(p, &code);
        CloseHandle(p);
        return code == 0;
    }

    bool Run(const fs::path& exe, const std::wstring& args, const fs::path& cwd) {
        HANDLE p = Start(exe, args, cwd);
        if (!p) return false;
        WaitForSingleObject(p, INFINITE);
        return Succeeded(p);
    }

    void Note(const std::wstring& line) {
        std::string s(line.begin(), line.end());
        s = "> " + s + "\r\n";
        DWORD n;
        WriteFile(log_, s.data(), DWORD(s.size()), &n, nullptr);
    }

private:
    HANDLE log_ = INVALID_HANDLE_VALUE;
};

std::wstring Q(const fs::path& p) { return L"\"" + p.wstring() + L"\""; }

// builds the game DLL into dll; false after saying why
bool Build(const std::vector<uint8_t>& xbe, const fs::path& dll) {
    fs::path exe_dir = app::ExeDir(), cc = exe_dir / "compiler", work = cc / "work";
    fs::path log = exe_dir / "logs" / "setup.log";
    std::error_code ec;
    for (const char* need : {"xbe_recomp.exe", "clang.exe", "lld-link.exe", "glue.obj", "game.def", "game.toml"})
        if (!fs::exists(cc / need, ec)) {
            Fail(L"The compiler folder beside the game is incomplete (" + fs::path(need).wstring() +
                 L" is missing). Extract the whole release again.");
        }
    fs::create_directories(exe_dir / "logs", ec);
    Progress progress;
    progress.Start();
    Runner run(log);
    LOGR("setup: building %s", dll.string().c_str());

    // the recompiler: default.xbe and the config into C++
    fs::remove_all(work, ec);
    fs::create_directories(work / "config", ec);
    fs::create_directories(work / "assets", ec);
    fs::create_directories(work / "obj", ec);
    fs::copy_file(cc / "game.toml", work / "config" / "game.toml", ec);
    {
        std::ofstream f(work / "assets" / IMAGE_XBE, std::ios::binary);
        f.write(reinterpret_cast<const char*>(xbe.data()), std::streamsize(xbe.size()));
    }
    progress.stage = 1;
    progress.done = 0.02f;
    if (!run.Run(cc / "xbe_recomp.exe", L"config/game.toml --xdk " + Q(cc / "xdk"), work)) {
        progress.Finish();
        Fail(L"Translating the game code failed. See logs\\setup.log.");
    }

    // the compiler: every part at once, as many as the CPU has threads
    progress.stage = 2;
    progress.done = 0.05f;
    std::vector<fs::path> sources;
    for (auto& e : fs::directory_iterator(work / "generated" / "game", ec))
        if (e.path().extension() == ".cpp") sources.push_back(e.path());
    std::sort(sources.begin(), sources.end(), [](const fs::path& a, const fs::path& b) {
        std::error_code e;
        return fs::file_size(a, e) > fs::file_size(b, e);  // the big ones first, so none is left running alone
    });
    size_t jobs = std::clamp<size_t>(std::thread::hardware_concurrency(), 1, MAXIMUM_WAIT_OBJECTS);
    std::vector<HANDLE> running;
    size_t next = 0, finished = 0;
    bool ok = !sources.empty();
    std::wstring flags = L"--target=x86_64-pc-windows-msvc -nostdinc -isystem " + Q(cc / "include") + L" -I" +
                         Q(cc / "include") + L" -I" + Q(work / "generated" / "game") +
                         L" -std=gnu++20 -march=x86-64-v2 -O2 -fno-strict-aliasing -w -c ";
    while (ok && (next < sources.size() || !running.empty())) {
        while (next < sources.size() && running.size() < jobs) {
            const fs::path& src = sources[next++];
            fs::path obj = work / "obj" / src.filename().replace_extension(".obj");
            HANDLE p = run.Start(cc / "clang.exe", flags + Q(src) + L" -o " + Q(obj), work);
            if (!p) {
                ok = false;
                break;
            }
            running.push_back(p);
        }
        if (running.empty()) break;
        DWORD w = WaitForMultipleObjects(DWORD(running.size()), running.data(), FALSE, INFINITE);
        size_t i = w - WAIT_OBJECT_0;
        if (i >= running.size()) {
            ok = false;
            break;
        }
        ok &= Runner::Succeeded(running[i]);
        running.erase(running.begin() + ptrdiff_t(i));
        progress.done = 0.05f + 0.85f * float(++finished) / float(sources.size());
    }
    for (HANDLE p : running) {
        TerminateProcess(p, 1);
        CloseHandle(p);
    }
    if (!ok) {
        progress.Finish();
        Fail(L"Compiling the game code failed. See logs\\setup.log.");
    }

    // the linker: the parts, the DLL's side of the split and the runtime imports, nothing from Visual Studio
    progress.stage = 3;
    progress.done = 0.92f;
    {
        std::ofstream rsp(work / "objs.rsp");
        for (const fs::path& src : sources) rsp << '"' << (work / "obj" / src.filename().replace_extension(".obj")).string() << "\"\n";
    }
    fs::path tmp = dll;
    tmp += L".new";
    std::wstring link = L"-dll -noentry -nodefaultlib -noimplib -machine:x64 -def:" + Q(cc / "game.def") + L" -out:" + Q(tmp) +
                        L" @" + Q(work / "objs.rsp") + L" " + Q(cc / "glue.obj") + L" " + Q(cc / "vcruntime140.lib") +
                        L" " + Q(cc / "ucrtbase.lib");
    if (!run.Run(cc / "lld-link.exe", link, work)) {
        progress.Finish();
        Fail(L"Linking the game failed. See logs\\setup.log.");
    }
    fs::rename(tmp, dll, ec);
    if (ec) {
        progress.Finish();
        Fail(L"Could not write CastlevaniaGame.dll beside the game.");
    }
    fs::remove_all(work, ec);  // 300 MB of C++ and objects, made again if ever needed
    progress.stage = 4;
    progress.done = 1;
    Sleep(400);
    progress.Finish();
    LOGR("setup: built %s", dll.string().c_str());
    return true;
}

#endif  // CV_SPLIT

}  // namespace

#ifdef CV_SPLIT
bool CvWire(HMODULE dll);  // host_glue.cpp, written by split/glue.py
#endif

void WidescreenInit();  // widescreen.cpp

// GameInfo::init: settings loaded, nothing of the game touched yet
void GameInit() {
    AskForGame();
#ifdef CV_SPLIT
    fs::path dll = app::ExeDir() / "CastlevaniaGame.dll";
    std::error_code ec;
    HMODULE m = fs::exists(dll, ec) ? LoadLibraryW(dll.c_str()) : nullptr;
    if (!m || !CvWire(m)) {
        if (m) FreeLibrary(m);
        std::vector<uint8_t> xbe;
        if (!ReadXbe(xbe))
            Fail(L"The game in the assets folder is not the release this port is made for.\n\nIt needs Castlevania: "
                 L"Curse of Darkness for the original Xbox, USA (NTSC-U).");
        Build(xbe, dll);
        m = LoadLibraryW(dll.c_str());
        if (!m || !CvWire(m)) Fail(L"CastlevaniaGame.dll was built but does not load. See logs\\setup.log.");
    }
#endif
    WidescreenInit();
}
