#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#include <shlobj.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "dwmapi.lib")

namespace {

constexpr int IDI_ARZ_ICON   = 101;
constexpr int IDB_ARZ_BANNER = 102;

constexpr int IDC_BTN_LANG       = 1001;
constexpr int IDC_CHK_HW_DECODE  = 1002;
constexpr int IDC_CHK_GPU_COLOR  = 1003;
constexpr int IDC_CHK_OVERRIDE   = 1004;
constexpr int IDC_CHK_CACHE      = 1005;
constexpr int IDC_BTN_SAVE       = 1006;
constexpr int IDC_BTN_CLEARCACHE = 1007;
constexpr int IDC_EDIT_TELEMETRY = 1008;
constexpr int IDT_TELEMETRY_POLL = 2001;

// ARZ Brand Palette (#0B0B0D Dark Obsidian, #16161A Card, #F3ECE0 Cream, #DA9B40 Gold)
constexpr COLORREF kColBg     = RGB(11, 11, 13);
constexpr COLORREF kColCard   = RGB(22, 22, 26);
constexpr COLORREF kColCream  = RGB(243, 236, 224);
constexpr COLORREF kColGold   = RGB(218, 155, 64);

HBRUSH g_bgBrush = nullptr;
HBRUSH g_cardBrush = nullptr;
HFONT  g_titleFont = nullptr;
HFONT  g_uiFont = nullptr;
HFONT  g_monoFont = nullptr;
HBITMAP g_bannerBmp = nullptr;
bool g_isArabic = true;

HWND g_btnLang = nullptr;
HWND g_chkHwDecode = nullptr;
HWND g_chkGpuColor = nullptr;
HWND g_chkOverride = nullptr;
HWND g_chkCache = nullptr;
HWND g_btnSave = nullptr;
HWND g_btnClearCache = nullptr;
HWND g_lblSub = nullptr;
HWND g_lblTeleHeader = nullptr;
HWND g_editTele = nullptr;

struct UIState {
    bool hwDecode = true;
    bool gpuColor = true;
    bool selectiveOverride = true;
    bool frameCache = true;
};
UIState g_state;

std::filesystem::path getConfigPath() {
    wchar_t* appData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appData)) && appData) {
        std::filesystem::path p = std::filesystem::path(appData) / L"ARZ";
        CoTaskMemFree(appData);
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        return p / L"ARZConfig.ini";
    }
    return L"ARZConfig.ini";
}

std::filesystem::path getStatusDir() {
    wchar_t* appData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appData)) && appData) {
        std::filesystem::path p = std::filesystem::path(appData) / L"ARZ" / L"Status";
        CoTaskMemFree(appData);
        return p;
    }
    return {};
}

void loadConfigState() {
    std::ifstream in(getConfigPath());
    if (!in.is_open()) return;
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        const bool b = (v.find('1') != std::string::npos || v.find("true") != std::string::npos);
        if (k.find("enableHardwareDecode") != std::string::npos) g_state.hwDecode = b;
        else if (k.find("enableGpuColorConvert") != std::string::npos) g_state.gpuColor = b;
        else if (k.find("selectiveOverride") != std::string::npos) g_state.selectiveOverride = b;
        else if (k.find("enableFrameCache") != std::string::npos) g_state.frameCache = b;
    }
}

bool saveConfigState() {
    std::ofstream out(getConfigPath(), std::ios::trunc);
    if (!out.is_open()) return false;
    out << "# ARZ Media Suite v1.2.0 Configuration\n"
        << "enableHardwareDecode=" << (g_state.hwDecode ? "true" : "false") << "\n"
        << "enableGpuColorConvert=" << (g_state.gpuColor ? "true" : "false") << "\n"
        << "selectiveOverride=" << (g_state.selectiveOverride ? "true" : "false") << "\n"
        << "enableFrameCache=" << (g_state.frameCache ? "true" : "false") << "\n"
        << "maxPercentVramBudget=78\n"
        << "maxVramCacheMB=384\n"
        << "maxRamCacheMB=512\n";
    return true;
}

