# KOTOR2VR

Native-engine VR for **Star Wars: Knights of the Old Republic II**, with stereo rendering, head tracking, first/third-person views and a separate VR HUD.

> **0.1.1 — Early preview, work in progress.** This update fixes movie playback in the headset, the supported decoder crash when skipping the prologue, character-dependent first-person height, head clipping, graphics-reset recovery and UI placement. Headset testing covered the prologue and parts of Peragus; a full playthrough and installation testing across supported setups remain incomplete.

## Install and play

1. Install the supported Windows Steam version of KOTOR II, start it normally once, then close it.
2. Connect your headset and activate Quest Link with Meta as the active OpenXR runtime.
3. Download the release ZIP and extract its **KOTOR2VR** folder into your Steam KOTOR II folder, beside `swkotor2.exe`.
4. Double-click **KOTOR2VR/Start-VR.cmd**, then load a save or start a game.

No installer or developer tools are required to use the binary ZIP. Do not copy individual DLLs into the game root. KOTOR2VR injects its module at runtime and does not patch the game executable on disk. To remove this package, close the game and delete the KOTOR2VR folder. Diagnostic logs can remain under `%LOCALAPPDATA%/Kotor2VR`.

The starter uses Steam's registered installation. A copied game folder is not yet a supported independent test installation. Existing third-party mods are not removed by this package.

## Requirements and compatibility

- 64-bit Windows, Steam, and the Windows Steam/Aspyr executable below.
- An active OpenXR headset/runtime. Tested development hardware: Quest 3 over USB Quest Link at 72 Hz, NVIDIA RTX 5070 Ti.
- Other GPUs, headsets, runtimes and game distributions are not yet validated. No universal performance claim is made.
- Movie capture and the prologue-skip fix require the original `binkw32.dll` (SHA-256 `D963112EA8545C8AAA6BD48A9D2D229605806AE1BBC03F90B0106DB3034466ED`). Replacement decoders are not supported or bundled.
- Game executable SHA-256: `6A522E71631DCEE93467BD2010F3B23D9145326E1E2E89305F13AB104DBBFFEF` (recorded Steam build 817494). The launcher refuses other executables.

## Controls

| Action | Keyboard | Xbox-style controller |
|---|---|---|
| Start stream manually if needed | F7 | View + Menu |
| First/third person | F10 | View + Y |
| Recenter | F11 | View + A, or hold both sticks for 650 ms |
| Show/hide VR HUD | F8 | View + D-pad down |
| Theater/world | Ctrl + F10 | View + X |

Use normal gamepad or mouse/keyboard controls for gameplay. Menus and movies use a fixed theater screen; recenter places it in your current viewing direction. The gameplay HUD follows your gaze while staying level when you tilt your head sideways. Dialogue cameras retain their game-authored placement. First-person hides verified human head attachments and T3 head/eye meshes while retaining the body and weapons. Other character models, especially combined head/body meshes, need further coverage. Post-combat movement lock, longer play sessions and minigames remain test items.

## Build it yourself

The release ZIP contains executable code, even though it has no installer. You can inspect and build the source yourself using [the build instructions](docs/BUILD.md). SHA-256 checksums detect differences between downloads; they are not a guarantee that software is harmless. We do not currently claim byte-for-byte reproducible builds.

See [third-party notices](THIRD_PARTY_NOTICES.md).

## DLSS 5 and performance

Performance optimizations are being developed in parallel. An experimental DLSS 5 integration is already working and delivers a substantial visual improvement. The plan is to bring optional DLSS 5 support and setup instructions into this repository once it is ready for wider testing.

KOTOR2VR is an unofficial community project and includes no game assets. Original project code is MIT licensed; third-party components retain their own licenses.
