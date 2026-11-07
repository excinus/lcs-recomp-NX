# Cheat Device Remastered in LCSNative

**Cheat Device Remastered** is a cheat menu for the PSP versions of Liberty City
Stories and Vice City Stories, by **Freakler**:
<https://github.com/Freakler/CheatDeviceRemastered>. This folder is a conversion of
its source code (as of commit `f90f677`, version v1.0h4) to run inside this port,
made with the author's permission. All credit for the cheat device, its cheats, its
menu and its research into the game goes to Freakler and its contributors.

It is licensed like the original: **GPL-3.0-or-later** (see `LICENSE`).
`minIni.cpp` / `minIni.h` are CompuPhase's minIni (Apache-2.0, as in the
original). `cdr_font.hpp` holds glyphs of DejaVu Sans Bold (Bitstream Vera
license). The rest of the port stays MIT; a build that includes this folder is,
as a whole, distributed under the GPL-3.0.

## Using it

1. Download the official release from
   <https://github.com/Freakler/CheatDeviceRemastered/releases>.
2. Copy its `cheatdevice_remastered` folder next to the port's `game` folder
   (on the Switch: `sdmc:/switch/LCSNative/cheatdevice_remastered/`). The port needs
   the release's `cheatdevice_remastered.prx` and `cheatdevice_names_lcs.ini`
   there and reads its translations and names from it; without the release the
   cheat device does not start.
3. In `LCSNative.ini` set:
   ```ini
   [CheatDevice]
   Enabled=1
   ```
4. In the game, **L + UP** opens and closes the menu (**L + DOWN** opens it without
   taking the buttons from the game), as on the PSP. The config is saved in the
   same folder (`cheatdevice_remastered.cfg`).

Liberty City Stories US (ULUS-10041) only, the version this port is built from.

## How it runs here

The plugin was MIPS code loaded beside the game, which patched the game's code.
Here the game is recompiled to C++, so the plugin's C source is compiled into the
port instead (`*.cpp` here are the original `.c` files, changed only where noted
with `LCSNative`):

- `cdr_platform.h/.cpp` stand in for the PSP SDK. The game's memory is the guest
  memory (a raw `*(int*)adr` became `*(int*)GPTR(adr)`); the game's functions are
  `GuestFunction` objects that run the recompiled code (`Runtime::call_guest`),
  copying the port's strings and buffers to guest memory; files map to the
  folder above; the pad is the player's.
- The plugin's scan of the game's code (`PatchLCS`, `initTextBlit`) runs as on the
  PSP and finds the game's variables and functions in guest memory. Locations it
  does not find are listed in the log (`[cheatdevice] not found: ...`).
- Code patches (`HIJACK_FUNCTION`, `MAKE_CALL`, writes into instructions) cannot
  change recompiled code: they are recorded, not applied. The three hooks the menu
  needs are driven from the port instead (`cdr_host.cpp`): the per-frame cheats
  and the menu's input run at the vblank wait, the game's pad is filtered while the
  menu has it, and the menu is drawn by the port over the frame (with its own
  font) instead of through the game's font renderer.
- Cheats that only work by patching the game's code are not listed yet (FPS limit,
  never fall off the bike, skip intros, gravity, walking speeds, random
  loadscreens, loadscreen debug text, swimming, the random-player button cheat).
  They need hooks placed in the recompiled code, which later stages add.

`tests/lcs_cheatdevice_test.cpp` starts it on an empty guest memory, opens the menu
and checks its picture.
