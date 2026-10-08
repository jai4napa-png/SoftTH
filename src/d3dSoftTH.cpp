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

#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0501

#include "d3dSoftTH.h"
#include "d3dtexture.h"

#include <stdio.h>
#include "main.h"

#include "win32.h"
#include <process.h>

#include "InputHandler.h"
#include "d3dDrawing.h"

#include "time.h"
#include <math.h>
#include "Shlobj.h"
#include "Dwmapi.h"

#include "overlay_interface.h"

#include <INITGUID.H>
DEFINE_GUID(IID_IDirect3DDevice9SoftTH, 0xb18b10ce, 0x2649, 0x405a, 0x87, 0xf, 0x95, 0xf7, 0xaa, 0xbb, 0xcc, 0xdd);
DEFINE_GUID(IID_SoftTHInvalidRTT, 0x12345542, 0x2134, 0x4545, 0xff, 0xff, 0xba, 0xdf, 0x00, 0xdb, 0xFF, 0xFF);

#undef dbgf
#define dbgf if(0)
//#define dbgf dbg

#define YIELD_CPU  YieldProcessor()
// #define YIELD_CPU SwitchToThread()
// #define YIELD_CPU Sleep(1)

volatile int SoftTHActive = 0; // >0 if SoftTH is currently active and resolution is overridden
bool *SoftTHActiveSquashed = NULL; // Pointer to latest SoftTH device squash variable (TODO: horrible)
HWND SoftTHPresentWindow = NULL; // FSX-owned window supplied through Present(hDestWindowOverride)

// New SoftTH device instance created
// Create our fake backbuffer etc.
IDirect3DDevice9SoftTH::IDirect3DDevice9SoftTH(IDirect3D9New *parentNew, IDirect3D9Ex *direct3D, HWND hFocusWindowNew, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pp)
:IDirect3DDevice9New(dev, direct3D)
{

  // Disable Vista desktop composition - this will give us a performance increase
  if(!config.main.keepComposition) {
    extern bool didDisableComposition;
    if(!didDisableComposition) {
      didDisableComposition = true;
      DwmEnableComposition(DWM_EC_DISABLECOMPOSITION);
      Sleep(100);
    }
  }

  parent = parentNew;
  memcpy(&lastPp, pp, sizeof(D3DPRESENT_PARAMETERS));
  hFocusWindow = hFocusWindowNew;
  copybuf = 0;
  lockableBB = squash = showGraph = nocopy = showLog = false;
  curSurfA = true;

  fsxCachedFVF = 0;
  fsxCachedUsingDecl = false;
  fsxCachedPositionT = false;
  fsxCachedVertexShader = false;
  fsxCachedStride0 = 0;
  fsxCachedTex0W = fsxCachedTex0H = 0;
  fsxCachedPhysicalVSConstReg = -1;
  fsxCachedPhysicalPSConstReg = -1;
  ZeroMemory(&fsxCachedViewport, sizeof(fsxCachedViewport));

  refs = messageTime = numDevs = 0;

  bbtex = NULL;
  ppaux = NULL;

  postprocess = false;

  newbb = NULL;
  copyPack24to32 = NULL;
  copyDither = NULL;

  SoftTHActiveSquashed = &squash;

  /*
  // Move window
  if(true)
  {
    SetWindowPos(pp->hDeviceWindow, HWND_TOPMOST, -1920, 0, 5760, 1200, SWP_SHOWWINDOW);
    pp->BackBufferWidth = config.main.renderResolution.x;
    pp->BackBufferHeight = config.main.renderResolution.y;
  }
  */

  if(!pp->Windowed &&
     (config.overrides.forceResolution || config.getNumAdditionalHeads() > 0) &&
     (pp->BackBufferWidth != config.main.renderResolution.x ||
      pp->BackBufferHeight != config.main.renderResolution.y)) {
    dbg("FSX multihead: forcing fullscreen device resolution from %dx%d to %dx%d",
        pp->BackBufferWidth, pp->BackBufferHeight,
        config.main.renderResolution.x, config.main.renderResolution.y);
    pp->BackBufferWidth = config.main.renderResolution.x;
    pp->BackBufferHeight = config.main.renderResolution.y;
  }

  const bool windowedMultiheadAtCreate =
      (pp->Windowed && config.main.windowedMultihead &&
       config.getNumAdditionalHeads() > 0);
  wantedX = windowedMultiheadAtCreate ? config.main.renderResolution.x : pp->BackBufferWidth;
  wantedY = windowedMultiheadAtCreate ? config.main.renderResolution.y : pp->BackBufferHeight;
  if(windowedMultiheadAtCreate)
    dbg("FSX: activating %dx%d multihead backing surface without changing the windowed FSX presentation mode",
        wantedX, wantedY);
  fpuPreserve = (BehaviorFlags&D3DCREATE_FPU_PRESERVE)!=0;

  if(!validateSettings(d3d)) {
    dbg("SoftTH: validateSettings FAILED");
    createDeviceResult = D3DERR_INVALIDCALL;
    return;
  }

  dbg("Initializing SoftTH device (%dx%d) %s %s", pp->BackBufferWidth, pp->BackBufferHeight, getMode(pp->BackBufferFormat), pp->Windowed?"Windowed":"Fullscreen");
  dbg("BehaviorFlags: <%s>", getD3DCreate(BehaviorFlags));
  dbg("Swapeffect: %s (%d backbuffer(s))", getSwapEffect(pp->SwapEffect), pp->BackBufferCount);

  detectTransportMethods();
  timeBeginPeriod(1);

  if(windowedMultiheadAtCreate ||
     (isSoftTHmode(pp->BackBufferWidth, pp->BackBufferHeight) && !pp->Windowed))
  {
    dbg("Multihead mode %dx%d detected", pp->BackBufferWidth, pp->BackBufferHeight);

    loadOverlay();

    D3DPRESENT_PARAMETERS newpp = *pp;
    adjustPP(&newpp);

    // Create device. adjustPP() may force only the underlying physical
    // primary device to windowed mode while FSX remains logically fullscreen.
    bool fullscreen = newpp.Windowed==0;
    D3DDISPLAYMODEEX mode = {
      sizeof(D3DDISPLAYMODEEX),
      newpp.BackBufferWidth,  newpp.BackBufferHeight,
      newpp.FullScreen_RefreshRateInHz, newpp.BackBufferFormat,
      D3DSCANLINEORDERING_PROGRESSIVE
    };

    char name[256];
    DWORD pid = 0;
    GetWindowText(hFocusWindow, name, 256);
    DWORD focusTid = GetWindowThreadProcessId(hFocusWindow, &pid);
    dbg("Focus window 0x%08X <%s>, thread 0x%08X", hFocusWindow, name, focusTid);
    if(focusTid)
      ihGlobal.hookRemoteThread(focusTid);
    GetWindowText(pp->hDeviceWindow, name, 256);
    DWORD deviceTid = GetWindowThreadProcessId(pp->hDeviceWindow, &pid);
    dbg("Device window 0x%08X <%s>, thread 0x%08X", pp->hDeviceWindow, name, deviceTid);
    if(deviceTid)
      ihGlobal.hookRemoteThread(deviceTid);

    RECT r;
    GetWindowRect(hFocusWindow, &r);
    dbg("Device window  pre-create: %s", strRect(&r));

    HRESULT ret = direct3D->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hFocusWindow, BehaviorFlags, &newpp, !fullscreen?NULL:&mode, &dev);
    createDeviceResult = ret;
    if(ret != D3D_OK) {
      dbg("SoftTH: CreateDeviceEx FAILED!");
      return;
    }

    /*dev->SetMaximumFrameLatency(2);*/

    GetWindowRect(hFocusWindow, &wpos);
    dbg("Device window post-create: %s", strRect(&wpos));

    pp->BackBufferFormat = newpp.BackBufferFormat;
    createBuffers();

    // Initialize overlay
    OVERLAY_INIT_BLOCK op;
    op.overlayVersion = OVERLAY_VERSION;
    op.dev = dev;
    initOverlay(&op);

  } else {
    dbg("Singlehead mode %dx%d %dHz", pp->BackBufferWidth, pp->BackBufferHeight, pp->FullScreen_RefreshRateInHz);

    pp->FullScreen_RefreshRateInHz = matchRefresh(pp);

    char name[256];
    DWORD pid = 0;
    GetWindowText(hFocusWindow, name, 256);
    DWORD focusTid = GetWindowThreadProcessId(hFocusWindow, &pid);
    dbg("Focus window 0x%08X <%s>, thread 0x%08X", hFocusWindow, name, focusTid);
    if(focusTid)
      ihGlobal.hookRemoteThread(focusTid);
    GetWindowText(pp->hDeviceWindow, name, 256);
    DWORD deviceTid = GetWindowThreadProcessId(pp->hDeviceWindow, &pid);
    dbg("Device window 0x%08X <%s>, thread 0x%08X", pp->hDeviceWindow, name, deviceTid);
    if(deviceTid)
      ihGlobal.hookRemoteThread(deviceTid);

    RECT r;
    GetWindowRect(hFocusWindow, &r);
    dbg("Device window  pre-create: %s", strRect(&r));

    // Create device
    bool fullscreen = pp->Windowed==0;
    D3DDISPLAYMODEEX mode = {
      sizeof(D3DDISPLAYMODEEX),
      pp->BackBufferWidth,   pp->BackBufferHeight,
      pp->FullScreen_RefreshRateInHz,  pp->BackBufferFormat,
      D3DSCANLINEORDERING_PROGRESSIVE
    };
    HRESULT ret = direct3D->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hFocusWindow, BehaviorFlags, pp, !fullscreen?NULL:&mode, &dev);
    createDeviceResult = ret;
    if(ret != D3D_OK) {
      dbg("SoftTH: CreateDeviceEx FAILED!");
      return;
    }

    GetWindowRect(hFocusWindow, &wpos);
    dbg("Device window post-create: %s", strRect(&wpos));
  }
}

void IDirect3DDevice9SoftTH::createBuffers()
{
  int br = getRefs();

  // Set mouse hook on application focus window
  ihGlobal.setHWND(hFocusWindow);
  dev->SetMaximumFrameLatency(1);

  // Get backbuffer size
  D3DCALL( dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb) );
  bb->GetDesc(&bbDesc);
  bb->Release();  // Pretend we never touched it
  dbg("Real backbuffer: %dx%d %s ms%d:%d", bbDesc.Width, bbDesc.Height, getMode(bbDesc.Format), bbDesc.MultiSampleType, bbDesc.MultiSampleQuality);

#ifdef ENABLE_POSTPROCESS
  if(postprocess)
  {
    // Override XRGB mode to ARGB
    // TODO: use only with FXAA pre-pass
    if(bbDesc.Format == D3DFMT_X8R8G8B8) bbDesc.Format = D3DFMT_A8R8G8B8;
  }
#endif

  // Create our new backbuffer
  dbg("Creating new backbuffer: %dx%d %s ms%d:%d", wantedX, wantedY, getMode(bbDesc.Format), msWanted, msQWanted);
  D3DCALL( dev->CreateRenderTarget(wantedX, wantedY, bbDesc.Format, msWanted, msQWanted, lockableBB, &newbb, NULL) );
  D3DCALL( dev->SetRenderTarget(0, newbb) );
  newbb->GetDesc(&newbbDesc);

  // The real D3D device backbuffer is only the primary monitor size, so D3D9Ex
  // initializes its default viewport to that physical size.  SoftTH renders to
  // a larger virtual backbuffer; expand the viewport to the virtual dimensions
  // immediately or vertically/horizontally offset heads can remain black.
  D3DVIEWPORT9 virtualVp = {0, 0, (DWORD)wantedX, (DWORD)wantedY, 0.0f, 1.0f};
  D3DCALL( dev->SetViewport(&virtualVp) );
  fsxCachedViewport = virtualVp;
  // Fullscreen D3D9 devices start with a physical-backbuffer scissor rectangle.
  // Set it to the entire virtual scene, otherwise FSX can render only the
  // top-left 1920x1080 region and the left head shows a narrow image strip.
  RECT virtualScissor = {0, 0, wantedX, wantedY};
  D3DCALL( dev->SetScissorRect(&virtualScissor) );
  dbg("FSX fullscreen: virtual scissor initialized to %dx%d", wantedX, wantedY);
  dbg("SoftTH: Virtual viewport set to %dx%d (physical backbuffer %dx%d)",
      wantedX, wantedY, bbDesc.Width, bbDesc.Height);

#ifdef ENABLE_POSTPROCESS
  if(postprocess)
  {
    // Create auxiliary buffer for postprocess
    dbg("Creating postprocess auxiliary buffer: %dx%d %s", wantedX, wantedY, getMode(bbDesc.Format));
    D3DCALL( dev->CreateTexture(wantedX, wantedY, 1, D3DUSAGE_RENDERTARGET, bbDesc.Format, D3DPOOL_DEFAULT, &bbtex, NULL) );
    D3DCALL( bbtex->GetSurfaceLevel(0, &ppaux) );
  }
