/*
SoftTH, Software multihead solution for Direct3D
Copyright (C) 2005-2012 Keijo Ruotsalainen, www.kegetys.fi

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Win32 api hooks for SoftTH
#include "win32hooks.h"
#include "windows.h"
#include "helper.h"

#include "globalHooker.h"
#include "main.h"
#include "inputHandler.h"
//#include "d3dSoftTH.h" // Removed by CJR for SDK 8.1 - 9 Aug 2015

#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

GHOOK InitHooks[];
DWORD RealDisplayCount = 99;

// Sets source of call to module mod
#if 1
#define SOURCE_MODULE(mod) HMODULE mod = 0;GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)_ReturnAddress(), &mod);
#else
#define SOURCE_MODULE(mod) HMODULE mod = 0;{DWORD *aebp = 0;__asm {mov aebp, EBP};DWORD srcPtr = (DWORD) *(aebp+1);GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)srcPtr, &mod);};
#endif
static bool isHooked(HMODULE mod);
void* getHookCall(char *name);
extern HWND SoftTHPresentWindow;
extern volatile int FSXVirtualMonitorActive;
static bool fsxForegroundIsDialog();
static bool fsxWindowBelongsToThisProcess(HWND hWnd);
__declspec(thread) static bool fsxCoordinateHookGuard = false;

// FSX needs virtual cursor coordinates for its 5760x2160 render/UI hit testing,
// but native Win32 popup menus must still be positioned in real desktop pixels.
// Remember both forms of the most recent cursor query so TrackPopupMenu can
// translate only the popup anchor back to the physical desktop.
__declspec(thread) static bool fsxLastCursorWasVirtual = false;
__declspec(thread) static POINT fsxLastPhysicalCursor = {0,0};
__declspec(thread) static POINT fsxLastVirtualCursor = {0,0};

//#ifndef _WIN64
#if 1

BOOL WINAPI NewClipCursor(CONST RECT *rect)
{
  dbgf("hooksSoftTH: NewClipCursor");
  typedef BOOL (WINAPI*OCALL)(CONST RECT*);
  const static OCALL origFunc = (OCALL) getHookCall("ClipCursor");

  SOURCE_MODULE(srcMod);
  if(isHooked(srcMod))
  {
    return true;
  } else {
    return origFunc(rect);
  }
}

BOOL WINAPI NewEnumDisplaySettingsW(LPCTSTR lpszDeviceName, DWORD iModeNum, LPDEVMODE lpDevMode)
{
  dbgf("NewEnumDisplaySettingsW");
  // This will actually call NewEnumDisplaySettingsExA, but SOURCE_MODULE will fail there so must handle source specific stuff here!
	typedef BOOL (WINAPI*OCALL)(LPCTSTR, DWORD, LPDEVMODE);
	const static OCALL origFunc = (OCALL) getHookCall("EnumDisplaySettingsW");

  int ret = origFunc(lpszDeviceName, iModeNum, lpDevMode);
  lpszDeviceName = (char *) "SoftTH test W";
  SOURCE_MODULE(srcMod);
  if(isHooked(srcMod) && iModeNum == ENUM_CURRENT_SETTINGS && SoftTHActive)
  {
    lpDevMode->dmPelsWidth = config.main.renderResolution.x;
    lpDevMode->dmPelsHeight = config.main.renderResolution.y;
    dbg("EnumDisplaySettings: Returning mode %dx%d %dHz", lpDevMode->dmPelsWidth, lpDevMode->dmPelsHeight, lpDevMode->dmDisplayFrequency);
  }

  return ret;
}

BOOL WINAPI NewEnumDisplaySettingsA(LPCTSTR lpszDeviceName, DWORD iModeNum, LPDEVMODE lpDevMode)
{
  dbgf("NewEnumDisplaySettingsA");
  // This will actually call NewEnumDisplaySettingsExA, but SOURCE_MODULE will fail there so must handle source specific stuff here!
	typedef BOOL (WINAPI*OCALL)(LPCTSTR, DWORD, LPDEVMODE);
	const static OCALL origFunc = (OCALL) getHookCall("EnumDisplaySettingsA");

  int ret = origFunc(lpszDeviceName, iModeNum, lpDevMode);
  lpszDeviceName = (char *) "SoftTH test A";
  SOURCE_MODULE(srcMod);
  if(isHooked(srcMod) && iModeNum == ENUM_CURRENT_SETTINGS && SoftTHActive)
  {
    lpDevMode->dmPelsWidth = config.main.renderResolution.x;
    lpDevMode->dmPelsHeight = config.main.renderResolution.y;
    dbg("EnumDisplaySettings: Returning mode %dx%d %dHz", lpDevMode->dmPelsWidth, lpDevMode->dmPelsHeight, lpDevMode->dmDisplayFrequency);
  }

  return ret;
}

BOOL WINAPI NewEnumDisplaySettingsExW(LPCWSTR lpszDeviceName, DWORD iModeNum, LPDEVMODEW lpDevMode, DWORD dwFlags)
{
  dbgf("NewEnumDisplaySettingsExW");
	typedef BOOL (WINAPI*OCALL)(LPCWSTR, DWORD, LPDEVMODEW, DWORD);
	const static OCALL origFunc = (OCALL) getHookCall("EnumDisplaySettingsExW");

  int ret = origFunc(lpszDeviceName, iModeNum, lpDevMode, dwFlags);
  lpszDeviceName = (WCHAR *) L"SoftTH test ExW";

  SOURCE_MODULE(srcMod);
  if(isHooked(srcMod) && iModeNum == ENUM_CURRENT_SETTINGS && SoftTHActive)
  {
    lpDevMode->dmPelsWidth = config.main.renderResolution.x;
    lpDevMode->dmPelsHeight = config.main.renderResolution.y;
  }

  if(iModeNum != ENUM_REGISTRY_SETTINGS && iModeNum != ENUM_CURRENT_SETTINGS && iModeNum > 2)
  {
    static DEVMODEW mode;
    mode.dmSize = sizeof(DEVMODEW);
    int rr = origFunc(lpszDeviceName, iModeNum-1, &mode, dwFlags);
    if(rr && !ret)
    {
      // Next to last mode is queried - this is going to be our mode
      origFunc(lpszDeviceName, ENUM_CURRENT_SETTINGS, &mode, dwFlags);
      mode.dmPelsWidth = config.main.renderResolution.x;
      mode.dmPelsHeight = config.main.renderResolution.y;
      memcpy(lpDevMode, &mode, sizeof(DEVMODEW));
      dbg("EnumDisplaySettingsEx: Added mode ID %d: %dx%d %dHz", iModeNum, mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency);
      return rr;
    }
  }
  return ret;
}

BOOL WINAPI NewEnumDisplaySettingsExA(LPCTSTR lpszDeviceName, DWORD iModeNum, LPDEVMODEA lpDevMode, DWORD dwFlags)
{
  dbgf("NewEnumDisplaySettingsExA");
	typedef BOOL (WINAPI*OCALL)(LPCTSTR, DWORD, LPDEVMODEA, DWORD);
	const static OCALL origFunc = (OCALL) getHookCall("EnumDisplaySettingsExA");

  int ret = origFunc(lpszDeviceName, iModeNum, lpDevMode, dwFlags);
  lpszDeviceName = (char *) "SoftTH test ExA";

  SOURCE_MODULE(srcMod);
  if(isHooked(srcMod) && iModeNum == ENUM_CURRENT_SETTINGS && SoftTHActive)
  {
    lpDevMode->dmPelsWidth = config.main.renderResolution.x;
    lpDevMode->dmPelsHeight = config.main.renderResolution.y;
  }

  if(iModeNum != ENUM_REGISTRY_SETTINGS && iModeNum != ENUM_CURRENT_SETTINGS && iModeNum > 2)
  {
    static DEVMODEA mode;
    mode.dmSize = sizeof(DEVMODEA);
    int rr = origFunc(lpszDeviceName, iModeNum-1, &mode, dwFlags);
    if(rr && !ret)
    {
      // Next to last mode is queried - this is going to be our mode
      origFunc(lpszDeviceName, ENUM_CURRENT_SETTINGS, &mode, dwFlags);
      mode.dmPelsWidth = config.main.renderResolution.x;
      mode.dmPelsHeight = config.main.renderResolution.y;
      memcpy(lpDevMode, &mode, sizeof(DEVMODEA));
      dbg("EnumDisplaySettingsEx: Added mode ID %d: %dx%d %dHz", iModeNum, mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency);
      return rr;
    }
  }
  return ret;
}

LONG WINAPI NewChangeDisplaySettingsA(LPDEVMODE lpDevMode, DWORD dwflags)
{
  dbgf("!! ChangeDisplaySettingsA %dx%d %d", lpDevMode->dmPelsWidth, lpDevMode->dmPelsHeight, dwflags);
  // This will actually call NewChangeDisplaySettingsExA, but SOURCE_MODULE will fail there so must handle source specific stuff here!
	typedef BOOL (WINAPI*OCALL)(LPDEVMODE, DWORD);
	const static OCALL origFunc = (OCALL) getHookCall("ChangeDisplaySettingsA");

	LONG ret = origFunc(lpDevMode, dwflags);
	return ret;
}

LONG WINAPI NewChangeDisplaySettingsExA(LPCTSTR lpszDeviceName, LPDEVMODE lpDevMode, HWND hwnd, DWORD dwflags, LPVOID lParam)
{
  dbgf("ChangeDisplaySettingsExA %dx%d %d", lpDevMode->dmPelsWidth, lpDevMode->dmPelsHeight, dwflags);
	typedef BOOL (WINAPI*OCALL)(LPCTSTR, LPDEVMODE, HWND, DWORD, LPVOID);
	const static OCALL origFunc = (OCALL) getHookCall("ChangeDisplaySettingsExA");

  SOURCE_MODULE(srcMod);
  if(lpDevMode)
  {
    if(lpDevMode->dmPelsWidth == config.main.renderResolution.x && lpDevMode->dmPelsHeight == config.main.renderResolution.y) {
      HEAD *h = config.getPrimaryHead();
      dbg("ChangeDisplaySettingsExA: Override resolution %dx%d %dHz -> %dx%d", lpDevMode->dmPelsWidth, lpDevMode->dmPelsHeight, lpDevMode->dmDisplayFrequency, h->screenMode.x, h->screenMode.y);
      lpDevMode->dmPelsWidth = h->screenMode.x;
      lpDevMode->dmPelsHeight = h->screenMode.y;
    } else {
      dbg("ChangeDisplaySettingsExA: %dx%d %dHz", lpDevMode->dmPelsWidth, lpDevMode->dmPelsHeight, lpDevMode->dmDisplayFrequency);
    }
  }

	LONG ret = origFunc(lpszDeviceName, lpDevMode, hwnd, dwflags, lParam);
	return ret;
}

static void pointToVirtual(POINT *point)
{
  // No coordinate remapping is required when there are no additional heads.
  if(config.getNumAdditionalHeads() == 0)
    return;

  dbgf("hooksSoftTH: pointToVirtual");

  POINT physical = *point;
  POINT vp; // Virtual bb coordinates
  if(!inputMapScreenToVirtual(&physical, &vp))
    return; // Outside visible SoftTH heads

  point->x = vp.x;
  point->y = vp.y;
  return;
}

BOOL WINAPI NewLogicalToPhysicalPoint(HWND hWnd, LPPOINT point)
{
  dbgf("hooksSoftTH: NewLogicalToPhysicalPoint");
	typedef BOOL (WINAPI*OCALL)(HWND, POINT *p);
	const static OCALL origFunc = (OCALL) getHookCall("LogicalToPhysicalPoint");

  int x = point->x;
  int y = point->y;
  BOOL ret = origFunc(hWnd, point);

  SOURCE_MODULE(srcMod);
  dbg_input("NewLogicalToPhysicalPoint: %d - %dx%d -> %dx%d (%d)", hWnd, x, y, point->x, point->y, ret);
  if(isHooked(srcMod) && SoftTHActive)
  {
    // hack: pretend everything is fine
    // TODO: support proper coordinate conversion
    point->x = x;
    point->y = y;
    return 1;
  }
  return ret;
}

BOOL WINAPI NewGetPhysicalCursorPos(LPPOINT point)
{
  dbgf("hooksSoftTH: NewGetPhysicalCursorPos");
	typedef BOOL (WINAPI*OCALL)(POINT *p);
	const static OCALL origFunc = (OCALL) getHookCall("GetPhysicalCursorPos");

  BOOL ret = origFunc(point);

  SOURCE_MODULE(srcMod);

  //dbg_input("NewGetPhysicalCursorPos %dx%d", point->x, point->y);
  if(isHooked(srcMod) && SoftTHActive)
  {
    // Find window under cursor, map to backbuffer coordinate
    pointToVirtual(point);
    return ret;
  }

  return ret;
}

BOOL WINAPI NewGetCursorPos(LPPOINT point)
{
  dbgf("hooksSoftTH: NewGetCursorPos");
	typedef BOOL (WINAPI*OCALL)(POINT *p);
	const static OCALL origFunc = (OCALL) getHookCall("GetCursorPos");
  BOOL ret = origFunc(point);

  // hack: Hook also setCursorPos from system32/d3d9.dll since some cursor manipulation is done there
  extern HINSTANCE hLibD3D9; // in main.cpp

  SOURCE_MODULE(srcMod);

  dbgf("NewGetCursorPos from %s", getModuleName(srcMod));
  fsxLastCursorWasVirtual = false;
  if((isHooked(srcMod) || srcMod == hLibD3D9) && SoftTHActive)
  {
    POINT physical = *point;
    POINT vp = physical;
    if((FSXVirtualMonitorActive ?
        inputMapPhysicalCursorToVirtual(&physical, &vp) :
        inputMapScreenToVirtual(&physical, &vp)))
    {
      fsxLastPhysicalCursor = physical;
      fsxLastVirtualCursor = vp;
      fsxLastCursorWasVirtual = true;
      *point = vp;
    }
    return ret;
  }
  return ret;
}

static void fsxPopupPointToPhysical(HWND hWnd, int *x, int *y)
{
  if(!x || !y || !SoftTHActive || !FSXVirtualMonitorActive ||
     !fsxWindowBelongsToThisProcess(hWnd))
    return;

  // Native menus must be anchored in real desktop pixels.  Match the
  // virtual cursor only when the menu is being opened at the mouse;
  // leave intentionally positioned FSX menus completely untouched.
  typedef BOOL (WINAPI* GETCURSORPOS_REAL)(LPPOINT);
  const static GETCURSORPOS_REAL realCursor =
      (GETCURSORPOS_REAL)getHookCall("GetCursorPos");
  POINT physical = {0,0};
  POINT vp = {0,0};
  if(realCursor && realCursor(&physical) &&
     inputMapPhysicalCursorToVirtual(&physical, &vp) &&
     abs(*x-vp.x) <= 8 && abs(*y-vp.y) <= 8)
  {
    static int logged = 0;
    if(logged < 16) {
      dbg("FSX popup anchor: virtual=%d,%d -> physical=%d,%d",
          *x,*y,physical.x,physical.y);
      logged++;
    }
    *x = physical.x;
    *y = physical.y;
  }
}


BOOL WINAPI NewTrackPopupMenu(HMENU hMenu, UINT uFlags, int x, int y,
                              int nReserved, HWND hWnd, const RECT *prcRect)
{
  typedef BOOL (WINAPI*OCALL)(HMENU, UINT, int, int, int, HWND, const RECT *);
  const static OCALL origFunc = (OCALL) getHookCall("TrackPopupMenu");
  fsxPopupPointToPhysical(hWnd, &x, &y);
  return origFunc(hMenu, uFlags, x, y, nReserved, hWnd, prcRect);
}

BOOL WINAPI NewTrackPopupMenuEx(HMENU hMenu, UINT uFlags, int x, int y,
                                HWND hWnd, LPTPMPARAMS lptpm)
{
  typedef BOOL (WINAPI*OCALL)(HMENU, UINT, int, int, HWND, LPTPMPARAMS);
  const static OCALL origFunc = (OCALL) getHookCall("TrackPopupMenuEx");
  fsxPopupPointToPhysical(hWnd, &x, &y);
  return origFunc(hMenu, uFlags, x, y, hWnd, lptpm);
}

BOOL WINAPI NewGetCursorInfo(CURSORINFO *pci)
{
  dbgf("hooksSoftTH: NewGetCursorInfo");
	typedef BOOL (WINAPI*OCALL)(CURSORINFO *p);
	const static OCALL origFunc = (OCALL) getHookCall("GetCursorInfo");
  BOOL ret = origFunc(pci);

  SOURCE_MODULE(srcMod);
  if((isHooked(srcMod)) && SoftTHActive)
  {
    pointToVirtual(&pci->ptScreenPos);
    return ret;
  }

  return ret;
}

BOOL WINAPI NewSetPhysicalCursorPos(int x, int y)
{
  dbgf("hooksSoftTH: NewSetPhysicalCursorPos");
	typedef BOOL (WINAPI*OCALL)(int, int);
	const static OCALL origFunc = (OCALL) getHookCall("SetPhysicalCursorPos");

  if(!SoftTHActive || config.getNumAdditionalHeads() == 0)
    return origFunc(x, y);

  SOURCE_MODULE(srcMod);
  POINT in = {x, y};
  POINT out = {-1, -1};
  if(inputMapVirtualToDesktop(&in, &out)) {
    dbg_input("SetPhysicalCursorPos %dx%d -> %dx%d (from %s)", x, y, out.x, out.y, getModuleName(srcMod));
    return origFunc(out.x, out.y);
  } else {
    // Outside visible monitors
    dbg_input("SetPhysicalCursorPos %dx%d -> DISCARD", x, y);
    return true;
  }
	return true;
}

BOOL WINAPI NewSetCursorPos(int x, int y)
{
  dbgf("hooksSoftTH: NewSetCursorPos");
	typedef BOOL (WINAPI*OCALL)(int, int);
	const static OCALL origFunc = (OCALL) getHookCall("SetCursorPos");

  if(!SoftTHActive || config.getNumAdditionalHeads() == 0)
    return origFunc(x, y);

  SOURCE_MODULE(srcMod);
  POINT in = {x, y};
  POINT out = {-1, -1};
  if(inputMapVirtualToDesktop(&in, &out)) {
    dbg_input("SetCursorPos %dx%d -> %dx%d (from %s)", x, y, out.x, out.y, getModuleName(srcMod));
    return origFunc(out.x, out.y);
  } else {
    // Outside visible monitors
    dbg_input("SetCursorPos %dx%d -> DISCARD", x, y);
    return true;
  }
	return true;
}

BOOL WINAPI NewClientToScreen(HWND hWnd, LPPOINT lpPoint)
{
  dbgf("hooksSoftTH: NewClientToScreen");
	typedef BOOL (WINAPI*OCALL)(HWND, LPPOINT);
	const static OCALL origFunc = (OCALL) getHookCall("ClientToScreen");

  if(!lpPoint || fsxCoordinateHookGuard ||
     !SoftTHActive || !FSXVirtualMonitorActive || fsxForegroundIsDialog() ||
     !fsxWindowBelongsToThisProcess(hWnd))
    return origFunc(hWnd, lpPoint);

  // FSX fullscreen client coordinates are virtual-backbuffer coordinates.
  // Convert them back to the real desktop before Win32 positions a popup,
  // tooltip, cursor, etc.  This is the inverse of NewScreenToClient below.
  POINT vin = *lpPoint;
  POINT physical = {-1,-1};
  if(vin.x >= 0 && vin.y >= 0 &&
     vin.x < config.main.renderResolution.x &&
     vin.y < config.main.renderResolution.y)
  {
    fsxCoordinateHookGuard = true;
    bool mapped = inputMapVirtualToDesktop(&vin, &physical);
    fsxCoordinateHookGuard = false;
    if(mapped)
    {
      static int logged = 0;
      if(logged < 16) {
        dbg("FSX ClientToScreen virtual->desktop hwnd=0x%08X %d,%d -> %d,%d",
            hWnd, vin.x, vin.y, physical.x, physical.y);
        logged++;
      }
      *lpPoint = physical;
      return TRUE;
    }
  }

  return origFunc(hWnd, lpPoint);
}

BOOL WINAPI NewScreenToClient(HWND hWnd, LPPOINT lpPoint)
{
  dbgf("hooksSoftTH: NewScreenToClient");
	typedef BOOL (WINAPI*OCALL)(HWND, LPPOINT);
	const static OCALL origFunc = (OCALL) getHookCall("ScreenToClient");

  if(!lpPoint || fsxCoordinateHookGuard ||
     !SoftTHActive || !FSXVirtualMonitorActive || fsxForegroundIsDialog() ||
     !fsxWindowBelongsToThisProcess(hWnd))
    return origFunc(hWnd, lpPoint);

  // Preserve the real desktop point and map it directly through the SoftTH
  // head rectangles to the 5760x2160 virtual canvas.  FSX uses this path for
  // VC mouse hit-testing on modern Windows.
  POINT screen = *lpPoint;
  POINT vp = {-1,-1};
  fsxCoordinateHookGuard = true;
  // FSX uses ScreenToClient for non-mouse layout operations as well.
  // Use new physical mapping only for a point at the ACTUAL live cursor.
  // Every other call retains the exact v3.47 conversion/layout behavior.
  typedef BOOL (WINAPI* GETCURSORPOS_REAL)(LPPOINT);
  const static GETCURSORPOS_REAL realCursor =
      (GETCURSORPOS_REAL)getHookCall("GetCursorPos");
  POINT liveCursor = {0,0};
  bool liveMousePoint =
      realCursor && realCursor(&liveCursor) &&
      abs(screen.x-liveCursor.x) <= 2 &&
      abs(screen.y-liveCursor.y) <= 2;
  bool mapped = false;
  if(liveMousePoint)
    mapped = inputMapPhysicalCursorToVirtual(&screen, &vp);
  if(!mapped)
    mapped = inputMapScreenToVirtual(&screen, &vp);
  fsxCoordinateHookGuard = false;
  if(mapped)
  {
    static int logged = 0;
    if(logged < 16) {
      dbg("FSX ScreenToClient desktop->virtual hwnd=0x%08X %d,%d -> %d,%d",
          hWnd, screen.x, screen.y, vp.x, vp.y);
      logged++;
    }
    *lpPoint = vp;
    return TRUE;
  }

  return origFunc(hWnd, lpPoint);
}

int WINAPI NewGetSystemMetrics(int nIndex)
{
  typedef int (WINAPI*OCALL)(int);
  const static OCALL origFunc = (OCALL) getHookCall("GetSystemMetrics");
  int ret = origFunc(nIndex);

  SOURCE_MODULE(srcMod);
  if(isHooked(srcMod) && SoftTHActive)
  {
    int v = ret;
    bool overrideMetric = true;
    switch(nIndex)
    {
      case SM_CXSCREEN:
      case SM_CXVIRTUALSCREEN:
        v = config.main.renderResolution.x;
        break;
      case SM_CYSCREEN:
      case SM_CYVIRTUALSCREEN:
        v = config.main.renderResolution.y;
        break;
      case SM_XVIRTUALSCREEN:
      case SM_YVIRTUALSCREEN:
        v = 0;
        break;
      case SM_CMONITORS:
        v = 1;
        break;
      default:
        overrideMetric = false;
        break;
    }

    if(overrideMetric)
    {
      static bool logged[100] = {false};
      int slot = (nIndex >= 0 && nIndex < 100) ? nIndex : 99;
      if(!logged[slot])
      {
        dbg("FSX GetSystemMetrics: index=%d physical=%d virtual=%d caller=%s",
            nIndex, ret, v, getModuleName(srcMod));
        logged[slot] = true;
      }
      return v;
    }
  }
  return ret;
}

BOOL WINAPI NewGetWindowRect(HWND win, LPRECT rect)
{
  dbgf("hooksSoftTH: NewGetWindowRect");
	typedef BOOL (WINAPI*OCALL)(HWND, LPRECT);
	const static OCALL origFunc = (OCALL) getHookCall("GetWindowRect");

	BOOL ret = origFunc(win, rect);
  SOURCE_MODULE(srcMod);
  if(SoftTHActive && FSXVirtualMonitorActive && !fsxForegroundIsDialog() &&
     (inputMapIsDeviceWindow(win) || win == SoftTHPresentWindow))
  {
    static bool loggedWindowRectOverride = false;
    static bool loggedPresentWindowRectOverride = false;
    if(inputMapIsDeviceWindow(win) && !loggedWindowRectOverride) {
      dbg("FSX fullscreen: forcing main GetWindowRect to virtual %dx%d (caller %s)",
          config.main.renderResolution.x, config.main.renderResolution.y,
          getModuleName(srcMod));
      loggedWindowRectOverride = true;
    }
    if(win == SoftTHPresentWindow && !loggedPresentWindowRectOverride) {
      dbg("FSX fullscreen: forcing present-child GetWindowRect to virtual %dx%d (caller %s)",
          config.main.renderResolution.x, config.main.renderResolution.y,
          getModuleName(srcMod));
      loggedPresentWindowRectOverride = true;
    }
    rect->right = rect->left + config.main.renderResolution.x;
    rect->bottom = rect->top + config.main.renderResolution.y;
  }
  if(SoftTHActive && inputMapIsDeviceWindow(win) &&
     (!FSXVirtualMonitorActive || fsxForegroundIsDialog())) {
    static bool loggedPhysicalWindow=false;
    if(!loggedPhysicalWindow) {
      dbg("FSX UI/dialog: GetWindowRect left physical");
      loggedPhysicalWindow=true;
    }
  }
	return ret;
}

BOOL WINAPI NewGetClientRect(HWND win, LPRECT rect)
{
  dbgf("hooksSoftTH: NewGetClientRect");
	typedef BOOL (WINAPI*OCALL)(HWND, LPRECT);
	const static OCALL origFunc = (OCALL) getHookCall("GetClientRect");
	BOOL ret = origFunc(win, rect);
  SOURCE_MODULE(srcMod);
  dbgf("NewGetClientRect from %s", getModuleName(srcMod));
  if(SoftTHActive && FSXVirtualMonitorActive && !fsxForegroundIsDialog() &&
     (inputMapIsDeviceWindow(win) || win == SoftTHPresentWindow))
  {
    static bool loggedClientRectOverride = false;
    static bool loggedPresentClientRectOverride = false;
    if(inputMapIsDeviceWindow(win) && !loggedClientRectOverride) {
      dbg("FSX fullscreen: forcing main GetClientRect to virtual %dx%d (caller %s)",
          config.main.renderResolution.x, config.main.renderResolution.y,
          getModuleName(srcMod));
      loggedClientRectOverride = true;
    }
    if(win == SoftTHPresentWindow && !loggedPresentClientRectOverride) {
      dbg("FSX fullscreen: forcing present-child GetClientRect to virtual %dx%d (caller %s)",
          config.main.renderResolution.x, config.main.renderResolution.y,
          getModuleName(srcMod));
      loggedPresentClientRectOverride = true;
    }
    rect->left = 0;
    rect->top = 0;
    rect->right = config.main.renderResolution.x;
    rect->bottom = config.main.renderResolution.y;
  }
  if(SoftTHActive && inputMapIsDeviceWindow(win) &&
     (!FSXVirtualMonitorActive || fsxForegroundIsDialog())) {
    static bool loggedPhysicalClient=false;
    if(!loggedPhysicalClient) {
      dbg("FSX UI/dialog: GetClientRect left physical");
      loggedPhysicalClient=true;
    }
  }
	return ret;
}

BOOL WINAPI nil_debug() {
  dbg("-- nil_debug --");
  return 0;
}
/*
BOOL WINAPI Newmouse_event(DWORD dwFlags, DWORD dx, DWORD dy, DWORD dwData, ULONG_PTR dwExtraInfo) {
	typedef BOOL (WINAPI*OCALL)(DWORD, DWORD, DWORD, DWORD, ULONG_PTR);
	const static OCALL origFunc = (OCALL) getHookCall("mouse_event");

  SOURCE_MODULE(srcMod);
  dbg("mouse_event from %s", getModuleName(srcMod));

	BOOL ret = origFunc(dwFlags, dx, dy, dwData, dwExtraInfo);
  return ret;
}

BOOL WINAPI NewSendInput(UINT nInputs, LPINPUT inputs, int cbSize) {
	typedef BOOL (WINAPI*OCALL)(UINT, LPINPUT, int);
	const static OCALL origFunc = (OCALL) getHookCall("SendInput");

  SOURCE_MODULE(srcMod);
  dbg("SendInput from %s", getModuleName(srcMod));

	BOOL ret = origFunc(nInputs, inputs, cbSize);
  return ret;
}


BOOL WINAPI NewSendMessageA(HWND win, UINT Msg, WPARAM wparam, LPARAM lparam) {
	typedef BOOL (WINAPI*OCALL)(HWND, UINT, WPARAM, LPARAM);

  SOURCE_MODULE(srcMod);
  dbg("SendMessage: %s from %s", getMouseEventName(Msg), getModuleName(srcMod));

	const static OCALL origFunc = (OCALL) getHookCall("SendMessageA");
  BOOL ret = origFunc(win, Msg, wparam, lparam);
  return ret;
}

BOOL WINAPI NewPostMessageA(HWND win, UINT Msg, WPARAM wparam, LPARAM lparam) {
	typedef BOOL (WINAPI*OCALL)(HWND, UINT, WPARAM, LPARAM);

  SOURCE_MODULE(srcMod);
  dbg("PostMessage: %s from %s", getMouseEventName(Msg), getModuleName(srcMod));

	const static OCALL origFunc = (OCALL) getHookCall("PostMessageA");
  BOOL ret = origFunc(win, Msg, wparam, lparam);
  return ret;
}
*/
/*
BOOL WINAPI NewMapWindowPoints(HWND hWndFrom, HWND hWndTo, LPPOINT lpPoints, UINT cPoints) {
	typedef BOOL (WINAPI*OCALL)(HWND, HWND, LPPOINT, UINT);
	const static OCALL origFunc = (OCALL) getHookCall("MapWindowPoints");
  dbg_input("NewMapWindowPoints: %d - %dx%d", cPoints, lpPoints->x, lpPoints->y);
	BOOL ret = origFunc(hWndFrom, hWndTo, lpPoints, cPoints);
  SOURCE_MODULE(srcMod);
  //dbg("NewGetClientRect from %s", getModuleName(srcMod));
  if(isHooked(srcMod) && SoftTHActive)
  {
    if(inputMapIsDeviceWindow(win)) {
      dbg_input("NewMapWindowPoints: %dx%d - %dx%d OVERRIDE", rect->left, rect->top, rect->right, rect->bottom);
      rect->right = config.main.renderResolution.x;
      rect->bottom = config.main.renderResolution.y;
    }
  }
	return ret;
}*/



