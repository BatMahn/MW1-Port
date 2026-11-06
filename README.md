# MW1-Port

A native port of **MechWarrior** (Dynamix / Activision, DOS, 1989) and its PC-98
remake **Battletech: Ubawareta Seihai** (1993) for macOS, Linux and Windows, built on SDL2.

The PC-98 edition is the default: better art, FM music, and it plays in English,
with the text taken from your DOS copy at start-up. The DOS edition is playable too.

**No game files are included.** The game's code is recompiled to C from *your own copy*
when you build, and the English text is read from *your own* DOS files when you play.

---

## 1. Folder setup

Everything lives in one folder: the program sits next to the game data.
After cloning and building, the repository folder *is* that folder:

```
MW1-Port/
├── mechwarrior            the program (mechwarrior.exe on Windows) — built by `make`
├── textmap/               ships with the source (offset maps and the English table, no Japanese text)
├── fonts/                 ships with the source (public-domain stand-in font)
│
├── DOS/                   ← you supply: the DOS MechWarrior files
├── PC98/                  ← you supply: the PC-98 disk images
├── MT32/                  ← optional: Roland MT-32 ROMs
```

Folder names are not case-sensitive (`DOS`, `dos`, `Dos` all work).

### DOS/ — DOS MechWarrior (1989)

Copy the files of the installed `MECHWAR` folder into `DOS/` (top level, no subfolders).

**For English text in the PC-98 edition only these two are needed:**

| File | Size (bytes) |
|---|---|
| `MW_MAIN.EXE` | 137,871 |
| `BTECH.EXE` | 174,533 |

The English text maps were built against the 1989 release with these sizes. A different
build of the DOS game would leave the text misaligned; the program warns at start-up if
the sizes differ.

**To play the DOS edition, all 126 files of the original install are needed:**

| Group | Files |
|---|---|
| Programs (6) | `MW.EXE`, `MW_MAIN.EXE`, `BTECH.EXE`, `MW_EGA.EXE`, `MW_TANDY.EXE`, `MW_CPICS.EXE` |
| Pictures (8) | `MW_PICS.BIN`, `MW_1PICS.BIN`, `MW_2PICS.BIN`, `MW_APICS.BIN`, `MW_CPICS.BIN`, `MW_FPICS.BIN`, `MW_GPICS.BIN`, `MW_TPICS.BIN` |
| Fonts (4) | `6X6.FNT`, `6X6B.FNT`, `8X8B.FNT`, `FOX88.FNT` |
| Music and sound (6) | `DEATH.MUS`, `LAUNCH.MUS`, `MECH.MUS`, `MECHBAR.MUS`, `VICTORY.MUS`, `BTECHSND.SND` |
| Palettes (4) | `ARCTIC.PAL`, `DESERT.PAL`, `TROPIC.PAL`, `DMGPAL.PAL` |
| Combat bitmaps (17) | `BAT.BMP`, `COCKPIT.BMP`, `DIGITS.BMP`, `DOTS.BMP`, `HUD_CYAN.BMP`, `HUD_NUMS.BMP`, `JEN.BMP`, `LOC.BMP`, `MAR.BMP`, `PHO.BMP`, `POINT.BMP`, `PROMPT.BMP`, `RIF.BMP`, `SHA.BMP`, `SM_MECHS.BMP`, `STRUTS.BMP`, `WAR.BMP` |
| Combat screens (5) | `HEAVY.SCR`, `LIGHT.SCR`, `MEDIUM.SCR`, `MAP.SCR`, `STATUS.SCR` |
| Mech and terrain models (12) | `BMAPCK.TBL`, `HAMPCK.TBL`, `JENPCK.TBL`, `LOCPCK.TBL`, `MARPCK.TBL`, `OTHPCK.TBL`, `PHAPCK.TBL`, `RIFPCK.TBL`, `SHAPCK.TBL`, `TERPCK.TBL`, `TERPCK.GI`, `SNARIO.DAT` |
| Terrain tiles (64) | `TILE0.GRD` … `TILE15.GRD`, `TILE0B.GRD` … `TILE15B.GRD`, `TILE0.WLD` … `TILE15.WLD`, `TILE0B.WLD` … `TILE15B.WLD` |

