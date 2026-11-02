// NBA Jam Mobile 0.2.3 – diagnostics: loader_log.txt + BUGREPORT.txt in LocalState

#include <windows.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>
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
static std::string g_report; // plain ASCII report body
static HANDLE g_file = INVALID_HANDLE_VALUE;
static HANDLE g_report_file = INVALID_HANDLE_VALUE;
static UI::Controls::TextBlock^ g_tb;
static Windows::UI::Core::CoreDispatcher^ g_disp;

static const wchar_t* kGameDir = L"D:\\DevelopmentFiles\\NBA Jam Mobile";
static const char* kAppVersion = "0.2.3";

static void WriteUtf8(HANDLE h, const char* s) {
    if (h == INVALID_HANDLE_VALUE || !s) return;
    DWORD w;
    WriteFile(h, s, (DWORD)strlen(s), &w, nullptr);
}

static void ReportA(const char* fmt, ...) {
    char buf[1024];
    va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    std::lock_guard<std::mutex> l(g_mu);
    g_report += buf;
    g_report += "\r\n";
    WriteUtf8(g_report_file, buf);
    WriteUtf8(g_report_file, "\r\n");
    if (g_report_file != INVALID_HANDLE_VALUE) FlushFileBuffers(g_report_file);
}

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

static void LogA(const char* line) {
    wchar_t w[900];
    size_t i = 0;
    for (; line[i] && i + 1 < 900; ++i) w[i] = (wchar_t)(unsigned char)line[i];
    w[i] = 0;
    Log(L"%s", w);
}

static void SoLogSink(const char* line) {
    LogA(line);
    ReportA("%s", line);
}

