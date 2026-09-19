CC = x86_64-w64-mingw32-gcc
# -I. because a plugin source in plugins\<name>\ includes scripthook.h,
# log.h and image.h from the repo root; MSVC gets the same through /I.
CFLAGS = -O2 -Wall -Wextra -shared -static-libgcc -I.
# make QUIET=1: drop the four warning families every file
# trips (GetProcAddress casts, unused statics, strncpy), so
# only errors and new warnings reach the terminal.
ifdef QUIET
CFLAGS += -Wno-cast-function-type -Wno-unused-function \
          -Wno-strict-aliasing -Wno-stringop-truncation
endif
# Two up from src, so builds land beside GRW.exe.
GAMEDIR = ../..

# Sources live one folder per plugin under plugins/<name>/: its .c, its
# own <name>.ini if it has one, and lang.ini for its text. Plugins land
# in $(GAMEDIR)/plugins/<name>/<name>.asi, one folder per plugin, which
# is what the loader scans for. Logs go into <gamedir>/logs at runtime.

.PHONY: all roulette fling spawner npcspawner enemyreinforce modeprobe \
        modecallprobe blacklistsample filewatchsample drawsample cnchat \
        crazycars freecam fov fps chaos sample skipintro opticalcamo \
        ammocapacity timeweathercontrol micfix \
        docs clean

capprobe: $(GAMEDIR)/plugins/CapProbe/CapProbe.asi

sample: $(GAMEDIR)/plugins/ui_sample/ui_sample.asi

$(GAMEDIR)/plugins/ui_sample/ui_sample.asi: plugins/ui_sample/ui_sample.c scripthook.h libscripthook.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ plugins/ui_sample/ui_sample.c -L. -lscripthook -luser32

ammocapacity: $(GAMEDIR)/plugins/ammo_capacity/ammo_capacity.asi

# The third-party AmmoCapacity.asi rewritten against this framework
# (docs/ammocapacity-reverse.md, kept out of the repository). The hook and
# the arithmetic are the framework's (scripthook_ammocap.c); this plugin owns
# the values, the ini and the menu, and links the import library.
$(GAMEDIR)/plugins/ammo_capacity/ammo_capacity.asi: plugins/ammo_capacity/ammo_capacity.c scripthook.h libscripthook.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ plugins/ammo_capacity/ammo_capacity.c -L. -lscripthook

timeweathercontrol: $(GAMEDIR)/plugins/TimeWeatherControl/TimeWeatherControl.asi

# The rewrite that replaced the third-party Time&Weather.asi outright
# (docs/timeweather-reverse.md, kept out of the repository); the old plugin's
# folder, its three ini and its switch are gone from the tree and from the
# game folder. It installs no hook either: every change goes through the
# framework's own weather API, so there is nothing to take over from anyone.
$(GAMEDIR)/plugins/TimeWeatherControl/TimeWeatherControl.asi: plugins/TimeWeatherControl/TimeWeatherControl.c scripthook.h libscripthook.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ plugins/TimeWeatherControl/TimeWeatherControl.c -L. -lscripthook

blacklistsample: $(GAMEDIR)/plugins/blacklist_sample/blacklist_sample.asi

# Late binds every framework call by name, so no import library here; see
# the file header for what it demonstrates.
$(GAMEDIR)/plugins/blacklist_sample/blacklist_sample.asi: plugins/blacklist_sample/blacklist_sample.c scripthook.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ plugins/blacklist_sample/blacklist_sample.c

filewatchsample: $(GAMEDIR)/plugins/file_watch_sample/file_watch_sample.asi

# The worked example docs/file-interception.md points at. It links the
# framework (unlike blacklist_sample, which late binds), because its rules
# are made with the ShFile* calls whose types only the header carries.
$(GAMEDIR)/plugins/file_watch_sample/file_watch_sample.asi: plugins/file_watch_sample/file_watch_sample.c scripthook.h libscripthook.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ plugins/file_watch_sample/file_watch_sample.c -L. -lscripthook

drawsample: $(GAMEDIR)/plugins/draw_sample/draw_sample.asi

# The worked example docs/ui-drawing.md points at. It links the framework,
# because the primitives and their types only the header carries. Note the
# primitives are no-ops in this build: the overlay (ImGui over D3D11) is
# MSVC-only, so under MinGW the plugin loads, registers and draws nothing.
$(GAMEDIR)/plugins/draw_sample/draw_sample.asi: plugins/draw_sample/draw_sample.c scripthook.h libscripthook.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ plugins/draw_sample/draw_sample.c -L. -lscripthook

cnchat: $(GAMEDIR)/plugins/cnchat/cnchat.asi

