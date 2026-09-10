#include <windows.h>

#include <psapi.h>

#include "detours.h"
#include "hijack.h"
#include "inputhook.h"
#include "tabbookmark.h"
#include "toast.h"
#include "utils.h"

using Startup = int (*)();
Startup ExeMain = nullptr;

void ChromePlus() {
  // Enhancement of the tab (double-click to close).
  TabBookmark();

  // Install input hooks.
  InstallInputHooks();

  // Initialize settings page toast notification.
  InitSettingsToast();
}

int Loader() {
  // Only hook into the main browser process, skip renderers, gpu, etc.
  LPWSTR param = GetCommandLineW();
  if (!wcsstr(param, L"-type=")) {
    ChromePlus();
  }

  // Return to the main function.
  return ExeMain();
}

void InstallLoader() {
  // Get the address of the original entry point of the main module.
  MODULEINFO mi;
  GetModuleInformation(GetCurrentProcess(), GetModuleHandle(nullptr), &mi,
                       sizeof(MODULEINFO));
  ExeMain = reinterpret_cast<Startup>(mi.EntryPoint);

  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  DetourAttach(reinterpret_cast<LPVOID*>(&ExeMain),
               reinterpret_cast<void*>(Loader));
  auto status = DetourTransactionCommit();
  if (status != NO_ERROR) {
    DebugLog(L"InstallLoader failed: {}", status);
  }
}

__declspec(dllexport) void portable() {}

BOOL WINAPI DllMain(HINSTANCE hModule, DWORD dwReason, LPVOID pv) {
  if (dwReason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hModule);
    hInstance = hModule;

    // Maintain the original function of system DLLs.
    LoadSysDll(hModule);

    InstallLoader();
  }
  return TRUE;
}
