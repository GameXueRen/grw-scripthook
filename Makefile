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

# Plugins land in plugins/<name>/<name>.asi, one folder per
# plugin, which is what the loader scans for. Logs go into
# <gamedir>/logs at runtime.

.PHONY: all roulette fling spawner npcspawner enemyreinforce modeprobe \
        modecallprobe blacklistsample filewatchsample drawsample cnchat \
        crazycars freecam fov fps chaos sample skipintro opticalcamo \
        docs clean

sample: $(GAMEDIR)/plugins/ui_sample/ui_sample.asi

$(GAMEDIR)/plugins/ui_sample/ui_sample.asi: ui_sample.c scripthook.h libscripthook.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ ui_sample.c -L. -lscripthook -luser32

blacklistsample: $(GAMEDIR)/plugins/blacklist_sample/blacklist_sample.asi

# Late binds every framework call by name, so no import library here; see
# the file header for what it demonstrates.
$(GAMEDIR)/plugins/blacklist_sample/blacklist_sample.asi: blacklist_sample.c scripthook.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ blacklist_sample.c

filewatchsample: $(GAMEDIR)/plugins/file_watch_sample/file_watch_sample.asi

# The worked example docs/file-interception.md points at. It links the
# framework (unlike blacklist_sample, which late binds), because its rules
# are made with the ShFile* calls whose types only the header carries.
$(GAMEDIR)/plugins/file_watch_sample/file_watch_sample.asi: file_watch_sample.c scripthook.h libscripthook.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ file_watch_sample.c -L. -lscripthook

drawsample: $(GAMEDIR)/plugins/draw_sample/draw_sample.asi

# The worked example docs/ui-drawing.md points at. It links the framework,
# because the primitives and their types only the header carries. Note the
# primitives are no-ops in this build: the overlay (ImGui over D3D11) is
# MSVC-only, so under MinGW the plugin loads, registers and draws nothing.
$(GAMEDIR)/plugins/draw_sample/draw_sample.asi: draw_sample.c scripthook.h libscripthook.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ draw_sample.c -L. -lscripthook

cnchat: $(GAMEDIR)/plugins/cnchat/cnchat.asi

# In-game Chinese text input. It links the framework: the box, the input
# session and the character collection are ShDraw* calls.
$(GAMEDIR)/plugins/cnchat/cnchat.asi: cnchat.c scripthook.h log.h libscripthook.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ cnchat.c -L. -lscripthook -luser32

docs:
	doxygen Doxyfile

all: $(GAMEDIR)/dinput8.dll $(GAMEDIR)/plugins/test_plugin/test_plugin.asi

roulette: $(GAMEDIR)/plugins/tp_roulette/tp_roulette.asi

fling: $(GAMEDIR)/plugins/hitfling/hitfling.asi

# These bind with GetProcAddress. Linking libscripthook.a
# instead works too, since the loader loads plugins from a
# thread rather than from DllMain.
$(GAMEDIR)/plugins/hitfling/hitfling.asi: hitfling.c scripthook.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ hitfling.c -lgdi32 -luser32

freecam: $(GAMEDIR)/plugins/freecam/freecam.asi

$(GAMEDIR)/plugins/freecam/freecam.asi: freecam.c scripthook.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ freecam.c -lgdi32 -luser32

fps: $(GAMEDIR)/plugins/firstperson/firstperson.asi

$(GAMEDIR)/plugins/firstperson/firstperson.asi: firstperson.c scripthook.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ firstperson.c -lgdi32 -luser32

chaos: $(GAMEDIR)/plugins/chaos/chaos.asi

$(GAMEDIR)/plugins/chaos/chaos.asi: chaos.c scripthook.h libscripthook.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ chaos.c \
		-L. -lscripthook -lgdi32 -luser32 -lwinmm

skipintro: $(GAMEDIR)/plugins/skipintro/skipintro.asi

$(GAMEDIR)/plugins/skipintro/skipintro.asi: skipintro.c scripthook.h libscripthook.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ skipintro.c -L. -lscripthook

# OpticalCamo, the third-party plugin rewritten against this framework
# (docs/opticacamo-reverse.md), plus the ini seeded next to the .asi by
# build_msvc.ps1. It links the import library.
opticalcamo: $(GAMEDIR)/plugins/OpticalCamo/OpticalCamo.asi

$(GAMEDIR)/plugins/OpticalCamo/OpticalCamo.asi: OpticalCamo.c scripthook.h log.h libscripthook.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ OpticalCamo.c -L. -lscripthook

fov: $(GAMEDIR)/plugins/fov_changer/fov_changer.asi

$(GAMEDIR)/plugins/fov_changer/fov_changer.asi: fov_changer.c scripthook.h libscripthook.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ fov_changer.c \
		-L. -lscripthook -lgdi32 -luser32

spawner: $(GAMEDIR)/plugins/spawner/spawner.asi

$(GAMEDIR)/plugins/spawner/spawner.asi: spawner.c scripthook.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ spawner.c -lgdi32 -luser32

npcspawner: $(GAMEDIR)/plugins/NPCSpawner/NPCSpawner.asi

$(GAMEDIR)/plugins/NPCSpawner/NPCSpawner.asi: NPCSpawner.c scripthook.h log.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ NPCSpawner.c

enemyreinforce: $(GAMEDIR)/plugins/EnemyReinforce/EnemyReinforce.asi

# EnemyReinforce.ini sits beside the source and is seeded into the
# plugin folder by build_msvc.ps1; this target only builds the .asi.
$(GAMEDIR)/plugins/EnemyReinforce/EnemyReinforce.asi: EnemyReinforce.c scripthook.h log.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ EnemyReinforce.c