BOOL WINAPI NewSetWindowPos(HWND win, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags)
{
	typedef BOOL (WINAPI*OCALL)(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags);
	const static OCALL origFunc = (OCALL) getHookCall("SetWindowPos");
  SOURCE_MODULE(srcMod);
  if(isHooked(srcMod) && SoftTHActive)
  {
    if(inputMapIsDeviceWindow(win)) {
      // Keep the physical main device window on the primary monitor.
      HEAD *h = config.getPrimaryHead();
      int ncx = h->screenMode.x;
      int ncy = h->screenMode.y;
      dbg_input("NewSetWindowPos: %dx%d -> %dx%d", cx, cy, ncx, ncy);
      cx = ncx;
      cy = ncy;
      X = Y = 0;
    } else if(win == SoftTHPresentWindow) {
      // The FSX render child must expose the virtual canvas size so FSX
      // does not keep rebuilding its scene at the primary monitor size.
      cx = config.main.renderResolution.x;
      cy = config.main.renderResolution.y;
      static bool loggedPresentResize = false;
      if(!loggedPresentResize) {
        dbg("FSX fullscreen: forcing present-child SetWindowPos size to %dx%d",
            cx, cy);
        loggedPresentResize = true;
      }
    }
  }

  BOOL ret = origFunc(win, hWndInsertAfter, X, Y, cx, cy, uFlags);
  return ret;
}