#endif

  if(depthWanted) {
    dbg("Creating DepthStencil surface: %dx%d %s %s", wantedX, wantedY, getMode(depthFormatWanted), discardDepth?"DISCARD":"");
    D3DCALL( dev->CreateDepthStencilSurface(wantedX, wantedY, depthFormatWanted, msWanted, msQWanted, discardDepth, &newdepth, NULL)  );
    dev->SetDepthStencilSurface(newdepth);
  } else
    newdepth = NULL;

  // Create temporary copy buffer.
  // Copy path is as follows: newbb -> copybuf (present bb) copybuf -> bb
  dbg("Creating copybuffer: %dx%d %s ms%d:%d", bbDesc.Width, bbDesc.Height, getMode(bbDesc.Format), bbDesc.MultiSampleType, bbDesc.MultiSampleQuality);
  D3DCALL( dev->CreateRenderTarget(bbDesc.Width, bbDesc.Height, bbDesc.Format, bbDesc.MultiSampleType, bbDesc.MultiSampleQuality, false, &copybuf, NULL) );

  // Init additional heads
  numDevs = config.getNumAdditionalHeads();

  // FSX multihead must never enter SoftTH's legacy "squash" or "nocopy"
  // debug modes. Squash deliberately scales the entire virtual render surface
  // onto the primary monitor and disables every secondary head, which exactly
  // mimics a broken multihead layout.
  if(numDevs > 0) {
    squash = false;
    nocopy = false;
    dbg("FSX multihead: forcing squash=OFF, nocopy=OFF");
  }

  int logoStopTime = GetTickCount() + 4000;

  bool needIndirect = true;
  if(config.getPrimaryHead()->manufacturer == MANF_NVIDIA)
  {
    needIndirect = false;
    dbg("NVIDIA card detected, using direct nonlocal read");
  }

  outDevs = new OUTDEVICE[numDevs];
  for(int i=0;i<numDevs;i++)
  {
    OUTDEVICE *o = &outDevs[i];

    // Create the output device
    HEAD *h = config.getHead(i);
    bool local = h->transportMethod==OUTMETHOD_LOCAL;
    dbg("Initializing head %d (DevID: %d, %s)...", i+1, h->devID, local?"local":"non-local");
    o->output = new outDirect3D9(h->devID, h->transportMethod, h->screenMode.x, h->screenMode.y, h->transportRes.x, h->transportRes.y, isNullRect(&h->destRect), hFocusWindow, fpuPreserve, logoStopTime, needIndirect);
    o->cfg = h;

    // Create shared surface
    HANDLE sha = o->output->GetShareHandleA();
    HANDLE shb = o->output->GetShareHandleB();
    if(sha && shb) {
      o->localSurfA = NULL;
      o->localSurfB = NULL;
      D3DCALL( dev->CreateRenderTargetEx(o->output->getBufWidth(), o->output->getBufHeight(), o->output->getFormat(), D3DMULTISAMPLE_NONE, 0, false, &o->localSurfA, &sha, NULL) );
      D3DCALL( dev->CreateRenderTargetEx(o->output->getBufWidth(), o->output->getBufHeight(), o->output->getFormat(), D3DMULTISAMPLE_NONE, 0, false, &o->localSurfB, &shb, NULL) );
    } else
      dbg("ERROR: Head %d: No share handles!", i+1);

    o->tempTex = NULL;

    if(h->transportMethod==OUTMETHOD_NONLOCAL_24b) {
      // Create packer copy
      if(!copyPack24to32)
        copyPack24to32 = new shCopy(dev, "D:\\dev\\data\\pack.psh");
      D3DCALL( dev->CreateTexture(o->output->getBufWidth(), o->output->getBufHeight(), 1, D3DUSAGE_RENDERTARGET, o->output->getFormat(), D3DPOOL_DEFAULT, &o->tempTex, NULL)  );
    }

    if(h->transportMethod==OUTMETHOD_NONLOCAL_16bDither) {
      // Create dither copy
      if(!copyDither)
        copyDither = new shCopy(dev, SHCOPY_DITHER);
        //copyDither = new shCopy(dev, "D:\\dev\\data\\dither.psh");
      D3DCALL( dev->CreateTexture(o->output->getBufWidth(), o->output->getBufHeight(), 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &o->tempTex, NULL)  );
    }
  }

  dev->SetRenderState(D3DRS_ZENABLE, depthWanted?D3DZB_TRUE:D3DZB_FALSE); // Default value depends on original state of EnableAutoDepthStencil

  D3DX( D3DXCreateFont(dev, 16, 0, FW_NORMAL, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, PROOF_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Verdana", &font) );
  D3DX( D3DXCreateFont(dev, 16, 16, FW_NORMAL, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, PROOF_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Verdana", &fontWide) );

  // Query used for pipeline synchronization
  D3DCALL( dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &squeryA) );
  D3DCALL( dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &squeryB) );

	// Check process affinity mask
#ifndef _WIN64
  DWORD mproc, msys;
	if(GetProcessAffinityMask(GetCurrentProcess(), &mproc, &msys)) {
		if(config.overrides.processAffinity <= 0) {
			// Display warning about affinity
			if(mproc < msys) {
				dbg("Warning! Process affinity is forced to less than processors available (0x%02X < 0x%02X),", mproc, msys);
				dbg("         SoftTH performance may not be optimal. Use processAffinity=1 to override.");
			}
		} else {
			// Force-fix affinity to all processors
			dbg("Forcing process affinity (0x%02X -> 0x%02X)", mproc, msys);
			if(!SetProcessAffinityMask(GetCurrentProcess(), msys))
				dbg("SetProcessAffinityMask failed!");
		}
	}
#endif

  drawing = new d3dDrawing(dev);
  int er = getRefs();
  refs = er-br;
  dbg("Additional refs: %d (%d - %d)", refs, br, er);

  SoftTHActive++;
}

void IDirect3DDevice9SoftTH::adjustPP(D3DPRESENT_PARAMETERS *pp)
{
  int wantedX = pp->BackBufferWidth;
  int wantedY = pp->BackBufferHeight;

  msWanted = pp->MultiSampleType;
  msQWanted = pp->MultiSampleQuality;
  depthWanted = pp->EnableAutoDepthStencil!=0;
  int numbbWanted =  pp->BackBufferCount;
  depthFormatWanted = pp->AutoDepthStencilFormat;

  if(config.overrides.antialiasing > 0)
    msWanted = (D3DMULTISAMPLE_TYPE) config.overrides.antialiasing;

  discardDepth = (pp->Flags&D3DPRESENTFLAG_DISCARD_DEPTHSTENCIL)!=0;
  lockableBB = (pp->Flags&D3DPRESENTFLAG_LOCKABLE_BACKBUFFER)!=0;
  /*bool fpuPreserve = (BehaviorFlags&D3DCREATE_FPU_PRESERVE)!=0;*/
  dbg("D3DCREATE_FPU_PRESERVE: %s", fpuPreserve?"enabled":"disabled");
  dbg("D3DPRESENTFLAG_DISCARD_DEPTHSTENCIL: %s", discardDepth?"enabled":"disabled");
  dbg("D3DPRESENTFLAG_LOCKABLE_BACKBUFFER: %s", lockableBB?"enabled":"disabled");

  dbg("%s (%d backbuffer(s))", getSwapEffect(pp->SwapEffect), pp->BackBufferCount);
  dbg("%s", getPresentationInterval(pp->PresentationInterval));
  dbg("Multisample level: %d (Quality %d)", msWanted, msQWanted);
  dbg("DepthStencil: %s (%s)", pp->EnableAutoDepthStencil?"enabled":"disabled", getMode(pp->AutoDepthStencilFormat));

  // Get mode from primary head config
  HEAD *hp = config.getPrimaryHead();
  pp->BackBufferWidth = hp->screenMode.x;
  pp->BackBufferHeight = hp->screenMode.y;
  dbg("Primary head: %dx%d", hp->screenMode.x, hp->screenMode.y);

  // FSX-Win11: keep SoftTH's physical primary device composited/windowed
  // even while FSX logically runs fullscreen on the virtual canvas.
  if(config.getNumAdditionalHeads() > 0 && !pp->Windowed) {
    dbg("FSX focus safety: physical primary device forced windowed at %dx%d while logical mode remains fullscreen",
        hp->screenMode.x, hp->screenMode.y);
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
  }

  hp->hwnd = hFocusWindow;

  // No multisampling required for the backbuffer
  pp->MultiSampleType = D3DMULTISAMPLE_NONE;
  pp->MultiSampleQuality = 0;
  pp->EnableAutoDepthStencil = false; // No depth buffer needed, we create our own

  pp->PresentationInterval = config.main.vsync?D3DPRESENT_INTERVAL_ONE:D3DPRESENT_INTERVAL_IMMEDIATE;
  pp->BackBufferCount = config.main.tripleBuffer?2:0;

  if(discardDepth)
    pp->Flags ^= D3DPRESENTFLAG_DISCARD_DEPTHSTENCIL;
  if(config.main.tripleBuffer && pp->SwapEffect != D3DSWAPEFFECT_DISCARD) {
    dbg("Warning: Overriding swapeffect to D3DSWAPEFFECT_DISCARD due to triplebuffer override");
    pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
  }
  memcpy(&lastPp, pp, sizeof(D3DPRESENT_PARAMETERS));
}

void IDirect3DDevice9SoftTH::destroyBuffers()
{
  dbg("SoftTH: Releasing buffers (%d devices)", numDevs);
  for(int i=0;i<numDevs;i++) {
    delete outDevs[i].output;
    SAFE_RELEASE_LAST(outDevs[i].localSurfA);
    SAFE_RELEASE_LAST(outDevs[i].localSurfB);
    SAFE_RELEASE_LAST(outDevs[i].tempTex);
  }
  numDevs = 0;

  D3DX(
    SAFE_RELEASE_LAST(font);
    SAFE_RELEASE_LAST(fontWide);
  )

  // Explicitly unbind SoftTH-owned default-pool surfaces before releasing
  // them. Direct3D keeps references to bound render/depth surfaces.
  IDirect3DSurface9 *cd = NULL;
  if(SUCCEEDED(dev->GetDepthStencilSurface(&cd)) && cd)
  {
    if(cd == newdepth)
    {
      dbg("SoftTH: Unbinding SoftTH depth surface before release");
      dev->SetDepthStencilSurface(NULL);
    }
    cd->Release();
  }

  IDirect3DSurface9 *crt = NULL;
  if(SUCCEEDED(dev->GetRenderTarget(0, &crt)) && crt)
  {
    if(crt == newbb)
    {
      IDirect3DSurface9 *nativeBB = NULL;
      if(SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &nativeBB)) && nativeBB)
      {
        dbg("SoftTH: Restoring native backbuffer before release");
        dev->SetRenderTarget(0, nativeBB);
        nativeBB->Release();
      }
    }
    crt->Release();
  }

  SAFE_RELEASE_LAST(copybuf);
  SAFE_RELEASE_LAST(squeryA);
  SAFE_RELEASE_LAST(squeryB);
  SAFE_RELEASE_LAST(bbtex);
  SAFE_RELEASE_LAST(newbb);
  SAFE_RELEASE_LAST(ppaux);
  SAFE_RELEASE_LAST(newdepth);

  newbb = NULL;
  bbtex = NULL;
  ppaux = NULL;
  newdepth = NULL;
  squeryB = squeryA = NULL;
  copybuf = NULL;

  delete copyPack24to32;
  delete copyDither;
  delete drawing;

  copyDither = NULL;
  copyPack24to32 = NULL;
  drawing = NULL;

  timeEndPeriod(1);
  SoftTHActive--;
  refs = 0;
}

ULONG IDirect3DDevice9SoftTH::Release()
{
  if(newbb)
  {
    int rr = getRefs();
    if(getRefs()-refs-1 == 0 && newbb) {

      // Let overlay release its stuff
      deinitOverlay();

      // Last ref - device will be freed on Release so release our stuff
      dbg("Release: SoftTH device freed (refcount %d-%d=0)", rr-1, refs);
      destroyBuffers();
      if(getRefs() != 1)
        dbg("Release: WARNING! Reference leak detected: Post-release refcount not 1 (%d)", getRefs());
      /*else
        delete this;*/
    }

    ULONG r = __super::Release();
    r -= refs;
    dbgf("dev SoftTH: Release result: %d", r);
    /*if(r == 0) {
      delete this;
    }*/
    return r;

  } else {
    return __super::Release();
  }
}

IDirect3DDevice9SoftTH::~IDirect3DDevice9SoftTH()
{
  dbg("~IDirect3DDevice9SoftTH");
  /*destroyBuffers();*/
  parent->destroyed(this);
}

void IDirect3DDevice9SoftTH::SetGammaRamp(UINT iSwapChain,DWORD Flags,CONST D3DGAMMARAMP* pRamp)
{
  dbgf("dev: SetGammaRamp %d", iSwapChain);
  if(iSwapChain == 0)
    for(int i=0;i<numDevs;i++)
      outDevs[i].output->SetGammaRamp(Flags, pRamp);
  dev->SetGammaRamp(iSwapChain, Flags, pRamp);
  return;
}

// Return matching refresh rate (emulate non-ex behaviour)
int IDirect3DDevice9SoftTH::matchRefresh(D3DPRESENT_PARAMETERS *pp)
{
  int numModes = d3d->GetAdapterModeCount(0, pp->BackBufferFormat);
  for(int m=0;m<numModes;m++) {
    D3DDISPLAYMODE mode;
    d3d->GetAdapterDisplayMode(0, &mode);
    if(mode.Width == pp->BackBufferWidth && mode.Height == pp->BackBufferHeight && mode.RefreshRate == pp->FullScreen_RefreshRateInHz)
      return mode.RefreshRate;
  }
  dbg("Mode %dx%d %dHz: Not found, using default refresh rate", pp->BackBufferWidth, pp->BackBufferHeight, pp->FullScreen_RefreshRateInHz);
  return 0; // No match found, use default
}

