// Vayu-IDE — dark theme, OUTPUT pane, live IntelliSense, auto-pairs.
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
#include <algorithm>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

static HWND gEdit, gOut, gStatus, gSplitLabel, gCompl, gTip;
static std::wstring gPath;
static bool gDirty = false;
static const wchar_t* CLS = L"VayuIDEWnd";
static HBRUSH gBgBrush, gEditBrush, gOutBrush;
static HFONT gFont, gUiFont;
static int gOutHeight = 180;
static bool gSuppressEnChange = false;

static const COLORREF COL_BG     = RGB(32, 32, 32);
static const COLORREF COL_EDIT   = RGB(30, 30, 30);
static const COLORREF COL_OUT    = RGB(24, 24, 24);
static const COLORREF COL_TEXT   = RGB(204, 204, 204);
static const COLORREF COL_STATUS = RGB(0, 122, 204);
static const COLORREF COL_DIM    = RGB(140, 140, 140);

struct Snippet {
    const wchar_t* trigger;   // completion label
    const wchar_t* insert;    // text to insert; use \x01 as cursor placeholder
    const wchar_t* help;      // hover / tip text
};

static const Snippet kSnips[] = {
    { L"print",     L"print(\"\x01\")",           L"print(value) — write to standard output" },
    { L"pass",      L"pass",                      L"pass — no-op statement" },
    { L"def",       L"def \x01():\n    pass\n",  L"def name(params) — define a function" },
    { L"return",    L"return \x01",               L"return — exit function with a value" },
    { L"if",        L"if \x01:\n    pass\n",     L"if condition — conditional branch" },
    { L"elif",      L"elif \x01:\n    pass\n",   L"elif — else-if branch" },
    { L"else",      L"else:\n    \x01\n",         L"else — default branch" },
    { L"while",     L"while \x01:\n    pass\n",  L"while condition — loop" },
    { L"for",       L"for \x01 in range(10):\n    pass\n", L"for x in iterable — loop" },
    { L"class",     L"class \x01:\n    pass\n",  L"class — define a type" },
    { L"import",    L"import \x01",               L"import module — load a module" },
    { L"from",      L"from \x01 import ",         L"from module import name" },
    { L"try",       L"try:\n    \x01\nexcept:\n    pass\n", L"try/except — error handling" },
    { L"except",    L"except \x01:\n    pass\n", L"except — catch errors" },
    { L"raise",     L"raise \x01",                L"raise — throw an error" },
    { L"true",      L"true",                      L"true — boolean true" },
    { L"false",     L"false",                     L"false — boolean false" },
    { L"None",      L"None",                      L"None — null value" },
    { L"input",     L"input(\"\x01\")",           L"input(prompt) — read a line as str" },
    { L"read_int",  L"read_int()",                L"read_int() — read an integer from stdin" },
    { L"len",       L"len(\x01)",                 L"len(x) — length of str/list/map/..." },
    { L"str",       L"str(\x01)",                 L"str(x) — convert to string" },
    { L"int",       L"int(\x01)",                 L"int(x) — convert to integer" },
    { L"float",     L"float(\x01)",               L"float(x) — convert to float" },
    { L"bool",      L"bool(\x01)",                L"bool(x) — convert to bool" },
    { L"and",       L"and ",                      L"and — logical AND" },
    { L"or",        L"or ",                       L"or — logical OR" },
    { L"not",       L"not ",                      L"not — logical NOT" },
    { L"self",      L"self",                      L"self — instance reference in methods" },
    { L"break",     L"break",                     L"break — exit the nearest loop" },
    { L"continue",  L"continue",                  L"continue — next loop iteration" },
    { nullptr, nullptr, nullptr }
};

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
static void SetStatus(const wchar_t* s) { if (gStatus) SetWindowTextW(gStatus, s); }
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
static void ClearOut() { SetWindowTextW(gOut, L""); }

static bool SaveAs(HWND hwnd);
static bool Save(HWND hwnd) {
    if (gPath.empty()) return SaveAs(hwnd);
    std::ofstream out(WideToUtf8(gPath), std::ios::binary);
    if (!out) { SetStatus(L"Save failed"); return false; }
    out << WideToUtf8(GetEditText(gEdit));
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
    gSuppressEnChange = true;
    SetWindowTextW(gEdit, Utf8ToWide(ss.str()).c_str());
    gSuppressEnChange = false;
    gDirty = false;
    SetStatus(gPath.c_str());
}

