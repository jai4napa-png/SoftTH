FSX MACH 101 POWER-THROTTLE 2.4 — BOOST INDICATOR WHEN ON (EXPERIMENTAL)
============================================
A stand-alone Windows application, independent from SoftTH.
No modification to FSX, SoftTH, aircraft.cfg or scenery files.

CONTROLS
  Ctrl+Alt+F4  — Toggle Mach101 power on/off (starts OFF)
  F1           — Idle throttle in either mode (native FSX)
  F4           — Full throttle in either mode (native FSX)
  F2/F3        — Normal FSX throttle increments with boost OFF
                 Boost ON: +1 Mach for F3, -1 Mach for F2 (101 steps)
  Mouse wheel  — Normal FSX throttle increment events when boost OFF
                 Boost ON: +1/-1 Mach per wheel detent (101 steps)
  Ctrl+Alt+W   — Toggle program wheel throttle control on/off
  Ctrl+Alt+D   — Print wheel-capture diagnostic counters
  Ctrl+Alt+Q   — Exit the controller; leaves flight in FSX

Boost ON exposes 101 one-Mach adjustments from the bottom endpoint:
F1 = Mach 0 (throttle 0%; no speed override), F4 = Mach 101
(throttle 100%). Every wheel notch and F3/F2 key press changes the
BOOST TARGET by exactly +1/-1 Mach, clamped between Mach 0 and Mach 101.
Example: 0, 1, 2, ..., 100, 101. Including Mach zero gives 102
distinct positions separated by 101 steps.
The utility still sets the FSX throttle lever proportionally for the
existing throttle controls; the extreme airspeed comes from its
separate speed override, not from physically increasing horsepower.
When enabled, it takes the current throttle position as its initial
Mach target. Boost target then remains owned by the utility so that FSX
throttle telemetry and autopilot do not override your wheel settings.
Acceleration is rate-limited to about 10 Mach per second.
Deceleration while boosted is limited to about 14 Mach per second.
OFF disables velocity writes entirely, restoring stock FSX engine
behavior. This does NOT rewind existing extreme airspeed immediately.
Load a saved flight to return to a normal flight state if required.

IMPORTANT: The engine thrust itself is not physically increased; the
utility overlays a velocity controller based on stock engine throttle.
The FSX aerodynamic, collision, engine and scenery models are NOT
capable of realistic Mach 101 for arbitrary aircraft; crashes, loss
of control and unstable instruments are possible. Test at high
altitude over water. Save a flight before using boost.

INSTALLATION
  1. Fully EXIT the previous FSX_Mach101Boost.exe instance; the older
     1.2 portable version does not need uninstalling.
  2. EXIT the independent AutoHotkey mouse-wheel-throttle script, or
     wheel inputs may be doubled/intercepted by the old script.
  3. Extract ALL bundled files to a new folder outside FSX.
  4. Start FSX, load an aircraft, launch FSX_Mach20Boost.exe.
  5. Press Ctrl+Alt+F4 to engage. F4 sets stock throttle to 100%.
  6. Press Ctrl+Alt+F4 again to disengage.
  7. If no SimConnect is found, use Repair_SimConnect.cmd included
     in this ZIP; it installs the official MSI from your FSX directory
     with explicit consent.

The EXE checks for fsx_softth.exe or fsx.exe foreground windows before
capturing mouse wheel or F2/F3. In FSX dialog windows, it avoids
interfering with native Windows mouse interaction.
Keyboard or mouse hooks may require matching FSX/admin permissions.
The built-in wheel does not require AutoHotkey.

This is a beta. Validate the SimConnect throttle percentage and
Mach indication against the display; a successful API call does
not prove that FSX achieves Mach 101.

Source: tools/mach20/Mach20Boost.cpp
Build: GitHub Actions, Windows x86/MSVC.

MOUSE WHEEL FIX IN 2.1
Previously v2.0 refused to intercept the wheel unless SimConnect was
already running AND the cursor window was identified as FSX. In SoftTH
multihead fullscreen those conditions can fail. 2.1 captures wheel
input whenever FSX is the foreground process (fsx_softth.exe/fsx.exe),
without requiring the hovered subwindow to be recognized. It caches
the FSX process identifier rather than doing slow process lookup inside
Windows' low-level mouse hook. When SimConnect is missing, normal wheel
throttle still attempts to send native FSX F2/F3 keyboard commands;
boost mode requires SimConnect.

If scrolling still changes nothing, switch back to FSX, move the wheel,
then press Ctrl+Alt+D. The console prints: FSX pid, foreground pid,
number of wheel messages seen/captured, command counts, and connection.
Wheel hook can require the same administrator elevation as FSX.
If Windows reports no wheel messages, it may be a driver/DirectInput
issue; the status output distinguishes that case.

V2.2 CHANGE: Boost scale is Mach 0-101, 101 increments of Mach 1;
Ctrl+Alt+F4 toggles. Old v2.1 wheel capture fallback and diagnostics
remain. With toggle OFF, F1-F4 and wheel use regular aircraft limits.

ON-SCREEN INDICATOR (V2.4)
In boosted mode, a small click-through status HUD appears in the top-right
of the laptop's physical display. It shows BOOST ON, TARGET MACH, and
ACTUAL MACH from live FSX telemetry. It is hidden whenever the boost
is switched OFF, when another application is foreground, or when FSX is
not yet active. It does not change SoftTH or any display coordinates.
Physical windowed fullscreen (SoftTH focus-safety workaround) is expected
to allow a topmost Windows window above FSX. True exclusive fullscreen
is not guaranteed to show external GDI windows.

V2.4 SPEED AND WHEEL REPAIR
* Faster boost ramp: up to 10 Mach/second (previously 0.5) while enabled.
  This only modifies the requested forward velocity in SimConnect;
  actual speed can be clamped, reset, or destabilized by FSX.
* The Mach target remains 0..101 in 101 increments (+/-1 per click).
* The target no longer snaps back to simulator throttle telemetry.
* Added Windows RAWINPUT mouse listener for hardware/DirectInput cases
  where WH_MOUSE_LL does not see a wheel event. Duplicate input from
  the two listeners is suppressed.
* Ctrl+Alt+D outputs hardware wheel, low-level hook, target Mach,
  actual Mach, SimConnect and velocity telemetry diagnostics.
* The indicator is STILL visible ONLY when boost is on and FSX active.
* Do not run old Mach Boost or AutoHotkey wheel throttle at the same time.

CONTROL TEST
Start with FSX and utility running; press Ctrl+Alt+F4 to enable boost,
press F1, then scroll up 5 detents. TARGET should become Mach 5.
Press F4 to select Mach 101. Compare TARGET versus ACTUAL separately.
If ACTUAL is capped, send the console lines beginning SPEED WRITE along
with Ctrl+Alt+D diagnostics; don't infer a max from gauge alone.
Do not change SoftTH DLLs or config to test this app.
