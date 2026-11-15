// ==WindhawkMod==
// @id              env-vars-unlimited
// @name            Environment Variables Unlimited
// @name:zh-CN      解除环境变量编辑器长度限制
// @description     Allow longer values in the classic Environment Variables dialogs
// @description:zh-CN 允许在经典环境变量对话框中编辑更长的值
// @version         0.1.1
// @author          alkali210
// @github          https://github.com/alkali210
// @include         SystemPropertiesAdvanced.exe
// @include         rundll32.exe
// @include         control.exe
// @architecture    x86-64
// @compilerOptions -lcomctl32 -lversion -ladvapi32 -lktmw32 -lbcrypt
// @license         GPL-3.0-or-later
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Environment Variables Unlimited

An experimental mod for the classic Windows Environment Variables dialogs. It
supports values up to 32,766 UTF-16 code units, retains the native dialogs, and
uses a registry transaction when saving. It is limited to the exact sysdm.cpl
build and file hash documented in the project README. Close existing environment
dialogs before enabling or disabling the mod.
*/
// ==/WindhawkModReadme==

#include <windhawk_utils.h>

#include <commctrl.h>
#include <bcrypt.h>
#include <ktmw32.h>
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <memory>
#include <string>
#include <vector>

namespace {

// These private interfaces and the list item layout were verified for this
// exact sysdm.cpl version. A symbol match alone does not prove ABI compatibility.
// The PDB identity and complete file hash pin the analyzed binary. Version
// information is not used: compatibility shims can change its reported value
// depending on the host process, even when the DLL bytes are identical.
constexpr GUID kSupportedPdb = {0x6fd15f0d, 0xdf7d, 0x215f,
                                {0xcf, 0x0d, 0x41, 0xf3, 0x5b, 0x3a, 0xa8, 0x78}};
constexpr BYTE kSupportedSha256[32] = {
    0x20, 0x57, 0xc7, 0x91, 0x0a, 0x57, 0x68, 0x36,
    0x81, 0x22, 0xe6, 0x90, 0x2b, 0x79, 0x9e, 0x2c,
    0xf4, 0x37, 0xb2, 0x86, 0x58, 0x72, 0x1d, 0x9b,
    0x03, 0xb6, 0x74, 0xac, 0x76, 0xc1, 0x0e, 0x99,
};
constexpr size_t kNativeBufferChars = 4096;
constexpr size_t kNativeInputLimit = 2047;
// The documented process environment limit includes the terminating NUL.
constexpr size_t kMaxValueChars = 32766;
constexpr int kValueControl = 103;
constexpr int kPathListControl = 110;
constexpr int kSystemListControl = 400;
constexpr int kUserListControl = 402;

struct VariableItem {
    DWORD type;
    PWSTR name;
    PWSTR value;
    PWSTR expanded;
};
static_assert(sizeof(VariableItem) == 32);

using EditFn = void (*)(HWND, unsigned, unsigned, int);
using DeleteFn = void (*)(HWND, unsigned, PCWSTR);
using SetFn = void (*)(HWND, unsigned, PCWSTR, PCWSTR);
using AddFn = HRESULT (*)(HWND, DWORD, unsigned, PCWSTR, PCWSTR, DWORD);
using CommandFn = void (*)(HWND, HWND, int, int);
using DialogFn = INT_PTR(CALLBACK*)(HWND, UINT, WPARAM, LPARAM);
using RebuildFn = bool (*)(HWND, bool);
using IsMultiFn = BOOL (*)(PWSTR);
using NextPathFn = PCWSTR(WINAPI*)(PCWSTR, PWSTR, UINT);
using LoadFn = DWORD (*)(HKEY, HWND, unsigned);
using EnumValueFn = decltype(&RegEnumValueW);

EditFn g_editOriginal;
DeleteFn g_deleteOriginal;
SetFn g_setOriginal;
AddFn g_addOriginal;
CommandFn g_commandOriginal;
DialogFn g_mainDialogOriginal;
DialogFn g_textDialogOriginal;
DialogFn g_pathDialogOriginal;
RebuildFn g_rebuildOriginal;
IsMultiFn g_isMultiOriginal;
NextPathFn g_nextPath;
LoadFn g_loadOriginal;
EnumValueFn g_enumValueOriginal;
PWSTR g_valueBuffer;
PWSTR g_nameBuffer;
BOOL* g_editSystemVars;

struct EditContext {
    HWND owner = nullptr;
    unsigned scope = 0;
    VariableItem* oldItem = nullptr;
    PWSTR originalValuePointer = nullptr;
    PWSTR marker = nullptr;
    std::wstring initialValue;
    std::wstring pendingValue;
    std::wstring preparedPath;
    bool pending = false;
    bool pathPrepared = false;
};

thread_local EditContext* g_edit;
// If loading any item fails, never commit a partial list of variables. The
// main dialog runs on the same UI thread as loading.
thread_local bool g_loadFailed;

struct RegistryEntry {
    std::wstring name;
    DWORD type = 0;
    std::vector<BYTE> data;

