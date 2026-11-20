# DOS edition: port against the original

The port runs the original programs (MW.EXE, MW_MAIN.EXE, BTECH.EXE) as recompiled code on an
emulated PC, so the game logic is the original's. This audit checks the two places the port can
still differ from a real machine: running that code, and every BIOS, DOS and hardware service the
game uses. The inventory comes from a session through the intro, city screens, house, contract,
launch and combat (`MW_INVENTORY=1` prints it at exit).

| # | Area | What the game uses | How it was checked | Status |
|---|---|---|---|---|
| 1 | CPU | 8088 and 80186 instructions | SingleStepTests 8088 suite (about 2.5 million cases) and the 80286 real-mode suite for the 186 additions | DONE. DAA / DAS / AAA / AAS / AAM 0 fixed. Remaining differences are 8088-only quirks no compiler emits (shift counts of 32 or more, `push sp`, IDIV with REP, word access across offset FFFFh) |
| 2 | Recompiled code | all three programs | lockstep against the interpreter (`--verify`) through the whole session | DONE. 31 million blocks, no mismatch; self-modifying blocks fall back to the interpreter |
| 3 | Video adapter detection | INT 10h AH=1Bh, AH=12h BL=10h, 0:0487h | launcher code at MW.EXE 0CC6h: result 2 (EGA) or 3 (VGA) both start MW_MAIN with `E` | DONE. Port answers EGA; a VGA machine gets the same mode |
| 4 | Video mode | INT 10h AH=00h mode 0Dh, AH=0Fh, AH=10h AL=00/01/02, AH=06h | against the EGA BIOS | DONE |
| 5 | EGA planar writes and reads | write modes 0-2, set / reset, rotate, AND / OR / XOR, bit mask, map mask, read modes 0-1, latches | against the EGA register descriptions | DONE (write mode 3 is VGA-only, as on EGA) |
| 6 | Page flipping and retrace | CRTC 0Ch / 0Dh, 3DAh | EGA timing | DONE. 60 Hz as on EGA (a VGA card runs this mode at 70 Hz) |
| 7 | Timer | PIT channel 0: 145.65 Hz in the city, 236.7 Hz in combat; counter latch reads at start-up | combat's rate traced to its timer routine (65535 / 13, the game asking for 13 times the 18.2 Hz tick) | DONE |
| 8 | Combat speed | combat logic | the same 3-second turn at 1,500, 3,000 and 12,000 instructions/ms | DONE. Turn is equal at 3,000 and 12,000: combat runs off the game's timer. Default 3,000 is right; below that the CPU is too slow |
| 9 | PC speaker | PIT channel 2 counts every 4.2 ms, port 61h gate and data | traced writes in combat | FIXED. A new count now changes the pitch at once (it waited for the next gate write). Channel 2 starts in the BIOS power-on state (square wave, 16-bit counts) |
| 10 | Port 61h reads | refresh bit 4, timer bit 5 | against AT timing | FIXED. Bit 4 toggles every 15.085 us of game time (it toggled on every read, so delay loops ran short) |
| 11 | Interrupt controller | non-specific EOI | | DONE. Specific EOI added for completeness |
| 12 | Keyboard | INT 16h AH=00h / 01h, the game's own INT 9 hook reading 60h | against the AT BIOS | FIXED. Alt, Ctrl and Shift with function keys, and Alt with letters / digits, now give the BIOS's codes. Held keys auto-repeat as on the real keyboard |
| 13 | Mouse | INT 33h functions 0, 6, 0Bh | against MOUSE.COM | FIXED. Function 0 now centres the cursor and clears the counters |
| 14 | DOS | INT 21h 25h, 30h, 35h, 3Dh-3Fh, 42h, 43h, 44h, 48h-4Dh | against DOS 3.3 | DONE. IOCTL 4400h now reports AUX / PRN as devices (no effect on the game) |
| 15 | AdLib | 388h / 389h writes, status reads with timer detection | | DONE |
| 16 | MPU-401 / MT-32 | 330h / 331h, UART mode | MIDI stream checked byte for byte (checksums, timing) | DONE. Doubled parts in the music's channel setup switched off (warble); the drum part keeps its channel |
| 17 | Joystick | 201h | | DONE. Reads as no joystick, as on a machine without one |
| 18 | Sound recording | `--wav` | | FIXED. The recording now includes the speaker and Tandy sound |
