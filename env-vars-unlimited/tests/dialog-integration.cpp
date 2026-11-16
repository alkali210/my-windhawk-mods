// Exercises the real sysdm.cpl dialogs in a private desktop and registry tree.
// MinHook and the RVAs below are used only by this isolated test executable.
#define WH_MOD
#ifdef ENV_VARS_TEST_WINDHAWK_INIT
#define WH_MOD_ID L"isolated@env-vars-unlimited"
#define WH_MOD_VERSION L"test"
#include <windows.h>
static HMODULE WINAPI TestLoadLibraryExW(LPCWSTR path, HANDLE file, DWORD flags);
#define LoadLibraryExW TestLoadLibraryExW
#else
#define WH_EDITING
#endif
#include "../env-vars-unlimited.wh.cpp"
#ifdef ENV_VARS_TEST_WINDHAWK_INIT
#undef LoadLibraryExW
#endif

#include <MinHook.h>
#include <atomic>
#include <cstdio>
#include <stdexcept>

#ifdef ENV_VARS_TEST_WINDHAWK_INIT
#include <dbghelp.h>
#include <string_view>

// Opt-in compatibility experiment: the mod itself is unchanged, but its
// System32 load is redirected to a supplied sysdm.cpl in this test process.
// The production build never includes this loader.
static HMODULE WINAPI TestLoadLibraryExW(LPCWSTR path, HANDLE file, DWORD flags) {
    WCHAR sample[MAX_PATH]{};
    if (path && _wcsicmp(path, L"sysdm.cpl") == 0 &&
        GetEnvironmentVariableW(L"ENV_VARS_TEST_SAMPLE", sample, ARRAYSIZE(sample))) {
        return LoadLibraryExW(sample, file, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
    return LoadLibraryExW(path, file, flags);
}

// Test-only Windhawk API implementation. It resolves the exact decorated names
// from the matching PDB and registers hooks with MinHook. This lets the real
// Wh_ModInit run in rundll32, without installing anything in Windhawk.
struct SymbolResolution {
    const WH_SYMBOL_HOOK* hooks;
    size_t count;
    std::vector<void*> addresses;
    std::vector<unsigned> matches;
};

BOOL CALLBACK ResolveTestSymbol(PSYMBOL_INFOW symbol, ULONG, PVOID context) {
    auto& resolution = *static_cast<SymbolResolution*>(context);
    std::wstring_view name(symbol->Name, symbol->NameLen);
    while (!name.empty() && name.back() == L'\0') name.remove_suffix(1);
    for (size_t i = 0; i < resolution.count; ++i) {
        for (size_t j = 0; j < resolution.hooks[i].symbolsCount; ++j) {
            const auto& candidate = resolution.hooks[i].symbols[j];
            if (name == std::wstring_view(candidate.string, candidate.length)) {
                resolution.addresses[i] = reinterpret_cast<void*>(symbol->Address);
                ++resolution.matches[i];
            }
        }
    }
    return TRUE;
}

extern "C" BOOL InternalWh_IsLogEnabled(void*) { return TRUE; }
extern "C" void InternalWh_Log(void*, PCWSTR format, va_list args) {
    vfwprintf(stderr, format, args);
    fwprintf(stderr, L"\n");
}
extern "C" BOOL InternalWh_SetFunctionHook(void*, void* target, void* hook, void** original) {
    return MH_CreateHook(target, hook, original) == MH_OK;
}
extern "C" BOOL InternalWh_HookSymbols(void*, HMODULE module, const WH_SYMBOL_HOOK* hooks,
                                       size_t count, const WH_HOOK_SYMBOLS_OPTIONS* options) {
    if (!options || !options->noUndecoratedSymbols) return FALSE;
    WCHAR symbolDir[MAX_PATH]{};
    WCHAR modulePath[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"ENV_VARS_TEST_SYMBOLS", symbolDir, ARRAYSIZE(symbolDir)) ||
        !GetModuleFileNameW(module, modulePath, ARRAYSIZE(modulePath))) return FALSE;
    HANDLE process = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &process,
                         0, FALSE, DUPLICATE_SAME_ACCESS)) return FALSE;
    SymSetOptions(SYMOPT_DEFERRED_LOADS);
    if (!SymInitializeW(process, symbolDir, FALSE)) {
        CloseHandle(process);
        return FALSE;
    }
    const DWORD64 moduleBase = reinterpret_cast<DWORD64>(module);
    SymbolResolution resolution{hooks, count, std::vector<void*>(count), std::vector<unsigned>(count)};
    const DWORD64 loaded = SymLoadModuleExW(process, nullptr, modulePath, nullptr, moduleBase, 0, nullptr, 0);
    const BOOL enumerated = loaded && SymEnumSymbolsW(process, loaded, nullptr, ResolveTestSymbol, &resolution);
    SymCleanup(process);
    CloseHandle(process);
    if (!enumerated) return FALSE;
    for (size_t i = 0; i < count; ++i) {
        if (resolution.matches[i] != 1 || !resolution.addresses[i]) return FALSE;
    }
    for (size_t i = 0; i < count; ++i) {
        if (hooks[i].hookFunction) {
            if (MH_CreateHook(resolution.addresses[i], hooks[i].hookFunction,
                              hooks[i].pOriginalFunction) != MH_OK) return FALSE;
        } else if (hooks[i].pOriginalFunction) {
            *hooks[i].pOriginalFunction = resolution.addresses[i];
        }
    }
    return TRUE;
}
#endif