    bool operator==(const RegistryEntry& other) const {
        return name == other.name && type == other.type && data == other.data;
    }
};

struct RegistrySnapshot {
    std::vector<RegistryEntry> entries;
    FILETIME lastWrite{};
    DWORD maxDataBytes = 0;
    bool valid = false;
};

thread_local RegistrySnapshot g_snapshots[2];  // system, user

bool CaptureSnapshot(HKEY key, RegistrySnapshot& result) {
    DWORD count = 0, maxName = 0, maxData = 0;
    FILETIME before{}, after{};
    if (RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                         &count, &maxName, &maxData, nullptr, &before) != ERROR_SUCCESS ||
        maxName == MAXDWORD || maxData == MAXDWORD) return false;
    RegistrySnapshot snapshot;
    snapshot.maxDataBytes = maxData;
    snapshot.entries.reserve(count);
    std::vector<WCHAR> name(static_cast<size_t>(maxName) + 1);
    std::vector<BYTE> data(std::max<size_t>(maxData, 1));
    for (DWORD i = 0; i < count; ++i) {
        DWORD nameLength = static_cast<DWORD>(name.size());
        DWORD dataLength = static_cast<DWORD>(data.size());
        DWORD type = 0;
        if (RegEnumValueW(key, i, name.data(), &nameLength, nullptr, &type,
                          data.data(), &dataLength) != ERROR_SUCCESS ||
            nameLength >= name.size() || dataLength > data.size()) return false;
        RegistryEntry entry;
        entry.name.assign(name.data(), nameLength);
        entry.type = type;
        entry.data.assign(data.begin(), data.begin() + dataLength);
        snapshot.entries.push_back(std::move(entry));
    }
    DWORD afterCount = 0;
    if (RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                         &afterCount, nullptr, nullptr, nullptr, &after) != ERROR_SUCCESS ||
        count != afterCount || CompareFileTime(&before, &after) != 0) return false;
    std::sort(snapshot.entries.begin(), snapshot.entries.end(),
              [](const RegistryEntry& a, const RegistryEntry& b) { return a.name < b.name; });
    snapshot.lastWrite = after;
    snapshot.valid = true;
    result = std::move(snapshot);
    return true;
}

bool SameSnapshot(const RegistrySnapshot& a, const RegistrySnapshot& b) {
    return a.valid && b.valid && CompareFileTime(&a.lastWrite, &b.lastWrite) == 0 &&
           a.entries == b.entries;
}

struct LoadContext {
    HKEY key;
    HWND list;
    std::wstring name;
    std::wstring value;
    bool captured = false;
};
thread_local LoadContext* g_load;

void Error(HWND owner, PCWSTR message) {
    MessageBoxW(owner, message, L"Environment Variables Unlimited",
                MB_OK | MB_ICONERROR);
}

HWND VariableList(HWND owner, int scope) {
    return GetDlgItem(owner, scope == 1 ? kSystemListControl : kUserListControl);
}

VariableItem* ItemAt(HWND list, int index) {
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = index;
    return ListView_GetItem(list, &item)
               ? reinterpret_cast<VariableItem*>(item.lParam)
               : nullptr;
}

int IndexOf(HWND list, const VariableItem* target) {
    const int count = ListView_GetItemCount(list);
    for (int i = 0; i < count; ++i) {
        if (ItemAt(list, i) == target) {
            return i;
        }
    }
    return -1;
}

PWSTR LocalString(const std::wstring& s) {
    const size_t bytes = (s.size() + 1) * sizeof(WCHAR);
    auto result = static_cast<PWSTR>(LocalAlloc(LMEM_FIXED, bytes));
    if (result) {
        memcpy(result, s.c_str(), bytes);
    }
    return result;
}

void FreeItem(VariableItem* item) {
    if (!item) {
        return;
    }
    LocalFree(item->name);
    LocalFree(item->value);
    LocalFree(item->expanded);
    LocalFree(item);
}

// sysdm displays user values with ExpandEnvironmentStrings, but leaves
// %SystemRoot% and %SystemDrive% unexpanded in system-variable values.
std::wstring ExpandForDisplay(const std::wstring& value, int scope) {
    if (scope != 1) {
        const DWORD size = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
        if (!size) {
            return value;
        }
        std::vector<WCHAR> buffer(size);
        const DWORD written = ExpandEnvironmentStringsW(value.c_str(), buffer.data(), size);
        return written && written <= size ? std::wstring(buffer.data()) : value;
    }

    std::wstring result;
    for (size_t pos = 0; pos < value.size();) {
        if (value[pos] != L'%') {
            result.push_back(value[pos++]);
            continue;
        }
        const size_t end = value.find(L'%', pos + 1);
        if (end == std::wstring::npos) {
            result.append(value, pos, std::wstring::npos);
            break;
        }
        std::wstring name = value.substr(pos + 1, end - pos - 1);
        if (_wcsicmp(name.c_str(), L"SystemRoot") != 0 &&
            _wcsicmp(name.c_str(), L"SystemDrive") != 0) {
            const DWORD size = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
            if (size) {
                std::vector<WCHAR> buffer(size);
                const DWORD written = GetEnvironmentVariableW(name.c_str(), buffer.data(), size);
                if (written && written < size) {
                    result.append(buffer.data(), written);
                    pos = end + 1;
                    continue;
                }
            }
        }
        result.append(value, pos, end - pos + 1);
        pos = end + 1;
    }
    return result;
}

VariableItem* MakeItem(const std::wstring& name, const std::wstring& value,
                       DWORD type, int scope) {
    std::wstring expanded = ExpandForDisplay(value, scope);
    auto item = static_cast<VariableItem*>(LocalAlloc(LPTR, sizeof(VariableItem)));
    if (!item) {
        return nullptr;
    }
    item->type = type;
    item->name = LocalString(name);
    item->value = LocalString(value);
    item->expanded = LocalString(expanded);
    if (!item->name || !item->value || !item->expanded) {
        FreeItem(item);
        return nullptr;
    }
    return item;
}

