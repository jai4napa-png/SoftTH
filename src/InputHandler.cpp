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

// Handle input, mainly mouse coordinate repositioning

#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0501

#include <windows.h>
#include "helper.h"
#include "inputhandler.h"
#include "Windowsx.h"

#include "configFile.h"
#include "main.h"
#include <math.h>

//#include "d3dSoftTH.h" // Removed by CJR for SDK 8.1 - 9 Aug 2015

#include <list>

#include "overlay_interface.h"

std::list<DWORD> hookedThreads; // List of all active hooks

InputHandler ihGlobal; // Global handler. init automatically on dll start
__declspec(thread) static HHOOK threadHookMsg = NULL;  // Thread-local-storage hook handle

static LRESULT CALLBACK GetMsgProc(int nCode, WPARAM wParamIn, LPARAM lParamIn)
{
  dbg("InputHandler: GetMsgProc");

  if(nCode < 0 || wParamIn == PM_NOREMOVE)
    return CallNextHookEx(NULL, nCode, wParamIn, lParamIn);
  HWND win = ihGlobal.getHWND();
  if(!win) {
    //dbg("!win");
    return CallNextHookEx(NULL, nCode, wParamIn, lParamIn); // Hook window not set
  }

  MSG* msg = (MSG*) lParamIn;
  WPARAM wParam = msg->wParam;
  LPARAM lParam = msg->lParam;
  UINT wmsg = msg->message;

  // Keyboard hooks
  switch(wmsg) {
    case WM_KEYUP:
      if(GetKeyState(VK_APPLICATION) < 0) {
        ihGlobal.keyDown((int)wParam);
        msg->message = WM_NULL;
        break;
      }
      if(wParam == VK_SNAPSHOT)
        ihGlobal.keyDown((int)wParam);
      break;
    case WM_KEYDOWN:
      if(GetKeyState(VK_APPLICATION) < 0) {
        ihGlobal.keyUp((int)wParam);
        msg->message = WM_NULL;
        break;
      }
      if(wParam == VK_SNAPSHOT)
        ihGlobal.keyUp((int)wParam);
      break;
  }

  bool hookAllWindows = false;
  if(msg->hwnd != win && !hookAllWindows) {

    char msgTxt[256];
    switch(wmsg) {
      case MOUSE_ALL_WM_EVENTS:
        strcpy(msgTxt, getMouseEventName(wmsg));
        break;
      default:
        sprintf(msgTxt, "0x%08X", wmsg);
        break;
    }

    char name[256];
    GetWindowText(msg->hwnd, name, 256);
    dbg_input("win %d != %d (%s, %s)", msg->hwnd, win, name, msgTxt);

    /*if(wmsg == WM_INPUT)
      msg->message = WM_NULL;*/

		return CallNextHookEx(NULL, nCode, wParamIn, lParamIn); // Not our window
  }

  if(wmsg == WM_MOUSELEAVE)
    msg->message = WM_NULL;

  switch(wmsg) {

    // Mouse hooks
#define DBG_MSG(x) case x: dbg("PrimaryWindow: %s", #x); break

    DBG_MSG(WM_ACTIVATE);
    DBG_MSG(WM_MOUSEACTIVATE);

    case WM_MOUSELEAVE:
    case WM_NCMOUSELEAVE:
      // Discard mouse leave messages - TODO: recall TrackMouseEvent
      msg->message = WM_NULL;
      break;

    case MOUSE_ALL_WM_EVENTS: // Note: special macro
    {
      if(!SoftTHActive)
        break;

      if(msg->wParam & MOUSE_EVENTS_ALREADY_MAPPED && wmsg != WM_MOUSEWHEEL) {
        // This message came from a secondary SoftTH window. lParam already
        // contains virtual-backbuffer coordinates; MSG.pt must match them too.
        msg->wParam -= MOUSE_EVENTS_ALREADY_MAPPED;
        msg->pt.x = GET_X_LPARAM(msg->lParam);
        msg->pt.y = GET_Y_LPARAM(msg->lParam);
        dbg_input("PrimaryWindow: %s: %dx%d MOUSE_EVENTS_ALREADY_MAPPED (wparam: 0x%08X)", getMouseEventName(wmsg), msg->pt.x, msg->pt.y, msg->wParam);

        // Send click to overlay
        if(wmsg == WM_LBUTTONDOWN || wmsg == WM_LBUTTONUP)
        {
          OVERLAY_CLICK_BLOCK p;
          p.overlayVersion = OVERLAY_VERSION;
          p.x = msg->pt.x;
          p.y = msg->pt.y;
          p.up = wmsg==WM_LBUTTONUP;
          p.event = wmsg;
          p.lparam = msg->lParam;
          p.wparam = msg->wParam;
          p.appWindow = win;
          overlayDoClick(&p);
        }

        break;
      }

      POINT vp = {msg->pt.x, msg->pt.y};
      bool mapped = false;
      HWND winCursor = WindowFromPoint(msg->pt);
      if(winCursor != win) {
        // Drag event is going from primary monitor to secondary
        POINT op = {msg->pt.x, msg->pt.y};
        ScreenToClient(winCursor, &op);
        mapped = inputMapClientToVirtual(winCursor, &op, &vp);
      } else {
        // MSG.pt is in desktop screen coordinates. Convert to the real
        // primary client coordinates before mapping to the virtual backbuffer.
        POINT op = {msg->pt.x, msg->pt.y};
        ScreenToClient(win, &op);
        mapped = inputMapClientToVirtual(win, &op, &vp);
      }
      if(!mapped) {
        // Do not eat FSX mouse input if a coordinate cannot be mapped. Passing
        // the original message through is safer than converting it to WM_NULL
        // and preserves cockpit interaction during transient window changes.
        dbg_input("PrimaryWindow: mouse mapping failed; passing original event");
        break;
      }
      dbg_input("PrimaryWindow: %s: %dx%d -> %dx%d (wparam: 0x%08X)", getMouseEventName(wmsg), msg->pt.x, msg->pt.y, vp.x, vp.y, msg->wParam);

      LPARAM lp = MAKELPARAM(vp.x, vp.y);
      msg->lParam = lp;
      msg->pt.x = vp.x;
      msg->pt.y = vp.y;

      // Send click to overlay
      if(wmsg == WM_LBUTTONDOWN || wmsg == WM_LBUTTONUP || wmsg == WM_MOUSEMOVE)
      {
        OVERLAY_CLICK_BLOCK p;
        p.overlayVersion = OVERLAY_VERSION;
        p.x = vp.x;
        p.y = vp.y;
        p.up = wmsg==WM_LBUTTONUP; // Deprecated
        p.event = wmsg;
        p.lparam = msg->lParam;
        p.wparam = msg->wParam;
        p.appWindow = win;
        overlayDoClick(&p);
      }

      break;
    }

      break;
  }

  return CallNextHookEx(NULL, nCode, wParamIn, lParamIn);
}