namespace {

using NtQueryKeyFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
struct NativeString { USHORT length; USHORT maximumLength; PWSTR buffer; };
using NtSetValueKeyFn = LONG(NTAPI*)(HANDLE, NativeString*, ULONG, ULONG, PVOID, ULONG);
NtQueryKeyFn queryKey;
std::wstring testRootPath;
decltype(&RegSetValueExW) realSetValue;
decltype(&RegDeleteValueW) realDeleteValue;
decltype(&SendNotifyMessageW) realNotify;
std::atomic_bool forbiddenWrite;
std::atomic_bool expectMessageBox;
std::atomic_bool injectSystemFailure;
std::atomic_bool injectedFailureObserved;
std::atomic_int messageBoxCount;
std::atomic_int broadcasts;
std::atomic_int crossSessionBroadcasts;
std::atomic<HWND> readyMainDialog;
std::atomic<HWND> readyTextDialog;
std::atomic<HWND> readyPathDialog;
WCHAR lastMessageBoxText[1024]{};
#ifdef ENV_VARS_TEST_WINDHAWK_INIT
HMODULE sampleModule;
HMODULE sampleResources;
decltype(&DialogBoxParamW) realDialogBoxParam;
INT_PTR WINAPI SampleDialogBoxParam(HINSTANCE instance, LPCWSTR name, HWND parent,
                                   DLGPROC proc, LPARAM parameter) {
    if (instance == sampleModule) {
        instance = sampleResources;
        HRSRC found = FindResourceW(instance, name, RT_DIALOG);
        HGLOBAL loaded = found ? LoadResource(instance, found) : nullptr;
        auto* dialogTemplate = loaded ? static_cast<const DLGTEMPLATE*>(LockResource(loaded)) : nullptr;
        if (!dialogTemplate) return -1;
        INT_PTR result = DialogBoxIndirectParamW(instance, dialogTemplate, parent, proc, parameter);
        if (result == -1) fprintf(stderr, "Sample indirect dialog creation failed: %lu\n", GetLastError());
        return result;
    }
    return realDialogBoxParam(instance, name, parent, proc, parameter);
}
#endif

std::wstring KeyPath(HKEY key) {
    std::vector<BYTE> data(4096);
    ULONG needed = 0;
    if (queryKey(key, 3, data.data(), static_cast<ULONG>(data.size()), &needed) < 0) {
        return {};
    }
    const ULONG length = *reinterpret_cast<ULONG*>(data.data());
    if (length + sizeof(ULONG) > data.size() || length % sizeof(WCHAR)) return {};
    return std::wstring(reinterpret_cast<WCHAR*>(data.data() + sizeof(ULONG)), length / sizeof(WCHAR));
}

bool IsTestKey(HKEY key) {
    try {
        std::wstring path = KeyPath(key);
        return path.size() >= testRootPath.size() &&
               CompareStringOrdinal(path.c_str(), static_cast<int>(testRootPath.size()),
                                    testRootPath.c_str(), static_cast<int>(testRootPath.size()), TRUE) == CSTR_EQUAL &&
               (path.size() == testRootPath.size() || path[testRootPath.size()] == L'\\');
    } catch (...) {
        return false;
    }
}

LSTATUS WINAPI SafeSetValue(HKEY key, LPCWSTR name, DWORD reserved, DWORD type,
                            const BYTE* data, DWORD size) {
    if (!IsTestKey(key)) {
        forbiddenWrite = true;
        return ERROR_ACCESS_DENIED;
    }
    if (injectSystemFailure && name && _wcsicmp(name, L"WH_TEST_SYSTEM") == 0) {
        injectedFailureObserved = true;
        return ERROR_ACCESS_DENIED;
    }
    return realSetValue(key, name, reserved, type, data, size);
}

LSTATUS WINAPI SafeDeleteValue(HKEY key, LPCWSTR name) {
    if (!IsTestKey(key)) {
        forbiddenWrite = true;
        return ERROR_ACCESS_DENIED;
    }
    return realDeleteValue(key, name);
}

BOOL WINAPI SafeNotify(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (window == HWND_BROADCAST && message == WM_SETTINGCHANGE) {
        ++broadcasts;
        return TRUE;
    }
    return realNotify(window, message, wp, lp);
}

LONG WINAPI SafeWinStationBroadcast(HANDLE, DWORD, DWORD, DWORD, DWORD, DWORD*,
                                    DWORD, ULONG_PTR, PCWSTR, DWORD*) {
    ++crossSessionBroadcasts;
    return 0;
}

int WINAPI TestMessageBox(HWND, LPCWSTR text, LPCWSTR, UINT) {
    wcsncpy_s(lastMessageBoxText, ARRAYSIZE(lastMessageBoxText), text ? text : L"", _TRUNCATE);
    ++messageBoxCount;
    if (!expectMessageBox) fwprintf(stderr, L"Unexpected message box: %ls\n", text);
    return IDOK;
}

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename T, typename U>
void Hook(void* target, T replacement, U* original) {
    MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(replacement),
                                     reinterpret_cast<void**>(original));
    if (status != MH_OK) throw std::runtime_error(MH_StatusToString(status));
}

template <typename T, typename U>
void HookRva(HMODULE module, size_t rva, T replacement, U* original) {
    Hook(reinterpret_cast<BYTE*>(module) + rva, replacement, original);
}

struct UiThread {
    HDESK desktop;
    HMODULE module;
    std::atomic_bool attached = false;
};

LRESULT CALLBACK ObserveDialogInitialization(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0) {
        const auto* message = reinterpret_cast<const CWPRETSTRUCT*>(lp);
        if (message && message->message == WM_INITDIALOG) {
            if (GetDlgItem(message->hwnd, kUserListControl)) readyMainDialog = message->hwnd;
            else if (GetDlgItem(message->hwnd, kValueControl)) readyTextDialog = message->hwnd;
            else if (GetDlgItem(message->hwnd, kPathListControl)) readyPathDialog = message->hwnd;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

DWORD WINAPI RunDialog(void* data) {
    auto* ui = static_cast<UiThread*>(data);
    readyMainDialog = nullptr;
    readyTextDialog = nullptr;
    readyPathDialog = nullptr;
    ui->attached = SetThreadDesktop(ui->desktop) != FALSE;
    if (!ui->attached) return 1;
    HHOOK observer = SetWindowsHookExW(WH_CALLWNDPROCRET, ObserveDialogInitialization,
                                      nullptr, GetCurrentThreadId());
    if (!observer) return 3;
    using ExportFn = void(CALLBACK*)(HWND, HINSTANCE, LPWSTR, int);
    auto open = reinterpret_cast<ExportFn>(GetProcAddress(ui->module, "EditEnvironmentVariables"));
    if (!open) {
        UnhookWindowsHookEx(observer);
        return 2;
    }
    open(nullptr, nullptr, nullptr, 0);
    UnhookWindowsHookEx(observer);
    return 0;
}

struct WindowSearch { int id; HWND result; };
BOOL CALLBACK FindDialogCallback(HWND hwnd, LPARAM param) {
    auto* search = reinterpret_cast<WindowSearch*>(param);
    WCHAR cls[32]{};
    if (GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) && wcscmp(cls, L"#32770") == 0 &&
        GetDlgItem(hwnd, search->id)) {
        search->result = hwnd;
        return FALSE;
    }
    return TRUE;
}

HWND WaitDialog(DWORD threadId, int id) {
    for (int i = 0; i < 300; ++i) {
        WindowSearch search{id, nullptr};
        EnumThreadWindows(threadId, FindDialogCallback, reinterpret_cast<LPARAM>(&search));
        if (search.result &&
            (id != kUserListControl || readyMainDialog == search.result) &&
            (id != kValueControl || readyTextDialog == search.result) &&
            (id != kPathListControl || readyPathDialog == search.result))
            return search.result;
        Sleep(10);
    }
    fprintf(stderr, "Timed out waiting for dialog control %d\n", id);
    throw std::runtime_error("Timed out waiting for a native dialog");
}

void WaitClosed(HWND window) {
    for (int i = 0; i < 300 && IsWindow(window); ++i) Sleep(10);
    Check(!IsWindow(window), "The native dialog did not close");
    if (readyTextDialog == window) readyTextDialog = nullptr;
    if (readyPathDialog == window) readyPathDialog = nullptr;
}

void Click(HWND dialog, int id) {
    Check(PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED),
                       reinterpret_cast<LPARAM>(GetDlgItem(dialog, id))) != FALSE,
          "Posting the command failed");
}

