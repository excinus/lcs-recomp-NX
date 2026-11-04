LCS Recomp for Nintendo Switch (Atmosphere homebrew)
=====================================================

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
5. Pick "LCS Recomp".

Controls
--------
In play, the PSP's buttons are where the Switch's are:
  B = run                    A = attack
  Y = jump                   X = enter or leave a vehicle
  ZR = accelerate, or aim    ZL = brake, or centre the camera
  R = aim (handbrake)        L = centre the camera, pick up a weapon
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
rate, FPS counter, confirm button, overclock. D-pad to change, B to close.
Changes are saved; resolution applies at the next start.

Speed
-----
If sys-clk is installed (it is in most CFW packs), the game raises the CPU
to 1785 MHz and the memory to 1600 MHz while it is in focus, and gives the
clocks back on HOME, sleep and exit. Settings > Overclock turns this off.

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