static bool PathExists(const wchar_t* path) {
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static bool FileExists(const wchar_t* path) {
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::string Narrow(const wchar_t* w) {
    std::string u;
    if (!w) return u;
    for (const wchar_t* p = w; *p; ++p)
        u += (*p < 128) ? (char)*p : '?';
    return u;
}

static bool ReadFileBytes(const wchar_t* path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFile2(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log(L"  CreateFile failed (%lu)", GetLastError());
        ReportA("ERROR CreateFile failed err=%lu path=%s", GetLastError(), Narrow(path).c_str());
        return false;
    }
    FILE_STANDARD_INFO info = {};
    if (!GetFileInformationByHandleEx(h, FileStandardInfo, &info, sizeof(info))) {
        ReportA("ERROR GetFileInformation failed err=%lu", GetLastError());
        CloseHandle(h);
        return false;
    }
    ULONGLONG sz = (ULONGLONG)info.EndOfFile.QuadPart;
    if (sz == 0 || sz > 256ull * 1024 * 1024) {
        ReportA("ERROR bad size %llu", sz);
        CloseHandle(h);
        return false;
    }
    out.resize((size_t)sz);
    size_t off = 0;
    while (off < out.size()) {
        DWORD n = 0;
        DWORD chunk = (DWORD)((out.size() - off > (1u << 20)) ? (1u << 20) : (out.size() - off));
        if (!ReadFile(h, out.data() + off, chunk, &n, nullptr) || n == 0) {
            ReportA("ERROR ReadFile failed at %zu err=%lu", off, GetLastError());
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

struct LoadResult {
    bool tried = false;
    bool ok = false;
    int rc = 0;
    size_t bytes = 0;
    bool arm64 = false;
    uintptr_t jni = 0;
    int unresolved_hint = -1;
};

static LoadResult g_load;

static void TryLoadSo(const wchar_t* soPath) {
    g_load.tried = true;
    Log(L"----------------------------------------");
    Log(L"Loading .so:");
    Log(L"  %s", soPath);
    ReportA("--- SO LOAD ---");
    ReportA("path=%s", Narrow(soPath).c_str());

    if (!FileExists(soPath)) {
        Log(L"  [ ] file missing");
        ReportA("RESULT=MISSING_FILE");
        return;
    }

    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(soPath, bytes)) {
        Log(L"  [ ] read failed");
        ReportA("RESULT=READ_FAILED");
        return;
    }
    g_load.bytes = bytes.size();
    Log(L"  read %zu bytes", bytes.size());
    ReportA("bytes=%zu", bytes.size());
    if (bytes.size() >= 20) {
        unsigned cls = bytes[4];
        unsigned mach = (unsigned)bytes[18] | ((unsigned)bytes[19] << 8);
        ReportA("elf_class=%u elf_machine=%u", cls, mach);
    }

    so_module mod;
    memset(&mod, 0, sizeof(mod));
    int rc = so_mem_load(&mod, bytes.data(), bytes.size(), 0);
    g_load.rc = rc;
    if (rc != 0) {
        Log(L"  so_mem_load failed (%d) — see [so] lines", rc);
        ReportA("RESULT=LOAD_FAILED rc=%d", rc);
        return;
    }
    g_load.ok = true;
    g_load.arm64 = mod.is_arm64 != 0;
    Log(L"  so_mem_load OK  arch=%s", mod.is_arm64 ? L"arm64" : L"arm32");
    ReportA("RESULT=LOAD_OK arch=%s text_base=0x%llx text_size=%zu",
            mod.is_arm64 ? "arm64" : "arm32",
            (unsigned long long)mod.text_base, mod.text_size);

    rc = so_relocate(&mod);
    Log(L"  so_relocate -> %d", rc);
    ReportA("relocate=%d", rc);

    rc = so_resolve(&mod, g_stubs, (int)sizeof(g_stubs), 0);
    Log(L"  so_resolve  -> %d", rc);
    ReportA("resolve=%d", rc);

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
    ReportA("--- SYMBOLS ---");
    for (int i = 0; try_syms[i]; ++i) {
        uintptr_t a = so_symbol(&mod, try_syms[i]);
        if (strcmp(try_syms[i], "JNI_OnLoad") == 0) g_load.jni = a;
        char line[200];
        if (a) snprintf(line, sizeof(line), "[x] %s = 0x%llx", try_syms[i], (unsigned long long)a);
        else   snprintf(line, sizeof(line), "[ ] %s", try_syms[i]);
        LogA(line);
        ReportA("%s", line);
    }

    so_initialize(&mod);
    so_unload(&mod);
    Log(L"  unloaded");
    ReportA("unloaded=1");
    Log(L"----------------------------------------");
}

static void ProbeGameFolder(const wchar_t* dir) {
    Log(L"Checking: %s", dir);
    ReportA("--- FOLDER ---");
    ReportA("check=%s", Narrow(dir).c_str());
    if (!PathExists(dir)) {
        Log(L"  [ ] folder not found");
        ReportA("folder=missing");
        return;
    }
    Log(L"  [x] folder exists");
    ReportA("folder=ok");

    std::wstring base = dir;
    if (!base.empty() && base.back() != L'\\') base += L'\\';
    std::wstring soPath = base + L"libnbajam.so";
    bool hasSo = FileExists(soPath.c_str());
    Log(L"  [%c] libnbajam.so", hasSo ? L'x' : L' ');
    ReportA("libnbajam.so=%s", hasSo ? "yes" : "no");

    std::wstring pattern = base + L"*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    bool hasObb = false, hasApk = false;
    ReportA("--- FILES ---");
    if (h != INVALID_HANDLE_VALUE) {
        int shown = 0;
        do {
            if (fd.cFileName[0] == L'.') continue;
            if (shown == 0) Log(L"  Files in folder:");
            Log(L"    - %s", fd.cFileName);
            ReportA("file=%s", Narrow(fd.cFileName).c_str());
            std::wstring name = fd.cFileName;
            for (auto& c : name) if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
            if (name.size() >= 4 && name.substr(name.size() - 4) == L".obb") hasObb = true;
            if (name.size() >= 4 && name.substr(name.size() - 4) == L".apk") hasApk = true;
            if (++shown >= 40) { Log(L"    ..."); ReportA("file=..."); break; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    Log(L"  [%c] .obb  [%c] .apk", hasObb ? L'x' : L' ', hasApk ? L'x' : L' ');
    ReportA("has_obb=%s has_apk=%s", hasObb ? "yes" : "no", hasApk ? "yes" : "no");

    if (hasSo) TryLoadSo(soPath.c_str());
    else {
        Log(L"  Skip load — no libnbajam.so");
        ReportA("RESULT=SKIP_NO_SO");
    }
}

static void WriteReportFooter() {
    ReportA("--- SUMMARY ---");
    ReportA("load_tried=%s", g_load.tried ? "yes" : "no");
    ReportA("load_ok=%s", g_load.ok ? "yes" : "no");
    ReportA("load_rc=%d", g_load.rc);
    ReportA("so_bytes=%zu", g_load.bytes);
    ReportA("arch=%s", g_load.ok ? (g_load.arm64 ? "arm64" : "arm32") : "n/a");
    ReportA("JNI_OnLoad=0x%llx", (unsigned long long)g_load.jni);
    ReportA("mem_limit_mb=%llu", Windows::System::MemoryManager::AppMemoryUsageLimit >> 20);
    ReportA("mem_used_mb=%llu", Windows::System::MemoryManager::AppMemoryUsage >> 20);
    ReportA("--- END BUGREPORT ---");
    ReportA("Paste this whole file to the developer (Grok / GitHub issue).");
}

static void RunProbe() {
    so_set_log(SoLogSink);

    // Timestamp
    time_t now = time(nullptr);
    char tbuf[64];
    struct tm tmv;
    gmtime_s(&tmv, &now);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S UTC", &tmv);

    ReportA("=== NBA JAM MOBILE BUGREPORT ===");
    ReportA("app_version=%s", kAppVersion);
    ReportA("build_time=%s", tbuf);
    ReportA("platform=XboxDevMode/UWP");

    Log(L"========================================");
    Log(L"  NBA Jam Mobile %hs", kAppVersion);
    Log(L"  diagnostics + bug report");
    Log(L"========================================");

    std::wstring local =
        std::wstring(Windows::Storage::ApplicationData::Current->LocalFolder->Path->Data());
    Log(L"LocalState:");
    Log(L"  %s", local.c_str());
    Log(L"Send these files:");
    Log(L"  BUGREPORT.txt   <--- paste this to Grok");
    Log(L"  loader_log.txt  (full log)");
    Log(L"Device Portal → File explorer → LocalState");
    Log(L"");

    ReportA("localstate=%s", Narrow(local.c_str()).c_str());

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
        Log(L"No game folder found: %s", kGameDir);
        ReportA("folder=NONE");
    }

    WriteReportFooter();

    Log(L"Mem limit=%llu MB  used=%llu MB",
        Windows::System::MemoryManager::AppMemoryUsageLimit >> 20,
        Windows::System::MemoryManager::AppMemoryUsage >> 20);
    Log(L"");
    Log(L"== DONE ==");
    Log(L"Upload BUGREPORT.txt from LocalState");
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
            tb->Text = L"NBA Jam Mobile — writing bug report...";

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

            std::wstring base =
                std::wstring(Windows::Storage::ApplicationData::Current->LocalFolder->Path->Data());
            std::wstring logPath = base + L"\\loader_log.txt";
            std::wstring reportPath = base + L"\\BUGREPORT.txt";

            g_file = CreateFile2(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, nullptr);
            g_report_file = CreateFile2(reportPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, nullptr);

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
