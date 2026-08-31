CC = x86_64-w64-mingw32-gcc
CFLAGS = -O2 -Wall -Wextra -shared -static-libgcc
# make QUIET=1: drop the four warning families every file
# trips (GetProcAddress casts, unused statics, strncpy), so
# only errors and new warnings reach the terminal.
ifdef QUIET
CFLAGS += -Wno-cast-function-type -Wno-unused-function \
          -Wno-strict-aliasing -Wno-stringop-truncation
endif
# Two up from src, so builds land beside GRW.exe.
GAMEDIR = ../..

# Plugins land in scripts/<name>/<name>.asi, one folder per
# plugin, which is what the loader scans for. Logs go into
# <gamedir>/logs at runtime.

.PHONY: all roulette fling spawner crazycars freecam fov fps \
        chaos sample docs clean

sample: $(GAMEDIR)/scripts/ui_sample/ui_sample.asi

$(GAMEDIR)/scripts/ui_sample/ui_sample.asi: ui_sample.c scripthook.h libscripthook.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ ui_sample.c -L. -lscripthook -luser32

docs:
	doxygen Doxyfile

all: $(GAMEDIR)/dinput8.dll $(GAMEDIR)/scripts/test_plugin/test_plugin.asi

roulette: $(GAMEDIR)/scripts/tp_roulette/tp_roulette.asi

fling: $(GAMEDIR)/scripts/hitfling/hitfling.asi

# These bind with GetProcAddress. Linking libscripthook.a
# instead works too, since the loader loads plugins from a
# thread rather than from DllMain.
$(GAMEDIR)/scripts/hitfling/hitfling.asi: hitfling.c scripthook.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ hitfling.c -lgdi32 -luser32

freecam: $(GAMEDIR)/scripts/freecam/freecam.asi

$(GAMEDIR)/scripts/freecam/freecam.asi: freecam.c scripthook.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ freecam.c -lgdi32 -luser32

fps: $(GAMEDIR)/scripts/firstperson/firstperson.asi

$(GAMEDIR)/scripts/firstperson/firstperson.asi: firstperson.c scripthook.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ firstperson.c -lgdi32 -luser32

chaos: $(GAMEDIR)/scripts/chaos/chaos.asi

$(GAMEDIR)/scripts/chaos/chaos.asi: chaos.c scripthook.h libscripthook.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ chaos.c \
		-L. -lscripthook -lgdi32 -luser32 -lwinmm

fov: $(GAMEDIR)/scripts/fov_changer/fov_changer.asi

$(GAMEDIR)/scripts/fov_changer/fov_changer.asi: fov_changer.c scripthook.h libscripthook.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ fov_changer.c \
		-L. -lscripthook -lgdi32 -luser32

spawner: $(GAMEDIR)/scripts/spawner/spawner.asi

$(GAMEDIR)/scripts/spawner/spawner.asi: spawner.c scripthook.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ spawner.c -lgdi32 -luser32

crazycars: $(GAMEDIR)/scripts/CrazyCars/CrazyCars.asi

$(GAMEDIR)/scripts/CrazyCars/CrazyCars.asi: crazycars.c scripthook.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ crazycars.c -lgdi32 -luser32

tpgun: $(GAMEDIR)/scripts/tpgun/tpgun.asi

$(GAMEDIR)/scripts/tpgun/tpgun.asi: tpgun.c scripthook.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ tpgun.c -lgdi32 -luser32

$(GAMEDIR)/scripts/tp_roulette/tp_roulette.asi: tp_roulette.c scripthook.h libscripthook.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ tp_roulette.c \
		-L. -lscripthook -lgdi32 -luser32

libscripthook.a: $(GAMEDIR)/dinput8.dll

$(GAMEDIR)/dinput8.dll: loader.c scripthook_api.c scripthook_config.c \
                        scripthook_physics.c \
                        scripthook_health.c scripthook_state.c \
                        scripthook_entity.c scripthook_spawn.c \
                        scripthook_npc.c scripthook_domino.c \
                        scripthook_hit.c scripthook_camera.c \
                        scripthook_head.c scripthook_fov.c \
                        scripthook_blur.c \
                        scripthook_stat.c scripthook_resource.c \
                        scripthook_stealth.c scripthook_ammo.c \
                        scripthook_weather.c scripthook_crash.c \
                        scripthook_input.c scripthook_havok.c \
                        scripthook_reflect.c scripthook_ui.c \
                        scripthook_scene.c scripthook_uiprop.c \
                        scripthook_uiinput.c scripthook_dinput.c \
                        scripthook_hud.c scripthook_menu.c guard.c scripthook.h log.h
	$(CC) $(CFLAGS) -o $@ loader.c scripthook_api.c \
		scripthook_config.c \
		scripthook_physics.c scripthook_health.c \
		scripthook_state.c scripthook_entity.c \
		scripthook_spawn.c scripthook_npc.c scripthook_domino.c scripthook_hit.c \
		scripthook_camera.c scripthook_head.c \
		scripthook_fov.c scripthook_blur.c \
		scripthook_stat.c scripthook_resource.c \
		scripthook_stealth.c scripthook_ammo.c \
		scripthook_weather.c scripthook_crash.c \
		scripthook_input.c scripthook_havok.c \
		scripthook_reflect.c scripthook_ui.c \
		scripthook_scene.c scripthook_uiprop.c \
		scripthook_uiinput.c scripthook_dinput.c \
		scripthook_hud.c scripthook_menu.c guard.c \
		-ldinput8 -ldxguid -lgdi32 -luser32 \
		-Wl,--out-implib,libscripthook.a
	@if x86_64-w64-mingw32-objdump -p $@ | grep -q libwinpthread; then \
		echo "dinput8.dll imports libwinpthread: the game cannot load it"; \
		rm -f $@; exit 1; fi

$(GAMEDIR)/scripts/test_plugin/test_plugin.asi: test_plugin.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ test_plugin.c -lws2_32 -lgdi32 -luser32

clean:
	rm -f $(GAMEDIR)/dinput8.dll
	rm -rf $(GAMEDIR)/logs $(GAMEDIR)/scripts
