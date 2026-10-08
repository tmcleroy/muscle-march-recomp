# Contributing to Wii Party Recomp

Thank you for helping improve the project! This document outlines the guidelines and technical requirements for contributing to this static recompilation ecosystem.

---

## Intellectual Property & Asset Protection
- **No Copyrighted Material:** Never upload, commit, or link to any copyrighted Nintendo data. This includes `main.dol`, `.rel` files, ISO/WBFS/RVZ images, textures, models, audio, fonts, videos, or memory dumps. This rule strictly applies to commits, issues, and pull requests.
- **Git Boundaries:** The directories `games/wiiparty/disc/`, `games/wiiparty/extracted/`, and `reference/` are explicitly ignored by Git. Never alter these exclusions.
- **Legal Source:** You must use your own legally obtained copy of Wii Party to develop, test, or verify code.

---

## Licensing & Third-Party Code

### Project License
All contributions are licensed under the **GNU General Public License, version 3 or later (GPL-3.0-or-later)**. By submitting a pull request, you certify that you possess the legal right to license your work under these terms.

### Third-Party Ingestion Rules
- **Granular Auditing:** Do not copy code from external projects unless its license is strictly compatible with GPL-3.0-or-later. Audit authors and individual files, not just the root repository license.
- **Retain Copyrights:** Always preserve original copyright notices and license text inside any added file.
- **Provenance Logging:** Record the precise origin in `THIRD_PARTY_NOTICES.md` (include the upstream path, commit/revision, authors, and exact license). Do not log this information in code comments.
- **Strict Prohibitions:** Code from InputEvelution/wp without explicit authorization, leaks, proprietary code, or unverified source material is strictly forbidden. If provenance or licensing is ambiguous, do not add the code.

---

## Code Quality & Style Conventions

- **Zero Code Comments:** Absolutely no comments are allowed inside source code files (`.c`, `.cpp`, `.h`, or scripts). All technical notes, architectural rationale, and documentation must live inside `docs/DECOMP_PROGRESS.md`.
- **Clean Workspace:** Never commit generated code (`build/`), execution logs, or temporary files.
- **Isolation of Concerns:** Keep core recompiled game logic entirely separate from PC-specific enhancements (e.g., custom resolution or gamepad scaling inputs).

### Recompilation Fidelity
- **No Arbitrary Stubs:** Do not replace original game or SDK logic with clean stubs for convenience.
- **Allowed Substitutions:** Only hardware or console-specific dependencies (such as thread switching or cache operations) may be substituted. Every substitution must be explicitly justified and logged in `docs/DECOMP_PROGRESS.md`.

---

## Build, Test & Verification

All development builds must happen outside the source tree within the `build/` directory:

```bash
# 1. Regenerate C++ code from the binary toolchain
python recompiler/recomp.py

# 2. Configure the build environment
cmake -S . -B build/out -G Ninja -DCMAKE_BUILD_TYPE=Release

# 3. Compile the native executable
cmake --build build/out

# 4. Execute the suite of core engine tests
ctest --test-dir build/out
```

> [!IMPORTANT]
> Passing compilation or core unit tests does not guarantee correctness. Any change affecting the runtime must be manually verified by running the game binary (`build/out/wiiparty`). If a runtime test was not performed, you must state this clearly in your pull request description.

---

## Commits & Documentation

### Git Commit Style
- **Format:** Written in English, neutral tone, using the imperative mood, and ending with a period.
- **Atomicity:** One logical change per commit.
- **Example:** `Fix module function splitting.`

### Progress Tracking
You must update `docs/DECOMP_PROGRESS.md` immediately following any change that alters runtime behavior. State exactly what was verified and the methodology used. Never declare a feature as fully working if it has not been verified under test conditions.

---

## Reporting Issues

When filing a bug or a crash report, please include:
1. The exact commit hash of your branch.
2. Your operating system version (e.g., Windows 11).
3. The exact command-line arguments used.
4. The complete console output. For crashes, attach the exact call stack and register dump printed by the program.

*Reminder: Do not attach or upload any original game assets to the issue description.*
