// FolderSizeViewer — Win32 GUI folder size analyzer
// Shows REAL disk usage — cloud-only files (OneDrive etc.) counted as 0

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstdio>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

// Cloud-only file attributes (OneDrive Files On-Demand)
#ifndef FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
#define FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS 0x400000
#endif

static bool IsCloudOnly(DWORD attr) {
    return (attr & FILE_ATTRIBUTE_OFFLINE) ||
           (attr & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS);
}

// --------------- Messages & IDs ---------------
const UINT WM_SCAN_DONE = WM_USER + 1;

const int ID_PANEL       = 100;
const int ID_STATUS      = 101;
const int ID_BTN_OPEN    = 102;
const int ID_BTN_UP      = 103;
const int ID_BTN_REFRESH = 104;
const int ID_PATH_LABEL  = 105;

// --------------- Dark theme colors ---------------
const COLORREF CLR_BG       = RGB(20,  21,  31);
const COLORREF CLR_ROW_ALT  = RGB(27,  28,  40);
const COLORREF CLR_SEL      = RGB(38,  62,  100);
const COLORREF CLR_HDR      = RGB(32,  33,  46);
const COLORREF CLR_TOOLBAR  = RGB(28,  29,  42);
const COLORREF CLR_TEXT     = RGB(218, 220, 230);
const COLORREF CLR_SUB      = RGB(148, 158, 175);
const COLORREF CLR_GRID     = RGB(44,  46,  64);
const COLORREF CLR_FOLDER   = RGB(88,  162, 232);
const COLORREF CLR_FILE     = RGB(185, 180, 105);
const COLORREF CLR_CLOUD    = RGB(80,  160, 240);
const COLORREF CLR_CLOUD_DIM= RGB(60,  90,  140);

static const COLORREF BAR_COLORS[] = {
    RGB(58,  138, 216), RGB(42,  190, 128), RGB(222, 148, 42),
    RGB(212,  68,  78), RGB(152,  98, 218), RGB(218, 112, 48),
    RGB(48,  192, 202), RGB(202,  68, 142),
};
const int NUM_BAR_COLORS = 8;

// --------------- Data ---------------
struct Entry {
    std::wstring name, path;
    ULONGLONG    diskSize  = 0;   // реально на диске
    ULONGLONG    cloudSize = 0;   // только в облаке
    ULONGLONG    fileCount = 0;
    bool         isDir     = false;
};

struct AppState {
    HWND hMain = nullptr, hPanel = nullptr;
    HWND hStatus = nullptr, hBtnOpen = nullptr;
    HWND hBtnUp  = nullptr, hBtnRefresh = nullptr;
    HWND hPathLabel = nullptr;

    std::wstring       rootPath;
    std::vector<Entry> entries;
    ULONGLONG          totalDisk  = 0;
    ULONGLONG          totalCloud = 0;
    std::mutex         mtx;

    std::thread       scanThread;
    std::atomic<bool> scanning  {false};
    std::atomic<bool> doCancel  {false};

    int  scrollPos   = 0;
    int  selectedIdx = -1;

    HFONT hFont = nullptr, hFontBold = nullptr, hFontMono = nullptr;
    HDC   hdcBuf = nullptr;
    HBITMAP hbmBuf = nullptr;
    int bufW = 0, bufH = 0;
};
static AppState G;

// --------------- Utilities ---------------
static std::wstring SizeStr(ULONGLONG b) {
    wchar_t s[64];
    if      (b >= 1ULL<<40) swprintf_s(s, L"%.2f TB", (double)b/(1ULL<<40));
    else if (b >= 1ULL<<30) swprintf_s(s, L"%.2f GB", (double)b/(1ULL<<30));
    else if (b >= 1ULL<<20) swprintf_s(s, L"%.2f MB", (double)b/(1ULL<<20));
    else if (b >= 1ULL<<10) swprintf_s(s, L"%.2f KB", (double)b/(1ULL<<10));
    else                    swprintf_s(s, L"%llu B",   b);
    return s;
}

