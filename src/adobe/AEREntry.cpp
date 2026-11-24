#ifndef PRWIN_ENV
#define PRWIN_ENV 1
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <PrSDKImport.h>
#include <PrSDKMALErrors.h>

#include <mutex>
#include <string>

namespace {

using xAEREntryFn = int32_t (*)(int32_t, void*, void*, void*, int32_t);

HMODULE g_selfModule = nullptr;
std::once_flag g_loadOnce;
HMODULE g_coreModule = nullptr;
xAEREntryFn g_coreEntry = nullptr;

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
        g_coreEntry = reinterpret_cast<xAEREntryFn>(GetProcAddress(g_coreModule, "xAEREntry"));
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

extern "C" PREMPLUGENTRY DllExport xImportEntry(
    csSDK_int32 selector,
    imStdParms* stdParms,
    void* param1,
    void* param2) {
    std::call_once(g_loadOnce, loadCoreNextToShim);
    if (!g_coreEntry) {
        return imOtherErr;
    }
#if AER_HIGH_PRIORITY
    constexpr int32_t kHighPriority = 1;
#else
    constexpr int32_t kHighPriority = 0;
#endif
    return static_cast<prMALError>(
        g_coreEntry(selector, stdParms, param1, param2, kHighPriority));
}