// Insert and populate the new row before removing any existing rows. Native
// sysdm owns the item and its LocalAlloc strings after a successful insertion.
enum class InsertResult { success, failed, inconsistent };

InsertResult InsertItem(HWND list, int index, VariableItem* value) {
    LVITEMW item{};
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = index;
    item.pszText = value->name;
    item.lParam = reinterpret_cast<LPARAM>(value);
    int inserted = ListView_InsertItem(list, &item);
    if (inserted < 0) {
        return InsertResult::failed;
    }
    LVITEMW text{};
    text.iSubItem = 1;
    text.pszText = value->expanded;
    if (!SendMessageW(list, LVM_SETITEMTEXTW, inserted,
                      reinterpret_cast<LPARAM>(&text))) {
        if (!ListView_DeleteItem(list, inserted)) {
            g_loadFailed = true;
            return InsertResult::inconsistent;
        }
        return InsertResult::failed;
    }
    return InsertResult::success;
}

bool RemoveItem(HWND list, VariableItem* item) {
    const int index = IndexOf(list, item);
    if (index < 0 || !ListView_DeleteItem(list, index)) {
        return false;
    }
    FreeItem(item);
    return true;
}

bool UseMarker(EditContext& context) {
    if (!context.oldItem || context.marker) {
        return true;
    }
    auto marker = static_cast<PWSTR>(LocalAlloc(LPTR, sizeof(WCHAR)));
    if (!marker) {
        return false;
    }
    context.originalValuePointer = context.oldItem->value;
    context.oldItem->value = marker;
    context.marker = marker;
    return true;
}

void RestoreMarker(EditContext& context) {
    if (context.marker) {
        context.oldItem->value = context.originalValuePointer;
        LocalFree(context.marker);
        context.marker = nullptr;
        context.originalValuePointer = nullptr;
    }
}

bool StageValue(EditContext& context, std::wstring value) {
    if (value.size() > kMaxValueChars || !UseMarker(context)) {
        return false;
    }
    context.pendingValue = std::move(value);
    context.pending = true;
    // The native parent compares this fixed buffer after the modal dialog.
    // The full value is kept separately until SetVar is called.
    const size_t copy = std::min(context.pendingValue.size(), kNativeBufferChars - 1);
    memcpy(g_valueBuffer, context.pendingValue.data(), copy * sizeof(WCHAR));
    g_valueBuffer[copy] = L'\0';
    return true;
}

bool ReadControl(HWND control, std::wstring& result) {
    const int length = GetWindowTextLengthW(control);
    if (length < 0 || static_cast<size_t>(length) > kMaxValueChars) {
        return false;
    }
    std::vector<WCHAR> buffer(static_cast<size_t>(length) + 1);
    const int copied = GetWindowTextW(control, buffer.data(), length + 1);
    if (copied != length) {
        return false;
    }
    result.assign(buffer.data(), copied);
    return true;
}

bool RebuildPath(HWND dialog, std::wstring& result) {
    HWND list = GetDlgItem(dialog, kPathListControl);
    const int count = ListView_GetItemCount(list);
    if (!list || count < 0) {
        return false;
    }
    std::vector<WCHAR> buffer(kMaxValueChars + 2);
    result.clear();
    for (int i = 0; i < count; ++i) {
        LVITEMW item{};
        item.iSubItem = 0;
        item.pszText = buffer.data();
        item.cchTextMax = static_cast<int>(buffer.size());
        const LRESULT length = SendMessageW(list, LVM_GETITEMTEXTW, i,
                                            reinterpret_cast<LPARAM>(&item));
        if (length < 0 || static_cast<size_t>(length) > kMaxValueChars) {
            return false;
        }
        const bool quote = std::char_traits<WCHAR>::find(buffer.data(),
                            static_cast<size_t>(length), L';') != nullptr;
        const size_t extra = static_cast<size_t>(length) + (quote ? 2 : 0) + 1;
        if (extra > kMaxValueChars - result.size()) {
            return false;
        }
        if (quote) result.push_back(L'"');
        result.append(buffer.data(), static_cast<size_t>(length));
        if (quote) result.push_back(L'"');
        result.push_back(L';');
    }
    return true;
}

bool PopulatePath(HWND dialog, const std::wstring& value) {
    HWND list = GetDlgItem(dialog, kPathListControl);
    if (!list || !g_nextPath || !ListView_DeleteAllItems(list)) {
        return false;
    }
    std::vector<WCHAR> part(value.size() + 1);
    PCWSTR current = value.c_str();
    int index = 0;
    while (PCWSTR next = g_nextPath(current, part.data(), static_cast<UINT>(part.size()))) {
        if (next <= current || next > value.c_str() + value.size()) {
            return false;
        }
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_STATE;
        item.iItem = index++;
        item.state = LVIS_SELECTED | LVIS_FOCUSED;
        item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        item.pszText = part.data();
        if (ListView_InsertItem(list, &item) < 0) {
            return false;
        }
        current = next;
    }
    return true;
}

