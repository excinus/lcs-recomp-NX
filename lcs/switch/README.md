# Nintendo Switch port (Atmosphère homebrew)

LCSNative built as a Switch homebrew `.nro`, from the same sources as the PC
build. Everything Switch-specific sits behind `__SWITCH__`, `NINTENDO_SWITCH`
(CMake) or `LCS_GL_GE_BACKEND`; the Windows and Linux builds do not change.

Player instructions: [`README_SD.txt`](README_SD.txt) (it ships in the zip).

## Status

| Phase | State |
| --- | --- |
| Builds for the Switch (devkitA64 GCC 15, libnx, SDL2, FFmpeg, Mesa) | Done, in CI (`.github/workflows/switch.yml`) |
| Platform layer: paths, log, env file, threads/cores, big game stack, sys-clk, applet focus | Written, not yet run on a console |
| Picture: OpenGL GE backend (Switch GPU) | Written; renders the same pixels as the Vulkan backend on desktop Mesa (see Testing) |
| Picture: software GE + SDL GLES2 presenter | Unchanged PC code, presented through SDL's GPU renderer |
| Sound (SDL audio), controllers (SDL game controller) | PC code through devkitPro's SDL2 |
| Videos (FFmpeg) | PC code with devkitPro's FFmpeg 7.1 |
| Runs on a console | Yes (OLED, firmware 22.5): boots, plays, OpenGL GE on Mesa 20.1 / NV120. About 10 fps at first; audio stutters because the game runs slow. Profiling in progress |

## How the PC port maps onto Horizon

Answers to the feasibility questions from the porting guide:

- **Guest memory** is a plain buffer (`GuestMemory`: `ram_data_ + offset`), so
  Horizon's lack of `mmap` at fixed addresses and ASLR do not matter. No 4 GB
  window, no custom loader: the `.nro` is the game.
- **Guest threads** are scheduled by the runtime itself (no fibers, `ucontext`
  or signals). Host threads are `std::thread`.
- **Floating point** is the host FPU, as on PC. There is no netplay or replay
  that needs bit-equal results.
- **Graphics**: no Vulkan for homebrew, so the Vulkan GE backend was ported to
  OpenGL 3.3 core (`host/ge_gpu_backend_gl.cpp`), which Mesa (nouveau) runs on
  the Switch through SDL2's EGL. The software rasterizer is kept as the
  fallback.
- **SDL**: devkitPro ships SDL2 2.28 for the Switch (`switch-sdl2`), the same
  major version the Linux build uses, so no SDL port was needed.
- **No processes, `dlopen`, environment variables or `sysconf`**: the runtime
  uses none of the first two; environment variables come from `lcs_env.txt`;
  `std::thread::hardware_concurrency()` returns 0, so thread pools are sized by
  `PSPRECOMP_RASTER_THREADS` / `PSPRECOMP_GE_DECODE_THREADS` defaults.

## Files

