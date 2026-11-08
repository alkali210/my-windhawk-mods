// Isolated-process integration harness. MinHook is TEST ONLY; the mod uses Windhawk.
#include <windows.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <commctrl.h>
#include <MinHook.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <initializer_list>

void Wh_Log(PCWSTR format, ...) {
    va_list args;
    va_start(args, format);
    vwprintf(format, args);
    wprintf(L"\n");
    va_end(args);
}
int Wh_GetIntSetting(PCWSTR) { return 1; }
PCWSTR Wh_GetStringSetting(PCWSTR) { return L"acrylic"; }
void Wh_FreeStringSetting(PCWSTR) {}
BOOL Wh_SetFunctionHook(void* api, void* hook, void** original) {
    MH_STATUS status = MH_CreateHook(api, hook, original);
    if (status != MH_OK) printf("MH_CreateHook: %s\n", MH_StatusToString(status));
    return status == MH_OK;
}
int activeOwnerSubclasses = 0;
bool scriptedCtrl = false;
SHORT ctrlStates[4]{};
unsigned ctrlStateCount = 0, ctrlStateIndex = 0;
SHORT WINAPI TestGetAsyncKeyState(int key) {
    if (scriptedCtrl && key == VK_CONTROL) {
        unsigned index = ctrlStateIndex++;
        return ctrlStates[index < ctrlStateCount ? index : ctrlStateCount - 1];
    }
    return GetAsyncKeyState(key);
}
int bufferedPaintDepth = 0;
unsigned bufferedInitCalls = 0;
bool failNextBufferedInit = false;
bool failNextSubclass = false;
bool seedBorderColor = false;
bool auditBorderRestore = false;
constexpr COLORREF kSeedBorderColor = RGB(33, 66, 99);
HRESULT WINAPI TestBufferedPaintInit() {
    ++bufferedInitCalls;
    if (failNextBufferedInit) {
        failNextBufferedInit = false;
        return E_OUTOFMEMORY;
    }
    HRESULT result = BufferedPaintInit();
    if (SUCCEEDED(result)) ++bufferedPaintDepth;
    return result;
}
HRESULT WINAPI TestBufferedPaintUnInit() {
    HRESULT result = BufferedPaintUnInit();
    if (SUCCEEDED(result)) --bufferedPaintDepth;
    return result;
}
BOOL TestSetSubclass(HWND hwnd, SUBCLASSPROC proc, UINT_PTR id, DWORD_PTR data) {
    if (failNextSubclass) {
        failNextSubclass = false;
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }
    if (seedBorderColor && GetClassLongPtrW(hwnd, GCW_ATOM) == 32768)
        DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &kSeedBorderColor, sizeof(kSeedBorderColor));
    BOOL result = SetWindowSubclass(hwnd, proc, id, data);
    if (result) ++activeOwnerSubclasses;
    return result;
}
BOOL TestRemoveSubclass(HWND hwnd, SUBCLASSPROC proc, UINT_PTR id) {
    BOOL result = RemoveWindowSubclass(hwnd, proc, id);
    if (result) --activeOwnerSubclasses;
    return result;
}
#define SetWindowSubclass TestSetSubclass
#define RemoveWindowSubclass TestRemoveSubclass
struct BackdropTrace {
    HWND hwnd;
    unsigned writes;
    unsigned frames;
    bool firstVisible;
    bool drawn;
    bool changedAfterDrawing;
    bool redrawRequested;
    bool shown;
    bool movedAfterShown;
    RECT shownRect;
    bool haveShownFrame;
    bool frameChangedAfterShow;
    RECT shownFrame;
    unsigned ncPolicyWrites;
    bool haveOriginalNc;
    BOOL originalNc;
    unsigned borderWrites;
    unsigned borderPaints;
    unsigned lateBorderWrites;
    COLORREF lastBorderColor;
    HRESULT lastBorderResult;
};
BackdropTrace backdropTraces[64]{};
unsigned backdropTraceCount = 0;
unsigned dwmWrites = 0;
BackdropTrace& Trace(HWND hwnd) {
    for (unsigned i = 0; i < backdropTraceCount; ++i)
        if (backdropTraces[i].hwnd == hwnd) return backdropTraces[i];
    auto& trace = backdropTraces[backdropTraceCount++];
    trace = {};
    trace.hwnd = hwnd;
    return trace;
}
decltype(&DwmSetWindowAttribute) realDwmSetWindowAttribute = DwmSetWindowAttribute;
HRESULT WINAPI TestDwmSetAttribute(HWND hwnd, DWORD attribute, LPCVOID value, DWORD size) {
    ++dwmWrites;
    if (attribute == DWMWA_BORDER_COLOR) ++Trace(hwnd).borderWrites;
    if (attribute == DWMWA_USE_IMMERSIVE_DARK_MODE) {
        auto& trace = Trace(hwnd);
        trace.haveOriginalNc = SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_NCRENDERING_ENABLED,
            &trace.originalNc, sizeof(trace.originalNc)));
    }
    if (attribute == DWMWA_NCRENDERING_POLICY) ++Trace(hwnd).ncPolicyWrites;
    HRESULT result = realDwmSetWindowAttribute(hwnd, attribute, value, size);
    if (attribute == DWMWA_BORDER_COLOR) {
        Trace(hwnd).lastBorderColor = *static_cast<const COLORREF*>(value);
        Trace(hwnd).lastBorderResult = result;
    }
    if (attribute == DWMWA_SYSTEMBACKDROP_TYPE &&
        *static_cast<const DWORD*>(value) >= DWMSBT_MAINWINDOW) {
        auto& trace = Trace(hwnd);
        if (!trace.writes) trace.firstVisible = IsWindowVisible(hwnd) != FALSE;
        ++trace.writes;
        trace.changedAfterDrawing = trace.changedAfterDrawing || trace.drawn;
    }
    return result;
}
HDC WINAPI TestGetDCEx(HWND hwnd, HRGN region, DWORD flags) {
    HDC dc = GetDCEx(hwnd, region, flags);
    if (dc) ++Trace(hwnd).borderPaints;
    return dc;
}
HRESULT WINAPI TestDwmGetAttribute(HWND hwnd, DWORD attribute, PVOID value, DWORD size) {
    // BORDER_COLOR is setter-only on this host. Supply a known readable value
    // in one controlled case; all other tests use the real getter/fallback.
    if (seedBorderColor && attribute == DWMWA_BORDER_COLOR && size == sizeof(COLORREF)) {
        *static_cast<COLORREF*>(value) = kSeedBorderColor;
        return S_OK;
    }
    return DwmGetWindowAttribute(hwnd, attribute, value, size);
}
BOOL WINAPI TestSetWindowPos(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags) {
    if (flags & SWP_FRAMECHANGED) {
        auto& trace = Trace(hwnd);
        ++trace.frames;
        trace.redrawRequested = trace.redrawRequested || !(flags & SWP_NOREDRAW);
    }
    return SetWindowPos(hwnd, after, x, y, cx, cy, flags);
}
#define DwmSetWindowAttribute TestDwmSetAttribute
#define SetWindowPos TestSetWindowPos
#define GetAsyncKeyState TestGetAsyncKeyState
#define BufferedPaintInit TestBufferedPaintInit
#define BufferedPaintUnInit TestBufferedPaintUnInit
#define GetDCEx TestGetDCEx
#define DwmGetWindowAttribute TestDwmGetAttribute
#define ETM_TEST
#include "../explorer-translucent-menus.wh.cpp"
#undef SetWindowSubclass
#undef RemoveWindowSubclass
#undef DwmSetWindowAttribute
#undef SetWindowPos
#undef GetAsyncKeyState
#undef BufferedPaintInit
#undef BufferedPaintUnInit
#undef GetDCEx
#undef DwmGetWindowAttribute

