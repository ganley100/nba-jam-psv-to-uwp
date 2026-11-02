// NBA Jam Mobile – UWP shell for Xbox Dev Mode
// Loads libnbajam.so from D:\DevelopmentFiles\NBA Jam Mobile (ARM32 + ARM64)

#include <windows.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "so_util_win.h"
}

namespace UI = Windows::UI::Xaml;

static std::mutex g_mu;
static std::wstring g_log;
static HANDLE g_file = INVALID_HANDLE_VALUE;
static UI::Controls::TextBlock^ g_tb;
static Windows::UI::Core::CoreDispatcher^ g_disp;

static const wchar_t* kGameDir = L"D:\\DevelopmentFiles\\NBA Jam Mobile";

static void Log(const wchar_t* fmt, ...) {
    wchar_t buf[900];
    va_list a; va_start(a, fmt); _vsnwprintf_s(buf, _TRUNCATE, fmt, a); va_end(a);
    std::wstring text;
    {
        std::lock_guard<std::mutex> l(g_mu);
        g_log += buf;
        g_log += L"\r\n";
        if (g_file != INVALID_HANDLE_VALUE) {
            std::string u;
            for (const wchar_t* p = buf; *p; ++p)
                u += (*p < 128) ? (char)*p : '?';
            u += "\r\n";
            DWORD w; WriteFile(g_file, u.data(), (DWORD)u.size(), &w, nullptr);
            FlushFileBuffers(g_file);
        }
        text = g_log;
    }
    if (g_disp && g_tb) {
        auto s = ref new Platform::String(text.c_str());
        g_disp->RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal,
            ref new Windows::UI::Core::DispatchedHandler([s]() { g_tb->Text = s; }));
    }
}

static void LogA(const char* fmt, ...) {
    char buf[900];
    va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    wchar_t w[900];
    size_t i = 0;
    for (; buf[i] && i + 1 < 900; ++i) w[i] = (wchar_t)(unsigned char)buf[i];
    w[i] = 0;
    Log(L"%s", w);
}