int clearAdobePluginCaches() {
    int cleared = 0;
    const wchar_t* roots[] = {
        L"Software\\Adobe\\Premiere Pro",
        L"Software\\Adobe\\Adobe Media Encoder",
        L"Software\\Adobe\\After Effects"
    };
    for (const wchar_t* r : roots) {
        HKEY hRoot = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, r, 0, KEY_READ | KEY_WRITE, &hRoot) == ERROR_SUCCESS) {
            wchar_t subName[256]{};
            DWORD idx = 0;
            std::vector<std::wstring> versions;
            while (true) {
                DWORD sz = 256;
                if (RegEnumKeyExW(hRoot, idx++, subName, &sz, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
                    break;
                }
                versions.emplace_back(subName);
            }
            for (const auto& ver : versions) {
                HKEY hVer = nullptr;
                if (RegOpenKeyExW(hRoot, ver.c_str(), 0, KEY_READ | KEY_WRITE, &hVer) == ERROR_SUCCESS) {
                    DWORD kIdx = 0;
                    std::vector<std::wstring> cacheKeys;
                    while (true) {
                        DWORD sz = 256;
                        if (RegEnumKeyExW(hVer, kIdx++, subName, &sz, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
                            break;
                        }
                        std::wstring ks(subName);
                        if (ks.find(L"PluginCache") != std::wstring::npos) {
                            cacheKeys.push_back(ks);
                        }
                    }
                    for (const auto& ck : cacheKeys) {
                        if (RegDeleteTreeW(hVer, ck.c_str()) == ERROR_SUCCESS) {
                            ++cleared;
                        }
                    }
                    RegCloseKey(hVer);
                }
            }
            RegCloseKey(hRoot);
        }
    }
    return cleared;
}

std::wstring readLatestTelemetryText() {
    const auto dir = getStatusDir();
    std::error_code ec;
    if (dir.empty() || !std::filesystem::exists(dir, ec)) {
        return g_isArabic
                   ? L"لا توجد جلسة بريمير نشطة حالياً — افتح فيديو عبر ARZ داخل Premiere Pro 27 لمشاهدة حالة الـ GPU والـ VRAM لحظياً."
                   : L"No active Premiere Pro session yet — import/play a clip via ARZ in Premiere Pro 27 to monitor live RTX GPU & VRAM usage.";
    }
    std::filesystem::path newest;
    std::filesystem::file_time_type newestTime{};
    bool found = false;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().extension() == L".json") {
            const auto wt = e.last_write_time(ec);
            if (!found || wt > newestTime) {
                newest = e.path();
                newestTime = wt;
                found = true;
            }
        }
    }
    if (!found) {
        return g_isArabic ? L"في انتظار فتح أول ملف داخل بريمير..." : L"Waiting for active ARZ media session...";
    }
    std::ifstream in(newest, std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (raw.empty()) return {};

    // Normalize LF -> CRLF for Win32 Edit control
    std::string crlf;
    crlf.reserve(raw.size() + 64);
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\n' && (i == 0 || raw[i - 1] != '\r')) crlf.push_back('\r');
        crlf.push_back(raw[i]);
    }
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, crlf.c_str(), -1, nullptr, 0);
    if (wlen <= 1) return {};
    std::wstring wout(static_cast<size_t>(wlen - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, crlf.c_str(), -1, wout.data(), wlen - 1);
    return wout;
}

