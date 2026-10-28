// Clip Director hotkeys (W4): Skip / Hold / Ban as frontend hotkeys, plus the listing and
// triggering helpers the MIDI Director dock uses to drive any registered hotkey by identity.
#include "cd-hotkeys.h"

#include <obs-module.h>
#include <obs.h>
#include <plugin-support.h>

#include <algorithm>
#include <string>
#include <vector>

#include "obs/cd-api.h"

namespace {

std::string text(const char *s)
{
	return s ? std::string(s) : std::string();
}

std::string registerer_of(const obs_hotkey_t *key)
{
	switch (obs_hotkey_get_registerer_type(key)) {
	case OBS_HOTKEY_REGISTERER_SOURCE:
		return "source";
	case OBS_HOTKEY_REGISTERER_OUTPUT:
		return "output";
	case OBS_HOTKEY_REGISTERER_ENCODER:
		return "encoder";
	case OBS_HOTKEY_REGISTERER_SERVICE:
		return "service";
	case OBS_HOTKEY_REGISTERER_FRONTEND:
		break;
	}
	return "frontend";
}

// Source-level hotkeys are identified by their source's name; nothing else carries one.
std::string registerer_name_of(const obs_hotkey_t *key)
{
	if (obs_hotkey_get_registerer_type(key) != OBS_HOTKEY_REGISTERER_SOURCE)
		return std::string();
	/* libobs registers a source hotkey with a WEAK reference as its registerer (obs-hotkey.c,
	 * obs_hotkey_register_source), so take a strong ref for the name, as OBS's own hotkey
	 * settings do. Reading it as an obs_source_t crashed OBS when the MIDI dock listed hotkeys. */
	auto *weak = static_cast<obs_weak_source_t *>(obs_hotkey_get_registerer(key));
	obs_source_t *source = weak ? obs_weak_source_get_source(weak) : nullptr;
	if (!source)
		return std::string();
	std::string name = text(obs_source_get_name(source));
	obs_source_release(source);
	return name;
}

bool collect(void *data, obs_hotkey_id, obs_hotkey_t *key)
{
	auto *out = static_cast<std::vector<cdhotkey::Entry> *>(data);
	cdhotkey::Entry entry;
	entry.name = text(obs_hotkey_get_name(key));
	if (entry.name.empty())
		return true; /* an unnamed hotkey cannot be stored in a mapping */
	entry.description = text(obs_hotkey_get_description(key));
	if (entry.description.empty())
		entry.description = entry.name;
	entry.registerer = registerer_of(key);
	entry.registerer_name = registerer_name_of(key);
	out->push_back(entry);
	return true;
}

struct Match {
	const cdhotkey::Entry *want = nullptr;
	obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
};

bool matches(void *data, obs_hotkey_id id, obs_hotkey_t *key)
{
	auto *match = static_cast<Match *>(data);
	if (match->id != OBS_INVALID_HOTKEY_ID)
		return false;
	if (text(obs_hotkey_get_name(key)) != match->want->name)
		return true;
	if (registerer_of(key) != match->want->registerer)
		return true;
	if (registerer_name_of(key) != match->want->registerer_name)
		return true;
	match->id = id;
	return false;
}

#if defined(ENABLE_FRONTEND_API)

using Action = void (*)(obs_source_t *source, bool pressed);

// A hotkey acts on the first Clip Director source of the scene collection: OBS hotkeys are global
// while the dock picks its own source, and one director drives the set.
void run_on_director(Action action, bool pressed)
{
	obs_source_t *director = nullptr;
	obs_enum_sources(
		[](void *data, obs_source_t *source) {
			if (!cdapi::is_clip_director(source))
				return true;
			*(obs_source_t **)data = obs_source_get_ref(source);
			return false;
		},
		&director);
	if (!director)
		return;
	action(director, pressed);
	obs_source_release(director);
}

/* cd_skip: cut to the next clip on the next beat (the dock's Skip button). */
void action_skip(obs_source_t *source, bool pressed)
{
	if (pressed)
		cdapi::skip(source);
}

/* cd_hold: pressed holds the clip, released resumes normal cuts. Momentary on purpose: the dock's
 * Hold button toggles, a MIDI pad or an OBS hotkey key is held down. */
void action_hold(obs_source_t *source, bool pressed)
{
	cdapi::set_hold(source, pressed);
}

/* cd_ban: ban the clip on screen and skip it (the dock's Ban button). */
void action_ban(obs_source_t *source, bool pressed)
{
	if (pressed)
		cdapi::ban_live(source);
}

Action g_actions[] = {action_skip, action_hold, action_ban};
obs_hotkey_id g_ids[] = {OBS_INVALID_HOTKEY_ID, OBS_INVALID_HOTKEY_ID, OBS_INVALID_HOTKEY_ID};
bool g_registered = false;

void hotkey_callback(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	run_on_director(*static_cast<Action *>(data), pressed);
}

#endif

} // namespace

namespace cdhotkey {

std::vector<Entry> list()
{
	std::vector<Entry> out;
	obs_enum_hotkeys(collect, &out);
	std::sort(out.begin(), out.end(), [](const Entry &a, const Entry &b) {
		if (a.description != b.description)
			return a.description < b.description;
		if (a.registerer != b.registerer)
			return a.registerer < b.registerer;
		if (a.registerer_name != b.registerer_name)
			return a.registerer_name < b.registerer_name;
		return a.name < b.name;
	});
	return out;
}

bool trigger(const Entry &entry, bool pressed)
{
	if (entry.name.empty())
		return false;
	Match match;
	match.want = &entry;
	obs_enum_hotkeys(matches, &match);
	if (match.id == OBS_INVALID_HOTKEY_ID) {
		obs_log(LOG_DEBUG, "midi: hotkey '%s' is not registered any more; mapping ignored", entry.name.c_str());
		return false;
	}
	/* Public libobs API (obs-hotkey.h): routes the callback through the frontend's router - we call
	 * it from the dock's UI thread - or invokes the hotkey callback directly when callback
	 * rerouting is off. Either way the hotkey fires exactly once. */
	obs_hotkey_trigger_routed_callback(match.id, pressed);
	return true;
}

} // namespace cdhotkey

extern "C" void clip_director_hotkey_load(void)
{
#if defined(ENABLE_FRONTEND_API)
	if (g_registered)
		return;
	g_ids[0] = obs_hotkey_register_frontend(cdhotkey::kSkip, obs_module_text("Hotkey.Skip"), hotkey_callback,
						&g_actions[0]);
	g_ids[1] = obs_hotkey_register_frontend(cdhotkey::kHold, obs_module_text("Hotkey.Hold"), hotkey_callback,
						&g_actions[1]);
	g_ids[2] = obs_hotkey_register_frontend(cdhotkey::kBan, obs_module_text("Hotkey.Ban"), hotkey_callback,
						&g_actions[2]);
	g_registered = true;
	obs_log(LOG_INFO, "hotkeys registered: %s, %s, %s", cdhotkey::kSkip, cdhotkey::kHold, cdhotkey::kBan);
#endif
}

extern "C" void clip_director_hotkey_unload(void)
{
#if defined(ENABLE_FRONTEND_API)
	if (!g_registered)
		return;
	for (obs_hotkey_id id : g_ids) {
		if (id != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(id);
	}
	g_registered = false;
#endif
}
