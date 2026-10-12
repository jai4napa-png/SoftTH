FSX MACH BOOST 1.2 — SIMCONNECT REPAIR BUILD
===================================
Run FSX Steam Edition, load a flight, then double-click FSX_Mach20Boost.exe.
This is a separate external controller; never put it in the FSX game folder.
It does not alter SoftTH or your calibrated multi-monitor configuration.

Ctrl+Alt+1 => Mach 1
Ctrl+Alt+2 => Mach 2
Ctrl+Alt+5 => Mach 5
Ctrl+Alt+0 => Mach 20
Ctrl+Alt+M => toggle speed override
Ctrl+Alt+Q => restore pre-boost speed and exit

The program starts with boost OFF and target Mach 2. Test at high altitude
over water, first at Mach 1/2. Save your flight before experimenting.
At Mach 20 expect physics, scenery, gauges, terrain loading, or crashes to
behave incorrectly. Speed override is based on local ambient temperature.
The program reports FSX's AIRSPEED MACH to check actual results. A successful
API call does not guarantee stable Mach 20 flight.

The normal physics engine is not replaced, and joystick response at hypersonic
speed cannot be guaranteed. Disengaging attempts to restore your earlier
forward speed; a loaded flight should be saved independently as backup.

Requirements:
- Windows 11 x64, FSX Steam Edition (32-bit)
- FSX SimConnect client installed (normally part of FSX-SE)
- No FSUIPC license; no aircraft.cfg editing required.
- If FSX runs elevated, run this utility elevated too.

Source: tools/mach20/Mach20Boost.cpp
Build: .github/workflows/build-mach20.yml, GitHub Actions Windows x86

SIMCONNECT ERROR FIX:
This version probes native WinSxS side-by-side assemblies using manifest
activation for FSX Steam versions 10.0.62615 and 10.0.62613, plus FSX-SP2
10.0.61259 and FSX-SP1 10.0.61242. Extract EVERYTHING from the ZIP to
one folder: the .exe must be next to its four SimConnect_*.manifest files.
If it still says 32-bit SimConnect client not accessible, run the included
Repair_SimConnect.cmd. It locates the MSI in your original FSX SDK install,
asks before installing, and can be given a different FSX root location.
No third-party DLL downloads. No SoftTH updates. Wheel throttle remains
available through your independent AutoHotkey script (not bundled here).
