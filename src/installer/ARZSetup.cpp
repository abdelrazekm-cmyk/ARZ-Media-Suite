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

constexpr int IDC_EDIT_PATH   = 3001;
constexpr int IDC_BTN_INSTALL = 3002;
constexpr int IDC_LBL_STATUS  = 3003;

constexpr COLORREF kColBg    = RGB(11, 11, 13);
constexpr COLORREF kColCard  = RGB(22, 22, 26);
constexpr COLORREF kColCream = RGB(243, 236, 224);
constexpr COLORREF kColGold  = RGB(218, 155, 64);

HBRUSH  g_bgBrush = nullptr;
HBRUSH  g_cardBrush = nullptr;
HFONT   g_titleFont = nullptr;
HFONT   g_uiFont = nullptr;
HBITMAP g_bannerBmp = nullptr;
HWND    g_editPath = nullptr;
HWND    g_btnInstall = nullptr;
HWND    g_lblStatus = nullptr;

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

void clearAdobePluginCaches() {
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
                    for (const auto& ck : cacheKeys) RegDeleteTreeW(hVer, ck.c_str());
                    RegCloseKey(hVer);
                }
            }
            RegCloseKey(hRoot);
        }
    }
}

void createDesktopShortcut(const std::wstring& targetExe) {
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
                ppf->Save(lnkPath.c_str(), TRUE);
                ppf->Release();
            }
            psl->Release();
        }
    }
    CoUninitialize();
}

bool performInstallation(HINSTANCE hInst, const std::wstring& targetDir, std::wstring& outErr) {
    HRSRC hRes = FindResourceW(hInst, MAKEINTRESOURCEW(IDR_PAYLOAD_ZIP), RT_RCDATA);
    if (!hRes) { outErr = L"Embedded payload resource missing."; return false; }
    HGLOBAL hMem = LoadResource(hInst, hRes);
    const DWORD zipSize = SizeofResource(hInst, hRes);
    const void* zipData = LockResource(hMem);
    if (!zipData || zipSize == 0) { outErr = L"Failed locking payload."; return false; }

    std::error_code ec;
    // Clean legacy AER sibling directory if migrating on Premiere Pro 2027
    std::filesystem::path dst(targetDir);
    std::filesystem::path legacyAer = dst.parent_path() / L"AER";
    if (std::filesystem::exists(legacyAer, ec)) {
        std::filesystem::remove_all(legacyAer, ec);
    }

    std::filesystem::create_directories(dst, ec);
    if (ec) {
        outErr = L"Administrator permission required to write to Program Files.";
        return false;
    }

    wchar_t tmpDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmpDir);
    std::filesystem::path tempZip = std::filesystem::path(tmpDir) / L"arz_setup_payload_tmp.zip";
    {
        std::ofstream out(tempZip, std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char*>(zipData), zipSize);
    }

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

    if (exitCode != 0 || !std::filesystem::exists(dst / L"ARZ.prm", ec)) {
        outErr = L"Archive extraction failed (ensure Premiere Pro is closed).";
        return false;
    }

    clearAdobePluginCaches();
    createDesktopShortcut((dst / L"ARZDesktop.exe").wstring());
    return true;
}

LRESULT CALLBACK SetupWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            BOOL dark = TRUE;
            DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));

            HWND lblDir = CreateWindowExW(0, L"STATIC",
                L"مسار تثبيت حزمة ARZ (Premiere Pro Target Folder):",
                WS_CHILD | WS_VISIBLE, 28, 132, 580, 24, hwnd, nullptr, nullptr, nullptr);
            g_editPath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", detectDefaultInstallDir().c_str(),
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 28, 160, 584, 30,
                hwnd, reinterpret_cast<HMENU>(IDC_EDIT_PATH), nullptr, nullptr);
            g_btnInstall = CreateWindowExW(0, L"BUTTON",
                L"تثبيت حزمة ARZ v1.0 الآن  (Install ARZ Suite)",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 28, 208, 584, 44,
                hwnd, reinterpret_cast<HMENU>(IDC_BTN_INSTALL), nullptr, nullptr);
            g_lblStatus = CreateWindowExW(0, L"STATIC",
                L"حزمة مستقلة 100% — تشمل المستورد والمصدّر ومكتبات FFmpeg 9.1+ بدون أي متطلبات إضافية.",
                WS_CHILD | WS_VISIBLE, 28, 266, 584, 40, hwnd, reinterpret_cast<HMENU>(IDC_LBL_STATUS), nullptr, nullptr);

            SendMessageW(lblDir, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
            SendMessageW(g_editPath, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
            SendMessageW(g_btnInstall, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
            SendMessageW(g_lblStatus, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
            return 0;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_BTN_INSTALL) {
                if (isPremiereRunning()) {
                    MessageBoxW(hwnd,
                        L"برجاء إغلاق برنامج Adobe Premiere Pro و Media Encoder أولاً قبل التثبيت.",
                        L"ARZ Setup", MB_OK | MB_ICONWARNING);
                    return 0;
                }
                wchar_t buf[MAX_PATH * 2]{};
                GetWindowTextW(g_editPath, buf, MAX_PATH * 2);
                SetWindowTextW(g_lblStatus, L"جاري استخراج وتثبيت ملفات ARZ ومسح PluginCache...");
                UpdateWindow(hwnd);
                EnableWindow(g_btnInstall, FALSE);

                std::wstring err;
                if (performInstallation(GetModuleHandleW(nullptr), buf, err)) {
                    SetWindowTextW(g_lblStatus, L"تم تثبيت حزمة ARZ v1.0 بنجاح! يمكنك فتح Premiere Pro الآن.");
                    if (MessageBoxW(hwnd,
                            L"تم تثبيت حزمة ARZ v1.0 (المستورد + المصدّر + مركز التحكم) بنجاح!\nهل ترغب في فتح مركز تحكم ARZ الآن؟",
                            L"ARZ Setup Complete", MB_YESNO | MB_ICONINFORMATION) == IDYES) {
                        std::wstring deskExe = std::wstring(buf) + L"\\ARZDesktop.exe";
                        ShellExecuteW(nullptr, L"open", deskExe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }
                    DestroyWindow(hwnd);
                } else {
                    EnableWindow(g_btnInstall, TRUE);
                    SetWindowTextW(g_lblStatus, err.c_str());
                    MessageBoxW(hwnd, err.c_str(), L"ARZ Setup Error", MB_OK | MB_ICONERROR);
                }
            }
            return 0;

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
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
            TextOutW(hdc, 252, 26, L"ARZ SUITE INSTALLER  v1.0", 25);
            SelectObject(hdc, oldFont);

            SetTextColor(hdc, kColCream);
            oldFont = SelectObject(hdc, g_uiFont);
            TextOutW(hdc, 252, 62, L"Premiere Pro 27 Native Importer + RTX Exporter", 46);
            SelectObject(hdc, oldFont);

            RECT goldLine{20, 116, rc.right - 20, 118};
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

    // Auto-elevate to Administrator if launched without admin rights
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
    g_uiFont    = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
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
        0, wc.lpszClassName, L"ARZ Media Suite v1.0.0 — Setup",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 655, 360,
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