void refreshLanguageLabels(HWND hwnd) {
    SetWindowTextW(hwnd, g_isArabic
                             ? L"ARZ Media Suite v1.0.0 — مركز التحكم ومراقبة كارت الشاشة"
                             : L"ARZ Media Suite v1.0.0 — Control Center & RTX GPU Monitor");
    SetWindowTextW(g_btnLang, g_isArabic ? L"English (LTR)" : L"العربية (RTL)");
    SetWindowTextW(g_lblSub, g_isArabic
                                 ? L"حزمة الاستيراد والتصدير الاحترافية (59 صيغة | RTX NVENC AV1 | كاش VRAM -> RAM)"
                                 : L"Native Premiere Pro 27 Importer & Exporter (59 Formats | RTX NVENC AV1 | VRAM -> RAM Cache)");
    SetWindowTextW(g_chkHwDecode, g_isArabic
                                      ? L"تفعيل فك التشفير العتادي على كارت الشاشة (NVIDIA D3D11VA / NVDEC)"
                                      : L"Enable GPU Hardware Decoding (NVIDIA D3D11VA / NVDEC)");
    SetWindowTextW(g_chkGpuColor, g_isArabic
                                      ? L"تحويل ألوان 10-bit HDR داخل كارت الشاشة (Zero-CPU VideoProcessorBlt)"
                                      : L"100% GPU 10-bit Color Conversion (Zero-CPU VideoProcessorBlt)");
    SetWindowTextW(g_chkOverride, g_isArabic
                                      ? L"أولوية ARZ التلقائية لملفات MKV / WebM / AV1 / ProRes / DXV"
                                      : L"High-Priority ARZ Override for MKV / WebM / AV1 / ProRes / DXV");
    SetWindowTextW(g_chkCache, g_isArabic
                                   ? L"تفعيل الكاش المزدوج الذكي (VRAM حتى 82% ثم الترحيل للـ RAM)"
                                   : L"Enable Two-Tier Smart Frame Cache (82% VRAM -> System RAM Spillover)");
    SetWindowTextW(g_btnSave, g_isArabic ? L"حفظ إعدادات ARZ" : L"Save ARZ Settings");
    SetWindowTextW(g_btnClearCache, g_isArabic ? L"مسح كاش إضافات بريمير" : L"Clear Premiere PluginCache");
    SetWindowTextW(g_lblTeleHeader, g_isArabic
                                        ? L"المراقبة الحية لكارت الشاشة والـ VRAM (تحديث تلقائي):"
                                        : L"Live NVIDIA GPU & VRAM Telemetry (Auto-Refresh):");
    SetWindowTextW(g_editTele, readLatestTelemetryText().c_str());
    InvalidateRect(hwnd, nullptr, TRUE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            BOOL dark = TRUE;
            DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));

            g_lblSub = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                       250, 56, 480, 44, hwnd, nullptr, nullptr, nullptr);
            g_btnLang = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                        610, 16, 125, 32, hwnd, reinterpret_cast<HMENU>(IDC_BTN_LANG), nullptr, nullptr);

            g_chkHwDecode = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                            28, 130, 705, 28, hwnd, reinterpret_cast<HMENU>(IDC_CHK_HW_DECODE), nullptr, nullptr);
            g_chkGpuColor = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                            28, 164, 705, 28, hwnd, reinterpret_cast<HMENU>(IDC_CHK_GPU_COLOR), nullptr, nullptr);
            g_chkOverride = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                            28, 198, 705, 28, hwnd, reinterpret_cast<HMENU>(IDC_CHK_OVERRIDE), nullptr, nullptr);
            g_chkCache    = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                            28, 232, 705, 28, hwnd, reinterpret_cast<HMENU>(IDC_CHK_CACHE), nullptr, nullptr);

            SendMessageW(g_chkHwDecode, BM_SETCHECK, g_state.hwDecode ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(g_chkGpuColor, BM_SETCHECK, g_state.gpuColor ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(g_chkOverride, BM_SETCHECK, g_state.selectiveOverride ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(g_chkCache,    BM_SETCHECK, g_state.frameCache ? BST_CHECKED : BST_UNCHECKED, 0);

            g_btnSave = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                        28, 274, 220, 38, hwnd, reinterpret_cast<HMENU>(IDC_BTN_SAVE), nullptr, nullptr);
            g_btnClearCache = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                              264, 274, 250, 38, hwnd, reinterpret_cast<HMENU>(IDC_BTN_CLEARCACHE), nullptr, nullptr);

            g_lblTeleHeader = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                              28, 328, 705, 24, hwnd, nullptr, nullptr, nullptr);
            g_editTele = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                         WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                         28, 356, 708, 200, hwnd, reinterpret_cast<HMENU>(IDC_EDIT_TELEMETRY), nullptr, nullptr);

            HWND ctrls[] = {g_lblSub, g_btnLang, g_chkHwDecode, g_chkGpuColor, g_chkOverride,
                            g_chkCache, g_btnSave, g_btnClearCache, g_lblTeleHeader};
            for (HWND c : ctrls) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
            SendMessageW(g_editTele, WM_SETFONT, reinterpret_cast<WPARAM>(g_monoFont), TRUE);

            refreshLanguageLabels(hwnd);
            SetTimer(hwnd, IDT_TELEMETRY_POLL, 1500, nullptr);
            return 0;
        }

        case WM_TIMER:
            if (wParam == IDT_TELEMETRY_POLL) {
                SetWindowTextW(g_editTele, readLatestTelemetryText().c_str());
            }
            return 0;

        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (id == IDC_BTN_LANG) {
                g_isArabic = !g_isArabic;
                refreshLanguageLabels(hwnd);
            } else if (id == IDC_BTN_SAVE) {
                g_state.hwDecode = (SendMessageW(g_chkHwDecode, BM_GETCHECK, 0, 0) == BST_CHECKED);
                g_state.gpuColor = (SendMessageW(g_chkGpuColor, BM_GETCHECK, 0, 0) == BST_CHECKED);
                g_state.selectiveOverride = (SendMessageW(g_chkOverride, BM_GETCHECK, 0, 0) == BST_CHECKED);
                g_state.frameCache = (SendMessageW(g_chkCache, BM_GETCHECK, 0, 0) == BST_CHECKED);
                saveConfigState();
                MessageBoxW(hwnd,
                            g_isArabic ? L"تم حفظ إعدادات ARZ بنجاح!" : L"ARZ settings saved successfully!",
                            L"ARZ Media Suite", MB_OK | MB_ICONINFORMATION);
            } else if (id == IDC_BTN_CLEARCACHE) {
                const int n = clearAdobePluginCaches();
                wchar_t msg[256]{};
                if (g_isArabic) {
                    wsprintfW(msg, L"تم مسح كاش إضافات Adobe بنجاح (%d مفاتيح).\nأعد تشغيل بريمير لتحديث القائمة فوراً.", n);
                } else {
                    wsprintfW(msg, L"Cleared %d Adobe PluginCache registry trees.\nRestart Premiere Pro to rescan ARZ plugins.", n);
                }
                MessageBoxW(hwnd, msg, L"ARZ Media Suite", MB_OK | MB_ICONINFORMATION);
            }
            return 0;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            HWND ctrl = reinterpret_cast<HWND>(lParam);
            if (ctrl == g_editTele) {
                SetTextColor(hdc, kColCream);
                SetBkColor(hdc, kColCard);
                return reinterpret_cast<INT_PTR>(g_cardBrush);
            }
            SetTextColor(hdc, (ctrl == g_lblTeleHeader) ? kColGold : kColCream);
            SetBkColor(hdc, kColBg);
            return reinterpret_cast<INT_PTR>(g_bgBrush);
        }

        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, kColCream);
            SetBkColor(hdc, kColCard);
            return reinterpret_cast<INT_PTR>(g_cardBrush);
        }

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, g_bgBrush);

            // Draw ARZ Logo Banner in top-left
            if (g_bannerBmp) {
                HDC memDc = CreateCompatibleDC(hdc);
                HGDIOBJ oldBmp = SelectObject(memDc, g_bannerBmp);
                BITMAP bm{};
                GetObjectW(g_bannerBmp, sizeof(bm), &bm);
                BitBlt(hdc, 20, 12, bm.bmWidth, bm.bmHeight, memDc, 0, 0, SRCCOPY);
                SelectObject(memDc, oldBmp);
                DeleteDC(memDc);
            }

            // Draw Title & Gold Accent Divider Line
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, kColGold);
            HGDIOBJ oldFont = SelectObject(hdc, g_titleFont);
            TextOutW(hdc, 250, 20, L"ARZ MEDIA SUITE  v1.0", 21);
            SelectObject(hdc, oldFont);

            RECT goldLine{20, 114, rc.right - 20, 116};
            HBRUSH goldBr = CreateSolidBrush(kColGold);
            FillRect(hdc, &goldLine, goldBr);
            DeleteObject(goldBr);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DESTROY:
            KillTimer(hwnd, IDT_TELEMETRY_POLL);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int nCmdShow) {
    if (cmdLine && std::wstring(cmdLine).find(L"--headless-self-test") != std::wstring::npos) {
        loadConfigState();
        saveConfigState();
        HICON ico = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_ARZ_ICON));
        HBITMAP bmp = LoadBitmapW(hInst, MAKEINTRESOURCEW(IDB_ARZ_BANNER));
        return (ico != nullptr && bmp != nullptr) ? 0 : 2;
    }

    loadConfigState();
    g_bgBrush   = CreateSolidBrush(kColBg);
    g_cardBrush = CreateSolidBrush(kColCard);
    g_titleFont = CreateFontW(26, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_uiFont    = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_monoFont  = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    g_bannerBmp = LoadBitmapW(hInst, MAKEINTRESOURCEW(IDB_ARZ_BANNER));

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_ARZ_ICON));
    wc.hIconSm       = wc.hIcon;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g_bgBrush;
    wc.lpszClassName = L"ARZDesktopWindowClass";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"ARZ Media Suite v1.0.0",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 775, 615,
        nullptr, nullptr, hInst, nullptr);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_bannerBmp) DeleteObject(g_bannerBmp);
    if (g_titleFont) DeleteObject(g_titleFont);
    if (g_uiFont) DeleteObject(g_uiFont);
    if (g_monoFont) DeleteObject(g_monoFont);
    if (g_bgBrush) DeleteObject(g_bgBrush);
    if (g_cardBrush) DeleteObject(g_cardBrush);
    return 0;
}
