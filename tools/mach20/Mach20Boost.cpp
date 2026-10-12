// FSX Mach Boost (experimental) — external FSX-SE SimConnect velocity override.
// No aircraft.cfg or SoftTH DLL changes. Build with MSVC x86 on Windows.
// Public-domain dedication for this small standalone utility.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <cwchar>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

static constexpr DWORD OBJ_USER = 0;
static constexpr DWORD DATA_FLOAT64 = 4;
static constexpr DWORD PERIOD_SECOND = 4;
static constexpr DWORD DEF_READ = 100;
static constexpr DWORD DEF_WRITE = 101;
static constexpr DWORD REQ_READ = 100;
static constexpr DWORD EVT_THROTTLE_SET = 200;
static constexpr DWORD EVT_THROTTLE_INCR = 201;
static constexpr DWORD EVT_THROTTLE_DECR = 202;
static constexpr DWORD EVTFLAG_PRIORITY = 0x10;
static constexpr DWORD PRIORITY_HIGHEST = 1;
static constexpr DWORD RECV_SIMOBJECT_DATA = 8;
static constexpr DWORD RECV_EXCEPTION = 1;
static constexpr DWORD RECV_QUIT = 3;
static constexpr double FPM = 3.28083989501312;

struct ScRecv { DWORD size, version, id; };
struct ScObjectData {
  DWORD size, version, id;
  DWORD requestId, objectId, defineId, flags, entryNumber, outOf, defineCount, data;
};
struct ScException {
  DWORD size, version, id, exception, sendId, index;
};
struct Telemetry {
  double tempC = 15.0;
  double mach = 0.0;
  double altitudeFt = 0.0;
  double bodyZfps = 0.0;
  double throttlePct = 0.0;
};
struct ScApi {
  HMODULE dll = nullptr;
  HANDLE connection = nullptr;
  HRESULT (WINAPI *Open)(HANDLE*,LPCSTR,HWND,DWORD,HANDLE,DWORD) = nullptr;
  HRESULT (WINAPI *Close)(HANDLE) = nullptr;
  HRESULT (WINAPI *Add)(HANDLE,DWORD,const char*,const char*,DWORD,float,DWORD) = nullptr;
  HRESULT (WINAPI *Request)(HANDLE,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD) = nullptr;
  HRESULT (WINAPI *Set)(HANDLE,DWORD,DWORD,DWORD,DWORD,DWORD,const void*) = nullptr;
  HRESULT (WINAPI *Next)(HANDLE,ScRecv**,DWORD*) = nullptr;
  HRESULT (WINAPI *MapEvent)(HANDLE,DWORD,const char*) = nullptr;
  HRESULT (WINAPI *Transmit)(HANDLE,DWORD,DWORD,DWORD,DWORD,DWORD) = nullptr;
};
static ScApi sc;

// Mach-boost mode is entirely external to SoftTH; it reuses FSX's
// native F1/F4 throttle commands. Only F2/F3 and the wheel receive
// 2%-per-step treatment when boosted.
static volatile LONG gBoost = 0;
static volatile LONG gWheelActive = 1;
static volatile LONG gWheelSteps = 0;
static volatile LONG gFineKeySteps = 0;
static volatile LONG gEndStop = -1; // 0=F1, 1=F4, -1=none
static HHOOK gMouseHook = nullptr;
static HHOOK gKeyHook = nullptr;
static int gWheelRemainder = 0;
static bool gEventMappingReady = false;
static volatile DWORD gFSXPid = 0;
static volatile LONG gWheelSeen = 0;
static volatile LONG gWheelAccepted = 0;
static volatile LONG gWheelSuppressed = 0;
static volatile LONG gWheelFallbackEvents = 0;
static volatile LONG gWheelSimConnectEvents = 0;
static volatile LONG gMouseScrollClicks = 0;

