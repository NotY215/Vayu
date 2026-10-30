// Vayu-IDE — Win32 dark-theme editor with output pane (VS Code–style).
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commdlg.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <process.h>
#include <memory>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

static HWND gEdit, gOut, gStatus, gSplitLabel;
static std::wstring gPath;
static bool gDirty = false;
static const wchar_t* CLS = L"VayuIDEWnd";
static HBRUSH gBgBrush, gEditBrush, gOutBrush;
static HFONT gFont, gUiFont;

// Win11-ish dark palette
static const COLORREF COL_BG     = RGB(32, 32, 32);
static const COLORREF COL_EDIT   = RGB(30, 30, 30);
static const COLORREF COL_OUT    = RGB(24, 24, 24);
static const COLORREF COL_TEXT   = RGB(204, 204, 204);
static const COLORREF COL_STATUS = RGB(0, 122, 204);
static const COLORREF COL_DIM    = RGB(140, 140, 140);

static int gOutHeight = 180;

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static void SetStatus(const wchar_t* s) {
    if (gStatus) SetWindowTextW(gStatus, s);
}

static std::wstring GetEditText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n + 1, 0);
    GetWindowTextW(h, &s[0], n + 1);
    s.resize(n);
    return s;
}

static void AppendOut(const std::wstring& text) {
    int len = GetWindowTextLengthW(gOut);
    SendMessageW(gOut, EM_SETSEL, len, len);
    SendMessageW(gOut, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
}

static void ClearOut() {
    SetWindowTextW(gOut, L"");
}

static bool SaveAs(HWND hwnd);
static bool Save(HWND hwnd) {
    if (gPath.empty()) return SaveAs(hwnd);
    std::string narrow = WideToUtf8(GetEditText(gEdit));
    std::ofstream out(WideToUtf8(gPath), std::ios::binary);
    if (!out) { SetStatus(L"Save failed"); return false; }
    out << narrow;
    gDirty = false;
    SetStatus(L"Saved");
    return true;
}

static bool SaveAs(HWND hwnd) {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Vayu source (*.vyu)\0*.vyu\0All\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT;
    ofn.lpstrDefExt = L"vyu";
    if (!GetSaveFileNameW(&ofn)) return false;
    gPath = file;
    return Save(hwnd);
}

static void OpenFile(HWND hwnd) {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Vayu source (*.vyu)\0*.vyu\0All\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;
    gPath = file;
    std::ifstream in(WideToUtf8(gPath), std::ios::binary);
    std::ostringstream ss; ss << in.rdbuf();
    SetWindowTextW(gEdit, Utf8ToWide(ss.str()).c_str());
    gDirty = false;
    SetStatus(gPath.c_str());
}

struct RunCtx {
    HWND hwnd;
    std::wstring cmd;
    bool compileVye;
};

static unsigned __stdcall RunThread(void* p) {
    std::unique_ptr<RunCtx> ctx(static_cast<RunCtx*>(p));
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        AppendOut(L"[error] CreatePipe failed\r\n");
        return 1;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> buf(ctx->cmd.begin(), ctx->cmd.end());
    buf.push_back(0);

    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        AppendOut(L"[error] Could not start vayuc.exe — place it beside Vayu-IDE.exe\r\n");
        return 1;
    }

    char block[4096];
    DWORD n = 0;
    std::string acc;
    while (ReadFile(rd, block, sizeof(block), &n, nullptr) && n) {
        acc.append(block, block + n);
        // stream chunks as UTF-8
        AppendOut(Utf8ToWide(std::string(block, block + n)));
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);

    wchar_t footer[128];
    swprintf_s(footer, L"\r\n—— exit %lu ——\r\n", code);
    AppendOut(footer);
    SetStatus(code == 0 ? L"Run finished" : L"Run finished with errors");
    return 0;
}

static void RunVayu(HWND hwnd, bool compileVye) {
    if (gPath.empty() && !SaveAs(hwnd)) return;
    if (gDirty) Save(hwnd);
    ClearOut();
    AppendOut(compileVye ? L"[compile to .vye]\r\n" : L"[run]\r\n");

    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir(self);
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash + 1);
    std::wstring vayuc = dir + L"vayuc.exe";
    if (GetFileAttributesW(vayuc.c_str()) == INVALID_FILE_ATTRIBUTES)
        vayuc = L"vayuc.exe";

    std::wstring cmd;
    if (compileVye) {
        std::wstring out = gPath;
        size_t dot = out.find_last_of(L'.');
        if (dot != std::wstring::npos) out = out.substr(0, dot) + L".vye";
        else out += L".vye";
        cmd = L"\"" + vayuc + L"\" \"" + gPath + L"\" --output \"" + out + L"\"";
    } else {
        cmd = L"\"" + vayuc + L"\" \"" + gPath + L"\"";
    }

    auto* ctx = new RunCtx{ hwnd, cmd, compileVye };
    uintptr_t th = _beginthreadex(nullptr, 0, RunThread, ctx, 0, nullptr);
    if (th) CloseHandle((HANDLE)th);
    else { delete ctx; AppendOut(L"[error] thread spawn failed\r\n"); }
}