// Device reset - This is not needed with D3D9Ex but app can do it anyway
HRESULT IDirect3DDevice9SoftTH::Reset(D3DPRESENT_PARAMETERS* pp)
{
  dbg("RESET");
  dbg("DIAG Reset input: %dx%d %s refresh=%d hwnd=0x%08X",
      pp->BackBufferWidth, pp->BackBufferHeight,
      pp->Windowed?"Windowed":"Fullscreen",
      pp->FullScreen_RefreshRateInHz, pp->hDeviceWindow);

  memcpy(&lastPp, pp, sizeof(D3DPRESENT_PARAMETERS));
  if(!pp->Windowed &&
     (config.overrides.forceResolution || config.getNumAdditionalHeads() > 0) &&
     (pp->BackBufferWidth != config.main.renderResolution.x ||
      pp->BackBufferHeight != config.main.renderResolution.y)) {
    dbg("FSX multihead: forcing fullscreen device resolution from %dx%d to %dx%d",
        pp->BackBufferWidth, pp->BackBufferHeight,
        config.main.renderResolution.x, config.main.renderResolution.y);
    pp->BackBufferWidth = config.main.renderResolution.x;
    pp->BackBufferHeight = config.main.renderResolution.y;
  }

  const bool windowedMultiheadAtReset =
      (pp->Windowed && config.main.windowedMultihead &&
       config.getNumAdditionalHeads() > 0);
  wantedX = windowedMultiheadAtReset ? config.main.renderResolution.x : pp->BackBufferWidth;
  wantedY = windowedMultiheadAtReset ? config.main.renderResolution.y : pp->BackBufferHeight;
  if(windowedMultiheadAtReset)
    dbg("FSX: windowed multihead RESET to virtual %dx%d (app physical %dx%d)",
        wantedX, wantedY, pp->BackBufferWidth, pp->BackBufferHeight);
  dbg("DIAG Reset effective: app=%dx%d wanted=%dx%d mode=%s",
      pp->BackBufferWidth, pp->BackBufferHeight, wantedX, wantedY,
      pp->Windowed?"Windowed":"Fullscreen");

  //dbg("SoftTH: RESET (%dx%d)", pp->BackBufferWidth, pp->BackBufferHeight);
  if(windowedMultiheadAtReset ||
     (isSoftTHmode(pp->BackBufferWidth, pp->BackBufferHeight) && !pp->Windowed))
  {
    dbg("Reset: Multihead mode virtual %dx%d, app %dx%d, %s",
        wantedX, wantedY, pp->BackBufferWidth, pp->BackBufferHeight,
        pp->Windowed?"(windowed)":"(fullscreen)");

    D3DPRESENT_PARAMETERS newpp = *pp;
    adjustPP(&newpp);

    RECT r;
    GetWindowRect(hFocusWindow, &r);
    dbg("Device window  pre-reset: %s", strRect(&r));

    // D3D9 requires explicit render targets, depth surfaces and other
    // default-pool resources to be released before Reset().
    if(newbb) {
      dbg("Reset: releasing SoftTH buffers before device reset");
      destroyBuffers();
    }

    HRESULT ret = dev->Reset(&newpp);
    dbg("Reset: underlying device returned %s", getD3DError(ret));
    if(ret == D3D_OK)
    {
      pp->BackBufferFormat = newpp.BackBufferFormat;
      createBuffers();

      // Initialize overlay
      OVERLAY_INIT_BLOCK op;
      op.overlayVersion = OVERLAY_VERSION;
      op.dev = dev;
      initOverlay(&op);
    } else {
      dbg("Reset: FAILED");
    }
    GetWindowRect(hFocusWindow, &r);
    dbg("Device window  post-reset: %s", strRect(&r));
    return ret;
  } else {
    dbg("Reset: Singlehead mode %dx%d %dHz %s", pp->BackBufferWidth, pp->BackBufferHeight, pp->FullScreen_RefreshRateInHz, pp->Windowed?"(windowed)":"(fullscreen)");
    if(newbb)
      destroyBuffers();

    pp->FullScreen_RefreshRateInHz = matchRefresh(pp);

    // Create device
    bool fullscreen = (pp->Windowed==0);
    D3DDISPLAYMODEEX mode = {
      sizeof(D3DDISPLAYMODEEX),
      pp->BackBufferWidth,   pp->BackBufferHeight,
      pp->FullScreen_RefreshRateInHz,  pp->BackBufferFormat,
      D3DSCANLINEORDERING_PROGRESSIVE
    };

    // Keep classic D3D9 applications on classic Reset() semantics.
    HRESULT ret;
    if(isEx)
      ret = dev->ResetEx(pp, fullscreen?&mode:NULL);
    else
      ret = dev->Reset(pp);

    if(ret != D3D_OK)
      dbg("Reset: FAILED: %s", getD3DError(ret));
    else if(isEx) {
      HRESULT r = dev->CheckDeviceState(pp->hDeviceWindow);
      if(r != D3D_OK) {
        dbg("Reset: DeviceState: %s!", getD3DError(r));
      }
      if(r == S_PRESENT_MODE_CHANGED)
        ret = D3DERR_DEVICELOST;  // Emulate invalid mode
    }

    //dbg("RESET ret ret");
    return ret;
  }
  //dbg("RESET ret");
  return D3D_OK;
  //HRESULT ret = __super::Reset(pp);

  //return ret;
}

HRESULT IDirect3DDevice9SoftTH::Clear(DWORD Count,CONST D3DRECT* pRects,DWORD Flags,D3DCOLOR Color,float Z,DWORD Stencil)
{
  if(!newbb)
    return dev->Clear(Count, pRects, Flags, Color, Z, Stencil);

  HRESULT ret;
  if(Flags&D3DCLEAR_ZBUFFER && (pRects==NULL || Count == 0) && config.main.zClear && !squash)
  //if(Flags&D3DCLEAR_ZBUFFER && (pRects==NULL || Count == 0) && config.main.zClear)
  {
    // App is clearing depth buffer...
    IDirect3DSurface9 *cdepth;
    dev->GetDepthStencilSurface(&cdepth);
    if(!cdepth)
      return dev->Clear(Count, pRects, Flags, Color, Z, Stencil);
    D3DSURFACE_DESC d;
    cdepth->GetDesc(&d);
    cdepth->Release();
    //if(cdepth == newdepth)
    if(d.Width == newbbDesc.Width && d.Height == newbbDesc.Height)  // Size matches, assume it is the render depthbuffer
    {
      // ...which is our buffer!
      if(Flags-D3DCLEAR_ZBUFFER)
        ret = dev->Clear(Count, pRects, Flags-D3DCLEAR_ZBUFFER, Color, Z, Stencil);
      else
        ret = D3D_OK;

      // Clear whole buffer with Z = 0 so no rendering is done with depth testing
      // Then re-clear display areas with Z = Z so rendering is only done on visible areas
      D3DCALL( dev->Clear(0, 0, D3DCLEAR_ZBUFFER, 0, 0, 0) );
      const int bz = 16; // Border zone
      int maxx = config.main.renderResolution.x;
      int maxy = config.main.renderResolution.y;

      D3DRECT *r = new D3DRECT[numDevs+1];
      HEAD *ph = config.getPrimaryHead();
      r[0].x1 = max(ph->sourceRect.left-bz, 0);  r[0].y1 = max(ph->sourceRect.top-bz, 0);
      r[0].x2 = min(ph->sourceRect.right+bz, maxx); r[0].y2 = min(ph->sourceRect.bottom+bz, maxy);
      for(int i=0;i<numDevs;i++) {
        r[i+1].x1 = max(outDevs[i].cfg->sourceRect.left-bz, 0);  r[i+1].y1 = max(outDevs[i].cfg->sourceRect.top-bz, 0);
        r[i+1].x2 = min(outDevs[i].cfg->sourceRect.right+bz, maxx); r[i+1].y2 = min(outDevs[i].cfg->sourceRect.bottom+bz, maxy);
      }
      D3DCALL( dev->Clear(numDevs+1, r, D3DCLEAR_ZBUFFER, 0, Z, 0) );
      delete[] r;
      return ret;
    }
  }

  return dev->Clear(Count, pRects, Flags, Color, Z, Stencil);
}

void IDirect3DDevice9SoftTH::printMessage(char *first, ...)
{
  static char tmp[512];
  va_list  argptr;
  va_start (argptr, first);
  vsprintf (tmp, first, argptr);
  va_end   (argptr);
  sprintf(message, "  %s  ", tmp);
  messageTime = GetTickCount() + 3000;
}

void IDirect3DDevice9SoftTH::drawOverlay()
{
  IDirect3DSurface9 *lastbb, *lastd = NULL;
  D3DVIEWPORT9 lastvp;
  dev->GetRenderTarget(0, &lastbb);
  dev->SetRenderTarget(0, bb);

  dev->GetDepthStencilSurface(&lastd);
  dev->SetDepthStencilSurface(NULL);

  D3DVIEWPORT9 vp = {0, 0, bbDesc.Width, bbDesc.Height, 0, 1};
  dev->GetViewport(&lastvp);
  dev->SetViewport(&vp);

  if(GetTickCount() < messageTime) {
    // Draw on-screen message
    HEAD *ph = config.getPrimaryHead();
    drawing->beginDraw();
    const int height = 24;
    const int width = 400;
    RECT r = {0, bbDesc.Height - height, width, bbDesc.Height};
    //dbg("rectPRE: %s", strRect(&r));
    fontWide->DrawText(NULL, message, -1, &r, DT_CALCRECT, D3DCOLOR_ARGB(255, 255, 255, 255));
    //dbg("rectPOST: %s", strRect(&r));
    drawing->drawBox(r.left, r.top, r.right-r.left+24, height, 0xA0000000);
    r.bottom = bbDesc.Height;
    fontWide->DrawText(NULL, message, -1, &r, DT_CENTER|DT_VCENTER, D3DCOLOR_ARGB(255, 255, 255, 255));
    drawing->endDraw();
  }

  dev->SetDepthStencilSurface(lastd);
  dev->SetRenderTarget(0, lastbb);
  dev->SetViewport(&lastvp);
  lastbb->Release();
  if(lastd)
    lastd->Release();
}


// Draw a virtual-canvas calibration pattern that survives the normal
// SoftTH crop/scaling path. A phone photo of all displays can then be used to
// measure physical placement, overlap, crop and scale.
void IDirect3DDevice9SoftTH::drawCalibrationGrid(IDirect3DSurface9 *target)
{
  if(!config.debug.calibrationGrid || !target || !drawing)
    return;

  IDirect3DSurface9 *oldRT = NULL;
  IDirect3DSurface9 *oldDS = NULL;
  D3DVIEWPORT9 oldVP;
  dev->GetRenderTarget(0, &oldRT);
  dev->GetDepthStencilSurface(&oldDS);
  dev->GetViewport(&oldVP);

  dev->SetRenderTarget(0, target);
  dev->SetDepthStencilSurface(NULL);

  D3DVIEWPORT9 vp = {0, 0,
    (DWORD)config.main.renderResolution.x,
    (DWORD)config.main.renderResolution.y, 0.0f, 1.0f};
  dev->SetViewport(&vp);

  const int W = config.main.renderResolution.x;
  const int H = config.main.renderResolution.y;
  const int step = config.debug.calibrationGridStep;
  const int lw = config.debug.calibrationLineWidth;

  drawing->beginDraw();

  // Regular virtual-pixel grid.
  for(int x=0; x<W; x+=step)
    drawing->drawBox(x, 0, lw, H, 0x90FFFFFF);
  for(int y=0; y<H; y+=step)
    drawing->drawBox(0, y, W, lw, 0x90FFFFFF);

  // Strong center axes make scale/rotation obvious in a phone photo.
  drawing->drawBox(max(0, W/2-lw), 0, lw*2, H, 0xE0FFFFFF);
  drawing->drawBox(0, max(0, H/2-lw), W, lw*2, 0xE0FFFFFF);

  // Border each configured physical head in its sourceRect coordinates.
  if(config.debug.calibrationHeadBorders) {
    for(int hi=-1; hi<config.getNumAdditionalHeads(); hi++) {
      HEAD *h = (hi < 0) ? config.getPrimaryHead() : config.getHead(hi);
      RECT r = h->sourceRect;
      const int bw = max(3, lw*2);
      drawing->drawBox(r.left, r.top, max(1, r.right-r.left), bw, 0xFFFFFFFF);
      drawing->drawBox(r.left, max(r.top, r.bottom-bw), max(1, r.right-r.left), bw, 0xFFFFFFFF);
      drawing->drawBox(r.left, r.top, bw, max(1, r.bottom-r.top), 0xFFFFFFFF);
      drawing->drawBox(max(r.left, r.right-bw), r.top, bw, max(1, r.bottom-r.top), 0xFFFFFFFF);

#ifdef USE_D3DX
      if(config.debug.calibrationLabels && font) {
        char label[128];
        if(hi < 0)
          sprintf(label, "PRIMARY  src=%d,%d  %dx%d", r.left, r.top, r.right-r.left, r.bottom-r.top);
        else
          sprintf(label, "HEAD %d / devID %d  src=%d,%d  %dx%d",
                  hi+1, h->devID, r.left, r.top, r.right-r.left, r.bottom-r.top);

        RECT tr = {r.left+18, r.top+18, min(r.right-18, r.left+900), min(r.bottom-18, r.top+90)};
        drawing->drawBox(tr.left-8, tr.top-6, max(1, tr.right-tr.left+16), 30, 0xB0000000);
        font->DrawText(NULL, label, -1, &tr, DT_LEFT|DT_TOP, D3DCOLOR_ARGB(255,255,255,255));
      }
#endif
    }
  }

  drawing->endDraw();

  dev->SetViewport(&oldVP);
  dev->SetDepthStencilSurface(oldDS);
  dev->SetRenderTarget(0, oldRT);
  if(oldDS) oldDS->Release();
  if(oldRT) oldRT->Release();
}