// The foreground process is cached from the process list. Do NOT call
// OpenProcess/QueryFullProcessImageName for every wheel event: low-level
// hooks have a strict timeout and can be silently removed by Windows.
static DWORD discoverFSXProcess() {
  HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
  if(snap==INVALID_HANDLE_VALUE)return 0;
  PROCESSENTRY32W ent={};
  ent.dwSize=sizeof(ent);
  DWORD pid=0;
  if(Process32FirstW(snap,&ent)) {
    do {
      if(_wcsicmp(ent.szExeFile,L"fsx_softth.exe")==0 ||
         _wcsicmp(ent.szExeFile,L"fsx.exe")==0) {
        pid=ent.th32ProcessID;
        break;
      }
    } while(Process32NextW(snap,&ent));
  }
  CloseHandle(snap);
  return pid;
}
static bool focusedFSX() {
  const DWORD fsxPid=gFSXPid;
  if(!fsxPid)return false;
  HWND window=GetForegroundWindow();
  if(!window)return false;
  DWORD fgPid=0;
  GetWindowThreadProcessId(window,&fgPid);
  if(fgPid!=fsxPid)return false;
  char cls[32]={};
  GetClassNameA(window,cls,sizeof(cls));
  return strcmp(cls,"#32770")!=0 && strcmp(cls,"#32768")!=0;
}
static LRESULT CALLBACK MouseHook(int n,WPARAM msg,LPARAM data) {
  if(n==HC_ACTION && msg==WM_MOUSEWHEEL) {
    InterlockedIncrement(&gWheelSeen);
    if(gWheelActive && focusedFSX()) {
      const auto* info=reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
      const int delta=static_cast<short>(HIWORD(info->mouseData));
      gWheelRemainder+=delta;
      const LONG detents=gWheelRemainder/120;
      gWheelRemainder-=detents*120;
      if(detents) {
        InterlockedExchangeAdd(&gWheelSteps,detents);
        InterlockedExchangeAdd(&gMouseScrollClicks,detents);
        InterlockedIncrement(&gWheelAccepted);
      }
      // Suppress native wheel zoom whenever this controller is active over
      // FSX. Do not require SimConnect or mouse hit testing in this callback.
      InterlockedIncrement(&gWheelSuppressed);
      return 1;
    }
  }
  return CallNextHookEx(gMouseHook,n,msg,data);
}
static LRESULT CALLBACK KeyHook(int n,WPARAM msg,LPARAM data) {
  if(n==HC_ACTION && (msg==WM_KEYDOWN || msg==WM_SYSKEYDOWN) &&
     focusedFSX() &&
     !(GetAsyncKeyState(VK_CONTROL)&0x8000) &&
     !(GetAsyncKeyState(VK_MENU)&0x8000)) {
    const auto* info=reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
    if(info->flags & LLKHF_INJECTED)
      return CallNextHookEx(gKeyHook,n,msg,data);
    if(info->vkCode==VK_F1) {
      InterlockedExchange(&gEndStop,0);
    } else if(info->vkCode==VK_F4) {
      InterlockedExchange(&gEndStop,1);
    } else if(gBoost && (info->vkCode==VK_F2 || info->vkCode==VK_F3)) {
      InterlockedExchangeAdd(&gFineKeySteps,info->vkCode==VK_F3?1:-1);
      return 1;
    }
  }
  return CallNextHookEx(gKeyHook,n,msg,data);
}
static bool sendThrottleKey(UINT vk) {
  INPUT keys[2]={};
  for(int i=0;i<2;++i) {
    keys[i].type=INPUT_KEYBOARD;
    keys[i].ki.wVk=static_cast<WORD>(vk);
    keys[i].ki.dwFlags=i==1?KEYEVENTF_KEYUP:0;
  }
  return SendInput(2,keys,sizeof(INPUT))==2;
}

