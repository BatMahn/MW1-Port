# PC-98 edition: port against the original

As for the DOS edition (AUDIT_DOS.md), the port runs the original programs (MW.EXE, F.COM,
MW_MAIN.EXE, BTECH.EXE) as recompiled code on an emulated PC-9801, so the game logic is the
original's. This audit checks running that code, every BIOS / DOS / hardware service the game
uses, and the English layer. The inventory comes from a session from boot through the opening,
city, house, contract, launch and combat (`MW_INVENTORY=1`).

| # | Area | What the game uses | How it was checked | Status |
|---|---|---|---|---|
| 1 | CPU | 8086 and 186 instructions | SingleStepTests 8088 and 80286 real-mode suites (see AUDIT_DOS.md) | DONE |
| 2 | Recompiled code | all four programs | lockstep against the interpreter (`--verify`), boot to combat | DONE. 361 million blocks, no mismatch |
| 3 | Clock family | system port 42h bit 5, BIOS 0:0501h | MW_MAIN picks its timer divisor from port 42h | FIXED earlier (the game ran 23 % fast); both now say 10 MHz family, PIT 2.4576 MHz |
| 4 | Timer | 37.5 Hz in the strategy screens, 487.5 Hz in combat | traced divisors | DONE |
| 5 | Keyboard | INT 18h AH=00h-04h (buffer, shift state, key matrix for combat) | against the PC-98 BIOS | DONE |
| 6 | CRT / text BIOS | INT 18h AH=0Ah-0Fh, 12h, 13h, 1Bh, 40h-42h, 4Ah | against the PC-98 BIOS | FIXED: AH=0Bh now returns the mode set with AH=0Ah. Display areas (AH=0Eh / 0Fh) recorded but not applied: read as byte offsets they would hide narration the opening draws on rows 18-24, so the layout the port already showed is kept (`MW_TAREAS=1` to experiment) |
| 7 | Font reads | INT 18h AH=14h, CG window ports A1h / A3h / A5h / A9h | | DONE (stand-in font, see README) |
| 8 | Graphics | 640x400 4-plane, GRCG TDW / RMW / TCR, analog palette, GDC start / stop / scroll | against the GRCG and GDC descriptions | DONE. No EGC use found |
| 9 | I/O ports | every port touched in the session | logged unhandled ports | DONE. Only 7Fh, the high byte of word writes to the GRCG tile register (nothing is attached there) |
| 10 | Sound | YM2203 at 3.9936 MHz, its timers driving F.COM | | DONE |
| 11 | Mouse | bus mouse ports and IRQ 13 | | DONE |
| 12 | DOS | INT 21h 02h, 09h, 19h, 25h, 30h, 31h (F.COM stays resident), 35h, 3Dh-3Fh, 42h-44h, 48h-4Dh | against DOS 3.3 | DONE |
| 13 | Combat speed | combat logic and frame rate | turning at several CPU speeds | DONE. Default 4,000 instructions/ms; Ctrl+F9 / F10 adjust |
| 14 | English text | every text row on screen | `MW_JPSCAN=1` reports any row still showing Japanese; checked against `--japanese` as a control | FIXED: the opening's first narration line was still in Japanese (added to textmap/pc98_english.tsv). Boot to combat now shows no Japanese text rows |
