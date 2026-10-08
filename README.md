# SoftTH

Software TripleHead / multihead rendering originally by Keijo "Kegetys" Ruotsalainen and later maintained by C. Justin Ratcliff / SoftTH.

Licensed under the **GNU GPL v3**. See [LICENSE](LICENSE).

> **Modified version notice:** the `fsx-win11` branch contains substantial changes made in 2026 for Microsoft Flight Simulator X: Steam Edition (FSX-SE) on Windows 11. This is an experimental community fork and is not an official release from the original SoftTH authors.

## FSX-SE / Windows 11 public beta

**Current beta: v3.42 x86**

This branch restores a practical SoftTH multi-monitor path for **FSX-SE (32-bit Direct3D 9)** on modern Windows 11 systems, including mixed-size and non-rectangular monitor layouts.

The key FSX-specific fix is render-only monitor virtualization for FSX's `g2d.dll`: FSX can build cockpit/world screen-space geometry against the full SoftTH virtual canvas while ordinary Windows dialogs and work-area placement continue to use physical monitor geometry.

### Tested configuration

The development configuration was tested with:

- FSX: Steam Edition, 32-bit D3D9
- Windows 11
- Intel graphics
- four 1920×1080 displays in a mixed T-style layout
- virtual render target: 5760×2160
- mixed per-head crops, including a scaled primary/head crop

Other monitor layouts should work in principle but require their own `sourceRect` calibration.

### What v3.42 includes

- FSX fullscreen multihead rendering on a virtual SoftTH canvas
- FSX `g2d.dll` monitor-size virtualization so screen-space geometry uses the virtual resolution
- a composited/windowed physical primary D3D device while FSX remains logically fullscreen
- improved Alt-Tab / Task Manager / FSX-dialog visibility
- protection against treating standard FSX `#32770` dialogs as render children
- virtualized FSX window/client geometry only while the fullscreen render path needs it
- optional calibration grid, head borders and labels
- Windows 11 x86 GitHub Actions build

### Beta limitations

- **FSX-SE x86 is the supported target.** This branch is not yet validated as a general replacement for other D3D9 games.
- x64 is not currently built/tested by the FSX workflow.
- monitor `sourceRect` values are layout-specific and must be calibrated.
- `zoomOutMultiplier` is a legacy fixed-function projection override. FSX's main world view does not consistently use that path, so values above 1.0 may have no visible effect.
- for wide FSX views, use FSX's own view controls and normally set `WideViewAspect=True` in the FSX configuration.
- UI/dialog behavior is much improved but remains part of the beta test surface.

## Installation: FSX-SE

See [docs/FSX-SE-Win11.md](docs/FSX-SE-Win11.md) for the complete setup and calibration procedure.

The short version:

1. Build/download the **x86** FSX-Win11 package.
2. Copy `d3d9.dll`, `D3DX9_43.dll`, and `D3DCompiler_43.dll` into the FSX root directory, normally:
   `C:\Program Files (x86)\Steam\steamapps\common\FSX`
3. Create `config.SoftTHconfig` in that same directory.
4. Select the SoftTH device and a fullscreen resolution matching `renderResolution` inside FSX.
5. Calibrate every head's `sourceRect`.

An example configuration is provided at [examples/config.SoftTHconfig.example](examples/config.SoftTHconfig.example). It is an example only; device IDs and crop rectangles are not universal.

## Building the FSX branch

The repository contains a Windows GitHub Actions workflow:

`.github/workflows/build-fsx-win11.yml`

It builds:

- solution: `make\SoftTH-FSX-Win11.sln`
- configuration: `Release`
- platform: `Win32`

The public artifact is intentionally packaged without PDB/debug-symbol files.

## License and source availability

SoftTH is GPLv3. If you distribute modified binaries, the corresponding source and license terms must remain available to recipients. This repository/branch is intended to provide that corresponding source for the FSX-SE Windows 11 build.

The original SoftTH copyright and GPL notices are preserved in the source.

## Original SoftTH project background

SoftTH is an open-source software multihead gaming solution. It renders the full scene on one display adapter, then splits/copies portions of that render to multiple output displays. It was designed to support combinations of monitors and resolutions without requiring NVIDIA Surround, AMD Eyefinity, or Matrox TripleHead2Go.

The original project was created by Keijo "Kegetys" Ruotsalainen. Later SoftTH development/maintenance was carried forward by C. Justin Ratcliff / SoftTH. The repository still contains older Direct3D 10/11 work and placeholders that are outside the scope of this FSX-SE beta.

For historical project details, see the repository history and source comments.