static bool emitThrottle(DWORD eventId,DWORD value=0) {
  if(!sc.connection || !gEventMappingReady)return false;
  return SUCCEEDED(sc.Transmit(sc.connection,OBJ_USER,eventId,value,
                               PRIORITY_HIGHEST,EVTFLAG_PRIORITY));
}


template<typename P>
bool fetch(P& slot, const char* name) {
  slot = reinterpret_cast<P>(GetProcAddress(sc.dll, name));
  if(!slot) fprintf(stderr, "SimConnect export not found: %s\n", name);
  return slot != nullptr;
}
// FSX-SE's client is a side-by-side WinSxS assembly. On many Win11
// installations LoadLibrary("SimConnect.dll") alone is NOT sufficient.
// Probe installed versions by activating a real SxS manifest first.
static bool getProgramDirectory(std::wstring& dir) {
  wchar_t exe[MAX_PATH]={};
  DWORD n=GetModuleFileNameW(NULL,exe,MAX_PATH);
  if(!n || n >= MAX_PATH) return false;
  std::wstring s(exe);
  const size_t pos=s.find_last_of(L"\\/");
  if(pos==std::wstring::npos)return false;
  dir=s.substr(0,pos);
  return true;
}
static bool isExistingFile(const std::wstring& path) {
  DWORD attrs=GetFileAttributesW(path.c_str());
  return attrs!=INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}