HMODULE WINAPI NewGetModuleHandleA(LPCSTR lpModuleName)
{
  typedef HMODULE (WINAPI*OCALL)(LPCSTR);
  const static OCALL origFunc = (OCALL) &InitHooks[0].stubCall;

  SOURCE_MODULE(srcMod);
  if(isHooked(srcMod) && lpModuleName && (!_strcmpi(lpModuleName, "d3d9.dll") || !_strcmpi(lpModuleName, "d3d9")))
  {
    SOURCE_MODULE(srcMod);
    extern char hLibD3D9_path[256]; // in main.cpp
    return origFunc(hLibD3D9_path);
  }
  return origFunc(lpModuleName);
}


HMODULE WINAPI NewGetModuleHandleW(LPCWSTR lpModuleName)
{
  typedef HMODULE (WINAPI*OCALL)(LPCWSTR);
  const static OCALL origFunc = (OCALL) &InitHooks[1].stubCall;

  SOURCE_MODULE(srcMod);
  char fn[256];
  GetModuleFileName(srcMod, fn, 256);
  OutputDebugString(fn);
  if(isHooked(srcMod) && lpModuleName)
  {
    char *name = new char[256];
    WideCharToMultiByte(CP_ACP, 0, lpModuleName, -1, name, 256, NULL, NULL);
    OutputDebugString(name);
    if(!_strcmpi(name, "d3d9.dll") || !_strcmpi(name, "d3d9"))
    {
      OutputDebugString("wide hit");
      extern char hLibD3D9_path[256]; // in main.cpp

      typedef HMODULE (WINAPI*OACALL)(LPCSTR);
      const static OACALL origFuncAscii = (OACALL) &InitHooks[0].stubCall;
      return origFuncAscii(hLibD3D9_path);
    }
  }
  return origFunc(lpModuleName);
}