int FindItem(HWND list, PCWSTR name) {
    int count = ListView_GetItemCount(list);
    for (int i = 0; i < count; ++i) {
        VariableItem* item = ItemAt(list, i);
        if (item && item->name && _wcsicmp(item->name, name) == 0) return i;
    }
    return -1;
}

VariableItem* FindVariable(HWND dialog, PCWSTR name) {
    HWND list = GetDlgItem(dialog, kUserListControl);
    int index = FindItem(list, name);
    return index < 0 ? nullptr : ItemAt(list, index);
}

VariableItem* FindSystemVariable(HWND dialog, PCWSTR name) {
    HWND list = GetDlgItem(dialog, kSystemListControl);
    int index = FindItem(list, name);
    return index < 0 ? nullptr : ItemAt(list, index);
}

void SelectVariable(HWND dialog, PCWSTR name) {
    HWND list = GetDlgItem(dialog, kUserListControl);
    const int index = FindItem(list, name);
    Check(index >= 0, "Variable is missing from the native list");
    ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(list, index, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
}

std::wstring ReadRegistry(HKEY key, PCWSTR name) {
    DWORD type = 0, size = 0;
    Check(RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
          (type == REG_SZ || type == REG_EXPAND_SZ) && size >= sizeof(WCHAR),
          "Registry value query failed");
    std::vector<BYTE> bytes(size);
    Check(RegQueryValueExW(key, name, nullptr, &type, bytes.data(), &size) == ERROR_SUCCESS,
          "Registry value read failed");
    auto* text = reinterpret_cast<WCHAR*>(bytes.data());
    Check(text[size / sizeof(WCHAR) - 1] == L'\0', "Registry value is not terminated");
    return text;
}

void PutValue(HKEY key, PCWSTR name, const std::wstring& value, DWORD type = REG_SZ) {
    Check(RegSetValueExW(key, name, 0, type, reinterpret_cast<const BYTE*>(value.c_str()),
                         static_cast<DWORD>((value.size() + 1) * sizeof(WCHAR))) == ERROR_SUCCESS,
          "Test registry setup failed");
}

void SetEditByInput(HWND edit, const std::wstring& value) {
    Check(SetWindowTextW(edit, L"") != FALSE, "Clearing the edit control failed");
    SendMessageW(edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(value.c_str()));
    Check(static_cast<size_t>(GetWindowTextLengthW(edit)) == value.size(), "Input was truncated");
}

BOOL CALLBACK CloseDialogCallback(HWND hwnd, LPARAM) {
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
    return TRUE;
}

}  // namespace