void EditHook(HWND owner, unsigned scope, unsigned operation, int index) {
    if (g_edit) {
        g_editOriginal(owner, scope, operation, index);
        return;
    }
    EditContext context;
    context.owner = owner;
    context.scope = scope;
    try {
        if (operation == 1) {
            context.oldItem = ItemAt(VariableList(owner, scope), index);
            if (!context.oldItem || !context.oldItem->value || !context.oldItem->name) {
                Error(owner, L"The selected variable could not be read. No change was made.");
                return;
            }
            context.initialValue = context.oldItem->value;
            if (context.initialValue.size() >= kNativeBufferChars && !UseMarker(context)) {
                Error(owner, L"There is not enough memory to edit this variable.");
                return;
            }
        }
        g_edit = &context;
        g_editOriginal(owner, scope, operation, index);
        g_edit = nullptr;
        RestoreMarker(context);
    } catch (...) {
        g_edit = nullptr;
        RestoreMarker(context);
        Error(owner, L"The variable could not be edited. No change was made.");
    }
}

BOOL IsMultiHook(PWSTR value) {
    if (!g_edit || !g_edit->marker ||
        g_edit->initialValue.size() < kNativeBufferChars) {
        return g_isMultiOriginal(value);
    }
    try {
        // IsMultiComponentPath inspects the start of the first components. A
        // component may itself exceed the native buffer, so a simple prefix
        // could hide the semicolon that causes the list editor to be selected.
        std::wstring probe;
        size_t start = 0;
        for (int i = 0; i < 4 && start < g_edit->initialValue.size(); ++i) {
            const size_t end = g_edit->initialValue.find(L';', start);
            const size_t stop = end == std::wstring::npos ? g_edit->initialValue.size() : end;
            probe.append(g_edit->initialValue, start, std::min<size_t>(stop - start, 64));
            if (end == std::wstring::npos) break;
            probe.push_back(L';');
            start = end + 1;
        }
        memcpy(g_valueBuffer, probe.c_str(), (probe.size() + 1) * sizeof(WCHAR));
        const BOOL result = g_isMultiOriginal(value);
        g_valueBuffer[0] = L'\0';
        return result;
    } catch (...) {
        g_valueBuffer[0] = L'\0';
        return FALSE;
    }
}

INT_PTR CALLBACK TextDialogHook(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    if (!g_edit) return g_textDialogOriginal(dialog, message, wp, lp);
    try {
        if (message == WM_INITDIALOG) {
            INT_PTR result = g_textDialogOriginal(dialog, message, wp, lp);
            HWND control = GetDlgItem(dialog, kValueControl);
            if (!control) {
                EndDialog(dialog, 0);
                return TRUE;
            }
            SendMessageW(control, EM_SETLIMITTEXT, kMaxValueChars, 0);
            if (g_edit->initialValue.size() >= kNativeBufferChars &&
                !SetWindowTextW(control, g_edit->initialValue.c_str())) {
                Error(dialog, L"The complete value could not be displayed.");
                EndDialog(dialog, 0);
            }
            return result;
        }
        if (message == WM_COMMAND && LOWORD(wp) == IDOK && HIWORD(wp) == 0) {
            std::wstring value;
            if (!ReadControl(GetDlgItem(dialog, kValueControl), value)) {
                Error(dialog, L"The value must be at most 32,766 UTF-16 code units.");
                return TRUE;
            }
            if (value.size() > kNativeInputLimit || g_edit->marker) {
                if (!StageValue(*g_edit, std::move(value))) {
                    Error(dialog, L"The complete value could not be prepared.");
                    return TRUE;
                }
            }
        }
        return g_textDialogOriginal(dialog, message, wp, lp);
    } catch (...) {
        Error(dialog, L"The complete value could not be prepared.");
        if (message == WM_INITDIALOG) EndDialog(dialog, 0);
        return TRUE;
    }
}

INT_PTR CALLBACK PathDialogHook(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    if (!g_edit) return g_pathDialogOriginal(dialog, message, wp, lp);
    try {
        if (message == WM_INITDIALOG) {
            INT_PTR result = g_pathDialogOriginal(dialog, message, wp, lp);
            if (g_edit->initialValue.size() >= kNativeBufferChars &&
                !PopulatePath(dialog, g_edit->initialValue)) {
                Error(dialog, L"The complete path could not be displayed.");
                EndDialog(dialog, 0);
            }
            return result;
        }
        if (message == WM_COMMAND && LOWORD(wp) == IDOK && HIWORD(wp) == 0) {
            if (!RebuildPath(dialog, g_edit->preparedPath)) {
                Error(dialog, L"The combined value must be at most 32,766 UTF-16 code units.");
                return TRUE;
            }
            g_edit->pathPrepared = true;
            INT_PTR result = g_pathDialogOriginal(dialog, message, wp, lp);
            g_edit->pathPrepared = false;
            return result;
        }
        return g_pathDialogOriginal(dialog, message, wp, lp);
    } catch (...) {
        g_edit->pathPrepared = false;
        Error(dialog, L"The complete path could not be prepared.");
        if (message == WM_INITDIALOG) EndDialog(dialog, 0);
        return TRUE;
    }
}

bool RebuildHook(HWND dialog, bool compare) {
    if (!g_edit) return g_rebuildOriginal(dialog, compare);
    try {
        std::wstring value;
        if (!compare && g_edit->pathPrepared) {
            value = g_edit->preparedPath;
        } else if (!RebuildPath(dialog, value)) {
            return false;
        }
        if (compare) return value == g_edit->initialValue;
        if (value.size() <= kNativeInputLimit && !g_edit->marker) {
            return g_rebuildOriginal(dialog, false);
        }
        return StageValue(*g_edit, std::move(value));
    } catch (...) {
        return false;
    }
}