// Рекурсивный подсчёт: diskSize = реальный, возврат = логический (disk+cloud)
static void CalcSizes(const std::wstring& dir,
                      ULONGLONG& diskSize, ULONGLONG& cloudSize,
                      ULONGLONG& fc, std::atomic<bool>& cancel) {
    if (cancel) return;
    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (cancel) break;
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CalcSizes(dir + L"\\" + n, diskSize, cloudSize, fc, cancel);
        } else {
            ULONGLONG sz = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            if (IsCloudOnly(fd.dwFileAttributes))
                cloudSize += sz;
            else
                diskSize += sz;
            ++fc;
        }
    } while (FindNextFile(h, &fd));
    FindClose(h);
}

// --------------- Scan thread ---------------
static void ScanThread(std::wstring path) {
    std::vector<Entry> result;
    ULONGLONG totalDisk = 0, totalCloud = 0;

    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile((path + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (G.doCancel) break;
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;

            Entry e;
            e.name  = n;
            e.path  = path + L"\\" + n;
            e.isDir = !!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);

            if (e.isDir) {
                wchar_t st[260];
                swprintf_s(st, L"Scanning: %s", n.c_str());
                SendMessage(G.hStatus, SB_SETTEXT, 0, (LPARAM)st);
                CalcSizes(e.path, e.diskSize, e.cloudSize, e.fileCount, G.doCancel);
            } else {
                ULONGLONG sz = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                if (IsCloudOnly(fd.dwFileAttributes))
                    e.cloudSize = sz;
                else
                    e.diskSize  = sz;
                e.fileCount = 1;
            }
            totalDisk  += e.diskSize;
            totalCloud += e.cloudSize;
            result.push_back(std::move(e));
        } while (FindNextFile(h, &fd));
        FindClose(h);
    }

    if (!G.doCancel) {
        // Сортировка по реальному месту на диске
        std::sort(result.begin(), result.end(), [](const Entry& a, const Entry& b){
            return a.diskSize > b.diskSize;
        });
        std::lock_guard<std::mutex> lk(G.mtx);
        G.entries    = std::move(result);
        G.totalDisk  = totalDisk;
        G.totalCloud = totalCloud;
    }
    G.scanning = false;
    PostMessage(G.hMain, WM_SCAN_DONE, 0, 0);
}

static void StartScan(const std::wstring& path) {
    G.doCancel = true;
    if (G.scanThread.joinable()) G.scanThread.join();
    { std::lock_guard<std::mutex> lk(G.mtx); G.entries.clear(); G.totalDisk = G.totalCloud = 0; }

    G.rootPath    = path;
    G.scrollPos   = 0;
    G.selectedIdx = -1;
    G.doCancel    = false;
    G.scanning    = true;

    SetWindowText(G.hPathLabel, path.c_str());
    SendMessage(G.hStatus, SB_SETTEXT, 0, (LPARAM)L"Scanning…");
    InvalidateRect(G.hPanel, nullptr, FALSE);
    G.scanThread = std::thread(ScanThread, path);
}

// --------------- Layout constants ---------------
const int TOOLBAR_H = 44;
const int COL_H     = 28;
const int ITEM_H    = 40;
const int PAD       = 10;
const int ICON_W    = 28;
const int NAME_W    = 210;
const int DISK_W    = 90;
const int CLOUD_W   = 90;
const int PCT_W     = 54;
const int FILES_W   = 76;

// --------------- Paint helpers ---------------
static HBRUSH HBrush(COLORREF c) { return CreateSolidBrush(c); }

static void DrawHLine(HDC hdc, int x1, int x2, int y, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HPEN old = (HPEN)SelectObject(hdc, p);
    MoveToEx(hdc, x1, y, nullptr); LineTo(hdc, x2, y);
    SelectObject(hdc, old); DeleteObject(p);
}

