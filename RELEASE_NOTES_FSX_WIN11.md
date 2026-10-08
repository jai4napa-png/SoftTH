# SoftTH v3.42 FSX-SE / Windows 11 Public Beta

Release status: **Beta**

Target: **Microsoft Flight Simulator X: Steam Edition, x86 Direct3D 9, Windows 11**

## Main changes

- restores usable FSX-SE fullscreen multi-monitor rendering on modern Windows 11 systems
- fixes the FSX physical-monitor-size leak that restricted world/cockpit rendering to the upper-left native primary-monitor area
- virtualizes primary monitor geometry specifically for FSX `g2d.dll` while the logical multihead fullscreen path is active
- keeps ordinary Windows work-area geometry physical for UI placement
- runs the physical primary D3D device composited/windowed while FSX remains logically fullscreen
- improves Alt-Tab, Task Manager and FSX dialog visibility
- does not classify standard `#32770` FSX dialogs as SoftTH render children
- adds calibration grid, head borders and labels
- adds Windows 11 x86 CI build and clean public package

## Packaging changes

The public ZIP contains:

- `d3d9.dll`
- `D3DX9_43.dll`
- `D3DCompiler_43.dll`
- `LICENSE`
- `README.md`
- this release-notes file
- FSX setup documentation
- an example SoftTH configuration

Development PDB files are intentionally excluded.

## Known limitations

- only the x86 FSX-SE path is currently built/tested
- this fork contains FSX-specific behavior and is not yet validated for other D3D9 games
- each monitor layout requires its own `sourceRect` calibration
- FSX UI/dialog sizing and placement can still need refinement on unusual mixed-monitor layouts
- `zoomOutMultiplier` does not reliably change the FSX main world view; keep it at `1.0` for normal testing
- wide-view behavior should be controlled with FSX view settings, including `WideViewAspect=True` where appropriate
- automatic startup directly into the final multihead fullscreen state is not yet considered finished
- the obsolete FSX Home/certificate-warning experience is not changed by this release

## Tested development layout

A mixed four-display T-style arrangement using four 1920x1080 panels and a 5760x2160 virtual render target has been used for development and validation.

This does not imply those crop coordinates are appropriate for other users.

## Upgrade

Replace the prior beta runtime DLLs in the FSX root with the three DLLs from this package. Preserve your existing `config.SoftTHconfig` unless intentionally recalibrating.

## License

SoftTH and this modified version are distributed under GNU GPL v3. See `LICENSE` and the corresponding source in the repository.
