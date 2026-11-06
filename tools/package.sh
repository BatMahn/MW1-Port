#!/bin/sh
# Package a built program for release: tools/package.sh PLATFORM PROGRAM [EXTRA_DIR]
# Produces dist/MW1-Port-PLATFORM.zip with the program, its data files (textmap/, fonts/),
# the README and licences, and empty DOS/ PC98/ MT32/ folders to put the game data in.
set -e
PLAT="$1"; PROG="$2"; EXTRA="$3"
OUT=dist/MW1-Port
rm -rf dist; mkdir -p $OUT/licenses
cp -R "$PROG" $OUT/
[ -n "$EXTRA" ] && [ -d "$EXTRA" ] && cp -R "$EXTRA" $OUT/
# Windows: the DLLs go next to the .exe
if [ "$EXTRA" = dlls ]; then mv $OUT/dlls/* $OUT/ 2>/dev/null || true; rmdir $OUT/dlls 2>/dev/null || true; fi
cp README.md $OUT/
# a macOS .app carries textmap/ and fonts/ inside it; other builds keep them next to the program
case "$PROG" in *.app) ;; *) cp -R textmap fonts $OUT/ ;; esac
cp third_party/ymfm/LICENSE $OUT/licenses/ymfm-LICENSE.txt
cp third_party/mt32emu/COPYING.LESSER.txt $OUT/licenses/mt32emu-COPYING.LESSER.txt
sed -n '1,30p' third_party/opl3.c > $OUT/licenses/NukedOPL3-header.txt
[ -f LICENSE ] && cp LICENSE $OUT/licenses/MW1-Port-LICENSE.txt
for d in DOS PC98 MT32; do mkdir -p $OUT/$d; done
printf 'Put the files of your DOS MechWarrior install (the MECHWAR folder) here.\nSee README.md, section 1.\n' > $OUT/DOS/PUT-FILES-HERE.txt
printf 'Put the three PC-98 disk images (.fdi or .hdm) here.\nSee README.md, section 1.\n' > $OUT/PC98/PUT-FILES-HERE.txt
printf 'Optional: Roland MT-32 control and PCM ROMs.\nSee README.md, section 1.\n' > $OUT/MT32/PUT-FILES-HERE.txt
(cd dist && zip -qr "MW1-Port-$PLAT.zip" MW1-Port)
echo "dist/MW1-Port-$PLAT.zip"