struct RunCtx { HWND hwnd; std::wstring cmd; };
static unsigned __stdcall RunThread(void* p) {
    std::unique_ptr<RunCtx> ctx(static_cast<RunCtx*>(p));
    (void)ctx->hwnd;
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { AppendOut(L"[error] pipe failed\r\n"); return 1; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = wr; si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(ctx->cmd.begin(), ctx->cmd.end());
    buf.push_back(0);
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); AppendOut(L"[error] could not start vayuc.exe\r\n"); return 1; }
    char block[4096]; DWORD n = 0;
    while (ReadFile(rd, block, sizeof(block), &n, nullptr) && n)
        AppendOut(Utf8ToWide(std::string(block, block + n)));
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(rd);
    wchar_t footer[64]; swprintf_s(footer, L"\r\n—— exit %lu ——\r\n", code);
    AppendOut(footer);
    SetStatus(code == 0 ? L"Run finished" : L"Run finished with errors");
    return 0;
}
static void RunVayu(HWND hwnd, bool compileVye) {
    if (gPath.empty() && !SaveAs(hwnd)) return;
    if (gDirty) Save(hwnd);
    ClearOut();
    AppendOut(compileVye ? L"[compile .vye]\r\n" : L"[run]\r\n");
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir(self);
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash + 1);
    std::wstring vayuc = dir + L"vayuc.exe";
    if (GetFileAttributesW(vayuc.c_str()) == INVALID_FILE_ATTRIBUTES) vayuc = L"vayuc.exe";
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
    auto* ctx = new RunCtx{ hwnd, cmd };
    uintptr_t th = _beginthreadex(nullptr, 0, RunThread, ctx, 0, nullptr);
    if (th) CloseHandle((HANDLE)th); else { delete ctx; AppendOut(L"[error] thread\r\n"); }
}

static std::wstring WordBeforeCaret() {
    int start = 0, end = 0;
    SendMessageW(gEdit, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    std::wstring all = GetEditText(gEdit);
    if (start > (int)all.size()) start = (int)all.size();
    int i = start;
    while (i > 0) {
        wchar_t c = all[(size_t)i - 1];
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
              (c >= L'0' && c <= L'9') || c == L'_')) break;
        --i;
    }
    return all.substr((size_t)i, (size_t)(start - i));
}

static void HideCompl() {
    if (gCompl) ShowWindow(gCompl, SW_HIDE);
    if (gTip) ShowWindow(gTip, SW_HIDE);
}

static void ShowTipForSel() {
    if (!gCompl || !IsWindowVisible(gCompl)) return;
    int sel = (int)SendMessageW(gCompl, LB_GETCURSEL, 0, 0);
    if (sel < 0) return;
    wchar_t label[128];
    SendMessageW(gCompl, LB_GETTEXT, sel, (LPARAM)label);
    const wchar_t* help = L"";
    for (int i = 0; kSnips[i].trigger; ++i)
        if (wcscmp(kSnips[i].trigger, label) == 0) { help = kSnips[i].help; break; }
    if (!gTip) {
        gTip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"STATIC", L"",
            WS_POPUP | WS_BORDER | SS_LEFT,
            0, 0, 360, 40, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(gTip, WM_SETFONT, (WPARAM)gUiFont, TRUE);
    }
    SetWindowTextW(gTip, help);
    RECT rc; GetWindowRect(gCompl, &rc);
    SetWindowPos(gTip, HWND_TOPMOST, rc.right + 4, rc.top, 380, 48, SWP_SHOWWINDOW);
}

