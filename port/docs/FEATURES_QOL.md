# Wii Party: quality-of-life features

Specification of features for the native PC version.

Scope: presentation, accessibility, performance, configuration, saving and connectivity. Keep the original look and behavior of the game wherever possible.

---

## 1. Video, performance and presentation

### 1.1. Dynamic aspect ratio

Goal: fit the game to wide and ultrawide displays without distorting the interface or 2D elements.

Requirements:
- Decouple the camera and field of view from the original 4:3 and 16:9 ratios.
- Compute the framing from the output resolution.
- Support formats such as 21:9 and 32:9.
- Keep `.brlyt` elements anchored to their relative positions.
- Do not stretch menus, scoreboards or other interface elements.

### 1.2. High refresh rate

Goal: present at 120 Hz, 144 Hz and higher without changing the game logic.

Requirements:
- Separate the presentation loop from the internal game logic.
- Make `VIWaitForRetrace` compatible with decoupled execution.
- Use timing based on `std::chrono` or an equivalent.
- Keep rendering rate, logic, physics and minigame timers independent.
- A higher frame rate must not speed up animations or gameplay.

### 1.3. High-resolution textures

Goal: allow custom higher-resolution textures.

Requirements:
- Create a resource replacement path inside `games/wiiparty/extracted/`.
- Look for a custom version of the resource on the PC file system first.
- Use the HD texture instead of the original `.brtex` resource when it exists.
- Fall back to the original resource.
- Document the naming convention and the directory structure.

Status: done in the engine's renderer rather than per resource: textures are replaced by their content hash with Dolphin's naming, from `games/wiiparty/textures/load/`, with dumping to `games/wiiparty/textures/dump/` (see the README, "Custom textures").

---

## 2. Pace of play

### 2.1. Dialog speed-up and skipping

Goal: reduce the time spent on repeated text and conversations.

Requirements:
- Allow dialog boxes to advance quickly.
- Add a configurable action that skips text instantly.
- Optionally increase the speed at which dialog text appears.
- Speed-up must not change the events tied to the end of each dialog.

### 2.2. Skip repeated introductions

Goal: allow skipping introduction sequences the player already knows.

Requirements:
- Add an option to skip the host presentation when minigames start.
- Allow skipping repeated main-menu animations where appropriate.
- Keep the original sequences available through a setting.
- Skipping an animation must not prevent its initialization events from running.

### 2.3. Fast board mode

Goal: speed up board games without changing their rules.

Requirements:
- Add an option enabled from the settings.
- Speed up the movement of Miis across spaces.
- Shorten the dice roll animations.
- Keep the rules, random results and win conditions unchanged.
- Apply the speed-up only to sequences that support it.

### 2.4. Resource preloading

Goal: reduce waiting time between scenes and minigames.

Requirements:
- Use the memory available on the PC to preload resources.
- Prepare early loading of `.rel` modules and their resources.
- Replace waits caused by original DVD access with local storage operations.
- Preload in the background when it does not interfere with execution.
- Set memory limits and release mechanisms.

---

## 3. AI and Mii system

### 3.1. Global CPU difficulty

Goal: allow a fixed difficulty for AI-controlled Miis.

Requirements:
- Add a global difficulty option.
- Allow levels such as `Expert` or `Master`, if the game logic has them.
- Apply the setting to the game modes that support it.
- Keep an option to use each mode's original difficulty.
- Do not change the difficulty of human players by accident.

### 3.2. Fix the Mii difficulty mapping

Goal: make sure AI statistics and levels are assigned correctly.

Requirements:
- Review the internal tables for CPU-controlled Miis.
- Check the correspondence between identifiers, statistics and difficulty levels.
- Fix the inconsistent assignments found.
- Check the legendary Miis specifically, such as Matt, Saburo and Elisa.
- Validate the resulting behavior in the affected game modes.

---

## 4. Interface, settings and saving

### 4.1. Built-in options menu

Goal: settings reachable from inside the game, without external tools.

Requirements:
- Add an options menu to the start screen.
- Keep the visual style and navigation of Wii Party.
- Allow configuring:
  - Output resolution.
  - Volume.
  - Graphics filters.
  - Control mapping.
  - Aspect ratio and refresh rate options, where applicable.
