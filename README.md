# FFXLipSync

Voice-driven mouth movement for the Windows Steam release of **Final Fantasy X HD Remaster**. The mod uses the playing dialogue to drive jaw opening, interpolates it during rendering, and reduces chatter on models that switch between open and closed mouth textures. It applies to procedural dialogue. Hand-animated scenes keep their authored animation.

## Requirements

- The Windows Steam version of **Final Fantasy X HD Remaster**. The module was developed against the executable from Steam BuildID `25501027` and checks its expected instructions at startup.
- **FF10 Module Loader**, bundled with ffgriever's [Final Fantasy X and X-2 HD External File Loader](https://www.nexusmods.com/finalfantasyxx2hdremaster/mods/150). Use an External File Loader release that supports your game build. This is the only required mod.
- To build the DLL, install Visual Studio 2022 or its Build Tools with the **Desktop development with C++** workload and the Windows SDK.

The FPS mod, PS2 face restorations, and Japanese audio patches are optional. This was developed with [FFX-60fps 0.13](https://github.com/peternguyenst/FFX-60fps/releases/tag/0.13) and verified against Japanese voice events. Voice support for other languages may vary with their event names.

## Build and install

1. Run `build.cmd` from the repository. It builds the 32-bit module and runs the built-in tests.
2. Close FFX. Install the External File Loader in the game folder if it is missing.
3. Copy `build\FFXProceduralMouth.dll` into the game's `modules` directory beside `ff10-file-loader.dll`. Steam's **Manage → Browse local files** opens the game folder.
4. Launch **Final Fantasy X**. The module loader records its status in `hook.log`. `ffx-voice-jaw.log` lists speakers as the module first matches their procedural dialogue.

To uninstall, close the game and remove `modules\FFXProceduralMouth.dll`.

## Speaker matching

The mod samples individual FMOD voice channels. It recognizes the party's voice labels and matches other speakers, including NPCs, against the game's procedural mouth cues. If the cues are ambiguous or the voice cannot be matched, the module falls back to smoother procedural animation. It drives jaw opening from the audio envelope, so it approximates syllable timing with the model's existing mouth controls.

The build tests exercise speaker matching, fallback behavior, audio response, and interpolation at 30, 60, 120, and 240 FPS. The source is self-contained. It loads the game's own audio libraries at runtime.