// Present backbuffer contents
HRESULT IDirect3DDevice9SoftTH::PresentEx(CONST RECT* pSourceRect,CONST RECT* pDestRect,HWND hDestWindowOverride,CONST RGNDATA* pDirtyRegion, DWORD dwFlags)
{
  //dbg("frame: %d", frameCounter);
  dbgf("IDirect3DDevice9SoftTH Present %d", dev);
  if(pSourceRect || pDestRect || pDirtyRegion) {
    ONCE {
      dbg("Warning: Complex present detected (%d, %d, %d)", pSourceRect, pDestRect, pDirtyRegion);
    }
  }
  if(hDestWindowOverride) {
    ONCE {
      dbg("Warning: Present with window override detected (0x%08X, 0x%08X)", hDestWindowOverride, hFocusWindow);
    }

    DWORD presentPid = 0;
    GetWindowThreadProcessId(hDestWindowOverride, &presentPid);
    if(presentPid == GetCurrentProcessId() && hDestWindowOverride != hFocusWindow) {
      char cls[256] = {0};
      char title[256] = {0};
      GetClassNameA(hDestWindowOverride, cls, sizeof(cls));
      GetWindowTextA(hDestWindowOverride, title, sizeof(title));

      // #32770 is the standard Win32 dialog class used by FSX setup / exit
      // dialogs.  Earlier FSX-Win11 builds incorrectly treated these dialogs
      // as a render child and resized them to the 5760x2160 virtual canvas.
      // Never virtualize or resize an FSX dialog.
      if(!_stricmp(cls, "#32770")) {
        static HWND lastDialog = NULL;
        if(lastDialog != hDestWindowOverride) {
          RECT cr = {0}, wr = {0};
          GetClientRect(hDestWindowOverride, &cr);
          GetWindowRect(hDestWindowOverride, &wr);
          dbg("FSX dialog present override: hwnd=0x%08X title=<%s> client=%dx%d window=%dx%d -- leaving physical",
              hDestWindowOverride, title,
              cr.right-cr.left, cr.bottom-cr.top, wr.right-wr.left, wr.bottom-wr.top);
          lastDialog = hDestWindowOverride;
        }
        if(SoftTHPresentWindow == hDestWindowOverride)
          SoftTHPresentWindow = NULL;
      } else if(SoftTHPresentWindow != hDestWindowOverride) {
        SoftTHPresentWindow = hDestWindowOverride;

        RECT cr = {0}, wr = {0};
        GetClientRect(hDestWindowOverride, &cr);
        GetWindowRect(hDestWindowOverride, &wr);
        dbg("FSX present child: hwnd=0x%08X parent=0x%08X class=<%s> title=<%s> client=%dx%d window=%dx%d",
            hDestWindowOverride, GetParent(hDestWindowOverride), cls, title,
            cr.right-cr.left, cr.bottom-cr.top, wr.right-wr.left, wr.bottom-wr.top);

        if(newbb && wantedX > 0 && wantedY > 0) {
          dbg("FSX present child: resizing non-dialog child to virtual %dx%d", wantedX, wantedY);
          SetWindowPos(hDestWindowOverride, NULL, 0, 0, wantedX, wantedY,
                       SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        }
      }
    }
  }

  /*
  if(true)
  {
    RECT r;
    GetWindowRect(hFocusWindow, &r);
    if(r.left != wpos.left || r.right != wpos.right || r.top != wpos.top ||r.bottom != wpos.bottom)
    {
      // Window has moved, put it back
      // Non-ex d3d9 seems to do this automatically?
      dbg("Resetting device window position (%s)", strRect(&r));
      MoveWindow(hFocusWindow, wpos.left, wpos.top, wpos.right-wpos.left, wpos.bottom-wpos.top, false);
      GetWindowRect(hFocusWindow, &r);
      dbg("Device window moved to (%s)", strRect(&r));
    }
  }*/

  static bool hasSetLogoTime = false;
  if(!hasSetLogoTime)
  {
    for(int i=0;i<numDevs;i++)
      outDevs[i].output->setLogoShowTime(GetTickCount() + 4000);
    hasSetLogoTime = true;
  }

  // Collect timing data
  float t = (float)stopTimer()*10;
  for(int i=1;i<NUM_FPS_GRAPH;i++)
    fpsGraph[i-1] = fpsGraph[i];
  fpsGraph[NUM_FPS_GRAPH-1] = t>1?1:t;

  float avg = 0;
  for(int i=0;i<50;i++)
    avg += fpsGraph[NUM_FPS_GRAPH-1-i];
  avg /= 50;

  startTimer();
  lastFrameTime = t;

  bool notactive = false;
  HWND fgw = GetForegroundWindow();
  if(fgw != hFocusWindow)
  {
    dbgf("IDirect3DDevice9SoftTH::Present: Focus lost");
    bool isSoftTHOutput = false;
    for(int i=0;i<numDevs;i++)
      if(fgw == outDevs[i].output->getWindow())
        isSoftTHOutput = true;

    if(!isSoftTHOutput)
    {
      char foo[256] = {0};
      char cls[128] = {0};
      GetWindowTextA(fgw, foo, sizeof(foo));
      GetClassNameA(fgw, cls, sizeof(cls));

      DWORD fgPid = 0;
      if(fgw)
        GetWindowThreadProcessId(fgw, &fgPid);

      static HWND lastForegroundLogged = NULL;
      if(fgPid == GetCurrentProcessId()) {
        if(lastForegroundLogged != fgw) {
          dbg("FSX-owned foreground window: class=<%s> title=<%s>; suspending SoftTH outputs so dialog stays visible",
              cls, foo);
          lastForegroundLogged = fgw;
        }
        BringWindowToTop(fgw);
      } else {
        if(lastForegroundLogged != fgw) {
          dbg("Lost focus to: <%s>", foo);
          lastForegroundLogged = fgw;
        }
      }

      // Do not leave secondary SoftTH fullscreen windows covering Task Manager,
      // FSX setup, End Flight, or other dialogs.
      for(int i=0;i<numDevs;i++)
        outDevs[i].output->minimize();

      notactive = true;
    }
  }

  if(!newbb || notactive)
  {
    HRESULT ret;
    if(isEx || dwFlags != 0)
      ret = dev->PresentEx(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    else
      ret = dev->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
    dbgf("IDirect3DDevice9SoftTH::Present: notactive present result: %s", getD3DError(ret));
    if(ret == S_PRESENT_OCCLUDED || ret == S_PRESENT_MODE_CHANGED)
      ret = D3D_OK; // TODO: do this only if not an Ex device
    frameCounter++;
    return ret;
    //return dev->PresentEx(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
  }

#ifdef USE_D3DX

  // Make sure to render graph to BB
  IDirect3DSurface9 *ort = NULL, *od = NULL;
  D3DVIEWPORT9 ovp;
  dev->GetRenderTarget(0, &ort);
  dev->GetDepthStencilSurface(&od);
  dev->GetViewport(&ovp);
  dev->SetRenderTarget(0, newbb);
  dev->SetDepthStencilSurface(NULL);

#ifdef ENABLE_POSTPROCESS
  // Apply postprocessing
  if(postprocess && GetKeyState('O')>=0)
  {
    // Copy backbuffer to pp texture
    //dev->ColorFill(ppaux, NULL, 0xFF00FFFF);
    D3DCALL( dev->StretchRect(newbb, NULL, ppaux, NULL, D3DTEXF_NONE) );

    //dev->ColorFill(newbb, NULL, 0xFF00FFFF);
    drawing->beginDraw();

    bool doLumaFXAAPass = false;
    if(doLumaFXAAPass)
    {
      // FXAA requires luma thing
      drawing->drawTexturePP(0, 0, 5760, 1800, bbtex, "PostProcessFXAAlumaPass");
      // Copy result back to ppaux so it will be available in FXAA pass
      // TODO: copy only alpha data on correct area?
      D3DCALL( dev->StretchRect(newbb, NULL, ppaux, NULL, D3DTEXF_NONE) );
    }
    //drawing->drawTexturePP(1920, 0, 1920, 1200, bbtex);

    drawing->drawTexturePP(0, 0, 1800, 900, bbtex, "PostProcessTest");
/*
    if(GetKeyState('O')<0)
      drawing->drawTexturePP(0, 0, 1920*2, 1200, bbtex, "PostProcessFXAA");
    else
      drawing->drawTexturePP(0, 0, 5760, 1800, bbtex, "PostProcessFXAA");
*/
    //drawing->drawTextureLens(0, 0, newbbDesc.Width, newbbDesc.Height, bbtex);
    drawing->endDraw();
  }
#endif

  // Let overlay plugin draw its things
  OVERLAY_DRAW_BLOCK op;
  op.overlayVersion = OVERLAY_VERSION;
  op.width = newbbDesc.Width;
  op.height = newbbDesc.Height;
  op.dev = dev;
  op.newbb = newbb;

  op.primaryHead = config.getPrimaryHead();
  op.numHeads = config.getNumAdditionalHeads();
  for(int i=0;i<op.numHeads;i++)
  {
    op.extraHeads[i] = config.getHead(i);
  }

  overlayDoDraw(&op);

  if(showGraph) {
    // FPS graph
    dbgf("IDirect3DDevice9SoftTH::Present: draw graph");
    HEAD *ph = config.getPrimaryHead();
    drawing->beginDraw();

    drawing->drawGraph(ph->sourceRect.left+100, ph->sourceRect.top+100, (ph->sourceRect.right-ph->sourceRect.left)/2, 300, fpsGraph, NUM_FPS_GRAPH);

    char foo[256];
    sprintf(foo, "FPS: %d ", FPS);
    for(int i=0;i<numDevs;i++)
      sprintf(foo, "%s/ %d", foo, outDevs[i].output->getFPS() );

    dev->AddRef();
    sprintf(foo, "%s\nRefcount: %d", foo, dev->Release());

    RECT r = {0, 100, bbDesc.Width*3, 200};
    (squash?fontWide:font)->DrawText(NULL, foo, -1, &r, DT_CENTER|DT_TOP, D3DCOLOR_ARGB(255, 255, 255, 255));

    // Draw backlog
/*
    if(showLog) {

      int l = 0;
      char *line = NULL;
      do {
        line = getBackLogLine(l);
        if(line) {
          RECT r = {0, 100+(l*16), bbDesc.Width*3, 100+(l*16)+16};
          (squash?fontWide:font)->DrawText(NULL, line, -1, &r, DT_LEFT|DT_TOP, D3DCOLOR_ARGB(255, 255, 255, 255));
        }
        l++;
      } while(line);

    }
    */

    //dev->EndScene();

    drawing->endDraw();
  }

  static bool showDebugBars = false;
  if(showDebugBars) {
    drawing->beginDraw();

    int debugBarY = (int) ((float)config.main.renderResolution.y*((sin((float)GetTickCount()/500.0f)+1.57079633f) / 3.14159265f));
    drawing->drawBox(0, debugBarY, config.main.renderResolution.x, 100, 0xFFFFFFFF);

    drawing->endDraw();
  }

  dev->SetRenderTarget(0, ort);
  dev->SetDepthStencilSurface(od);
  dev->SetViewport(&ovp);
  if(ort) ort->Release();
  if(od) od->Release();
#endif

  IDirect3DSurface9 *srcbuf = newbb;  // Source buffer for head stretchrects
  if(srcbuf == bb)
    dbg("ERROR: srcbuf == bb??");

  // Photo-calibration overlay is intentionally drawn before head cropping so
  // every monitor shows its exact slice of one shared virtual coordinate grid.
  drawCalibrationGrid(srcbuf);

  static bool doStall = config.main.smoothing;

  // Legacy SoftTH debug hotkeys are dangerous in FSX because S/W/G/E/B are
  // normal simulator keys. Only honor them while the Application/Menu key is
  // explicitly held.
  if(ihGlobal.key(VK_APPLICATION))
  {
    if(ihGlobal.keyAsync('S'))
      squash = !squash, printMessage("Squash: %s", squash?"ON":"OFF"), Sleep(50);
    if(ihGlobal.keyAsync('W'))
      nocopy = !nocopy, printMessage("No copy: %s", nocopy?"ON":"OFF"), Sleep(50);
    if(ihGlobal.keyAsync('G')) {
      if(!showGraph) showGraph = true, showLog = false;
      else if(showGraph && !showLog) showLog = true;
      else if(showGraph && showLog) showGraph = showLog = false;
      printMessage("Graph: %s %s", showGraph?"ON":"OFF", showLog?"+log":"");
    }
    if(ihGlobal.keyAsync('E'))
      doStall = !doStall, printMessage("Smoothing: %s", doStall?"ON":"OFF");
    if(ihGlobal.keyAsync('B'))
      showDebugBars = !showDebugBars, printMessage("Debug bar: %s", showDebugBars?"ON":"OFF");
    if(ihGlobal.keyAsync('M'))
      dbg("--- mark ---");
    if(ihGlobal.keyAsync('N'))
      SetCursorPos(2880, 600);

    if(ihGlobal.keyAsync(VK_F4))
      exit(0);
  }
  if(ihGlobal.keyAsync(VK_SNAPSHOT))
    saveScreenshot();
  ihGlobal.resetAsyncKeys();

  // FPS counter
  fCounter++;
  if(GetTickCount() - fTimer >= 1000)
  {
    FPS = fCounter;
    fCounter = 0;
    fTimer = GetTickCount();
  }

  // Do not allow legacy debug modes to collapse a live FSX multihead layout.
  if(numDevs > 0 && (squash || nocopy)) {
    dbg("FSX multihead: suppressing legacy squash/nocopy state");
    squash = false;
    nocopy = false;
  }

  // Squash? Dump everything to primary head
  if(squash) {
    D3DCALL( dev->StretchRect(srcbuf, NULL, bb, NULL, D3DTEXF_LINEAR) );
    for(int i=0;i<numDevs;i++)
      outDevs[i].output->PresentOff();
    drawOverlay();
    frameCounter++;
    return dev->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
  }

  // nocopy? present only primary head
  if(nocopy) {
    HEAD *ph = config.getPrimaryHead();
    D3DCALL( dev->StretchRect(srcbuf, &ph->sourceRect, bb, isNullRect(&ph->destRect), D3DTEXF_LINEAR) );
    for(int i=0;i<numDevs;i++)
      outDevs[i].output->PresentOff();
    drawOverlay();
    frameCounter++;
    return dev->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
  }

  if(doStall) {
    // Stall the pipeline
    squeryA->Issue(D3DISSUE_END);
  }

  // Copy primary head rect
  dbgf("IDirect3DDevice9SoftTH::Present: primary head stretchrect");
  int tb = GetTickCount();
  HEAD *ph = config.getPrimaryHead();
  D3DCALL( dev->StretchRect(srcbuf, &ph->sourceRect, copybuf, isNullRect(&ph->destRect), D3DTEXF_LINEAR) );
  timeWarn(tb, 250, "Primary head stretchrect");
  // Copy secondary heads
  tb = GetTickCount();
  for(int i=0;i<numDevs;i++) {
    dbgf("IDirect3DDevice9SoftTH::Present: copy head %d", i);

    // FPS limiting
    if(outDevs[i].cfg->rateLimit && (GetTickCount() - outDevs[i].cfg->lastUpdate) <= outDevs[i].cfg->rateLimit)
    {
      outDevs[i].cfg->skipPresentNext = true;
      continue;
    }
    outDevs[i].cfg->lastUpdate = GetTickCount();
    outDevs[i].cfg->skipPresentNext = false;

    /*if(outDevs[i].cfg->rateLimit)
      dbg("update! %d", GetTickCount());*/

    if(outDevs[i].output->isReadyForData()) {
      if(outDevs[i].cfg->transportMethod == OUTMETHOD_NONLOCAL_24b) {
        // Copy srcbuf -> tempTex, then shader copy to remote surface
        IDirect3DSurface9 *s;
        outDevs[i].tempTex->GetSurfaceLevel(0, &s);
        D3DCALL( dev->StretchRect(srcbuf, &outDevs[i].cfg->sourceRect, s, NULL, D3DTEXF_LINEAR) );
        //D3DCALL( dev->StretchRect(srcbuf, &outDevs[i].cfg->sourceRect, s, isNullRect(&outDevs[i].cfg->destRect), D3DTEXF_LINEAR) );
        s->Release();
        copyPack24to32->surfCopyShader(outDevs[i].tempTex, curSurfA?outDevs[i].localSurfA:outDevs[i].localSurfB);
      } else if(outDevs[i].cfg->transportMethod == OUTMETHOD_NONLOCAL_16bDither) {
        // Same as above but with dither effect
        IDirect3DSurface9 *s;
        outDevs[i].tempTex->GetSurfaceLevel(0, &s);
        D3DCALL( dev->StretchRect(srcbuf, &outDevs[i].cfg->sourceRect, s, NULL, D3DTEXF_LINEAR) );
        //D3DCALL( dev->StretchRect(srcbuf, &outDevs[i].cfg->sourceRect, s, isNullRect(&outDevs[i].cfg->destRect), D3DTEXF_LINEAR) );
        s->Release();
        copyDither->surfCopyShader(outDevs[i].tempTex, curSurfA?outDevs[i].localSurfA:outDevs[i].localSurfB);
      } else {
        D3DCALL( dev->StretchRect(srcbuf, &outDevs[i].cfg->sourceRect, curSurfA?outDevs[i].localSurfA:outDevs[i].localSurfB, NULL, D3DTEXF_LINEAR) );
        //D3DCALL( dev->StretchRect(srcbuf, &outDevs[i].cfg->sourceRect, curSurfA?outDevs[i].localSurfA:outDevs[i].localSurfB, isNullRect(&outDevs[i].cfg->destRect), D3DTEXF_LINEAR) );
      }
    }
  }
  timeWarn(tb, 250, "Secondary head stretchrect");

  tb = GetTickCount();

  if(doStall) {
    // Stall the pipeline
    //squeryA->Issue(D3DISSUE_END);
    long long x = 0;
    while(squeryA->GetData((void *)&x, sizeof(long long), D3DGETDATA_FLUSH) == S_FALSE) YIELD_CPU;

  } else {
    // Signal this frame end...
    if(curSurfA)
      squeryA->Issue(D3DISSUE_END);
    else
      squeryB->Issue(D3DISSUE_END);

    // ...and wait for previous one to complete
    long long x = 0;
    if(curSurfA)
      while(squeryB->GetData((void *)&x, sizeof(long long), D3DGETDATA_FLUSH) == S_FALSE) YIELD_CPU;
    else
      while(squeryA->GetData((void *)&x, sizeof(long long), D3DGETDATA_FLUSH) == S_FALSE) YIELD_CPU;
  }
  timeWarn(tb, 250, "doStall");

  // Copy to secondary heads
  tb = GetTickCount();
  dbgf("IDirect3DDevice9SoftTH::Present: output heads");
  for(int i=0;i<numDevs;i++)
  {
    if(!outDevs[i].cfg->skipPresent)
    {
      outDevs[i].output->DoCopy(!outDevs[i].cfg->noSync, curSurfA);
    }
  }
  timeWarn(tb, 250, "Secondary head copy");

  //dev->WaitForVBlank(0);

  // Present primary head
  tb = GetTickCount();
  drawOverlay();
  dbgf("IDirect3DDevice9SoftTH::Present: present primary head");
  HRESULT ret;
  if(isEx || dwFlags != 0)
    ret = dev->PresentEx(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
  else
    ret = dev->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
  if(ret != D3D_OK) {
    dbg("Present failed: %s", getD3DError(ret));
    if(ret == S_PRESENT_OCCLUDED || ret == S_PRESENT_MODE_CHANGED)
      ret = D3D_OK; // TODO: do this only if not an Ex device
  }
  timeWarn(tb, 250, isEx?"Primary head PresentEx":"Primary head Present");

  // Present secondary heads
  tb = GetTickCount();
  dbgf("IDirect3DDevice9SoftTH::Present: present secondary heads");
  for(int i=0;i<numDevs;i++) {
    if(!outDevs[i].cfg->skipPresent)
    {
      outDevs[i].output->Present();
    }
  }
  timeWarn(tb, 250, "Secondary head Present");

  // Flip shared buffers
  tb = GetTickCount();
  dbgf("IDirect3DDevice9SoftTH::Present: flipbuffers");
  curSurfA=!curSurfA;
  D3DCALL( dev->StretchRect(copybuf, NULL, bb, NULL, D3DTEXF_NONE) );
  timeWarn(tb, 250, "Flip buffers");

  // Discard mode emulation - clear "backbuffer"
  tb = GetTickCount();
  d3dClearTarget(dev, newbb, newdepth);
  timeWarn(tb, 250, "d3dClearTarget");

  for(int i=0;i<numDevs;i++)
  {
    outDevs[i].cfg->skipPresent = false;
    // FPS limiting: We want to skip _next_ frame
    if(outDevs[i].cfg->skipPresentNext)
    {
      outDevs[i].cfg->skipPresent = true;
    }
    outDevs[i].cfg->skipPresentNext = false;
  }
  frameCounter++;
  return ret;
}

HRESULT IDirect3DDevice9SoftTH::GetSwapChain(UINT iSwapChain,IDirect3DSwapChain9** pSwapChain) {
  dbgf("IDirect3DDevice9SoftTH: GetSwapChain");
  HRESULT ret = dev->GetSwapChain(iSwapChain, pSwapChain);
  //if(newbb && ret == D3D_OK) {
  if(ret == D3D_OK) {
    (*pSwapChain) = new IDirect3DSwapChain9SoftTH(this, *pSwapChain);
  }
  return ret;
}

bool needQuirkRTT = false;
HRESULT IDirect3DDevice9SoftTH::CreateRenderTarget(UINT Width,UINT Height,D3DFORMAT Format,D3DMULTISAMPLE_TYPE MultiSample,DWORD MultisampleQuality,BOOL Lockable,IDirect3DSurface9** ppSurface,HANDLE* pSharedHandle)
{
  if(newbb) {
    static UINT lastW=0xffffffff,lastH=0xffffffff;
    static D3DFORMAT lastF=(D3DFORMAT)-1;
    if(Width!=lastW || Height!=lastH || Format!=lastF) {
      dbg("DIAG CreateRenderTarget: %dx%d %s ms=%d q=%d lock=%d",
          Width,Height,getMode(Format),MultiSample,MultisampleQuality,Lockable);
      lastW=Width; lastH=Height; lastF=Format;
    }
  }
  return __super::CreateRenderTarget(Width,Height,Format,MultiSample,MultisampleQuality,Lockable,ppSurface,pSharedHandle);
}

HRESULT IDirect3DDevice9SoftTH::CreateTexture(UINT Width,UINT Height,UINT Levels,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DTexture9** ppTexture,HANDLE* pSharedHandle)
{
  if(newbb && (Usage & D3DUSAGE_RENDERTARGET)) {
    static UINT lastW=0xffffffff,lastH=0xffffffff,lastL=0xffffffff;
    static DWORD lastU=0xffffffff;
    static D3DFORMAT lastF=(D3DFORMAT)-1;
    if(Width!=lastW || Height!=lastH || Levels!=lastL || Usage!=lastU || Format!=lastF) {
      dbg("DIAG CreateTexture RT: %dx%d levels=%d usage=0x%08X fmt=%s pool=%d",
          Width,Height,Levels,Usage,getMode(Format),Pool);
      lastW=Width; lastH=Height; lastL=Levels; lastU=Usage; lastF=Format;
    }
  }
  return __super::CreateTexture(Width,Height,Levels,Usage,Format,Pool,ppTexture,pSharedHandle);
}

HRESULT IDirect3DDevice9SoftTH::StretchRect(IDirect3DSurface9* pSourceSurface,CONST RECT* pSourceRect,IDirect3DSurface9* pDestSurface,CONST RECT* pDestRect,D3DTEXTUREFILTERTYPE Filter)
{
  if(newbb && pSourceSurface && pDestSurface) {
    D3DSURFACE_DESC s={0},d={0};
    if(SUCCEEDED(pSourceSurface->GetDesc(&s)) && SUCCEEDED(pDestSurface->GetDesc(&d))) {
      static UINT lsw=0xffffffff,lsh=0xffffffff,ldw=0xffffffff,ldh=0xffffffff;
      if(s.Width!=lsw || s.Height!=lsh || d.Width!=ldw || d.Height!=ldh) {
        dbg("DIAG StretchRect: src=%dx%d dst=%dx%d srcRect=%s dstRect=%s filter=%d",
            s.Width,s.Height,d.Width,d.Height,
            pSourceRect?strRect(pSourceRect):"<full>",
            pDestRect?strRect(pDestRect):"<full>",Filter);
        lsw=s.Width; lsh=s.Height; ldw=d.Width; ldh=d.Height;
      }
    }
  }
  return __super::StretchRect(pSourceSurface,pSourceRect,pDestSurface,pDestRect,Filter);
}

HRESULT IDirect3DDevice9SoftTH::SetRenderTarget(THIS_ DWORD RenderTargetIndex,IDirect3DSurface9* pRenderTarget)
{
  dbgf("IDirect3DDevice9SoftTH: SetRenderTarget %d %d", RenderTargetIndex, pRenderTarget);

  if(needQuirkRTT && pRenderTarget)
  {
    DWORD foo = 0;
    DWORD size = sizeof(foo);
    pRenderTarget->GetPrivateData(IID_SoftTHInvalidRTT, &foo, &size);
    if(foo)
    {
      ONCE dbg("QUIRK MODE: Program attempted to use invalid backbuffer for rendering, overriding");

      IDirect3DSurface9 *bbZero;
      dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bbZero);
      dev->SetRenderTarget(0, bbZero);
      bbZero->Release();
      return D3D_OK;
    }
  }

  IDirect3DSurface9 *actualRT = OriginalFromNewSurface(pRenderTarget);
  if(RenderTargetIndex == 0 && newbb && actualRT) {
    D3DSURFACE_DESC rd={0};
    if(SUCCEEDED(actualRT->GetDesc(&rd))) {
      static IDirect3DSurface9 *lastDiagRT=NULL;
      static UINT lastDiagW=0xffffffff,lastDiagH=0xffffffff;
      if(actualRT!=lastDiagRT || rd.Width!=lastDiagW || rd.Height!=lastDiagH) {
        dbg("DIAG SetRenderTarget0: ptr=0x%08X size=%dx%d fmt=%s %s",
            actualRT,rd.Width,rd.Height,getMode(rd.Format),
            actualRT==newbb?"<SoftTH-newbb>":"<other>");
        lastDiagRT=actualRT; lastDiagW=rd.Width; lastDiagH=rd.Height;
      }
    }
  }
  HRESULT ret = dev->SetRenderTarget(RenderTargetIndex, actualRT);
  if(SUCCEEDED(ret) && RenderTargetIndex == 0 && newbb && actualRT == newbb) {
    D3DVIEWPORT9 dvp;
    RECT dsc;
    if(SUCCEEDED(dev->GetViewport(&dvp)) && SUCCEEDED(dev->GetScissorRect(&dsc))) {
      static DWORD lastRW=0xffffffff, lastRH=0xffffffff;
      static LONG lastRR=LONG_MIN, lastRB=LONG_MIN;
      if(dvp.Width != lastRW || dvp.Height != lastRH ||
         dsc.right != lastRR || dsc.bottom != lastRB) {
        dbg("DIAG SetRenderTarget newbb: vp=%d,%d %dx%d sc=%ld,%ld,%ld,%ld wanted=%dx%d physical=%dx%d",
            dvp.X, dvp.Y, dvp.Width, dvp.Height,
            dsc.left, dsc.top, dsc.right, dsc.bottom,
            wantedX, wantedY, bbDesc.Width, bbDesc.Height);
        lastRW=dvp.Width; lastRH=dvp.Height; lastRR=dsc.right; lastRB=dsc.bottom;
      }
    }
  }
  // Restoring the virtual target may leave the underlying D3D9 device with
  // the native primary head's clip state. Repair only full-physical rectangles;
  // preserve FSX's deliberate smaller viewports and instrument scissor boxes.
  if(SUCCEEDED(ret) && RenderTargetIndex == 0 && newbb && actualRT == newbb &&
     ((DWORD)wantedX != bbDesc.Width || (DWORD)wantedY != bbDesc.Height))
  {
    D3DVIEWPORT9 vp;
    if(SUCCEEDED(dev->GetViewport(&vp)) && vp.X == 0 && vp.Y == 0 &&
       vp.Height == bbDesc.Height &&
       (vp.Width == bbDesc.Width || vp.Width == (DWORD)wantedX))
    {
      dbg("FSX fullscreen: correcting render-target viewport %dx%d -> %dx%d",
          vp.Width, vp.Height, wantedX, wantedY);
      vp.Width = wantedX;
      vp.Height = wantedY;
      D3DCALL(dev->SetViewport(&vp));
      static bool loggedVpRestore = false;
      if(!loggedVpRestore) {
        dbg("FSX fullscreen: restored full virtual viewport after render-target change");
        loggedVpRestore = true;
      }
    }
    RECT sc;
    if(SUCCEEDED(dev->GetScissorRect(&sc)) &&
       sc.left == 0 && sc.top == 0 &&
       sc.bottom == (LONG)bbDesc.Height &&
       (sc.right == (LONG)bbDesc.Width || sc.right == (LONG)wantedX))
    {
      dbg("FSX fullscreen: correcting render-target scissor %ldx%ld -> %dx%d",
          sc.right, sc.bottom, wantedX, wantedY);
      RECT virtualScissor = {0, 0, wantedX, wantedY};
      D3DCALL(dev->SetScissorRect(&virtualScissor));
      static bool loggedScissorRestore = false;
      if(!loggedScissorRestore) {
        dbg("FSX fullscreen: restored full virtual scissor after render-target change");
        loggedScissorRestore = true;
      }
    }
  }
  return ret;
}

// Must return our fake backbuffer to application
HRESULT IDirect3DDevice9SoftTH::GetBackBuffer(UINT iSwapChain,UINT iBackBuffer,D3DBACKBUFFER_TYPE Type,IDirect3DSurface9** ppBackBuffer)
{
  dbgf("IDirect3DDevice9SoftTH: GetBackBuffer %d", ppBackBuffer);
  HRESULT ret = dev->GetBackBuffer(iSwapChain, iBackBuffer, Type, ppBackBuffer);

  if(ret == D3D_OK && iBackBuffer > 0 && !newbb)
  {
    // QUIRK for Falcon 4 BMS
    // D3D9Ex doesn't like >0 backbuffer to be set as rendertarget - mark this as invalid for RT use
    // So in case it's set as RT later we can override it
    DWORD foo = 1;
    (*ppBackBuffer)->SetPrivateData(IID_SoftTHInvalidRTT, &foo, sizeof(DWORD), NULL);
    needQuirkRTT = true;
  }

  if(newbb && ret == D3D_OK) {
    (*ppBackBuffer)->Release();
    newbb->AddRef();
    *ppBackBuffer = newbb;
  }
  return ret;
}

HRESULT IDirect3DDevice9SoftTH::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode)
{
  dbgf("IDirect3DDevice9SoftTH::GetDisplayMode");
  HRESULT ret = dev->GetDisplayMode(iSwapChain, pMode);
  // Return our mode
  if(newbb)
  {
    pMode->Width = newbbDesc.Width;
    pMode->Height = newbbDesc.Height;
  }
  return ret;
}

// Keep a full-backbuffer viewport virtual while rendering to SoftTH's
// virtual render target.  FSX can re-apply the physical 1920x1080 viewport
// after device reset; map only that exact full physical viewport to the
// virtual size.  Smaller/sub-view viewports and offscreen render targets pass
// through unchanged.
HRESULT IDirect3DDevice9SoftTH::SetViewport(CONST D3DVIEWPORT9* pViewport)
{
  if(!pViewport || !newbb)
    if(pViewport) fsxCachedViewport = *pViewport;
  return dev->SetViewport(pViewport);

  bool virtualTarget = false;
  IDirect3DSurface9 *rt = NULL;
  if(dev->GetRenderTarget(0, &rt) == D3D_OK && rt) {
    virtualTarget = (rt == newbb);
    rt->Release();
  }

  if(virtualTarget) {
    static DWORD lastVX = 0xffffffff, lastVY = 0xffffffff;
    static DWORD lastVW = 0xffffffff, lastVH = 0xffffffff;
    if(pViewport->X != lastVX || pViewport->Y != lastVY ||
       pViewport->Width != lastVW || pViewport->Height != lastVH) {
      dbg("DIAG SetViewport virtual: x=%d y=%d w=%d h=%d wanted=%dx%d physical=%dx%d",
          pViewport->X, pViewport->Y, pViewport->Width, pViewport->Height,
          wantedX, wantedY, bbDesc.Width, bbDesc.Height);
      lastVX=pViewport->X; lastVY=pViewport->Y;
      lastVW=pViewport->Width; lastVH=pViewport->Height;
    }
  }

  // FSX-SE continues to express its scene and UI viewports in the native
  // primary-head coordinate system after SoftTH has switched to a larger
  // virtual render target. Map any viewport fully contained in that native
  // 1920x1080 coordinate space into the 5760x2160 virtual space.
  if(virtualTarget &&
     bbDesc.Width > 0 && bbDesc.Height > 0 &&
     ((DWORD)wantedX != bbDesc.Width || (DWORD)wantedY != bbDesc.Height) &&
     pViewport->X <= bbDesc.Width && pViewport->Y <= bbDesc.Height &&
     pViewport->Width <= bbDesc.Width && pViewport->Height <= bbDesc.Height &&
     pViewport->X + pViewport->Width <= bbDesc.Width &&
     pViewport->Y + pViewport->Height <= bbDesc.Height)
  {
    D3DVIEWPORT9 vp = *pViewport;
    vp.X = (DWORD)(((ULONGLONG)pViewport->X * (ULONGLONG)wantedX + bbDesc.Width/2) / bbDesc.Width);
    vp.Y = (DWORD)(((ULONGLONG)pViewport->Y * (ULONGLONG)wantedY + bbDesc.Height/2) / bbDesc.Height);
    vp.Width = (DWORD)(((ULONGLONG)pViewport->Width * (ULONGLONG)wantedX + bbDesc.Width/2) / bbDesc.Width);
    vp.Height = (DWORD)(((ULONGLONG)pViewport->Height * (ULONGLONG)wantedY + bbDesc.Height/2) / bbDesc.Height);

    static DWORD lastInX=0xffffffff,lastInY=0xffffffff,lastInW=0xffffffff,lastInH=0xffffffff;
    if(pViewport->X!=lastInX || pViewport->Y!=lastInY ||
       pViewport->Width!=lastInW || pViewport->Height!=lastInH) {
      dbg("FSX virtual viewport map: %d,%d %dx%d -> %d,%d %dx%d",
          pViewport->X,pViewport->Y,pViewport->Width,pViewport->Height,
          vp.X,vp.Y,vp.Width,vp.Height);
      lastInX=pViewport->X; lastInY=pViewport->Y;
      lastInW=pViewport->Width; lastInH=pViewport->Height;
    }
    fsxCachedViewport = vp;
    return dev->SetViewport(&vp);
  }

  return dev->SetViewport(pViewport);
}

// A full-size native scissor rectangle clips an otherwise valid virtual
// 5760x2160 scene to the upper-left 1920x1080 pixels. Preserve FSX's
// intentional smaller scissor boxes and offscreen passes.
HRESULT IDirect3DDevice9SoftTH::SetScissorRect(CONST RECT* pRect)
{
  if(!pRect || !newbb)
    return dev->SetScissorRect(pRect);

  IDirect3DSurface9 *rt = NULL;
  bool virtualTarget = false;
  if(SUCCEEDED(dev->GetRenderTarget(0, &rt)) && rt) {
    virtualTarget = (rt == newbb);
    rt->Release();
  }
  if(virtualTarget) {
    static LONG lastSL = LONG_MIN, lastST = LONG_MIN;
    static LONG lastSR = LONG_MIN, lastSB = LONG_MIN;
    if(pRect->left != lastSL || pRect->top != lastST ||
       pRect->right != lastSR || pRect->bottom != lastSB) {
      dbg("DIAG SetScissor virtual: l=%ld t=%ld r=%ld b=%ld wanted=%dx%d physical=%dx%d",
          pRect->left, pRect->top, pRect->right, pRect->bottom,
          wantedX, wantedY, bbDesc.Width, bbDesc.Height);
      lastSL=pRect->left; lastST=pRect->top;
      lastSR=pRect->right; lastSB=pRect->bottom;
    }
  }
  // Apply the same native->virtual coordinate mapping to scissor rectangles
  // that live completely inside the primary 1920x1080 coordinate space.
  if(virtualTarget &&
     bbDesc.Width > 0 && bbDesc.Height > 0 &&
     ((DWORD)wantedX != bbDesc.Width || (DWORD)wantedY != bbDesc.Height) &&
     pRect->left >= 0 && pRect->top >= 0 &&
     pRect->right >= pRect->left && pRect->bottom >= pRect->top &&
     pRect->right <= (LONG)bbDesc.Width &&
     pRect->bottom <= (LONG)bbDesc.Height)
  {
    RECT sc;
    sc.left   = (LONG)(((LONGLONG)pRect->left   * (LONGLONG)wantedX + bbDesc.Width/2) / bbDesc.Width);
    sc.top    = (LONG)(((LONGLONG)pRect->top    * (LONGLONG)wantedY + bbDesc.Height/2) / bbDesc.Height);
    sc.right  = (LONG)(((LONGLONG)pRect->right  * (LONGLONG)wantedX + bbDesc.Width/2) / bbDesc.Width);
    sc.bottom = (LONG)(((LONGLONG)pRect->bottom * (LONGLONG)wantedY + bbDesc.Height/2) / bbDesc.Height);

    static LONG lastL=LONG_MIN,lastT=LONG_MIN,lastR=LONG_MIN,lastB=LONG_MIN;
    if(pRect->left!=lastL || pRect->top!=lastT ||
       pRect->right!=lastR || pRect->bottom!=lastB) {
      dbg("FSX virtual scissor map: %ld,%ld,%ld,%ld -> %ld,%ld,%ld,%ld",
          pRect->left,pRect->top,pRect->right,pRect->bottom,
          sc.left,sc.top,sc.right,sc.bottom);
      lastL=pRect->left; lastT=pRect->top; lastR=pRect->right; lastB=pRect->bottom;
    }
    return dev->SetScissorRect(&sc);
  }
  return dev->SetScissorRect(pRect);
}

static bool fsxNearFloat(float v, float target)
{
  float tol = fabsf(target) * 0.0005f;
  if(tol < 0.000001f) tol = 0.000001f;
  return fabsf(v - target) <= tol;
}

static bool fsxLooksLikePhysicalScreenConstant(float v, float w, float h)
{
  if(w <= 0.0f || h <= 0.0f) return false;
  const float vals[] = {
    w, h, w*0.5f, h*0.5f,
    1.0f/w, 1.0f/h, 2.0f/w, 2.0f/h
  };
  for(int i=0;i<8;i++)
    if(fsxNearFloat(v, vals[i])) return true;
  return false;
}

HRESULT IDirect3DDevice9SoftTH::SetVertexShaderConstantF(UINT StartRegister,CONST float* pConstantData,UINT Vector4fCount)
{
  if(newbb && pConstantData && Vector4fCount && bbDesc.Width && bbDesc.Height) {
    bool hit=false;
    UINT hitVec=0;
    for(UINT v=0; v<Vector4fCount && !hit; v++) {
      for(int k=0;k<4;k++) {
        if(fsxLooksLikePhysicalScreenConstant(pConstantData[v*4+k], (float)bbDesc.Width, (float)bbDesc.Height)) {
          hit=true; hitVec=v; break;
        }
      }
    }
    if(hit) {
      fsxCachedPhysicalVSConstReg = (int)(StartRegister + hitVec);
      static bool seen[256]={false};
      const UINT r=StartRegister+hitVec;
      if(r<256 && !seen[r]) {
        seen[r]=true;
        const float *p=pConstantData+hitVec*4;
        dbg("DIAG CACHE VS physical constant c%d=(%g,%g,%g,%g)", r,p[0],p[1],p[2],p[3]);
      }
    }
  }
  return dev->SetVertexShaderConstantF(StartRegister,pConstantData,Vector4fCount);
}

HRESULT IDirect3DDevice9SoftTH::SetPixelShaderConstantF(UINT StartRegister,CONST float* pConstantData,UINT Vector4fCount)
{
  if(newbb && pConstantData && Vector4fCount && bbDesc.Width && bbDesc.Height) {
    bool hit=false;
    UINT hitVec=0;
    for(UINT v=0; v<Vector4fCount && !hit; v++) {
      for(int k=0;k<4;k++) {
        if(fsxLooksLikePhysicalScreenConstant(pConstantData[v*4+k], (float)bbDesc.Width, (float)bbDesc.Height)) {
          hit=true; hitVec=v; break;
        }
      }
    }
    if(hit) {
      fsxCachedPhysicalPSConstReg = (int)(StartRegister + hitVec);
      static bool seen[224]={false};
      const UINT r=StartRegister+hitVec;
      if(r<224 && !seen[r]) {
        seen[r]=true;
        const float *p=pConstantData+hitVec*4;
        dbg("DIAG CACHE PS physical constant c%d=(%g,%g,%g,%g)", r,p[0],p[1],p[2],p[3]);
      }
    }
  }
  return dev->SetPixelShaderConstantF(StartRegister,pConstantData,Vector4fCount);
}

HRESULT IDirect3DDevice9SoftTH::SetFVF(DWORD FVF)
{
  fsxCachedFVF = FVF;
  fsxCachedUsingDecl = false;
  fsxCachedPositionT = ((FVF & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW);
  return dev->SetFVF(FVF);
}

HRESULT IDirect3DDevice9SoftTH::SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl)
{
  fsxCachedUsingDecl = true;
  fsxCachedPositionT = false;
  if(pDecl) {
    D3DVERTEXELEMENT9 elems[MAXD3DDECLLENGTH+1];
    UINT n=MAXD3DDECLLENGTH+1;
    if(SUCCEEDED(pDecl->GetDeclaration(elems,&n))) {
      for(UINT i=0;i<n;i++) {
        if(elems[i].Stream==0xFF) break;
        if(elems[i].Usage==D3DDECLUSAGE_POSITIONT) {
          fsxCachedPositionT=true;
          break;
        }
      }
    }
  }
  return dev->SetVertexDeclaration(pDecl);
}

HRESULT IDirect3DDevice9SoftTH::SetVertexShader(IDirect3DVertexShader9* pShader)
{
  fsxCachedVertexShader = (pShader != NULL);
  return dev->SetVertexShader(pShader);
}

HRESULT IDirect3DDevice9SoftTH::SetStreamSource(UINT StreamNumber,IDirect3DVertexBuffer9* pStreamData,UINT OffsetInBytes,UINT Stride)
{
  if(StreamNumber==0) fsxCachedStride0=Stride;
  return dev->SetStreamSource(StreamNumber, OriginalFromNewVBuffer(pStreamData), OffsetInBytes, Stride);
}

HRESULT IDirect3DDevice9SoftTH::SetTexture(DWORD Stage,IDirect3DBaseTexture9* pTexture)
{
  if(Stage==0) {
    fsxCachedTex0W=fsxCachedTex0H=0;
    if(pTexture && pTexture->GetType()==D3DRTYPE_TEXTURE) {
      IDirect3DTexture9 *tex=NULL;
      if(SUCCEEDED(pTexture->QueryInterface(IID_IDirect3DTexture9,(void**)&tex)) && tex) {
        D3DSURFACE_DESC td;
        if(SUCCEEDED(tex->GetLevelDesc(0,&td))) {
          fsxCachedTex0W=td.Width;
          fsxCachedTex0H=td.Height;
        }
        tex->Release();
      }
    }
  }
  return dev->SetTexture(Stage, OriginalFromNewTexture(pTexture));
}

void IDirect3DDevice9SoftTH::diagFSXCachedDraw(const char *kind, D3DPRIMITIVETYPE primitiveType, UINT primitiveCount)
{
  if(!newbb) return;

  const bool interesting =
      fsxCachedPositionT ||
      fsxCachedPhysicalVSConstReg >= 0 ||
      (primitiveCount >= 100 && fsxCachedVertexShader);
  if(!interesting) return;

  unsigned long long hash=1469598103934665603ULL;
  #define MIX_CACHE(v) do { hash ^= (unsigned long long)(v); hash *= 1099511628211ULL; } while(0)
  MIX_CACHE(primitiveType); MIX_CACHE(primitiveCount>=100);
  MIX_CACHE(fsxCachedFVF); MIX_CACHE(fsxCachedUsingDecl); MIX_CACHE(fsxCachedPositionT);
  MIX_CACHE(fsxCachedVertexShader); MIX_CACHE(fsxCachedStride0);
  MIX_CACHE(fsxCachedTex0W); MIX_CACHE(fsxCachedTex0H);
  MIX_CACHE(fsxCachedPhysicalVSConstReg+1); MIX_CACHE(fsxCachedPhysicalPSConstReg+1);
  MIX_CACHE(fsxCachedViewport.Width); MIX_CACHE(fsxCachedViewport.Height);
  #undef MIX_CACHE

  static unsigned long long seen[80]={0};
  static int seenCount=0;
  for(int i=0;i<seenCount;i++) if(seen[i]==hash) return;
  if(seenCount>=80) return;
  seen[seenCount++]=hash;

  dbg("DIAG CACHE DRAW %s pt=%d prim=%d vp=%dx%d VS=%d FVF=0x%08X decl=%d posT=%d stride=%d tex0=%dx%d physVS=c%d physPS=c%d",
      kind,primitiveType,primitiveCount,
      fsxCachedViewport.Width,fsxCachedViewport.Height,
      fsxCachedVertexShader?1:0,fsxCachedFVF,fsxCachedUsingDecl?1:0,
      fsxCachedPositionT?1:0,fsxCachedStride0,
      fsxCachedTex0W,fsxCachedTex0H,
      fsxCachedPhysicalVSConstReg,fsxCachedPhysicalPSConstReg);
}

void IDirect3DDevice9SoftTH::diagFSXDrawState(const char *kind, D3DPRIMITIVETYPE primitiveType, UINT primitiveCount, UINT strideHint)
{
  if(!newbb) return;

  IDirect3DSurface9 *rt=NULL;
  if(FAILED(dev->GetRenderTarget(0,&rt)) || !rt) return;
  const bool isVirtual=(rt==newbb);
  rt->Release();
  if(!isVirtual) return;

  D3DVIEWPORT9 vp={0};
  RECT sc={0};
  dev->GetViewport(&vp);
  dev->GetScissorRect(&sc);

  DWORD scissorEnable=0, fvf=0;
  dev->GetRenderState(D3DRS_SCISSORTESTENABLE,&scissorEnable);
  dev->GetFVF(&fvf);

  IDirect3DVertexShader9 *vs=NULL;
  dev->GetVertexShader(&vs);
  const bool hasVS=(vs!=NULL);
  if(vs) vs->Release();

  bool positionT=false;
  IDirect3DVertexDeclaration9 *decl=NULL;
  if(SUCCEEDED(dev->GetVertexDeclaration(&decl)) && decl) {
    D3DVERTEXELEMENT9 elems[MAXD3DDECLLENGTH+1];
    UINT n=MAXD3DDECLLENGTH+1;
    if(SUCCEEDED(decl->GetDeclaration(elems,&n))) {
      for(UINT i=0;i<n;i++)
        if(elems[i].Usage==D3DDECLUSAGE_POSITIONT) {positionT=true;break;}
    }
    decl->Release();
  }
  if((fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW)
    positionT=true;

  UINT stride=strideHint;
  if(!stride) {
    IDirect3DVertexBuffer9 *vb=NULL;
    UINT off=0;
    if(SUCCEEDED(dev->GetStreamSource(0,&vb,&off,&stride)) && vb)
      vb->Release();
  }

  UINT texW=0,texH=0;
  IDirect3DBaseTexture9 *baseTex=NULL;
  if(SUCCEEDED(dev->GetTexture(0,&baseTex)) && baseTex) {
    if(baseTex->GetType()==D3DRTYPE_TEXTURE) {
      IDirect3DTexture9 *tex=NULL;
      if(SUCCEEDED(baseTex->QueryInterface(IID_IDirect3DTexture9,(void**)&tex)) && tex) {
        D3DSURFACE_DESC td={0};
        if(SUCCEEDED(tex->GetLevelDesc(0,&td))) {texW=td.Width;texH=td.Height;}
        tex->Release();
      }
    }
    baseTex->Release();
  }

  // Large geometry calls identify the 3D scene; POSITIONT/XYZRHW identifies
  // screen-space compositing/UI draws that bypass the viewport transform.
  if(primitiveCount < 100 && !positionT) return;

  unsigned long long hash=1469598103934665603ULL;
  #define MIX_DIAG(v) do { hash ^= (unsigned long long)(v); hash *= 1099511628211ULL; } while(0)
  MIX_DIAG(primitiveType); MIX_DIAG(primitiveCount>=100);
  MIX_DIAG(vp.X); MIX_DIAG(vp.Y); MIX_DIAG(vp.Width); MIX_DIAG(vp.Height);
  MIX_DIAG(sc.right); MIX_DIAG(sc.bottom); MIX_DIAG(scissorEnable);
  MIX_DIAG(hasVS); MIX_DIAG(fvf); MIX_DIAG(positionT); MIX_DIAG(stride); MIX_DIAG(texW); MIX_DIAG(texH);
  #undef MIX_DIAG

  static unsigned long long seen[64]={0};
  static int seenCount=0;
  for(int i=0;i<seenCount;i++) if(seen[i]==hash) return;
  if(seenCount>=64) return;
  seen[seenCount++]=hash;

  dbg("DIAG DRAW %s pt=%d prim=%d vp=%d,%d %dx%d sc=%ld,%ld,%ld,%ld scEn=%d VS=%d FVF=0x%08X posT=%d stride=%d tex0=%dx%d",
      kind,primitiveType,primitiveCount,
      vp.X,vp.Y,vp.Width,vp.Height,
      sc.left,sc.top,sc.right,sc.bottom,
      scissorEnable?1:0,hasVS?1:0,fvf,positionT?1:0,stride,texW,texH);
}

// FSX can restore a physical 1920x1080 viewport through a raw D3D9
// state block. State-block Apply() calls bypass this wrapper's SetViewport(),
// so repair that exact full-physical viewport immediately before every draw
// on SoftTH's virtual render target. Smaller UI viewports remain untouched.
void IDirect3DDevice9SoftTH::repairFSXVirtualViewportForDraw()
{
  if(!newbb ||
     ((DWORD)wantedX == bbDesc.Width && (DWORD)wantedY == bbDesc.Height))
    return;

  IDirect3DSurface9 *rt = NULL;
  if(FAILED(dev->GetRenderTarget(0, &rt)) || !rt)
    return;
  const bool virtualTarget = (rt == newbb);
  rt->Release();
  if(!virtualTarget)
    return;

  // A raw IDirect3DStateBlock9::Apply() bypasses SoftTH's SetViewport and
  // SetScissorRect wrappers.  Repair viewport and scissor INDEPENDENTLY:
  // FSX can restore only the native 1920x1080 scissor while leaving the
  // already-correct 5760x2160 viewport intact.
  D3DVIEWPORT9 vp;
  if(SUCCEEDED(dev->GetViewport(&vp)) &&
     vp.X == 0 && vp.Y == 0 &&
     vp.Width == bbDesc.Width && vp.Height == bbDesc.Height)
  {
    const DWORD oldW = vp.Width;
    const DWORD oldH = vp.Height;
    vp.Width = (DWORD)wantedX;
    vp.Height = (DWORD)wantedY;
    if(SUCCEEDED(dev->SetViewport(&vp))) {
      static bool loggedDrawRepair = false;
      if(!loggedDrawRepair) {
        dbg("FSX draw guard: repaired full physical viewport %dx%d -> %dx%d",
            oldW, oldH, vp.Width, vp.Height);
        loggedDrawRepair = true;
      }
    }
  }

  RECT sc;
  if(SUCCEEDED(dev->GetScissorRect(&sc)) &&
     sc.left == 0 && sc.top == 0 &&
     sc.right == (LONG)bbDesc.Width && sc.bottom == (LONG)bbDesc.Height)
  {
    RECT virtualScissor = {0, 0, wantedX, wantedY};
    if(SUCCEEDED(dev->SetScissorRect(&virtualScissor))) {
      static bool loggedDrawScissorRepair = false;
      if(!loggedDrawScissorRepair) {
        D3DVIEWPORT9 nowVp;
        if(SUCCEEDED(dev->GetViewport(&nowVp)))
          dbg("FSX draw guard: repaired independent physical scissor %dx%d -> %dx%d (viewport %dx%d)",
              bbDesc.Width, bbDesc.Height, wantedX, wantedY,
              nowVp.Width, nowVp.Height);
        else
          dbg("FSX draw guard: repaired independent physical scissor %dx%d -> %dx%d",
              bbDesc.Width, bbDesc.Height, wantedX, wantedY);
        loggedDrawScissorRepair = true;
      }
    }
  }
}

static UINT fsxUPVertexCount(D3DPRIMITIVETYPE pt, UINT primitiveCount)
{
  switch(pt) {
    case D3DPT_POINTLIST:     return primitiveCount;
    case D3DPT_LINELIST:      return primitiveCount * 2;
    case D3DPT_LINESTRIP:     return primitiveCount + 1;
    case D3DPT_TRIANGLELIST:  return primitiveCount * 3;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN:   return primitiveCount + 2;
    default:                   return 0;
  }
}

bool IDirect3DDevice9SoftTH::scaleFSXPhysicalScreenVertices(const void *src, UINT vertexCount, UINT stride, BYTE **scaledCopy)
{
  if(scaledCopy) *scaledCopy = NULL;
  if(!scaledCopy || !src || !newbb || vertexCount < 3 || vertexCount > 64 ||
     stride < 8 || stride > 256 ||
     bbDesc.Width == 0 || bbDesc.Height == 0 ||
     ((DWORD)wantedX == bbDesc.Width && (DWORD)wantedY == bbDesc.Height))
    return false;

  // Only touch draws going directly to SoftTH's virtual backbuffer.
  IDirect3DSurface9 *rt = NULL;
  if(FAILED(dev->GetRenderTarget(0, &rt)) || !rt)
    return false;
  const bool virtualTarget = (rt == newbb);
  rt->Release();
  if(!virtualTarget)
    return false;

  // Determine where POSITIONT / XYZRHW lives in each vertex.
  UINT posOffset = 0;
  bool positionT = false;

  DWORD fvf = 0;
  if(SUCCEEDED(dev->GetFVF(&fvf)) &&
     (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW)
  {
    positionT = true;
    posOffset = 0;
  }
  else
  {
    IDirect3DVertexDeclaration9 *decl = NULL;
    if(SUCCEEDED(dev->GetVertexDeclaration(&decl)) && decl) {
      D3DVERTEXELEMENT9 elems[MAXD3DDECLLENGTH+1];
      UINT n = MAXD3DDECLLENGTH+1;
      if(SUCCEEDED(decl->GetDeclaration(elems, &n))) {
        for(UINT i=0; i<n; i++) {
          if(elems[i].Stream == 0xFF) break;
          if(elems[i].Stream == 0 &&
             elems[i].Usage == D3DDECLUSAGE_POSITIONT &&
             elems[i].Type == D3DDECLTYPE_FLOAT4)
          {
            positionT = true;
            posOffset = elems[i].Offset;
            break;
          }
        }
      }
      decl->Release();
    }
  }

  if(!positionT || posOffset + sizeof(float)*2 > stride)
    return false;

  // We are looking for the exact failure seen in the SoftTH screenshot:
  // a full-screen quad whose screen-space coordinates still span the native
  // 1920x1080 primary head even though the render target is 5760x2160.
  float minX=1.0e30f, minY=1.0e30f, maxX=-1.0e30f, maxY=-1.0e30f;
  const BYTE *bytes = (const BYTE*)src;
  for(UINT i=0; i<vertexCount; i++) {
    const float *p = (const float*)(bytes + i*stride + posOffset);
    const float x=p[0], y=p[1];
    if(x < minX) minX=x; if(x > maxX) maxX=x;
    if(y < minY) minY=y; if(y > maxY) maxY=y;
  }

  const float pw=(float)bbDesc.Width, ph=(float)bbDesc.Height;
  if(minX < -4.0f || minY < -4.0f ||
     maxX > pw+4.0f || maxY > ph+4.0f ||
     (maxX-minX) < pw*0.80f || (maxY-minY) < ph*0.80f)
    return false;

  // Strong safety guard: only scale a physical-size screen quad when it is
  // sampling a virtual-size texture.  This targets FSX's final scene
  // composition pass without stretching ordinary 2D dialogs or gauges.
  UINT texW=0, texH=0;
  IDirect3DBaseTexture9 *baseTex = NULL;
  if(SUCCEEDED(dev->GetTexture(0, &baseTex)) && baseTex) {
    if(baseTex->GetType() == D3DRTYPE_TEXTURE) {
      IDirect3DTexture9 *tex = NULL;
      if(SUCCEEDED(baseTex->QueryInterface(IID_IDirect3DTexture9, (void**)&tex)) && tex) {
        D3DSURFACE_DESC td;
        if(SUCCEEDED(tex->GetLevelDesc(0, &td))) {
          texW=td.Width; texH=td.Height;
        }
        tex->Release();
      }
    }
    baseTex->Release();
  }
  if(texW != (UINT)wantedX || texH != (UINT)wantedY)
    return false;

  const size_t total = (size_t)vertexCount * (size_t)stride;
  BYTE *copy = new BYTE[total];
  memcpy(copy, src, total);

  const float sx=(float)wantedX/pw;
  const float sy=(float)wantedY/ph;
  const bool halfPixel = (minX < -0.25f || minY < -0.25f);

  for(UINT i=0; i<vertexCount; i++) {
    float *p = (float*)(copy + i*stride + posOffset);
    if(halfPixel) {
      p[0] = (p[0] + 0.5f) * sx - 0.5f;
      p[1] = (p[1] + 0.5f) * sy - 0.5f;
    } else {
      p[0] *= sx;
      p[1] *= sy;
    }
  }

  static bool logged = false;
  if(!logged) {
    dbg("FSX screen-space scene composite: scaled POSITIONT quad %.1f,%.1f-%.1f,%.1f -> virtual %dx%d (texture %dx%d)",
        minX,minY,maxX,maxY,wantedX,wantedY,texW,texH);
    logged = true;
  }

  *scaledCopy = copy;
  return true;
}

HRESULT IDirect3DDevice9SoftTH::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType,UINT StartVertex,UINT PrimitiveCount)
{
  repairFSXVirtualViewportForDraw();
  diagFSXCachedDraw("DP",PrimitiveType,PrimitiveCount);
  return dev->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
}

HRESULT IDirect3DDevice9SoftTH::DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType,INT BaseVertexIndex,UINT MinVertexIndex,UINT NumVertices,UINT startIndex,UINT primCount)
{
  repairFSXVirtualViewportForDraw();
  diagFSXCachedDraw("DIP",PrimitiveType,primCount);
  return dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
}

HRESULT IDirect3DDevice9SoftTH::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType,UINT PrimitiveCount,CONST void* pVertexStreamZeroData,UINT VertexStreamZeroStride)
{
  repairFSXVirtualViewportForDraw();
  const UINT oldStride=fsxCachedStride0;
  fsxCachedStride0=VertexStreamZeroStride;
  diagFSXCachedDraw("DPUP",PrimitiveType,PrimitiveCount);
  fsxCachedStride0=oldStride;
  return dev->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
}

