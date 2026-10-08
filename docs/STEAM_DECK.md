# Muscle March on Steam Deck

Set it up once in Desktop Mode, then play from Game Mode with the Deck's controls.

## 1. Install (Desktop Mode)

1. Switch to Desktop Mode.
2. Download `MuscleMarch-x86_64.AppImage` from the [releases page](../../../releases) and move it
   to `~/Applications` (create the folder in Dolphin, the file manager, if needed).
3. Right-click it → **Properties → Permissions**, tick **Is executable**, and close.

## 2. Set it up (Desktop Mode)

1. Double-click the AppImage to open the launcher.
2. Drop your WAD on the window, or press **Choose…** (an SD card is under
   `/run/media/`). It must be `Muscle March (USA) (WiiWare).wad`; the launcher only reads it.
3. Give your Wii's common key: **Choose key file…** (e.g. `keys.bin` from your console's NAND
   backup), or type the 32 hexadecimal digits and press **Use key**. The launcher builds the
   game's files from your WAD in a few seconds.
4. Optional: check the **Features** page (Enhanced is recommended on the Deck: it's built for
   twin sticks) and the **Controls** page.
5. Press **Play** once to check it runs, then close it.

## 3. Add it to Steam

1. In the launcher's **Game** page, press **Add to Steam** (under **Steam**). It adds two entries
   to your library: **Muscle March**, which starts straight into the game, and
   **Muscle March Launcher**, for changing features or testing controllers later (it works with the
   touchscreen or trackpads).
2. Choose **Return to Gaming Mode**.

If you move the AppImage, press **Add to Steam** again (and remove the old entries). Without the
button, Steam's **Add a Non-Steam Game** works too: pick the AppImage and set its launch option to
`--play`.

## Playing

- Find **Muscle March** under **Library → Non-Steam**.
- Steam's default gamepad layout is right: with the Enhanced preset, the left stick moves the left
  arm and the right stick the right arm; wiggle both sticks to run. The on-screen dials in the side
  pillars show where to point them.
- Press the **⋯** button for Steam's menu, and **STEAM** to leave the game.
- Saves and settings are in `~/.local/share/Muscle March/`.

## Troubleshooting

- **"The game isn't set up yet"**: run the launcher once (step 2).
- **No sound or a black screen**: run the AppImage from Konsole in Desktop Mode and
  [open an issue](../../../issues) with what it prints.