FARPROC WINAPI NewGetProcAddress(HMODULE hModule, LPCSTR lpProcName)
{
  typedef FARPROC (WINAPI*OCALL)(HMODULE hModule, LPCSTR lpProcName);
  const static OCALL origFunc = (OCALL) getHookCall("GetProcAddress");

  SOURCE_MODULE(srcMod);
  char fn[256];
  GetModuleFileName(srcMod, fn, 256);
  if(isHooked(srcMod) || true)
  {
    if(HIWORD(lpProcName))
    {
      // If application is trying to hook D3D, point it to the real DLL instead of SoftTH dll
      bool doDirect = false;
      if(!strcmp(lpProcName, "Direct3DCreate9")) doDirect = true;
      if(!strcmp(lpProcName, "Direct3DCreate9Ex")) doDirect = true;

      extern HINSTANCE hLibD3D9; // in main.cpp
      if(doDirect && hLibD3D9)
      {
        OutputDebugString(fn);
        OutputDebugString(lpProcName);
        return origFunc(hLibD3D9, lpProcName);
      }
    }
    //dbg("GETPROCADDRESS <%s> from <%s>", lpProcName, fn);
  }

  return origFunc(hModule, lpProcName);
}

static bool fsxMonitorVirtualizationCaller(HMODULE mod)
{
  if(!mod) return false;
  char path[MAX_PATH]={0};
  if(!GetModuleFileNameA(mod,path,MAX_PATH))
    return false;
  const char *base=strrchr(path,'\\');
  base=base ? base+1 : path;
  return _stricmp(base,"g2d.dll")==0;
}