void InputHandler::hookRemoteThread(DWORD threadID)
{
  if(config.getNumAdditionalHeads() == 0)
    return;
  setHook(threadID);
}

void InputHandler::newThread()
{
  // Per-thread auto-hooking disabled for modern Windows compatibility.
}

void InputHandler::detachThread()
{
  //dbg("InputHandler: Detaching from thread 0x%08X", threadID);
  releaseHook();
}

void InputHandler::setHook(DWORD tid)
{
  if(tid == NULL)
    tid = GetCurrentThreadId(); // Set the hook on current thread

  std::list<DWORD>::iterator i = hookedThreads.begin();
  while(i != hookedThreads.end()) {
    if(*i == tid) {
      return;
    }
    i++;
  }

  HHOOK newHook = SetWindowsHookEx(WH_GETMESSAGE, (HOOKPROC) GetMsgProc, 0, tid);
  if(!newHook) {
    dbg("InputHandler: FAILED to install mouse hook on thread 0x%08X (error %lu)", tid, GetLastError());
    return;
  }
  threadHookMsg = newHook;
  hookedThreads.push_back(tid);
  dbg("InputHandler: Installed mouse hook 0x%08X on thread 0x%08X (%d hooks)", threadHookMsg, tid, hookedThreads.size());
}

void InputHandler::releaseHook()
{
  DWORD tid = GetCurrentThreadId();
  dbg("InputHandler: Release mouse hook 0x%08X on thread 0x%08X (%d hooks)", threadHookMsg, tid, hookedThreads.size());

  UnhookWindowsHookEx(threadHookMsg);
  hookedThreads.remove(tid);
}