# In-game Chinese text input. It links the framework: the box, the input
# session and the character collection are ShDraw* calls.
$(GAMEDIR)/plugins/cnchat/cnchat.asi: plugins/cnchat/cnchat.c scripthook.h log.h libscripthook.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ plugins/cnchat/cnchat.c -L. -lscripthook -luser32

docs:
	doxygen Doxyfile

all: $(GAMEDIR)/dinput8.dll $(GAMEDIR)/plugins/test_plugin/test_plugin.asi

roulette: $(GAMEDIR)/plugins/tp_roulette/tp_roulette.asi

fling: $(GAMEDIR)/plugins/hitfling/hitfling.asi

# These bind with GetProcAddress. Linking libscripthook.a
# instead works too, since the loader loads plugins from a
# thread rather than from DllMain.
$(GAMEDIR)/plugins/hitfling/hitfling.asi: plugins/hitfling/hitfling.c scripthook.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ plugins/hitfling/hitfling.c -lgdi32 -luser32

freecam: $(GAMEDIR)/plugins/freecam/freecam.asi

$(GAMEDIR)/plugins/freecam/freecam.asi: plugins/freecam/freecam.c scripthook.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ plugins/freecam/freecam.c -lgdi32 -luser32

fps: $(GAMEDIR)/plugins/firstperson/firstperson.asi

$(GAMEDIR)/plugins/firstperson/firstperson.asi: plugins/firstperson/firstperson.c scripthook.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ plugins/firstperson/firstperson.c -lgdi32 -luser32

chaos: $(GAMEDIR)/plugins/chaos/chaos.asi

$(GAMEDIR)/plugins/chaos/chaos.asi: plugins/chaos/chaos.c scripthook.h libscripthook.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ plugins/chaos/chaos.c \
		-L. -lscripthook -lgdi32 -luser32 -lwinmm

skipintro: $(GAMEDIR)/plugins/skipintro/skipintro.asi

$(GAMEDIR)/plugins/skipintro/skipintro.asi: plugins/skipintro/skipintro.c scripthook.h libscripthook.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ plugins/skipintro/skipintro.c -L. -lscripthook

# OpticalCamo, the third-party plugin rewritten against this framework
# (docs/opticacamo-reverse.md, kept out of the repository), plus the ini
# seeded next to the .asi by build_msvc.ps1. It links the import library.
opticalcamo: $(GAMEDIR)/plugins/OpticalCamo/OpticalCamo.asi

$(GAMEDIR)/plugins/OpticalCamo/OpticalCamo.asi: plugins/OpticalCamo/OpticalCamo.c scripthook.h log.h libscripthook.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ plugins/OpticalCamo/OpticalCamo.c -L. -lscripthook

fov: $(GAMEDIR)/plugins/fov_changer/fov_changer.asi

$(GAMEDIR)/plugins/fov_changer/fov_changer.asi: plugins/fov_changer/fov_changer.c scripthook.h libscripthook.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ plugins/fov_changer/fov_changer.c \
		-L. -lscripthook -lgdi32 -luser32

spawner: $(GAMEDIR)/plugins/spawner/spawner.asi

$(GAMEDIR)/plugins/spawner/spawner.asi: plugins/spawner/spawner.c scripthook.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ plugins/spawner/spawner.c -lgdi32 -luser32

npcspawner: $(GAMEDIR)/plugins/NPCSpawner/NPCSpawner.asi

$(GAMEDIR)/plugins/NPCSpawner/NPCSpawner.asi: plugins/NPCSpawner/NPCSpawner.c scripthook.h log.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ plugins/NPCSpawner/NPCSpawner.c

enemyreinforce: $(GAMEDIR)/plugins/EnemyReinforce/EnemyReinforce.asi

# EnemyReinforce.ini sits beside the source and is seeded into the
# plugin folder by build_msvc.ps1; this target only builds the .asi.
$(GAMEDIR)/plugins/EnemyReinforce/EnemyReinforce.asi: plugins/EnemyReinforce/EnemyReinforce.c scripthook.h log.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ plugins/EnemyReinforce/EnemyReinforce.c

modeprobe: $(GAMEDIR)/plugins/ModeProbe/ModeProbe.asi

# ModeProbe.ini sits beside the source and is seeded into the plugin
# folder by build_msvc.ps1; this target only builds the .asi.
$(GAMEDIR)/plugins/ModeProbe/ModeProbe.asi: plugins/ModeProbe/ModeProbe.c scripthook.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ plugins/ModeProbe/ModeProbe.c -luser32

modecallprobe: $(GAMEDIR)/plugins/ModeCallProbe/ModeCallProbe.asi

