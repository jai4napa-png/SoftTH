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
static constexpr DWORD PERIOD_SIM_FRAME = 3;
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
// All data is received as SIMCONNECT_DATATYPE_FLOAT64; preserve SDK ordering.
struct Telemetry {
  double tempC = 15.0;
  double mach = 0.0;
  double altitudeFt = 0.0;
  double bodyZfps = 0.0;
  double engineCount = 0.0;
  double throttlePct[4] = {0.0,0.0,0.0,0.0};
  double combustion[4] = {0.0,0.0,0.0,0.0};
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
    {"VELOCITY BODY Z","feet per second"},
    {"NUMBER OF ENGINES","number"},
    {"GENERAL ENG THROTTLE LEVER POSITION:1","percent"},
    {"GENERAL ENG THROTTLE LEVER POSITION:2","percent"},
    {"GENERAL ENG THROTTLE LEVER POSITION:3","percent"},
    {"GENERAL ENG THROTTLE LEVER POSITION:4","percent"},
    {"GENERAL ENG COMBUSTION:1","bool"},
    {"GENERAL ENG COMBUSTION:2","bool"},
    {"GENERAL ENG COMBUSTION:3","bool"},
    {"GENERAL ENG COMBUSTION:4","bool"}
  };
  for(const auto& v: vars)
    if(FAILED(sc.Add(sc.connection,DEF_READ,v.name,v.unit,DATA_FLOAT64,0.0f,0xFFFFFFFF))) {
      puts("Unable to define read-only flight telemetry."); disconnect(); return false;
    }
  if(FAILED(sc.Add(sc.connection,DEF_WRITE,"VELOCITY BODY Z","feet per second",
                   DATA_FLOAT64,0.0f,0xFFFFFFFF))) {
    puts("Unable to define aircraft speed control."); disconnect(); return false;
  }
  // Read lever updates frequently enough for F1/F4 and wheel throttle while
  // keeping the number of SimConnect requests modest (~10-15 per second).
  if(FAILED(sc.Request(sc.connection,REQ_READ,DEF_READ,OBJ_USER,PERIOD_SIM_FRAME,0,0,5,0))) {
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

// Average the actual engine throttle levers. This observes native F1-F4,
// joystick throttle and the user's independent mouse-wheel mapping, without
// intercepting or remapping any of those inputs.
static double commandedThrottle(const Telemetry& t) {
  if(!std::isfinite(t.engineCount)) return 0.0;
  const int engines = (int)std::max(0.0,std::min(4.0,t.engineCount));
  if(engines == 0) return 0.0; // unpowered gliders have no engine thrust
  double total=0.0;
  int running=0;
  for(int i=0;i<engines;i++) {
    if(std::isfinite(t.combustion[i]) && t.combustion[i]>0.5 &&
       std::isfinite(t.throttlePct[i])) {
      total += std::max(0.0,std::min(1.0,t.throttlePct[i]/100.0));
      running++;
    }
  }
  return running ? total / running : 0.0;
}

int main() {
  SetConsoleOutputCP(CP_UTF8);
  puts("FSX MACH 20 THROTTLE BOOST 1.3 - EXPERIMENTAL");
  puts("Separate from SoftTH. No flight model or aircraft files are changed.");
  puts("Native F1/F2/F3/F4 and mouse-wheel throttle remain unchanged.");
  puts("");
  puts("Ctrl+Alt+F4 = toggle Mach 20 throttle boost ON/OFF");
  puts("F1 = idle / no synthetic boost");
  puts("F4 = full throttle / boost aims toward approximately Mach 20");
  puts("Ctrl+Alt+Q = exit, leaving current FSX speed to its native physics");
  puts("");
  puts("WARNING: Simulation at Mach 20 can crash the simulator or aircraft.");
  puts("Test from a saved flight at altitude. Toggle defaults OFF.");
  puts("");
  if(!loadSimConnect()) {
    puts("SimConnect is not available. Read README.txt or run Repair_SimConnect.cmd.");
    puts("Press Enter to close."); getchar(); return 2;
  }
  const UINT mods = MOD_CONTROL|MOD_ALT|MOD_NOREPEAT;
  bool boostHotkeyOk= RegisterHotKey(NULL,99,mods,VK_F4) != 0;
  if(!boostHotkeyOk)
    printf("ERROR: Ctrl+Alt+F4 registration failed, Windows error %lu.\n",
           (unsigned long)GetLastError());
  if(!RegisterHotKey(NULL,100,mods,'Q'))
    puts("WARNING: Ctrl+Alt+Q was already in use.");

  bool active=false, haveTelemetry=false, exiting=false;
  DWORD lastTelemetry=0,lastConnectAttempt=0,lastWrite=0,lastPrint=0;
  Telemetry telemetry;
  puts("Boost OFF. F4 remains the aircraft's original full throttle.");
  while(!exiting) {
    MSG msg;
    while(PeekMessageW(&msg,NULL,0,0,PM_REMOVE)) {
      if(msg.message!=WM_HOTKEY) continue;
      if(msg.wParam==99 && boostHotkeyOk) {
        active=!active;
        lastWrite=0;
        if(active) {
          puts("Mach 20 throttle boost ON.");
          puts("Power follows actual engine lever position; F1 idle, F4 maximum.");
        } else {
          puts("Mach 20 throttle boost OFF. Native FSX speed control restored.");
          puts("Current airspeed is not reset; aerodynamic drag slows the aircraft.");
        }
      } else if(msg.wParam==100) {
        active=false;
        exiting=true;
      }
    }
    const DWORD now=GetTickCount();
    if(!sc.connection && now-lastConnectAttempt>3000) {
      lastConnectAttempt=now;
      if(connect()) {haveTelemetry=false;active=false;lastWrite=0;}
    }
    if(sc.connection) {
      for(int i=0;i<100;i++) {
        ScRecv* packet=nullptr;
        DWORD cb=0;
        const HRESULT hr=sc.Next(sc.connection,&packet,&cb);
        if(hr!=S_OK || !packet) break;
        if(packet->id==RECV_QUIT) {
          puts("FSX closed. Boost disabled.");
          disconnect();haveTelemetry=false;active=false;break;
        }
        if(packet->id==RECV_EXCEPTION) {
          if(cb>=sizeof(ScException)) {
            const auto* ex=reinterpret_cast<const ScException*>(packet);
            printf("FSX SimConnect exception %lu, send ID %lu; disabling boost.\n",
                   (unsigned long)ex->exception,(unsigned long)ex->sendId);
          }
          active=false;continue;
        }
        if(packet->id==RECV_SIMOBJECT_DATA &&
           cb>=offsetof(ScObjectData,data)+sizeof(Telemetry)) {
          const auto* d=reinterpret_cast<const ScObjectData*>(packet);
          if(d->requestId==REQ_READ && d->defineCount>=13) {
            std::memcpy(&telemetry,&d->data,sizeof(Telemetry));
            haveTelemetry=true;
            lastTelemetry=now;
          }
        }
      }
    }

    // Only push forward speed when active and throttle actually demands power.
    // This synthesizes EXTRA acceleration; it does not override F1/F4 levers.
    // No speed writes at idle or when boost is OFF. Acceleration is rate-limited
    // so toggling ON at full throttle does not instantly teleport to Mach 20.
    if(active && sc.connection && haveTelemetry && now-lastTelemetry<2000 &&
       (lastWrite==0 || now-lastWrite>=100)) {
      const double power=commandedThrottle(telemetry);
      const double current=telemetry.bodyZfps;
      const double target=20.0*power*soundSpeedFeetPerSecond(telemetry.tempC);
      if(power>0.001 && std::isfinite(current) && current>=0.0 &&
         std::isfinite(target) && current<target) {
        const double dt=lastWrite ? std::max(0.02,std::min(0.25,(double)(now-lastWrite)/1000.0)):0.1;
        // Extra "engine" acceleration in forward body axis, proportional
        // to throttle; controller tapers at the requested Mach ceiling.
        const double maxAccel=400.0*power; // ft/s^2 (experimental)
        const double demand=std::max(0.0,(target-current)/5.0);
        const double delta=std::min(maxAccel,demand)*dt;
        if(delta>0.0 && !writeSpeed(std::min(target,current+delta))) {
          puts("Unable to add boost velocity: disengaging.");
          active=false;
        }
      }
      lastWrite=now;
    }
    if(now-lastPrint>=3000 && sc.connection && haveTelemetry) {
      const double percent=100.0*commandedThrottle(telemetry);
      printf("Boost %s | throttle %.1f%% | M target %.2f | FSX indicated M %.2f | altitude %.0fft\n",
             active?"ON":"OFF",percent,20.0*percent/100.0,
             telemetry.mach,telemetry.altitudeFt);
      if(active && percent>1.0 && fabs(telemetry.mach-20.0*percent/100.0)>2.0)
        puts("Actual Mach is not at the requested target; FSX physics may limit speed.");
      lastPrint=now;
    }
    Sleep(15);
  }
  UnregisterHotKey(NULL,99);
  UnregisterHotKey(NULL,100);
  disconnect();
  if(sc.dll)FreeLibrary(sc.dll);
  return 0;
}