| File | What |
| --- | --- |
| `switch.cmake` | Build branch included by `lcs/CMakeLists.txt` on `NINTENDO_SWITCH`; NACP, NRO, link wraps |
| `switch_platform.cpp` | Entry (`__wrap_main`), log, env file, clocks, thread placement, missing-files screen |
| `build.sh` | Configure, build, stage the SD layout in `out/switch-sd/` |
| `LCSNative.ini` | Switch defaults, shipped as `LCSNative.default.ini`; the game copies it to `LCSNative.ini` when there is none, so updates keep the player's settings |
| `lcs_env.txt` | Switch/debug options, all commented out |
| `README_SD.txt`, `game_README.txt` | Player notes shipped on the card |
| `icon.jpg` | Homebrew menu icon (256x256, the game's cover) |
| `symbolize.py` | Turns `lcs_samples.bin` from the console into function names |
| `../host/ge_gpu_backend_gl.cpp` | OpenGL GE backend (also builds on Linux with `-DLCS_GE_OPENGL=ON`) |
| `../../tests/lcs_ge_gl_backend_test.cpp` | Desktop check of that backend |

Small `__SWITCH__` changes in shared code:

- `display_window_sdl.cpp`: full-screen 1280x720 window with desktop-GL
  attributes; SDL's accelerated renderer for the software path; touch does not
  become mouse input (it would turn the camera and fire); every connected
  controller drives the game; L3 + R3 opens the settings panel (the PC's F10)
  and neither click reaches the game while both are held; d-pad moves, B
  closes; in menus A and B trade places when `[Switch] ConfirmButton=A`.
- `lcs_display_menu.cpp`: the panel is a row table. The Switch rows are
  resolution (1x-3x only), view distance (1-2), frame rate, FPS counter,
  confirm button and overclock; the PC keeps its five rows.
- `lcs_prompt_textures.cpp`: `ButtonPrompts=Switch` paints A/B/X/Y over the
  PSP glyphs as each texture is decoded (no files from the disc needed; also
  works on PC). It measures the game's glyph (disk, ring, fill and symbol
  colours) and redraws it with a 7x8 letter; the white mask font gets the
  letter cut out, as the system font draws its button glyphs. The pause-menu
  chips follow the confirm setting, the in-world help text the gameplay
  positions. `prompt_texture_variant` lets the GL backend re-decode them when
  the setting changes.
- `ge_renderer.cpp`: raster workers spin 1024 times instead of 262144 before
  parking, and spin-waits yield (Horizon never time-slices threads of equal
  priority, so a spinner holds its core).
- `runtime.cpp`: `set_game_root` does not canonicalize (`sdmc:` paths).
- `lcs_profile.cpp`: disc reads keep the last four files open (each open is a
  file-service request on the Switch).
- `lcs_render_config.cpp`: `Backend=OpenGL`/`Software` accepted (also on PC);
  the backend's display name is "OpenGL" when the GL backend is built; a
  `[Switch]` section (`ConfirmButton`, `Overclock`); on the Switch internal
  resolution is clamped to 3x and view distance to 2.
- `host_font_5x7.hpp`: `+` and `-` glyphs for the panel.
- Top-level `CMakeLists.txt`: GNU C++ dialect (newlib hides `setenv`/`dup2`
  in strict mode); the recompiler tools and tests are not built.
- `lcs/CMakeLists.txt`: generated units without debug info (`-g0`); the
  libraries built with `-fPIE` instead of `-fPIC` (no GOT loads, no
  interposition limits on inlining in one static executable).
- Top-level `CMakeLists.txt`: `PSPRECOMP_AOT_ASSUME_NO_WRITE_WATCH` and
  `PSPRECOMP_AOT_PRODUCTION_FASTPATHS` default to ON for the Switch (they
  only drop debug checks from every guest store and chained call).

## Start-up, threads and clocks

`__wrap_main` (the libnx entry calls it through `-Wl,--wrap=main`):

1. Takes the `.nro`'s folder from `argv[0]`, drops `sdmc:` (newlib resolves
   `/...` on the card, and `std::filesystem` treats only those as absolute),
   and `chdir`s there.
2. Keeps the previous log as `lcs_log_prev.txt`, opens `lcs_log.txt` once and
   `dup2`s it onto stdout and stderr. Both are fully buffered (64 KB) and a
   thread flushes them twice a second; `std::cerr`'s unitbuf is turned off.
3. Reads `lcs_env.txt` into the environment, then sets defaults.
4. Logs the applet type (applet mode means the Album: little memory), model,
   firmware and memory.
5. Shows a text screen and exits if `game/EBOOT.ELF` is missing.
6. Opens sys-clk's service. Once the game has read `LCSNative.ini`,
   `[Switch] Overclock` (or the panel) asks it for CPU 1785 MHz and memory
   1600 MHz (`sys:clk` command 8, temporary overrides). An applet hook
   releases them when the game loses focus (HOME, sleep) and takes them again
   on return; `atexit` releases them. Without sys-clk nothing happens.
   `LCS_NX_CPU_BOOST=1` uses the console's own boost mode instead (the GPU
   drops to 76.8 MHz, so only for the software GE).
7. Starts the performance watch (below).
8. Runs the PC `main()` on a thread with a 64 MiB stack (Windows links with
   `/STACK:67108864`) pinned to core 0, with `--game /switch/LCSNative/game`.

Every other thread goes through `__wrap_pthread_create` and is placed on cores
1 and 2 (alternating ideal core, affinity mask 1|2). Horizon starts new threads
on the process's default core and never moves them, so without this the GE
worker and raster workers would share core 0 with the game. Each gets at least
8 MiB of stack (Linux's default, which the PC code assumes; libnx's is much
smaller). Defaults: two raster participants (the GE thread and one worker), no
extra decode threads.

## Performance watch and profiler