int failures = 0;
bool emulateUnprotectedBorder = false;
HRESULT WINAPI ObserveDwmSetAttribute(HWND hwnd, DWORD attribute, LPCVOID value, DWORD size) {
    return emulateUnprotectedBorder ? TestDwmSetAttribute(hwnd, attribute, value, size)
        : etm::DwmSetWindowAttributeHook(hwnd, attribute, value, size);
}
int backgrounds = 0;
int texts = 0;
int accepted = 0;
int step = 0;
bool expectActive = true;
bool selectCommand = false;
bool sawSubmenu = false;
bool alphaChecked = false;
bool cancelForUnload = false;
bool placeNearEdge = false;
ULONGLONG childSeenAt = 0;
ULONGLONG menuLoopStarted = 0;
bool processingFixtureTimer = false;
bool shellFixture = false;
unsigned shellDraws = 0;
unsigned shellMisses = 0;
unsigned shellHookedTexts = 0;
bool shellChildDrawn = false;
HMENU shellChild = nullptr;
bool previewMode = false;
HMENU previewMenu = nullptr;
HWND previewMaterials = nullptr, previewThemes = nullptr;
HWND previewBorder = nullptr, previewCorners = nullptr;
HBITMAP previewIcon = nullptr;
etm::CompositionAttribute_t realComposition = nullptr;
bool rejectNextAccent = false;
unsigned rejectedAccents = 0;
unsigned compositionWrites = 0;
BOOL WINAPI TestComposition(HWND hwnd, etm::CompositionData* data) {
    ++compositionWrites;
    if (rejectNextAccent && data->attribute == 19 &&
        static_cast<etm::AccentPolicy*>(data->data)->state == 4) {
        rejectNextAccent = false;
        ++rejectedAccents;
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }
    return realComposition(hwnd, data);
}

void CheckAlpha(HWND owner);

const void* borderProbeValue = nullptr;
DWORD borderProbeSize = 0;
COLORREF borderProbeColor = 0;
HRESULT WINAPI BorderProbe(HWND, DWORD, LPCVOID value, DWORD size) {
    borderProbeValue = value;
    borderProbeSize = size;
    borderProbeColor = value && size == sizeof(COLORREF) ? *static_cast<const COLORREF*>(value) : 0;
    SetLastError(0x2468);
    return E_ACCESSDENIED;
}

