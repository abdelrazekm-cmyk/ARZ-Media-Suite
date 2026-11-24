#ifndef PRWIN_ENV
#define PRWIN_ENV 1
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <PrSDKExport.h>
#include <PrSDKMALErrors.h>

#include <mutex>
#include <string>

namespace {

using xAERExportEntryFn = int32_t (*)(int32_t, void*, void*, void*);

HMODULE g_selfModule = nullptr;
std::once_flag g_loadOnce;
HMODULE g_coreModule = nullptr;
xAERExportEntryFn g_exportEntry = nullptr;

void loadCoreNextToShim() {
    wchar_t modulePath[MAX_PATH * 4]{};
    if (!GetModuleFileNameW(g_selfModule, modulePath, static_cast<DWORD>(sizeof(modulePath) / sizeof(wchar_t)))) {
        return;
    }
    std::wstring dir(modulePath);
    const auto slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        dir.erase(slash + 1);
    } else {
        dir.clear();
    }
    const std::wstring corePath = dir + L"ARZCore.dll";
    g_coreModule = LoadLibraryExW(
        corePath.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (g_coreModule) {
        g_exportEntry = reinterpret_cast<xAERExportEntryFn>(GetProcAddress(g_coreModule, "xAERExportEntry"));
    }
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ulReason, LPVOID /*lpReserved*/) {
    if (ulReason == DLL_PROCESS_ATTACH) {
        g_selfModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

extern "C" DllExport PREMPLUGENTRY xSDKExport(
    csSDK_int32 selector,
    exportStdParms* stdParms,
    void* param1,
    void* param2) {
    std::call_once(g_loadOnce, loadCoreNextToShim);
    if (!g_exportEntry) {
        return exportReturn_ErrOther;
    }
    return static_cast<prMALError>(
        g_exportEntry(selector, stdParms, param1, param2));
}
