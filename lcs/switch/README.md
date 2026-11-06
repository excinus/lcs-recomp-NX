# Nintendo Switch port (Atmosphère homebrew)

LCSNative built as a Switch homebrew `.nro`, from the same sources as the PC
build. Everything Switch-specific sits behind `__SWITCH__`, `NINTENDO_SWITCH`
(CMake) or `LCS_GL_GE_BACKEND`; the Windows and Linux builds do not change.

Player instructions: [`README_SD.txt`](README_SD.txt) (it ships in the zip).

## Status

| Phase | State |
| --- | --- |
| Builds for the Switch (devkitA64 GCC 15, libnx, SDL2, FFmpeg, Mesa) | Done, in CI (`.github/workflows/switch.yml`) |
| Platform layer: paths, log, env file, threads/cores, big game stack, profiler | Written, not yet run on a console |
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
  and neither click reaches the game while both are held; d-pad moves, the
  confirm button (A, or B with `ConfirmButton=B`) changes a row or picks a
  save, the other closes; in menus A and B trade places when
  `[Switch] ConfirmButton=A`.
- `lcs_display_menu.cpp`: the panel is a row table. The Switch rows are
  resolution (1x-3x only), view distance (1-2), frame rate, FPS counter,
  confirm button and ZL / ZR; the PC keeps its five rows. The ini
  is read in full and closed before it is written again (Horizon refuses to
  open a file for writing while another handle has it open, which is why the
  first builds lost every change).
- `lcs_save_picker.cpp`: the PSP's save list for `sceUtilitySavedata`
  LISTLOAD / LISTSAVE (the game's "Load game" and save prompts). The game
  only names its slots (`saveNameList`); the list shows them with the
  `PARAM.SFO` title and detail and the file date, asks before overwriting,
  and writes the chosen slot into `saveName`. Saves of the game's other
  releases (EU, JP disc IDs, or a `PARAM.SFO` titled Liberty City) are listed
  for loading; they are read from their own folder and saved back under this
  release's ID. Saves now get a `PARAM.SFO`. Off on the PC unless
  `LCS_SAVE_PICKER=1`.
- The save list is drawn as the PSP's save utility (`save_picker_image`, a
  960x544 picture): the saves' `ICON0.PNG` (decoded with FFmpeg's PNG
  decoder, `lcs_png.cpp`) in a column with the selected one large, its
  `PARAM.SFO` title, detail lines and date beside it, charcoal with the red of
  the game's logo. The OpenGL and SDL presenters show it over the frame; the
  others keep the text panel.
- `lcs_ge_exec.cpp`: block transfers (`TRXKICK`, 0xEA) copy their rectangle
  in guest memory.
- `ge_gpu_backend_gl.cpp`: a GL thread. The GE worker hands each finished
  frame over (its vertices, indices and draws) and decodes the next one while
  the GL thread uploads it, draws it and swaps; one frame in flight. The
  driver's share (submissions, buffer copies, waits, the draw loop) was about
  a third of the GE worker's time. Planning (textures, targets, uniforms)
  runs under the backend's lock, the draws without it; every GL call is on
  that thread, the guest-frame present included. Switch default,
  `LCS_GL_THREAD=0` / `1`. Vertices grow without being initialised first.