static void UpdateIntelliSense(HWND hwnd) {
    std::wstring prefix = WordBeforeCaret();
    if (prefix.empty()) { HideCompl(); return; }

    std::vector<std::wstring> items;
    for (int i = 0; kSnips[i].trigger; ++i) {
        if (_wcsnicmp(kSnips[i].trigger, prefix.c_str(), prefix.size()) == 0)
            items.push_back(kSnips[i].trigger);
    }
    // identifiers in buffer
    std::wstring body = GetEditText(gEdit);
    std::wstring cur;
    for (size_t i = 0; i <= body.size(); ++i) {
        wchar_t c = (i < body.size()) ? body[i] : L' ';
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
            (c >= L'0' && c <= L'9') || c == L'_') cur.push_back(c);
        else {
            if (cur.size() > prefix.size() &&
                _wcsnicmp(cur.c_str(), prefix.c_str(), prefix.size()) == 0) {
                bool seen = false;
                for (auto& x : items) if (x == cur) { seen = true; break; }
                if (!seen && cur != prefix) items.push_back(cur);
            }
            cur.clear();
        }
    }
    if (items.empty()) { HideCompl(); return; }
    std::sort(items.begin(), items.end());
    items.erase(std::unique(items.begin(), items.end()), items.end());

    if (!gCompl) {
        gCompl = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"LISTBOX", L"",
            WS_POPUP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS,
            0, 0, 260, 200, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(gCompl, WM_SETFONT, (WPARAM)gFont, TRUE);
    }
    SendMessageW(gCompl, LB_RESETCONTENT, 0, 0);
    for (auto& it : items) SendMessageW(gCompl, LB_ADDSTRING, 0, (LPARAM)it.c_str());
    SendMessageW(gCompl, LB_SETCURSEL, 0, 0);

    // caret-ish position under edit
    RECT rc; GetWindowRect(gEdit, &rc);
    POINT pt{ 24, 48 };
    ClientToScreen(gEdit, &pt);
    SetWindowPos(gCompl, HWND_TOPMOST, pt.x, pt.y, 260, 200, SWP_SHOWWINDOW);
    ShowTipForSel();
}

static void AcceptCompletion() {
    if (!gCompl || !IsWindowVisible(gCompl)) return;
    int sel = (int)SendMessageW(gCompl, LB_GETCURSEL, 0, 0);
    if (sel < 0) { HideCompl(); return; }
    wchar_t label[128];
    SendMessageW(gCompl, LB_GETTEXT, sel, (LPARAM)label);

    const wchar_t* insert = label;
    for (int i = 0; kSnips[i].trigger; ++i)
        if (wcscmp(kSnips[i].trigger, label) == 0) { insert = kSnips[i].insert; break; }

    std::wstring prefix = WordBeforeCaret();
    int start = 0, end = 0;
    SendMessageW(gEdit, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    int p0 = start - (int)prefix.size();
    if (p0 < 0) p0 = 0;

    // Build insert text; \x01 marks final caret
    std::wstring text = insert;
    int caretOff = -1;
    size_t marker = text.find(L'\x01');
    if (marker != std::wstring::npos) {
        caretOff = (int)marker;
        text.erase(marker, 1);
    }

    gSuppressEnChange = true;
    SendMessageW(gEdit, EM_SETSEL, p0, start);
    SendMessageW(gEdit, EM_REPLACESEL, TRUE, (LPARAM)text.c_str());
    if (caretOff >= 0) {
        int cpos = p0 + caretOff;
        SendMessageW(gEdit, EM_SETSEL, cpos, cpos);
    }
    gSuppressEnChange = false;
    HideCompl();
    SetFocus(gEdit);
    SetStatus(L"Ready");
}

// Subclass edit for auto-pairs and live IS
static WNDPROC gOldEditProc;
static LRESULT CALLBACK EditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CHAR) {
        wchar_t ch = (wchar_t)wp;
        // Auto-pair brackets / quotes
        const wchar_t* pair = nullptr;
        if (ch == L'(') pair = L")";
        else if (ch == L'[') pair = L"]";
        else if (ch == L'{') pair = L"}";
        else if (ch == L'"') pair = L"\"";
        else if (ch == L'\'') pair = L"'";
        if (pair) {
            LRESULT r = CallWindowProcW(gOldEditProc, h, msg, wp, lp);
            int start = 0, end = 0;
            SendMessageW(h, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
            gSuppressEnChange = true;
            SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)pair);
            SendMessageW(h, EM_SETSEL, start, start); // caret between
            gSuppressEnChange = false;
            UpdateIntelliSense(GetParent(h));
            return r;
        }
        if (ch == L')' || ch == L']' || ch == L'}') {
            // skip if next char is the closer
            int start = 0, end = 0;
            SendMessageW(h, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
            std::wstring all = GetEditText(h);
            if (start < (int)all.size() && all[(size_t)start] == ch) {
                SendMessageW(h, EM_SETSEL, start + 1, start + 1);
                return 0;
            }
        }
        LRESULT r = CallWindowProcW(gOldEditProc, h, msg, wp, lp);
        if ((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || ch == L'_')
            UpdateIntelliSense(GetParent(h));
        else if (ch == L' ' || ch == L'\r' || ch == L'\n' || ch == L'\t')
            HideCompl();
        return r;
    }
    if (msg == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) { HideCompl(); return 0; }
        if (IsWindowVisible(gCompl)) {
            if (wp == VK_RETURN || wp == VK_TAB) { AcceptCompletion(); return 0; }
            if (wp == VK_UP || wp == VK_DOWN) {
                int sel = (int)SendMessageW(gCompl, LB_GETCURSEL, 0, 0);
                int cnt = (int)SendMessageW(gCompl, LB_GETCOUNT, 0, 0);
                if (wp == VK_UP) sel = (sel <= 0) ? cnt - 1 : sel - 1;
                else sel = (sel + 1) % cnt;
                SendMessageW(gCompl, LB_SETCURSEL, sel, 0);
                ShowTipForSel();
                return 0;
            }
        }
    }
    return CallWindowProcW(gOldEditProc, h, msg, wp, lp);
}

