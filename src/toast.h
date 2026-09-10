#ifndef CHROME_PLUS_SRC_TOAST_H_
#define CHROME_PLUS_SRC_TOAST_H_

#include <windows.h>

// Initializes the settings toast manager.
void InitSettingsToast();

// Checks and updates the toast state for the given Chrome window.
void UpdateSettingsToast(HWND chrome_hwnd);

#endif  // CHROME_PLUS_SRC_TOAST_H_
