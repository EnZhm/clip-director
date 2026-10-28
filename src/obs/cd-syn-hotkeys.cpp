/*
Clip Director: frontend hotkeys for the Synesthesia controls (W2/W3). A separate MIDI dock
triggers these by name (cd_syn_next_scene, cd_syn_hold_scene, cd_syn_bang), and OBS's own
Settings > Hotkeys list shows them. They act on the first Clip Director source in the scene
collection, the same one the dock's picker would default to.
*/
#include <obs-module.h>
#include <plugin-support.h>

#include <string>

#include "obs/cd-api.h"

namespace {

obs_hotkey_id hk_next = OBS_INVALID_HOTKEY_ID;
obs_hotkey_id hk_hold = OBS_INVALID_HOTKEY_ID;
obs_hotkey_id hk_bang = OBS_INVALID_HOTKEY_ID;

enum class Action { Next, Hold, Bang };

Action g_next = Action::Next;
Action g_hold = Action::Hold;
Action g_bang = Action::Bang;

/* the action runs inside the enumeration, where libobs holds the reference for us */
void on_hotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	Action action = *static_cast<Action *>(data);
	obs_enum_sources(
		[](void *p, obs_source_t *s) {
			if (!cdapi::is_clip_director(s))
				return true; /* keep looking */
			switch (*static_cast<Action *>(p)) {
			case Action::Next:
				cdapi::syn_next(s);
				break;
			case Action::Hold:
				cdapi::syn_hold_toggle(s);
				break;
			case Action::Bang:
				cdapi::syn_bang(s);
				break;
			}
			return false; /* first source wins: stop enumerating */
		},
		&action);
}

} // namespace

extern "C" void clip_director_syn_hotkeys_load(void)
{
	hk_next = obs_hotkey_register_frontend("cd_syn_next_scene", obs_module_text("Hotkey.SynNextScene"), on_hotkey,
					       &g_next);
	hk_hold = obs_hotkey_register_frontend("cd_syn_hold_scene", obs_module_text("Hotkey.SynHoldScene"), on_hotkey,
					       &g_hold);
	hk_bang = obs_hotkey_register_frontend("cd_syn_bang", obs_module_text("Hotkey.SynBang"), on_hotkey, &g_bang);
	obs_log(LOG_INFO, "Synesthesia hotkeys registered (cd_syn_next_scene, cd_syn_hold_scene, cd_syn_bang)");
}

extern "C" void clip_director_syn_hotkeys_unload(void)
{
	if (hk_next != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(hk_next);
	if (hk_hold != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(hk_hold);
	if (hk_bang != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(hk_bang);
	hk_next = hk_hold = hk_bang = OBS_INVALID_HOTKEY_ID;
}