void Check(bool condition, const char* name) {
    printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

void CheckBorderHookScope(etm::Popup& popup, HWND owner) {
    auto original = etm::g_dwmSetWindowAttribute;
    etm::g_dwmSetWindowAttribute = BorderProbe;
    COLORREF requested = RGB(87, 65, 43);
    HRESULT result = DwmSetWindowAttribute(popup.hwnd, DWMWA_BORDER_COLOR, &requested, sizeof(requested));
    Check(result == E_ACCESSDENIED && GetLastError() == 0x2468 &&
          borderProbeColor == DWMWA_COLOR_NONE && requested == RGB(87, 65, 43),
          "border hook preserves result/error and never mutates caller storage");
    for (int scenario = 0; scenario < 7; ++scenario) {
        bool allowed = etm::t_scope->allowed;
        if (scenario == 0) etm::g_settings.hideMenuBorder = false;
        if (scenario == 1) popup.active = false;
        if (scenario == 2) etm::t_scope->allowed = false;
        if (scenario == 3) etm::g_stopping.store(true);
        DWORD size = scenario == 6 ? 0 : sizeof(requested);
        result = DwmSetWindowAttribute(scenario == 4 ? owner : popup.hwnd,
            scenario == 5 ? DWMWA_CAPTION_COLOR : DWMWA_BORDER_COLOR, &requested, size);
        DWORD error = GetLastError();
        Check(result == E_ACCESSDENIED && error == 0x2468 &&
              borderProbeValue == &requested && borderProbeSize == size,
              "border hook forwards disabled/inactive/bypassed/stopping/unrelated/invalid calls unchanged");
        etm::g_settings.hideMenuBorder = true;
        popup.active = true;
        etm::t_scope->allowed = allowed;
        etm::g_stopping.store(false);
    }
    etm::g_dwmSetWindowAttribute = original;
}

void SampleFrame(BackdropTrace& trace) {
    if (!trace.shown) return;
    RECT frame{};
    if (FAILED(DwmGetWindowAttribute(trace.hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) return;
    if (!trace.haveShownFrame) {
        trace.haveShownFrame = true;
        trace.shownFrame = frame;
    } else if (!EqualRect(&frame, &trace.shownFrame)) {
        if (!trace.frameChangedAfterShow)
            printf("Late DWM frame: dpi=%u initial=(%ld,%ld,%ld,%ld) current=(%ld,%ld,%ld,%ld)\n",
                GetDpiForWindow(trace.hwnd), trace.shownFrame.left, trace.shownFrame.top,
                trace.shownFrame.right, trace.shownFrame.bottom, frame.left, frame.top, frame.right, frame.bottom);
        trace.frameChangedAfterShow = true;
    }
}

LRESULT CALLBACK PositionObserver(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && etm::t_scope && etm::t_scope->allowed &&
        etm::g_settings.material != etm::Material::Native) {
        auto& message = *reinterpret_cast<const CWPSTRUCT*>(lp);
        if (etm::IsClassicPopup(message.hwnd) && message.message == WM_WINDOWPOSCHANGED && message.lParam) {
            auto& position = *reinterpret_cast<const WINDOWPOS*>(message.lParam);
            if (!(position.flags & SWP_HIDEWINDOW)) {
                RECT rect{};
                if (GetWindowRect(message.hwnd, &rect)) {
                    auto& trace = Trace(message.hwnd);
                    if (trace.shown && !EqualRect(&rect, &trace.shownRect))
                        trace.movedAfterShown = true;
                    if (!trace.shown && (position.flags & SWP_SHOWWINDOW)) {
                        trace.shown = true;
                        trace.shownRect = rect;
                    }
                    SampleFrame(trace);
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

UINT nativeProbeFlags = 0;
bool nativeProbeClean = false;
int nativeProbeBufferedDepth = 0;
DWORD nativeProbeIncomingError = 0;
BOOL WINAPI NativeProbe(HMENU, UINT flags, int, int, HWND, LPTPMPARAMS) {
    nativeProbeIncomingError = GetLastError();
    nativeProbeFlags = flags;
    nativeProbeBufferedDepth = bufferedPaintDepth;
    nativeProbeClean = etm::t_scope && !etm::t_scope->allowed &&
        !etm::t_scope->messageHook && !etm::t_scope->ownerSubclass &&
        !etm::t_scope->count && etm::t_bypassDepth;
    SetLastError(0x1234);
    return 42;
}

BOOL WINAPI NativeProbeLegacy(HMENU menu, UINT flags, int x, int y, int, HWND owner, const RECT*) {
    return NativeProbe(menu, flags, x, y, owner, nullptr);
}

void CheckNativeBypass() {
    auto original = etm::g_trackPopupMenuEx;
    auto material = etm::g_settings.material;
    etm::g_trackPopupMenuEx = NativeProbe;
    etm::g_settings.material = etm::Material::Native;
    UINT flags = TPM_RETURNCMD | TPM_RIGHTALIGN;
    BOOL result = etm::TrackPopupMenuExHook(nullptr, flags, 1, 2, nullptr, nullptr);
    DWORD error = GetLastError();
    Check(result == 42 && error == 0x1234, "native bypass preserves return value and last error");
    Check(nativeProbeFlags == flags && nativeProbeClean,
          "native bypass keeps animation flags and creates no painting hooks");
    etm::g_settings.material = etm::Material::Acrylic;
    ++etm::t_bypassDepth; // An outer Ctrl-bypass must survive key release in nested calls.
    result = etm::TrackPopupMenuExHook(nullptr, flags, 1, 2, nullptr, nullptr);
    --etm::t_bypassDepth;
    Check(result == 42 && nativeProbeFlags == flags && nativeProbeClean,
          "nested bypass cannot re-enable rendering after Ctrl is released");
    Check(etm::g_scopes == nullptr && etm::t_scope == nullptr && etm::g_calls.load() == 0,
          "native bypass leaves no scopes or callbacks behind");
    etm::g_settings.material = material;
    etm::g_trackPopupMenuEx = original;
}

void CheckLateCtrlBypass(HWND owner) {
    auto original = etm::g_trackPopupMenuEx;
    auto originalLegacy = etm::g_trackPopupMenu;
    auto material = etm::g_settings.material;
    bool bypass = etm::g_settings.ctrlBypass;
    bool hideBorder = etm::g_settings.hideMenuBorder;
    etm::g_trackPopupMenuEx = NativeProbe;
    etm::g_trackPopupMenu = NativeProbeLegacy;
    etm::g_settings.material = etm::Material::Acrylic;
    etm::g_settings.ctrlBypass = true;
    etm::g_settings.hideMenuBorder = true;
    const char* cases[] = {"Ctrl held at entry", "Ctrl pressed during setup", "buffered init failure", "subclass failure"};
    for (bool legacy : {false, true}) for (unsigned scenario = 0; scenario < 4; ++scenario) {
        printf("Native fallback audit: %s / %s\n", legacy ? "TrackPopupMenu" : "TrackPopupMenuEx", cases[scenario]);
        scriptedCtrl = true;
        ctrlStates[0] = scenario == 0 ? static_cast<SHORT>(0x8000) : 0;
        ctrlStates[1] = scenario == 1 ? static_cast<SHORT>(0x8000) : 0;
        ctrlStateCount = 2;
        ctrlStateIndex = 0;
        failNextBufferedInit = scenario == 2;
        failNextSubclass = scenario == 3;
        int originalDepth = bufferedPaintDepth;
        unsigned originalDwm = dwmWrites, originalComposition = compositionWrites, originalInits = bufferedInitCalls;
        UINT flags = TPM_RETURNCMD | TPM_RIGHTALIGN;
        SetLastError(0x7788);
        BOOL result = legacy ? etm::TrackPopupMenuHook(nullptr, flags, 1, 2, 0, owner, nullptr)
            : etm::TrackPopupMenuExHook(nullptr, flags, 1, 2, owner, nullptr);
        DWORD error = GetLastError();
        scriptedCtrl = false;
        Check(result == 42 && error == 0x1234 && nativeProbeIncomingError == 0x7788,
              "fallback preserves incoming last error, native result and outgoing error");
        Check(nativeProbeFlags == flags && nativeProbeClean && nativeProbeBufferedDepth == originalDepth,
              "fallback forwards original flags after detaching callbacks and buffered painting");
        Check(dwmWrites == originalDwm && compositionWrites == originalComposition &&
              bufferedPaintDepth == originalDepth && activeOwnerSubclasses == 0 &&
              !etm::t_scope && !etm::g_scopes && etm::g_calls.load() == 0,
              "fallback makes no DWM/composition edits and leaves no resources behind");
        Check((scenario != 1 || ctrlStateIndex >= 2) && !failNextBufferedInit && !failNextSubclass &&
              (scenario != 0 || bufferedInitCalls == originalInits),
              "requested fallback trigger exercised; early Ctrl skips buffered initialization");
    }
    etm::g_settings.ctrlBypass = bypass;
    etm::g_settings.hideMenuBorder = hideBorder;
    etm::g_settings.material = material;
    etm::g_trackPopupMenuEx = original;
    etm::g_trackPopupMenu = originalLegacy;
}

HRESULT WINAPI ObserveBackground(HTHEME theme, HDC dc, int part, int state,
                                 const RECT* rect, const RECT* clip) {
    if (auto popup = etm::DrawingPopup(theme, dc)) {
        ++backgrounds;
        if (etm::IsSystemMaterial(etm::g_settings.material)) Trace(popup->hwnd).drawn = true;
    }
    return etm::DrawThemeBackgroundHook(theme, dc, part, state, rect, clip);
}
HRESULT WINAPI ObserveText(HTHEME theme, HDC dc, int part, int state,
                           PCWSTR text, int len, DWORD flags, DWORD flags2,
                           const RECT* rect) {
    if (etm::DrawingPopup(theme, dc)) ++texts;
    return etm::DrawThemeTextHook(theme, dc, part, state, text, len, flags, flags2, rect);
}

LRESULT CALLBACK OwnerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (previewMode && msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (previewMode && msg == WM_COMMAND) {
        if (LOWORD(wp) == 1001 && HIWORD(wp) == CBN_SELCHANGE)
            etm::g_settings.material = static_cast<etm::Material>(SendMessageW(previewMaterials, CB_GETCURSEL, 0, 0));
        if (LOWORD(wp) == 1002 && HIWORD(wp) == CBN_SELCHANGE)
            etm::g_settings.theme = static_cast<int>(SendMessageW(previewThemes, CB_GETCURSEL, 0, 0));
        if (LOWORD(wp) == 1004)
            etm::g_settings.hideMenuBorder = SendMessageW(previewBorder, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (LOWORD(wp) == 1005)
            etm::g_settings.roundedCorners = SendMessageW(previewCorners, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (LOWORD(wp) == 1003 && previewMenu) {
            POINT point{250, 180};
            ClientToScreen(hwnd, &point);
            TrackPopupMenuEx(previewMenu, TPM_RETURNCMD, point.x, point.y, hwnd, nullptr);
        }
        return 0;
    }
    if (previewMode && msg == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT client{}; GetClientRect(hwnd, &client);
        for (int y = 70; y < client.bottom; y += 40) for (int x = 0; x < client.right; x += 100) {
            RECT tile{x, y, x+100, y+40};
            HBRUSH brush = CreateSolidBrush(((x/100+y/40)&1) ? RGB(240,248,255) : RGB(255,255,255));
            FillRect(dc, &tile, brush); DeleteObject(brush);
            SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(50,90,130));
            DrawTextW(dc, L"ABC 0123", -1, &tile, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        EndPaint(hwnd, &paint); return 0;
    }
    if (msg == WM_MEASUREITEM) {
        auto item = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        item->itemWidth = 160;
        item->itemHeight = 28;
        return TRUE;
    }
    if (msg == WM_DRAWITEM) {
        auto item = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (shellFixture) {
            ++shellDraws;
            if (reinterpret_cast<HMENU>(item->hwndItem) == shellChild) shellChildDrawn = true;
            if (!etm::DrawingPopupForDC(item->hDC)) ++shellMisses;
            FillRect(item->hDC, &item->rcItem, GetSysColorBrush(COLOR_MENU));
            HTHEME theme = OpenThemeData(hwnd, L"Menu");
            DrawThemeBackground(theme, item->hDC, MENU_POPUPITEM,
                item->itemState & ODS_SELECTED ? MPI_HOT : MPI_NORMAL, &item->rcItem, nullptr);
            RECT bounds = item->rcItem;
            bounds.left += previewMode ? 36 : 16;
            if (previewMode && previewIcon) {
                HDC source = CreateCompatibleDC(item->hDC);
                HGDIOBJ saved = SelectObject(source, previewIcon);
                BitBlt(item->hDC, item->rcItem.left+5, item->rcItem.top+2,
                       24, 24, source, 0, 0, SRCCOPY);
                SelectObject(source, saved); DeleteDC(source);
            }
            auto popup = etm::DrawingPopupForDC(item->hDC);
            unsigned before = popup ? popup->shellTextDraws : 0;
            DrawTextW(item->hDC, previewMode ? L"Legacy RGB icon" : L"Shell DrawTextW / \u81ea\u7ed8", -1, &bounds, DT_SINGLELINE | DT_VCENTER);
            if (popup && popup->shellTextDraws > before) ++shellHookedTexts;
            CloseThemeData(theme);
            return TRUE;
        }
        FillRect(item->hDC, &item->rcItem, GetSysColorBrush(COLOR_MENU));
        DrawTextW(item->hDC, L"Unknown owner draw", -1, &item->rcItem, DT_SINGLELINE);
        return TRUE;
    }
    if (msg == WM_TIMER) {
        if (processingFixtureTimer) return 0;
        struct TimerGuard {
            TimerGuard() { processingFixtureTimer = true; }
            ~TimerGuard() { processingFixtureTimer = false; }
        } timerGuard;
        auto scope = etm::t_scope;
        unsigned live = 0;
        HWND popupWindow = nullptr;
        if (scope) for (unsigned i = 0; i < scope->count; ++i) {
            auto& popup = scope->popups[i];
            if (popup.active) {
                ++live; popupWindow = popup.hwnd; SampleFrame(Trace(popup.hwnd));
                auto& trace = Trace(popup.hwnd);
                if (etm::g_settings.hideMenuBorder && !trace.lateBorderWrites) {
                    if (!emulateUnprotectedBorder) CheckBorderHookScope(popup, hwnd);
                    for (COLORREF requested : {static_cast<COLORREF>(DWMWA_COLOR_DEFAULT), RGB(225, 225, 225)}) {
                        ++trace.lateBorderWrites;
                        unsigned before = trace.borderWrites;
                        HRESULT result = DwmSetWindowAttribute(popup.hwnd, DWMWA_BORDER_COLOR, &requested, sizeof(requested));
                        Check(SUCCEEDED(result) && trace.borderWrites == before + 1 && trace.lastBorderColor == DWMWA_COLOR_NONE,
                              "late Shell-style border reset cannot restore a modified menu outline");
                    }
                }
            }
        }
        if (!step) {
            if (expectActive) {
                Check(live > 0, "native popup accepted with acrylic");
                accepted += live;
                if (!alphaChecked && live) {
                    alphaChecked = true;
                    CheckAlpha(hwnd);
                }
                if (etm::IsSystemMaterial(etm::g_settings.material) && popupWindow) {
                    auto& trace = Trace(popupWindow);
                    Check(trace.writes == 1 && !trace.firstVisible && !trace.changedAfterDrawing,
                          "system popup configured once before visibility and drawing");
                    Check(trace.frames == 0 && !trace.redrawRequested,
                          "system popup causes no synthetic frame repositioning");
                    BOOL enabled = FALSE;
                    Check(SUCCEEDED(DwmGetWindowAttribute(popupWindow, DWMWA_NCRENDERING_ENABLED,
                          &enabled, sizeof(enabled))) && trace.haveOriginalNc &&
                          enabled == trace.originalNc && trace.ncPolicyWrites == 0,
                          "system popup preserves its original non-client rendering policy");
                    DWORD backdrop = 0;
                    Check(SUCCEEDED(DwmGetWindowAttribute(popupWindow, DWMWA_SYSTEMBACKDROP_TYPE,
                          &backdrop, sizeof(backdrop))) && backdrop >= DWMSBT_MAINWINDOW && backdrop <= DWMSBT_TABBEDWINDOW,
                          "system backdrop attribute remains applied with native frame policy");
                }
            } else Check(live == 0, "unsupported/bypassed popup stays native");
            if (live && etm::g_settings.hideMenuBorder) {
                auto popup = scope->Find(popupWindow);
                Check(SUCCEEDED(Trace(popupWindow).lastBorderResult) &&
                      Trace(popupWindow).lastBorderColor == DWMWA_COLOR_NONE,
                      "DWM accepts the hidden-outline setting");
                SendMessageW(popupWindow, WM_NCPAINT, 1, 0);
                RECT window{}; GetWindowRect(popupWindow, &window);
                HRGN update = CreateRectRgnIndirect(&window);
                SendMessageW(popupWindow, WM_NCPAINT, reinterpret_cast<WPARAM>(update), 0);
                RECT remaining{};
                Check(GetRgnBox(update, &remaining) != ERROR && Trace(popupWindow).borderPaints >= 2,
                      "border painting preserves ownership of the original update region");
                DeleteObject(update);
                if (auditBorderRestore) {
                    COLORREF expected = popup->haveBorderColor ? popup->oldBorderColor : DWMWA_COLOR_DEFAULT;
                    Check(!seedBorderColor || (popup->haveBorderColor && expected == kSeedBorderColor),
                          "readable custom border state captured in controlled getter case");
                    etm::RestorePopup(*popup);
                    Check(SUCCEEDED(Trace(popupWindow).lastBorderResult) && Trace(popupWindow).lastBorderColor == expected,
                          "cleanup submits the saved border color or the documented default");
                    KillTimer(hwnd, 1); EndMenu(); return 0;
                }
            }
        }
        if (live >= 2) sawSubmenu = true;
        if (live >= 2) {
            if (!childSeenAt) childSeenAt = GetTickCount64();
            // The real Explorer trace changes DWM bounds around 172 ms. Keep
            // the child up well past that, instead of closing it at 120 ms.
            if (GetTickCount64() - childSeenAt < 600) return 0;
        }
        if (shellFixture && popupWindow && step < 2) {
            if (step == 0) {
                // Exercise more draws than the old fixed CWP frame stack held.
                // Each nested subclass dispatch must restore the outer context.
                HDC dc = GetDC(popupWindow);
                DRAWITEMSTRUCT draw{};
                draw.CtlType = ODT_MENU;
                draw.itemID = 100;
                draw.hwndItem = reinterpret_cast<HWND>(scope->Find(popupWindow)->menu);
                draw.hDC = dc;
                GetClientRect(popupWindow, &draw.rcItem);
                for (unsigned i = 0; i < 160; ++i)
                    SendMessageW(hwnd, WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&draw));
                ReleaseDC(popupWindow, dc);
            }
            PostMessageW(popupWindow, WM_KEYDOWN, step == 0 ? VK_DOWN : VK_RIGHT, 0);
            ++step;
            return 0;
        }
        if (shellFixture && popupWindow && live < 2 && GetTickCount64() - menuLoopStarted < 2000) {
            PostMessageW(popupWindow, WM_KEYDOWN, VK_RIGHT, 0);
            return 0;
        }
        if (cancelForUnload && step == 0) {
            Wh_ModBeforeUninit();
            ++step;
            return 0;
        }
        if (selectCommand && popupWindow && step < 2) {
            // Wait for the observed child instead of assuming it opened within
            // one timer tick. Never dispatch its command to the parent menu.
            if (step == 1 && live < 2) {
                if (GetTickCount64() - menuLoopStarted < 2000) {
                    PostMessageW(popupWindow, WM_CHAR, L's', 0);
                    return 0;
                }
                KillTimer(hwnd, 1); EndMenu(); return 0;
            }
            PostMessageW(popupWindow, WM_CHAR, step == 0 ? L's' : L't', 0);
        } else {
            KillTimer(hwnd, 1);
            EndMenu();
        }
        ++step;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void CheckAlpha(HWND owner) {
    constexpr int width = 360, height = 180;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    HDC dc = CreateCompatibleDC(nullptr);
    DWORD* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS,
                                    reinterpret_cast<void**>(&pixels), nullptr, 0);
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    HTHEME menuTheme = OpenThemeData(owner, L"Menu");
    HTHEME buttonTheme = OpenThemeData(owner, L"Button");
    auto scope = etm::t_scope;
    auto popup = scope->Find(scope->paintingWindow);
    if (!popup || !popup->active) {
        for (unsigned i = 0; i < scope->count; ++i)
            if (scope->popups[i].active) popup = &scope->popups[i];
    }
    HWND oldPainting = scope->paintingWindow;
    scope->paintingWindow = popup->hwnd;
    bool oldDark = popup->dark;
    HDC ownerDC = GetDC(owner);
    Check(!etm::DrawingPopup(menuTheme, ownerDC), "drawing gate excludes other window DCs");
    ReleaseDC(owner, ownerDC);
    RECT all{0, 0, width, height};
    Check(!etm::ClearBackground(buttonTheme, dc, MENU_POPUPBACKGROUND, 0, &all, nullptr),
          "drawing gate excludes other theme classes");
    for (int i = 0; i < width * height; ++i) pixels[i] = 0xffffffff;
    DrawThemeBackground(menuTheme, dc, MENU_POPUPBACKGROUND, 0, &all, nullptr);
    GdiFlush();
    unsigned alphaPixels = 0;
    for (int i = 0; i < width * height; ++i) if (pixels[i] >> 24) ++alphaPixels;
    Check(alphaPixels == 0, "menu background clears to zero alpha");
    for (int i = 0; i < width * height; ++i) pixels[i] = 0x7f112233;
    RECT partial{10, 10, 40, 40};
    DrawThemeBackground(menuTheme, dc, MENU_POPUPBACKGROUND, 0, &all, &partial);
    GdiFlush();
    Check(pixels[20 * width + 20] == 0 && pixels[50 * width + 50] == 0x7f112233,
          "background painting respects explicit clipping");
    DrawThemeBackground(menuTheme, dc, MENU_POPUPBACKGROUND, 0, &all, nullptr);
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    HFONT font = CreateFontIndirectW(&metrics.lfMenuFont);
    HGDIOBJ oldFont = SelectObject(dc, font);
    for (int row = 0; row < 3; ++row) {
        popup->dark = row > 0;
        RECT textRect{20, row * 60 + 12, width - 20, row * 60 + 48};
        HRESULT result = DrawThemeText(menuTheme, dc, MENU_POPUPITEM,
            row == 2 ? MPI_DISABLED : MPI_NORMAL,
            row == 0 ? L"Light / 浅色菜单" : row == 1 ? L"Dark / 深色菜单" : L"Disabled / 禁用项",
            -1, DT_SINGLELINE | DT_VCENTER, 0, &textRect);
        Check(SUCCEEDED(result), "composited menu text draws successfully");
    }
    GdiFlush();
    alphaPixels = 0;
    for (int i = 0; i < width * height; ++i) if (pixels[i] >> 24) ++alphaPixels;
    printf("Text alpha pixels: %u\n", alphaPixels);
    Check(alphaPixels > 100 && alphaPixels < width * height / 2,
          "glyphs have visible alpha without an opaque text rectangle");
    // Flatten this renderer-owned DIB onto a checkerboard for an inspectable
    // artifact. This is a rendering fixture, not a desktop screenshot.
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        DWORD& pixel = pixels[y * width + x];
        unsigned alpha = pixel >> 24;
        unsigned bg = y < 60 ? 235 : 42;
        if ((x / 12 + y / 12) % 2) bg += y < 60 ? 12 : 12;
        DWORD color = 0xff000000;
        for (int shift = 0; shift <= 16; shift += 8) {
            unsigned value = ((pixel >> shift) & 255) + (bg * (255 - alpha) + 127) / 255;
            color |= (value > 255 ? 255 : value) << shift;
        }
        pixel = color;
    }
    BITMAPFILEHEADER fileHeader{};
    fileHeader.bfType = 0x4d42;
    fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize = fileHeader.bfOffBits + width * height * 4;
    FILE* file = fopen("build/text-alpha-fixture.bmp", "wb");
    if (file) {
        fwrite(&fileHeader, sizeof(fileHeader), 1, file);
        fwrite(&bmi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, file);
        fwrite(pixels, width * height * 4, 1, file);
        fclose(file);
    }
    const DRAWITEMSTRUCT* oldItem = scope->drawingItem;
    DRAWITEMSTRUCT shellItem{};
    shellItem.CtlType = ODT_MENU;
    shellItem.hwndItem = reinterpret_cast<HWND>(popup->menu);
    shellItem.hDC = dc;
    scope->drawingItem = &shellItem;
    popup->dark = oldDark;
    for (int i = 0; i < width * height; ++i) pixels[i] = 0xffffffff;
    FillRect(dc, &all, GetSysColorBrush(COLOR_MENU));
    GdiFlush();
    Check(pixels[0] == 0 && pixels[width * height - 1] == 0,
          "Shell FillRect background clears alpha on first draw");
    RECT gdiText{20, 12, width - 20, 48};
    int gdiHeight = DrawTextW(dc, L"Shell DrawTextW / \u6587\u5b57", -1, &gdiText, DT_SINGLELINE | DT_VCENTER);
    GdiFlush();
    unsigned shellAlpha = 0;
    for (int i = 0; i < width * height; ++i) if (pixels[i] >> 24) ++shellAlpha;
    printf("Shell text alpha pixels: %u, height: %d\n", shellAlpha, gdiHeight);
    Check(gdiHeight > 0 && shellAlpha > 100 && shellAlpha < width * height / 2,
          "Shell DrawTextW has glyph alpha and preserves its height result");
    // A legacy RGB icon with absent alpha and a corner-connected white background.
    BITMAPINFO iconInfo{};
    iconInfo.bmiHeader = {sizeof(BITMAPINFOHEADER), 24, -24, 1, 32, BI_RGB};
    DWORD* iconPixels = nullptr;
    HBITMAP iconBitmap = CreateDIBSection(nullptr, &iconInfo, DIB_RGB_COLORS,
        reinterpret_cast<void**>(&iconPixels), nullptr, 0);
    HDC iconDC = CreateCompatibleDC(nullptr);
    HGDIOBJ oldIcon = SelectObject(iconDC, iconBitmap);
    for (int i = 0; i < 24*24; ++i) iconPixels[i] = 0x00ffffff;
    for (int y = 5; y < 19; ++y) for (int x = 5; x < 19; ++x) iconPixels[y*24+x] = 0x0000aa00;
    GdiFlush();
    {
        etm::BitmapGuard guard;
        etm::IconSurface normalized;
        Check(etm::PrepareIcon(iconDC, normalized), "legacy RGB icon normalized on an owned copy");
        DIBSECTION section{};
        GetObjectW(normalized.bitmap, sizeof(section), &section);
        auto normalizedPixels = static_cast<DWORD*>(section.dsBm.bmBits);
        Check(normalizedPixels && normalizedPixels[0] == 0 &&
              normalizedPixels[12*24+12] == 0xff00aa00,
              "legacy icon keeps colored pixels and clears only its connected border");
    }
    Check(iconPixels[0] == 0x00ffffff && iconPixels[12*24+12] == 0x0000aa00,
          "source icon bitmap remains unchanged");
    for (int i = 0; i < width*height; ++i) pixels[i] = 0;
    Check(BitBlt(dc, 0, 0, 24, 24, iconDC, 0, 0, SRCCOPY), "legacy icon BitBlt is alpha blended");
    GdiFlush();
    Check(pixels[0] == 0 && (pixels[12*width+12] >> 24) == 255,
          "icon transfer has transparent background and visible colored center");
    SelectObject(iconDC, oldIcon);
    DeleteObject(iconBitmap);
    DeleteDC(iconDC);
    for (int i = 0; i < width*height; ++i) pixels[i] = 0;
    popup->dark = false;
    RECT hover{0, 0, width, 40};
    Check(etm::DrawHighlight(*popup, dc, &hover, nullptr), "light-theme hover highlight rendered");
    GdiFlush();
    Check((pixels[20*width+width/2] >> 24) >= 65 && pixels[0] == 0,
          "hover highlight has visible alpha and rounded transparent corners");
    BYTE firstHighlightAlpha = pixels[20*width+width/2] >> 24;
    etm::DrawHighlight(*popup, dc, &hover, nullptr);
    GdiFlush();
    Check((pixels[20*width+width/2] >> 24) == firstHighlightAlpha,
          "repeated hover does not accumulate opacity");
    for (int i = 0; i < width*height; ++i) pixels[i] = 0x7f112233;
    RECT client{4, 5, width - 7, height - 6};
    Check(etm::ClearBorderPixels(dc, all, client), "non-client border alpha cleared");
    GdiFlush();
    Check(pixels[0] == 0 && pixels[width*height-1] == 0 && pixels[10*width+10] == 0x7f112233,
          "border clearing preserves every tested client-area pixel");
    RECT printedOuter{}, printedClient{};
    bool originalHide = etm::g_settings.hideMenuBorder;
    if (etm::GetBorderRects(popup->hwnd, printedOuter, printedClient) &&
        printedOuter.right <= width && printedOuter.bottom <= height) {
        for (int i = 0; i < width*height; ++i) pixels[i] = 0x7f112233;
        etm::g_settings.hideMenuBorder = false;
        SendMessageW(popup->hwnd, WM_PRINT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_NONCLIENT);
        GdiFlush();
        DWORD savedClient[width*height];
        memcpy(savedClient, pixels, sizeof(savedClient));
        for (int i = 0; i < width*height; ++i) pixels[i] = 0x7f112233;
        etm::g_settings.hideMenuBorder = true;
        SendMessageW(popup->hwnd, WM_PRINT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_NONCLIENT);
        GdiFlush();
        bool preserved = true, cleared = true;
        for (int y = 0; y < printedOuter.bottom; ++y) for (int x = 0; x < printedOuter.right; ++x) {
            bool inside = x >= printedClient.left && x < printedClient.right &&
                y >= printedClient.top && y < printedClient.bottom;
            if (inside) preserved = preserved && pixels[y*width+x] == savedClient[y*width+x];
            else cleared = cleared && pixels[y*width+x] == 0;
        }
        Check(preserved && cleared, "WM_PRINT hides only the frame and preserves captured client content");
    }
    etm::g_settings.hideMenuBorder = originalHide;
    scope->drawingItem = oldItem;
    popup->dark = oldDark;
    scope->paintingWindow = oldPainting;
    CloseThemeData(buttonTheme);
    CloseThemeData(menuTheme);
    SelectObject(dc, oldFont);
    DeleteObject(font);
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

void ShowTestMenu(HWND owner, HMENU menu, bool active, bool select = false, bool legacy = false) {
    backdropTraceCount = 0;
    expectActive = active;
    selectCommand = select;
    step = 0;
    childSeenAt = 0;
    menuLoopStarted = GetTickCount64();
    SetForegroundWindow(owner);
    SetTimer(owner, 1, 120, nullptr);
    UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_NOANIMATION;
    HHOOK positionHook = SetWindowsHookExW(WH_CALLWNDPROC, PositionObserver, nullptr, GetCurrentThreadId());
    Check(positionHook != nullptr, "native popup geometry observer installed");
    int x = placeNearEdge ? GetSystemMetrics(SM_CXSCREEN) - 40 : 260;
    int y = placeNearEdge ? GetSystemMetrics(SM_CYSCREEN) - 60 : 240;
    BOOL command = legacy ? TrackPopupMenu(menu, flags, x, y, 0, owner, nullptr)
                          : TrackPopupMenuEx(menu, flags, x, y, owner, nullptr);
    if (positionHook) UnhookWindowsHookEx(positionHook);
    KillTimer(owner, 1);
    if (etm::IsSystemMaterial(etm::g_settings.material) && active) {
        Check(backdropTraceCount >= (select || shellFixture ? 2u : 1u), "system material root and requested child observed");
        for (unsigned i = 0; i < backdropTraceCount; ++i) {
            const auto& trace = backdropTraces[i];
            Check(trace.writes == 1 && trace.frames == 0 && !trace.firstVisible &&
                  !trace.changedAfterDrawing && !trace.redrawRequested,
                  "system material lifecycle has no repeated visible or paint-time setup");
            Check(trace.shown && !trace.movedAfterShown,
                  "native root/child popup geometry stays fixed after first show");
            Check(trace.haveShownFrame && !trace.frameChangedAfterShow,
                  "DWM frame bounds stay fixed through delayed submenu presentation");
        }
    }
    if (active && !auditBorderRestore) {
        for (unsigned i = 0; i < backdropTraceCount; ++i)
            Check(backdropTraces[i].borderWrites == (etm::g_settings.hideMenuBorder ? 1u : 0u) + backdropTraces[i].lateBorderWrites,
                  "border attribute is changed only when border hiding is enabled");
    }
    Check(etm::t_scope == nullptr, "tracking scope and thread callback cleaned up");
    Check(activeOwnerSubclasses == 0, "temporary owner and popup subclasses removed after tracking");
    MENUINFO info{sizeof(info)};
    info.fMask = MIM_BACKGROUND;
    GetMenuInfo(menu, &info);
    Check(info.hbrBack != GetStockObject(BLACK_BRUSH), "menu brush restored");
    if (select) {
        printf("Selected command: %d\n", command);
        Check(command == 42, "submenu command dispatched by native menu loop");
    }
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    bool preview = argc > 1 && strcmp(argv[1], "--preview") == 0;
    bool probeFrame = argc > 1 && strcmp(argv[1], "--probe-frame") == 0;
    bool probeBorder = argc > 1 && strcmp(argv[1], "--probe-border") == 0;
    bool probeUnprotectedBorder = argc > 1 && strcmp(argv[1], "--probe-unprotected-border") == 0;
    bool materialsOnly = argc > 1 && strcmp(argv[1], "--materials-only") == 0;
    const auto materials = {etm::Material::Native, etm::Material::Solid,
        etm::Material::Transparent, etm::Material::Acrylic,
        etm::Material::ModernAcrylic, etm::Material::SystemAcrylic,
        etm::Material::Mica, etm::Material::MicaAlt};
    Check(etm::ParseMaterial(L"blur") == etm::Material::Acrylic,
          "removed material in saved settings uses the default Acrylic");
    bool probeAcrylic = argc > 1 && strcmp(argv[1], "--probe-acrylic") == 0;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    previewMode = preview;
    Check(!Wh_ModInit(), "production process guard rejects non-Explorer host");
    Check(MH_Initialize() == MH_OK, "isolated hook engine initialized");
    Check(Wh_SetFunctionHook(reinterpret_cast<void*>(DwmSetWindowAttribute),
          reinterpret_cast<void*>(ObserveDwmSetAttribute),
          reinterpret_cast<void**>(&realDwmSetWindowAttribute)), "DwmSetWindowAttribute hook installed");
    etm::g_dwmSetWindowAttribute = TestDwmSetAttribute;
    etm::g_getThemeClass = reinterpret_cast<etm::GetThemeClass_t>(GetProcAddress(
        GetModuleHandleW(L"uxtheme.dll"), MAKEINTRESOURCEA(74)));
    Check(etm::g_getThemeClass != nullptr, "UxTheme class export available");
    if (!etm::g_getThemeClass) return 1;
    // Automated fixtures must not depend on the user's physical Ctrl state.
    // Native/nested bypass behavior is checked separately by CheckNativeBypass.
    etm::g_settings = {true, preview, true};
    etm::g_setComposition = reinterpret_cast<etm::CompositionAttribute_t>(GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
    realComposition = etm::g_setComposition;
    etm::g_setComposition = TestComposition;
    etm::g_getComposition = reinterpret_cast<etm::CompositionAttribute_t>(GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "GetWindowCompositionAttribute"));
    Check(realComposition != nullptr, "adjustable Acrylic export available");
    Check(etm::EffectsAllowed(), "system permits transparency effects");
#define TEST_HOOK(api, hook, original) Check(Wh_SetFunctionHook((void*)api, (void*)hook, (void**)&original), #api " hook installed")
    TEST_HOOK(TrackPopupMenu, etm::TrackPopupMenuHook, etm::g_trackPopupMenu);
    TEST_HOOK(TrackPopupMenuEx, etm::TrackPopupMenuExHook, etm::g_trackPopupMenuEx);
    TEST_HOOK(DrawThemeBackground, ObserveBackground, etm::g_drawThemeBackground);
    TEST_HOOK(DrawThemeBackgroundEx, etm::DrawThemeBackgroundExHook, etm::g_drawThemeBackgroundEx);
    TEST_HOOK(DrawThemeText, ObserveText, etm::g_drawThemeText);
    TEST_HOOK(DrawThemeTextEx, etm::DrawThemeTextExHook, etm::g_drawThemeTextEx);
    TEST_HOOK(DrawTextW, etm::DrawTextHook, etm::g_drawText);
    TEST_HOOK(FillRect, etm::FillRectHook, etm::g_fillRect);
    TEST_HOOK(BitBlt, etm::BitBltHook, etm::g_bitBlt);
    TEST_HOOK(StretchBlt, etm::StretchBltHook, etm::g_stretchBlt);
    TEST_HOOK(AlphaBlend, etm::AlphaBlendHook, etm::g_alphaBlend);
#undef TEST_HOOK
    Check(MH_EnableHook(MH_ALL_HOOKS) == MH_OK, "all isolated hooks enabled");
    CheckNativeBypass();

    WNDCLASSW wc{};
    wc.lpfnWndProc = OwnerProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ETM_Smoke_Ordinary_Window";
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    RegisterClassW(&wc);
    HWND owner = CreateWindowW(wc.lpszClassName, L"Isolated menu test - no Explorer changes",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 160, 140, 680, 440, nullptr, nullptr, wc.hInstance, nullptr);
    Check(!etm::IsClassicPopup(owner), "ordinary/modern window class excluded");
    CheckLateCtrlBypass(owner);

    HMENU child = CreatePopupMenu();
    AppendMenuW(child, MF_STRING, 42, L"&Test command\tEnter");
    AppendMenuW(child, MF_STRING | MF_DISABLED, 43, L"Disabled command");
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(child), L"&Submenu");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | MF_CHECKED, 44, L"Checked command");
    Check(etm::IsSupportedMenu(menu, owner), "standard menu recognized");
    if (preview) {
        SetWindowTextW(owner, L"Menu material visual check - isolated fixture");
        previewMenu = menu;
        AppendMenuW(menu, MF_OWNERDRAW, 45, reinterpret_cast<PCWSTR>(1));
        SetPropW(owner, L"ImmersiveContextMenuArray_1-45", reinterpret_cast<HANDLE>(1));
        shellFixture = true;
        BITMAPINFO icon{};
        icon.bmiHeader = {sizeof(BITMAPINFOHEADER), 24, -24, 1, 32, BI_RGB};
        DWORD* bits = nullptr;
        previewIcon = CreateDIBSection(nullptr, &icon, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
        for (int i = 0; i < 24*24; ++i) bits[i] = 0x00ffffff;
        for (int y = 4; y < 20; ++y) for (int x = 4; x < 20; ++x) bits[y*24+x] = 0x00009933;
        previewMaterials = CreateWindowW(L"COMBOBOX", L"Material", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
            20, 20, 205, 260, owner, reinterpret_cast<HMENU>(1001), wc.hInstance, nullptr);
        for (PCWSTR label : {L"Native", L"Solid", L"Transparent",
                L"Acrylic", L"Modern Acrylic", L"System Acrylic", L"Mica", L"Mica Alt"})
            SendMessageW(previewMaterials, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        SendMessageW(previewMaterials, CB_SETCURSEL, 3, 0);
        previewThemes = CreateWindowW(L"COMBOBOX", L"Theme", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
            240, 20, 140, 140, owner, reinterpret_cast<HMENU>(1002), wc.hInstance, nullptr);
        for (PCWSTR label : {L"Auto", L"Light", L"Dark"})
            SendMessageW(previewThemes, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        SendMessageW(previewThemes, CB_SETCURSEL, 1, 0);
        etm::g_settings.theme = 1;
        CreateWindowW(L"BUTTON", L"Open menu", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            400, 20, 150, 28, owner, reinterpret_cast<HMENU>(1003), wc.hInstance, nullptr);
        previewBorder = CreateWindowW(L"BUTTON", L"Hide menu border", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            20, 50, 205, 24, owner, reinterpret_cast<HMENU>(1004), wc.hInstance, nullptr);
        previewCorners = CreateWindowW(L"BUTTON", L"Rounded corners", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            240, 50, 205, 24, owner, reinterpret_cast<HMENU>(1005), wc.hInstance, nullptr);
        SendMessageW(previewCorners, BM_SETCHECK, BST_CHECKED, 0);
        SetForegroundWindow(owner);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        DeleteObject(previewIcon);
    } else if (materialsOnly) {
        for (auto material : materials) {
            etm::g_settings.material = material;
            ShowTestMenu(owner, menu, material != etm::Material::Native);
        }
    } else if (probeFrame || probeAcrylic || probeBorder || probeUnprotectedBorder) {
        emulateUnprotectedBorder = probeUnprotectedBorder;
        etm::g_settings.material = probeAcrylic ? etm::Material::Acrylic : etm::Material::SystemAcrylic;
        etm::g_settings.hideMenuBorder = probeBorder || probeUnprotectedBorder;
        ShowTestMenu(owner, menu, true, true);
    } else {
        ShowTestMenu(owner, menu, true, true);
        Check(sawSubmenu, "child popup independently receives acrylic");
        ShowTestMenu(owner, menu, true, false, true);
        Check(backgrounds > 0, "real native menu theme painting intercepted");
        printf("Native rendering calls: background=%d text=%d\n", backgrounds, texts);
        DWORD gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        for (int i = 0; i < 8; ++i) ShowTestMenu(owner, menu, true);
        DWORD gdiAfter = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        printf("GDI handles: before=%lu after=%lu\n", gdiBefore, gdiAfter);
        Check(gdiAfter <= gdiBefore + 2, "repeated opening does not accumulate GDI handles");
        etm::g_settings.hideMenuBorder = true;
        seedBorderColor = auditBorderRestore = true;
        ShowTestMenu(owner, menu, true);
        seedBorderColor = false;
        ShowTestMenu(owner, menu, true);
        auditBorderRestore = false;
        for (bool rounded : {false, true}) {
            etm::g_settings.roundedCorners = rounded;
            for (auto material : {etm::Material::Acrylic, etm::Material::SystemAcrylic,
                    etm::Material::Mica, etm::Material::MicaAlt}) {
                etm::g_settings.material = material;
                ShowTestMenu(owner, menu, true, true);
            }
        }
        etm::g_settings.material = etm::Material::Acrylic;
        etm::g_settings.roundedCorners = true;
        DWORD hiddenBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        for (int i = 0; i < 5; ++i) ShowTestMenu(owner, menu, true);
        DWORD hiddenAfter = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        Check(hiddenAfter <= hiddenBefore + 2, "border hiding does not accumulate GDI resources");
        etm::g_settings.hideMenuBorder = false;
        HMENU custom = CreatePopupMenu();
        AppendMenuW(custom, MF_OWNERDRAW, 99, reinterpret_cast<PCWSTR>(1));
        Check(!etm::IsSupportedMenu(custom, owner), "unknown owner-draw rejected");
        ShowTestMenu(owner, custom, false);
        SetPropW(owner, L"ImmersiveContextMenuArray_1-99", reinterpret_cast<HANDLE>(1));
        Check(etm::IsSupportedMenu(custom, owner), "shell owner-draw property recognized without private data reads");
        RemovePropW(owner, L"ImmersiveContextMenuArray_1-99");
        DestroyMenu(custom);
        shellChild = CreatePopupMenu();
        HMENU shellMenu = CreatePopupMenu();
        AppendMenuW(shellChild, MF_OWNERDRAW, 101, reinterpret_cast<PCWSTR>(1));
        AppendMenuW(shellChild, MF_OWNERDRAW, 102, reinterpret_cast<PCWSTR>(1));
        AppendMenuW(shellMenu, MF_POPUP | MF_OWNERDRAW,
                    reinterpret_cast<UINT_PTR>(shellChild), reinterpret_cast<PCWSTR>(1));
        wchar_t property[96];
        MENUITEMINFOW shellInfo{sizeof(shellInfo)};
        shellInfo.fMask = MIIM_ID | MIIM_DATA;
        GetMenuItemInfoW(shellMenu, 0, TRUE, &shellInfo);
        swprintf_s(property, L"ImmersiveContextMenuArray_%lu-%lu",
            static_cast<ULONG>(shellInfo.dwItemData), static_cast<ULONG>(shellInfo.wID));
        SetPropW(owner, property, reinterpret_cast<HANDLE>(1));
        SetPropW(owner, L"ImmersiveContextMenuArray_1-101", reinterpret_cast<HANDLE>(1));
        SetPropW(owner, L"ImmersiveContextMenuArray_1-102", reinterpret_cast<HANDLE>(1));
        shellFixture = true;
        ShowTestMenu(owner, shellMenu, true);
        shellFixture = false;
        Check(shellDraws > 0 && shellMisses == 0, "Shell owner-draw intercepted from its FIRST paint");
        Check(shellChildDrawn, "Shell child owner-draw intercepted before mouse hover");
        Check(shellHookedTexts == shellDraws && shellHookedTexts > 0,
              "all native owner-draw WM_DRAWITEM calls reach composited DrawTextW");
        for (auto material : {etm::Material::SystemAcrylic, etm::Material::Mica, etm::Material::MicaAlt}) {
            etm::g_settings.material = material;
            for (int theme : {1, 2}) {
                etm::g_settings.theme = theme;
                for (bool edge : {false, true}) {
                    placeNearEdge = edge;
                    shellFixture = true;
                    ShowTestMenu(owner, shellMenu, true);
                    shellFixture = false;
                }
            }
        }
        placeNearEdge = false;
        etm::g_settings.material = etm::Material::Acrylic;
        etm::g_settings.theme = 0;
        Check(shellMisses == 0, "light and dark system Shell submenus retain first-draw context");
        RemovePropW(owner, property);
        RemovePropW(owner, L"ImmersiveContextMenuArray_1-101");
        RemovePropW(owner, L"ImmersiveContextMenuArray_1-102");
        DestroyMenu(shellMenu);
        shellChild = nullptr;
        rejectNextAccent = true;
        ShowTestMenu(owner, menu, true);
        Check(rejectedAccents == 1, "temporary composition setup failure retried without a dead cached popup");
        // Exercise the alternative documented backend too; success isn't a
        // claim that the compositor's final pixels have been visually verified.
        for (auto material : materials) {
            etm::g_settings.material = material;
            ShowTestMenu(owner, menu, material != etm::Material::Native);
        }
        for (auto material : {etm::Material::SystemAcrylic, etm::Material::Mica, etm::Material::MicaAlt}) {
            etm::g_settings.material = material;
            for (int theme : {1, 2}) {
                etm::g_settings.theme = theme;
                for (bool edge : {false, true}) {
                    placeNearEdge = edge;
                    ShowTestMenu(owner, menu, true, true);
                }
            }
        }
        placeNearEdge = false;
        etm::g_settings.theme = 0;
        etm::g_settings.material = etm::Material::Acrylic;
        cancelForUnload = true;
        ShowTestMenu(owner, menu, true);
        cancelForUnload = false;
        Check(etm::g_calls.load() == 0 && etm::g_scopes == nullptr,
              "unload cancels an active menu and drains all callback scopes");
        Wh_ModUninit();
        etm::g_stopping.store(true);
        ShowTestMenu(owner, menu, false);
        etm::g_stopping.store(false);
    }
    DestroyMenu(menu);
    DestroyWindow(owner);
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    printf("Failures: %d\n", failures);
    return failures ? 1 : 0;
}