Every 5 s `lcs_log.txt` gets a `[watch]` line: FPS, each known thread's share
of a core (`game`, `ge`, and the workers `t0`, `t1`, ...), memory, and the GPU
backend's frames, draws per frame, texture uploads and decodes. A sampling
profiler pauses each thread about 100 times a second (`svcSetThreadActivity`,
`svcGetThreadContext3`) and logs its hottest addresses as offsets from the
program's first byte; all samples go to `lcs_samples.bin`. To read them:

```sh
python3 lcs/switch/symbolize.py LCSNative.elf lcs_samples.bin lcs_log.txt
```

`LCSNative.elf` must be from the same build (CI's `LCSNative-map-<version>`
artifact). `LCS_NX_PROFILE=0` in `lcs_env.txt` turns the sampling off.

## The OpenGL GE backend

A line-by-line port of `ge_gpu_backend_vulkan.cpp`: same batching, same
render targets per PSP framebuffer address, same texture keys and cache, same
uniform block and shaders (GLSL 3.30, the only math change is GL's -w..w clip
depth). Notes:

- The vertex shader negates Y like the Vulkan one, so row 0 of every target is
  the PSP's top line. Scissors and readbacks use PSP rows directly; only the
  final present flips. Front faces are swapped against Vulkan (Vulkan's
  polygon area has the opposite sign); `LCS_GL_INVERT_WINDING=1` flips them
  again if culling ever looks wrong.
- All of a frame's uniform blocks go up in one buffer upload and are bound by
  range; redundant state, texture, sampler and scissor changes are skipped.
- The context is made current for each GL scope (frame finish, video frame)
  under the backend mutex and released after, so the GE worker and the thread
  showing videos can both use it.
- Textures drop their CPU copy once uploaded.
- `LCS_GL_READBACK=1` reads the frame back instead of presenting it (tests);
  `LCS_GL_VSYNC=1` turns on swap interval 1.

## Testing

Done:

- The whole game builds and links for the Switch (CI artifact).
- The Linux build (Vulkan, default) and its tests still pass.
- `tests/lcs_ge_gl_backend_test.cpp` draws synthetic GE work (through-mode and
  hardware-transform triangles, scissor, 2x2 texture, alpha blend, inverted
  depth test, culling both ways, render-to-texture) with the OpenGL backend and
  checks pixels. The same draws went through the Vulkan backend on Mesa's
  lavapipe: the steady-state frames are identical pixel for pixel (0 of
  130,560 differ). The one first-frame difference is the GL backend showing a
  texture decoded that same frame; the Vulkan backend shows it one frame later.
  The direct present (no readback) was checked visually under Xvfb.

```sh
cmake -S . -B out/gl -G Ninja -DLCS_GE_OPENGL=ON
ninja -C out/gl lcs_ge_gl_backend_test
xvfb-run -a ctest --test-dir out/gl -R lcs_ge_gl_backend_test --output-on-failure
```

Not done: anything on a console. First console run, what to look at:

1. Does it start (homebrew menu through a game, hold R)? If it closes at once,
   send `lcs_log.txt` and `sdmc:/atmosphere/crash_reports/`.
2. Log lines `[switch] ...` (applet type, sys-clk) and `[gl] ...` (GL version,
   or why the OpenGL GE fell back to software).
3. Title screen, menus, a cutscene video, sound.
4. Driving around: FPS counter (top left), stutter, picture errors (photos
   help). Try `Backend=Software` and `InternalScale=1` for comparison.
5. Controllers: handheld, detached Joy-Cons, Pro Controller.
6. HOME and back, sleep and back, closing from HOME.

## Building

```sh
docker run --rm -v "$PWD:/src" -w /src devkitpro/devkita64:latest lcs/switch/build.sh
```

Or with devkitPro installed locally (`switch-dev`, `switch-sdl2`,
`switch-ffmpeg`, `switch-mesa`): `DEVKITPRO=/opt/devkitpro lcs/switch/build.sh`.
The `.nro` is `out/lcs-switch/lcs/LCSNative.nro`; `out/switch-sd/` is the SD
card layout. A clean build takes about 20 minutes on 4 cores, nearly all of it
the generated units; CI caches them with ccache.

## Possible next steps

- Console results first (above).
- A deko3d backend if Mesa's CPU cost per draw turns out to be the limit (the
  Vulkan-shaped design ports to deko3d more directly than to GL).
- Ring buffers with fences instead of orphaned buffers, if uploads show up.
- Software keyboard / Joy-Con details if the game ever asks for text.