static HEAD* findHead(HWND win)
{
  // Find head for this window
  HEAD *head = NULL;
  int numDevs = config.getNumAdditionalHeads();
  for(int i=0;i<numDevs;i++) {
    HEAD *h = config.getHead(i);
    if(h->hwnd && h->hwnd == win)
    {
      head = h;
      dbg("findHead: SecondaryHead");
      break;
    }
  }
  if(!head) {
    HEAD *h = config.getPrimaryHead();
    if(h->hwnd && h->hwnd == win)
    {
      head = h;
      dbg("findHead: PrimaryHead");
    }
    else
    {
      dbg("findHead: Head not found");
      return NULL; // Head not found
    }
  }
  return head;
}

// Return window client rect... Ignores main window size - Takes destrect into account as well
static bool getTrueClientRect(HWND win, RECT *r)
{
  dbg("InputHandler: Getting true client rectangle");
  int numDevs = config.getNumAdditionalHeads();
  HEAD *h;
  for(int i=-1;i<numDevs;i++) {
    if(i == -1)
      h = config.getPrimaryHead();
    else
      h = config.getHead(i);
    if(h->hwnd && h->hwnd == win) {
      // A null destRect means StretchRect targets the entire physical head.
      // The legacy input mapper had this test reversed, returning a 0x0
      // client rectangle whenever destRect was omitted (the normal config).
      // That made mouse -> virtual coordinate conversion divide by zero and
      // prevented FSX virtual-cockpit mouse hit testing from lining up.
      if(!isNullRect(&h->destRect)) {
        *r = h->destRect;
      } else {
        r->left = r->top = 0;
        r->right = h->screenMode.x;
        r->bottom = h->screenMode.y;
      }

      // Defensive fallback for incomplete configs/device initialization.
      if(r->right <= r->left || r->bottom <= r->top) {
        RECT cr = {0,0,0,0};
        if(GetClientRect(win, &cr) &&
           cr.right > cr.left && cr.bottom > cr.top) {
          *r = cr;
        } else {
          return false;
        }
      }
      return true;
    }
  }
  return false;
}

// Return true if win is the main device window
bool inputMapIsDeviceWindow(HWND win)
{
  HEAD *h = config.getPrimaryHead();
  return (h->hwnd && win == h->hwnd);
}

// Translates virtual backbuffer coordinates to desktop coordinates
bool inputMapVirtualToDesktop(POINT *in, POINT *out)
{
  dbg("InputHandler: Map virtual to desktop");
  //if(SoftTHActiveSquashed && *SoftTHActiveSquashed)
  RECT *sr;
  RECT fullbb = {0, 0, config.main.renderResolution.x, config.main.renderResolution.y};
  int numDevs = config.getNumAdditionalHeads();
  for(int i=-1;i<numDevs;i++) {
    HEAD *h = i==-1?config.getPrimaryHead():config.getHead(i);
    if(SoftTHActiveSquashed && *SoftTHActiveSquashed)
      sr = &fullbb;
    else
      sr = &h->sourceRect;
    if(PtInRect(sr, *in) && h->hwnd)
    {
      //RECT *sr = &h->sourceRect;
      int sw = sr->right - sr->left;
      int sh = sr->bottom - sr->top;

      float xr = (in->x - sr->left) / (float)sw;
      float yr = (in->y - sr->top)  / (float)sh;

      RECT lpr;
      if(!getTrueClientRect(h->hwnd, &lpr))
        continue;
      POINT pp = {(int)floor(((float)(lpr.right-lpr.left)*xr)+0.5), (int)floor(((float)(lpr.bottom-lpr.top)*yr)+0.5)};
      pp.x += lpr.left;
      pp.y += lpr.top;
      if(ClientToScreen(h->hwnd, &pp)) {
        out->x = pp.x;
        out->y = pp.y;
        return true;
      }
    }
  }
  return false;
}

