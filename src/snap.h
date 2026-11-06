/* Save states for testing: each module writes/reads its own state. */
#ifndef SNAP_H
#define SNAP_H
#include <stdio.h>
#define S(v) do { if (save) fwrite(&(v), sizeof(v), 1, f); else if (fread(&(v), sizeof(v), 1, f) != 1) fprintf(stderr, "snapshot short read: %s\n", #v); } while (0)
void hw_snap(FILE *f, int save);
void pc98_snap(FILE *f, int save);
void dos_snap(FILE *f, int save);
void rc_snap(FILE *f, int save);
void audio_snap(FILE *f, int save);
void opn_snap(FILE *f, int save);
#endif
