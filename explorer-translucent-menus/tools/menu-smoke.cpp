// Isolated-process integration harness. MinHook is TEST ONLY; the mod uses Windhawk.
#include <windows.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <commctrl.h>
#include <MinHook.h>
#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

// Use the SDK's declaration-only utility path, not Windhawk's injected runtime.
#define WH_EDITING
#define WH_MOD_ID L"isolated@explorer-translucent-menus"

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
HANDLE replayPresentationGate = nullptr;
std::atomic<HANDLE> nextReplayGate{nullptr};
BOOL WINAPI TestSetWindowPos(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags) {
    if (flags & SWP_FRAMECHANGED) {
        auto& trace = Trace(hwnd);
        ++trace.frames;
        trace.redrawRequested = trace.redrawRequested || !(flags & SWP_NOREDRAW);
    }
    if ((flags & SWP_SHOWWINDOW) && nextReplayGate.load()) {
        wchar_t name[80]{};
        if (GetClassNameW(hwnd, name, ARRAYSIZE(name)) &&
            wcscmp(name, L"ExplorerTranslucentMenus.SelectionReplay") == 0) {
            HANDLE gate = nextReplayGate.exchange(nullptr);
            if (gate && WaitForSingleObject(gate, 2000) != WAIT_OBJECT_0)
                return FALSE;
        }
    }
    return SetWindowPos(hwnd, after, x, y, cx, cy, flags);
}
unsigned forbiddenAnimationWrites = 0;
BOOL WINAPI TestSystemParametersInfoW(UINT action, UINT param, PVOID value, UINT flags) {
    if (action == SPI_SETSELECTIONFADE || action == SPI_SETMENUFADE ||
        action == SPI_SETMENUANIMATION) {
        ++forbiddenAnimationWrites;
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;  // A regression must not alter the user's desktop.
    }
    return SystemParametersInfoW(action, param, value, flags);
}

#define SystemParametersInfoW TestSystemParametersInfoW
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
#undef SystemParametersInfoW

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
bool highlightGeometryOnly = false;
bool cancelForUnload = false;
bool placeNearEdge = false;
ULONGLONG childSeenAt = 0;
ULONGLONG menuLoopStarted = 0;
bool processingFixtureTimer = false;
bool shellFixture = false;
unsigned shellDraws = 0;
unsigned shellMisses = 0;
bool shellChildDrawn = false;
HMENU shellChild = nullptr;
bool previewMode = false;
HMENU previewMenu = nullptr;
HWND previewMaterials = nullptr, previewThemes = nullptr;
HWND previewBorder = nullptr, previewCorners = nullptr;
HWND previewSelectionFade = nullptr;
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
void CheckHighlightGeometry(HWND owner);


void Check(bool condition, const char* name) {
    printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
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

constexpr COLORREF kFadeBackground = RGB(255, 235, 64);
bool selectionFixtureMode = false;
unsigned selectionCaseNumber = 0;
unsigned selectionLastCase = ~0u;

enum class FadeAction { Root, Child, Cancel, Disabled, Submenu };
enum class PositionStyle { None, RootOnly, ChildOnly, Both };
struct FadeFixture {
    bool running = false, mouse = false, sent = false;
    bool modified = false, childSeen = false, disabledStayedOpen = false;
    unsigned stage = 0, commands = 0, replayCount = 0;
    unsigned positionCommands = 0;
    UINT notifiedCommand = 0, notifiedPosition = static_cast<UINT>(-1);
    HMENU notifiedMenu = nullptr;
    bool notificationParametersValid = true;
    HMENU menu = nullptr, child = nullptr;
    FadeAction action = FadeAction::Root;
    ULONGLONG rowReady = 0;
    ULONGLONG started = 0, activated = 0;
    RECT row{};
    COLORREF samples[64]{};
    HWND replays[16]{};
    BOOL selectionFade = FALSE, menuFade = FALSE, menuAnimation = FALSE;
} fade;

bool ReadAnimationPreferences(BOOL& selection, BOOL& menu, BOOL& animation) {
    return SystemParametersInfoW(SPI_GETSELECTIONFADE, 0, &selection, 0) &&
        SystemParametersInfoW(SPI_GETMENUFADE, 0, &menu, 0) &&
        SystemParametersInfoW(SPI_GETMENUANIMATION, 0, &animation, 0);
}

void CheckAnimationPreferences() {
    BOOL selection = FALSE, menu = FALSE, animation = FALSE;
    Check(ReadAnimationPreferences(selection, menu, animation) &&
          selection == fade.selectionFade && menu == fade.menuFade &&
          animation == fade.menuAnimation && !forbiddenAnimationWrites,
          "selection and opening animation desktop preferences remain unchanged");
}

bool IsReplayWindow(HWND hwnd) {
    wchar_t name[80]{};
    return GetClassNameW(hwnd, name, ARRAYSIZE(name)) &&
        wcscmp(name, L"ExplorerTranslucentMenus.SelectionReplay") == 0;
}

void RecordReplay(HWND hwnd) {
    if (!IsReplayWindow(hwnd)) return;
    for (unsigned i = 0; i < fade.replayCount; ++i)
        if (fade.replays[i] == hwnd) return;
    if (fade.replayCount < ARRAYSIZE(fade.replays))
        fade.replays[fade.replayCount++] = hwnd;
}

void CALLBACK ReplayEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object,
                          LONG child, DWORD, DWORD) {
    if (object == OBJID_WINDOW && child == CHILDID_SELF &&
        (event == EVENT_OBJECT_CREATE || event == EVENT_OBJECT_SHOW))
        RecordReplay(hwnd);
}

BOOL CALLBACK FindReplay(HWND hwnd, LPARAM param) {
    DWORD process = 0;
    GetWindowThreadProcessId(hwnd, &process);
    if (process == GetCurrentProcessId() && IsReplayWindow(hwnd)) {
        RecordReplay(hwnd);
        if (IsWindowVisible(hwnd)) ++*reinterpret_cast<unsigned*>(param);
    }
    return TRUE;
}

unsigned VisibleReplays() {
    unsigned count = 0;
    EnumWindows(FindReplay, reinterpret_cast<LPARAM>(&count));
    return count;
}

void PumpFor(DWORD milliseconds) {
    ULONGLONG end = GetTickCount64() + milliseconds;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (GetTickCount64() >= end) break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
    } while (true);
}

unsigned WaitForVisibleReplay() {
    ULONGLONG deadline = GetTickCount64() + 250;
    unsigned visible;
    while (!(visible = VisibleReplays()) && GetTickCount64() < deadline)
        PumpFor(5);
    return visible;
}

void SampleSelectedRow(COLORREF* samples) {
    for (unsigned i = 0; i < 64; ++i) samples[i] = CLR_INVALID;
    int width = fade.row.right - fade.row.left;
    int height = fade.row.bottom - fade.row.top;
    if (width <= 0 || height <= 0) return;
    HDC screen = GetDC(nullptr);
    etm::IconSurface image;
    image.dc = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
    DWORD* pixels = nullptr;
    image.bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS,
        reinterpret_cast<void**>(&pixels), nullptr, 0);
    if (image.dc && image.bitmap) image.old = SelectObject(image.dc, image.bitmap);
    bool captured = image.old && image.old != HGDI_ERROR &&
        BitBlt(image.dc, 0, 0, width, height, screen, fade.row.left,
               fade.row.top, SRCCOPY | CAPTUREBLT) && GdiFlush();
    ReleaseDC(nullptr, screen);
    if (!captured) return;
    // One screen transfer samples a single frame; repeated desktop GetPixel
    // calls can span most of the animation and compare different frames.
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 16; ++x) {
        DWORD pixel = pixels[(height * (2 * y + 1) / 8) * width +
                             width * (2 * x + 1) / 32];
        samples[y * 16 + x] = RGB((pixel >> 16) & 255, (pixel >> 8) & 255, pixel & 255);
    }
}


unsigned ColorDistance(COLORREF a, COLORREF b) {
    int red = static_cast<int>(GetRValue(a)) - GetRValue(b);
    int green = static_cast<int>(GetGValue(a)) - GetGValue(b);
    int blue = static_cast<int>(GetBValue(a)) - GetBValue(b);
    return static_cast<unsigned>(std::abs(red) + std::abs(green) + std::abs(blue));
}

