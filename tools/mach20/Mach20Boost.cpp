// FSX Mach Boost (experimental) — external FSX-SE SimConnect velocity override.
// No aircraft.cfg or SoftTH DLL changes. Build with MSVC x86 on Windows.
// Public-domain dedication for this small standalone utility.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
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
};
static ScApi sc;

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
         fetch(sc.Next,"SimConnect_GetNextDispatch");
}

static void disconnect() {
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
    {"VELOCITY BODY Z","feet per second"}
  };
  for(const auto& v: vars)
    if(FAILED(sc.Add(sc.connection,DEF_READ,v.name,v.unit,DATA_FLOAT64,0.0f,0xFFFFFFFF))) {
      puts("Unable to define read-only flight telemetry."); disconnect(); return false;
    }
  if(FAILED(sc.Add(sc.connection,DEF_WRITE,"VELOCITY BODY Z","feet per second",
                   DATA_FLOAT64,0.0f,0xFFFFFFFF))) {
    puts("Unable to define aircraft speed control."); disconnect(); return false;
  }
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
  puts("FSX MACH BOOST 1.2 — SIMCONNECT REPAIR / EXPERIMENTAL");
  puts("Works separately from SoftTH. No aircraft files are edited.");
  puts("Start FSX-SE and load a flight before enabling the boost.");
  puts("CAUTION: Mach 20 can overwhelm FSX physics/scenery and cause a crash.");
  puts("");
  puts("Ctrl+Alt+1 = Mach 1 target");
  puts("Ctrl+Alt+2 = Mach 2 target");
  puts("Ctrl+Alt+5 = Mach 5 target");
  puts("Ctrl+Alt+0 = Mach 20 target");
  puts("Ctrl+Alt+M = toggle boost ON / OFF");
  puts("Ctrl+Alt+Q = exit and restore original pre-boost speed");
  puts("");
  if(!loadSimConnect()) { puts("Press Enter to close."); getchar(); return 2; }
  const UINT hotkeyMods = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
  const struct { int id, key; } hotkeys[] = {
    {1,'1'}, {2,'2'}, {5,'5'}, {20,'0'}, {99,'M'}, {100,'Q'}
  };
  for(const auto& hk : hotkeys)
    if(!RegisterHotKey(NULL,hk.id,hotkeyMods,hk.key))
      printf("WARNING: hotkey Ctrl+Alt+%c unavailable (already in use)\n",hk.key);
  double targetMach = 2.0; // Test gently; select 20 explicitly.
  Telemetry telemetry;
  bool haveTelemetry=false,boost=false,exitProgram=false,havePreBoost=false;
  double preBoostSpeed=0;
  DWORD lastTelemetry=0, lastConnectAttempt=0, lastWrite=0, lastPrint=0;
  puts("Default target Mach 2. Press Ctrl+Alt+0 for Mach 20.");

  while(!exitProgram) {
    MSG msg;
    while(PeekMessageW(&msg,NULL,0,0,PM_REMOVE)) {
      if(msg.message!=WM_HOTKEY) continue;
      switch((int)msg.wParam) {
        case 1: targetMach=1;break;
        case 2: targetMach=2;break;
        case 5: targetMach=5;break;
        case 20: targetMach=20;break;
        case 99:
          if(boost) {
            boost=false;
            if(havePreBoost && sc.connection) writeSpeed(preBoostSpeed);
            puts("Boost OFF. Restored initial forward speed.");
          } else if(sc.connection && haveTelemetry && GetTickCount()-lastTelemetry<4000) {
            preBoostSpeed=telemetry.bodyZfps;
            havePreBoost=true;
            boost=true;
            puts("BOOST ON (speed override active)");
          } else {
            puts("No fresh FSX flight telemetry; cannot engage boost.");
          }
          break;
        case 100:
          if(boost && havePreBoost && sc.connection) writeSpeed(preBoostSpeed);
          boost=false; exitProgram=true;
          break;
      }
      if((int)msg.wParam==1 || (int)msg.wParam==2 || (int)msg.wParam==5 ||
         (int)msg.wParam==20)
        printf("New target Mach %.0f; boost %s\n",targetMach,boost?"ON":"OFF");
    }
    DWORD now=GetTickCount();
    if(!sc.connection && now-lastConnectAttempt >= 3000) {
      lastConnectAttempt=now;
      if(connect()) {haveTelemetry=false; boost=false;}
    }
    if(sc.connection) {
      for(int i=0;i<100;++i) {
        ScRecv* packet=nullptr;
        DWORD cb=0;
        HRESULT hr=sc.Next(sc.connection,&packet,&cb);
        if(hr!=S_OK || !packet)break;
        if(packet->id==RECV_QUIT) {
          puts("FSX closed."); disconnect(); haveTelemetry=false;boost=false;break;
        }
        if(packet->id==RECV_EXCEPTION) {
          if(cb>=sizeof(ScException)) {
            const auto* ex=reinterpret_cast<const ScException*>(packet);
            printf("FSX SimConnect exception %lu (send id %lu).\n",
                   (unsigned long)ex->exception,(unsigned long)ex->sendId);
          }
          continue;
        }
        if(packet->id==RECV_SIMOBJECT_DATA &&
           cb>=offsetof(ScObjectData,data)+sizeof(double)*4) {
          const auto* d=reinterpret_cast<const ScObjectData*>(packet);
          if(d->requestId==REQ_READ && d->defineCount>=4) {
            std::memcpy(&telemetry,&d->data,sizeof(Telemetry));
            haveTelemetry=true;
            lastTelemetry=now;
          }
        }
      }
    }
    if(boost && sc.connection && haveTelemetry && now-lastTelemetry<4000 &&
       now-lastWrite>=100) {
      const double fps=targetMach*soundSpeedFeetPerSecond(telemetry.tempC);
      if(!writeSpeed(fps)) {
        puts("ERROR: FSX did not accept the speed command. Boost disabled.");
        boost=false;
      }
      lastWrite=now;
    }
    if(now-lastPrint>=3000 && sc.connection && haveTelemetry) {
      printf("FSX: %s | target M%.0f | indicated M%.2f | altitude %.0fft | temp %.0fC\n",
        boost?"BOOST ON":"normal",targetMach,telemetry.mach,
        telemetry.altitudeFt,telemetry.tempC);
      if(boost && fabs(telemetry.mach-targetMach)>1.5)
        puts("NOTE: FSX may be limiting actual speed. Requested speed is not verified.");
      lastPrint=now;
    }
    Sleep(15);
  }
  for(const auto& hk: hotkeys) UnregisterHotKey(NULL,hk.id);
  disconnect();
  if(sc.dll) FreeLibrary(sc.dll);
  return 0;
}
