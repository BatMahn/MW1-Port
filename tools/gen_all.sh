#!/bin/sh
# Recompile one edition's programs from the player's own files into C.
# usage: tools/gen_all.sh dos|pc98 /path/to/game
set -e
ED="$1"; G="$2"; OUT=generated/$ED; mkdir -p $OUT; rm -f $OUT/rc_*.c
find_ci() { find "$G" -maxdepth 1 -iname "$1" | head -1; }
# PC-98 disk images instead of files: unpack them first (same as the program does at run time)
if [ "$ED" = pc98 ] && [ -z "$(find_ci IO98.SYS)" ]; then
  if [ -n "$(find "$G/files" -maxdepth 1 -iname IO98.SYS 2>/dev/null | head -1)" ]; then G="$G/files"
  else
    IMGS=$(find "$G" -maxdepth 1 \( -iname '*.fdi' -o -iname '*.hdm' \) | sort)
    [ -n "$IMGS" ] || { echo "no PC-98 files or disk images in $G"; exit 1; }
    mkdir -p $OUT/disk; python3 tools/fdi_extract.py $OUT/disk $IMGS; G=$OUT/disk
  fi
fi
LIST=""; DECL=""
if [ "$ED" = dos ]; then
  python3 tools/unexepack.py "$(find_ci BTECH.EXE)" $OUT/BTECH_U.EXE >/dev/null
  SPECS="MW.EXE:$(find_ci MW.EXE) MW_MAIN.EXE:$(find_ci MW_MAIN.EXE) BTECH.EXE:$OUT/BTECH_U.EXE MW_EGA.EXE:$(find_ci MW_EGA.EXE) MW_TANDY.EXE:$(find_ci MW_TANDY.EXE)"
else
  SPECS="MW.EXE:$(find_ci MW.EXE) MW_MAIN.EXE:$(find_ci MW_MAIN.EXE) BTECH.EXE:$(find_ci BTECH.EXE) F.COM:$(find_ci F.COM)"
fi
for spec in $SPECS; do
  name=${spec%%:*}; file=${spec#*:}
  id=${ED}_$(echo $name | tr 'A-Z.' 'a-z_')
  cov=trace/$ED/$name.cov; [ -f "$cov" ] || { : > $OUT/empty.cov; cov=$OUT/empty.cov; }
  python3 tools/recomp.py "$file" $id "$cov" $OUT/rc_$id.c
  DECL="${DECL}extern const RcBlock rc_${id}_blocks[]; extern const int rc_${id}_nblocks; extern u16 *rc_${id}_loadseg;\n"
  LIST="$LIST    { \"$name\", rc_${id}_blocks, &rc_${id}_nblocks, &rc_${id}_loadseg },\n"
done
printf "$DECL" > $OUT/images.inc
printf "static const RcImageDesc rc_images_$ED[] = {\n$LIST    { 0 }\n};\n" >> $OUT/images.inc
# top-level include: both editions (an edition not generated yet gets an empty list)
{ for e in dos pc98; do
    if [ -f generated/$e/images.inc ]; then echo "#include \"$e/images.inc\""; else echo "static const RcImageDesc rc_images_$e[] = { { 0 } };"; fi
  done; } > generated/images.inc