HRESULT IDirect3DDevice9SoftTH::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType,UINT MinVertexIndex,UINT NumVertices,UINT PrimitiveCount,CONST void* pIndexData,D3DFORMAT IndexDataFormat,CONST void* pVertexStreamZeroData,UINT VertexStreamZeroStride)
{
  repairFSXVirtualViewportForDraw();
  const UINT oldStride=fsxCachedStride0;
  fsxCachedStride0=VertexStreamZeroStride;
  diagFSXCachedDraw("DIPUP",PrimitiveType,PrimitiveCount);
  fsxCachedStride0=oldStride;
  return dev->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount,
                                     pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
}

// FOV/zoom overrides for fixed-function perspective transformations.
// Applied to the complete virtual view before it is sliced into heads;
// this does not change the existing head mapping or physical resolutions.
HRESULT IDirect3DDevice9SoftTH::SetTransform(D3DTRANSFORMSTATETYPE State,CONST D3DMATRIX* pMatrix)
{
  if(State != D3DTS_PROJECTION || !pMatrix || !newbb)
    return dev->SetTransform(State, pMatrix);

  const bool useLegacyFov = config.overrides.FOVForceHorizontal || config.overrides.FOVForceVertical;
  const float zoom = config.overrides.zoomOutMultiplier;
  if(!useLegacyFov && zoom <= 1.0f)
    return dev->SetTransform(State, pMatrix);

  D3DVIEWPORT9 vp;
  if(FAILED(dev->GetViewport(&vp)))
    return dev->SetTransform(State, pMatrix);
  if(vp.Width != newbbDesc.Width || vp.Height != newbbDesc.Height)
    return dev->SetTransform(State, pMatrix);

  // Do not modify 2D/orthographic UI projection transforms.
  const bool perspective = (fabsf(pMatrix->_34) > 0.001f && fabsf(pMatrix->_44) < 0.1f);

  // Copy the application's const matrix: legacy SoftTH wrote through
  // a const pointer, which can modify caller memory or fault on readonly pages.
  D3DMATRIX corrected = *pMatrix;
  if(config.overrides.FOVForceHorizontal)
    corrected._11 /= 3.0f;
  if(config.overrides.FOVForceVertical)
    corrected._22 *= (float)newbbDesc.Width / (float)newbbDesc.Height;

  if(perspective && zoom > 1.0f) {
    corrected._11 /= zoom;
    corrected._22 /= zoom;
    static bool loggedZoom = false;
    if(!loggedZoom) {
      dbg("FSX Zoom-out: factor %.2f applied to virtual perspective", zoom);
      loggedZoom = true;
    }
  }

  return dev->SetTransform(State, &corrected);
}


