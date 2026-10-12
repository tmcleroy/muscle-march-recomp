# Specialised TEV shaders

How the renderers draw the game's pixel pipeline (TEV) fast enough for internal resolutions up
to 6x, what was measured, and what is still open.

## What it does

One general pixel shader, `kTevFragment` in `port/engine/src/gpu/gx_render_gl.cpp`, interprets
the Wii's TEV setup for every pixel: each input selection, bias, scale, compare mode, swap table
and wrap mode is a branch on uniform bits. That arithmetic was the whole cost of menus and the
title screen at high resolutions, because those stack many blended full-screen layers (3 to 8
screens of pixels per frame, see below).

Now each TEV setup gets its own small shader, and the general shader is the fallback:

- **Key.** `tev_key()` in `gx_render_common.cpp` packs everything in `Constants` that selects the
  code path and not just a value: stage count, early depth test, alpha test modes, output
  conversion, fog mode, indirect setup, swap tables, and per stage the colour word, alpha word,
  order word, konst selection and indirect word. Bits that cannot matter (an unused konst
  selection, the map of a stage without a texture, indirect bits with no indirect stages) are
  cleared so equivalent setups share a key. Colours, konst values, texture sizes, matrices, fog
  parameters and alpha references stay uniforms.
- **Specialiser.** `specialise_tev()` in `tev_specialise.cpp` rewrites the general shader's own
  text for one key: the uniforms that are part of the key become constants and the loops over
  gradients, indirect stages and TEV stages are unrolled. A compiler then folds it down to the
  arithmetic that setup needs (a one-stage "texture times colour" setup becomes one texture sample
  and a multiply, with no `discard` if the alpha test always passes). Nothing is re-implemented,
  so the specialised arithmetic cannot drift from the general shader. If the general shader
  changes in a way the specialiser does not know, it fails loudly and the general shader is used.
- **OpenGL renderer** (`gx_render_gl.cpp`, Linux and the Steam Deck): compiles the specialised
  GLSL when a setup is first drawn. It needs no list of setups.
- **SDL_GPU renderer** (`gx_render_gpu.cpp`, macOS and Android): takes precompiled shaders only,
  so `port/tools/gen_gpu_shaders.py` builds the specialiser as a small program, runs it for every
  key listed in `port/games/*/tev_keys.txt`, compiles each result (glslangValidator, then
  `spirv-opt -O` to make it straight-line code, `spirv-val`, spirv-cross for Metal) and writes
  them into `gpu_shaders.inc` with a table of keys. Keys whose shaders come out the same share
  one. `draw()` looks the batch's key up; `tev_pipeline()` takes the shader index as part of its
  cache key and makes the shader the first time it is used. A setup that is not listed, or a
  shader or pipeline that cannot be made, draws with the general shader.
- **Direct3D 11 renderer** (`gx_render.cpp`, Windows): unchanged. It has its own HLSL shader and
  nothing here was built or run on Windows, so its limit stays 3x. It compiles HLSL at run time,
  so the OpenGL approach would carry over.

`kMaxScale` is 6 for the SDL_GPU and OpenGL renderers (also in `options.cpp` and the launcher's
`video_page`).

## The list of setups

`port/games/musclemarch/tev_keys.txt` has the 7 setups the game draws across the three movies in
`movies/` (start-up, title, mode select, the story stages, "Continue?", results, the attract
loop). No draw in those movies falls back to the general shader.

**Not yet covered:** a manual pass through every menu and mode (options, Endless Rush, multiplayer,
credits). Setups met there draw with the general shader: the right picture, at the old speed. To
add them, run the game with `WP_LOG_TEV_KEYS=<path to tev_keys.txt>` and play through them (new
setups are appended), then run `python3 port/tools/gen_gpu_shaders.py` and rebuild. The `tev` line
in `game.log` counts draws by kind of shader, and with `WP_LOG_TEV_KEYS` the game prints every
300 frames which keys covered the most pixels (`g` marks one drawn by the general shader).

