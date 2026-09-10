#include "toast.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <string>
#include <string_view>

#include "utils.h"

namespace {

constexpr wchar_t kToastClassName[] = L"ChromePlus_SettingsToast";
constexpr int kToastHeight = 64;
constexpr UINT_PTR kCheckTimerId = 0x545354;  // 'TST'

HWND g_toast_hwnd = nullptr;
HWND g_owner_chrome_hwnd = nullptr;
bool g_dismissed_for_this_visit = false;
bool g_was_in_settings = false;

// Dynamic card dimensions (content-adaptive)
int g_toast_width = 240;

// Hover states
bool g_hover_close = false;
bool g_hover_link = false;
RECT g_link_rect = {};
RECT g_close_rect = {};

HWINEVENTHOOK g_title_hook = nullptr;
UINT_PTR g_ambient_timer_id = 0;

// Text definitions (strictly plain text, no decorative symbols, no bold)
constexpr wchar_t kTopText[] = L"[双击关闭标签页] 功能已就绪";
constexpr wchar_t kPrefixText[] = L"由 ";
constexpr wchar_t kLinkText[] = L"增进工坊";
constexpr wchar_t kSuffixText[] = L" 强力驱动";

bool IsDarkMode() {
  DWORD data = 1;
  DWORD size = sizeof(data);
  if (RegGetValueW(
          HKEY_CURRENT_USER,
          L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
          L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &data,
          &size) == ERROR_SUCCESS) {
    return data == 0;
  }
  return false;
}

bool IsSettingsPageTitle(std::wstring_view title) {
  // Exclude standalone DevTools windows first (e.g. "DevTools - ...", "Developer Tools - ...")
  if (title.find(L"DevTools") != std::wstring_view::npos ||
      title.find(L"Developer Tools") != std::wstring_view::npos ||
      title.find(L"chrome-devtools://") != std::wstring_view::npos) {
    return false;
  }

  // Chrome settings pages include:
  // "设置", "关于 Chrome", "关于 Chromium", "Settings", "About Chrome", "About Chromium"
  if (title.find(L"设置") != std::wstring_view::npos ||
      title.find(L"关于") != std::wstring_view::npos ||
      title.find(L"Settings") != std::wstring_view::npos ||
      title.find(L"About") != std::wstring_view::npos) {
    return true;
  }
  return false;
}

// Measures exact text dimensions using standard regular font to dynamically fit card width.
int CalculateAdaptiveWidth(HWND hwnd) {
  HDC hdc = GetDC(hwnd);
  if (!hdc) {
    return 240;
  }

  HFONT font = CreateFontW(
      -12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
  HFONT old_font = static_cast<HFONT>(SelectObject(hdc, font));

  SIZE top_sz = {};
  GetTextExtentPoint32W(hdc, kTopText, static_cast<int>(wcslen(kTopText)), &top_sz);

  SIZE pfx_sz = {}, lnk_sz = {}, sfx_sz = {};
  GetTextExtentPoint32W(hdc, kPrefixText, static_cast<int>(wcslen(kPrefixText)), &pfx_sz);
  GetTextExtentPoint32W(hdc, kLinkText, static_cast<int>(wcslen(kLinkText)), &lnk_sz);
  GetTextExtentPoint32W(hdc, kSuffixText, static_cast<int>(wcslen(kSuffixText)), &sfx_sz);

  SelectObject(hdc, old_font);
  DeleteObject(font);
  ReleaseDC(hwnd, hdc);

  const int line1_w = top_sz.cx + 28;  // text + gap + close button
  const int line2_w = pfx_sz.cx + lnk_sz.cx + sfx_sz.cx;

  // Margin: 16px left + 16px right padding
  const int target_w = (std::max)(line1_w, line2_w) + 32;
  return target_w;
}

// Locates the actual web contents viewport rect to avoid docking DevTools (right/bottom)
RECT GetSettingsContentArea(HWND owner) {
  RECT rcClient = {};
  GetClientRect(owner, &rcClient);

  // Search child windows for docked DevTools or web contents bounds if present
  // In Chromium, if DevTools is docked, inspecting child HWNDs or clipping provides safe margins
  struct DockContext {
    HWND owner;
    RECT content_rect;
    bool found_docked_devtools;
  } ctx = {owner, rcClient, false};

  EnumChildWindows(
      owner,
      [](HWND child, LPARAM lParam) -> BOOL {
        if (!IsWindowVisible(child)) {
          return TRUE;
        }

        auto* pCtx = reinterpret_cast<DockContext*>(lParam);
        wchar_t cls[64] = {};
        GetClassNameW(child, cls, static_cast<int>(std::size(cls)));

        // If a docked DevTools sub-window or side panel is detected
        if (wcscmp(cls, L"Chrome_WidgetWin_1") == 0 && child != pCtx->owner) {
          RECT r = {};
          GetWindowRect(child, &r);
          POINT pt_tl = {r.left, r.top};
          POINT pt_br = {r.right, r.bottom};
          ScreenToClient(pCtx->owner, &pt_tl);
          ScreenToClient(pCtx->owner, &pt_br);

          // If docked to the right side (occupying right portion)
          if (pt_tl.x > pCtx->content_rect.left + 200 &&
              pt_br.x >= pCtx->content_rect.right - 20) {
            pCtx->content_rect.right = (std::min)(pCtx->content_rect.right, static_cast<LONG>(pt_tl.x));
            pCtx->found_docked_devtools = true;
          }
          // If docked to the bottom (occupying bottom portion)
          else if (pt_tl.y > pCtx->content_rect.top + 150 &&
                   pt_br.y >= pCtx->content_rect.bottom - 20) {
            pCtx->content_rect.bottom = (std::min)(pCtx->content_rect.bottom, static_cast<LONG>(pt_tl.y));
            pCtx->found_docked_devtools = true;
          }
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&ctx));

  return ctx.content_rect;
}

void UpdateToastPosition(HWND toast, HWND owner) {
  if (!toast || !owner || !IsWindow(owner)) {
    return;
  }

  if (IsIconic(owner) || !IsWindowVisible(owner)) {
    ShowWindow(toast, SW_HIDE);
    return;
  }

  // Content-adaptive width calculation
  const int target_width = CalculateAdaptiveWidth(toast);
  if (target_width != g_toast_width) {
    g_toast_width = target_width;
    HRGN rgn = CreateRoundRectRgn(0, 0, g_toast_width + 1, kToastHeight + 1, 14, 14);
    SetWindowRgn(toast, rgn, TRUE);
  }

  // Obtain compatible viewport bounding rect (auto-accommodating docked DevTools)
  const RECT content_area = GetSettingsContentArea(owner);

  // Position at bottom-right of the actual settings content area with 20px margin
  POINT pt = {content_area.right - g_toast_width - 20,
              content_area.bottom - kToastHeight - 20};
  ClientToScreen(owner, &pt);

  RECT rcToast = {};
  GetWindowRect(toast, &rcToast);
  if (rcToast.left != pt.x || rcToast.top != pt.y ||
      (rcToast.right - rcToast.left) != g_toast_width ||
      (rcToast.bottom - rcToast.top) != kToastHeight ||
      !IsWindowVisible(toast)) {
    SetWindowPos(toast, HWND_TOP, pt.x, pt.y, g_toast_width, kToastHeight,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
  }
}

void PaintToast(HWND hwnd, HDC hdc) {
  RECT rc;
  GetClientRect(hwnd, &rc);

  const bool dark = IsDarkMode();
  const COLORREF bg_color = dark ? RGB(41, 42, 45) : RGB(255, 255, 255);
  const COLORREF border_color = dark ? RGB(68, 71, 75) : RGB(218, 220, 224);
  const COLORREF text_primary = dark ? RGB(232, 234, 237) : RGB(31, 31, 31);
  const COLORREF text_secondary = dark ? RGB(154, 160, 166) : RGB(95, 99, 104);
  const COLORREF link_color = dark ? RGB(138, 180, 248) : RGB(26, 115, 232);
  const COLORREF close_hover_bg = dark ? RGB(60, 64, 67) : RGB(241, 243, 244);

  // Double-buffering
  HDC mem_dc = CreateCompatibleDC(hdc);
  HBITMAP mem_bm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
  HBITMAP old_bm = static_cast<HBITMAP>(SelectObject(mem_dc, mem_bm));

  // 1. Draw card background
  HBRUSH bg_brush = CreateSolidBrush(bg_color);
  FillRect(mem_dc, &rc, bg_brush);
  DeleteObject(bg_brush);

  // 2. Draw rounded border
  HPEN border_pen = CreatePen(PS_SOLID, 1, border_color);
  HPEN old_pen = static_cast<HPEN>(SelectObject(mem_dc, border_pen));
  HBRUSH null_brush = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
  HBRUSH old_brush = static_cast<HBRUSH>(SelectObject(mem_dc, null_brush));
  RoundRect(mem_dc, rc.left, rc.top, rc.right, rc.bottom, 14, 14);
  SelectObject(mem_dc, old_brush);
  SelectObject(mem_dc, old_pen);
  DeleteObject(border_pen);

  SetBkMode(mem_dc, TRANSPARENT);

  // Standard regular font: FW_NORMAL, clean and without any bolding
  HFONT normal_font = CreateFontW(
      -12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
  HFONT old_font = static_cast<HFONT>(SelectObject(mem_dc, normal_font));

  // 3. Top Line: [双击关闭标签页] 功能已就绪
  SetTextColor(mem_dc, text_primary);
  RECT top_rc = {16, 12, rc.right - 32, 30};
  DrawTextW(mem_dc, kTopText, -1, &top_rc,
            DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

  // 4. Bottom Line: 由 增进工坊 强力驱动
  int x = 16;
  const int y = 35;

  // Prefix: "由 "
  SetTextColor(mem_dc, text_secondary);
  RECT prefix_rc = {x, y, x + 40, y + 18};
  DrawTextW(mem_dc, kPrefixText, -1, &prefix_rc,
            DT_CALCRECT | DT_SINGLELINE | DT_VCENTER);
  DrawTextW(mem_dc, kPrefixText, -1, &prefix_rc,
            DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
  x += (prefix_rc.right - prefix_rc.left);

  // Link: "增进工坊" (Regular weight, underlined only on hover)
  HFONT link_font = CreateFontW(
      -12, 0, 0, 0, FW_NORMAL, FALSE, g_hover_link ? TRUE : FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
  SelectObject(mem_dc, link_font);
  SetTextColor(mem_dc, link_color);

  RECT link_calc = {x, y, x + 80, y + 18};
  DrawTextW(mem_dc, kLinkText, -1, &link_calc,
            DT_CALCRECT | DT_SINGLELINE | DT_VCENTER);
  DrawTextW(mem_dc, kLinkText, -1, &link_calc,
            DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
  g_link_rect = link_calc;
  x += (link_calc.right - link_calc.left);

  // Suffix: " 强力驱动"
  SelectObject(mem_dc, normal_font);
  SetTextColor(mem_dc, text_secondary);
  RECT suffix_rc = {x, y, rc.right - 16, y + 18};
  DrawTextW(mem_dc, kSuffixText, -1, &suffix_rc,
            DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

  // 5. Close Button (✕)
  g_close_rect = {rc.right - 26, 10, rc.right - 10, 26};
  if (g_hover_close) {
    HBRUSH close_brush = CreateSolidBrush(close_hover_bg);
    HPEN close_pen = CreatePen(PS_SOLID, 1, border_color);
    HPEN old_cp = static_cast<HPEN>(SelectObject(mem_dc, close_pen));
    HBRUSH old_cb = static_cast<HBRUSH>(SelectObject(mem_dc, close_brush));
    RoundRect(mem_dc, g_close_rect.left, g_close_rect.top, g_close_rect.right,
              g_close_rect.bottom, 4, 4);
    SelectObject(mem_dc, old_cb);
    SelectObject(mem_dc, old_cp);
    DeleteObject(close_brush);
    DeleteObject(close_pen);
  }

  SetTextColor(mem_dc, text_secondary);
  DrawTextW(mem_dc, L"✕", -1, &g_close_rect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);

  // Blit to screen
  BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem_dc, 0, 0, SRCCOPY);

  // Cleanup
  SelectObject(mem_dc, old_font);
  DeleteObject(normal_font);
  DeleteObject(link_font);
  SelectObject(mem_dc, old_bm);
  DeleteObject(mem_bm);
  DeleteDC(mem_dc);
}

LRESULT CALLBACK ToastWndProc(HWND hwnd,
                              UINT msg,
                              WPARAM wParam,
                              LPARAM lParam) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      PaintToast(hwnd, hdc);
      EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_SETCURSOR: {
      POINT pt;
      GetCursorPos(&pt);
      ScreenToClient(hwnd, &pt);
      if (PtInRect(&g_close_rect, pt) || PtInRect(&g_link_rect, pt)) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
      }
      break;
    }

    case WM_MOUSEMOVE: {
      POINT pt = {LOWORD(lParam), HIWORD(lParam)};
      const bool hover_close = PtInRect(&g_close_rect, pt) != FALSE;
      const bool hover_link = PtInRect(&g_link_rect, pt) != FALSE;

      if (hover_close != g_hover_close || hover_link != g_hover_link) {
        g_hover_close = hover_close;
        g_hover_link = hover_link;
        InvalidateRect(hwnd, nullptr, FALSE);
      }

      TRACKMOUSEEVENT tme = {sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
      TrackMouseEvent(&tme);
      return 0;
    }

    case WM_MOUSELEAVE: {
      if (g_hover_close || g_hover_link) {
        g_hover_close = false;
        g_hover_link = false;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }

    case WM_LBUTTONUP: {
      POINT pt = {LOWORD(lParam), HIWORD(lParam)};
      if (PtInRect(&g_close_rect, pt)) {
        g_dismissed_for_this_visit = true;
        ShowWindow(hwnd, SW_HIDE);
        return 0;
      }
      if (PtInRect(&g_link_rect, pt)) {
        ShellExecuteW(nullptr, L"open", L"https://zengjin.work", nullptr,
                      nullptr, SW_SHOWNORMAL);
        return 0;
      }
      return 0;
    }

    case WM_TIMER: {
      if (wParam == kCheckTimerId && g_owner_chrome_hwnd) {
        UpdateSettingsToast(g_owner_chrome_hwnd);
      }
      return 0;
    }

    case WM_DESTROY: {
      KillTimer(hwnd, kCheckTimerId);
      g_toast_hwnd = nullptr;
      return 0;
    }

    default:
      break;
  }

  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND FindActiveChromeWindow() {
  HWND fg = GetForegroundWindow();
  if (fg) {
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) {
      wchar_t cls[64] = {};
      GetClassNameW(fg, cls, static_cast<int>(std::size(cls)));
      if (wcscmp(cls, L"Chrome_WidgetWin_1") == 0) {
        wchar_t title[256] = {};
        GetWindowTextW(fg, title, static_cast<int>(std::size(title)));
        if (IsSettingsPageTitle(title)) {
          return fg;
        }
      }
    }
  }

  HWND found = nullptr;
  EnumWindows(
      [](HWND hwnd, LPARAM lParam) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) &&
            !IsIconic(hwnd)) {
          wchar_t cls[64] = {};
          GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls)));
          if (wcscmp(cls, L"Chrome_WidgetWin_1") == 0) {
            wchar_t title[256] = {};
            GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
            if (IsSettingsPageTitle(title)) {
              *reinterpret_cast<HWND*>(lParam) = hwnd;
              return FALSE;
            }
          }
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&found));

  return found;
}

VOID CALLBACK AmbientTimerProc(HWND, UINT, UINT_PTR, DWORD) {
  HWND chrome_hwnd = FindActiveChromeWindow();
  if (chrome_hwnd) {
    UpdateSettingsToast(chrome_hwnd);
  } else if (g_toast_hwnd && IsWindow(g_toast_hwnd)) {
    ShowWindow(g_toast_hwnd, SW_HIDE);
  }
}

void CALLBACK WinEventProc(HWINEVENTHOOK,
                           DWORD event,
                           HWND hwnd,
                           LONG idObject,
                           LONG idChild,
                           DWORD,
                           DWORD) {
  if (event == EVENT_OBJECT_NAMECHANGE && idObject == OBJID_WINDOW &&
      idChild == INDEXID_CONTAINER && hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) {
      wchar_t cls[64] = {};
      GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls)));
      if (wcscmp(cls, L"Chrome_WidgetWin_1") == 0) {
        UpdateSettingsToast(hwnd);
      }
    }
  }
}

}  // namespace

