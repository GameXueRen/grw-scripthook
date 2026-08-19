CC = x86_64-w64-mingw32-gcc
CFLAGS = -O2 -Wall -Wextra -shared -static-libgcc
# Two up from src, so builds land beside GRW.exe.
GAMEDIR = ../..

.PHONY: all roulette fling tpgun spawner crazycars freecam fov fps clean

all: $(GAMEDIR)/dinput8.dll $(GAMEDIR)/test_plugin.asi

roulette: $(GAMEDIR)/tp_roulette.asi

fling: $(GAMEDIR)/hitfling.asi

# No import lib: plugins load from inside dinput8's
# DllMain, so a static import on it deadlocks the loader.
$(GAMEDIR)/hitfling.asi: hitfling.c scripthook.h
	$(CC) $(CFLAGS) -o $@ hitfling.c -lgdi32 -luser32

freecam: $(GAMEDIR)/freecam.asi

$(GAMEDIR)/freecam.asi: freecam.c scripthook.h
	$(CC) $(CFLAGS) -o $@ freecam.c -lgdi32 -luser32

fps: $(GAMEDIR)/firstperson.asi

$(GAMEDIR)/firstperson.asi: firstperson.c scripthook.h
	$(CC) $(CFLAGS) -o $@ firstperson.c -lgdi32 -luser32

fov: $(GAMEDIR)/fov_changer.asi

$(GAMEDIR)/fov_changer.asi: fov_changer.c scripthook.h
	$(CC) $(CFLAGS) -o $@ fov_changer.c -lgdi32 -luser32

spawner: $(GAMEDIR)/spawner.asi

$(GAMEDIR)/spawner.asi: spawner.c scripthook.h
	$(CC) $(CFLAGS) -o $@ spawner.c -lgdi32 -luser32

crazycars: $(GAMEDIR)/CrazyCars.asi

$(GAMEDIR)/CrazyCars.asi: crazycars.c scripthook.h
	$(CC) $(CFLAGS) -o $@ crazycars.c -lgdi32 -luser32

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
                        scripthook_hit.c scripthook_camera.c \
                        scripthook_head.c \
                        scripthook_hud.c scripthook_menu.c guard.c scripthook.h log.h
	$(CC) $(CFLAGS) -o $@ loader.c scripthook_api.c \
		scripthook_physics.c scripthook_health.c \
		scripthook_state.c scripthook_entity.c \
		scripthook_spawn.c scripthook_hit.c \
		scripthook_camera.c scripthook_head.c \
		scripthook_hud.c scripthook_menu.c guard.c \
		-ldinput8 -ldxguid -lgdi32 -luser32 \
		-Wl,--out-implib,libscripthook.a

$(GAMEDIR)/test_plugin.asi: test_plugin.c
	$(CC) $(CFLAGS) -o $@ test_plugin.c -lws2_32 -lgdi32 -luser32

clean:
	rm -f $(GAMEDIR)/dinput8.dll $(GAMEDIR)/test_plugin.asi
	rm -f $(GAMEDIR)/scripthook.log $(GAMEDIR)/test_plugin.log