**Why the list is in the repository:** it comes from running the game, and the repository rule is
never to commit game data or anything derived from it, so the owner was asked and approved it
(2026-10-11). The lines are render state words (for example "one stage, texture times vertex
colour, alpha test greater than a reference"), not assets or code. Without the list the SDL_GPU
renderer would need to compile shaders on the player's machine (a shader compiler in every build)
to get the same speed; the OpenGL renderer does not use it.

## Measurements

Apple M4, SDL_GPU on Metal, `movies/smoke_warmup.wpm` replayed headless and unthrottled with
`WP_LOG_GPU_TIME=1`: the game waits for the graphics card after every submit and reports the time
waited, averaged over 60 frames. General and specialised runs were taken back to back with the
same binary (`WP_TEV_GENERAL=1` for the general shader). Milliseconds of graphics card time per
frame:

| Screen | 3x general | 3x specialised | 4x general | 4x specialised | 6x general | 6x specialised |
| --- | --- | --- | --- | --- | --- | --- |
| Title screen | 18.2 | 3.1 | 31.5 | 4.3 | 68.3 | 8.8 |
| Mode select menu | 17.4 | 2.8 | 30.6 | 4.3 | 67.4 | 8.8 |
| Results | 16.6 | 3.0 | 29.8 | 4.6 | 63.2 | 8.7 |
| "Continue?" | 13.0 | 2.5 | 21.7 | 3.2 | 47.7 | 7.8 |
| Race (Warmup City) | 10.0 | 2.4 | 15.4 | 3.3 | 32.9 | 4.4 |
| Whole movie, mean | 10.2 | 2.3 | 16.4 | 3.2 | 35.2 | 5.1 |
| Worst second | 18.6 | 4.8 | 32.7 | 5.7 | 71.5 | 9.3 |

A frame at 60 fps has 16.7 ms, so 6x now fits with room to spare where 4x did not.

The whole of `movies/autoplay_story.wpm` (every story stage, about 24 minutes of play) at 6x with
specialised shaders: mean 5.6 ms, worst second 11.3 ms, and none of its 5.5 million draw batches
used the general shader.

Overdraw, from the `WP_LOG_TEV_KEYS` report (triangle area before depth and alpha tests): the
title screen, mode select and results draw 5 to 8.4 screens of pixels per frame, races 6 to 7.
Two kinds of setup carry almost all of it: untextured vertex colour (1.9 screens every frame) and
"texture times colour".

OpenGL renderer on the same machine (Apple's OpenGL 4.1, itself on Metal), same movie, cut off
after a fixed time. The number is the wait in `glFinish` at the end of each frame, which is less
than the whole graphics card time, so compare it only with itself: mode select at 6x 13.3 ms
general against 3.4 ms specialised, at 3x 3.8 ms against 1.9 ms; in four minutes of unthrottled
replay at 6x the general shader got through 3,180 frames and the specialised ones 10,080.

Not measured: a Steam Deck (it did not answer on the network when the build for it was ready),
Android, Vulkan in general. The specialised SPIR-V passes `spirv-val`; only the Metal path and
the OpenGL path on macOS were run. The Linux and Windows release builds
compile in the release Docker image; neither was run.

## Verification done

- Frame hashes (`WP_DET_FRAMES=1`, every 60 retraces) with specialised shaders against the general
  shader, same binary: identical for `smoke_warmup` at 3x, 4x and 6x (SDL_GPU) and for the OpenGL
  renderer at 3x and 6x over the part of the movie that was run.
- The binary from before this change against the new one, memory and frame hashes: identical for
  `smoke_warmup` and `autoplay_story` at 3x, and for all three movies at 1x
  (`make replay-test FRAMES=1` against golden files recorded from the old binary; the golden files
  that were in the main checkout had no frame hashes).
- No draw in the three movies used the general shader (the counts in the `WP_LOG_GPU_TIME` line).
- Setups the game does not use (early depth test, indirect stages with fog and a fixed
  destination alpha, 16 stages) were only checked to specialise and compile.
- `spirv-opt` turns `x / 255.0` into a multiplication by the reciprocal and Metal compiles with
  fast math, so the specialised shaders are not bit-for-bit the same floating point code as the
  general one. The frame hashes say the results after rounding to 8 bits are the same in
  everything the movies draw.

## Still open

- **A play-through of every menu and mode** to complete `tev_keys.txt` (above).
- **Windows**: specialise the HLSL shader of `gx_render.cpp` the same way, then raise its limit.
- **Steam Deck and Android numbers**, and a run of the specialised SPIR-V on Vulkan.
- **Blurred menus.** Menus that blur the background copy the picture back into game memory and
  wait for the graphics card (`write_copy_to_ram`, a blocking submit); before PR #4 that wait was
  about 28 ms per frame at 4x. The read-back is already made from a native-size copy; the wait is
  for the rest of the frame's drawing, which is what got faster here. In `autoplay_story` at 6x
  the 95 seconds that read back on every frame average 8.4 ms of graphics card time per frame
  (worst 11.3 ms), against 5.4 ms for the rest, so nothing more was done. The numbers include
  the waiting that `WP_LOG_GPU_TIME` adds; they were not taken at normal speed in a window.
- **First use of a shader.** The SDL_GPU renderer makes a specialised shader the first time its
  setup is drawn; on Metal that took under 1 ms each, so they are not made ahead of time. The
  OpenGL renderer compiles and links a program on first use, which was not timed.

## Tools

- `WP_LOG_GPU_TIME=1`: graphics card time per frame on standard error every 60 frames (SDL_GPU:
  every submit waits; OpenGL: `glFinish` at the end of the frame). For measuring only. Compare
  runs back to back on an idle machine.
- `WP_TEV_GENERAL=1`: draw everything with the general shader.
- `WP_LOG_TEV_KEYS=file` (SDL_GPU): append every setup drawn to the file and report coverage.
- The `fps` line in `game.log` still reports CPU-side drawing time only.

One trap found on the way: SDL's Metal backend tells that a submitted command buffer has finished
by its fence, and hands a released fence to the next command buffer. Releasing the fence right
after waiting, on every submit, meant no command buffer was ever cleaned up and graphics memory
grew by gigabytes a minute. `submit()` now releases each fence one submit late.

## Rules that apply

- Never commit game data or anything derived from it; run `tools/audit.sh` before pushing.
- Work in a git worktree: other sessions may be using the main checkout.
- `make replay-test FRAMES=1` (never two at once) must match `build/golden`.
- The default picture must stay exactly as it is.