void IDirect3DDevice9SoftTH::saveScreenshot()
{
  char ext[4];
  strcpy(ext, config.main.screenshotFormat);

  D3DXIMAGE_FILEFORMAT fmt = (D3DXIMAGE_FILEFORMAT) -1;
  if(!_strcmpi(ext, "jpg")) fmt = D3DXIFF_JPG;
  if(!_strcmpi(ext, "bmp")) fmt = D3DXIFF_BMP;
  if(!_strcmpi(ext, "png")) fmt = D3DXIFF_PNG;
  if(fmt == -1) {
    dbg("Unrecognized screenshot format: <%s>, supported formats: jpg,bmp,png", ext);
    fmt = D3DXIFF_PNG;
    strcpy(ext, "png");
  }

  // Parse time string
  char timestr[64];
  struct tm * timeinfo;
  time_t rawtime;
  time(&rawtime);
  timeinfo = localtime(&rawtime);
  strftime(timestr, 64 ,"%Y_%m_%d_%H_%M_%S", timeinfo);

  char mydocs[256] = "";
  SHGetFolderPath(0, CSIDL_PERSONAL, NULL, NULL, mydocs);

  char path[256];
  sprintf(path, "%s\\SoftTH\\Screenshots\\SoftTH_%s_%s_%d.%s", mydocs, processName(), timestr, GetTickCount()%60, ext);
  D3DXSaveSurfaceToFile(path, fmt, newbb, NULL, NULL);
  dbg("Saved screenshot: <%s>", path);
  printMessage("Saved screenshot: <%s>", path);
}


