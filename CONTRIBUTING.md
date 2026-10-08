# Contributing

Thanks for helping! Bug reports from real machines and controllers are the most valuable thing
during the beta, and code, docs and testing are all welcome.

## The rules that keep this project legal

These aren't negotiable; pull requests that break them are closed.

1. **Never add anything from the game.** No WAD, DOL, `.app` contents, extracted files, textures,
   models, sounds, text dumps, save files, RAM dumps, screenshots (beyond the few maintained in
   `docs/screenshots/` for the README), or code
   generated from the game (the recompiler's output in `port/build/` and dtk's symbol and split
   lists stay on your machine; `.gitignore` covers them). The same goes for issues: attach logs,
   not game files.
2. **Never add keys**, or point to where keys, WADs or other copyrighted files can be found.
3. **Only add code you have the right to contribute**, under the GPL-3.0. Code from other projects
   must have a compatible license and be credited in `THIRD_PARTY_NOTICES.md`.

Hand-written C++ that does what a game function does (`port/games/musclemarch/src`, "lifted"
code) is fine and welcome: write it yourself from your own analysis, the way the existing files do.

## Reporting bugs

Open an issue with:

- your system (OS and version, CPU, GPU) and how you installed (release build or source);
- your controller, and whether you play Pure or Enhanced (and which features are on);
- what happened and what you expected, and how to make it happen again;
- the game's output: `logs/game.log` in your user folder (the launcher's **Open folder** button),
  or what the launcher prints when run from a terminal. The launcher's **Report a bug** button
  fills in most of this for you.

## Building and testing

[docs/BUILDING.md](docs/BUILDING.md) explains the build with your own WAD. Before any pull request,
run `tools/audit.sh`: it checks that no game files, keys or personal paths are in the repository
(CI runs it too, and builds the engine, the game code and the launcher on Linux and macOS without
the game). Before a pull request that touches game code or the runtime:

- run `make replay-test`: with every feature off, the game must behave exactly as before;
- if you changed a lifted function, run it under `WP_VERIFY=1` (see BUILDING.md).

## Changing gameplay or controls

Anything that changes how the game plays, looks or sounds compared with the Wii is an optional
**feature** (`port/games/musclemarch/features/`, registered with `WP_FEATURE`), off in the Pure
preset, with a toggle in the launcher. With all features off the port must play exactly like the
Wii. See [docs/FEATURES.md](docs/FEATURES.md).

## Code style

- Match the surrounding code: naming, comment density and idiom. Comments explain why, and name
  the game function a lifted function comes from (`fn_8002B528`).
- C++20; 4-space indents, braces on the same line, lines up to about 140 columns.
- Commit messages: a short summary line, then what changed and why.

`port/` is vendored from wiiparty-recomp. Its own `README.md` and `CONTRIBUTING.md` describe the
upstream project; for this repository, this file applies.

## Code of conduct

Be kind and constructive. See [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).