void DeleteHook(HWND owner, unsigned scope, PCWSTR name) {
    if (g_edit && g_edit->pending && g_edit->owner == owner && g_edit->scope == scope) {
        return;  // The original row is removed only after the replacement exists.
    }
    g_deleteOriginal(owner, scope, name);
}

void SetHook(HWND owner, unsigned scope, PCWSTR nameArg, PCWSTR valueArg) {
    if (!g_edit || !g_edit->pending || g_edit->owner != owner || g_edit->scope != scope) {
        g_setOriginal(owner, scope, nameArg, valueArg);
        return;
    }
    try {
        EditContext& context = *g_edit;
        std::wstring name(g_nameBuffer);
        while (!name.empty() && iswspace(name.back())) name.pop_back();
        const size_t equals = name.find(L'=');
        if (equals != std::wstring::npos) name.resize(equals);
        if (name.empty()) {
            Error(owner, L"The variable name is empty. No change was made.");
            return;
        }
        HWND list = VariableList(owner, scope);
        VariableItem* oldItem = context.oldItem;
        VariableItem* conflict = nullptr;
        const int count = ListView_GetItemCount(list);
        for (int i = 0; i < count; ++i) {
            VariableItem* item = ItemAt(list, i);
            if (item && item != oldItem && item->name &&
                _wcsicmp(item->name, name.c_str()) == 0) {
                conflict = item;
                break;
            }
        }
        const size_t first = context.pendingValue.find(L'%');
        const DWORD type = first != std::wstring::npos &&
                                   context.pendingValue.find(L'%', first + 1) != std::wstring::npos
                               ? REG_EXPAND_SZ : REG_SZ;
        VariableItem* replacement = MakeItem(name, context.pendingValue, type, scope);
        if (!replacement) {
            Error(owner, L"There is not enough memory to save the complete value.");
            return;
        }
        const InsertResult inserted = InsertItem(list, count, replacement);
        if (inserted != InsertResult::success) {
            if (inserted == InsertResult::failed) FreeItem(replacement);
            Error(owner, L"The replacement could not be added. No change was made.");
            return;
        }
        RestoreMarker(context);
        if ((oldItem && !RemoveItem(list, oldItem)) ||
            (conflict && !RemoveItem(list, conflict))) {
            // Do not commit a potentially inconsistent list.
            g_loadFailed = true;
            Error(owner, L"The variable list could not be updated. Cancel the dialog.");
            return;
        }
        const int index = IndexOf(list, replacement);
        if (index >= 0) {
            ListView_SetItemState(list, index, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
        }
        context.oldItem = nullptr;
    } catch (...) {
        Error(owner, L"The complete value could not be saved. No change was made.");
    }
}

HRESULT AddHook(HWND list, DWORD index, unsigned scope, PCWSTR name, PCWSTR value, DWORD type) {
    // _LoadEnvVars truncates its enumeration buffer to 4095 characters before
    // calling this function, even when RegEnumValueW returned the full value.
    // Recover the matching value captured during that enumeration.
    if (g_load && g_load->captured && g_load->list == list && name &&
        _wcsicmp(g_load->name.c_str(), name) == 0) {
        value = g_load->value.c_str();
    }
    if (!value || wcslen(value) < kNativeBufferChars) {
        HRESULT result = g_addOriginal(list, index, scope, name, value, type);
        if (FAILED(result)) g_loadFailed = true;
        return result;
    }
    try {
        VariableItem* item = MakeItem(name ? name : L"", value, type, scope);
        if (item) {
            const InsertResult result = InsertItem(list, index - 1, item);
            if (result == InsertResult::success) return S_OK;
            if (result == InsertResult::failed) FreeItem(item);
        }
    } catch (...) {
    }
    g_loadFailed = true;
    return E_OUTOFMEMORY;
}

LSTATUS WINAPI EnumValueHook(HKEY key, DWORD index, LPWSTR name, LPDWORD nameSize,
                             LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD dataSize) {
    const LSTATUS result = g_enumValueOriginal(key, index, name, nameSize, reserved, type, data, dataSize);
    if (!g_load || key != g_load->key) return result;
    g_load->captured = false;
    if (result != ERROR_SUCCESS) {
        if (result != ERROR_NO_MORE_ITEMS) g_loadFailed = true;
        return result;
    }
    if (!name || !nameSize || !type || !data || !dataSize ||
        (*type != REG_SZ && *type != REG_EXPAND_SZ)) return result;
    try {
        if (*dataSize % sizeof(WCHAR)) {
            g_loadFailed = true;
            return result;
        }
        const size_t count = *dataSize / sizeof(WCHAR);
        const auto* text = reinterpret_cast<PCWSTR>(data);
        const WCHAR* end = std::char_traits<WCHAR>::find(text, count, L'\0');
        if (!end) {
            // The native code cannot round-trip a malformed nonterminated
            // string safely, so prevent the dialog from rewriting it.
            g_loadFailed = true;
            return result;
        }
        if (static_cast<size_t>(end - text) >= kNativeBufferChars) {
            g_load->name.assign(name, *nameSize);
            g_load->value.assign(text, end);
            g_load->captured = true;
        }
    } catch (...) {
        g_loadFailed = true;
    }
    return result;
}

DWORD LoadHook(HKEY key, HWND list, unsigned scope) {
    if (g_load) return g_loadOriginal(key, list, scope);
    if (scope != 1 && scope != 2) {
        g_loadFailed = true;
        return 0;
    }
    try {
        RegistrySnapshot before, after;
        if (!CaptureSnapshot(key, before)) {
            g_loadFailed = true;
            return 0;
        }
        DWORD expected = 0;
        for (const auto& entry : before.entries) {
            if (entry.type == REG_SZ || entry.type == REG_EXPAND_SZ) {
                ++expected;
                if (entry.data.size() < sizeof(WCHAR)) g_loadFailed = true;
            }
        }
        DWORD result = 0;
        if (before.maxDataBytes != 0) {
            LoadContext context{key, list};
            g_load = &context;
            result = g_loadOriginal(key, list, scope);
            g_load = nullptr;
        }
        // The native loader writes a WCHAR even if its maximum-data size is
        // zero. Skip it for an empty key instead of risking a zero-size buffer.
        if (result != expected || !CaptureSnapshot(key, after) || !SameSnapshot(before, after)) {
            g_loadFailed = true;
        } else {
            g_snapshots[scope - 1] = std::move(after);
        }
        return result;
    } catch (...) {
        g_load = nullptr;
        g_loadFailed = true;
        return 0;
    }
}

struct RegistryRow {
    std::wstring name;
    std::wstring value;
    DWORD type;
};

bool ReadRows(HWND list, std::vector<RegistryRow>& rows) {
    if (!list) return false;
    const int count = ListView_GetItemCount(list);
    if (count < 0) return false;
    rows.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        VariableItem* item = ItemAt(list, i);
        if (!item || !item->name || !item->value || !item->name[0] ||
            (item->type != REG_SZ && item->type != REG_EXPAND_SZ)) return false;
        RegistryRow row{item->name, item->value, item->type};
        if (row.value.size() >= MAXDWORD / sizeof(WCHAR)) return false;
        for (const auto& previous : rows) {
            if (_wcsicmp(previous.name.c_str(), row.name.c_str()) == 0) return false;
        }
        rows.push_back(std::move(row));
    }
    return true;
}