bool isSoftTHmode(int w, int h)
{
  if(w == config.main.renderResolution.x && h == config.main.renderResolution.y)
    return true;
  return false;
}

static int detectManufacturer(int devID, int headID)
{
  int result = MANF_UNKNOWN;
  IDirect3D9Ex *d3d = NULL;
  dllDirect3DCreate9Ex(D3D_SDK_VERSION, &d3d);
  if(!d3d) {
    dbg("detectManufacturer: Direct3DCreate9Ex failed!", devID);
    return result;
  }

  D3DADAPTER_IDENTIFIER9 id;
  d3d->GetAdapterIdentifier(devID, NULL, &id);

  char orig[256];
  char lowr[256];
  ZeroMemory(orig, 256);
  ZeroMemory(lowr, 256);
  strncpy(orig, id.Description, 128);
  for(int i=0;i<128;i++)
  {
    lowr[i] = tolower(orig[i]);
  }

  if(strstr(orig, "AMD") || strstr(lowr, "radeon"))
  {
    result = MANF_AMD;
  }
  if(strstr(orig, "NVIDIA") || strstr(lowr, "geforce") || strstr(lowr, "quadro"))
  {
    result = MANF_NVIDIA;
  }

  if(result == MANF_NVIDIA) dbg("Head %d manufacturer: NVIDIA", headID);
  if(result == MANF_AMD) dbg("Head %d manufacturer: AMD", headID);
  if(result == MANF_UNKNOWN) dbg("Head %d manufacturer: Unknown", headID);

  d3d->Release();
  return result;
}

