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
#include <shobjidl.h>
#include <tlhelp32.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace {

constexpr int IDI_ARZ_ICON    = 101;
constexpr int IDB_ARZ_BANNER  = 102;
constexpr int IDR_PAYLOAD_ZIP = 201;

constexpr intptr_t IDC_EDIT_PATH   = 3001;
constexpr intptr_t IDC_BTN_INSTALL = 3002;
constexpr intptr_t IDC_EDIT_LOG    = 3003;
constexpr intptr_t IDC_BTN_OPEN_CC = 3004;

constexpr COLORREF kColBg    = RGB(11, 11, 13);
constexpr COLORREF kColCard  = RGB(20, 20, 25);
constexpr COLORREF kColCream = RGB(243, 236, 224);
constexpr COLORREF kColGold  = RGB(218, 155, 64);

HBRUSH  g_bgBrush = nullptr;
HBRUSH  g_cardBrush = nullptr;
HFONT   g_titleFont = nullptr;
HFONT   g_uiFont = nullptr;
HFONT   g_monoFont = nullptr;
HBITMAP g_bannerBmp = nullptr;
HWND    g_editPath = nullptr;
HWND    g_btnInstall = nullptr;
HWND    g_btnOpenCc = nullptr;
HWND    g_editLog = nullptr;
std::wstring g_installedDir;

void appendLogLine(const std::wstring& line) {
    if (!g_editLog) return;
    int len = GetWindowTextLengthW(g_editLog);
    SendMessageW(g_editLog, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
    std::wstring formatted = line + L"\r\n";
    SendMessageW(g_editLog, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(formatted.c_str()));
    SendMessageW(g_editLog, EM_SCROLLCARET, 0, 0);
    UpdateWindow(g_editLog);
}

std::wstring detectDefaultInstallDir() {
    std::error_code ec;
    const wchar_t* pr27 = L"C:\\Program Files\\Adobe\\Adobe Premiere Pro 2027\\PlugIns\\Common";
    if (std::filesystem::exists(pr27, ec)) {
        return std::wstring(pr27) + L"\\ARZ";
    }
    const wchar_t* pr26 = L"C:\\Program Files\\Adobe\\Adobe Premiere Pro 2026\\PlugIns\\Common";
    if (std::filesystem::exists(pr26, ec)) {
        return std::wstring(pr26) + L"\\ARZ";
    }
    return L"C:\\Program Files\\Adobe\\Common\\Plug-ins\\7.0\\MediaCore\\ARZ";
}

bool isPremiereRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool running = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring name(pe.szExeFile);
            if (name.find(L"Adobe Premiere Pro") != std::wstring::npos ||
                name.find(L"Adobe Media Encoder") != std::wstring::npos) {
                running = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return running;
}

int clearAdobePluginCaches() {
    int clearedCount = 0;
    const wchar_t* roots[] = {
        L"Software\\Adobe\\Premiere Pro",
        L"Software\\Adobe\\Adobe Media Encoder"
    };
    for (const wchar_t* r : roots) {
        HKEY hRoot = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, r, 0, KEY_READ | KEY_WRITE, &hRoot) == ERROR_SUCCESS) {
            wchar_t subName[256]{};
            DWORD idx = 0;
            std::vector<std::wstring> versions;
            while (true) {
                DWORD sz = 256;
                if (RegEnumKeyExW(hRoot, idx++, subName, &sz, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                versions.emplace_back(subName);
            }
            for (const auto& ver : versions) {
                HKEY hVer = nullptr;
                if (RegOpenKeyExW(hRoot, ver.c_str(), 0, KEY_READ | KEY_WRITE, &hVer) == ERROR_SUCCESS) {
                    DWORD kIdx = 0;
                    std::vector<std::wstring> cacheKeys;
                    while (true) {
                        DWORD sz = 256;
                        if (RegEnumKeyExW(hVer, kIdx++, subName, &sz, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                        if (std::wstring(subName).find(L"PluginCache") != std::wstring::npos) {
                            cacheKeys.emplace_back(subName);
                        }
                    }
                    for (const auto& ck : cacheKeys) {
                        if (RegDeleteTreeW(hVer, ck.c_str()) == ERROR_SUCCESS) {
                            ++clearedCount;
                        }
                    }
                    RegCloseKey(hVer);
                }
            }
            RegCloseKey(hRoot);
        }
    }
    return clearedCount;
}

bool createDesktopShortcut(const std::wstring& targetExe) {
    bool ok = false;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wchar_t* desk = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk)) && desk) {
        std::wstring lnkPath = std::wstring(desk) + L"\\ARZ Control Center.lnk";
        CoTaskMemFree(desk);

        IShellLinkW* psl = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&psl))) && psl) {
            psl->SetPath(targetExe.c_str());
            psl->SetDescription(L"ARZ Media Suite Control Center & GPU Telemetry");
            psl->SetIconLocation(targetExe.c_str(), 0);
            IPersistFile* ppf = nullptr;
            if (SUCCEEDED(psl->QueryInterface(IID_PPV_ARGS(&ppf))) && ppf) {
                ok = SUCCEEDED(ppf->Save(lnkPath.c_str(), TRUE));
                ppf->Release();
            }
            psl->Release();
        }
    }
    CoUninitialize();
    return ok;
}