static void Layout(HWND hwnd, int w, int h) {
    const int statusH = 22;
    const int labelH = 18;
    int outH = gOutHeight;
    if (outH < 80) outH = 80;
    if (outH > h / 2) outH = h / 2;
    int editH = h - statusH - labelH - outH;
    if (editH < 40) editH = 40;
    MoveWindow(gEdit, 0, 0, w, editH, TRUE);
    MoveWindow(gSplitLabel, 0, editH, w, labelH, TRUE);
    MoveWindow(gOut, 0, editH + labelH, w, outH, TRUE);
    MoveWindow(gStatus, 0, h - statusH, w, statusH, TRUE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        gBgBrush = CreateSolidBrush(COL_BG);
        gEditBrush = CreateSolidBrush(COL_EDIT);
        gOutBrush = CreateSolidBrush(COL_OUT);
        gFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Cascadia Mono");
        if (!gFont)
            gFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        gUiFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

        gEdit = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
            ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN,
            0, 0, 0, 0, hwnd, (HMENU)1001, GetModuleHandleW(nullptr), nullptr);
        gSplitLabel = CreateWindowExW(0, L"STATIC", L"  OUTPUT",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)1003, GetModuleHandleW(nullptr), nullptr);
        gOut = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
            ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY,
            0, 0, 0, 0, hwnd, (HMENU)1004, GetModuleHandleW(nullptr), nullptr);
        gStatus = CreateWindowExW(0, L"STATIC", L" Ready  ·  F5 Run  ·  F7 Compile .vye",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)1002, GetModuleHandleW(nullptr), nullptr);

        SendMessageW(gEdit, WM_SETFONT, (WPARAM)gFont, TRUE);
        SendMessageW(gOut, WM_SETFONT, (WPARAM)gFont, TRUE);
        SendMessageW(gSplitLabel, WM_SETFONT, (WPARAM)gUiFont, TRUE);
        SendMessageW(gStatus, WM_SETFONT, (WPARAM)gUiFont, TRUE);
        // Dark edit colors via EM_SETBKGNDCOLOR if available — use CTLCOLOR
        return 0;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        HWND ctrl = (HWND)lp;
        SetTextColor(hdc, COL_TEXT);
        if (ctrl == gOut) {
            SetBkColor(hdc, COL_OUT);
            return (LRESULT)gOutBrush;
        }
        if (ctrl == gEdit) {
            SetBkColor(hdc, COL_EDIT);
            return (LRESULT)gEditBrush;
        }
        if (ctrl == gStatus) {
            SetTextColor(hdc, RGB(255, 255, 255));
            SetBkColor(hdc, COL_STATUS);
            static HBRUSH st = CreateSolidBrush(COL_STATUS);
            return (LRESULT)st;
        }
        if (ctrl == gSplitLabel) {
            SetTextColor(hdc, COL_DIM);
            SetBkColor(hdc, COL_BG);
            return (LRESULT)gBgBrush;
        }
        SetBkColor(hdc, COL_BG);
        return (LRESULT)gBgBrush;
    }
    case WM_ERASEBKGND: {
        RECT rc; GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, gBgBrush);
        return 1;
    }
    case WM_SIZE: {
        Layout(hwnd, LOWORD(lp), HIWORD(lp));
        return 0;
    }
    case WM_COMMAND: {
        switch (LOWORD(wp)) {
        case 1: OpenFile(hwnd); break;
        case 2: Save(hwnd); break;
        case 3: SaveAs(hwnd); break;
        case 4: RunVayu(hwnd, false); break;
        case 5: RunVayu(hwnd, true); break;
        case 6: SendMessageW(gEdit, EM_UNDO, 0, 0); break;
        case 7: PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
        case 8: ClearOut(); break;
        }
        if (HIWORD(wp) == EN_CHANGE && (HWND)lp == gEdit) gDirty = true;
        return 0;
    }
    case WM_CLOSE:
        if (gDirty) {
            int r = MessageBoxW(hwnd, L"Save changes?", L"Vayu-IDE", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) return 0;
            if (r == IDYES && !Save(hwnd)) return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.lpszClassName = CLS;
    wc.hCursor = LoadCursor(nullptr, IDC_IBEAM);
    wc.hbrBackground = CreateSolidBrush(COL_BG);
    wc.hIcon = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassW(&wc);

    HMENU menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, 1, L"&Open...\tCtrl+O");
    AppendMenuW(file, MF_STRING, 2, L"&Save\tCtrl+S");
    AppendMenuW(file, MF_STRING, 3, L"Save &As...");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, 7, L"E&xit");
    HMENU edit = CreatePopupMenu();
    AppendMenuW(edit, MF_STRING, 6, L"&Undo\tCtrl+Z");
    HMENU run = CreatePopupMenu();
    AppendMenuW(run, MF_STRING, 4, L"&Run\tF5");
    AppendMenuW(run, MF_STRING, 5, L"&Compile to .vye\tF7");
    AppendMenuW(run, MF_STRING, 8, L"Clear &Output");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)file, L"&File");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)edit, L"&Edit");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)run, L"&Build");

    HWND hwnd = CreateWindowExW(0, CLS, L"Vayu-IDE",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1100, 750,
        nullptr, menu, hi, nullptr);
    ShowWindow(hwnd, show);

    ACCEL acc[] = {
        { FVIRTKEY | FCONTROL, 'O', 1 },
        { FVIRTKEY | FCONTROL, 'S', 2 },
        { FVIRTKEY | FCONTROL, 'Z', 6 },
        { FVIRTKEY, VK_F5, 4 },
        { FVIRTKEY, VK_F7, 5 },
    };
    HACCEL hAcc = CreateAcceleratorTableW(acc, 5);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!TranslateAcceleratorW(hwnd, hAcc, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return (int)msg.wParam;
}
#else
int main() { return 1; }
#endif