- Build the menu with the graphics and interface infrastructure of the native version.

### 4.2. Achievements

Goal: optional achievements specific to the PC version.

Requirements:
- Create an achievement event system inside the engine.
- Show pop-up notifications during play.
- Define achievements based on specific actions or challenges.
- Save progress locally in a `.json` file.
- Achievements must not change the game rules.
- Allow disabling the notifications.

### 4.3. Controller hot-swap

Goal: manage input devices while the game runs.

Requirements:
- Detect controllers being connected and disconnected.
- Allow switching between keyboard and Xbox, PlayStation, Switch or other supported controllers.
- Reassign devices to players 1 to 4 without restarting the game.
- Keep a common abstraction layer for the controller types.
- Handle a temporary loss of connection without blocking the game threads.

### 4.4. Profiles and backups

Goal: replace the single save with flexible save management.

Requirements:
- Implement independent user profiles on the PC.
- Allow multiple save slots.
- Create automatic backups of progress.
- Stay compatible with the original save data where feasible.
- Validate files before loading them.
- Handle damaged or incompatible files safely.

---

## 5. Native online mode

Design principle: the online mode is integrated into the original Wii Party interface. No external launcher and no separate modern menu will be added to reach it.

### 5.1. Integration into the original interface

Goal: add access to the online mode inside the minigame selection flow.

Requirements:
- Modify the layout of the minigame selection screen.
- Add a fourth button next to the three original buttons.
- Place the button in the third position if the layout allows it.
- Keep the proportions, animations and navigation of the original design.

### 5.2. Online button design

Requirements:
- Use a green base consistent with the Wii Party palette.
- Use a white icon that combines a Mii silhouette with an Internet connection symbol.
- Show the text `Online`.
- Keep the typography and visual style consistent with the rest of the interface.
- Prepare visual states for idle, selected, pressed and locked.

### 5.3. Connection menu

Goal: a connection screen that fits visually into the game.

Requirements:
- Run a native HLE scene in C++ when the online button is selected.
- Design the connection menu from scratch.
- Reuse or reproduce, where possible, the textures, sounds, transitions and animations of the original game.
- Keep navigation and presentation consistent with the rest of Wii Party.
- Separate the network logic from the presentation logic.

### 5.4. Network architecture

Goal: allow games between several machines through network infrastructure specific to the PC version.

Requirements:
- Define an abstraction layer for sockets and transport.
- Evaluate Dolphin's network code as a technical reference, without assuming direct compatibility.
- Choose a network model: server, host-client or peer-to-peer.
- Design a deterministic synchronization system for the relevant game states.
- Handle latency, packet loss, reconnection and players leaving.
- Separate synchronization from game logic, presentation and input.
- Define validation mechanisms to prevent diverging states between clients.

---

## 6. Implementation priorities

### Priority 1: blockers and foundations

- [ ] Connect and validate the input system.
- [ ] Allow getting past the Wii Remote strap screen.
- [ ] Replace the audio stubs progressively.
- [ ] Verify the stability of the main loop and the rendering.
- [ ] Set up basic regression tests.

### Priority 2: immediate usability

- [ ] Implement the options menu.
- [ ] Add controller hot-swap.
- [ ] Implement profiles and save slots.
- [ ] Add dialog speed-up and fast board mode.
- [ ] Add resource preloading.

### Priority 3: graphics

- [ ] Implement the dynamic aspect ratio.
- [ ] Separate rendering and logic to support high refresh rates.
- [x] Add HD texture support.
- [ ] Validate the 2D interface at different resolutions.

### Priority 4: extended features

- [ ] Implement the achievement system.
- [ ] Review and fix the CPU difficulty system.
- [ ] Design the online mode interface.
- [ ] Define and validate the network architecture.
- [ ] Implement the online mode in phases.

---

## 7. General quality criteria

- Keep the visual and audio identity of Wii Party.
- Do not change the original rules unless an option says so explicitly.
- Keep PC improvements separate from the recompiled game logic wherever possible.
- Document each relevant change.
- Add regression tests for the affected functions.
- Prefer stability over new features.
- Avoid unnecessary dependencies and keep build times reasonable.
- Log errors and debug state in a structured way.