static bool usableSimConnect(HMODULE dll) {
  const char* names[]={
    "SimConnect_Open","SimConnect_Close",
    "SimConnect_AddToDataDefinition",
    "SimConnect_RequestDataOnSimObject",
    "SimConnect_SetDataOnSimObject",
    "SimConnect_GetNextDispatch"
  };
  for(const char* name : names)
    if(!GetProcAddress(dll,name)) return false;
  return true;
}
static bool tryDll(HMODULE dll, const char* description) {
  if(!dll) return false;
  if(!usableSimConnect(dll)) {
    printf("SimConnect client at %s lacks required API symbols; skipping.\n",description);
    FreeLibrary(dll);
    return false;
  }
  sc.dll=dll;
  printf("Loaded FSX SimConnect: %s\n",description);
  return true;
}
static bool loadWithManifest(const std::wstring& path,const char* version) {
  if(!isExistingFile(path))return false;
  ACTCTXW act={};
  act.cbSize=sizeof(act);
  act.lpSource=path.c_str();
  HANDLE handle=CreateActCtxW(&act);
  if(handle==INVALID_HANDLE_VALUE) {
    printf("SimConnect %s activation unavailable (Windows error %lu).\n",
           version,static_cast<unsigned long>(GetLastError()));
    return false;
  }
  ULONG_PTR cookie=0;
  bool okay=false;
  if(ActivateActCtx(handle,&cookie)) {
    HMODULE dll=LoadLibraryW(L"SimConnect.dll");
    const DWORD err=dll ? ERROR_SUCCESS : GetLastError();
    okay=tryDll(dll,version);
    if(!okay && err!=ERROR_SUCCESS)
      printf("SimConnect %s load failed (Windows error %lu).\n",
             version,static_cast<unsigned long>(err));
    DeactivateActCtx(0,cookie);
  }
  ReleaseActCtx(handle);
  return okay;
}
static std::wstring installerPath() {
  // The user currently has FSX in the conventional Steam directory.
  // Checking this path is a diagnostic only; we NEVER launch MSI silently.
  wchar_t pf[MAX_PATH]={};
  std::wstring base;
  if(SUCCEEDED(SHGetFolderPathW(NULL,CSIDL_PROGRAM_FILESX86,NULL,SHGFP_TYPE_CURRENT,pf)))
    base=pf;
  if(base.empty())base=L"C:\\Program Files (x86)";
  return base+L"\\Steam\\steamapps\\common\\FSX\\SDK\\Core Utilities Kit\\SimConnect SDK\\lib\\SimConnect.msi";
}
static bool loadSimConnect() {
  std::wstring dir;
  getProgramDirectory(dir);

  // First prefer a SimConnect.dll explicitly installed beside this EXE,
  // then versions registered in WinSxS via an activation manifest.
  if(tryDll(LoadLibraryW(L"SimConnect.dll"),"normal Windows DLL lookup"))
    goto loaded;

  if(!dir.empty()) {
    struct Candidate { const wchar_t* manifest;const char* label; };
    const Candidate versions[] = {
      {L"SimConnect_62615.manifest","FSX-Steam 10.0.62615.0"},
      {L"SimConnect_62613.manifest","FSX-Steam 10.0.62613.0"},
      {L"SimConnect_61259.manifest","FSX-SP2 10.0.61259.0"},
      {L"SimConnect_61242.manifest","FSX-SP1 10.0.61242.0"}
    };
    for(const Candidate& ver:versions) {
      if(loadWithManifest(dir+L"\\"+ver.manifest,ver.label))
        goto loaded;
    }
  }
  // Last-chance fallback for valid WinSxS installs without proper SxS
  // assembly binding. Keep each load attempt in diagnostic output.
  {
    wchar_t win[MAX_PATH]={};
    if(GetWindowsDirectoryW(win,MAX_PATH)) {
      std::wstring root=std::wstring(win)+L"\\WinSxS\\";
      WIN32_FIND_DATAW data={};
      HANDLE search=FindFirstFileW((root+L"x86_microsoft.flightsimulator.simconnect_*").c_str(),&data);
      if(search!=INVALID_HANDLE_VALUE) {
        do {
          if(!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))continue;
          const std::wstring dllPath=root+data.cFileName+L"\\SimConnect.dll";
          if(!isExistingFile(dllPath))continue;
          HMODULE dll=LoadLibraryExW(dllPath.c_str(),NULL,LOAD_WITH_ALTERED_SEARCH_PATH);
          if(dll && usableSimConnect(dll)) {
            sc.dll=dll;
            wprintf(L"Loaded FSX SimConnect via physical WinSxS path: %ls\n",dllPath.c_str());
            break;
          }
          const DWORD err=dll ? ERROR_SUCCESS : GetLastError();
          if(dll)FreeLibrary(dll);
          wprintf(L"Unable to load %ls (Windows error %lu).\n",
                  dllPath.c_str(),static_cast<unsigned long>(err));
        } while(FindNextFileW(search,&data));
        FindClose(search);
      }
    }
    if(sc.dll)goto loaded;
  }
  puts("");
  puts("ERROR: 32-bit FSX SimConnect client not accessible.");
  puts("This can mean the client is missing OR the WinSxS assembly is not registered.");
  puts("In this same folder, run Repair_SimConnect.cmd to locate FSX's own MSI.");
  puts("It asks before running Windows Installer and never downloads DLLs.");
  {
    const std::wstring path=installerPath();
    if(isExistingFile(path))
      wprintf(L"Found bundled MSI: %ls\n",path.c_str());
    else
      puts("If the FSX SDK/SimConnect MSI is missing, verify FSX files in Steam.");
  }
  return false;

loaded:
  return fetch(sc.Open,"SimConnect_Open") &&
         fetch(sc.Close,"SimConnect_Close") &&
         fetch(sc.Add,"SimConnect_AddToDataDefinition") &&
         fetch(sc.Request,"SimConnect_RequestDataOnSimObject") &&
         fetch(sc.Set,"SimConnect_SetDataOnSimObject") &&
         fetch(sc.Next,"SimConnect_GetNextDispatch") &&
         fetch(sc.MapEvent,"SimConnect_MapClientEventToSimEvent") &&
         fetch(sc.Transmit,"SimConnect_TransmitClientEvent");
}