static void TxtR(HDC hdc, const wchar_t* s, RECT r, UINT fmt) {
    DrawText(hdc, s, -1, &r, fmt | DT_SINGLELINE | DT_NOPREFIX);
}

// --------------- Panel paint ---------------
static void PaintPanel(HDC hdc, int W, int H) {
    RECT rc = {0, 0, W, H};
    HBRUSH hbg = HBrush(CLR_BG); FillRect(hdc, &rc, hbg); DeleteObject(hbg);
    SetBkMode(hdc, TRANSPARENT);

    std::vector<Entry> ents;
    ULONGLONG totalDisk, totalCloud;
    {
        std::lock_guard<std::mutex> lk(G.mtx);
        ents       = G.entries;
        totalDisk  = G.totalDisk;
        totalCloud = G.totalCloud;
    }

    if (G.scanning && ents.empty()) {
        SelectObject(hdc, G.hFontBold); SetTextColor(hdc, CLR_SUB);
        TxtR(hdc, L"Scanning…  Please wait", rc, DT_CENTER | DT_VCENTER);
        return;
    }
    if (!G.scanning && ents.empty()) {
        SelectObject(hdc, G.hFontBold); SetTextColor(hdc, CLR_SUB);
        TxtR(hdc, L"Open a folder to analyze its contents", rc, DT_CENTER | DT_VCENTER);
        return;
    }

    // ---- Column header ----
    {
        RECT hr = {0, 0, W, COL_H};
        HBRUSH hh = HBrush(CLR_HDR); FillRect(hdc, &hr, hh); DeleteObject(hh);
        DrawHLine(hdc, 0, W, COL_H - 1, CLR_GRID);

        SelectObject(hdc, G.hFont); SetTextColor(hdc, CLR_SUB);
        int x = PAD;

        TxtR(hdc, L"Name",
             {x+ICON_W, 0, x+ICON_W+NAME_W, COL_H}, DT_LEFT|DT_VCENTER);

        int dx = x + ICON_W + NAME_W;
        TxtR(hdc, L"На диске",
             {dx, 0, dx+DISK_W, COL_H}, DT_RIGHT|DT_VCENTER);

        int cx = dx + DISK_W + 4;
        SetTextColor(hdc, CLR_CLOUD_DIM);
        TxtR(hdc, L"Облако",
             {cx, 0, cx+CLOUD_W, COL_H}, DT_RIGHT|DT_VCENTER);

        int barX = cx + CLOUD_W + PAD;
        int barW = W - barX - PCT_W - FILES_W - PAD*3;
        if (barW > 40) {
            SetTextColor(hdc, CLR_SUB);
            TxtR(hdc, L"Диск %", {barX, 0, barX+barW, COL_H}, DT_CENTER|DT_VCENTER);
        }

        int px = W - PCT_W - FILES_W - PAD*2;
        SetTextColor(hdc, CLR_SUB);
        TxtR(hdc, L"%",     {px, 0, px+PCT_W,   COL_H}, DT_RIGHT|DT_VCENTER);

        int fx = W - FILES_W - PAD;
        TxtR(hdc, L"Файлы", {fx, 0, fx+FILES_W, COL_H}, DT_RIGHT|DT_VCENTER);
    }

    // ---- Items ----
    int startY   = COL_H;
    int visStart = G.scrollPos;
    int maxVis   = (H - startY) / ITEM_H + 2;
    int visEnd   = (int)std::min((int)ents.size(), visStart + maxVis);

    for (int i = visStart; i < visEnd; i++) {
        const Entry& e = ents[i];
        int y = startY + (i - visStart) * ITEM_H;
        if (y > H) break;

        RECT row = {0, y, W, y + ITEM_H};
        COLORREF bg = (i == G.selectedIdx) ? CLR_SEL
                    : (i % 2 == 0)         ? CLR_BG : CLR_ROW_ALT;
        HBRUSH hbr = HBrush(bg); FillRect(hdc, &row, hbr); DeleteObject(hbr);
        DrawHLine(hdc, 0, W, y + ITEM_H - 1, CLR_GRID);

        int midY = y + (ITEM_H - 16) / 2;
        int x = PAD;

        // Иконка — облако если есть cloudSize, иначе папка/файл
        SelectObject(hdc, G.hFont);
        bool hasCloud = (e.cloudSize > 0);
        if (hasCloud && e.diskSize == 0) {
            // Полностью в облаке
            SetTextColor(hdc, CLR_CLOUD);
            TxtR(hdc, L"\x2601", {x, y, x+ICON_W, y+ITEM_H}, DT_CENTER|DT_VCENTER);
        } else if (hasCloud) {
            // Частично в облаке
            SetTextColor(hdc, CLR_CLOUD_DIM);
            TxtR(hdc, L"\x2601", {x, y, x+ICON_W, y+ITEM_H}, DT_CENTER|DT_VCENTER);
        } else {
            SetTextColor(hdc, e.isDir ? CLR_FOLDER : CLR_FILE);
            TxtR(hdc, e.isDir ? L"\x25B6" : L"\x2022",
                 {x, y, x+ICON_W, y+ITEM_H}, DT_CENTER|DT_VCENTER);
        }

        // Имя
        SetTextColor(hdc, CLR_TEXT);
        SelectObject(hdc, G.hFont);
        RECT nr = {x+ICON_W, midY, x+ICON_W+NAME_W, midY+20};
        DrawText(hdc, e.name.c_str(), -1, &nr,
                 DT_LEFT|DT_TOP|DT_END_ELLIPSIS|DT_SINGLELINE|DT_NOPREFIX);

        // На диске
        int dx = x + ICON_W + NAME_W;
        SelectObject(hdc, G.hFontMono);
        SetTextColor(hdc, e.diskSize > 0 ? CLR_TEXT : CLR_SUB);
        TxtR(hdc, (e.diskSize > 0 ? SizeStr(e.diskSize) : L"—").c_str(),
             {dx, midY, dx+DISK_W, midY+20}, DT_RIGHT|DT_TOP);

        // Облако
        int cx = dx + DISK_W + 4;
        SetTextColor(hdc, hasCloud ? CLR_CLOUD : CLR_GRID);
        TxtR(hdc, (hasCloud ? SizeStr(e.cloudSize) : L"—").c_str(),
             {cx, midY, cx+CLOUD_W, midY+20}, DT_RIGHT|DT_TOP);

        // Bar — только реальное место на диске
        int barX = cx + CLOUD_W + PAD;
        int barW = W - barX - PCT_W - FILES_W - PAD*3;
        if (barW > 10) {
            int bh = 12, bby = y + (ITEM_H - bh) / 2;

            // Фон бара
            RECT bbg = {barX, bby, barX+barW, bby+bh};
            HBRUSH hbbg = HBrush(RGB(38,40,58)); FillRect(hdc, &bbg, hbbg); DeleteObject(hbbg);

            // Диск (цветной)
            double diskRatio  = totalDisk > 0 ? (double)e.diskSize / totalDisk : 0.0;
            int    diskFill   = (int)(diskRatio * barW);
            if (diskFill > 0) {
                COLORREF bc = BAR_COLORS[i % NUM_BAR_COLORS];
                RECT bf = {barX, bby, barX+diskFill, bby+bh};
                HBRUSH hf = HBrush(bc); FillRect(hdc, &bf, hf); DeleteObject(hf);
                // Блик
                HPEN sp = CreatePen(PS_SOLID, 1,
                    RGB(std::min(255,(int)GetRValue(bc)+55),
                        std::min(255,(int)GetGValue(bc)+55),
                        std::min(255,(int)GetBValue(bc)+55)));
                HPEN op = (HPEN)SelectObject(hdc, sp);
                MoveToEx(hdc, barX, bby+1, nullptr); LineTo(hdc, barX+diskFill, bby+1);
                SelectObject(hdc, op); DeleteObject(sp);
            }

            // Облако (полупрозрачная синяя полоса поверх)
            ULONGLONG total = e.diskSize + e.cloudSize;
            if (e.cloudSize > 0 && total > 0) {
                double cloudRatio = (double)e.cloudSize / (totalDisk + totalCloud);
                int cloudFill = (int)(cloudRatio * barW);
                if (cloudFill > 0) {
                    RECT cf = {barX + diskFill, bby, barX + diskFill + cloudFill, bby + bh};
                    HBRUSH hcf = HBrush(RGB(40, 80, 130));
                    FillRect(hdc, &cf, hcf); DeleteObject(hcf);
                }
            }

            // Процент (от диска)
            int px = barX + barW + PAD;
            wchar_t pct[16];
            swprintf_s(pct, L"%.1f%%", diskRatio * 100.0);
            SetTextColor(hdc, diskRatio > 0 ? CLR_SUB : CLR_GRID);
            TxtR(hdc, pct, {px, midY, px+PCT_W, midY+20}, DT_RIGHT|DT_TOP);
        }

        // Кол-во файлов
        if (e.fileCount > 0) {
            int fx = W - FILES_W - PAD;
            wchar_t fc[32]; swprintf_s(fc, L"%llu", e.fileCount);
            SetTextColor(hdc, CLR_SUB); SelectObject(hdc, G.hFontMono);
            TxtR(hdc, fc, {fx, midY, fx+FILES_W, midY+20}, DT_RIGHT|DT_TOP);
        }
    }

    // Полоса прокрутки
    int total_items = (int)ents.size();
    if (total_items > 0) {
        int usable = H - startY;
        int vis    = usable / ITEM_H;
        if (vis < total_items) {
            int sbarH = (int)((double)vis / total_items * usable);
            int sbarY = startY + (int)((double)G.scrollPos / total_items * usable);
            sbarH = std::max(20, sbarH);
            RECT sb = {W-5, sbarY, W-2, sbarY+sbarH};
            HBRUSH hsb = HBrush(RGB(75,78,108)); FillRect(hdc, &sb, hsb); DeleteObject(hsb);
        }
    }
}