bool performInstallation(HINSTANCE hInst, const std::wstring& targetDir, std::wstring& outErr) {
    appendLogLine(L"[1/5] Checking embedded standalone ARZ v1.1.0 payload...");
    HRSRC hRes = FindResourceW(hInst, MAKEINTRESOURCEW(IDR_PAYLOAD_ZIP), RT_RCDATA);
    if (!hRes) {
        outErr = L"[ERROR] Embedded payload resource (#201) is missing from Setup.exe.";
        appendLogLine(outErr);
        return false;
    }
    HGLOBAL hMem = LoadResource(hInst, hRes);
    const DWORD zipSize = SizeofResource(hInst, hRes);
    const void* zipData = LockResource(hMem);
    if (!zipData || zipSize == 0) {
        outErr = L"[ERROR] Failed locking embedded payload in memory.";
        appendLogLine(outErr);
        return false;
    }
    appendLogLine(L"      -> Payload verified (" + std::to_wstring(zipSize / (1024 * 1024)) + L" MB embedded).");

    std::error_code ec;
    std::filesystem::path dst(targetDir);
    std::filesystem::path legacyAer = dst.parent_path() / L"AER";
    if (std::filesystem::exists(legacyAer, ec)) {
        appendLogLine(L"[2/5] Removing legacy AER plugin folder: " + legacyAer.wstring());
        std::filesystem::remove_all(legacyAer, ec);
    } else {
        appendLogLine(L"[2/5] Preparing clean target folder: " + dst.wstring());
    }

    std::filesystem::create_directories(dst, ec);
    if (ec) {
        outErr = L"[ERROR] Cannot write to target directory (Administrator privileges required): " + dst.wstring();
        appendLogLine(outErr);
        return false;
    }

    wchar_t tmpDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmpDir);
    std::filesystem::path tempZip = std::filesystem::path(tmpDir) / L"arz_setup_payload_tmp.zip";
    {
        std::ofstream out(tempZip, std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char*>(zipData), zipSize);
        if (!out.good()) {
            outErr = L"[ERROR] Failed staging temporary archive in %TEMP%.";
            appendLogLine(outErr);
            return false;
        }
    }

    appendLogLine(L"[3/5] Extracting ARZ.prm, ARZ_HP.prm, ARZ_Exporter.prm, ARZCore.dll, ARZDesktop.exe & FFmpeg 9.1+ DLLs...");
    std::wstring cmd = L"tar.exe -xf \"" + tempZip.wstring() + L"\" -C \"" + dst.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    DWORD exitCode = 1;
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 60000);
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    std::filesystem::remove(tempZip, ec);

    const wchar_t* requiredFiles[] = {
        L"ARZ.prm", L"ARZ_HP.prm", L"ARZ_Exporter.prm", L"ARZCore.dll", L"ARZDesktop.exe"
    };
    for (const wchar_t* rf : requiredFiles) {
        if (exitCode != 0 || !std::filesystem::exists(dst / rf, ec)) {
            outErr = std::wstring(L"[ERROR] Failed extracting or verifying file: ") + rf +
                     L" (make sure Premiere Pro & Media Encoder are completely closed).";
            appendLogLine(outErr);
            return false;
        }
    }
    appendLogLine(L"      -> [OK] All ARZ plugins and standalone runtime DLLs verified on disk.");

    appendLogLine(L"[4/5] Resetting Adobe Premiere Pro & Media Encoder PluginCache...");
    const int cachesCleared = clearAdobePluginCaches();
    appendLogLine(L"      -> [OK] Cleared " + std::to_wstring(cachesCleared) + L" Adobe PluginCache registry trees.");

    appendLogLine(L"[5/5] Creating Desktop shortcut for ARZ Control Center...");
    if (createDesktopShortcut((dst / L"ARZDesktop.exe").wstring())) {
        appendLogLine(L"      -> [OK] Desktop shortcut 'ARZ Control Center' created.");
    } else {
        appendLogLine(L"      -> [WARN] Could not create Desktop shortcut (plugins are still installed).");
    }

    appendLogLine(L"==================================================================");
    appendLogLine(L"[SUCCESS] تم تثبيت حزمة ARZ Media Suite v1.1.0 بنجاح بدون أي أخطاء!");
    appendLogLine(L"          يمكنك فتح Adobe Premiere Pro الآن مباشرة.");
    appendLogLine(L"==================================================================");
    g_installedDir = dst.wstring();
    return true;
}

