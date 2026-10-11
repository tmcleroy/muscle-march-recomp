# Plan: specialised TEV shaders

A plan for whoever takes this on. Nothing here is implemented yet.

## Goal

Let the internal resolution go above 3x again (4x to 6x, so a 4K screen gets a full-detail
picture) with menus and the title screen at a steady 60 fps on an Apple M4 and on a Steam Deck at
its own sensible scale. The picture must not change: deterministic replays must give the same
frame hashes as today.

The limit to lift is `kMaxScale = 3` in `port/engine/src/gpu/gx_render_gpu.cpp`, repeated in
`gx_render.cpp`, `gx_render_gl.cpp`, `port/engine/src/platform/options.cpp` and the launcher
(`port/games/musclemarch/launcher/src/app.cpp`, `video_page`).

## What is known

Measured for PR #4 on an Apple M4, SDL_GPU on Metal, title screen at 4x:

| Variant | GPU time per frame |
| --- | --- |
| TEV shader before PR #4 | about 42 ms |
| TEV shader now (register arrays as globals) | 16-19 ms |
| A trivial pixel shader in its place | about 3 ms |
| Draws skipped | about 2 ms |

- The cost is the pixel shader's own arithmetic, not copies, the copy filter, the sampler switch
  or the number of passes. Races are fine at the same scale; menus and the title are not. The
  likely reason is that menus stack blended full-screen layers, so each pixel is shaded several
  times. That layer count was not measured.
- Within the shader, the stage loop is what remains: the code before it costs about 4 ms and the
  code after it about 3 ms.
- Three rewrites that kept the general shader made no gain or a loss: bounding the gradient loop
  by the texgen count (about 48 ms), lookup tables for the input selections (about 22 ms),
  separate variables for the registers instead of arrays (about 35 ms).
- These timings came from temporary instrumentation that was not committed. The `fps` line in
  `game.log` reports CPU-side drawing time only.

## How the renderer works today

- One general pixel shader, `kTevFragment` in `port/engine/src/gpu/gx_render_gl.cpp`, interprets
  the Wii's TEV setup per pixel. The setup arrives in the `Constants` uniform block
  (`gx_render_common.h`): `header.x` is the stage count, `stage[16]` holds each stage's colour
  word, alpha word, order word and konst selection, `tevind[16]` the indirect setup, `indirect`
  the indirect stage count and texgen count, plus fog and the alpha test.
- `main` computes gradients for all 8 texture coordinates, runs the indirect stages, then loops
  over `count` stages. Every input selection, bias, scale, compare mode, swap table and wrap mode
  is a branch or switch on uniform bits. Texture maps are chosen by an 8-way switch
  (`sample_map`).
- SDL_GPU takes precompiled shaders only. `port/tools/gen_gpu_shaders.py` turns the GLSL into
  SPIR-V (glslangValidator) and MSL (spirv-cross) and writes the committed
  `port/engine/src/gpu/gpu_shaders.inc`, so a normal build needs neither tool. There are two
  variants today: `tev_fragment` and `tev_early_fragment` (early depth test).
- Pipelines are cached in `tev_pipeline()` in `gx_render_gpu.cpp`, keyed by blend word, depth
  state, early depth and pass. The pixel shader is picked there.
- The OpenGL renderer compiles the same GLSL at start-up and could compile more at run time; the
  SDL_GPU renderer, which is the default, cannot without a shader compiler in the game.

## Approach

Replace interpretation with one small shader per TEV setup, keeping the general shader as the
fallback for any setup without one.

**Specialisation key.** Everything in `Constants` that changes the code path and not just a
value: stage count; per stage the colour word, alpha word, order word, konst selection and
`tevind` word; the swap tables; indirect stage count, references and texgen count; alpha test
modes; fog mode. Colours, konst values, texture sizes, matrices, fog parameters and alpha
references stay as uniforms.

**Generator.** A function that takes a key and writes GLSL with the same integer arithmetic as
`kTevFragment`, with the loop unrolled and every selection resolved: only the gradients, samples
and registers the setup uses. Write it so its output can be compared with the general shader
(see Verification). Keep the general shader as the single description of the arithmetic; where
the two could drift, generate both from shared pieces.

**Where the shaders come from** is the main design decision. In order of preference:

1. *Build-time set, general shader as fallback.* The game is fixed, so the set of setups it uses
   is finite. Record the keys the game uses (a `WP_LOG_TEV_KEYS` style dump during the three
   movies and a play-through of every menu and mode), generate GLSL for each, and run them through
   `gen_gpu_shaders.py` into `gpu_shaders.inc`. At run time `tev_pipeline()` looks the key up
   and falls back to the general shader. No compiler ships with the game and no stutter.
   **Ask the owner first:** the key list comes from running the game. The repository rule is
   never to commit game data or anything derived from it. A list of render-state words is not
   game content in the way assets or translated code are, but the owner decides. If the answer
   is no, the keys can be recorded on the player's machine instead, which needs option 2 or 3 to
   turn them into shaders.
2. *Compile at run time.* Generate GLSL (or SPIR-V directly) in the game and compile it there.
   Needs a compiler in every build (glslang plus spirv-cross, or SDL_shadercross), compiling on a
   worker thread while the general shader draws, and a disk cache. Covers every setup without a
   recorded list, at the cost of build size, licences to add to `THIRD_PARTY_NOTICES.md`, and
   work on all four platforms.
3. *Partial specialisation with no recorded list.* Precompile a fixed family of variants that
   needs no knowledge of the game: one per stage count with the loop unrolled, with flags for
   "no indirect stages", "no fog", "alpha test always passes". Much less work, but the
   per-stage selections stay as uniform branches, and PR #4's experiments suggest that is where
   the time goes. Only worth it if step 2 below shows a large gain.

## Steps

1. **Permanent GPU timing.** Add a log line with GPU time per frame (on Metal the command
   buffer's `GPUStartTime`/`GPUEndTime`; elsewhere a blocking submit around the frame, behind an
   environment variable). Reproduce the table above at 4x before changing anything. Take every
   comparison back to back on an idle machine: an earlier table in PR #4 was wrong because other
   programs were loading the graphics card.
2. **Find the ceiling by hand.** Dump the keys the title screen and one slow menu use, with how
   many pixels each covers. Hand-write the specialised shader for the top one or two and measure.
   If the title screen at 4x does not come down to roughly 5 ms, stop and report: the premise is
   wrong and the layer count or something else is the cost.
3. **Count the setups.** Log distinct keys across the three movies in `movies/` and a manual pass
   through every menu. This sizes option 1 (tens of shaders is fine; thousands is not).
4. **Decide the source of shaders** with the owner, using the numbers from steps 2 and 3.
5. **Write the generator and wire the lookup** into `tev_pipeline()`, for both the plain and the
   early-depth variants. Extend `gen_gpu_shaders.py` rather than adding a second tool.
6. **Blurred menus.** Menus that blur the background copy the picture back into game memory and
   wait for the graphics card (`write_copy_to_ram`, a blocking submit). Before PR #4's shader
   change that wait was about 28 ms per frame at 4x against 5 ms at 1x. Re-measure once the
   shader is fast; if it still matters, do the read-back from a native-size copy.
   `video.copies_to_ram` already turns it off, and it is off by default on Android.
7. **Raise `kMaxScale`** to what the measurements support, restore the launcher's wording for 4K,
   and update `CHANGELOG.md`.

## Verification

- `make replay-test FRAMES=1` (never two at once) must match `build/golden` for `smoke_warmup`,
  `autoplay_story` and `enhanced_story`. Memory hashes prove the game logic is untouched; frame
  hashes prove the picture is. Also compare frame hashes old against new with `WP_SCALE=3`; check
  first which scale the golden frame hashes were taken at.
- A fallback check: with specialised shaders turned off by an environment variable the frame
  hashes must be the same as with them on.
- Log how many draws used the fallback shader. For option 1 it should be zero across the movies.
- GPU time per frame on the title screen, the main menu, a blurred menu and a race, at 3x, 4x and
  6x, before and after, on the M4. A Steam Deck number at 2x and 3x if the device is awake.
- The OpenGL renderer shares the GLSL and is chosen when the game is built (the `WP_RENDERER`
  CMake option; the code tests `WP_RENDERER_GPU`): check it still compiles and draws. Linux and Windows builds go through CI for compiling only; say plainly
  which platforms were run.

## Rules that apply

- Never commit game data or anything derived from it; run `tools/audit.sh` before pushing.
- Work in a git worktree: other sessions may be using the main checkout.
- Stop Dolphin with SIGTERM, never SIGINT.
- The default picture must stay exactly as it is today.