static void detectTransportMethods()
{
  bool gotLocal = false;
  bool allLocal = true;
  // Do transportmethod autodetection
  for(int i=0;i<config.getNumAdditionalHeads();i++) {
    HEAD *h = config.getHead(i);
    if(h->transportMethod == OUTMETHOD_AUTO)
      h->transportMethod = detectTransportType(h->devID);
    if(h->transportMethod == OUTMETHOD_LOCAL)
      gotLocal = true;

    if(h->transportMethod != OUTMETHOD_LOCAL)
      allLocal = false;
  }

  if(!gotLocal && config.getNumAdditionalHeads()>0)
    dbg("WARNING: All secondary heads are non-local.");

  if(allLocal && config.main.smoothing)
  {
    dbg("All heads are local, auto-disabling smoothing");
    config.main.smoothing = false;
  }

  // Detect device manufacturers (NVIDIA/AMD)
  config.getPrimaryHead()->manufacturer = detectManufacturer(0, 0);

  for(int i=0;i<config.getNumAdditionalHeads();i++) {
    HEAD *h = config.getHead(i);
    h->manufacturer = detectManufacturer(h->devID, i+1);
  }
}

static int detectTransportType(int devID)
{

  IDirect3D9Ex *d3d = NULL;
  IDirect3DDevice9Ex *dev = NULL, *devSec = NULL;
  D3DPRESENT_PARAMETERS pp;
  int method = OUTMETHOD_BLIT;

  dllDirect3DCreate9Ex(D3D_SDK_VERSION, &d3d);
  if(!d3d) {
    dbg("Using transport method BLIT for device %d (Direct3DCreate9Ex failed)", devID);
    return method;
  }

  WINDOWPARAMS wp = {0, 0, 10, 10, NULL, NULL, false, NULL};
  wp.hWnd = NULL;
  _beginthread(windowHandler, 0, (void*) &wp);
  while(!wp.hWnd)
    Sleep(1);

  // Create device
  pp.BackBufferWidth = 32;
  pp.BackBufferHeight = 32;
  pp.BackBufferFormat = D3DFMT_A8R8G8B8;
  pp.BackBufferCount = 0;
  pp.MultiSampleType = D3DMULTISAMPLE_NONE;
  pp.MultiSampleQuality = 0;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = wp.hWnd;
  pp.Windowed  = true;
  pp.EnableAutoDepthStencil = false;
  pp.AutoDepthStencilFormat = D3DFMT_UNKNOWN;
  pp.Flags = 0;
  pp.FullScreen_RefreshRateInHz = 0;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

  DWORD flags = D3DCREATE_MIXED_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE;
  d3d->CreateDeviceEx(devID, D3DDEVTYPE_HAL, wp.hWnd, flags, &pp, NULL, &dev);
  if(!dev) {
    DestroyWindow(wp.hWnd);
    dbg("Using transport method BLIT for device %d (CreateDeviceEx failed)", devID);
    return method;
  }

  // Device creation succeeded - we can at least use NONLOCAL
  method = nonlocalMethodDefault;

  // Create primary head device + shared rendertargets
  IDirect3DSurface9 *surfA = NULL, *surfB = NULL;
  HANDLE shareHandle = NULL;
  dev->CreateRenderTargetEx(32, 32, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, false, &surfA, &shareHandle, NULL);
  if(!surfA || !shareHandle) {
    DestroyWindow(wp.hWnd);
    dev->Release();
    d3d->Release();
    dbg("Using transport method NONLOCAL for device %d (CreateRenderTargetEx failed)", devID);
    return method;
  }

  d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wp.hWnd, flags, &pp, NULL, &devSec);
  if(!devSec) {
    DestroyWindow(wp.hWnd);
    surfA->Release();
    dev->Release();
    d3d->Release();
    dbg("Using transport method NONLOCAL for device %d (CreateDeviceEx step 2 failed)", devID);
    return method;
  }

  devSec->CreateRenderTargetEx(32, 32, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, false, &surfB, &shareHandle, NULL);
  if(!surfB) {
    surfA->Release();
    dev->Release();
    devSec->Release();
    d3d->Release();
    DestroyWindow(wp.hWnd);
    dbg("Using transport method NONLOCAL for device %d (CreateRenderTargetEx step 2 failed)", devID);
    return method;
  }

  // Sharing succeeded! local device!
  method = OUTMETHOD_LOCAL;
  surfB->Release();
  surfA->Release();
  dev->Release();
  devSec->Release();
  d3d->Release();
  DestroyWindow(wp.hWnd);
  DestroyWindow(wp.hWnd);
  dbg("Using transport method LOCAL for device %d", devID);
  return method;
}

bool IDirect3DDevice9SoftTH::validateSettings(IDirect3D9Ex *d3d)
{
  dbg("Validating settings");
  D3DCAPS9 caps;
  d3d->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);

  int resX = config.main.renderResolution.x;
  int resY = config.main.renderResolution.y;

  if(resX < 2 || resY < 2) {
		ShowMessage("Invalid resolution (%dx%d)", resX, resY);
		return false;
  }

	if((DWORD)resX > caps.MaxTextureWidth) {
		ShowMessage("Requested resolution is too wide for device (%d > %d)", resX, caps.MaxTextureWidth);
		return false;
	}
	if((DWORD)resY > caps.MaxTextureHeight) {
		ShowMessage("Requested resolution is too tall for device (%d > %d)", resY, caps.MaxTextureHeight);
		return false;
	}

  int numDevs = config.getNumAdditionalHeads();
  for(int i=-1;i<numDevs;i++)
  {
    HEAD *h = i==-1?config.getPrimaryHead():config.getHead(i);
    if(i != -1)
    {
      if(h->devID == D3DADAPTER_DEFAULT) {
		    ShowMessage("Head %d device ID invalid (DevID 0 reserved for head_primary)", i+1);
		    return false;
	    }

      if(!d3d->GetAdapterMonitor(h->devID)) {
        ShowMessage("Head %d device ID invalid (%d)", i+1, h->devID);
		    return false;
	    }

      for(int o=0;o<numDevs;o++) {
        HEAD *hh = config.getHead(o);
        if(hh->devID == h->devID && o!=i) {
          ShowMessage("Head %d and %d: Device ID conflict (%d)", i+1,o+1, h->devID);
		      return false;
        }
      }
    }

    if(h->sourceRect.left < 0 || h->sourceRect.top < 0 || h->sourceRect.right > resX || h->sourceRect.bottom > resY) {
      ShowMessage("Head %d: sourceRect (%dx%d-%dx%d) outside\nrender surface (%dx%d)", i+1, h->sourceRect.left, h->sourceRect.top, h->sourceRect.right, h->sourceRect.bottom, resX, resY);
      return false;
    }

    dbg("Head %d: OK", i+1);
  }

  return true;
}