struct KeyHandle {
    HKEY value = nullptr;
    ~KeyHandle() { if (value) RegCloseKey(value); }
};

struct TransactionHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    bool committed = false;
    ~TransactionHandle() {
        if (value != INVALID_HANDLE_VALUE) {
            if (!committed) RollbackTransaction(value);
            CloseHandle(value);
        }
    }
};

bool ApplyRows(HKEY key, const RegistrySnapshot& snapshot,
               const std::vector<RegistryRow>& rows) {
    // Match the native save: replace string values, retaining other types.
    for (const auto& entry : snapshot.entries) {
        if ((entry.type == REG_SZ || entry.type == REG_EXPAND_SZ) &&
            RegDeleteValueW(key, entry.name.c_str()) != ERROR_SUCCESS) return false;
    }
    for (const auto& row : rows) {
        const DWORD bytes = static_cast<DWORD>((row.value.size() + 1) * sizeof(WCHAR));
        if (RegSetValueExW(key, row.name.c_str(), 0, row.type,
                           reinterpret_cast<const BYTE*>(row.value.c_str()), bytes) != ERROR_SUCCESS) return false;
    }
    return true;
}

void FlushEnvironmentKey(HKEY root, PCWSTR path) {
    KeyHandle key;
    if (RegOpenKeyExW(root, path, 0, KEY_QUERY_VALUE, &key.value) == ERROR_SUCCESS) {
        RegFlushKey(key.value);
    }
}

void BroadcastEnvironmentChange() {
    SendNotifyMessageW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                       reinterpret_cast<LPARAM>(L"Environment"));
    // The verified sysdm build also notifies other sessions when elevated.
    using IsAdminFn = BOOL(WINAPI*)();
    using BroadcastFn = LONG(WINAPI*)(HANDLE, DWORD, DWORD, DWORD, DWORD, DWORD*,
                                      DWORD, ULONG_PTR, PCWSTR, DWORD*);
    HMODULE shell = GetModuleHandleW(L"shell32.dll");
    HMODULE winsta = GetModuleHandleW(L"winsta.dll");
    auto isAdmin = shell ? reinterpret_cast<IsAdminFn>(GetProcAddress(shell, MAKEINTRESOURCEA(680))) : nullptr;
    auto broadcast = winsta ? reinterpret_cast<BroadcastFn>(
                                  GetProcAddress(winsta, "WinStationBroadcastSystemMessage")) : nullptr;
    if (isAdmin && broadcast && isAdmin()) {
        DWORD recipients = 0, result = 0;
        broadcast(nullptr, 1, 0, 1, 8, &recipients, WM_SETTINGCHANGE, 0,
                  L"Environment", &result);
    }
}

enum class SaveResult { success, changed, failed };