static bool fsxForegroundIsDialog()
{
  HWND fg=GetForegroundWindow();
  if(!fg) return false;
  DWORD pid=0;
  GetWindowThreadProcessId(fg,&pid);
  if(pid != GetCurrentProcessId())
    return false;
  char cls[64]={0};
  GetClassNameA(fg,cls,sizeof(cls));
  return strcmp(cls,"#32770")==0;
}

static bool fsxWindowBelongsToThisProcess(HWND hWnd)
{
  if(!hWnd) return false;
  DWORD pid=0;
  GetWindowThreadProcessId(hWnd,&pid);
  if(pid != GetCurrentProcessId())
    return false;

  char cls[64]={0};
  GetClassNameA(hWnd,cls,sizeof(cls));
  // Never virtualize native Windows menu/dialog windows.
  if(strcmp(cls,"#32768")==0 || strcmp(cls,"#32770")==0)
    return false;
  return true;
}

BOOL WINAPI NewGetMonitorInfoW(HMONITOR hMonitor, LPMONITORINFOEX lpmi) {
  typedef BOOL (WINAPI*OCALL)(HMONITOR, LPMONITORINFOEX);
  const static OCALL origFunc = (OCALL) getHookCall("GetMonitorInfoW");
  if(!lpmi) return FALSE;

  MONITORINFOEX tmp;
  ZeroMemory(&tmp, sizeof(tmp));
  tmp.cbSize = lpmi->cbSize;
  BOOL ret = origFunc(hMonitor, &tmp);

  SOURCE_MODULE(srcMod);
  if(ret) {
    memcpy(lpmi, &tmp, lpmi->cbSize);
    if(isHooked(srcMod) && SoftTHActive && FSXVirtualMonitorActive &&
       !fsxForegroundIsDialog() &&
       fsxMonitorVirtualizationCaller(srcMod) &&
       (tmp.dwFlags & MONITORINFOF_PRIMARY)) {
      const LONG ow = tmp.rcMonitor.right - tmp.rcMonitor.left;
      const LONG oh = tmp.rcMonitor.bottom - tmp.rcMonitor.top;

      // g2d.dll uses rcMonitor to build FSX's screen-space/cockpit geometry.
      // Keep rcWork untouched/physical so ordinary window and dialog placement
      // does not treat the 5760x2160 virtual render canvas as a Windows desktop.
      lpmi->rcMonitor.left = 0;
      lpmi->rcMonitor.top = 0;
      lpmi->rcMonitor.right = config.main.renderResolution.x;
      lpmi->rcMonitor.bottom = config.main.renderResolution.y;

      static bool logged=false;
      if(!logged) {
        dbg("FSX GetMonitorInfoW render-only virtual monitor: rcMonitor %ldx%ld -> %dx%d, rcWork stays physical caller=%s",
            ow,oh,config.main.renderResolution.x,config.main.renderResolution.y,
            getModuleName(srcMod));
        logged=true;
      }
    }
  }

  if(ret && isHooked(srcMod) && SoftTHActive &&
     fsxMonitorVirtualizationCaller(srcMod) &&
     (!FSXVirtualMonitorActive || fsxForegroundIsDialog())) {
    static bool loggedPhysical=false;
    if(!loggedPhysical) {
      dbg("FSX GetMonitorInfoW: returning physical monitor for UI/dialog state");
      loggedPhysical=true;
    }
  }
  return ret;
}

