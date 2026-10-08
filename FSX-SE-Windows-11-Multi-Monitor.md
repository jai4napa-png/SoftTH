# SoftTH for Microsoft Flight Simulator X: Steam Edition on Windows 11

SoftTH v3.42 is a public beta update for **Microsoft Flight Simulator X: Steam Edition (FSX-SE)** running on **Windows 11**.

It is intended for people searching for:

- FSX Steam Edition multiple monitors
- FSX triple monitor / triple screen
- FSX multi-monitor Windows 11
- FSX quad monitor
- SoftTH Windows 11
- FSX Direct3D 9 multihead
- FSX mixed-resolution monitors
- FSX non-rectangular monitor layouts
- FSX wide multi-screen cockpit
- SoftTH FSX Steam Edition

## Download

Public beta release:

https://github.com/jai4napa-png/SoftTH/releases/tag/v3.42

Direct ZIP:

https://github.com/jai4napa-png/SoftTH/releases/download/v3.42/SoftTH-v3.42-FSX-SE-Win11-Beta-x86.zip

## Source

FSX-Win11 branch:

https://github.com/jai4napa-png/SoftTH/tree/fsx-win11

## What this version fixes

Older SoftTH/FSX behavior on modern Windows could create the full virtual render target while FSX still generated important screen-space geometry using the physical primary-monitor dimensions, leaving most of the virtual canvas black.

The FSX-Win11 branch adds FSX-specific monitor-geometry virtualization for the rendering path so the cockpit/world screen-space geometry can span the complete SoftTH virtual canvas. It also keeps the underlying physical primary Direct3D device composited/windowed while FSX remains logically fullscreen, improving Alt-Tab, Task Manager, and dialog visibility.

## Status

This is a **public beta**, not a final stable release.

The core fullscreen multi-monitor rendering path is working. Remaining beta areas include:

- monitor-by-monitor sourceRect calibration
- some FSX dialog/UI sizing and placement
- automatic fullscreen startup
- wide-view / FOV / zoom tuning

See the v3.42 release notes and the documentation on the `fsx-win11` branch for installation and calibration details.
