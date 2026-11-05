Grand Theft Auto: Liberty City Stories for Nintendo Switch (Atmosphere homebrew)
================================================================================

GTA: Liberty City Stories, recompiled from the PSP. Game files are not
included: you need your own copy of the US v1.05 disc (ULUS-10041).

Install
-------
1. Copy the "switch" folder from the zip to the root of the SD card.
2. On a PC, run the launcher (LCSLauncher.exe, from the PC release) with
   your ISO and press "Process ISO". It writes EBOOT.ELF and PSP_GAME into
   its lcs\game folder.
3. Copy EBOOT.ELF and the PSP_GAME folder into
   sdmc:/switch/LCSNative/game/
4. Start the homebrew menu THROUGH A GAME (hold R while starting any game),
   not from the Album: only then does the game get the full memory.
5. Pick "Grand Theft Auto: Liberty City Stories".

Controls
--------
In play, the PSP's buttons are where the Switch's are:
  B = run                    A = attack
  Y = jump                   X = enter or leave a vehicle
  R = aim (handbrake)        L = centre the camera, pick up a weapon
  ZL / ZR = d-pad left / right (Settings > ZL / ZR for other layouts)
  + = pause                  - = change camera
  D-pad left/right = weapon or radio station.
  Left stick moves, right stick looks around.
In menus, A confirms and B goes back (Settings > Confirm button = B for the
PSP's way). The on-screen button hints show the Switch buttons.
Any controller works: handheld, Joy-Con pair, Pro Controller.
Change bindings in LCSNative.ini, section [Controls].

Settings
--------
Press both sticks in (L3 + R3) during play: resolution, view distance, frame
rate, FPS counter, confirm button, ZL / ZR. A or the d-pad
changes a row, B closes. Changes are saved to LCSNative.ini; resolution
applies at the next start.

Saves
-----
Saves live in sdmc:/switch/LCSNative/game/PSP/SAVEDATA/ (one folder per save,
as on the PSP). "Load game" and saving show the save list: A picks, B goes
back. Saves from the game's other releases (EU, JP...) are listed too, marked
with their region; they may not work with this one. Saves from a PSP or from
PPSSPP are encrypted: they are decrypted when loaded, with the key the game
itself uses (a save of another release may use another key).

Speed
-----
The game does not change the clocks. With sys-clk, set them yourself
(Ultrahand / sys-clk overlay); a faster CPU helps the most.

Files
-----
LCSNative.ini     your settings (renderer, resolution, controls). Made from
                  LCSNative.default.ini on first start; updates leave it
                  alone. Delete it to get the defaults back.
lcs_env.txt       debug options

Problems
--------
Send lcs_log.txt, lcs_log_prev.txt and lcs_samples.bin from
sdmc:/switch/LCSNative/, and any file in sdmc:/atmosphere/crash_reports/ from
the time of the crash.
If the picture is wrong, try Backend=Software in LCSNative.ini.
