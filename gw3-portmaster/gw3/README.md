# Geometry Wars 3: Dimensions (PortMaster)

A native **32-bit ARM (armhf)** port of the Android release of *Geometry Wars
3: Dimensions*. It runs the original `libgwnext.so` game engine directly on Linux
through a custom Android-on-Linux loader (KMS/DRM video, GLES2, FMOD audio) -
no emulation, no Box86/Box64.

You must own and supply the game's original **APK** and **OBB**. Version
**1.3.0 "Evolved"** is recommended: it has the controller-first console menus
and the Evolved levels. 1.0.0 also runs, with the phone menus. The launcher
extracts the engine/FMOD `.so` files from the APK automatically. None of
Activision's copyrighted content is included in this port.

## Installation

1. Install this port through PortMaster (or copy this folder into your
   `ports/` directory).
2. From your own legally-owned copy, place these into:
   ```
   <ports>/Geometry Wars 3/gw3/
   ```
   - the game APK - either a `.apk`, or a `.xapk` / `.apkm` bundle (which also
     contains the OBB)
   - the OBB: `main.101.com.activision.gw3.dimensions.obb` (1.3.0) or
     `main.35.com.activision.gw3.dimensions.obb` (1.0.0). Keep one version only.
3. Launch the port. The launcher auto-extracts `libgwnext.so`, `libfmodex.so`
   and `libfmodevent.so` from the APK on first run, verifies the files are
   present, then runs `gw3` (the engine reads the OBB directly - no unpacking
   step).

## Controls

| Button | Menus | In a level |
|---|---|---|
| Left stick / d-pad | Move the selection | Move |
| Right stick | | Shoot |
| A (right) | Confirm | |
| B (bottom) | Back | |
| L2 | | Super |
| R2 | | Smart bomb |
| START | | Pause |
| SELECT (hold 1 s) | Port menu | Port menu |
| START + SELECT | Quit | Quit |

If your firmware already swaps A/B, set **Options > Graphics > Buttons** to
**Xbox**.

## Extras

- **Larger menus** (1.3.0): the menus, text and HUD are drawn 33% larger to
  fill the R36S's 4:3 screen. Set **Options > Graphics > Menu size** to
  Normal for the original size (applies at the next start).
- **Options > Graphics** (1.3.0): particle and background detail, presets
  (Auto, Low, Android High, Console), button layout, an FPS counter, the
  menu size and stage zoom. Effects changes apply from the next level.
- **Stage zoom** (optional, both versions): Off / 10% / 20% / 30% in Options >
  Graphics or the port menu. The arena is drawn bigger and the camera follows
  your ship, but you see less of the arena at once.
- **Achievements**, offline: the main menu's Achievements tile lists all 30;
  unlocks pop up in the game's style and are kept in `achievements.txt`.
- **Port menu** (hold SELECT): the same presets, FPS counter, button layout,
  menu size, stage zoom and an achievements list, in any version.

Files in `gw3/`: `gw3.ini` (settings), `achievements.txt`, `savegame.dat` /
`cloudsave.bin` (your progress) and `gw3.log` (the last run's log; attach it
to bug reports).