# ModeCallProbe.ini sits beside the source and is seeded into the plugin
# folder by build_msvc.ps1; this target only builds the .asi. MinHook is
# compiled in: it keeps its state per DLL, so each plugin that hooks
# carries its own copy. Contrast: GhostWipeProbe / ModeExitProbe.
$(GAMEDIR)/plugins/ModeCallProbe/ModeCallProbe.asi: plugins/ModeCallProbe/ModeCallProbe.c scripthook.h \
        third_party/minhook/src/buffer.c third_party/minhook/src/hook.c \
        third_party/minhook/src/trampoline.c \
        third_party/minhook/src/hde/hde64.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ plugins/ModeCallProbe/ModeCallProbe.c \
		third_party/minhook/src/buffer.c \
		third_party/minhook/src/hook.c \
		third_party/minhook/src/trampoline.c \
		third_party/minhook/src/hde/hde64.c \
		-Ithird_party/minhook/include -luser32

crazycars: $(GAMEDIR)/plugins/CrazyCars/CrazyCars.asi

$(GAMEDIR)/plugins/CrazyCars/CrazyCars.asi: plugins/CrazyCars/crazycars.c scripthook.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ plugins/CrazyCars/crazycars.c -lgdi32 -luser32

tpgun: $(GAMEDIR)/plugins/tpgun/tpgun.asi

$(GAMEDIR)/plugins/tpgun/tpgun.asi: plugins/tpgun/tpgun.c scripthook.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ plugins/tpgun/tpgun.c -lgdi32 -luser32

$(GAMEDIR)/plugins/tp_roulette/tp_roulette.asi: plugins/tp_roulette/tp_roulette.c scripthook.h libscripthook.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ plugins/tp_roulette/tp_roulette.c \
		-L. -lscripthook -lgdi32 -luser32

micfix: $(GAMEDIR)/plugins/micfix/micfix.asi

# micfix.ini and lang.ini sit beside the source and are seeded into the plugin
# folder by build_msvc.ps1; this target only builds the .asi. MinHook is
# compiled in (the plugin hooks ole32!CoCreateInstance) and the framework is
# linked, because the menu, the text and the paths are all Sh* calls.
# oleaut32 is the BSTR written back into a VARIANT, ole32 is
# CoInitializeEx/CoUninitialize for the scan's own thread; CoCreateInstance
# itself is resolved by name at run time.
$(GAMEDIR)/plugins/micfix/micfix.asi: plugins/micfix/micfix.c scripthook.h log.h \
        libscripthook.a \
        third_party/minhook/src/buffer.c third_party/minhook/src/hook.c \
        third_party/minhook/src/trampoline.c \
        third_party/minhook/src/hde/hde64.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ plugins/micfix/micfix.c \
		third_party/minhook/src/buffer.c \
		third_party/minhook/src/hook.c \
		third_party/minhook/src/trampoline.c \
		third_party/minhook/src/hde/hde64.c \
		-Ithird_party/minhook/include \
		-L. -lscripthook -lole32 -loleaut32

libscripthook.a: $(GAMEDIR)/dinput8.dll

$(GAMEDIR)/dinput8.dll: loader.c scripthook_api.c scripthook_config.c scripthook_tick.c \
                        scripthook_text.c \
                        scripthook_physics.c \
                        scripthook_health.c scripthook_state.c \
                        scripthook_playmode.c scripthook_blacklist.c \
                        scripthook_entity.c scripthook_spawn.c \
                        scripthook_npc.c scripthook_domino.c \
                        scripthook_hit.c scripthook_camera.c \
                        scripthook_head.c scripthook_fov.c \
                        scripthook_blur.c scripthook_fpx.c \
                        scripthook_stat.c scripthook_resource.c \
                        scripthook_stealth.c \
			scripthook_ammocap.c \
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
		scripthook_config.c scripthook_text.c scripthook_tick.c \
		scripthook_physics.c scripthook_health.c \
		scripthook_state.c scripthook_playmode.c scripthook_blacklist.c \
		scripthook_entity.c \
		scripthook_spawn.c scripthook_npc.c scripthook_domino.c scripthook_hit.c \
		scripthook_camera.c scripthook_head.c \
		scripthook_fov.c scripthook_blur.c scripthook_fpx.c \
		scripthook_stat.c scripthook_resource.c \
		scripthook_stealth.c \
			scripthook_ammocap.c \
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

$(GAMEDIR)/plugins/test_plugin/test_plugin.asi: plugins/test_plugin/test_plugin.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ plugins/test_plugin/test_plugin.c -lws2_32 -lgdi32 -luser32

clean:
	rm -f $(GAMEDIR)/dinput8.dll
	rm -rf $(GAMEDIR)/logs $(GAMEDIR)/plugins