SaveResult SaveAtomically(HWND dialog) {
    constexpr PCWSTR userPath = L"Environment";
    constexpr PCWSTR systemPath = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";
    if (!g_editSystemVars || !g_snapshots[1].valid) return SaveResult::failed;
    const bool saveSystem = *g_editSystemVars != FALSE;
    if (saveSystem && !g_snapshots[0].valid) return SaveResult::failed;

    std::vector<RegistryRow> userRows, systemRows;
    if (!ReadRows(VariableList(dialog, 2), userRows) ||
        (saveSystem && !ReadRows(VariableList(dialog, 1), systemRows))) return SaveResult::failed;

    TransactionHandle transaction;
    transaction.value = CreateTransaction(nullptr, nullptr, 0, 0, 0, 0, nullptr);
    if (transaction.value == INVALID_HANDLE_VALUE) return SaveResult::failed;
    KeyHandle userKey, systemKey;
    if (RegOpenKeyTransactedW(HKEY_CURRENT_USER, userPath, 0, KEY_QUERY_VALUE | KEY_SET_VALUE,
                              &userKey.value, transaction.value, nullptr) != ERROR_SUCCESS ||
        (saveSystem && RegOpenKeyTransactedW(HKEY_LOCAL_MACHINE, systemPath, 0,
                                              KEY_QUERY_VALUE | KEY_SET_VALUE, &systemKey.value,
                                              transaction.value, nullptr) != ERROR_SUCCESS)) return SaveResult::failed;

    RegistrySnapshot currentUser, currentSystem;
    if (!CaptureSnapshot(userKey.value, currentUser) ||
        (saveSystem && !CaptureSnapshot(systemKey.value, currentSystem))) return SaveResult::failed;
    if (!SameSnapshot(g_snapshots[1], currentUser) ||
        (saveSystem && !SameSnapshot(g_snapshots[0], currentSystem))) return SaveResult::changed;

    if (!ApplyRows(userKey.value, currentUser, userRows) ||
        (saveSystem && !ApplyRows(systemKey.value, currentSystem, systemRows)) ||
        !CommitTransaction(transaction.value)) return SaveResult::failed;
    transaction.committed = true;

    FlushEnvironmentKey(HKEY_CURRENT_USER, userPath);
    if (saveSystem) FlushEnvironmentKey(HKEY_LOCAL_MACHINE, systemPath);
    BroadcastEnvironmentChange();
    return SaveResult::success;
}

void CommandHook(HWND dialog, HWND source, int command, int notification) {
    if (command == IDOK) {
        if (g_loadFailed) {
            Error(dialog, L"The variable list could not be loaded or updated completely. "
                          L"Cancel this dialog to leave the stored variables unchanged.");
            return;
        }
        try {
            const SaveResult result = SaveAtomically(dialog);
            if (result == SaveResult::success) {
                EndDialog(dialog, 0);
            } else if (result == SaveResult::changed) {
                Error(dialog, L"Environment variables changed while this dialog was open. "
                              L"Cancel and reopen it before trying again.");
            } else {
                Error(dialog, L"The environment variables could not be saved atomically. "
                              L"No changes were committed.");
            }
        } catch (...) {
            Error(dialog, L"The environment variables could not be saved atomically. "
                          L"No changes were committed.");
        }
        return;
    }
    g_commandOriginal(dialog, source, command, notification);
}

INT_PTR CALLBACK MainDialogHook(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_INITDIALOG) {
        g_loadFailed = false;
        g_snapshots[0] = RegistrySnapshot{};
        g_snapshots[1] = RegistrySnapshot{};
    }
    return g_mainDialogOriginal(dialog, message, wp, lp);
}

bool HasSupportedPdb(HMODULE module) {
    const auto* base = reinterpret_cast<const BYTE*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    const DWORD imageSize = nt->OptionalHeader.SizeOfImage;
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (directory.VirtualAddress > imageSize || directory.Size > imageSize - directory.VirtualAddress) return false;
    const auto* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + directory.VirtualAddress);
    for (DWORD i = 0; i < directory.Size / sizeof(*entries); ++i) {
        const auto& entry = entries[i];
        if (entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW || entry.SizeOfData < 24 ||
            entry.AddressOfRawData > imageSize || entry.SizeOfData > imageSize - entry.AddressOfRawData) continue;
        const BYTE* record = base + entry.AddressOfRawData;
        DWORD signature = 0, age = 0;
        GUID guid{};
        memcpy(&signature, record, sizeof(signature));
        memcpy(&guid, record + 4, sizeof(guid));
        memcpy(&age, record + 20, sizeof(age));
        if (signature == 0x53445352 && IsEqualGUID(guid, kSupportedPdb) && age == 1) return true;
    }
    return false;
}

bool HasSupportedHash(PCWSTR path) {
    struct File {
        HANDLE value = INVALID_HANDLE_VALUE;
        ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    } file;
    struct Algorithm {
        BCRYPT_ALG_HANDLE value = nullptr;
        ~Algorithm() { if (value) BCryptCloseAlgorithmProvider(value, 0); }
    } algorithm;
    struct Hash {
        BCRYPT_HASH_HANDLE value = nullptr;
        ~Hash() { if (value) BCryptDestroyHash(value); }
    };
    std::vector<BYTE> object;
    Hash hash;

    file.value = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file.value == INVALID_HANDLE_VALUE ||
        BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    DWORD objectSize = 0, returned = 0;
    if (BCryptGetProperty(algorithm.value, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &returned, 0) < 0 ||
        returned != sizeof(objectSize) || objectSize == 0 || objectSize > 65536) return false;
    object.resize(objectSize);
    if (BCryptCreateHash(algorithm.value, &hash.value, object.data(), objectSize,
                         nullptr, 0, 0) < 0) return false;
    BYTE buffer[16384];
    DWORD read = 0;
    do {
        if (!ReadFile(file.value, buffer, sizeof(buffer), &read, nullptr) ||
            (read && BCryptHashData(hash.value, buffer, read, 0) < 0)) return false;
    } while (read);
    BYTE digest[sizeof(kSupportedSha256)];
    return BCryptFinishHash(hash.value, digest, sizeof(digest), 0) >= 0 &&
           memcmp(digest, kSupportedSha256, sizeof(digest)) == 0;
}

