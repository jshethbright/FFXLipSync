# FFXLipSync

Dialogue sync improvements for the Windows Steam release of **Final Fantasy X HD Remaster**. FFXLipSync includes voice-driven procedural mouth movement, a shorter script wait before voice playback, and an optional lower-latency FMOD mixer.

## Requirements

- The Windows Steam version of **Final Fantasy X HD Remaster**. The modules were developed against the executable from Steam BuildID `25501027` and check the instructions they patch at startup.
- **FF10 Module Loader**, bundled with ffgriever's [Final Fantasy X and X-2 HD External File Loader](https://www.nexusmods.com/finalfantasyxx2hdremaster/mods/150). Use an External File Loader release that supports your game build. This is the only required mod.
- To build the DLLs, install Visual Studio 2022 or its Build Tools with the **Desktop development with C++** workload and the Windows SDK.

The FPS mod, PS2 face restorations, and Japanese audio patches are optional. This was developed with [FFX-60fps 0.13](https://github.com/peternguyenst/FFX-60fps/releases/tag/0.13) and verified against Japanese voice events. Voice support for other languages may vary with their event names.

## Build and install

1. Run `build.cmd` to build the three 32-bit DLLs and run the lip-sync tests.
2. Close FFX. Install the External File Loader in the game folder if needed.
3. Copy the DLLs you want from `build` into the game's `modules` directory beside `ff10-file-loader.dll`. In Steam, choose **Browse local files** from the game's **Manage** menu.
4. Launch **Final Fantasy X**. Check `hook.log` for module status. The mouth module logs matched speakers in `ffx-voice-jaw.log`, and the mixer module records the active buffer in `ffx-fmod-mixer.log`. Set `Logging=0` under `[Diagnostics]` in `modules\config\FFXLipSync.ini` to turn off these logs on the next launch.

| DLL | What it does |
| --- | --- |
| `FFXProceduralMouth.dll` | Drives procedural mouth movement from dialogue audio. Hand-animated scenes keep their authored animation. |
| `FFXVoiceTiming.dll` | Reduces the five-tick voice wait to one tick. The original wait handler still checks the game's pause conditions. |
| `FFXFMODMixer.dll` | Sets FMOD's software mixer to four 512-sample blocks at initialization. This affects dialogue, music, and effects. |

Each DLL can be installed independently. To uninstall any of them, close FFX and remove its DLL from the `modules` directory. If you hear clicks or dropouts, removing `FFXFMODMixer.dll` restores the game's default mixer setting on the next launch.

The timing and mixer changes were verified together on one Japanese-voiced cutscene with English text and the FPS mod set to 120 FPS. Smaller mixer blocks give the audio engine less time to fill each block. Audio stability under heavier loads can vary by system.

## Speaker matching

The mod samples individual FMOD voice channels. It recognizes the party's voice labels and matches other speakers, including NPCs, against the game's procedural mouth cues. If the cues are ambiguous or the voice cannot be matched, the module falls back to smoother procedural animation. It drives jaw opening from the audio envelope, so it approximates syllable timing with the model's existing mouth controls.

The build tests exercise speaker matching, fallback behavior, audio response, and interpolation at 30, 60, 120, and 240 FPS. The source is self-contained. It loads the game's own audio libraries at runtime.
