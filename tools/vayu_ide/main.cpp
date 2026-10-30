// Vayu-IDE — lightweight Win32 editor for .vyu files.
// Build: linked from compiler/CMakeLists as Vayu-IDE.exe when WIN32.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

static HWND gEdit, gStatus;
static std::wstring gPath;
static bool gDirty = false;
static const wchar_t* CLS = L"VayuIDEWnd";

static void SetStatus(const wchar_t* s) {
    if (gStatus) SetWindowTextW(gStatus, s);
}

static std::wstring GetEditText() {
    int n = GetWindowTextLengthW(gEdit);
    std::wstring s(n + 1, 0);
    GetWindowTextW(gEdit, &s[0], n + 1);
    s.resize(n);
    return s;
}

static void SetEditText(const std::wstring& s) {
    SetWindowTextW(gEdit, s.c_str());
    gDirty = false;
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
    std::wstring body = GetEditText();
    std::ofstream out(std::string(gPath.begin(), gPath.end()), std::ios::binary);
    // UTF-8 naive: write as narrow (ASCII-compatible subset).
    std::string narrow(body.begin(), body.end());
    out << narrow;
    gDirty = false;
    SetStatus(L"Saved");
    return true;
}

static bool Save(HWND hwnd) {
    if (gPath.empty()) return SaveAs(hwnd);
    std::wstring body = GetEditText();
    std::ofstream out(std::string(gPath.begin(), gPath.end()), std::ios::binary);
    std::string narrow(body.begin(), body.end());
    out << narrow;
    gDirty = false;
    SetStatus(L"Saved");
    return true;
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
    std::ifstream in(std::string(gPath.begin(), gPath.end()), std::ios::binary);
    std::ostringstream ss; ss << in.rdbuf();
    std::string narrow = ss.str();
    SetEditText(std::wstring(narrow.begin(), narrow.end()));
    SetStatus(gPath.c_str());
}

static void RunVayu(HWND hwnd, bool compileVye) {
    if (gPath.empty() && !SaveAs(hwnd)) return;
    if (gDirty) Save(hwnd);
    std::wstring cmd;
    // Prefer vayuc next to IDE or on PATH
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir(self);
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash + 1);
    std::wstring vayuc = dir + L"vayuc.exe";
    if (GetFileAttributesW(vayuc.c_str()) == INVALID_FILE_ATTRIBUTES)
        vayuc = L"vayuc.exe";
    if (compileVye) {
        std::wstring out = gPath;
        size_t dot = out.find_last_of(L'.');
        if (dot != std::wstring::npos) out = out.substr(0, dot) + L".vye";
        else out += L".vye";
        cmd = L"\"" + vayuc + L"\" \"" + gPath + L"\" --output \"" + out + L"\"";
    } else {
        cmd = L"\"" + vayuc + L"\" \"" + gPath + L"\"";
    }
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
        CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        SetStatus(compileVye ? L"Compile (.vye) launched" : L"Run launched");
    } else {
        SetStatus(L"Failed to launch vayuc.exe");
        MessageBoxW(hwnd,
            L"Could not start vayuc.exe. Build Vayu and place vayuc.exe beside Vayu-IDE.exe.",
            L"Vayu-IDE", MB_ICONERROR);
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        gEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
            ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN,
            0, 0, 0, 0, hwnd, (HMENU)1001, GetModuleHandleW(nullptr), nullptr);
        gStatus = CreateWindowExW(0, L"STATIC", L"Ready — File Open / Save / Run / Compile",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)1002, GetModuleHandleW(nullptr), nullptr);
        HFONT font = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        SendMessageW(gEdit, WM_SETFONT, (WPARAM)font, TRUE);
        return 0;
    }
    case WM_SIZE: {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(gEdit, 0, 0, w, h > 24 ? h - 24 : h, TRUE);
        MoveWindow(gStatus, 0, h > 24 ? h - 24 : 0, w, 24, TRUE);
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
        }
        if (HIWORD(wp) == EN_CHANGE && (HWND)lp == gEdit) {
            gDirty = true;
        }
        return 0;
    }
    case WM_CLOSE:
        if (gDirty) {
            int r = MessageBoxW(hwnd, L"Save changes?", L"Vayu-IDE", MB_YESNOCANCEL);
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
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    // Load IDE.ico if present beside exe or from resource ID 1
    wc.hIcon = (HICON)LoadImageW(hi, L"IDI_ICON1", IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
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
    AppendMenuW(run, MF_STRING, 4, L"&Run (.vyu)\tF5");
    AppendMenuW(run, MF_STRING, 5, L"&Compile to .vye\tF7");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)file, L"&File");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)edit, L"&Edit");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)run, L"&Build");

    HWND hwnd = CreateWindowExW(0, CLS, L"Vayu-IDE",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 900, 700,
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