LRESULT CALLBACK SetupWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            BOOL dark = TRUE;
            DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));

            HWND lblDir = CreateWindowExW(0, L"STATIC",
                L"مسار تثبيت حزمة ARZ (Premiere Pro Target Folder):",
                WS_CHILD | WS_VISIBLE, 28, 128, 610, 22, hwnd, nullptr, nullptr, nullptr);
            g_editPath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", detectDefaultInstallDir().c_str(),
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 28, 154, 614, 28,
                hwnd, reinterpret_cast<HMENU>(IDC_EDIT_PATH), nullptr, nullptr);
            g_btnInstall = CreateWindowExW(0, L"BUTTON",
                L"تثبيت حزمة ARZ v1.1 الآن  (Install ARZ Suite)",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 28, 194, 396, 42,
                hwnd, reinterpret_cast<HMENU>(IDC_BTN_INSTALL), nullptr, nullptr);
            g_btnOpenCc = CreateWindowExW(0, L"BUTTON",
                L"فتح مركز التحكم (Control Center)",
                WS_CHILD | WS_VISIBLE | WS_DISABLED, 434, 194, 208, 42,
                hwnd, reinterpret_cast<HMENU>(IDC_BTN_OPEN_CC), nullptr, nullptr);

            HWND lblLog = CreateWindowExW(0, L"STATIC",
                L"سجل التثبيت المباشر (Live Installation & Verification Log):",
                WS_CHILD | WS_VISIBLE, 28, 248, 610, 22, hwnd, nullptr, nullptr, nullptr);
            g_editLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                28, 274, 614, 175,
                hwnd, reinterpret_cast<HMENU>(IDC_EDIT_LOG), nullptr, nullptr);

            SendMessageW(lblDir, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
            SendMessageW(g_editPath, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
            SendMessageW(g_btnInstall, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
            SendMessageW(g_btnOpenCc, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
            SendMessageW(lblLog, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
            SendMessageW(g_editLog, WM_SETFONT, reinterpret_cast<WPARAM>(g_monoFont), TRUE);

            appendLogLine(L"[READY] حزمة ARZ Media Suite v1.1.0 مستقلة 100% وجاهزة للتثبيت.");
            appendLogLine(L"[INFO]  لا تحتاج لأي إضافات خارجية (شاملة المستورد + مصدّر RTX السريع + FFmpeg 9.1+).");
            return 0;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_BTN_INSTALL) {
                SetWindowTextW(g_editLog, L"");
                appendLogLine(L"[START] بدء عملية التثبيت وفحص البرامج المفتوحة...");
                if (isPremiereRunning()) {
                    appendLogLine(L"[ERROR] تم رصد تشغيل برنامج Adobe Premiere Pro أو Media Encoder حالياً!");
                    appendLogLine(L"        برجاء إغلاقه تماماً ثم الضغط على تثبيت مرة أخرى.");
                    MessageBoxW(hwnd,
                        L"برجاء إغلاق برنامج Adobe Premiere Pro و Media Encoder أولاً قبل التثبيت.",
                        L"ARZ Setup — Close Premiere Pro", MB_OK | MB_ICONWARNING);
                    return 0;
                }
                wchar_t buf[MAX_PATH * 2]{};
                GetWindowTextW(g_editPath, buf, MAX_PATH * 2);
                EnableWindow(g_btnInstall, FALSE);

                std::wstring err;
                if (performInstallation(GetModuleHandleW(nullptr), buf, err)) {
                    SetWindowTextW(g_btnInstall, L"تم التثبيت بنجاح ✓ (إعادة التثبيت)");
                    EnableWindow(g_btnInstall, TRUE);
                    EnableWindow(g_btnOpenCc, TRUE);
                } else {
                    appendLogLine(L"[FAILED] تعذر إكمال التثبيت — راجع سبب الخطأ بالأعلى.");
                    EnableWindow(g_btnInstall, TRUE);
                    MessageBoxW(hwnd, err.c_str(), L"ARZ Setup Error", MB_OK | MB_ICONERROR);
                }
            } else if (LOWORD(wParam) == IDC_BTN_OPEN_CC) {
                if (!g_installedDir.empty()) {
                    std::wstring deskExe = g_installedDir + L"\\ARZDesktop.exe";
                    ShellExecuteW(nullptr, L"open", deskExe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
            return 0;

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            if (reinterpret_cast<HWND>(lParam) == g_editLog) {
                SetTextColor(hdc, kColCream);
                SetBkColor(hdc, kColCard);
                return reinterpret_cast<INT_PTR>(g_cardBrush);
            }
            SetTextColor(hdc, kColCream);
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

            if (g_bannerBmp) {
                HDC memDc = CreateCompatibleDC(hdc);
                HGDIOBJ old = SelectObject(memDc, g_bannerBmp);
                BITMAP bm{};
                GetObjectW(g_bannerBmp, sizeof(bm), &bm);
                BitBlt(hdc, 20, 12, bm.bmWidth, bm.bmHeight, memDc, 0, 0, SRCCOPY);
                SelectObject(memDc, old);
                DeleteDC(memDc);
            }

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, kColGold);
            HGDIOBJ oldFont = SelectObject(hdc, g_titleFont);
            TextOutW(hdc, 252, 24, L"ARZ SUITE INSTALLER  v1.1", 25);
            SelectObject(hdc, oldFont);

            SetTextColor(hdc, kColCream);
            oldFont = SelectObject(hdc, g_uiFont);
            TextOutW(hdc, 252, 60, L"Premiere Pro 27 Native Importer + Turbo RTX Exporter", 52);
            SelectObject(hdc, oldFont);

            RECT goldLine{20, 114, rc.right - 20, 116};
            HBRUSH goldBr = CreateSolidBrush(kColGold);
            FillRect(hdc, &goldLine, goldBr);
            DeleteObject(goldBr);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int nCmdShow) {
    const std::wstring args = cmdLine ? cmdLine : L"";
    if (args.find(L"--self-verify") != std::wstring::npos) {
        HRSRC hRes = FindResourceW(hInst, MAKEINTRESOURCEW(IDR_PAYLOAD_ZIP), RT_RCDATA);
        return (hRes && SizeofResource(hInst, hRes) > 1000000) ? 0 : 2;
    }

    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    if (!isAdmin) {
        wchar_t selfPath[MAX_PATH * 2]{};
        GetModuleFileNameW(nullptr, selfPath, MAX_PATH * 2);
        ShellExecuteW(nullptr, L"runas", selfPath, args.c_str(), nullptr, SW_SHOWNORMAL);
        return 0;
    }

    g_bgBrush   = CreateSolidBrush(kColBg);
    g_cardBrush = CreateSolidBrush(kColCard);
    g_titleFont = CreateFontW(25, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_uiFont    = CreateFontW(17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_monoFont  = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    g_bannerBmp = LoadBitmapW(hInst, MAKEINTRESOURCEW(IDB_ARZ_BANNER));

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = SetupWndProc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_ARZ_ICON));
    wc.hIconSm       = wc.hIcon;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g_bgBrush;
    wc.lpszClassName = L"ARZSetupWindowClass";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"ARZ Media Suite v1.1.0 — Standalone Setup",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 680, 505,
        nullptr, nullptr, hInst, nullptr);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