- Threads: the GE worker has core 1 to itself (see "Start-up and threads").
- `lcs_file_cache.cpp`: the game's `.IMG` archives are read whole into memory
  by a background thread the first time the game reads one (each up to
  384 MiB, 1 GiB in all; the log says `[file-cache] NAME in memory`). Reads
  of a cached archive (`sceIoRead`, the raw disc-sector reads, and the view
  distance option's neighbouring sectors) are then copies. With View
  distance 2 every sector change read nine sectors (6-8 MB) from the card on
  the game thread, and the game thread spent about 7% of a drive in file
  system requests: the drops to 20 fps. `LCS_FILE_CACHE=0` turns it off.
- A crash writes `[crash]` lines to `lcs_log.txt`: the exception, the thread,
  PC / LR and the frame chain as offsets from the image base (place them with
  the build's `LCSNative.elf` and `aarch64-none-elf-addr2line`), the far
  address and the registers. The exception is then passed on, so Atmosphere's
  crash report and the system error still follow.
- The draw-decode worker spins 64 rounds instead of 2000 before sleeping (a
  round is a yield, a system call, and its core is shared).
- Sound goes out through libnx's `audout` (`AudoutDevice` in
  `lcs_audio_output.cpp`) instead of SDL. SDL's Switch audio took one block
  per pass of its thread, so a late pass was a gap the hardware filled with
  silence, never made up: the mixer's queue overflowed about six times a
  second even at a steady 30 fps and guest time (crackle), and in one test
  its thread stopped for good. Our thread waits for audout to hand back one
  of four buffers, refills it from the queue (silence while prebuffering)
  and appends it, at a priority above the other core-2 threads; after a
  second with nothing handed back it restarts audout. The mixer runs at
  48 kHz on the Switch (audout's only rate; the PSP's 44.1 kHz streams are
  resampled).
- The island loading-card experiments are gone (stalled presents, frame
  dumps, list digests): the PC port shows no card either.
- `lcs_profile.cpp`: with `LCS_GE_OVERLAP=1`, `sceGeListEnQueue` does not
  wait for the GE worker to go idle (at most four lists queued); the game
  waits in `sceGeDrawSync`, as on a PSP, and a list's finish callback waits
  for the worker to have run that list (the game reuses the list's memory
  once it is called). The prescan keeps its own BASE/OFFSET registers.
  `[watch]` shows how long the game waits. Off by default: the first console
  test, before callbacks waited, drew lists the game was rewriting.
- `ge_gpu_backend_gl.cpp`: geometry and uniforms go into one of three sets
  of buffers in turn, each kept at its size (`glBufferSubData`); a new
  `glBufferData` size every frame made the Switch's driver allocate and map
  megabytes of fresh memory per frame. `LCS_GL_UPLOAD_RING=0` restores it.
- CPU-time ticks no longer carry the guest clock ahead of real time (Switch
  default; `LCS_GUEST_CLOCK_CLAMP=0` off); `[watch]` shows `guest clock N%`
  (100% is right). The mixer's headroom over the prebuffer is a third of it
  (4 blocks at the Switch's 12) instead of 2.
- `ge_gpu_backend_gl.cpp`: per-draw uniforms go in arrays of 341 blocks per
  64 KiB range; each draw finds its block through an instanced attribute and
  its base instance (GL 4.2), so the range is bound once per 341 draws, not
  per draw (each bind made nouveau re-emit the constant buffer).
  `LCS_GL_BATCHED_UNIFORMS=0` goes back to a range per draw.
- `[watch]` lines count audio gaps (queue ran dry) and skips (blocks dropped
  over the latency limit).
- `lcs_savedata_crypto.cpp`: reads saves a PSP or PPSSPP encrypted
  (`SAVEDATA_PARAMS` 0x01/0x21/0x41, modes 1/3/5). Whether a `DATA.BIN` is
  encrypted is decided by its bytes (near 8 bits of entropy), not by
  PARAM.SFO, since a save decrypted on a PC keeps the PSP's PARAM.SFO. The
  load decrypts with the key the game passes the utility (`key`, at 0x5DC)
  and refuses with LOAD_DATA_BROKEN when the result is still noise. Saves are
  written back plain. Own AES-128; checked against known answers.
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
  `[Switch]` section (`ConfirmButton`, `Triggers`); on the
  Switch internal resolution is clamped to 3x and view distance to 2, and
  ZL / ZR follow `Triggers` (d-pad left/right by default).
- `lcs_key_bindings.hpp`: a trigger keeps its buttons in a vehicle unless it
  also accelerates or brakes (so ZL / ZR change the radio station).
- `lcs_profile.cpp`: with the GPU presenting directly, a frame the game drew
  itself (no GE frame) is read from the buffer it displays, not from the last
  GE target, whose memory the GPU never wrote.
- `ge_gpu_backend_gl.cpp`: the packed 0115 vertices go straight into the
  upload buffer.
- Audio: the sound queue is 12 blocks (139 ms) instead of 6, so slow frames do
  not drain it.
- `host_font_5x7.hpp`: `+ - : / % ( ) , ' ! ? & < >` glyphs for the panel.
- Top-level `CMakeLists.txt`: GNU C++ dialect (newlib hides `setenv`/`dup2`
  in strict mode); the recompiler tools and tests are not built.
- `lcs/CMakeLists.txt`: generated units without debug info (`-g0`); the
  libraries built with `-fPIE` instead of `-fPIC` (no GOT loads, no
  interposition limits on inlining in one static executable).
- Top-level `CMakeLists.txt`: `PSPRECOMP_AOT_ASSUME_NO_WRITE_WATCH` and
  `PSPRECOMP_AOT_PRODUCTION_FASTPATHS` default to ON for the Switch (they
  only drop debug checks from every guest store and chained call).

## Start-up and threads

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
6. Starts the performance watch (below).
7. Runs the PC `main()` on a thread with a 64 MiB stack (Windows links with
   `/STACK:67108864`) pinned to core 0, with `--game /switch/LCSNative/game`.

Every other thread goes through `__wrap_pthread_create` and is placed on core
2; the GE worker then moves itself to core 1, which it has alone. Core 2
holds the GL thread, draw decoding, the raster helper, sound and the profiler.
Horizon starts new threads on the process's default core and does not
time-slice equal priorities, so threads that share a core take turns: the old
layout (alternating cores 1 and 2 in creation order) put the draw-decode
worker on the GE worker's core. Each gets at least
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
2. Log lines `[switch] ...` (applet type, model, memory) and `[gl] ...` (GL version,
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