BOOL WINAPI NewGetMonitorInfoA(HMONITOR hMonitor, LPMONITORINFOEX lpmi) {
  typedef BOOL (WINAPI*OCALL)(HMONITOR, LPMONITORINFOEX);
  const static OCALL origFunc = (OCALL) getHookCall("GetMonitorInfoA");
  if(!lpmi) return FALSE;

  MONITORINFOEX tmp;
  ZeroMemory(&tmp, sizeof(tmp));
  tmp.cbSize = lpmi->cbSize;
  BOOL ret = origFunc(hMonitor, &tmp);

  SOURCE_MODULE(srcMod);
  if(ret) {
    memcpy(lpmi, &tmp, lpmi->cbSize);
    if(isHooked(srcMod) && SoftTHActive && FSXVirtualMonitorActive &&
       !fsxForegroundIsDialog() &&
       fsxMonitorVirtualizationCaller(srcMod) &&
       (tmp.dwFlags & MONITORINFOF_PRIMARY)) {
      const LONG ow = tmp.rcMonitor.right - tmp.rcMonitor.left;
      const LONG oh = tmp.rcMonitor.bottom - tmp.rcMonitor.top;

      // Render geometry gets the virtual monitor, but Windows placement keeps
      // the real physical work area.
      lpmi->rcMonitor.left = 0;
      lpmi->rcMonitor.top = 0;
      lpmi->rcMonitor.right = config.main.renderResolution.x;
      lpmi->rcMonitor.bottom = config.main.renderResolution.y;

      static bool logged=false;
      if(!logged) {
        dbg("FSX GetMonitorInfoA render-only virtual monitor: rcMonitor %ldx%ld -> %dx%d, rcWork stays physical caller=%s",
            ow,oh,config.main.renderResolution.x,config.main.renderResolution.y,
            getModuleName(srcMod));
        logged=true;
      }
    }
  }

  if(ret && isHooked(srcMod) && SoftTHActive &&
     fsxMonitorVirtualizationCaller(srcMod) &&
     (!FSXVirtualMonitorActive || fsxForegroundIsDialog())) {
    static bool loggedPhysical=false;
    if(!loggedPhysical) {
      dbg("FSX GetMonitorInfoA: returning physical monitor for UI/dialog state");
      loggedPhysical=true;
    }
  }
  return ret;
}

