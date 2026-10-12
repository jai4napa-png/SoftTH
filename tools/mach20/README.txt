FSX MACH 20 THROTTLE BOOST v1.3 (experimental)
=================================================

Controls
--------
Ctrl+Alt+F4    Toggle boosted engine-output simulation on/off
F1             Keep normal FSX idle-throttle action
F2 / F3        Keep normal decrease/increase throttle actions
F4             Keep normal max-throttle action
Mouse wheel    Keep using the separate AutoHotkey throttle mapper
Ctrl+Alt+Q     Exit and release all boost control

When boost is ON, all throttle sources are read from FSX using SimConnect.
0% throttle   -> no extra boost (the aircraft continues coasting normally)
25% throttle  -> approximately Mach 5 target ceiling
50% throttle  -> approximately Mach 10 target ceiling
75% throttle  -> approximately Mach 15 target ceiling
100% throttle -> approximately Mach 20 target ceiling

"Target" is a speed ceiling for the artificial acceleration controller, not
a guarantee. Reaching Mach 20 may be impossible due to FSX flight physics,
aircraft crashes, bad terrain/scenery loading, or limit behavior. It does
not directly multiply real engine thrust inside FSX because that is not
a universal writable setting for arbitrary aircraft. Instead, it
adds limited forward velocity when the throttle is above idle.
Acceleration is gradual; F4 is not an instant jump to Mach 20.
Pulling throttle to idle turns off extra acceleration, not aircraft speed.
Toggling boost OFF immediately stops all velocity overrides, but doesn't
magically return a Mach 20 aircraft to its normal cruise speed. FSX's physics
then controls any deceleration or instability.

The program observes NUMBER OF ENGINES and engine combustion so it does
not add synthetic thrust when no configured engine is operating.
The average of running engines' throttle fractions controls boost.

INSTALLATION
------------
1. Extract ALL files into one folder outside the FSX game folder.
2. Start FSX Steam Edition and load a flight.
3. Run FSX_Mach20Boost.exe.
4. Use Ctrl+Alt+F4 to toggle. Starts OFF.
5. If SimConnect reports missing, exit FSX and run Repair_SimConnect.cmd
   in this package. This uses the official SimConnect MSI from your FSX
   installation, and requires your consent. Do not download random DLLs.

The 4 SimConnect_*.manifest files must stay beside the EXE.
Requires Windows 11 and FSX Steam Edition with a 32-bit SimConnect client.
No SoftTH DLL, display geometry, aircraft.cfg or flight model changes.

Source: https://github.com/jai4napa-png/SoftTH/tree/fsx-mach20-throttle-toggle-v13/tools/mach20