modeprobe: $(GAMEDIR)/plugins/ModeProbe/ModeProbe.asi

# ModeProbe.ini sits beside the source and is seeded into the plugin
# folder by build_msvc.ps1; this target only builds the .asi.
$(GAMEDIR)/plugins/ModeProbe/ModeProbe.asi: ModeProbe.c scripthook.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ ModeProbe.c -luser32

modecallprobe: $(GAMEDIR)/plugins/ModeCallProbe/ModeCallProbe.asi

# ModeCallProbe.ini sits beside the source and is seeded into the plugin
# folder by build_msvc.ps1; this target only builds the .asi. MinHook is
# compiled in: it keeps its state per DLL, so each plugin that hooks
# carries its own copy. Contrast: GhostWipeProbe / ModeExitProbe.
$(GAMEDIR)/plugins/ModeCallProbe/ModeCallProbe.asi: ModeCallProbe.c scripthook.h \
        third_party/minhook/src/buffer.c third_party/minhook/src/hook.c \
        third_party/minhook/src/trampoline.c \
        third_party/minhook/src/hde/hde64.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ ModeCallProbe.c \
		third_party/minhook/src/buffer.c \
		third_party/minhook/src/hook.c \
		third_party/minhook/src/trampoline.c \
		third_party/minhook/src/hde/hde64.c \
		-Ithird_party/minhook/include -luser32

crazycars: $(GAMEDIR)/plugins/CrazyCars/CrazyCars.asi

$(GAMEDIR)/plugins/CrazyCars/CrazyCars.asi: crazycars.c scripthook.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ crazycars.c -lgdi32 -luser32

tpgun: $(GAMEDIR)/plugins/tpgun/tpgun.asi

$(GAMEDIR)/plugins/tpgun/tpgun.asi: tpgun.c scripthook.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ tpgun.c -lgdi32 -luser32

$(GAMEDIR)/plugins/tp_roulette/tp_roulette.asi: tp_roulette.c scripthook.h libscripthook.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ tp_roulette.c \
		-L. -lscripthook -lgdi32 -luser32

libscripthook.a: $(GAMEDIR)/dinput8.dll

$(GAMEDIR)/dinput8.dll: loader.c scripthook_api.c scripthook_config.c \
                        scripthook_physics.c \
                        scripthook_health.c scripthook_state.c \
                        scripthook_playmode.c scripthook_blacklist.c \
                        scripthook_entity.c scripthook_spawn.c \
                        scripthook_npc.c scripthook_domino.c \
                        scripthook_hit.c scripthook_camera.c \
                        scripthook_head.c scripthook_fov.c \
                        scripthook_blur.c scripthook_fpx.c \
                        scripthook_stat.c scripthook_resource.c \
                        scripthook_stealth.c scripthook_ammo.c \
                        scripthook_weather.c scripthook_crash.c \
                        scripthook_input.c scripthook_havok.c \
                        scripthook_reflect.c scripthook_ui.c \
                        scripthook_scene.c scripthook_uiprop.c \
                        scripthook_uiinput.c scripthook_dinput.c \
                        scripthook_hud.c scripthook_menu.c \
                        scripthook_draw.c \
                        guard.c scripthook.h log.h \
                        scripthook_corefix.c scripthook_modsettings.c \
                        forge.c scripthook_forge.c scripthook_forge_io.c \
                        scripthook_forgeprobe.c scripthook_files.c \
                        third_party/minhook/src/buffer.c \
                        third_party/minhook/src/hook.c \
                        third_party/minhook/src/trampoline.c \
                        third_party/minhook/src/hde/hde64.c
	$(CC) $(CFLAGS) -o $@ loader.c scripthook_api.c \
		scripthook_config.c \
		scripthook_physics.c scripthook_health.c \
		scripthook_state.c scripthook_playmode.c scripthook_blacklist.c \
		scripthook_entity.c \
		scripthook_spawn.c scripthook_npc.c scripthook_domino.c scripthook_hit.c \
		scripthook_camera.c scripthook_head.c \
		scripthook_fov.c scripthook_blur.c scripthook_fpx.c \
		scripthook_stat.c scripthook_resource.c \
		scripthook_stealth.c scripthook_ammo.c \
		scripthook_weather.c scripthook_crash.c \
		scripthook_input.c scripthook_havok.c \
		scripthook_reflect.c scripthook_ui.c \
		scripthook_scene.c scripthook_uiprop.c \
		scripthook_uiinput.c scripthook_dinput.c \
		scripthook_hud.c scripthook_menu.c \
		scripthook_draw.c guard.c \
		scripthook_corefix.c scripthook_modsettings.c \
		forge.c scripthook_forge.c scripthook_forge_io.c \
		scripthook_forgeprobe.c scripthook_files.c \
		third_party/minhook/src/buffer.c \
		third_party/minhook/src/hook.c \
		third_party/minhook/src/trampoline.c \
		third_party/minhook/src/hde/hde64.c \
		-Ithird_party/minhook/include \
		-ldinput8 -ldxguid -lgdi32 -luser32 \
		-Wl,--out-implib,libscripthook.a
	@if x86_64-w64-mingw32-objdump -p $@ | grep -q libwinpthread; then \
		echo "dinput8.dll imports libwinpthread: the game cannot load it"; \
		rm -f $@; exit 1; fi

$(GAMEDIR)/plugins/test_plugin/test_plugin.asi: test_plugin.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ test_plugin.c -lws2_32 -lgdi32 -luser32

clean:
	rm -f $(GAMEDIR)/dinput8.dll
	rm -rf $(GAMEDIR)/logs $(GAMEDIR)/plugins
