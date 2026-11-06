# MW1-Port: native MechWarrior (DOS, 1989) / Battletech: Ubawareta Seihai (PC-98, 1993)
#
#   make            build ./mechwarrior (mechwarrior.exe on Windows); if DOS/ or PC98/ hold
#                   game data and the recompiled code isn't there yet, it is generated first
#   make generate   (re)generate the recompiled game code from DOS/ and PC98/
#   make clean      remove the program and objects (generated code is kept)
#   make distclean  also remove the generated code
#
# Needs: a C/C++ compiler, SDL2 (sdl2-config on the PATH), python3.

UNAME := $(shell uname -s 2>/dev/null)
ifneq (,$(findstring MINGW,$(UNAME))$(findstring MSYS,$(UNAME)))
  EXE := mechwarrior.exe
  PLATFORM_LIBS := -static-libgcc -static-libstdc++
else
  EXE := mechwarrior
  PLATFORM_LIBS :=
endif

CC  ?= cc
CXX ?= c++
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs)
CFLAGS   ?= -O2 -g
CXXFLAGS ?= -O2 -g
CFLAGS   += -Wall -Wno-unused-function -Isrc -Igenerated $(SDL_CFLAGS)
CXXFLAGS += -std=c++14 -Isrc $(SDL_CFLAGS)
MT32_DEFS := -DMT32EMU_WITH_INTERNAL_RESAMPLER=1

DATA_DOS  := $(firstword $(wildcard DOS dos Dos))
DATA_PC98 := $(firstword $(wildcard PC98 pc98 Pc98))

C_SRC := src/english.c src/recomp.c src/pc98.c src/cpu.c src/dos.c src/hw.c src/video.c \
         src/audio.c src/trace.c src/fdi.c src/main.c third_party/opl3.c
GEN_SRC = $(wildcard generated/dos/rc_*.c generated/pc98/rc_*.c)
CXX_SRC := src/opn.cpp src/mt32.cpp \
         third_party/ymfm/ymfm_opn.cpp third_party/ymfm/ymfm_ssg.cpp third_party/ymfm/ymfm_adpcm.cpp
MT32_SRC := $(wildcard third_party/mt32emu/*.cpp) third_party/mt32emu/sha1/sha1.cpp \
         third_party/mt32emu/srchelper/InternalResampler.cpp \
         $(wildcard third_party/mt32emu/srchelper/srctools/src/*.cpp)
MT32_SRC := $(filter-out third_party/mt32emu/c_interface/%,$(MT32_SRC))

OBJDIR := build
C_OBJ    = $(patsubst %.c,$(OBJDIR)/%.o,$(C_SRC) $(GEN_SRC))
CXX_OBJ  := $(patsubst %.cpp,$(OBJDIR)/%.o,$(CXX_SRC))
MT32_OBJ := $(patsubst %.cpp,$(OBJDIR)/%.o,$(MT32_SRC))

.PHONY: all generate distclean clean
all: generated/images.inc
	@$(MAKE) --no-print-directory $(EXE)

$(EXE): $(C_OBJ) $(CXX_OBJ) $(MT32_OBJ)
	$(CXX) -o $@ $^ $(SDL_LIBS) $(PLATFORM_LIBS) -lm

$(OBJDIR)/%.o: %.c src/*.h generated/images.inc
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<
$(OBJDIR)/third_party/mt32emu/%.o: third_party/mt32emu/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(MT32_DEFS) -c -o $@ $<
$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(MT32_DEFS) -c -o $@ $<

# The recompiled game code is made from your own copy of the game and is never committed.
# Without it the program still runs, on the built-in CPU interpreter (more host CPU).
generated/images.inc:
	@if [ -n "$(DATA_DOS)$(DATA_PC98)" ]; then $(MAKE) --no-print-directory generate; \
	else mkdir -p generated; \
	  printf 'static const RcImageDesc rc_images_dos[] = { { 0 } };\nstatic const RcImageDesc rc_images_pc98[] = { { 0 } };\n' > $@; \
	  echo "note: no DOS/ or PC98/ data yet - building without recompiled code (run 'make generate' later)"; fi

generate:
	@rm -f generated/images.inc
	@if [ -n "$(DATA_DOS)" ]; then sh tools/gen_all.sh dos "$(DATA_DOS)"; fi
	@if [ -n "$(DATA_PC98)" ]; then sh tools/gen_all.sh pc98 "$(DATA_PC98)"; fi
	@[ -f generated/images.inc ] || { echo "nothing to generate: put the game data in DOS/ and/or PC98/ (see README)"; exit 1; }
	@rm -rf $(OBJDIR)/generated $(OBJDIR)/src/recomp.o

clean:
	rm -rf $(OBJDIR) $(EXE)

distclean: clean
	rm -rf generated