void InitSettingsToast() {
  static bool registered = false;
  if (registered) {
    return;
  }

  WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
  wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
  wc.lpfnWndProc = ToastWndProc;
  wc.hInstance = hInstance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;
  wc.lpszClassName = kToastClassName;

  RegisterClassExW(&wc);
  registered = true;

  // Listen to title changes immediately
  g_title_hook = SetWinEventHook(
      EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE, nullptr, WinEventProc,
      GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);

  // Periodic check timer on main UI thread
  g_ambient_timer_id = SetTimer(nullptr, 0, 400, AmbientTimerProc);
}

void UpdateSettingsToast(HWND chrome_hwnd) {
  if (!chrome_hwnd || !IsWindow(chrome_hwnd)) {
    if (g_toast_hwnd && IsWindow(g_toast_hwnd)) {
      ShowWindow(g_toast_hwnd, SW_HIDE);
    }
    return;
  }

  // Read window title
  wchar_t title[256] = {};
  GetWindowTextW(chrome_hwnd, title, static_cast<int>(std::size(title)));
  const bool is_settings = IsSettingsPageTitle(title);

  if (is_settings) {
    if (!g_was_in_settings) {
      // Just entered settings page from elsewhere: reset dismiss flag!
      g_dismissed_for_this_visit = false;
      g_was_in_settings = true;
    }

    if (g_dismissed_for_this_visit) {
      // User closed the toast for this visit
      if (g_toast_hwnd && IsWindow(g_toast_hwnd)) {
        ShowWindow(g_toast_hwnd, SW_HIDE);
      }
      return;
    }

    // Ensure toast window exists
    if (!g_toast_hwnd || !IsWindow(g_toast_hwnd) ||
        g_owner_chrome_hwnd != chrome_hwnd) {
      if (g_toast_hwnd && IsWindow(g_toast_hwnd)) {
        DestroyWindow(g_toast_hwnd);
      }

      InitSettingsToast();
      g_owner_chrome_hwnd = chrome_hwnd;

      // Create owned popup tool window (never steals focus, never in taskbar)
      g_toast_hwnd = CreateWindowExW(
          WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kToastClassName,
          L"ChromePlusToast", WS_POPUP | WS_CLIPSIBLINGS, 0, 0, g_toast_width,
          kToastHeight, chrome_hwnd, nullptr, hInstance, nullptr);

      if (g_toast_hwnd) {
        HRGN rgn = CreateRoundRectRgn(0, 0, g_toast_width + 1, kToastHeight + 1,
                                      14, 14);
        SetWindowRgn(g_toast_hwnd, rgn, TRUE);
        SetTimer(g_toast_hwnd, kCheckTimerId, 400, nullptr);
      }
    }

    if (g_toast_hwnd) {
      UpdateToastPosition(g_toast_hwnd, chrome_hwnd);
    }
  } else {
    // Navigated away from settings
    g_was_in_settings = false;
    if (g_toast_hwnd && IsWindow(g_toast_hwnd)) {
      ShowWindow(g_toast_hwnd, SW_HIDE);
    }
  }
}