int RunIntegrationTests() {
    HKEY root = nullptr, user = nullptr, machine = nullptr, system = nullptr;
    HANDLE thread = nullptr;
    HDESK desktop = nullptr;
    bool overriddenUser = false, overriddenMachine = false;
    DWORD threadId = 0;
    std::wstring keyName = L"Software\\EnvVarsUnlimitedTest-" + std::to_wstring(GetCurrentProcessId());
    int exitCode = 1;
    try {
        queryKey = reinterpret_cast<NtQueryKeyFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryKey"));
        Check(queryKey != nullptr, "NtQueryKey is unavailable");
        Check(RegCreateKeyExW(HKEY_CURRENT_USER, keyName.c_str(), 0, nullptr, REG_OPTION_VOLATILE,
                              KEY_ALL_ACCESS, nullptr, &root, nullptr) == ERROR_SUCCESS, "Creating the test root failed");
        Check(RegCreateKeyExW(root, L"Environment", 0, nullptr, REG_OPTION_VOLATILE,
                              KEY_ALL_ACCESS, nullptr, &user, nullptr) == ERROR_SUCCESS, "Creating the test user key failed");
        Check(RegCreateKeyExW(root, L"Machine", 0, nullptr, REG_OPTION_VOLATILE,
                              KEY_ALL_ACCESS, nullptr, &machine, nullptr) == ERROR_SUCCESS, "Creating the test machine root failed");
        Check(RegCreateKeyExW(machine, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment",
                              0, nullptr, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &system, nullptr) == ERROR_SUCCESS,
              "Creating the test system key failed");
        const std::wstring sentinel = L"do-not-change";
        const std::wstring existingMidPath = L"C:\\" + std::wstring(3000, L'm') + L";D:\\second;";
        const std::wstring clearLongPath = L"C:\\" + std::wstring(5000, L'c') + L";D:\\second;";
        const std::wstring clearShortPath = L"C:\\first;D:\\second;";
        PutValue(user, L"WH_TEST_SENTINEL", sentinel);
        PutValue(user, L"WH_TEST_DUPLICATE", L"old-value");
        PutValue(user, L"Path", L"C:\\first;D:\\second;", REG_EXPAND_SZ);
        PutValue(user, L"WH_TEST_MID_PATH", existingMidPath, REG_EXPAND_SZ);
        PutValue(user, L"WH_TEST_CLEAR_LONG", clearLongPath, REG_EXPAND_SZ);
        PutValue(user, L"WH_TEST_CLEAR_SHORT", clearShortPath, REG_EXPAND_SZ);
        // DLL loading and compatibility checks can themselves consult HKLM.
#ifdef ENV_VARS_TEST_WINDHAWK_INIT
        HMODULE module = TestLoadLibraryExW(L"sysdm.cpl", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
#else
        HMODULE module = LoadLibraryExW(L"sysdm.cpl", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
#endif
        if (!module) fprintf(stderr, "LoadLibraryEx(sysdm.cpl) failed: %lu\n", GetLastError());
        Check(module && IsSupportedBuild(module), "The test binary is not supported by this build");
#ifndef ENV_VARS_TEST_WINDHAWK_INIT
        // The standalone harness below uses addresses verified only for the
        // original 29708 sample. Never apply those addresses to another build.
        WCHAR localModulePath[MAX_PATH]{};
        BYTE localDigest[32]{};
        Check(GetModuleFileNameW(module, localModulePath, ARRAYSIZE(localModulePath)) &&
                  HashFile(localModulePath, localDigest) &&
                  memcmp(localDigest, kBuildProfiles[0].sha256, sizeof(localDigest)) == 0,
              "Fixed-RVA test requires sysdm.cpl 10.0.29708.1003");
#endif
#ifdef ENV_VARS_TEST_WINDHAWK_INIT
        WCHAR samplePath[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"ENV_VARS_TEST_SAMPLE", samplePath, ARRAYSIZE(samplePath))) {
            WCHAR systemPath[MAX_PATH]{};
            Check(GetSystemDirectoryW(systemPath, ARRAYSIZE(systemPath)) != 0,
                  "Could not locate the test resources");
            std::wstring resourcePath = std::wstring(systemPath) + L"\\en-US\\sysdm.cpl.mui";
            sampleResources = LoadLibraryExW(resourcePath.c_str(), nullptr,
                                              LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
            sampleModule = module;
            Check(sampleResources != nullptr, "Could not load the host's test dialog resources");
            // The current host template contains a resize-only helper class
            // absent from older binaries. This private-desktop experiment does
            // not test layout, so a no-op window class supplies that resource.
            WNDCLASSW resizeClass{};
            resizeClass.lpfnWndProc = DefWindowProcW;
            resizeClass.hInstance = sampleResources;
            for (PCWSTR name : {L"DialogResizeControlClassW", L"DialogResizeDataClassW"}) {
                resizeClass.lpszClassName = name;
                Check(RegisterClassW(&resizeClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
                      "Could not register the test resize helper");
            }
        }
#endif
        testRootPath = KeyPath(root);
        Check(!testRootPath.empty(), "Could not identify the test root");
        Check(RegOverridePredefKey(HKEY_CURRENT_USER, root) == ERROR_SUCCESS, "Overriding HKCU failed");
        overriddenUser = true;
        Check(RegOverridePredefKey(HKEY_LOCAL_MACHINE, machine) == ERROR_SUCCESS, "Overriding HKLM failed");
        overriddenMachine = true;
        HKEY check = nullptr;
        Check(RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_READ, &check) == ERROR_SUCCESS &&
              IsTestKey(check), "HKCU was not redirected to the test tree");
        RegCloseKey(check);
        Check(RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment",
                            0, KEY_READ, &check) == ERROR_SUCCESS && IsTestKey(check),
              "HKLM was not redirected to the test tree");
        RegCloseKey(check);

        desktop = CreateDesktopW((L"EnvVarsUnlimitedTest-" + std::to_wstring(GetCurrentProcessId())).c_str(),
                                 nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        Check(desktop && SetThreadDesktop(desktop), "Could not create the private test desktop");
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
        Check(InitCommonControlsEx(&controls), "List view initialization failed");
        Check(MH_Initialize() == MH_OK, "Test hook initialization failed");
#ifdef ENV_VARS_TEST_WINDHAWK_INIT
        if (sampleModule) {
            Hook(reinterpret_cast<void*>(DialogBoxParamW), SampleDialogBoxParam, &realDialogBoxParam);
        }
#endif
        HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
        Check(kernelbase != nullptr, "kernelbase.dll is unavailable");
#ifdef ENV_VARS_TEST_WINDHAWK_INIT
        Check(Wh_ModInit() != FALSE, "Wh_ModInit failed in the rundll32 host");
        Check(g_valueBuffer && g_nameBuffer && g_editSystemVars && g_nextPath,
              "Wh_ModInit did not resolve the required data symbols");
#else
        g_valueBuffer = reinterpret_cast<PWSTR>(reinterpret_cast<BYTE*>(module) + 0x2b300);
        g_nameBuffer = reinterpret_cast<PWSTR>(reinterpret_cast<BYTE*>(module) + 0x2d300);
        g_editSystemVars = reinterpret_cast<BOOL*>(reinterpret_cast<BYTE*>(module) + 0x30018);
        g_nextPath = reinterpret_cast<NextPathFn>(GetProcAddress(GetModuleHandleW(L"shlwapi.dll"), MAKEINTRESOURCEA(450)));
        Check(g_nextPath != nullptr, "NextPathW is unavailable");
        HookRva(module, 0xbc84, EditHook, &g_editOriginal);
        HookRva(module, 0xb7cc, DeleteHook, &g_deleteOriginal);
        HookRva(module, 0xc9c0, SetHook, &g_setOriginal);
        HookRva(module, 0xcdd0, AddHook, &g_addOriginal);
        HookRva(module, 0xd0e0, LoadHook, &g_loadOriginal);
        HookRva(module, 0xbb14, CommandHook, &g_commandOriginal);
        HookRva(module, 0xd470, MainDialogHook, &g_mainDialogOriginal);
        HookRva(module, 0xb570, TextDialogHook, &g_textDialogOriginal);
        HookRva(module, 0xb620, PathDialogHook, &g_pathDialogOriginal);
        HookRva(module, 0xb2c4, RebuildHook, &g_rebuildOriginal);
        HookRva(module, 0xc888, IsMultiHook, &g_isMultiOriginal);
        Hook(reinterpret_cast<void*>(GetProcAddress(kernelbase, "RegEnumValueW")), EnumValueHook, &g_enumValueOriginal);
#endif
        Hook(reinterpret_cast<void*>(GetProcAddress(kernelbase, "RegSetValueExW")), SafeSetValue, &realSetValue);
        Hook(reinterpret_cast<void*>(GetProcAddress(kernelbase, "RegDeleteValueW")), SafeDeleteValue, &realDeleteValue);
        Hook(reinterpret_cast<void*>(SendNotifyMessageW), SafeNotify, &realNotify);
        void* ignored = nullptr;
        Hook(reinterpret_cast<void*>(MessageBoxW), TestMessageBox, &ignored);
        HMODULE winsta = GetModuleHandleW(L"winsta.dll");
        Check(winsta != nullptr, "winsta.dll is unavailable");
        Hook(reinterpret_cast<void*>(GetProcAddress(winsta, "WinStationBroadcastSystemMessage")),
             SafeWinStationBroadcast, &ignored);
        Check(MH_EnableHook(MH_ALL_HOOKS) == MH_OK, "Enabling the test hooks failed");
#ifdef ENV_VARS_TEST_WINDHAWK_INIT
        Wh_ModAfterInit();
#endif

        UiThread ui{desktop, module};
        thread = CreateThread(nullptr, 0, RunDialog, &ui, 0, &threadId);
        Check(thread != nullptr, "Creating the dialog thread failed");
        HWND main = WaitDialog(threadId, kUserListControl);
        Check(ui.attached, "The dialog thread could not use the private desktop");
        Check(FindVariable(main, L"WH_TEST_SENTINEL") != nullptr, "The test environment was not loaded");

        // Emptying a long PATH list must behave the same as the native short
        // case, including whether the dialog allows the empty list to close.
        bool clearEnabled[2]{};
        int clearResult[2]{};  // deleted, empty, unchanged, other
        const PCWSTR clearNames[] = {L"WH_TEST_CLEAR_SHORT", L"WH_TEST_CLEAR_LONG"};
        const std::wstring* clearOriginal[] = {&clearShortPath, &clearLongPath};
        for (int n = 0; n < 2; ++n) {
            SelectVariable(main, clearNames[n]);
            Click(main, 413);
            HWND clearDialog = WaitDialog(threadId, kPathListControl);
            HWND list = GetDlgItem(clearDialog, kPathListControl);
            while (ListView_GetItemCount(list) > 0) {
                const int before = ListView_GetItemCount(list);
                ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                                      LVIS_SELECTED | LVIS_FOCUSED);
                SendMessageW(clearDialog, WM_COMMAND, MAKEWPARAM(114, BN_CLICKED),
                             reinterpret_cast<LPARAM>(GetDlgItem(clearDialog, 114)));
                Check(ListView_GetItemCount(list) < before, "Deleting a path entry failed");
            }
            clearEnabled[n] = IsWindowEnabled(GetDlgItem(clearDialog, IDOK)) != FALSE;
            Click(clearDialog, clearEnabled[n] ? IDOK : IDCANCEL);
            WaitClosed(clearDialog);
            VariableItem* item = FindVariable(main, clearNames[n]);
            clearResult[n] = !item ? 0 : !item->value[0] ? 1 :
                             item->value == *clearOriginal[n] ? 2 : 3;
        }
        if (clearEnabled[0] != clearEnabled[1] || clearResult[0] != clearResult[1])
            fprintf(stderr, "Clearing path: short=(%d,%d) long=(%d,%d)\n",
                    clearEnabled[0], clearResult[0], clearEnabled[1], clearResult[1]);
        Check(clearEnabled[0] == clearEnabled[1] && clearResult[0] == clearResult[1],
              "Clearing a long path behaved differently from clearing a short path");

        // Reproduce saving an already-long PATH-style value without changes.
        SelectVariable(main, L"WH_TEST_MID_PATH");
        Click(main, 413);
        HWND pathDialog = WaitDialog(threadId, kPathListControl);
        Click(pathDialog, IDOK);
        WaitClosed(pathDialog);
        VariableItem* midPathItem = FindVariable(main, L"WH_TEST_MID_PATH");
        Check(midPathItem && midPathItem->value == existingMidPath,
              "Saving an existing 2048-4095 character path failed");

        // Simulate the native privileged system-variable mode, but with HKLM
        // already redirected and all registry writes confined to the test tree.
        *g_editSystemVars = TRUE;
        EnableWindow(GetDlgItem(main, 407), TRUE);
        Click(main, 407);
        HWND editDialog = WaitDialog(threadId, kValueControl);
        Check(SetDlgItemTextW(editDialog, 101, L"WH_TEST_SYSTEM"), "Setting the system variable name failed");
        const std::wstring systemValue = L"%SystemRoot%\\" + std::wstring(5000, L'S');
        SetEditByInput(GetDlgItem(editDialog, kValueControl), systemValue);
        Click(editDialog, IDOK);
        WaitClosed(editDialog);
        VariableItem* systemItem = FindSystemVariable(main, L"WH_TEST_SYSTEM");
        Check(systemItem && systemItem->value == systemValue &&
              wcsstr(systemItem->expanded, L"%SystemRoot%") != nullptr,
              "The system variable or its displayed expansion is incorrect");

        Click(main, 412);  // New user variable.
        editDialog = WaitDialog(threadId, kValueControl);
        HWND valueControl = GetDlgItem(editDialog, kValueControl);
        Check(SendMessageW(valueControl, EM_GETLIMITTEXT, 0, 0) == kMaxValueChars, "The input limit was not lifted");
        Check(SetDlgItemTextW(editDialog, 101, L"WH_TEST_LONG"), "Setting the variable name failed");
        for (size_t length : {2047u, 2048u, 4095u, 4096u, 32766u}) {
            SetEditByInput(valueControl, std::wstring(length, L'X'));
        }
        SendMessageW(valueControl, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L"Y"));
        Check(GetWindowTextLengthW(valueControl) == kMaxValueChars, "The upper input bound was not enforced");
        Click(editDialog, IDOK);
        WaitClosed(editDialog);
        VariableItem* longItem = FindVariable(main, L"WH_TEST_LONG");
        Check(longItem && std::wstring(longItem->value) == std::wstring(32766, L'X'), "The complete new value was not staged");

        for (size_t length : {2048u, 4095u, 4096u}) {
            std::wstring name = L"WH_TEST_N" + std::to_wstring(length);
            Click(main, 412);
            editDialog = WaitDialog(threadId, kValueControl);
            Check(SetDlgItemTextW(editDialog, 101, name.c_str()), "Setting a boundary test name failed");
            SetEditByInput(GetDlgItem(editDialog, kValueControl), std::wstring(length, L'B'));
            Click(editDialog, IDOK);
            WaitClosed(editDialog);
            VariableItem* boundary = FindVariable(main, name.c_str());
            Check(boundary && std::wstring(boundary->value) == std::wstring(length, L'B'), "A boundary value was not staged intact");
        }

        SelectVariable(main, L"WH_TEST_LONG");
        Click(main, 413);
        editDialog = WaitDialog(threadId, kValueControl);
        Check(GetWindowTextLengthW(GetDlgItem(editDialog, kValueControl)) == kMaxValueChars,
              "Reopening a long text value truncated it");
        SetEditByInput(GetDlgItem(editDialog, kValueControl), L"cancelled");
        Click(editDialog, IDCANCEL);
        WaitClosed(editDialog);
        longItem = FindVariable(main, L"WH_TEST_LONG");
        Check(longItem && std::wstring(longItem->value) == std::wstring(32766, L'X'), "Cancel changed the long value");

        SelectVariable(main, L"Path");
        Click(main, 413);
        pathDialog = WaitDialog(threadId, kPathListControl);
        HWND pathList = GetDlgItem(pathDialog, kPathListControl);
        Check(ListView_GetItemCount(pathList) == 2, "The path list was not initialized");
        const std::wstring firstPath = L"C:\\" + std::wstring(5000, L'p');
        LVITEMW pathText{};
        pathText.iSubItem = 0;
        pathText.pszText = const_cast<PWSTR>(firstPath.c_str());
        Check(SendMessageW(pathList, LVM_SETITEMTEXTW, 0, reinterpret_cast<LPARAM>(&pathText)),
              "Updating the path item failed");
        // Native move buttons use a fixed-size temporary buffer on some builds.
        // Moving a long item must preserve it exactly in both directions.
        ListView_SetItemState(pathList, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(pathList, 0, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        SendMessageW(pathDialog, WM_COMMAND, MAKEWPARAM(116, BN_CLICKED),
                     reinterpret_cast<LPARAM>(GetDlgItem(pathDialog, 116)));
        std::vector<WCHAR> moved(6000);
        ListView_GetItemText(pathList, 1, 0, moved.data(), static_cast<int>(moved.size()));
        Check(moved.data() == firstPath, "Moving a long path item down truncated it");
        SendMessageW(pathDialog, WM_COMMAND, MAKEWPARAM(115, BN_CLICKED),
                     reinterpret_cast<LPARAM>(GetDlgItem(pathDialog, 115)));
        ListView_GetItemText(pathList, 0, 0, moved.data(), static_cast<int>(moved.size()));
        Check(moved.data() == firstPath, "Moving a long path item up truncated it");
        // Exercise the real in-place list-view editor, including its limit.
        SendMessageW(pathDialog, WM_COMMAND, MAKEWPARAM(112, BN_CLICKED),
                     reinterpret_cast<LPARAM>(GetDlgItem(pathDialog, 112)));
        HWND inlineEdit = nullptr;
        for (int i = 0; i < 300 && !inlineEdit; ++i) {
            inlineEdit = reinterpret_cast<HWND>(SendMessageW(pathList, LVM_GETEDITCONTROL, 0, 0));
            if (!inlineEdit) Sleep(10);
        }
        Check(inlineEdit != nullptr, "The path in-place editor did not open");
        Check(GetWindowTextLengthW(inlineEdit) == static_cast<int>(firstPath.size()),
              "The path in-place editor truncated the existing entry");
        SendMessageW(inlineEdit, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
        SendMessageW(inlineEdit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L"q"));
        Check(GetWindowTextLengthW(inlineEdit) == static_cast<int>(firstPath.size() + 1),
              "The path in-place editor rejected additional input");
        SendMessageW(inlineEdit, WM_KEYDOWN, VK_RETURN, 0);
        ListView_GetItemText(pathList, 0, 0, moved.data(), static_cast<int>(moved.size()));
        if (moved.data() != firstPath + L"q") {
            fprintf(stderr, "Inline edit: expected %zu, row %zu, still open %d\n",
                    firstPath.size() + 1, wcslen(moved.data()), IsWindow(inlineEdit));
        }
        Check(moved.data() == firstPath + L"q", "Committing the path in-place editor lost data");
        pathText.pszText = const_cast<PWSTR>(firstPath.c_str());
        Check(SendMessageW(pathList, LVM_SETITEMTEXTW, 0, reinterpret_cast<LPARAM>(&pathText)),
              "Restoring the path item after the in-place test failed");
        Click(pathDialog, IDOK);
        WaitClosed(pathDialog);
        const std::wstring expectedPath = firstPath + L";D:\\second;";
        VariableItem* pathItem = FindVariable(main, L"Path");
        Check(pathItem && pathItem->value == expectedPath, "The long path was not staged intact");
        Check(ReadRegistry(user, L"WH_TEST_SENTINEL") == sentinel, "The test registry changed before confirmation");

        SelectVariable(main, L"Path");
        Click(main, 413);
        pathDialog = WaitDialog(threadId, kPathListControl);
        pathList = GetDlgItem(pathDialog, kPathListControl);
        const std::wstring excessivePath = L"C:\\" + std::wstring(kMaxValueChars, L'p');
        pathText.pszText = const_cast<PWSTR>(excessivePath.c_str());
        Check(SendMessageW(pathList, LVM_SETITEMTEXTW, 0, reinterpret_cast<LPARAM>(&pathText)),
              "Setting the excessive path test value failed");
        int beforeErrors = messageBoxCount.load();
        expectMessageBox = true;
        Click(pathDialog, IDOK);
        for (int i = 0; i < 300 && messageBoxCount.load() == beforeErrors; ++i) Sleep(10);
        expectMessageBox = false;
        Check(messageBoxCount.load() > beforeErrors && IsWindow(pathDialog), "An excessive path was not blocked");
        Click(pathDialog, IDCANCEL);
        WaitClosed(pathDialog);
        pathItem = FindVariable(main, L"Path");
        Check(pathItem && pathItem->value == expectedPath, "Cancelling the excessive path changed the staged value");

        Click(main, IDOK);
        Check(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0, "The main dialog did not finish");
        CloseHandle(thread);
        thread = nullptr;
        Check(ReadRegistry(user, L"WH_TEST_LONG") == std::wstring(32766, L'X'), "The stored long value was truncated");
        Check(ReadRegistry(user, L"Path") == expectedPath, "The stored path was truncated");
        Check(ReadRegistry(user, L"WH_TEST_MID_PATH") == existingMidPath,
              "The existing 2048-4095 character path was not saved intact");
        Check(ReadRegistry(user, L"WH_TEST_SENTINEL") == sentinel, "An unrelated value changed");
        Check(ReadRegistry(system, L"WH_TEST_SYSTEM") == systemValue, "The system variable was not saved intact");
        Check(broadcasts.load() == 1, "A successful transaction did not broadcast exactly once");
        for (size_t length : {2048u, 4095u, 4096u}) {
            std::wstring name = L"WH_TEST_N" + std::to_wstring(length);
            Check(ReadRegistry(user, name.c_str()) == std::wstring(length, L'B'), "A boundary value was truncated in the registry");
        }

        thread = CreateThread(nullptr, 0, RunDialog, &ui, 0, &threadId);
        Check(thread != nullptr, "Recreating the dialog thread failed");
        main = WaitDialog(threadId, kUserListControl);
        longItem = FindVariable(main, L"WH_TEST_LONG");
        pathItem = FindVariable(main, L"Path");
        systemItem = FindSystemVariable(main, L"WH_TEST_SYSTEM");
        if (!longItem || std::wstring(longItem->value) != std::wstring(32766, L'X')) {
            HWND debugList = GetDlgItem(main, kUserListControl);
            fprintf(stderr, "Reload list count: %d\n", ListView_GetItemCount(debugList));
            for (int i = 0; i < ListView_GetItemCount(debugList); ++i) {
                VariableItem* item = ItemAt(debugList, i);
                if (item && item->name && item->value)
                    fwprintf(stderr, L"  %ls length=%llu\n", item->name, static_cast<unsigned long long>(wcslen(item->value)));
            }
        }
        Check(longItem && std::wstring(longItem->value) == std::wstring(32766, L'X'), "Reloading a long value failed");
        Check(pathItem && pathItem->value == expectedPath, "Reloading a long path failed");
        Check(systemItem && systemItem->value == systemValue &&
              wcsstr(systemItem->expanded, L"%SystemRoot%") != nullptr,
              "Reloading the long system variable failed");
        for (size_t length : {2048u, 4095u, 4096u}) {
            std::wstring name = L"WH_TEST_N" + std::to_wstring(length);
            VariableItem* boundary = FindVariable(main, name.c_str());
            Check(boundary && std::wstring(boundary->value) == std::wstring(length, L'B'), "Reloading a boundary value failed");
        }
        SelectVariable(main, L"Path");
        Click(main, 413);
        pathDialog = WaitDialog(threadId, kPathListControl);
        pathList = GetDlgItem(pathDialog, kPathListControl);
        std::vector<WCHAR> displayed(6000);
        ListView_GetItemText(pathList, 0, 0, displayed.data(), static_cast<int>(displayed.size()));
        Check(displayed.data() == firstPath, "Reopening the long path did not restore its first item");
        Click(pathDialog, 117);  // Edit text, preserving the original long path.
        WaitClosed(pathDialog);
        editDialog = WaitDialog(threadId, kValueControl);
        std::wstring textPath;
        Check(ReadControl(GetDlgItem(editDialog, kValueControl), textPath) && textPath == expectedPath,
              "Switching from the path list to text lost data");
        Click(editDialog, IDCANCEL);
        WaitClosed(editDialog);

        // Repeat the unchanged-save case for a value larger than the native
        // 4096-character work buffer, then commit it again with the parent.
        SelectVariable(main, L"Path");
        Click(main, 413);
        pathDialog = WaitDialog(threadId, kPathListControl);
        Click(pathDialog, IDOK);
        WaitClosed(pathDialog);
        pathItem = FindVariable(main, L"Path");
        Check(pathItem && pathItem->value == expectedPath,
              "Saving an existing path above 4095 characters failed");

        SelectVariable(main, L"WH_TEST_LONG");
        Click(main, 413);
        editDialog = WaitDialog(threadId, kValueControl);
        SetEditByInput(GetDlgItem(editDialog, kValueControl), std::wstring(5000, L'Z'));
        Check(SetDlgItemTextW(editDialog, 101, L"WH_TEST_DUPLICATE"), "Renaming the long variable failed");
        Click(editDialog, IDOK);
        WaitClosed(editDialog);
        Check(ReadRegistry(user, L"WH_TEST_LONG") == std::wstring(32766, L'X') &&
              ReadRegistry(user, L"WH_TEST_DUPLICATE") == L"old-value", "An edit changed the registry before confirmation");
        Click(main, IDOK);
        Check(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0, "The second dialog did not finish");
        CloseHandle(thread);
        thread = nullptr;
        Check(ReadRegistry(user, L"WH_TEST_DUPLICATE") == std::wstring(5000, L'Z'), "Renaming or replacing the existing variable failed");
        Check(RegQueryValueExW(user, L"WH_TEST_LONG", nullptr, nullptr, nullptr, nullptr) == ERROR_FILE_NOT_FOUND,
              "The old variable name was not removed");
        Check(ReadRegistry(user, L"Path") == expectedPath, "Cancel or an unrelated edit changed the path");
        Check(ReadRegistry(system, L"WH_TEST_SYSTEM") == systemValue, "The system value changed unexpectedly");

        // Fail in the system scope after the user scope has already been
        // written inside the same transaction. Neither scope may commit.
        thread = CreateThread(nullptr, 0, RunDialog, &ui, 0, &threadId);
        Check(thread != nullptr, "Creating the rollback dialog failed");
        main = WaitDialog(threadId, kUserListControl);
        *g_editSystemVars = TRUE;
        Click(main, 412);
        editDialog = WaitDialog(threadId, kValueControl);
        Check(SetDlgItemTextW(editDialog, 101, L"WH_TEST_FAIL"), "Setting the rollback test name failed");
        SetEditByInput(GetDlgItem(editDialog, kValueControl), std::wstring(4096, L'F'));
        Click(editDialog, IDOK);
        WaitClosed(editDialog);
        beforeErrors = messageBoxCount.load();
        const int beforeBroadcasts = broadcasts.load();
        const int beforeCrossSessionBroadcasts = crossSessionBroadcasts.load();
        injectSystemFailure = true;
        expectMessageBox = true;
        Click(main, IDOK);
        for (int i = 0; i < 300 && messageBoxCount.load() == beforeErrors; ++i) Sleep(10);
        injectSystemFailure = false;
        expectMessageBox = false;
        if (!(injectedFailureObserved && messageBoxCount.load() > beforeErrors && IsWindow(main))) {
            fprintf(stderr, "Rollback diagnostic: injected=%d messages=%d before=%d window=%d system=%d\n",
                    static_cast<int>(injectedFailureObserved.load()), messageBoxCount.load(), beforeErrors,
                    IsWindow(main), *g_editSystemVars);
            fwprintf(stderr, L"Last message: %ls\n", lastMessageBoxText);
        }
        Check(injectedFailureObserved && messageBoxCount.load() > beforeErrors && IsWindow(main),
              "A failed system write did not keep the dialog open");
        Check(RegQueryValueExW(user, L"WH_TEST_FAIL", nullptr, nullptr, nullptr, nullptr) == ERROR_FILE_NOT_FOUND &&
              ReadRegistry(user, L"WH_TEST_DUPLICATE") == std::wstring(5000, L'Z') &&
              ReadRegistry(system, L"WH_TEST_SYSTEM") == systemValue && broadcasts.load() == beforeBroadcasts &&
              crossSessionBroadcasts.load() == beforeCrossSessionBroadcasts,
              "A failed transaction changed a scope or broadcast a change");
        Click(main, IDCANCEL);
        Check(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0, "The rollback dialog did not cancel");
        CloseHandle(thread);
        thread = nullptr;

        // Do not overwrite a value changed externally while the dialog was open.
        thread = CreateThread(nullptr, 0, RunDialog, &ui, 0, &threadId);
        Check(thread != nullptr, "Creating the concurrent-change dialog failed");
        main = WaitDialog(threadId, kUserListControl);
        PutValue(user, L"WH_TEST_SENTINEL", L"external-update");
        beforeErrors = messageBoxCount.load();
        expectMessageBox = true;
        Click(main, IDOK);
        for (int i = 0; i < 300 && messageBoxCount.load() == beforeErrors; ++i) Sleep(10);
        expectMessageBox = false;
        Check(messageBoxCount.load() > beforeErrors && IsWindow(main) &&
              ReadRegistry(user, L"WH_TEST_SENTINEL") == L"external-update",
              "An external registry change was not protected");
        Click(main, IDCANCEL);
        Check(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0, "The concurrent-change dialog did not cancel");
        CloseHandle(thread);
        thread = nullptr;
        PutValue(user, L"WH_TEST_SENTINEL", sentinel);

        // A malformed string must never cause the native delete-and-rewrite
        // save routine to overwrite the rest of the environment.
        const WCHAR malformed[] = {L'b', L'a', L'd'};
        WCHAR malformedName[] = L"WH_TEST_MALFORMED";
        NativeString nativeName{sizeof(malformedName) - sizeof(WCHAR), sizeof(malformedName), malformedName};
        auto nativeSet = reinterpret_cast<NtSetValueKeyFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetValueKey"));
        Check(nativeSet && IsTestKey(user) &&
                  nativeSet(user, &nativeName, 0, REG_SZ, const_cast<WCHAR*>(malformed), sizeof(malformed)) == 0,
              "Creating malformed test data failed");
        thread = CreateThread(nullptr, 0, RunDialog, &ui, 0, &threadId);
        Check(thread != nullptr, "Creating the fail-closed dialog failed");
        main = WaitDialog(threadId, kUserListControl);
        beforeErrors = messageBoxCount.load();
        expectMessageBox = true;
        Click(main, IDOK);
        for (int i = 0; i < 300 && messageBoxCount.load() == beforeErrors; ++i) Sleep(10);
        expectMessageBox = false;
        if (!(messageBoxCount.load() > beforeErrors && IsWindow(main)))
            fprintf(stderr, "Fail-closed result: messages=%d before=%d window=%d\n",
                    messageBoxCount.load(), beforeErrors, IsWindow(main));
        Check(messageBoxCount.load() > beforeErrors && IsWindow(main), "Malformed data did not block save");
        Click(main, IDCANCEL);
        Check(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0, "The fail-closed dialog did not cancel");
        CloseHandle(thread);
        thread = nullptr;
        Check(ReadRegistry(user, L"WH_TEST_SENTINEL") == sentinel &&
              ReadRegistry(user, L"WH_TEST_DUPLICATE") == std::wstring(5000, L'Z'),
              "A blocked save changed the test registry");
        Check(!forbiddenWrite, "A registry write outside the isolated tree was blocked");
        puts("PASS: user and system values, long paths, transaction rollback, concurrent changes and fail-closed guards");
        exitCode = 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "FAIL: %s\n", error.what());
    }
    if (thread) {
        EnumThreadWindows(threadId, CloseDialogCallback, 0);
        if (WaitForSingleObject(thread, 3000) != WAIT_OBJECT_0) {
            fprintf(stderr, "Test dialog did not exit; process termination will release the registry overrides.\n");
            return 1;
        }
        CloseHandle(thread);
    }
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    if (overriddenMachine) RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
    if (overriddenUser) RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    if (system) RegCloseKey(system);
    if (machine) RegCloseKey(machine);
    if (user) RegCloseKey(user);
    if (root) RegCloseKey(root);
    RegDeleteTreeW(HKEY_CURRENT_USER, keyName.c_str());
    if (desktop) CloseDesktop(desktop);
    return exitCode;
}

#ifdef ENV_VARS_TEST_WINDHAWK_INIT
DWORD WINAPI RunIntegrationThread(void*) {
    return static_cast<DWORD>(RunIntegrationTests());
}

extern "C" __declspec(dllexport) void CALLBACK RunInitTest(HWND, HINSTANCE, LPSTR, int) {
    WCHAR path[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"ENV_VARS_TEST_OUTPUT", path, ARRAYSIZE(path))) ExitProcess(2);
    FILE* output = _wfopen(path, L"w");
    if (!output) ExitProcess(2);
    fclose(output);
    if (!_wfreopen(path, L"a", stdout) || !_wfreopen(path, L"a", stderr)) ExitProcess(2);
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    // rundll32's entry thread can already own windows. A fresh thread can
    // safely attach to the isolated test desktop before creating any UI.
    HANDLE worker = CreateThread(nullptr, 0, RunIntegrationThread, nullptr, 0, nullptr);
    if (!worker || WaitForSingleObject(worker, 90000) != WAIT_OBJECT_0) ExitProcess(2);
    DWORD result = 2;
    GetExitCodeThread(worker, &result);
    CloseHandle(worker);
    fflush(stdout);
    fflush(stderr);
    ExitProcess(result);
}
#else
int main() { return RunIntegrationTests(); }
#endif