// Translates window client coordinates to virtual backbuffer coordinates
bool inputMapClientToVirtual(HWND win, POINT *in, POINT *out)
{
  dbg("InputHandler: Map client to virtual");
  HEAD *head = findHead(win);
  if(!head)
    return false;

  RECT lpr;
  if(!getTrueClientRect(win, &lpr))
    return false;
  const int cw = lpr.right-lpr.left;
  const int ch = lpr.bottom-lpr.top;
  if(cw <= 0 || ch <= 0)
    return false;
  float xr = (float)in->x / (float)cw;
  float yr = (float)in->y / (float)ch;

  xr -= lpr.left / (float) (lpr.right-lpr.left);
  yr -= lpr.top / (float) (lpr.bottom-lpr.top);

  float xx, yy;
  if(SoftTHActiveSquashed && *SoftTHActiveSquashed) {
    // Squashing is active - everything is cramped onto the primary monitor
    xx = (float)0 + (float)(config.main.renderResolution.x - 0)*xr;
    yy = (float)0 + (float)(config.main.renderResolution.y - 0)*yr;
  } else {
    xx = (float)head->sourceRect.left + (float)(head->sourceRect.right - head->sourceRect.left)*xr;
    yy = (float)head->sourceRect.top + (float)(head->sourceRect.bottom - head->sourceRect.top)*yr;
  }

  out->x = (int)floor(xx+0.5);
  out->y = (int)floor(yy+0.5);

  return true;
}

char* getMouseEventName(UINT event)
{
  switch(event) {
    case WM_LBUTTONDBLCLK: return "WM_LBUTTONDBLCLK";
		case WM_LBUTTONDOWN: return "WM_LBUTTONDOWN";
		case WM_LBUTTONUP: return "WM_LBUTTONUP";
		case WM_MBUTTONDBLCLK: return "WM_MBUTTONDBLCLK";
		case WM_MBUTTONDOWN: return "WM_MBUTTONDOWN";
		case WM_MBUTTONUP: return "WM_MBUTTONUP";
		case WM_MOUSEMOVE: return "WM_MOUSEMOVE";
		case WM_NCLBUTTONDBLCLK: return "WM_NCLBUTTONDBLCLK";
		case WM_NCLBUTTONDOWN: return "WM_NCLBUTTONDOWN";
		case WM_NCLBUTTONUP: return "WM_NCLBUTTONUP";
		case WM_NCMBUTTONDBLCLK: return "WM_NCMBUTTONDBLCLK";
		case WM_NCMBUTTONDOWN: return "WM_NCMBUTTONDOWN";
		case WM_NCMBUTTONUP: return "WM_NCMBUTTONUP";
		case WM_NCMOUSEMOVE: return "WM_NCMOUSEMOVE";
		case WM_NCRBUTTONDBLCLK: return "WM_NCRBUTTONDBLCLK";
		case WM_NCRBUTTONDOWN: return "WM_NCRBUTTONDOWN";
		case WM_NCRBUTTONUP: return "WM_NCRBUTTONUP";
		case WM_RBUTTONDBLCLK: return "WM_RBUTTONDBLCLK";
		case WM_RBUTTONDOWN: return "WM_RBUTTONDOWN";
		case WM_RBUTTONUP: return "WM_RBUTTONUP";
    case WM_MOUSEWHEEL:  return "WM_MOUSEWHEEL";
    default: return "WM_??? UNKNOWN";
  }
}
