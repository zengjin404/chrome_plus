#include "tabbookmark.h"

#include <windows.h>

#include "inputhook.h"
#include "uia.h"
#include "utils.h"

namespace {

POINT lbutton_down_point = {-1, -1};

// Double-click to close tab.
bool HandleDoubleClick(const MOUSEHOOKSTRUCT* pmouse) {
  const POINT pt = pmouse->pt;
  HWND hwnd = WindowFromPoint(pt);
  const auto hit = FindTabHitResult(pt, false, true);
  if (!hit || hit->on_close_button) {
    return false;
  }
  ExecuteCommand(IDC_CLOSE_TAB, hwnd);
  return true;
}

// Mouse handler for tab double click operation
bool TabBookmarkMouseHandler(WPARAM wParam, LPARAM lParam) {
  const auto* pmouse = reinterpret_cast<const MOUSEHOOKSTRUCT*>(lParam);

  static bool closing_tab_by_dblclk = false;
  static bool last_lbutton_down_on_tab = false;

  switch (wParam) {
    case WM_LBUTTONDOWN:
    case WM_NCLBUTTONDOWN:
      closing_tab_by_dblclk = false;
      last_lbutton_down_on_tab = false;
      if (wParam == WM_LBUTTONDOWN) {
        lbutton_down_point = pmouse->pt;
        const auto hit = FindTabHitResult(pmouse->pt, false, true);
        last_lbutton_down_on_tab = hit && !hit->on_close_button;
      }
      return false;

    case WM_LBUTTONUP:
      if (closing_tab_by_dblclk) {
        return true;
      }
      return false;

    case WM_NCLBUTTONUP:
      return closing_tab_by_dblclk;

    case WM_LBUTTONDBLCLK:
      if (closing_tab_by_dblclk) {
        return true;
      }
      if (last_lbutton_down_on_tab && HandleDoubleClick(pmouse)) {
        closing_tab_by_dblclk = true;
        return true;
      }
      return false;

    default:
      break;
  }

  return false;
}

}  // namespace

void TabBookmark() {
  RegisterMouseHandler(TabBookmarkMouseHandler, HandlerPriority::kNormal);
}