static void disconnect() {
  gEventMappingReady=false;
  InterlockedExchange(&gWheelSteps,0);
  InterlockedExchange(&gFineKeySteps,0);
  if(sc.connection) {
    sc.Close(sc.connection);
    sc.connection = nullptr;
  }
}
static bool connect() {
  if(FAILED(sc.Open(&sc.connection,"FSX Mach Boost",NULL,0,NULL,0))) {
    sc.connection = nullptr;
    return false;
  }
  const struct { const char* name; const char* unit; } vars[] = {
    {"AMBIENT TEMPERATURE","celsius"},
    {"AIRSPEED MACH","mach"},
    {"PLANE ALTITUDE","feet"},
    {"VELOCITY BODY Z","feet per second"},
    {"GENERAL ENG THROTTLE LEVER POSITION:1","percent"}
  };
  for(const auto& v: vars)
    if(FAILED(sc.Add(sc.connection,DEF_READ,v.name,v.unit,DATA_FLOAT64,0.0f,0xFFFFFFFF))) {
      puts("Unable to define read-only flight telemetry."); disconnect(); return false;
    }
  if(FAILED(sc.Add(sc.connection,DEF_WRITE,"VELOCITY BODY Z","feet per second",
                   DATA_FLOAT64,0.0f,0xFFFFFFFF))) {
    puts("Unable to define aircraft speed control."); disconnect(); return false;
  }
  gEventMappingReady =
      SUCCEEDED(sc.MapEvent(sc.connection,EVT_THROTTLE_SET,"THROTTLE_SET")) &&
      SUCCEEDED(sc.MapEvent(sc.connection,EVT_THROTTLE_INCR,"THROTTLE_INCR")) &&
      SUCCEEDED(sc.MapEvent(sc.connection,EVT_THROTTLE_DECR,"THROTTLE_DECR"));
  if(!gEventMappingReady) puts("WARNING: throttle event mapping unavailable.");
  if(FAILED(sc.Request(sc.connection,REQ_READ,DEF_READ,OBJ_USER,PERIOD_SECOND,0,0,0,0))) {
    puts("Unable to request live flight telemetry."); disconnect(); return false;
  }
  puts("Connected to FSX SimConnect.");
  return true;
}
static double soundSpeedFeetPerSecond(double tempC) {
  // Speed of sound in dry air, sqrt(gamma * R * absoluteTemperature).
  const double kelvin = std::max(160.0,std::min(330.0,tempC+273.15));
  return std::sqrt(1.4 * 287.05 * kelvin) * FPM;
}
static bool writeSpeed(double fps) {
  return sc.connection && SUCCEEDED(sc.Set(sc.connection,DEF_WRITE,OBJ_USER,0,0,
                                           sizeof(fps),&fps));
}
int main() {
  SetConsoleOutputCP(CP_UTF8);
  puts("FSX MACH 20 POWER-THROTTLE 2.1 — WHEEL REPAIR");
  puts("Separate program; no SoftTH changes or aircraft file edits.");
  puts("Mach boost OFF by default; airplane throttle remains normal.");
  puts("Ctrl+Alt+F4 = Mach 20 engine-power mode ON / OFF");
  puts("F1 = 0 percent throttle; F4 = 100 percent throttle");
  puts("F2/F3 = normal steps OFF; 2 percent steps ON");
  puts("Mouse wheel = native throttle steps OFF; 2 percent steps ON");
  puts("Ctrl+Alt+W = wheel mapping ON / OFF");
  puts("Ctrl+Alt+D = diagnostic status and wheel event counters");
  puts("Wheel control works in normal mode even without SimConnect.");
  puts("Ctrl+Alt+Q = quit");
  puts("");
  puts("NOTE: Only a simulation speed override, NOT accurate Mach 20 physics.");
  puts("IMPORTANT: EXIT your previous Mach Boost program and wheel AutoHotkey");
  puts("script so there are no competing throttle controllers.");
  puts("");
  const bool simconnectAvailable=loadSimConnect();
  if(!simconnectAvailable) {
    puts("SimConnect not available: Mach boost disabled, but the mouse");
    puts("wheel can still control normal FSX throttle via synthetic F2/F3.");
    puts("Repair_SimConnect.cmd remains available in this folder.");
  }

  const UINT mods=MOD_CONTROL|MOD_ALT|MOD_NOREPEAT;
  const struct {int id;UINT key;} bindings[]={
    {20,VK_F4},{21,'W'},{22,'Q'},{23,'D'}
  };
  for(const auto& h: bindings) {
    if(!RegisterHotKey(NULL,h.id,mods,h.key))
      printf("WARNING: Ctrl+Alt+%s shortcut unavailable (Windows %lu).\n",
             h.id==20?"F4":h.id==21?"W":h.id==22?"Q":"D",
             static_cast<unsigned long>(GetLastError()));
  }
  gMouseHook=SetWindowsHookExW(WH_MOUSE_LL,MouseHook,GetModuleHandleW(NULL),0);
  gKeyHook=SetWindowsHookExW(WH_KEYBOARD_LL,KeyHook,GetModuleHandleW(NULL),0);
  if(!gMouseHook)
    printf("WARNING: wheel hook unavailable, Windows error %lu.\n",
           static_cast<unsigned long>(GetLastError()));
  else puts("Wheel low-level hook INSTALLED: switch FSX to foreground and scroll.");
  if(!gKeyHook)
    printf("WARNING: fine F2/F3 keyboard hook unavailable, Windows error %lu.\n",
           static_cast<unsigned long>(GetLastError()));
  puts("Waiting for FSX SimConnect connection. Start/load your flight.");

  Telemetry telemetry;
  bool connectedFlight=false;
  DWORD lastTelemetry=0,lastConnectTry=GetTickCount()-3000,lastSend=0,lastReport=0;
  DWORD lastPidScan=GetTickCount()-3000;
  LONG lastReportedWheel=0;
  double commandedThrottle=0.0;
  bool throttleLocal=false;
  DWORD localCommandTime=0;
  double currentOverrideFps=0.0;
  bool initializedBoostSpeed=false;
  bool quit=false;

  while(!quit) {
    MSG msg={};
    while(PeekMessageW(&msg,NULL,0,0,PM_REMOVE)) {
      if(msg.message!=WM_HOTKEY)continue;
      if(msg.wParam==20) {
        LONG next=gBoost?0:1;
        InterlockedExchange(&gBoost,next);
        InterlockedExchange(&gWheelSteps,0);
        InterlockedExchange(&gFineKeySteps,0);
        initializedBoostSpeed=false;
        throttleLocal=false;
        if(next) {
          if(!connectedFlight || GetTickCount()-lastTelemetry>3500 ||
             !gEventMappingReady) {
            InterlockedExchange(&gBoost,0);
            puts("Cannot enable: no recent FSX throttle telemetry / SimConnect.");
          } else {
            puts("MACH 20 POWER ON: F4=full boost; F1=idle; 50 fine steps.");
          }
        } else {
          puts("MACH 20 POWER OFF: FSX native engine limits restored.");
          puts("NOTE: Existing high airspeed decays under FSX physics; not instantaneous.");
        }
      } else if(msg.wParam==21) {
        InterlockedExchange(&gWheelActive,gWheelActive?0:1);
        InterlockedExchange(&gWheelSteps,0);
        gWheelRemainder=0;
        printf("Mouse-wheel throttle %s\n",gWheelActive?"ON":"OFF");
      } else if(msg.wParam==22) {
        InterlockedExchange(&gBoost,0);
        quit=true;
      } else if(msg.wParam==23) {
        DWORD fgPid=0;
        HWND fg=GetForegroundWindow();
        if(fg) GetWindowThreadProcessId(fg,&fgPid);
        printf("DIAGNOSTIC: FSX pid=%lu foreground pid=%lu; wheel hook=%s; enabled=%ld; boost=%ld; SimConnect=%s; mapped=%d; telemetry=%d\n",
               static_cast<unsigned long>(gFSXPid),
               static_cast<unsigned long>(fgPid),
               gMouseHook?"yes":"NO",
               static_cast<long>(gWheelActive),static_cast<long>(gBoost),
               sc.connection?"connected":"disconnected",
               gEventMappingReady?1:0, connectedFlight?1:0);
        printf("Wheel messages seen=%ld, intercepted=%ld, increments=%ld, SimConnect sent=%ld, F2/F3 fallback=%ld\n",
               static_cast<long>(gWheelSeen),
               static_cast<long>(gWheelAccepted),
               static_cast<long>(gMouseScrollClicks),
               static_cast<long>(gWheelSimConnectEvents),
               static_cast<long>(gWheelFallbackEvents));
      }
    }
    DWORD now=GetTickCount();
    if(now-lastPidScan>=1500) {
      gFSXPid=discoverFSXProcess();
      lastPidScan=now;
    }
    if(simconnectAvailable && !sc.connection && now-lastConnectTry>=3000) {
      lastConnectTry=now;
      if(connect()) {
        connectedFlight=false;
        throttleLocal=false;
        initializedBoostSpeed=false;
        InterlockedExchange(&gBoost,0);
      }
    }
    if(sc.connection) {
      for(int i=0;i<100;i++) {
        ScRecv* packet=nullptr;
        DWORD bytes=0;
        HRESULT hr=sc.Next(sc.connection,&packet,&bytes);
        if(hr!=S_OK || !packet)break;
        if(packet->id==RECV_QUIT) {
          puts("FSX closed. Boost disabled.");
          InterlockedExchange(&gBoost,0);
          disconnect();
          connectedFlight=false;
          break;
        }
        if(packet->id==RECV_EXCEPTION) {
          if(bytes>=sizeof(ScException)) {
            const auto* exception=reinterpret_cast<const ScException*>(packet);
            printf("SimConnect exception %lu; event or simvar may be unsupported.\n",
                   static_cast<unsigned long>(exception->exception));
          }
          continue;
        }
        if(packet->id==RECV_SIMOBJECT_DATA &&
           bytes>=offsetof(ScObjectData,data)+sizeof(double)*5) {
          const auto* d=reinterpret_cast<const ScObjectData*>(packet);
          if(d->requestId==REQ_READ && d->defineCount>=5) {
            std::memcpy(&telemetry,&d->data,sizeof(Telemetry));
            if(std::isfinite(telemetry.throttlePct) &&
               std::isfinite(telemetry.bodyZfps) && std::isfinite(telemetry.tempC)) {
              connectedFlight=true;
              lastTelemetry=now;
              if(!throttleLocal || now-localCommandTime>1500) {
                commandedThrottle=std::max(0.0,std::min(100.0,telemetry.throttlePct));
                throttleLocal=false;
              } else if(fabs(commandedThrottle-telemetry.throttlePct)<1.1) {
                throttleLocal=false;
              }
            }
          }
        }
      }
    }
    const LONG endstop=InterlockedExchange(&gEndStop,-1);
    if(endstop==0 || endstop==1) {
      commandedThrottle=endstop==1?100.0:0.0;
      throttleLocal=true;
      localCommandTime=now;
    }
    LONG wheel=InterlockedExchange(&gWheelSteps,0);
    LONG fineKeys=InterlockedExchange(&gFineKeySteps,0);
    wheel=std::max(-20L,std::min(20L,wheel));
    fineKeys=std::max(-20L,std::min(20L,fineKeys));
    if(gBoost && sc.connection && connectedFlight && gEventMappingReady) {
        LONG ticks=wheel+fineKeys;
        if(ticks) {
          commandedThrottle=std::max(0.0,std::min(100.0,
                                    commandedThrottle+2.0*ticks));
          const DWORD throttleValue=static_cast<DWORD>(
                                   std::floor(commandedThrottle*16383.0/100.0+0.5));
          if(emitThrottle(EVT_THROTTLE_SET,throttleValue)) {
            localCommandTime=now;
            throttleLocal=true;
          } else {
            puts("WARNING: cannot set fine throttle.");
          }
        }
      }
    } else if(wheel) {
      // Normal mode wheel works with OR WITHOUT SimConnect. Prefer simulator
      // events if the connection is healthy; otherwise inject FSX's F2/F3
      // keystrokes, exactly like the independent AHK wheel utility.
      const DWORD evt=wheel>0?EVT_THROTTLE_INCR:EVT_THROTTLE_DECR;
      const UINT vk=wheel>0?VK_F3:VK_F2;
      for(LONG n=0;n<std::abs(wheel);n++) {
        const bool sent=sc.connection && gEventMappingReady && emitThrottle(evt);
        if(sent) InterlockedIncrement(&gWheelSimConnectEvents);
        else if(sendThrottleKey(vk)) InterlockedIncrement(&gWheelFallbackEvents);
        else puts("WARNING: throttle fallback SendInput failed (FSX must be foreground, with compatible admin privileges).");
      }
    }
    if(wheel && now-lastReportedWheel>450) {
      printf("WHEEL %s %ld step(s), mode=%s, output=%s\n",
             wheel>0?"UP":"DOWN",
             static_cast<long>(wheel>0?wheel:-wheel),
             gBoost?"precision 2%":"normal",
             (sc.connection && gEventMappingReady)?"SimConnect":"F2/F3 fallback");
      lastReportedWheel=now;
    }
    if(gBoost && sc.connection && connectedFlight &&
       now-lastTelemetry<=3500 && now-lastSend>=100) {
      const double throttleFraction=std::max(0.0,
                                std::min(1.0,commandedThrottle/100.0));
      if(throttleFraction<0.0001) {
        // Idle means NO boost. Do not set the velocity to zero:
        // FSX can coast/decelerate like a normal airplane.
        initializedBoostSpeed=false;
      } else {
        const double sound=soundSpeedFeetPerSecond(telemetry.tempC);
        const double targetMach=20.0*throttleFraction;
        const double targetFps=targetMach*sound;
        if(!initializedBoostSpeed) {
          currentOverrideFps=std::max(0.0,telemetry.bodyZfps);
          initializedBoostSpeed=true;
        }
        // Smooth ramp: at most 0.5 Mach/sec acceleration and
        // 1 Mach/sec deceleration to avoid immediate speed teleportation.
        const double increase=sound*0.05;
        const double decrease=sound*0.10;
        const double error=targetFps-currentOverrideFps;
        if(error>0) currentOverrideFps+=std::min(error,increase);
        else currentOverrideFps-=std::min(-error,decrease);
        if(!writeSpeed(currentOverrideFps)) {
          puts("ERROR: FSX rejected boost-speed write. Boost disabled.");
          InterlockedExchange(&gBoost,0);
          initializedBoostSpeed=false;
        }
      }
      lastSend=now;
    }
    if(now-lastReport>=3000 && sc.connection && connectedFlight) {
      printf("%s | throttle %.1f%% | target Mach %.2f | indicated Mach %.2f | altitude %.0fft\n",
             gBoost?"BOOST ON":"Normal",
             commandedThrottle,
             gBoost?20.0*commandedThrottle/100.0:0.0,
             telemetry.mach,telemetry.altitudeFt);
      lastReport=now;
    }
    Sleep(12);
  }
  if(gKeyHook)UnhookWindowsHookEx(gKeyHook);
  if(gMouseHook)UnhookWindowsHookEx(gMouseHook);
  for(const auto& h: bindings)UnregisterHotKey(NULL,h.id);
  disconnect();
  if(sc.dll)FreeLibrary(sc.dll);
  return 0;
}