static void Layout(HWND hwnd, int w, int h) {
    const int statusH = 22, labelH = 18;
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
        gFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Cascadia Mono");
        if (!gFont)
            gFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        gUiFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

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
        gStatus = CreateWindowExW(0, L"STATIC", L" Ready  ·  F5 Run  ·  F7 Compile .vye  ·  type for IntelliSense",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)1002, GetModuleHandleW(nullptr), nullptr);

        SendMessageW(gEdit, WM_SETFONT, (WPARAM)gFont, TRUE);
        SendMessageW(gOut, WM_SETFONT, (WPARAM)gFont, TRUE);
        SendMessageW(gSplitLabel, WM_SETFONT, (WPARAM)gUiFont, TRUE);
        SendMessageW(gStatus, WM_SETFONT, (WPARAM)gUiFont, TRUE);

        gOldEditProc = (WNDPROC)SetWindowLongPtrW(gEdit, GWLP_WNDPROC, (LONG_PTR)EditProc);
        return 0;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp; HWND ctrl = (HWND)lp;
        SetTextColor(hdc, COL_TEXT);
        if (ctrl == gOut) { SetBkColor(hdc, COL_OUT); return (LRESULT)gOutBrush; }
        if (ctrl == gEdit) { SetBkColor(hdc, COL_EDIT); return (LRESULT)gEditBrush; }
        if (ctrl == gStatus) {
            SetTextColor(hdc, RGB(255, 255, 255));
            SetBkColor(hdc, COL_STATUS);
            static HBRUSH st = CreateSolidBrush(COL_STATUS);
            return (LRESULT)st;
        }
        if (ctrl == gSplitLabel) {
            SetTextColor(hdc, COL_DIM); SetBkColor(hdc, COL_BG); return (LRESULT)gBgBrush;
        }
        SetBkColor(hdc, COL_BG); return (LRESULT)gBgBrush;
    }
    case WM_ERASEBKGND: {
        RECT rc; GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, gBgBrush);
        return 1;
    }
    case WM_SIZE: Layout(hwnd, LOWORD(lp), HIWORD(lp)); return 0;
    case WM_COMMAND: {
        if (HIWORD(wp) == LBN_DBLCLK && (HWND)lp == gCompl) { AcceptCompletion(); return 0; }
        if (HIWORD(wp) == LBN_SELCHANGE && (HWND)lp == gCompl) { ShowTipForSel(); return 0; }
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
        if (HIWORD(wp) == EN_CHANGE && (HWND)lp == gEdit && !gSuppressEnChange)
            gDirty = true;
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
    case WM_DESTROY: PostQuitMessage(0); return 0;
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
    HICON big = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    if (!big) big = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    HICON small = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hIcon = big;
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
    if (big) SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
    if (small) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)small);
    else if (big) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)big);
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