static bool PathExists(const wchar_t* path) {
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static bool FileExists(const wchar_t* path) {
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool ReadFileBytes(const wchar_t* path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFile2(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log(L"  CreateFile failed (%lu)", GetLastError());
        return false;
    }
    FILE_STANDARD_INFO info = {};
    if (!GetFileInformationByHandleEx(h, FileStandardInfo, &info, sizeof(info))) {
        CloseHandle(h);
        return false;
    }
    ULONGLONG sz = (ULONGLONG)info.EndOfFile.QuadPart;
    if (sz == 0 || sz > 256ull * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    out.resize((size_t)sz);
    size_t off = 0;
    while (off < out.size()) {
        DWORD n = 0;
        DWORD chunk = (DWORD)((out.size() - off > (1u << 20)) ? (1u << 20) : (out.size() - off));
        if (!ReadFile(h, out.data() + off, chunk, &n, nullptr) || n == 0) {
            CloseHandle(h);
            return false;
        }
        off += n;
    }
    CloseHandle(h);
    return true;
}

static int stub_ret0(void) { return 0; }
static void* stub_malloc(size_t n) { return malloc(n ? n : 1); }
static void  stub_free(void* p) { free(p); }
static void* stub_memcpy(void* d, const void* s, size_t n) { return memcpy(d, s, n); }
static void* stub_memset(void* d, int c, size_t n) { return memset(d, c, n); }
static int   stub_strcmp(const char* a, const char* b) { return strcmp(a, b); }
static size_t stub_strlen(const char* s) { return strlen(s); }

static so_default_dynlib g_stubs[] = {
    { "malloc",  (uintptr_t)stub_malloc },
    { "free",    (uintptr_t)stub_free },
    { "memcpy",  (uintptr_t)stub_memcpy },
    { "memset",  (uintptr_t)stub_memset },
    { "strcmp",  (uintptr_t)stub_strcmp },
    { "strlen",  (uintptr_t)stub_strlen },
    { "__errno", (uintptr_t)stub_ret0 },
};

static void TryLoadSo(const wchar_t* soPath) {
    Log(L"----------------------------------------");
    Log(L"Loading .so:");
    Log(L"  %s", soPath);

    if (!FileExists(soPath)) {
        Log(L"  [ ] file missing");
        return;
    }

    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(soPath, bytes)) {
        Log(L"  [ ] read failed");
        return;
    }
    Log(L"  read %zu bytes", bytes.size());

    if (bytes.size() >= 20) {
        Log(L"  ELF class=%u machine=%u",
            bytes[4],
            (unsigned)bytes[18] | ((unsigned)bytes[19] << 8));
    }

    so_module mod;
    memset(&mod, 0, sizeof(mod));

    int rc = so_mem_load(&mod, bytes.data(), bytes.size(), 0);
    if (rc != 0) {
        Log(L"  so_mem_load failed (%d)", rc);
        return;
    }
    Log(L"  so_mem_load OK  arch=%s", mod.is_arm64 ? L"arm64" : L"arm32");

    if (mod.soname)
        LogA("  soname: %s", mod.soname);

    Log(L"  text_base=0x%llx  text_size=%zu",
        (unsigned long long)mod.text_base, mod.text_size);
    Log(L"  dynsym=%d  rela/rel=%d+%d  init_array=%d",
        mod.num_dynsym, mod.num_reldyn, mod.num_relplt, mod.num_init_array);

    rc = so_relocate(&mod);
    Log(L"  so_relocate -> %d", rc);

    rc = so_resolve(&mod, g_stubs, (int)sizeof(g_stubs), 0);
    Log(L"  so_resolve  -> %d", rc);

    const char* try_syms[] = {
        "JNI_OnLoad",
        "JNI_OnUnload",
        "Java_com_eamobile_nbajam_NBAJamActivity_nativeOnCreate",
        "Java_com_eamobile_nbajam_NBAJamActivity_nativeOnResume",
        "Java_com_eamobile_nbajam_NBAJamActivity_nativeOnPause",
        "Java_com_eamobile_nbajam_NBAJamActivity_nativeKeyDown",
        "Java_com_eamobile_nbajam_NBAJamActivity_nativeKeyUp",
        "Java_com_eamobile_nbajam_NBAJamActivity_nativeTouchEvent",
        "ANativeActivity_onCreate",
        nullptr
    };

    Log(L"  Symbols:");
    for (int i = 0; try_syms[i]; ++i) {
        uintptr_t a = so_symbol(&mod, try_syms[i]);
        if (a)
            LogA("    [x] %s = 0x%llx", try_syms[i], (unsigned long long)a);
        else
            LogA("    [ ] %s", try_syms[i]);
    }

    Log(L"  init_array not executed (no dynarec)");
    so_initialize(&mod);
    so_unload(&mod);
    Log(L"  unloaded");
    Log(L"----------------------------------------");
}

static void ProbeGameFolder(const wchar_t* dir) {
    Log(L"Checking: %s", dir);
    if (!PathExists(dir)) {
        Log(L"  [ ] folder not found");
        return;
    }
    Log(L"  [x] folder exists");

    std::wstring base = dir;
    if (!base.empty() && base.back() != L'\\') base += L'\\';

    std::wstring soPath = base + L"libnbajam.so";
    bool hasSo = FileExists(soPath.c_str());
    Log(L"  [%c] libnbajam.so", hasSo ? L'x' : L' ');

    std::wstring pattern = base + L"*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    bool hasObb = false, hasApk = false;
    if (h != INVALID_HANDLE_VALUE) {
        int shown = 0;
        do {
            if (fd.cFileName[0] == L'.') continue;
            if (shown == 0) Log(L"  Files in folder:");
            Log(L"    - %s", fd.cFileName);
            std::wstring name = fd.cFileName;
            for (auto& c : name) if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
            if (name.size() >= 4 && name.substr(name.size() - 4) == L".obb") hasObb = true;
            if (name.size() >= 4 && name.substr(name.size() - 4) == L".apk") hasApk = true;
            if (++shown >= 30) { Log(L"    ..."); break; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    Log(L"  [%c] .obb present", hasObb ? L'x' : L' ');
    Log(L"  [%c] .apk present", hasApk ? L'x' : L' ');

    if (hasSo)
        TryLoadSo(soPath.c_str());
    else
        Log(L"  Skip load — no libnbajam.so");
}

static void RunProbe() {
    Log(L"========================================");
    Log(L"  NBA Jam Mobile 0.2.1");
    Log(L"  ARM32 + ARM64 loader");
    Log(L"========================================");
    Log(L"");
    Log(L"Game folder: %s", kGameDir);
    Log(L"");

    const wchar_t* candidates[] = {
        L"D:\\DevelopmentFiles\\NBA Jam Mobile",
        L"D:\\DevelopmentFiles\\NBAJamMobile",
        L"D:\\NBA Jam Mobile",
        L"E:\\DevelopmentFiles\\NBA Jam Mobile",
    };

    bool any = false;
    for (const wchar_t* c : candidates) {
        if (PathExists(c)) {
            any = true;
            ProbeGameFolder(c);
            Log(L"");
        }
    }
    if (!any) {
        Log(L"No game folder found.");
        Log(L"Create: %s", kGameDir);
    }

    try {
        auto local = Windows::Storage::ApplicationData::Current->LocalFolder->Path;
        Log(L"LocalState: %s", local->Data());
    } catch (...) {}

    Log(L"");
    Log(L"Mem limit=%llu MB  used=%llu MB",
        Windows::System::MemoryManager::AppMemoryUsageLimit >> 20,
        Windows::System::MemoryManager::AppMemoryUsage >> 20);
    Log(L"");
    Log(L"== DONE ==");
    Log(L"ARM code is NOT executed yet (needs dynarec).");
}

namespace NbaJamLoader {
    ref class App sealed : public UI::Application {
    protected:
        virtual void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^ e) override {
            auto tb = ref new UI::Controls::TextBlock();
            tb->FontFamily = ref new UI::Media::FontFamily(L"Consolas");
            tb->FontSize = 18;
            tb->TextWrapping = UI::TextWrapping::Wrap;
            tb->Margin = UI::ThicknessHelper::FromUniformLength(28);
            tb->Foreground = ref new UI::Media::SolidColorBrush(Windows::UI::Colors::White);
            tb->Text = L"NBA Jam Mobile — loading...";

            auto sv = ref new UI::Controls::ScrollViewer();
            sv->Content = tb;
            sv->IsTabStop = true;

            auto grid = ref new UI::Controls::Grid();
            grid->Background = ref new UI::Media::SolidColorBrush(
                Windows::UI::ColorHelper::FromArgb(255, 0, 0, 0));
            grid->Children->Append(sv);

            UI::Window::Current->Content = grid;
            UI::Window::Current->Activate();
            sv->Focus(UI::FocusState::Programmatic);

            g_tb = tb;
            g_disp = UI::Window::Current->Dispatcher;

            std::wstring path =
                std::wstring(Windows::Storage::ApplicationData::Current->LocalFolder->Path->Data()) +
                L"\\loader_log.txt";
            g_file = CreateFile2(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, nullptr);

            std::thread(RunProbe).detach();
        }
    };
}

int main(Platform::Array<Platform::String^>^) {
    UI::Application::Start(ref new UI::ApplicationInitializationCallback(
        [](UI::ApplicationInitializationCallbackParams^) {
            ref new NbaJamLoader::App();
        }));
    return 0;
}