// --------------- Panel window proc ---------------
LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        int W = rc.right, H = rc.bottom;
        if (W != G.bufW || H != G.bufH || !G.hdcBuf) {
            if (G.hbmBuf) DeleteObject(G.hbmBuf);
            if (G.hdcBuf) DeleteDC(G.hdcBuf);
            G.hdcBuf = CreateCompatibleDC(hdc);
            G.hbmBuf = CreateCompatibleBitmap(hdc, W, H);
            G.bufW = W; G.bufH = H;
        }
        SelectObject(G.hdcBuf, G.hbmBuf);
        PaintPanel(G.hdcBuf, W, H);
        BitBlt(hdc, 0, 0, W, H, G.hdcBuf, 0, 0, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int step = -GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA * 3;
        std::lock_guard<std::mutex> lk(G.mtx);
        int mx = std::max(0, (int)G.entries.size() - 1);
        G.scrollPos = std::max(0, std::min(mx, G.scrollPos + step));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        SetFocus(hwnd);
        int my = GET_Y_LPARAM(lParam);
        if (my > COL_H) {
            int idx = (my - COL_H) / ITEM_H + G.scrollPos;
            std::lock_guard<std::mutex> lk(G.mtx);
            if (idx >= 0 && idx < (int)G.entries.size())
                G.selectedIdx = idx;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        int my = GET_Y_LPARAM(lParam);
        if (my > COL_H) {
            int idx = (my - COL_H) / ITEM_H + G.scrollPos;
            std::wstring sub;
            {
                std::lock_guard<std::mutex> lk(G.mtx);
                if (idx >= 0 && idx < (int)G.entries.size() && G.entries[idx].isDir)
                    sub = G.entries[idx].path;
            }
            if (!sub.empty()) StartScan(sub);
        }
        return 0;
    }
    case WM_MOUSEMOVE: SetFocus(hwnd); return 0;
    case WM_SIZE: InvalidateRect(hwnd, nullptr, FALSE); return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// --------------- Main window proc ---------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        G.hMain = hwnd;

        G.hFont = CreateFont(-14,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        G.hFontBold = CreateFont(-14,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI Semibold");
        G.hFontMono = CreateFont(-13,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,FIXED_PITCH,L"Consolas");

        auto MkBtn = [&](const wchar_t* text, int x, int w, int id) -> HWND {
            HWND h = CreateWindow(L"BUTTON", text,
                WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                x, 9, w, 26, hwnd, (HMENU)(intptr_t)id,
                GetModuleHandle(nullptr), nullptr);
            SendMessage(h, WM_SETFONT, (WPARAM)G.hFont, TRUE);
            return h;
        };
        G.hBtnOpen    = MkBtn(L"Open Folder",   8,   115, ID_BTN_OPEN);
        G.hBtnUp      = MkBtn(L"\x2191 Up",   131,    70, ID_BTN_UP);
        G.hBtnRefresh = MkBtn(L"\x21BB Refresh", 209, 90, ID_BTN_REFRESH);

        G.hPathLabel = CreateWindow(L"STATIC", L"No folder selected",
            WS_CHILD|WS_VISIBLE|SS_LEFT|SS_ENDELLIPSIS,
            308, 14, 600, 20, hwnd, (HMENU)ID_PATH_LABEL,
            GetModuleHandle(nullptr), nullptr);
        SendMessage(G.hPathLabel, WM_SETFONT, (WPARAM)G.hFont, TRUE);

        WNDCLASS wc{};
        wc.style         = CS_DBLCLKS|CS_HREDRAW|CS_VREDRAW;
        wc.lpfnWndProc   = PanelProc;
        wc.hInstance     = GetModuleHandle(nullptr);
        wc.lpszClassName = L"FSV_Panel";
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        RegisterClass(&wc);

        G.hPanel = CreateWindowEx(0, L"FSV_Panel", nullptr,
            WS_CHILD|WS_VISIBLE|WS_TABSTOP,
            0, TOOLBAR_H, 800, 500, hwnd, (HMENU)ID_PANEL,
            GetModuleHandle(nullptr), nullptr);

        G.hStatus = CreateStatusWindow(WS_CHILD|WS_VISIBLE|SBARS_SIZEGRIP,
            L"Open a folder with \x201COpen Folder\x201D or drag-and-drop",
            hwnd, ID_STATUS);
        SendMessage(G.hStatus, WM_SETFONT, (WPARAM)G.hFont, TRUE);

        DragAcceptFiles(hwnd, TRUE);
        return 0;
    }

    case WM_ERASEBKGND: {
        RECT rc; GetClientRect(hwnd, &rc);
        rc.bottom = TOOLBAR_H;
        HBRUSH hbr = HBrush(CLR_TOOLBAR);
        FillRect((HDC)wParam, &rc, hbr); DeleteObject(hbr);
        return 1;
    }

    case WM_CTLCOLORSTATIC: {
        static HBRUSH hTBrush = nullptr;
        if (!hTBrush) hTBrush = CreateSolidBrush(CLR_TOOLBAR);
        SetTextColor((HDC)wParam, CLR_TEXT);
        SetBkColor((HDC)wParam, CLR_TOOLBAR);
        return (LRESULT)hTBrush;
    }

    case WM_DROPFILES: {
        wchar_t path[MAX_PATH] = {};
        DragQueryFile((HDROP)wParam, 0, path, MAX_PATH);
        DragFinish((HDROP)wParam);
        DWORD attr = GetFileAttributes(path);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
            StartScan(path);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_BTN_OPEN: {
            wchar_t path[MAX_PATH] = {};
            BROWSEINFO bi{};
            bi.hwndOwner      = hwnd;
            bi.pszDisplayName = path;
            bi.lpszTitle      = L"Select folder to analyze";
            bi.ulFlags        = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            LPITEMIDLIST pidl = SHBrowseForFolder(&bi);
            if (pidl) { SHGetPathFromIDList(pidl, path); CoTaskMemFree(pidl); StartScan(path); }
            break;
        }
        case ID_BTN_UP: {
            if (!G.rootPath.empty()) {
                std::wstring p = G.rootPath;
                size_t pos = p.find_last_of(L"\\/");
                if (pos != std::wstring::npos && pos > 0) {
                    p = p.substr(0, pos);
                    if (p.size() == 2 && p[1] == L':') p += L'\\';
                    StartScan(p);
                }
            }
            break;
        }
        case ID_BTN_REFRESH:
            if (!G.rootPath.empty()) StartScan(G.rootPath);
            break;
        }
        return 0;

    case WM_SIZE: {
        RECT rc; GetClientRect(hwnd, &rc);
        SendMessage(G.hStatus, WM_SIZE, wParam, lParam);
        RECT sr; GetWindowRect(G.hStatus, &sr);
        int sh = sr.bottom - sr.top;
        MoveWindow(G.hPanel, 0, TOOLBAR_H, rc.right, rc.bottom - TOOLBAR_H - sh, TRUE);
        MoveWindow(G.hPathLabel, 308, 14, rc.right - 316, 20, TRUE);
        RECT tr = {0, 0, rc.right, TOOLBAR_H};
        InvalidateRect(hwnd, &tr, TRUE);
        return 0;
    }

    case WM_SCAN_DONE: {
        int cnt; ULONGLONG disk, cloud;
        { std::lock_guard<std::mutex> lk(G.mtx);
          cnt   = (int)G.entries.size();
          disk  = G.totalDisk;
          cloud = G.totalCloud; }
        wchar_t st[400];
        swprintf_s(st,
            L"Done: %d items  \x2502  \x25A0 На диске: %s  \x2502  \x2601 В облаке: %s"
            L"  \x2502  Double-click = войти в папку",
            cnt, SizeStr(disk).c_str(), SizeStr(cloud).c_str());
        SendMessage(G.hStatus, SB_SETTEXT, 0, (LPARAM)st);
        InvalidateRect(G.hPanel, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSEWHEEL:
        SendMessage(G.hPanel, WM_MOUSEWHEEL, wParam, lParam);
        return 0;

    case WM_DESTROY:
        G.doCancel = true;
        if (G.scanThread.joinable()) G.scanThread.join();
        if (G.hFont)     DeleteObject(G.hFont);
        if (G.hFontBold) DeleteObject(G.hFontBold);
        if (G.hFontMono) DeleteObject(G.hFontMono);
        if (G.hbmBuf)    DeleteObject(G.hbmBuf);
        if (G.hdcBuf)    DeleteDC(G.hdcBuf);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// --------------- Entry point ---------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow) {
    INITCOMMONCONTROLSEX icex = {sizeof(icex), ICC_BAR_CLASSES | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icex);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEX wc = {sizeof(wc)};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"FolderSizeViewer";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm       = LoadIcon(nullptr, IDI_APPLICATION);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    RegisterClassEx(&wc);

    HWND hwnd = CreateWindowEx(WS_EX_ACCEPTFILES,
        L"FolderSizeViewer", L"Folder Size Viewer  \x2014  Disk vs Cloud",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1150, 720,
        nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessage(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    CoUninitialize();
    return (int)m.wParam;
}