bool IsSupportedBuild(HMODULE module) {
    if (!HasSupportedPdb(module)) {
        Wh_Log(L"sysdm.cpl PDB identity mismatch or debug directory unavailable");
        return false;
    }
    WCHAR path[MAX_PATH];
    const DWORD pathLength = GetModuleFileNameW(module, path, ARRAYSIZE(path));
    if (!pathLength || pathLength >= ARRAYSIZE(path)) {
        Wh_Log(L"Could not get the sysdm.cpl path (error %lu)", GetLastError());
        return false;
    }
    if (!HasSupportedHash(path)) {
        Wh_Log(L"sysdm.cpl SHA-256 mismatch or file/hash read failed: %s", path);
        return false;
    }
    Wh_Log(L"Verified sysdm.cpl PDB identity and SHA-256: %s", path);
    return true;
}

bool IsTargetProcess() {
    WCHAR path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (!length || length >= ARRAYSIZE(path)) return false;
    PCWSTR exe = wcsrchr(path, L'\\');
    exe = exe ? exe + 1 : path;
    if (_wcsicmp(exe, L"SystemPropertiesAdvanced.exe") == 0) return true;
    if (_wcsicmp(exe, L"rundll32.exe") != 0 && _wcsicmp(exe, L"control.exe") != 0) return false;
    PCWSTR cmd = GetCommandLineW();
    constexpr PCWSTR needle = L"sysdm.cpl";
    for (; *cmd; ++cmd) {
        if (_wcsnicmp(cmd, needle, wcslen(needle)) == 0) return true;
    }
    return false;
}

}  // namespace

BOOL Wh_ModInit() {
    try {
        if (!IsTargetProcess()) return FALSE;
        HMODULE module = LoadLibraryExW(L"sysdm.cpl", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) {
            Wh_Log(L"Could not load System32 sysdm.cpl (error %lu); no hooks installed", GetLastError());
            return FALSE;
        }
        if (!IsSupportedBuild(module)) {
            Wh_Log(L"Unsupported sysdm.cpl identity; no hooks installed");
            return FALSE;
        }
        HMODULE shlwapi = GetModuleHandleW(L"shlwapi.dll");
        g_nextPath = shlwapi ? reinterpret_cast<NextPathFn>(
                                  GetProcAddress(shlwapi, MAKEINTRESOURCEA(450))) : nullptr;
        if (!g_nextPath) {
            Wh_Log(L"shlwapi NextPathW is unavailable; no hooks installed");
            return FALSE;
        }
        const WindhawkUtils::SYMBOL_HOOK hooks[] = {
            {{L"?EVDoEdit@@YAXPEAUHWND__@@IIH@Z"}, &g_editOriginal, EditHook},
            {{L"?DeleteVar@@YAXPEAUHWND__@@IPEBG@Z"}, &g_deleteOriginal, DeleteHook},
            {{L"?SetVar@@YAXPEAUHWND__@@IPEBG1@Z"}, &g_setOriginal, SetHook},
            {{L"?_AddVarToListbox@@YAJPEAUHWND__@@KIPEBG1K@Z"}, &g_addOriginal, AddHook},
            {{L"?_LoadEnvVars@@YAKPEAUHKEY__@@PEAUHWND__@@I@Z"}, &g_loadOriginal, LoadHook},
            {{L"?EVDoCommand@@YAXPEAUHWND__@@0HH@Z"}, &g_commandOriginal, CommandHook},
            {{L"EnvVarsDlgProc"}, &g_mainDialogOriginal, MainDialogHook},
            {{L"EnvVarsEditDlg"}, &g_textDialogOriginal, TextDialogHook},
            {{L"PathEnvVarsEditDlg"}, &g_pathDialogOriginal, PathDialogHook},
            {{L"?PathEnvVarsRebuildVariableStringImpl@@YA_NPEAUHWND__@@_N@Z"}, &g_rebuildOriginal, RebuildHook},
            {{L"?IsMultiComponentPath@@YAHPEAG@Z"}, &g_isMultiOriginal, IsMultiHook},
            {{L"g_szVarValue"}, &g_valueBuffer},
            {{L"g_szVarName"}, &g_nameBuffer},
            {{L"?g_fEditSystemVars@@3HA"}, &g_editSystemVars},
        };
        WH_HOOK_SYMBOLS_OPTIONS options{};
        options.optionsSize = sizeof(options);
        options.noUndecoratedSymbols = TRUE;
        Wh_Log(L"Resolving required sysdm.cpl symbols");
        if (!WindhawkUtils::HookSymbols(module, hooks, ARRAYSIZE(hooks), &options)) {
            Wh_Log(L"Required sysdm.cpl symbols not found; no hooks installed");
            return FALSE;
        }
        HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
        void* enumValue = kernelbase ? reinterpret_cast<void*>(
                                          GetProcAddress(kernelbase, "RegEnumValueW")) : nullptr;
        if (!enumValue || !Wh_SetFunctionHook(enumValue,
                                reinterpret_cast<void*>(EnumValueHook),
                                reinterpret_cast<void**>(&g_enumValueOriginal))) {
            Wh_Log(L"Could not hook RegEnumValueW; no hooks installed");
            return FALSE;
        }
        Wh_Log(L"Required symbols resolved and hooks registered");
        return TRUE;
    } catch (...) {
        Wh_Log(L"Initialization failed; no hooks installed");
        return FALSE;
    }
}

void Wh_ModAfterInit() {
    Wh_Log(L"Environment Variables Unlimited hooks activated in process %lu", GetCurrentProcessId());
}