The program checks this list when the DOS edition starts and names any file that is
missing. Extra files (for example a release group's intro program) are ignored.
The DOS edition keeps its saved games in `DOS/`.

### PC98/ — Battletech: Ubawareta Seihai (PC-98, 1993)

Put **all three disk images** in `PC98/`:

| Disk | Typical image name | Contents |
|---|---|---|
| System disk | `BTECHSYS.FDI` | `IO98.SYS`, `MW.EXE`, `MW_MAIN.EXE`, `BTECH.EXE`, `F.COM`, combat data |
| Game disk | `BTECHGAM.FDI` | `GRP\`, `GDT\` (pictures, strategy data) |
| Demo/opening disk | `BTECHDEM.FDI` | `GRP\` (opening pictures), `BT_INST.EXE` |

- Formats: **FDI** (Anex86 / Neko Project, 1.2 MB 2HD with a 4 KB header) or raw **HDM**.
  Image names don't matter; every `.fdi`/`.hdm` in the folder is used.
  Other formats (D88, NFD) can be converted to HDM/FDI with common PC-98 disk tools.
- On first start the images are unpacked into `PC98/files/`. The game runs from there and
  keeps its saved games there.
- Alternatively, put the files themselves in `PC98/` (system disk files at the top level,
  plus the `GRP` and `GDT` folders from the other two disks merged together).

### MT32/ — Roland MT-32 ROMs (optional, DOS edition)

Two ROM files dumped from an MT-32 (or a CM-32L):

| File | Size |
|---|---|
| control ROM, e.g. `MT32_CONTROL.ROM` (v1.07 recommended) | 64 KB |
| PCM ROM, e.g. `MT32_PCM.ROM` | 512 KB |

Names don't matter: the ROMs are recognised by their contents (via Munt's ROM database).
With ROMs present the DOS edition uses the MT-32 (the game's own default); without them it
uses AdLib. The PC-98 edition has its own FM sound and doesn't use these.

### English for PC-98-only text (included)

Text the DOS game never had (the PC-98's longer opening, story scenes, news, extra menus) comes
from `textmap/pc98_english.tsv`, which ships with the port. It holds offsets, lengths and
English only, plus a checksum of the original bytes at each offset; rows that don't match your
copy of the game are skipped. No Japanese text is stored in it.

To use a different table instead (same columns; a sixth column may hold the Japanese or a
checksum), put it next to the program as `mechwarrior_pc98_translation.tsv`.

**About the translation.** The English in `pc98_english.tsv` was machine-translated from the
PC-98 game. All story elements, characters, names, places and setting are the property of the
original rights holders of *MechWarrior* (Dynamix / Activision, 1989) and *Battletech:
Ubawareta Seihai* (PC-98, 1993). BattleTech and MechWarrior are trademarks of their respective
owners.

---

## 2. Building

Needs a C/C++ compiler, SDL2 (with `sdl2-config`), `make` and `python3`.

**macOS**

    xcode-select --install      # compiler, make, python3
    brew install sdl2
    make

**Linux** (Debian/Ubuntu shown)

    sudo apt install build-essential libsdl2-dev python3
    make

**Windows** (MSYS2, "MINGW64" shell)

    pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2 make python
    make
    cp /mingw64/bin/SDL2.dll .   # SDL2.dll must sit next to mechwarrior.exe

Put the game data in place (section 1) **before** the first `make`: the build recompiles the
game's code from `DOS/` and `PC98/` into `generated/`. If you add data later, run
`make generate && make`. Without generated code the program still works, on its built-in
x86 interpreter (correct, but uses more CPU).

`make clean` removes the program and objects; `make distclean` also removes `generated/`.

---

## 3. Playing

    ./mechwarrior                 PC-98 edition (English if DOS/ is present)
    ./mechwarrior --skip-intro    same, fast-forwarding the five-minute opening
    ./mechwarrior --japanese      PC-98 edition with its original text
    ./mechwarrior --dos           DOS edition (MT-32 if ROMs are present, else AdLib)
    ./mechwarrior --dos A         DOS edition with AdLib (S speaker, T Tandy, R MT-32)

Alt+Enter toggles fullscreen; F12 fast-forwards while held. The mouse works as the game's
bus mouse / DOS mouse.

**PC-98 controls in combat** (keyboard): arrows steer and set throttle, Space fires,
**D** damage panel, **C** lance orders (with a lancemate deployed), Esc retreat.

Other options: `--data DIR` (look for the folders in DIR), `--mt32 DIR`, `--cycles N`
(emulated CPU speed, default 30000), `--help`.

---

## 4. How it works (short)

- **CPU**: the game's 8086 code is statically recompiled to C (`tools/recomp.py`), using the
  coverage traces in `trace/` plus recursive descent; anything not found runs on the
  reference interpreter (`src/cpu.c`). Every block is checksummed against your copy before
  use; `--verify` runs recompiled code in lockstep against the interpreter.
- **Hardware**: PIT, PICs, keyboard, mouse, EGA/Tandy video, PC speaker, Tandy sound,
  AdLib (Nuked OPL3), MT-32 (Munt), and for the PC-98: 640×400 planar graphics with GRCG,
  text VRAM and the character-generator window, the YM2203 FM chip (ymfm), bus mouse,
  INT 18h BIOS. DOS services are emulated (files, memory, EXEC, overlays).
- **Timing** runs on an emulated clock, so game speed doesn't depend on the host.
- **English**: first from your DOS copy, then from `textmap/pc98_english.tsv`. `textmap/*.map` pairs Japanese string offsets in the PC-98 programs with
  English string offsets in the DOS programs (offsets only, no text). At start-up the English
  is copied from your DOS files: in place where it fits, otherwise swapped in when drawn.
  `*.overrides` adjust individual strings; `pc98_art.overrides` repaints a few labels that are
  painted into the pictures. Names the combat engine checks against its mission data are
  kept Japanese in memory and switched to English only once its start-up check is done.

## 5. Known limitations

- The PC-98 title logo keeps its Japanese title (it is the game's artwork).
- A few story fragments have no entry in either the DOS game or `pc98_english.tsv` and stay
  Japanese.
- The Japanese font is a public-domain stand-in for the PC-98 character ROM, so Japanese
  and half-width characters look slightly different from real hardware.
- The PC-98 opening's hardware smooth-scroll isn't emulated.

## 6. Developer notes

`--dir PATH` runs a game folder directly (the current folder is kept). Testing aids:
`--headless MS` with `--key T:SCAN[:HOLD]`, `--mouse T:DX:DY:BTN`, `--shot T`,
`--anchor FILE` (times relative to when the game opens FILE), `--save-state FILE MS`,
`--load-state FILE`, `--poke16 T:ADDR:VAL`; `--log`, `--trace`, `--interp`, `--verify`,
`--no-idle`. Environment: `MW_WATCH=addr` (memory write watch), `MW_BREAK=addr[,addr]`,
`MW_TXTDBG=ms` (text draw log), `MW_ARTHASH=1`, `MW_MPULOG=1`.
Text maps are rebuilt with `tools/textalign.py`.

## 7. Third-party code

| Component | Licence | Path |
|---|---|---|
| Nuked OPL3 (AdLib) | LGPL-2.1-or-later | `third_party/opl3.*` |
| ymfm (YM2203) | BSD-3-Clause | `third_party/ymfm/` |
| libmt32emu 2.7.1 (Munt) | LGPL-2.1-or-later | `third_party/mt32emu/` |
| fonts/pc98font.bin (8x16rk, j90-16) | public domain | `fonts/` |

MechWarrior and BattleTech are trademarks of their respective owners. This project contains
no game code or data.
