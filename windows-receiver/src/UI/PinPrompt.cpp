// =============================================================================
// PinPrompt.cpp — Modal PIN entry dialog (plain Win32, no resource file)
// =============================================================================

#include "UI/PinPrompt.h"

namespace {

constexpr const wchar_t* kClassName = L"ExternalDisplayPinPrompt";
constexpr int kIdEdit = 100;
constexpr UINT_PTR kValidityTimer = 1;
constexpr size_t kPinLength = 6;

struct PromptState {
    HWND edit = nullptr;
    std::function<bool()> stillValid;
    bool done = false;
    bool accepted = false;
    std::string pin;
};

// Closes the dialog. Returns false (and keeps it open) if an accepted PIN is malformed.
bool Finish(HWND hwnd, PromptState* st, bool accept) {
    if (accept) {
        wchar_t buf[16] = {};
        GetWindowTextW(st->edit, buf, 16);
        std::string pin;
        for (const wchar_t* p = buf; *p; ++p) {
            if (*p >= L'0' && *p <= L'9') pin.push_back(static_cast<char>(*p));
        }
        if (pin.size() != kPinLength) {
            MessageBeep(MB_ICONWARNING);
            SetFocus(st->edit);
            return false;
        }
        st->pin = pin;
    }
    st->accepted = accept;
    st->done = true;
    KillTimer(hwnd, kValidityTimer);
    DestroyWindow(hwnd);
    return true;
}

LRESULT CALLBACK PromptProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* st = reinterpret_cast<PromptState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_COMMAND:
        if (st && LOWORD(wp) == IDOK)     { Finish(hwnd, st, true);  return 0; }
        if (st && LOWORD(wp) == IDCANCEL) { Finish(hwnd, st, false); return 0; }
        break;
    case WM_TIMER:
        if (st && wp == kValidityTimer && st->stillValid && !st->stillValid()) {
            Finish(hwnd, st, false);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (st) { Finish(hwnd, st, false); return 0; }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterPromptClass(HINSTANCE hInst) {
    WNDCLASSEXW existing = { sizeof(existing) };
    if (GetClassInfoExW(hInst, kClassName, &existing)) return;

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = PromptProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
}

HWND CreateChild(const wchar_t* cls, const wchar_t* text, DWORD style,
                 int x, int y, int w, int h, HWND parent, int id, HFONT font) {
    HWND child = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                                 x, y, w, h, parent,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 GetModuleHandleW(nullptr), nullptr);
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return child;
}

} // namespace

bool PromptForPIN(HWND owner, std::string& outPIN,
                  const std::function<bool()>& stillValid) {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    RegisterPromptClass(hInst);

    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    HFONT uiFont = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW bigLf = ncm.lfMessageFont;
    bigLf.lfHeight *= 2;
    bigLf.lfWeight = FW_BOLD;
    HFONT pinFont = CreateFontIndirectW(&bigLf);

    // Client area 360×170, centered on the owner's monitor
    const DWORD style   = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_TOPMOST;
    RECT rc = { 0, 0, 360, 170 };
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;

    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTOPRIMARY), &mi);
    int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - winW) / 2;
    int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - winH) / 2;

    PromptState st;
    st.stillValid = stillValid;

    HWND dlg = CreateWindowExW(exStyle, kClassName, L"Pair with Mac", style,
                               x, y, winW, winH, owner, nullptr, hInst, &st);
    if (!dlg) {
        DeleteObject(uiFont);
        DeleteObject(pinFont);
        return false;
    }

    CreateChild(L"STATIC",
                L"Enter the 6-digit PIN shown in the Mac app\nto pair this receiver.",
                SS_LEFT, 20, 14, 320, 40, dlg, -1, uiFont);
    st.edit = CreateChild(L"EDIT", L"",
                          WS_BORDER | WS_TABSTOP | ES_CENTER | ES_NUMBER,
                          20, 62, 320, 44, dlg, kIdEdit, pinFont);
    SendMessageW(st.edit, EM_LIMITTEXT, kPinLength, 0);
    CreateChild(L"BUTTON", L"Pair", WS_TABSTOP | BS_DEFPUSHBUTTON,
                150, 124, 90, 30, dlg, IDOK, uiFont);
    CreateChild(L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON,
                250, 124, 90, 30, dlg, IDCANCEL, uiFont);

    SetTimer(dlg, kValidityTimer, 250, nullptr);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    SetFocus(st.edit);

    // Local modal loop (owner disabled, like DialogBox)
    EnableWindow(owner, FALSE);
    MSG msg;
    while (!st.done) {
        BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r <= 0) {
            if (r == 0) PostQuitMessage(static_cast<int>(msg.wParam));  // let the app loop see it
            break;
        }
        bool forDialog = (msg.hwnd == dlg || IsChild(dlg, msg.hwnd));
        if (forDialog && msg.message == WM_KEYDOWN &&
            (msg.wParam == VK_RETURN || msg.wParam == VK_ESCAPE)) {
            Finish(dlg, &st, msg.wParam == VK_RETURN);
            continue;
        }
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (!st.done && IsWindow(dlg)) DestroyWindow(dlg);

    DeleteObject(uiFont);
    DeleteObject(pinFont);

    if (st.accepted) outPIN = st.pin;
    return st.accepted;
}
