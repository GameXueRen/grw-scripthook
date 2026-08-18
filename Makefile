CC = x86_64-w64-mingw32-gcc
CFLAGS = -O2 -Wall -Wextra -shared -static-libgcc
# Two up from src, so builds land beside GRW.exe.
GAMEDIR = ../..

.PHONY: all roulette fling tpgun clean

all: $(GAMEDIR)/dinput8.dll $(GAMEDIR)/test_plugin.asi

roulette: $(GAMEDIR)/tp_roulette.asi

fling: $(GAMEDIR)/hitfling.asi

# No import lib: plugins load from inside dinput8's
# DllMain, so a static import on it deadlocks the loader.
$(GAMEDIR)/hitfling.asi: hitfling.c scripthook.h
	$(CC) $(CFLAGS) -o $@ hitfling.c -lgdi32 -luser32

tpgun: $(GAMEDIR)/tpgun.asi

$(GAMEDIR)/tpgun.asi: tpgun.c scripthook.h
	$(CC) $(CFLAGS) -o $@ tpgun.c -lgdi32 -luser32

$(GAMEDIR)/tp_roulette.asi: tp_roulette.c scripthook.h libscripthook.a
	$(CC) $(CFLAGS) -o $@ tp_roulette.c \
		-L. -lscripthook -lgdi32 -luser32

libscripthook.a: $(GAMEDIR)/dinput8.dll

$(GAMEDIR)/dinput8.dll: loader.c scripthook_api.c scripthook_physics.c \
                        scripthook_health.c scripthook_state.c \
                        scripthook_entity.c scripthook_spawn.c \
                        scripthook_hit.c scripthook.h log.h
	$(CC) $(CFLAGS) -o $@ loader.c scripthook_api.c \
		scripthook_physics.c scripthook_health.c \
		scripthook_state.c scripthook_entity.c \
		scripthook_spawn.c scripthook_hit.c \
		-ldinput8 -ldxguid \
		-Wl,--out-implib,libscripthook.a

$(GAMEDIR)/test_plugin.asi: test_plugin.c
	$(CC) $(CFLAGS) -o $@ test_plugin.c -lws2_32 -lgdi32 -luser32

clean:
	rm -f $(GAMEDIR)/dinput8.dll $(GAMEDIR)/test_plugin.asi
	rm -f $(GAMEDIR)/scripthook.log $(GAMEDIR)/test_plugin.log
