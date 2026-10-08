# FSX-SE / Windows 11 setup

This document applies to the **SoftTH v3.42 FSX-SE / Windows 11 public beta, x86**.

## Scope

Supported/test target:

- Microsoft Flight Simulator X: Steam Edition
- 32-bit Direct3D 9
- Windows 11
- SoftTH multihead fullscreen rendering

This branch contains FSX-specific behavior and should not yet be treated as a general SoftTH replacement for unrelated games.

## Install

Copy these runtime files into the FSX installation directory:

- `d3d9.dll`
- `D3DX9_43.dll`
- `D3DCompiler_43.dll`

Typical Steam path:

`C:\Program Files (x86)\Steam\steamapps\common\FSX`

Do not copy a PDB file for normal use.

Place your SoftTH configuration in the same directory as:

`config.SoftTHconfig`

## FSX display settings

Inside FSX:

1. Select the SoftTH display device.
2. Select a fullscreen resolution that exactly matches `renderResolution` in `config.SoftTHconfig`.
3. Start the flight and enter fullscreen.

Example:

`renderResolution=5760x1080`

means FSX should use:

`5760x1080x32`

For very wide views, `WideViewAspect=True` is normally recommended in the FSX configuration file used by your SoftTH launcher/profile.

## Why this fork is FSX-specific

FSX can ask Windows for the physical monitor size through its rendering module `g2d.dll` and then generate screen-space geometry using that physical size. On a 1920x1080 primary monitor this caused FSX to populate only the upper-left 1920x1080 portion of a much larger SoftTH virtual backbuffer.

The FSX-Win11 branch virtualizes the primary monitor geometry specifically for the FSX rendering path so `g2d.dll` can construct geometry for the complete SoftTH virtual canvas.

At the same time, the underlying primary D3D device remains composited/windowed even while FSX logically considers itself fullscreen. This allows Windows dialogs, Task Manager and Alt-Tab to remain usable.

## Configure heads

Each output has a source rectangle:

`sourceRect=x,y,width,height`

It selects a rectangle from the full virtual render surface and maps that rectangle onto the physical output.

Useful calibration rules:

- increase `x`: sample farther right in the virtual image
- decrease `x`: sample farther left
- increase `y`: sample farther down
- decrease `y`: sample farther up
- increase `width,height`: show a larger area of the virtual canvas on that monitor
- decrease `width,height`: show a smaller area

The correct values depend on monitor placement, physical size, viewing distance and desired overlap.

## Calibration grid

Temporarily enable:

```ini
[debug]
calibrationGrid=1
calibrationGridStep=240
calibrationLineWidth=2
calibrationHeadBorders=1
calibrationLabels=1
```

The grid is drawn on the full virtual backbuffer before each head is cropped. Use it to line up horizontal/vertical continuation and compare scale between monitors.

After calibration, use:

`calibrationGrid=0`

## Zoom / field of view

The current `zoomOutMultiplier` implementation operates on the legacy fixed-function projection transform. FSX's main world rendering does not consistently use that path.

For FSX-SE beta testing, leave:

`zoomOutMultiplier=1.0`

Use FSX's own view zoom and normally `WideViewAspect=True` for wider views.

## Dialogs and UI

The beta contains specific protections so standard Win32 FSX dialogs (`#32770`) are not resized as SoftTH render windows, and it temporarily suspends secondary output windows when another FSX-owned window gets focus.

This remains a beta area. Depending on layout, some FSX setup/escape UI may still need additional sizing or positioning work. If a dialog is difficult to see, Alt-Tab back to the FSX UI and use the normal FSX keyboard controls.

## Example config

See:

`examples/config.SoftTHconfig.example`

That file is a three-across 1920x1080 example. It is not a universal configuration.

## Troubleshooting

SoftTH log:

`%USERPROFILE%\Documents\SoftTH\SoftTH.log`

When reporting a bug, include:

- SoftTH version
- FSX fullscreen resolution
- `config.SoftTHconfig`
- monitor layout/resolutions
- screenshot/photo of the outputs
- `SoftTH.log`

## License

This fork remains licensed under GNU GPL v3. The original SoftTH notices remain in the source. The FSX-Win11 branch is a modified version and the source for the public binary build is available in this repository.