static MONITORENUMPROC MonitorEnumProcReal;
static BOOL CALLBACK NewMonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData)
{
  // Only pass primary monitor to the real proc
  MONITORINFO mi;
  mi.cbSize = sizeof(MONITORINFO);
  GetMonitorInfo(hMonitor, &mi);
  if(mi.dwFlags & MONITORINFOF_PRIMARY)
    return MonitorEnumProcReal(hMonitor, hdcMonitor, lprcMonitor, dwData);
  else
    return TRUE;
}

BOOL WINAPI NewEnumDisplayDevicesW(LPCWSTR lpDevice, DWORD iDevNum, PDISPLAY_DEVICEW lpDisplayDevice, DWORD dwFlags)
{
  typedef BOOL (WINAPI*OCALL)(LPCWSTR, DWORD, PDISPLAY_DEVICEW, DWORD);
  const static OCALL origFunc = (OCALL) getHookCall("EnumDisplayDevicesW");

  if (iDevNum == 0) RealDisplayCount = 99;

  BOOL ret = origFunc(lpDevice, iDevNum, lpDisplayDevice, dwFlags);

  SOURCE_MODULE(srcMod);
  if(isHooked(srcMod)) {
    // TODO: CJR - Add SoftTH as a display device once all other displays have been enumerated
    if (!ret && iDevNum < RealDisplayCount)
    {
      dbg("NewEnumDisplayDevicesW");
      dbg("Adding SoftTH as a display device");

      RealDisplayCount = iDevNum - 1;

      WCHAR             dname[32] = L"SoftTH";
      WCHAR             dstr[128] = SOFTTH_VERSIONW;
      WCHAR             did[128]  = SOFTTHDEVIDW;
      WCHAR             dkey[128] = L"";


      ZeroMemory(lpDisplayDevice, sizeof(DISPLAY_DEVICEW));
      lpDisplayDevice->cb = sizeof(DISPLAY_DEVICEW);
      lstrcpynW(lpDisplayDevice->DeviceName, dname, 32);
      lstrcpynW(lpDisplayDevice->DeviceString, dstr, 128);
      lpDisplayDevice->StateFlags = DISPLAY_DEVICE_ACTIVE;// | DISPLAY_DEVICE_ATTACHED_TO_DESKTOP;
      lstrcpynW(lpDisplayDevice->DeviceID, did, 128);
      lstrcpynW(lpDisplayDevice->DeviceKey, dkey, 128);

      //memcpy(&lpDisplayDevice, &dd, sizeof(DISPLAY_DEVICE));

      //lpDisplayDevice = *lpdd;

      ret = true;
    }
  }
  return ret;
}

BOOL WINAPI NewEnumDisplayDevicesA(LPCTSTR lpDevice, DWORD iDevNum, PDISPLAY_DEVICEA lpDisplayDevice, DWORD dwFlags)
{
  typedef BOOL (WINAPI*OCALL)(LPCTSTR, DWORD, PDISPLAY_DEVICEA, DWORD);
  const static OCALL origFunc = (OCALL) getHookCall("EnumDisplayDevicesA");

  if (iDevNum == 0) RealDisplayCount = 99;

  BOOL ret = origFunc(lpDevice, iDevNum, lpDisplayDevice, dwFlags);

  SOURCE_MODULE(srcMod);
  if(isHooked(srcMod)) {
    // TODO: CJR - Add SoftTH as a display device once all other displays have been enumerated
    if (!ret && iDevNum < RealDisplayCount)
    {
      dbg("NewEnumDisplayDevicesA");
      dbg("Adding SoftTH as a display device");

      RealDisplayCount = iDevNum - 1;

      CHAR             dname[32] = "SoftTH";
      CHAR             dstr[128] = SOFTTH_VERSION;
      CHAR             did[128]  = SOFTTHDEVID;
      CHAR             dkey[128] = "";


      ZeroMemory(lpDisplayDevice, sizeof(DISPLAY_DEVICEA));
      lpDisplayDevice->cb = sizeof(DISPLAY_DEVICEA);
      lstrcpynA(lpDisplayDevice->DeviceName, dname, 32);
      lstrcpynA(lpDisplayDevice->DeviceString, dstr, 128);
      lpDisplayDevice->StateFlags = DISPLAY_DEVICE_ACTIVE;// | DISPLAY_DEVICE_ATTACHED_TO_DESKTOP;
      lstrcpynA(lpDisplayDevice->DeviceID, did, 128);
      lstrcpynA(lpDisplayDevice->DeviceKey, dkey, 128);

      //memcpy(&lpDisplayDevice, &dd, sizeof(DISPLAY_DEVICE));

      //lpDisplayDevice = *lpdd;

      ret = true;
    }
  }
  return ret;
}