void SendFadeInput(HWND popup, HMENU menu, unsigned position, bool activate) {
    RECT row{};
    if (!GetMenuItemRect(nullptr, menu, position, &row)) return;
    if (fade.mouse) {
        // Real mouse input: posting internal MN_* messages is not activation.
        SetCursorPos((row.left + row.right) / 2, (row.top + row.bottom) / 2);
        if (!activate) {
            INPUT move[2]{};
            move[0].type = move[1].type = INPUT_MOUSE;
            move[0].mi.dwFlags = move[1].mi.dwFlags =
                MOUSEEVENTF_MOVE | MOUSEEVENTF_MOVE_NOCOALESCE;
            move[0].mi.dx = 1;
            move[1].mi.dx = -1;
            Check(SendInput(2, move, sizeof(INPUT)) == 2,
                  "hover is delivered even when successive popups reuse the cursor position");
        }
        if (activate) {
            INPUT input[3]{};
            input[0].type = input[1].type = input[2].type = INPUT_MOUSE;
            input[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE |
                MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE_NOCOALESCE;
            input[0].mi.dx = MulDiv((row.left + row.right) / 2 -
                GetSystemMetrics(SM_XVIRTUALSCREEN), 65535,
                GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1);
            input[0].mi.dy = MulDiv((row.top + row.bottom) / 2 -
                GetSystemMetrics(SM_YVIRTUALSCREEN), 65535,
                GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);
            input[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            input[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            Check(SendInput(3, input, sizeof(INPUT)) == 3, "native mouse activation injected");
        }
    } else if (activate) {
        wchar_t key = position == 0 ? (menu == fade.child ? L'c' : L'r') :
            position == 1 ? L'd' : L's';
        PostMessageW(popup, WM_CHAR, key, 0);
    } else if (position == 0) {
        PostMessageW(popup, WM_KEYDOWN, VK_HOME, 0);
    }
}

void FadeTimer(HWND owner) {
    auto scope = etm::t_scope;
    HWND root = nullptr, child = nullptr;
    if (scope) for (unsigned i = 0; i < scope->count; ++i) {
        const auto& popup = scope->popups[i];
        if (!popup.active || !IsWindowVisible(popup.hwnd)) continue;
        fade.modified = true;
        if (popup.menu == fade.menu) root = popup.hwnd;
        if (popup.menu == fade.child) child = popup.hwnd;
    }
    if (child) fade.childSeen = true;
    if (GetTickCount64() - fade.started > 2500) {
        Check(false, "selection fixture completes without a native-loop timeout");
        KillTimer(owner, 2);
        EndMenu();
        return;
    }
    if (!root) return;
    if (fade.sent) {
        if (fade.action == FadeAction::Disabled &&
            GetTickCount64() - fade.activated >= 150) {
            Check(VisibleReplays() == 0, "disabled activation keeps the menu open without replay");
            fade.disabledStayedOpen = true;
            EndMenu();
        }
        return;
    }
    bool needsChild = fade.action == FadeAction::Child || fade.action == FadeAction::Submenu;
    if (needsChild && !child) {
        if (!fade.stage) {
            SendFadeInput(root, fade.menu, 2, true);
            fade.stage = 1;
        }
        return;
    }
    if (fade.action == FadeAction::Cancel || fade.action == FadeAction::Submenu) {
        Check(VisibleReplays() == 0, "cancellation and opening a submenu create no replay");
        fade.sent = true;
        EndMenu();
        return;
    }
    HWND popup = needsChild ? child : root;
    HMENU menu = needsChild ? fade.child : fade.menu;
    unsigned position = fade.action == FadeAction::Disabled ? 1 : 0;
    if (fade.stage < 2) {
        SendFadeInput(popup, menu, position, false);
        fade.stage = 2;
        fade.rowReady = GetTickCount64() + 350;
        return;  // Let the highlighted row reach the desktop compositor.
    }
    if (GetTickCount64() < fade.rowReady) return;
    if (fade.stage == 2) {
        GetMenuItemRect(nullptr, menu, position, &fade.row);
        DwmFlush();
        SampleSelectedRow(fade.samples);
        fade.stage = 3;
        return;  // Do not capture and inject activation in the same timer dispatch.
    }
    CheckAnimationPreferences();
    fade.activated = GetTickCount64();
    SendFadeInput(popup, menu, position, true);
    fade.sent = true;
}

void CheckReplayShapePixels(bool rounded) {
    int width = fade.row.right - fade.row.left;
    int height = fade.row.bottom - fade.row.top;
    if (width < 8 || height < 8) {
        Check(false, "replay row has usable screen dimensions");
        return;
    }
    HDC screen = GetDC(nullptr);
    etm::IconSurface image;
    image.dc = screen ? CreateCompatibleDC(screen) : nullptr;
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
    DWORD* pixels = nullptr;
    if (screen)
        image.bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS,
            reinterpret_cast<void**>(&pixels), nullptr, 0);
    if (image.dc && image.bitmap) image.old = SelectObject(image.dc, image.bitmap);
    bool captured = pixels && image.old && image.old != HGDI_ERROR &&
        BitBlt(image.dc, 0, 0, width, height, screen, fade.row.left,
               fade.row.top, SRCCOPY | CAPTUREBLT) && GdiFlush();
    if (screen) ReleaseDC(nullptr, screen);
    Check(captured, "replay screen pixels captured");
    if (!captured) return;
    auto distance = [&](int x, int y) {
        DWORD pixel = pixels[y * width + x];
        return ColorDistance(RGB((pixel >> 16) & 255, (pixel >> 8) & 255,
                                 pixel & 255), kFadeBackground);
    };
    int left = 0, right = width - 1, top = 0, bottom = height - 1;
    while (left < width && distance(left, height / 2) <= 35) ++left;
    while (right >= 0 && distance(right, height / 2) <= 35) --right;
    while (top < height && distance(width * 3 / 4, top) <= 35) ++top;
    while (bottom >= 0 && distance(width * 3 / 4, bottom) <= 35) --bottom;
    bool visible = left + 2 < right && top + 2 < bottom &&
                   distance(width * 3 / 4, height / 2) > 35;
    Check(visible, "replay contains a visible selected row");
    if (!visible) return;
    bool margins = distance(0, height / 2) <= 20 &&
                   distance(width - 1, height / 2) <= 20 &&
                   distance(width * 3 / 4, 0) <= 20 &&
                   distance(width * 3 / 4, height - 1) <= 20;
    Check(margins, "replay leaves the inset and menu margins transparent");
    unsigned corners[] = {distance(left + 1, top + 1),
                          distance(right - 1, top + 1),
                          distance(left + 1, bottom - 1),
                          distance(right - 1, bottom - 1)};
    bool shape = true;
    for (unsigned corner : corners)
        shape = shape && (rounded ? corner <= 20 : corner > 35);
    Check(shape, rounded ? "replay leaves rounded corners transparent"
                         : "zero-radius replay keeps square corners");
}

void RunReplayShapeCase(HWND owner, int radius, bool child, bool ownerDraw,
                        bool nativeHighlight = false, HANDLE presentationGate = nullptr) {
    printf("Replay mask: radius=%d target=%s highlight=%s\n", radius,
           child ? "submenu" : ownerDraw ? "owner-draw" : "root",
           nativeHighlight ? "native" : "custom");
    fade = {};
    backdropTraceCount = 0;
    fade.running = true;
    fade.mouse = true;
    fade.action = child ? FadeAction::Child : FadeAction::Root;
    Check(ReadAnimationPreferences(fade.selectionFade, fade.menuFade, fade.menuAnimation),
          "desktop animation preferences are readable");
    fade.menu = CreatePopupMenu();
    fade.child = CreatePopupMenu();
    AppendMenuW(fade.child, MF_STRING, 42, L"&Child command");
    AppendMenuW(fade.menu, ownerDraw ? MF_OWNERDRAW : MF_STRING, 44,
                ownerDraw ? reinterpret_cast<PCWSTR>(1) : L"&Root command");
    AppendMenuW(fade.menu, MF_STRING | MF_DISABLED, 45, L"&Disabled command");
    AppendMenuW(fade.menu, MF_POPUP, reinterpret_cast<UINT_PTR>(fade.child), L"&Submenu");
    if (ownerDraw) {
        SetPropW(owner, L"ImmersiveContextMenuArray_1-44", reinterpret_cast<HANDLE>(1));
        shellFixture = true;
    }
    etm::g_settings.selectionFade = etm::SelectionFade::Replay;
    etm::g_settings.highlightOpacity = nativeHighlight ? 0 : 80;
    etm::g_settings.highlightRadius = radius;
    etm::g_settings.highlightInset = 4;
    if (presentationGate) {
        etm::g_settings.highlightColor = etm::ParseRgbColor(L"#EF15E9", CLR_INVALID);
        nextReplayGate.store(presentationGate);
    }
    SetForegroundWindow(owner);
    fade.started = GetTickCount64();
    SetTimer(owner, 2, 60, nullptr);
    UINT flags = presentationGate ? 0 : TPM_RETURNCMD;
    BOOL result = TrackPopupMenuEx(fade.menu, flags, 260, 240, owner, nullptr);
    KillTimer(owner, 2);
    Check(fade.modified && fade.sent &&
          static_cast<UINT>(result) == (presentationGate ? TRUE : child ? 42u : 44u),
          "real menu click selects the requested command");
    if (presentationGate) {
        PumpFor(5);
        Check(fade.commands == 1 && fade.notifiedCommand == 44 && !VisibleReplays(),
              "command reaches the owner while replay presentation is paused");
        SetEvent(presentationGate);
    }
    bool visible = WaitForVisibleReplay() == 1;
    Check(visible, "selection replay is visible");
    DwmFlush();
    if (visible && !nativeHighlight) CheckReplayShapePixels(radius != 0);
    PumpFor(500);
    Check(VisibleReplays() == 0, "selection replay expires after fading");
    CheckAnimationPreferences();
    fade.running = false;
    if (ownerDraw) {
        shellFixture = false;
        RemovePropW(owner, L"ImmersiveContextMenuArray_1-44");
    }
    DestroyMenu(fade.menu);
}

void CALLBACK EndReplayWarmup(HWND hwnd, UINT, UINT_PTR timer, DWORD) {
    KillTimer(hwnd, timer);
    EndMenu();
}

void CheckReplayMasks(HWND owner) {
    BOOL selection = FALSE, menu = FALSE, animation = FALSE, effects = FALSE;
    if (!ReadAnimationPreferences(selection, menu, animation) ||
        !SystemParametersInfoW(SPI_GETUIEFFECTS, 0, &effects, 0)) {
        Check(false, "replay animation preferences are readable");
        return;
    }
    if (!selection || !effects) {
        printf("Replay mask test skipped: Windows selection fade or UI effects are disabled.\n");
        return;
    }
    auto saved = etm::g_settings;
    selectionFixtureMode = true;
    RedrawWindow(owner, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    POINT cursor{};
    GetCursorPos(&cursor);
    // Prime the native menu window and theme before sending real mouse input.
    HMENU warmup = CreatePopupMenu();
    AppendMenuW(warmup, MF_STRING, 1, L"Warmup");
    SetForegroundWindow(owner);
    if (!SetTimer(owner, 3, 250, EndReplayWarmup)) {
        Check(false, "replay warmup timer started");
        DestroyMenu(warmup);
        SetCursorPos(cursor.x, cursor.y);
        etm::g_settings = saved;
        return;
    }
    TrackPopupMenuEx(warmup, TPM_RETURNCMD | TPM_NOANIMATION,
                     260, 240, owner, nullptr);
    KillTimer(owner, 3);
    DestroyMenu(warmup);
    replayPresentationGate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(replayPresentationGate != nullptr, "replay presentation gate created");
    RunReplayShapeCase(owner, 10, false, false, false, replayPresentationGate);
    etm::g_settings.highlightColor = saved.highlightColor;
    RunReplayShapeCase(owner, 0, false, false);
    RunReplayShapeCase(owner, 10, true, false);
    RunReplayShapeCase(owner, 10, false, true);
    RunReplayShapeCase(owner, 10, false, false, true);
    SetCursorPos(cursor.x, cursor.y);
    etm::g_settings = saved;
}

void RunSelectionCase(HWND owner, etm::SelectionFade choice, bool legacy,
                      bool mouse, FadeAction action, bool unload = false,
                      UINT flags = TPM_RETURNCMD, PositionStyle positionStyle = PositionStyle::None) {
    if (selectionCaseNumber > selectionLastCase) return;
    ++selectionCaseNumber;
    fade = {};
    backdropTraceCount = 0;
    fade.running = true;
    fade.mouse = mouse;
    fade.action = action;
    Check(ReadAnimationPreferences(fade.selectionFade, fade.menuFade, fade.menuAnimation),
          "desktop animation preferences are readable");
    fade.menu = CreatePopupMenu();
    fade.child = CreatePopupMenu();
    AppendMenuW(fade.child, MF_STRING, 42, L"&Child command");
    AppendMenuW(fade.menu, MF_STRING, 44, L"&Root command");
    AppendMenuW(fade.menu, MF_STRING | MF_DISABLED, 45, L"&Disabled command");
    AppendMenuW(fade.menu, MF_POPUP, reinterpret_cast<UINT_PTR>(fade.child), L"&Submenu");
    bool byPosition = positionStyle == PositionStyle::RootOnly ||
                      positionStyle == PositionStyle::Both;
    bool childByPosition = positionStyle == PositionStyle::ChildOnly ||
                           positionStyle == PositionStyle::Both;
    if (byPosition || childByPosition) {
        MENUINFO info{sizeof(info)};
        info.fMask = MIM_STYLE;
        info.dwStyle = MNS_NOTIFYBYPOS;
        Check((!byPosition || SetMenuInfo(fade.menu, &info)) &&
              (!childByPosition || SetMenuInfo(fade.child, &info)),
              "position-based notification style applied to the requested menus");
    }
    etm::g_settings.selectionFade = choice;
    printf("Selection fade: case=%u choice=%u api=%s input=%s action=%u flags=%#x positionStyle=%u%s\n",
        selectionCaseNumber - 1, static_cast<unsigned>(choice),
        legacy ? "TrackPopupMenu" : "TrackPopupMenuEx",
        mouse ? "mouse" : "keyboard", static_cast<unsigned>(action), flags,
        static_cast<unsigned>(positionStyle), unload ? " unload" : "");
    SetForegroundWindow(owner);
    fade.started = GetTickCount64();
    SetTimer(owner, 2, 60, nullptr);
    BOOL result = legacy ? TrackPopupMenu(fade.menu, flags, 260, 240, 0, owner, nullptr)
        : TrackPopupMenuEx(fade.menu, flags, 260, 240, owner, nullptr);
    ULONGLONG returned = GetTickCount64();
    KillTimer(owner, 2);
    Check(fade.modified, "real native loop uses an active modified popup");
    bool command = action == FadeAction::Root || action == FadeAction::Child;
    UINT expectedCommand = action == FadeAction::Child ? 42 : 44;
    Check(flags & TPM_RETURNCMD ?
          static_cast<UINT>(result) == (command && !byPosition ? expectedCommand : 0u) :
          (result != FALSE) == command, "native return value preserves return-command and notification contracts");
    if (action == FadeAction::Child || action == FadeAction::Submenu)
        Check(fade.childSeen, "native child popup opens before child activation or cancellation");
    if (action == FadeAction::Disabled)
        Check(fade.disabledStayedOpen, "disabled activation does not dismiss or dispatch the menu");
    Check(!etm::t_scope && !etm::g_scopes && etm::g_calls.load() == 0 &&
          !activeOwnerSubclasses && !bufferedPaintDepth,
          "native loop releases tracking, painting and subclass resources");
    bool replay = choice == etm::SelectionFade::Replay && command && mouse;
    if (!replay) PumpFor(10);
    unsigned visible = replay ? WaitForVisibleReplay() : VisibleReplays();
    Check(visible == (replay ? 1u : 0u) && fade.replayCount == (replay ? 1u : 0u),
          "only enabled mouse command activation in replay mode creates one visible replay");
    COLORREF initial[64]{}, middle[64]{};
    if (replay) {
        Check(returned - fade.activated < 300, "command returns before the replay animation duration");
        COLORREF current[64]{};
        DwmFlush();
        SampleSelectedRow(current);
        memcpy(initial, current, sizeof(initial));
        unsigned matching = 0;
        for (unsigned i = 0; i < ARRAYSIZE(current); ++i) {
            auto close = [](int a, int b) { return a > b ? a - b <= 48 : b - a <= 48; };
            if (current[i] != CLR_INVALID && fade.samples[i] != CLR_INVALID &&
                close(GetRValue(current[i]), GetRValue(fade.samples[i])) &&
                close(GetGValue(current[i]), GetGValue(fade.samples[i])) &&
                close(GetBValue(current[i]), GetBValue(fade.samples[i]))) ++matching;
        }
        Check(matching >= 48, "initial replay retains the screen-composited selected row");
        Check(GetForegroundWindow() == owner, "replay does not steal foreground activation");
        if (unload) {
            DWORD threadId = fade.replayCount ? GetWindowThreadProcessId(fade.replays[0], nullptr) : 0;
            HANDLE worker = threadId ? OpenThread(SYNCHRONIZE, FALSE, threadId) : nullptr;
            Check(worker && threadId != GetCurrentThreadId(), "replay animation runs on its own worker thread");
            Wh_ModBeforeUninit();
            Wh_ModUninit();
            Check(VisibleReplays() == 0, "unload synchronously removes an in-flight replay");
            Check(worker && WaitForSingleObject(worker, 0) == WAIT_OBJECT_0,
                  "unload synchronously joins the replay worker");
            if (worker) CloseHandle(worker);
        }
    }
    if (replay && !unload) {
        PumpFor(130);
        DwmFlush();
        SampleSelectedRow(middle);
        Check(VisibleReplays() == 1, "replay remains visible partway through its 350 ms fade");
    }
    if (command && !replay && mouse) {
        PumpFor(55);
        DwmFlush();
        COLORREF current[64]{};
        SampleSelectedRow(current);
        unsigned uncovered = 0;
        for (COLORREF pixel : current)
            if (pixel != CLR_INVALID && ColorDistance(pixel, kFadeBackground) <= 12) ++uncovered;
        if (choice == etm::SelectionFade::Instant && mouse)
            Check(uncovered >= 60, "instant dismissal exposes the owner with no native selected-row fade");
        else if (fade.selectionFade && fade.menuFade && fade.menuAnimation)
            Check(uncovered < 48, "native mode retains Windows' selected-row fade over the owner");
        else
            printf("Native selected-row fade observation skipped: desktop animation preferences disable it.\n");
    }
    PumpFor(500);
    Check(VisibleReplays() == 0 && fade.replayCount == (replay ? 1u : 0u),
          "replay expires without duplicate snapshots or lingering windows");
    CheckAnimationPreferences();
    if (replay && !unload) {
        COLORREF background[64]{};
        DwmFlush();
        SampleSelectedRow(background);
        unsigned firstDistance = 0, middleDistance = 0;
        for (unsigned i = 0; i < ARRAYSIZE(background); ++i) {
            firstDistance += ColorDistance(initial[i], background[i]);
            middleDistance += ColorDistance(middle[i], background[i]);
        }
        Check(firstDistance > 0 && middleDistance < firstDistance,
              "composited replay pixels fade toward the uncovered desktop");
    }
    unsigned expectedNotifications = command && !(flags & (TPM_RETURNCMD | TPM_NONOTIFY)) ? 1 : 0;
    Check(fade.commands == (byPosition ? 0u : expectedNotifications) &&
          fade.positionCommands == (byPosition ? expectedNotifications : 0u) &&
          fade.notificationParametersValid,
          "command dispatch occurs exactly once through the documented notification channel");
    if (expectedNotifications) {
        Check(fade.notifiedCommand == expectedCommand &&
              (!byPosition || (fade.notifiedPosition == 0 &&
               fade.notifiedMenu == (action == FadeAction::Child ? fade.child : fade.menu))),
              "command ID or menu-position notification identifies the activated row");
    }
    fade.running = false;
    DestroyMenu(fade.menu);
}

void CheckSelectionFade(HWND owner) {
    selectionFixtureMode = true;
    RedrawWindow(owner, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    HWINEVENTHOOK events = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW, nullptr,
        ReplayEvent, GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
    Check(events != nullptr, "out-of-context replay window observer installed");
    POINT cursor{};
    GetCursorPos(&cursor);
    // Warm native menu and font/theme caches before measuring owned resources.
    RunSelectionCase(owner, etm::SelectionFade::Replay, false, true, FadeAction::Root);
    DWORD gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    // A style on the child alone must not replace the root's WM_COMMAND
    // notification or change TPM_RETURNCMD. Include native as a control.
    for (auto choice : {etm::SelectionFade::Native, etm::SelectionFade::Instant,
                        etm::SelectionFade::Replay})
        for (bool legacy : {false, true})
            for (UINT flags : {0u, static_cast<UINT>(TPM_RETURNCMD)})
                RunSelectionCase(owner, choice, legacy, true, FadeAction::Child,
                                 false, flags, PositionStyle::ChildOnly);
    // Root-only styles govern child clicks too. A successful click still
    // returns zero with TPM_RETURNCMD when position notifications are enabled.
    for (auto choice : {etm::SelectionFade::Instant, etm::SelectionFade::Replay})
        for (bool legacy : {false, true})
            for (auto action : {FadeAction::Root, FadeAction::Child})
                for (UINT flags : {0u, static_cast<UINT>(TPM_RETURNCMD)})
                    RunSelectionCase(owner, choice, legacy, true, action, false,
                                     flags, PositionStyle::RootOnly);
    RunSelectionCase(owner, etm::SelectionFade::Replay, false, true, FadeAction::Child,
                     false, TPM_RETURNCMD | TPM_NONOTIFY, PositionStyle::Both);
    for (auto choice : {etm::SelectionFade::Native, etm::SelectionFade::Instant, etm::SelectionFade::Replay})
        for (bool legacy : {false, true}) {
            RunSelectionCase(owner, choice, legacy, true, FadeAction::Root);
            RunSelectionCase(owner, choice, legacy, true, FadeAction::Child);
        }
    for (auto action : {FadeAction::Cancel, FadeAction::Disabled, FadeAction::Submenu})
        RunSelectionCase(owner, etm::SelectionFade::Replay, false, true, action);
    for (bool legacy : {false, true})
        RunSelectionCase(owner, etm::SelectionFade::Replay, legacy, false, FadeAction::Child);
    // Delivery contracts are independent of which native menu entry point or
    // selected row produced the command; avoid repeating the full matrix.
    for (auto choice : {etm::SelectionFade::Instant, etm::SelectionFade::Replay})
        for (bool legacy : {false, true}) {
            RunSelectionCase(owner, choice, legacy, true, FadeAction::Root, false, 0);
            RunSelectionCase(owner, choice, legacy, true, FadeAction::Child, false, TPM_NONOTIFY);
            RunSelectionCase(owner, choice, legacy, true, FadeAction::Child, false, 0, PositionStyle::Both);
            RunSelectionCase(owner, choice, legacy, true, FadeAction::Root, false, TPM_RETURNCMD | TPM_NONOTIFY);
        }
    RunSelectionCase(owner, etm::SelectionFade::Replay, false, true, FadeAction::Root, true);
    DWORD after = 0;
    GetProcessHandleCount(GetCurrentProcess(), &after);
    Check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= gdi + 2 && after <= handles + 2,
          "repeated replay and unload do not accumulate GDI or kernel resources");
    if (events) UnhookWinEvent(events);
    SetCursorPos(cursor.x, cursor.y);
}

LRESULT CALLBACK OwnerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (selectionFixtureMode && msg == WM_ERASEBKGND) {
        HDC dc = reinterpret_cast<HDC>(wp);
        RECT client{};
        GetClientRect(hwnd, &client);
        COLORREF previous = SetDCBrushColor(dc, kFadeBackground);
        FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        SetDCBrushColor(dc, previous);
        return 1;
    }
    if (fade.running && msg == WM_COMMAND) {
        ++fade.commands;
        fade.notifiedCommand = LOWORD(wp);
        fade.notificationParametersValid &= HIWORD(wp) == 0 && lp == 0;
        return 0;
    }
    if (fade.running && msg == WM_MENUCOMMAND) {
        ++fade.positionCommands;
        fade.notifiedMenu = reinterpret_cast<HMENU>(lp);
        fade.notifiedPosition = static_cast<UINT>(wp);
        fade.notifiedCommand = GetMenuItemID(fade.notifiedMenu, fade.notifiedPosition);
        return 0;
    }
    if (fade.running && msg == WM_TIMER && wp == 2) {
        if (!processingFixtureTimer) {
            processingFixtureTimer = true;
            FadeTimer(hwnd);
            processingFixtureTimer = false;
        }
        return 0;
    }
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
        if (LOWORD(wp) == 1006 && HIWORD(wp) == CBN_SELCHANGE)
            etm::g_settings.selectionFade = static_cast<etm::SelectionFade>(
                SendMessageW(previewSelectionFade, CB_GETCURSEL, 0, 0));
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
            DrawTextW(item->hDC, previewMode ? L"Legacy RGB icon" : L"Shell DrawTextW / \u81ea\u7ed8", -1, &bounds, DT_SINGLELINE | DT_VCENTER);
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
                    CheckHighlightGeometry(hwnd);
                    if (!highlightGeometryOnly) CheckAlpha(hwnd);
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

void CheckHighlightGeometry(HWND) {
    auto scope = etm::t_scope;
    etm::Popup* popup = nullptr;
    if (scope) for (unsigned i = 0; i < scope->count; ++i)
        if (scope->popups[i].active) { popup = &scope->popups[i]; break; }
    if (!popup) {
        Check(false, "highlight geometry test has an active menu");
        return;
    }
    UINT dpi = GetDpiForWindow(popup->hwnd);
    if (!dpi) dpi = 96;
    int width = MulDiv(160, dpi, 96), height = MulDiv(40, dpi, 96);
    if (width <= 0 || height <= 0) {
        Check(false, "highlight geometry test has valid dimensions");
        return;
    }
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
    DWORD* pixels = nullptr;
    etm::IconSurface image;
    image.dc = CreateCompatibleDC(nullptr);
    image.bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS,
        reinterpret_cast<void**>(&pixels), nullptr, 0);
    if (image.dc && image.bitmap) image.old = SelectObject(image.dc, image.bitmap);
    if (!pixels || !image.old || image.old == HGDI_ERROR) {
        Check(false, "highlight geometry test has a drawing surface");
        return;
    }
    int savedRadius = etm::g_settings.highlightRadius;
    int savedInset = etm::g_settings.highlightInset;
    int savedOpacity = etm::g_settings.highlightOpacity;
    etm::g_settings.highlightOpacity = 80;
    RECT row{0, 0, width, height};
    auto draw = [&](int radius, int inset) {
        etm::g_settings.highlightRadius = radius;
        etm::g_settings.highlightInset = inset;
        memset(pixels, 0, static_cast<size_t>(width) * height * sizeof(DWORD));
        bool rendered = etm::DrawHighlight(*popup, image.dc, &row, nullptr);
        GdiFlush();
        return rendered;
    };
    auto alpha = [&](int x, int y) { return pixels[y * width + x] >> 24; };
    Check(draw(4, 0), "default highlight geometry renders");
    unsigned center = alpha(width / 2, height / 2);
    Check(center > 0 && (MulDiv(4, dpi, 96) > 0
          ? alpha(0, 0) < center : alpha(0, 0) == center),
          "default highlight retains rounded corners and a visible center");
    Check(draw(0, 0), "square highlight renders");
    center = alpha(width / 2, height / 2);
    Check(center > 0 && alpha(0, 0) == center && alpha(width - 1, height - 1) == center,
          "zero radius produces a full-opacity square highlight");
    Check(draw(0, 2), "inset highlight renders");
    int inset = MulDiv(2, dpi, 96);
    if (inset > 0) {
        Check(alpha(inset - 1, height / 2) == 0 && alpha(inset, height / 2) > 0 &&
              alpha(width - inset, height / 2) == 0 && alpha(width - inset - 1, height / 2) > 0 &&
              alpha(width / 2, inset - 1) == 0 && alpha(width / 2, inset) > 0 &&
              alpha(width / 2, height - inset) == 0 && alpha(width / 2, height - inset - 1) > 0,
              "configured inset leaves equal transparent margins on all sides");
    }
    Check(draw(8, 2), "rounded inset highlight renders");
    center = alpha(width / 2, height / 2);
    Check(center > 0 && alpha(inset, inset) < center &&
          alpha(inset, height / 2) == center && alpha(width / 2, inset) == center,
          "configured radius rounds the inset corners without shrinking straight edges");
    Check(draw(0x7fffffff, 0), "oversized highlight radius renders");
    center = alpha(width / 2, height / 2);
    Check(center > 0 && alpha(0, 0) < center,
          "oversized radius is limited to the available row");
    Check(draw(4, 0x7fffffff), "oversized highlight inset renders");
    bool transparent = true;
    for (int i = 0; i < width * height; ++i)
        transparent = transparent && (pixels[i] >> 24) == 0;
    Check(transparent, "an inset consuming the row produces no highlight");
    auto centerPixel = [&]() -> DWORD& { return pixels[(height / 2) * width + width / 2]; };
    COLORREF savedColor = etm::g_settings.highlightColor;
    bool savedDark = popup->dark;
    popup->dark = false;
    etm::g_settings.highlightColor = etm::ParseRgbColor(L"auto", CLR_INVALID);
    Check(draw(4, 0) && centerPixel() == 0xcc005498,
          "auto keeps the default light highlight");
    popup->dark = true;
    etm::g_settings.highlightColor = etm::ParseRgbColor(L"#00Z123", CLR_INVALID);
    Check(draw(4, 0) && centerPixel() == 0xcc669acc,
          "invalid hex falls back to the default dark highlight");
    etm::g_settings.highlightColor = etm::ParseRgbColor(L"#fF6a00", CLR_INVALID);
    Check(draw(4, 0) && centerPixel() == 0xcccc5500,
          "custom RGB is drawn with the configured opacity");
    etm::g_settings.highlightColor = etm::ParseRgbColor(L"#000000", CLR_INVALID);
    Check(draw(4, 0) && centerPixel() == 0xcc000000,
          "black is a valid custom highlight, not the default");

    // Exercise the selected-item paint used by native menus during an offscreen
    // draw, without depending on Windows choosing to animate this opening.
    UINT itemId = GetMenuItemID(popup->menu, 0);
    DRAWITEMSTRUCT item{ODT_MENU, 0, itemId, ODA_DRAWENTIRE, ODS_SELECTED,
                        reinterpret_cast<HWND>(popup->menu), image.dc, row};
    const etm::UahPaintTarget paint{popup->hwnd, image.dc, &item};
    const auto* previousPaint = scope->uahPaint;
    scope->uahPaint = &paint;
    auto savedFade = etm::g_settings.selectionFade;
    etm::g_settings.selectionFade = etm::SelectionFade::Replay;
    RECT hot{3, 2, width - 5, height - 3};
    RECT screenRow = row;
    RECT expected = hot;
    OffsetRect(&screenRow, 250, 300);
    OffsetRect(&expected, 250, 300);
    RECT resolved{};
    HRESULT result = DrawThemeBackground(popup->palette, image.dc,
        etm::kPopupItemFocusable, MPI_HOT, &hot, nullptr);
    Check(SUCCEEDED(result) &&
          etm::ResolveReplayHighlight(*popup, screenRow, itemId, resolved) &&
          EqualRect(&resolved, &expected),
          "native offscreen highlight resolves to the item-relative replay crop");
    popup->replayHighlightSpace = etm::ReplayHighlightSpace::None;
    etm::g_settings.selectionFade = savedFade;

    bool savedBorder = etm::g_settings.hideMenuBorder;
    for (bool hide : {false, true}) {
        etm::g_settings.hideMenuBorder = hide;
        GdiFlush();
        centerPixel() = 0x7f112233;
        bool cleared = etm::ClearBackground(popup->palette, image.dc,
            MENU_POPUPBORDERS, 0, &row, nullptr);
        GdiFlush();
        Check(cleared == hide && centerPixel() == (hide ? 0 : 0x7f112233),
              "theme border clearing respects the hide border setting on a scoped DC");
    }
    etm::g_settings.hideMenuBorder = savedBorder;
    scope->uahPaint = previousPaint;
    popup->dark = savedDark;
    etm::g_settings.highlightColor = savedColor;
    etm::g_settings.highlightRadius = savedRadius;
    etm::g_settings.highlightInset = savedInset;
    etm::g_settings.highlightOpacity = savedOpacity;
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
        Check(etm::PrepareIcon(iconDC, 0, 0, 24, 24, normalized),
              "legacy RGB icon normalized on an owned copy");
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

bool scrollMenuSampled;

void CALLBACK SampleScrollMenu(HWND owner, UINT, UINT_PTR timer, DWORD) {
    KillTimer(owner, timer);
    scrollMenuSampled = true;
    HWND popup = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND hwnd, LPARAM param) -> BOOL {
        if (!IsWindowVisible(hwnd) || !etm::IsClassicPopup(hwnd))
            return TRUE;
        *reinterpret_cast<HWND*>(param) = hwnd;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&popup));
    MENUBARINFO bar{sizeof(bar)};
    RECT row{};
    HDC screen = GetDC(nullptr);
    bool visible = screen && popup && GetMenuBarInfo(popup, OBJID_CLIENT, 0, &bar) &&
                   GetMenuItemRect(owner, bar.hMenu, 0, &row);
    Check(visible, "the native scrollbar context menu opens");
    if (visible) {
        DwmFlush();
        int padding = MulDiv(10, GetDpiForWindow(popup), 96);
        int middle = (row.top + row.bottom) / 2;
        COLORREF gutter = GetPixel(screen, row.left + padding, middle);
        COLORREF body = GetPixel(screen, row.right - padding, middle);
        auto tinted = [](COLORREF color) {
            return color != CLR_INVALID &&
                GetRValue(color) > GetGValue(color) + 40 &&
                GetBValue(color) > GetGValue(color) + 40;
        };
        Check(tinted(gutter) && tinted(body),
              "the scrollbar menu's opening frame has no opaque gutter");
        RECT bounds{};
        Check(GetWindowRect(popup, &bounds) &&
              (ColorDistance(GetPixel(screen, bounds.left, middle), gutter) < 55) ==
                  etm::g_settings.hideMenuBorder,
              "the scrollbar menu's outline follows the hide border setting");
        bool glyph = false;
        for (int y = row.top + 2; y < row.bottom - 2 && !glyph; y += 2) {
            for (int x = row.left + padding; x < row.right - padding; x += 2) {
                COLORREF color = GetPixel(screen, x, y);
                if (color != CLR_INVALID && GetRValue(color) < 75 &&
                    GetGValue(color) < 75 && GetBValue(color) < 75) {
                    glyph = true;
                    break;
                }
            }
        }
        Check(glyph, "the scrollbar menu's item text is visible before any hover");
    }
    if (screen) ReleaseDC(nullptr, screen);
    EndMenu();
}

void CheckScrollMenu(HWND owner) {
    HWND scroll = CreateWindowW(WC_SCROLLBAR, nullptr, WS_CHILD | WS_VISIBLE | SBS_VERT,
        480, 35, 24, 270, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!scroll) {
        Check(false, "native scrollbar fixture created");
        return;
    }
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS, 0, 100, 45, 30};
    SetScrollInfo(scroll, SB_CTL, &info, TRUE);
    etm::g_settings.theme = 1;
    etm::g_settings.lightTint = RGB(220, 10, 170);
    etm::g_settings.tintOpacity = 85;
    SetForegroundWindow(owner);
    UpdateWindow(owner);
    POINT cursor{}, away{10, 10}, anchor{12, 95};
    GetCursorPos(&cursor);
    ClientToScreen(owner, &away);
    ClientToScreen(scroll, &anchor);
    SetCursorPos(away.x, away.y);
    for (bool hide : {false, true}) {
        etm::g_settings.hideMenuBorder = hide;
        scrollMenuSampled = false;
        UINT_PTR timer = SetTimer(owner, 3, 650, SampleScrollMenu);
        if (!timer) {
            Check(false, "scrollbar menu sampling timer created");
            break;
        }
        // Let the actual scrollbar open its menu with the system animation.
        SendMessageW(scroll, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(scroll),
                     MAKELPARAM(anchor.x, anchor.y));
        KillTimer(owner, timer);
        Check(scrollMenuSampled, "scrollbar menu stayed open for pixel sampling");
    }
    SetCursorPos(cursor.x, cursor.y);
    DestroyWindow(scroll);
}

