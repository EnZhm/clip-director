/*
Clip Director
Copyright (C) 2026 EnZhm

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <plugin-support.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

extern void clip_director_source_load(void);
extern void clip_director_dock_load(void);
extern void clip_director_syn_hotkeys_load(void);
extern void clip_director_syn_hotkeys_unload(void);
extern void clip_director_midi_dock_load(void);
extern void clip_director_midi_dock_unload(void);
extern void clip_director_hotkey_load(void);
extern void clip_director_hotkey_unload(void);

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	clip_director_source_load();
	clip_director_dock_load();
	clip_director_syn_hotkeys_load();
	clip_director_midi_dock_load();
	clip_director_hotkey_load();
	return true;
}

void obs_module_unload(void)
{
	clip_director_syn_hotkeys_unload();
	clip_director_hotkey_unload();
	clip_director_midi_dock_unload();
	obs_log(LOG_INFO, "plugin unloaded");
}