BOOL WINAPI NewEnumDisplayMonitors(HDC hdc, LPCRECT lprcClip, MONITORENUMPROC lpfnEnum, LPARAM dwData)
{
	typedef BOOL (WINAPI*OCALL)(HDC, LPCRECT, MONITORENUMPROC, LPARAM);
	const static OCALL origFunc = (OCALL) getHookCall("EnumDisplayMonitors");
  SOURCE_MODULE(srcMod);
  if(isHooked(srcMod)) {
    dbg("NewEnumDisplayMonitors");
    MonitorEnumProcReal = lpfnEnum;
    lpfnEnum = NewMonitorEnumProc;
  }
  return origFunc(hdc, lprcClip, lpfnEnum, dwData);
}

void WINAPI NullHook()
{
  dbg("NullHook hit!");
}

// Init hook function table
GHOOK InitHooks[] = {
  HOOK(NewGetModuleHandleA, kernel32.dll, GetModuleHandleA)
  HOOK(NewGetModuleHandleW, kernel32.dll, GetModuleHandleW) // Disabled: doesnt seem to be needed
  HOOKEND
};

// Main hook function table
GHOOK SoftTHHooks[] = {
#if 1
  //HOOK(NewGetModuleHandleA, kernel32.dll, GetModuleHandleA)
  //HOOK(NewGetProcAddress, kernel32.dll, GetProcAddress)
  HOOK(NewClipCursor, user32.dll, ClipCursor)
  HOOK(NewGetCursorPos, user32.dll, GetCursorPos)
  HOOK(NewGetCursorInfo, user32.dll, GetCursorInfo)
  HOOK(NewSetCursorPos, user32.dll, SetCursorPos)
  HOOK(NewTrackPopupMenu, user32.dll, TrackPopupMenu)
  HOOK(NewTrackPopupMenuEx, user32.dll, TrackPopupMenuEx)
  HOOK(NewGetSystemMetrics, user32.dll, GetSystemMetrics)
  HOOK(NewGetWindowRect, user32.dll, GetWindowRect)
  HOOK(NewGetClientRect, user32.dll, GetClientRect)
  HOOK(NewSetWindowPos, user32.dll, SetWindowPos)

  // WOW testing:
  HOOK(NewSetPhysicalCursorPos, user32.dll, SetPhysicalCursorPos)
  HOOK(NewGetPhysicalCursorPos, user32.dll, GetPhysicalCursorPos)
  HOOK(NewLogicalToPhysicalPoint, user32.dll, LogicalToPhysicalPoint)
  /*HOOK(NewSendInput, user32.dll, SendInput)
  HOOK(Newmouse_event, user32.dll, mouse_event)  */
  /*HOOK(nil_debug, user32.dll, GetMouseMovePointsEx)
  HOOK(nil_debug, user32.dll, DragDetect)
  HOOK(nil_debug, user32.dll, GetMouseMovePointsEx)*/
  /*HOOK(NewSendMessageA, user32.dll, SendMessageA)
  HOOK(NewPostMessageA, user32.dll, PostMessageA)	*/
  //HOOK(NewMapWindowPoints, user32.dll, MapWindowPoints)

  // GDI
  HOOK(NewGetMonitorInfoW, user32.dll, GetMonitorInfoW)
  HOOK(NewGetMonitorInfoA, user32.dll, GetMonitorInfoA)
  HOOK(NewEnumDisplayDevicesW, user32.dll, EnumDisplayDevicesW)
  HOOK(NewEnumDisplayDevicesA, user32.dll, EnumDisplayDevicesA)
  HOOK(NewEnumDisplaySettingsW, user32.dll, EnumDisplaySettingsW)
  HOOK(NewEnumDisplaySettingsA, user32.dll, EnumDisplaySettingsA)
  HOOK(NewEnumDisplaySettingsExW, user32.dll, EnumDisplaySettingsExW)
  HOOK(NewEnumDisplaySettingsExA, user32.dll, EnumDisplaySettingsExA)
  //HOOK(NewChangeDisplaySettingsW, user32.dll, ChangeDisplaySettingsW)
  HOOK(NewChangeDisplaySettingsA, user32.dll, ChangeDisplaySettingsA)
  //HOOK(NewChangeDisplaySettingsExW, user32.dll, ChangeDisplaySettingsExW)
  HOOK(NewChangeDisplaySettingsExA, user32.dll, ChangeDisplaySettingsExA)
  HOOK(NewEnumDisplayMonitors, user32.dll, EnumDisplayMonitors)
  HOOK(NewClientToScreen, user32.dll, ClientToScreen)
  HOOK(NewScreenToClient, user32.dll, ScreenToClient)

#else
  HOOK(NewGetClientRect, user32.dll, GetClientRect)
#endif


  HOOKEND
};
const int numHooks = (sizeof(SoftTHHooks)/sizeof(GHOOK));

// Internal functionality //
#define NUM_MOD 32
static HMODULE noHookModules[NUM_MOD];
static int curNumModules = 0;

extern "C" __declspec(dllexport) void addNoHookModule(HMODULE mod)
{
  if(!mod) return;
  char fn[256];
  GetModuleFileName(mod, fn, 256);
  dbg("Added module <%s> to hook ignore list", fn);
  noHookModules[curNumModules++] = mod;
}

// Return false if this module is in nohookmodules list
static bool isHooked(HMODULE mod)
{
  for(int i=0;i<curNumModules;i++)
    if(noHookModules[i] == mod)
      return false;

  // Never virtualize USER32/display APIs for Windows system components or
  // display-driver modules. SoftTH only needs to present its virtual topology
  // to the application and its own modules.
  if(mod) {
    char fn[MAX_PATH] = {0};
    if(GetModuleFileNameA(mod, fn, MAX_PATH)) {
      CharLowerBuffA(fn, (DWORD)strlen(fn));
      if(strstr(fn, "\\windows\\system32\\") ||
         strstr(fn, "\\windows\\syswow64\\") ||
         strstr(fn, "\\driverstore\\"))
        return false;
    }
  }

  return true;
}

// Return index to hooks table from name
int getHookId(char *name) {
	//DebugMessage("getHookId: Hook '%s'...", name);
	for(int i=0;i<numHooks;i++)
		if(!_stricmp(SoftTHHooks[i].name, name))
			return i;
	// Oops! hook not found, prepare for crash
	dbg("getHookId: WARNING! Hook '%s' not found!", name);
	return 0;
}

void* getHookCall(char *name) {
	return &SoftTHHooks[getHookId(name)].stubCall;
}


#else
// 64bit stubs

GHOOK SoftTHHooks[] = {
  HOOKEND
};

void addNoHookModule(HMODULE mod)
{
}

#endif