bool windowMenuPaintObserved;
bool windowMenuTinted;
bool windowMenuSelect;
RECT windowMenuItem;
POINT windowMenuClick;
unsigned windowMenuStage, systemCommands, otherCommands;
WPARAM systemCommand;
LPARAM systemCommandPoint;
unsigned windowMenuEdgeContrast;
bool windowMenuHighlight;

LRESULT CALLBACK ObserveWindowMenuItem(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto& message = *reinterpret_cast<const CWPSTRUCT*>(lp);
        if (message.message == etm::kUahDrawMenuItem && message.lParam &&
            etm::IsClassicPopup(message.hwnd)) {
            const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(message.lParam);
            if (item.CtlType == ODT_MENU && item.itemID == SC_MAXIMIZE)
                windowMenuItem = item.rcItem;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void CALLBACK SampleWindowMenu(HWND owner, UINT, UINT_PTR timer, DWORD) {
    if (windowMenuStage >= 2) {
        KillTimer(owner, timer);
        EndMenu();
        return;
    }
    MENUBARINFO bar{sizeof(bar)};
    HWND popup = nullptr;
    if (GetMenuBarInfo(owner, OBJID_SYSMENU, 0, &bar)) {
        struct Search {
            HMENU menu;
            HWND& popup;
        } search{GetSubMenu(bar.hMenu, 0), popup};
        EnumThreadWindows(GetCurrentThreadId(), [](HWND hwnd, LPARAM data) -> BOOL {
            auto& search = *reinterpret_cast<Search*>(data);
            if (IsWindowVisible(hwnd) && etm::IsClassicPopup(hwnd) && search.menu &&
                reinterpret_cast<HMENU>(SendMessageW(hwnd, etm::kMenuGetHandle, 0, 0)) == search.menu) {
                search.popup = hwnd;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&search));
    }
    RECT bounds{};
    if (popup && GetWindowRect(popup, &bounds)) {
        DwmFlush();
        HDC screen = GetDC(nullptr);
        int midY = bounds.top + (bounds.bottom - bounds.top) / 4;
        COLORREF edge = GetPixel(screen, bounds.left, midY);
        COLORREF body = GetPixel(screen, bounds.left + 8, midY);
        windowMenuEdgeContrast = ColorDistance(edge, body);
        for (int row : {1, 2, 3}) {
            COLORREF color = GetPixel(screen, bounds.right - 16,
                bounds.top + (bounds.bottom - bounds.top) * row / 4);
            windowMenuPaintObserved |= color != CLR_INVALID;
            windowMenuTinted |= color != CLR_INVALID &&
                GetRValue(color) > GetGValue(color) + 40 &&
                GetBValue(color) > GetGValue(color) + 40;
            printf("Title bar menu sample RGB=(%u,%u,%u)\n",
                   GetRValue(color), GetGValue(color), GetBValue(color));
        }
        if (windowMenuStage == 1) {
            COLORREF hot = GetPixel(screen, fade.row.right -
                (fade.row.right - fade.row.left) / 5, windowMenuClick.y);
            windowMenuHighlight = GetBValue(hot) > GetRValue(hot) + 60 &&
                                  GetGValue(hot) > GetRValue(hot) + 45;
        }
        ReleaseDC(nullptr, screen);
        if (windowMenuSelect && !IsRectEmpty(&windowMenuItem)) {
            if (windowMenuStage++ == 0) {
                fade.row = windowMenuItem;
                MapWindowPoints(popup, nullptr, reinterpret_cast<POINT*>(&fade.row), 2);
                windowMenuClick = {(fade.row.left + fade.row.right) / 2,
                                   (fade.row.top + fade.row.bottom) / 2};
                SetCursorPos(windowMenuClick.x, windowMenuClick.y);
                INPUT move[2]{};
                move[0].type = move[1].type = INPUT_MOUSE;
                move[0].mi.dwFlags = move[1].mi.dwFlags =
                    MOUSEEVENTF_MOVE | MOUSEEVENTF_MOVE_NOCOALESCE;
                move[0].mi.dx = 1;
                move[1].mi.dx = -1;
                SendInput(2, move, sizeof(INPUT));
            } else {
                INPUT click[2]{};
                click[0].type = click[1].type = INPUT_MOUSE;
                click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
                Check(SendInput(2, click, sizeof(INPUT)) == 2,
                      "clicked the native system menu's Maximize row");
            }
            return;
        }
    }
    KillTimer(owner, timer);
    EndMenu();
}

void CheckWindowMenu(HWND owner, bool caption, bool styled, bool select = false) {
    windowMenuPaintObserved = windowMenuTinted = windowMenuHighlight = false;
    windowMenuSelect = select;
    windowMenuItem = {};
    windowMenuStage = systemCommands = otherCommands = 0;
    HHOOK observer = nullptr;
    SetForegroundWindow(owner);
    if (select) {
        HWND foreground = GetForegroundWindow();
        if (foreground != owner) {
            DWORD thread = GetWindowThreadProcessId(foreground, nullptr);
            if (thread && AttachThreadInput(GetCurrentThreadId(), thread, TRUE)) {
                BringWindowToTop(owner);
                SetForegroundWindow(owner);
                AttachThreadInput(GetCurrentThreadId(), thread, FALSE);
            }
        }
        RECT window{};
        GetWindowRect(owner, &window);
        SetCursorPos(window.left + 4, window.top + 4);
        observer = SetWindowsHookExW(WH_CALLWNDPROC, ObserveWindowMenuItem,
                                     nullptr, GetCurrentThreadId());
        Check(observer && GetForegroundWindow() == owner,
              "interactive system menu fixture is foreground and observed");
    }
    if (!SetTimer(owner, 4, 450, SampleWindowMenu)) {
        Check(false, "title bar probe timer armed");
        if (observer) UnhookWindowsHookEx(observer);
        return;
    }
    if (caption) {
        RECT bounds{};
        GetWindowRect(owner, &bounds);
        LPARAM point = MAKELPARAM((bounds.left + bounds.right) / 2,
            bounds.top + GetSystemMetrics(SM_CYFRAME) +
            GetSystemMetrics(SM_CXPADDEDBORDER) + GetSystemMetrics(SM_CYCAPTION) / 2);
        SendMessageW(owner, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(owner), point);
    } else {
        SendMessageW(owner, WM_SYSCOMMAND, SC_KEYMENU, L' ');
    }
    KillTimer(owner, 4);
    if (observer) UnhookWindowsHookEx(observer);
    Check(windowMenuPaintObserved && windowMenuTinted == styled,
          styled ? "native window menu displays the selected tint"
                 : "native window menu remains usable and untinted");
    if (styled)
        Check((windowMenuEdgeContrast < 55) == etm::g_settings.hideMenuBorder,
              "the system menu's visible outline follows the hide border setting");
    if (!select) return;
    PumpFor(5);
    Check(windowMenuHighlight, "the hovered system row uses the custom color");
    Check(systemCommands == 1 && !otherCommands && systemCommand == SC_MAXIMIZE &&
          systemCommandPoint == MAKELPARAM(windowMenuClick.x, windowMenuClick.y),
          "selection delivers one WM_SYSCOMMAND with the native ID and click point");
    BOOL selection = FALSE, effects = FALSE;
    SystemParametersInfoW(SPI_GETSELECTIONFADE, 0, &selection, 0);
    SystemParametersInfoW(SPI_GETUIEFFECTS, 0, &effects, 0);
    bool replay = selection && effects &&
                  etm::g_settings.selectionFade == etm::SelectionFade::Replay;
    unsigned overlays = replay ? WaitForVisibleReplay() : VisibleReplays();
    Check(overlays == (replay ? 1u : 0u),
          "only Recreate shows a system menu replay overlay");
    if (replay && overlays) {
        DwmFlush();
        CheckReplayShapePixels(true);
    }
    PumpFor(500);
    Check(!VisibleReplays(), "system menu replay expires without lingering windows");
}

LRESULT CALLBACK WindowMenuFixtureProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (windowMenuSelect && message == WM_SYSCOMMAND && wp == SC_MAXIMIZE) {
        ++systemCommands;
        systemCommand = wp;
        systemCommandPoint = lp;
        return 0;  // Record the actual command without covering the replay.
    }
    if (windowMenuSelect && (message == WM_COMMAND || message == WM_MENUCOMMAND))
        ++otherCommands;
    // Invoke the native path without going through the old DefWindowProcW hook.
    if (message == WM_SYSCOMMAND || message == WM_CONTEXTMENU)
        return DefWindowProcA(hwnd, message, wp, lp);
    return OwnerProc(hwnd, message, wp, lp);
}

int RunWindowMenuProbe() {
    selectionFixtureMode = true;
    etm::g_settings.styleWindowMenu = true;
    etm::g_settings.theme = 1;
    etm::g_settings.lightTint = RGB(220, 10, 170);
    etm::g_settings.tintOpacity = 85;
    etm::g_settings.highlightColor = RGB(25, 170, 235);
    etm::g_settings.highlightOpacity = 85;
    etm::g_settings.highlightRadius = 10;
    etm::g_settings.highlightInset = 4;
    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowMenuFixtureProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CabinetWClass";
    if (!RegisterClassW(&wc))
        return 1;
    auto create = [&] {
        return CreateWindowExW(0, wc.lpszClassName, L"Isolated title bar menu",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, 170, 180, 620, 380,
            nullptr, nullptr, wc.hInstance, nullptr);
    };
    HWND existing = create();
    if (!existing)
        return 1;
    CheckWindowMenu(existing, false, false);
    Check(WindhawkUtils::SetFunctionHook(CreateWindowExW, etm::CreateWindowExWHook,
                                         &etm::g_createWindowEx) &&
          MH_EnableHook(reinterpret_cast<void*>(CreateWindowExW)) == MH_OK,
          "future Explorer windows can receive the owner observer");
    Wh_ModAfterInit();
    CheckWindowMenu(existing, false, true);
    HWND created = create();
    if (created)
        CheckWindowMenu(created, true, true);
    else
        Check(false, "new Explorer fixture window created");
    POINT cursor{};
    GetCursorPos(&cursor);
    etm::g_settings.hideMenuBorder = true;
    const auto modes = {etm::SelectionFade::Native, etm::SelectionFade::Instant,
                        etm::SelectionFade::Replay};
    for (auto mode : modes) {
        etm::g_settings.selectionFade = mode;
        CheckWindowMenu(existing, false, true, true);
    }
    SetCursorPos(cursor.x, cursor.y);
    Wh_ModBeforeUninit();
    CheckWindowMenu(existing, false, false);
    if (created) DestroyWindow(created);
    DestroyWindow(existing);
    return failures ? 1 : 0;
}

void CheckInfoTipFade() {
    ACTCTXW request{sizeof(request)};
    request.lpSource = L"tools/tooltip-controls.manifest";
    HANDLE context = CreateActCtxW(&request);
    ULONG_PTR cookie = 0;
    bool activated = context != INVALID_HANDLE_VALUE && ActivateActCtx(context, &cookie);
    Check(activated, "themed common controls available for the InfoTip fixture");
    if (!activated) {
        if (context != INVALID_HANDLE_VALUE) ReleaseActCtx(context);
        return;
    }

    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    HINSTANCE instance = GetModuleHandleW(nullptr);
    auto create = [&](PCWSTR name, HWND parent, int width, int height) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = instance;
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
        wc.lpszClassName = name;
        if (!RegisterClassW(&wc)) return HWND{};
        return CreateWindowW(name, L"InfoTip fade fixture",
            WS_VISIBLE | (parent ? WS_CHILD : WS_OVERLAPPEDWINDOW),
            parent ? 0 : 200, parent ? 0 : 210, width, height, parent, nullptr, instance, nullptr);
    };
    HWND owner = create(L"CabinetWClass", nullptr, 660, 460);
    HWND shell = owner ? create(L"SHELLDLL_DefView", owner, 620, 410) : nullptr;
    HWND view = shell ? create(L"DirectUIHWND", shell, 620, 410) : nullptr;
    HWND tip = view ? CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0,
        owner, nullptr, instance, nullptr) : nullptr;
    Check(tip && GetWindowTheme(tip), "a real themed tooltip was created");
    if (tip && GetWindowTheme(tip)) {
        etm::g_settings.styleFileInfoTips = true;
        etm::g_settings.theme = 1;
        etm::g_settings.hideMenuBorder = true;
        etm::g_settings.tintOpacity = 40;
        etm::g_settings.lightTint = RGB(250, 227, 246);
        etm::g_detachInfoTipMessage = RegisterWindowMessageW(L"ETM.Fixture.DetachInfoTip");
        etm::AttachInfoTip(tip);

        wchar_t label[] = L"Type: C++ source\r\nSize: 130 KB\r\nModified: 2026/11/21";
        TOOLINFOW tool{sizeof(tool)};
        tool.uFlags = TTF_TRACK | TTF_ABSOLUTE;
        tool.hwnd = view;
        tool.lpszText = label;
        SendMessageW(tip, TTM_SETMAXTIPWIDTH, 0, 400);
        Check(SendMessageW(tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool)),
              "registered a file-list tooltip tool");
        UpdateWindow(owner);
        POINT position{200, 150};
        ClientToScreen(owner, &position);
        SendMessageW(tip, TTM_TRACKPOSITION, 0, MAKELPARAM(position.x, position.y));
        SendMessageW(tip, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&tool));
        PumpFor(380);
        RECT bounds{};
        GetWindowRect(tip, &bounds);
        auto sample = [&] {
            DwmFlush();
            HDC dc = GetDC(nullptr);
            COLORREF pixel = GetPixel(dc, bounds.right - 12, (bounds.top + bounds.bottom) / 2);
            ReleaseDC(nullptr, dc);
            return pixel;
        };
        COLORREF visible = sample();
        Check(IsWindowVisible(tip) && visible != CLR_INVALID && GetGValue(visible) > 150 &&
              GetRValue(visible) > GetGValue(visible) + 2,
              "the visible InfoTip has a light acrylic tint");
        SendMessageW(tip, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&tool));
        BOOL animation = FALSE, fadeEnabled = FALSE;
        SystemParametersInfoW(SPI_GETTOOLTIPANIMATION, 0, &animation, 0);
        SystemParametersInfoW(SPI_GETTOOLTIPFADE, 0, &fadeEnabled, 0);
        if (animation && fadeEnabled) {
            BYTE alpha = 255;
            ULONGLONG deadline = GetTickCount64() + 350;
            while (IsWindowVisible(tip) && alpha == 255 && GetTickCount64() < deadline) {
                PumpFor(5);
                GetLayeredWindowAttributes(tip, nullptr, &alpha, nullptr);
            }
            TOOLINFOW current{sizeof(current)};
            Check(IsWindowVisible(tip) && alpha > 0 && alpha < 255 &&
                  !SendMessageW(tip, TTM_GETCURRENTTOOLW, 0, reinterpret_cast<LPARAM>(&current)),
                  "the native fade is visible after the current tool is cleared");
            // Repaint while the old frame is still fading, as when moving
            // across the file list before the next tooltip has opened.
            RedrawWindow(tip, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
            COLORREF fading = sample();
            Check(IsWindowVisible(tip) && fading != CLR_INVALID &&
                  GetGValue(fading) + 8 >= GetGValue(visible),
                  "repainting during the native fade does not darken the InfoTip");
        } else {
            puts("SKIP: tooltip fade disabled by Windows");
        }
        PumpFor(420);
        Check(!IsWindowVisible(tip), "Windows finishes dismissing the InfoTip");
    }
    if (tip) DestroyWindow(tip);
    if (owner) DestroyWindow(owner);
    DeactivateActCtx(0, cookie);
    ReleaseActCtx(context);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    bool preview = argc > 1 && strcmp(argv[1], "--preview") == 0;
    bool selectionFade = argc > 1 && strcmp(argv[1], "--selection-fade") == 0;
    if (selectionFade && argc > 2) {
        // Preserve all predecessor cases: an isolated replay can hide this bug.
        char* end = nullptr;
        const char* value = argv[2];
        if (strncmp(value, "--through=", 10) == 0) value += 10;
        else value = "";
        unsigned long last = strtoul(value, &end, 10);
        if (argc != 3 || !*value || *end || *value == '-') {
            fprintf(stderr, "Usage: menu-smoke --selection-fade [--through=CASE]\n");
            return 2;
        }
        selectionLastCase = static_cast<unsigned>(last);
    }
    bool probeFrame = argc > 1 && strcmp(argv[1], "--probe-frame") == 0;
    bool probeBorder = argc > 1 && strcmp(argv[1], "--probe-border") == 0;
    bool probeUnprotectedBorder = argc > 1 && strcmp(argv[1], "--probe-unprotected-border") == 0;
    bool materialsOnly = argc > 1 && strcmp(argv[1], "--materials-only") == 0;
    const auto materials = {etm::Material::Native, etm::Material::Solid,
        etm::Material::Transparent, etm::Material::Acrylic,
        etm::Material::ModernAcrylic, etm::Material::SystemAcrylic,
        etm::Material::Mica, etm::Material::MicaAlt};
    bool probeAcrylic = argc > 1 && strcmp(argv[1], "--probe-acrylic") == 0;
    bool probeHighlight = argc > 1 && strcmp(argv[1], "--probe-highlight") == 0;
    bool probeReplayMask = argc > 1 && strcmp(argv[1], "--probe-replay-mask") == 0;
    bool probeWindowMenu = argc > 1 && strcmp(argv[1], "--probe-window-menu") == 0;
    bool probeScrollMenu = argc > 1 && strcmp(argv[1], "--probe-scroll-menu") == 0;
    bool probeInfoTipFade = argc > 1 && strcmp(argv[1], "--probe-infotip-fade") == 0;
    highlightGeometryOnly = probeHighlight;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    previewMode = preview;
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
    etm::g_settings = {};
    etm::g_settings.roundedCorners = true;
    etm::g_settings.ctrlBypass = preview;
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
    if (probeInfoTipFade) {
        CheckInfoTipFade();
        MH_DisableHook(MH_ALL_HOOKS);
        Wh_ModUninit();
        MH_Uninitialize();
        printf("Failures: %d\n", failures);
        return failures ? 1 : 0;
    }
    if (probeWindowMenu) {
        int result = RunWindowMenuProbe();
        MH_DisableHook(MH_ALL_HOOKS);
        Wh_ModUninit();
        MH_Uninitialize();
        printf("Failures: %d\n", failures);
        return result;
    }
    if (!selectionFade && !probeHighlight && !probeReplayMask && !probeScrollMenu)
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
    if (!selectionFade && !probeHighlight && !probeReplayMask && !probeScrollMenu)
        CheckLateCtrlBypass(owner);

    HMENU child = CreatePopupMenu();
    AppendMenuW(child, MF_STRING, 42, L"&Test command\tEnter");
    AppendMenuW(child, MF_STRING | MF_DISABLED, 43, L"Disabled command");
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(child), L"&Submenu");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | MF_CHECKED, 44, L"Checked command");
    Check(etm::IsSupportedMenu(menu, owner), "standard menu recognized");
    if (probeScrollMenu) {
        CheckScrollMenu(owner);
    } else if (selectionFade) {
        CheckSelectionFade(owner);
    } else if (preview) {
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
        previewSelectionFade = CreateWindowW(L"COMBOBOX", L"Selection fade",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 460, 50, 180, 140,
            owner, reinterpret_cast<HMENU>(1006), wc.hInstance, nullptr);
        for (PCWSTR label : {L"Native selection fade", L"Instant dismissal", L"Replay selection fade"})
            SendMessageW(previewSelectionFade, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        SendMessageW(previewSelectionFade, CB_SETCURSEL, 2, 0);
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
    } else if (probeReplayMask) {
        CheckReplayMasks(owner);
    } else if (probeHighlight) {
        // Exercise these settings on a live menu as well as the owned DIB.
        etm::g_settings.highlightRadius = 0;
        etm::g_settings.highlightInset = 2;
        ShowTestMenu(owner, menu, true, true);
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
    Wh_ModBeforeUninit();
    if (replayPresentationGate) CloseHandle(replayPresentationGate);
    DestroyMenu(menu);
    DestroyWindow(owner);
    MH_DisableHook(MH_ALL_HOOKS);
    Wh_ModUninit();
    MH_Uninitialize();
    printf("Failures: %d\n", failures);
    return failures ? 1 : 0;
}
