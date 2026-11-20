# Changelog

## v0.6

PC-98 edition checked against a real PC-9801 (docs/AUDIT_PC98.md).

- English: the first narration line of the opening was still in Japanese; it is now translated.
- PC-98 BIOS: "sense CRT mode" (INT 18h AH=0Bh) now returns the mode the game set.
- New `MW_JPSCAN=1` check reports any text row still showing Japanese; boot to combat now shows none.
- Recompiled code lockstep-checked against the interpreter from boot to combat: no mismatches.

## v0.5

DOS edition checked against a real PC (docs/AUDIT_DOS.md).

- PC speaker: pitch changes in combat sound effects now take effect immediately.
- PC speaker: the timer channel starts in the state the BIOS leaves it at power-on.
- Port 61h refresh bit now toggles in game time, so delay loops that count it run the right length.
- Keyboard: Alt, Ctrl and Shift with function keys, and Alt with letters or digits, give the BIOS's key codes.
- Mouse reset (INT 33h function 0) centres the cursor and clears its counters.
- `--wav` recordings now include the PC speaker and Tandy sound.
- AUX and PRN are reported as devices to the DOS device-info call.

## v0.4

- MT-32: the bar music no longer loses its drums. When removing doubled parts, the drum part keeps its channel and instrument parts on the drum channel are switched off.

## v0.3

- MT-32: fixed the warbling music. The game's music doubles some instrument parts on the same channel; the doubled parts are now switched off before the setup reaches the MT-32.
- Mech bay (PC-98): Japanese text no longer overlaps the left-arm motor when it is selected in Fix Part; the motor row now follows the original layout.

## v0.2

- macOS: the app bundle starts again ("Failed loading SDL3 library"). Builds use SDL 3 directly; SDL 2 still works with `make SDL=2`.
- Game speed: the PC-98 opening, strategy screens and music had run 23% too fast; the game now picks the correct timer setting.
- Game speed: combat runs near original-hardware speed (DOS 3000, PC-98 4000 instructions/ms); Ctrl+F9 / Ctrl+F10 adjust it while playing, shown in the window title.
- CPU: decimal-adjust instructions (DAA, DAS, AAA, AAS, AAM) corrected against real 8088 test data.
- Keyboard: held keys auto-repeat like a real keyboard; the window takes keyboard focus when started from Terminal on macOS.
- Audio: emulation is paced by the sound card's clock, so audio no longer drifts or drops out.
- `MW_KEYLOG=1` logs key events for troubleshooting.

## v0.1

First release.

- Native port of MechWarrior (DOS, 1989) and Battletech: Ubawareta Seihai (PC-98, 1993): the original programs recompiled to native code, with an interpreter fallback.
- Emulated PC (EGA, AdLib, Tandy, PC speaker, MT-32 via Munt, mouse) and PC-9801 (graphics, GRCG, text, FM sound, bus mouse).
- PC-98 edition playable in English, using text from your DOS copy plus a translation table.
- Builds for macOS (Apple Silicon and Intel app bundles), Windows and Linux; game files are not included.
