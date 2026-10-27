/*
Clip Director source (P4): beat-synced, shuffled clip playback following Palette Director's clock.

Threads: video_tick + video_render run on the graphics thread; update/buttons on the UI thread.
One mutex guards the state; the graphics thread holds it only for in-memory work.

Decks: four private ffmpeg_source children. A deck is "prepared" by loading a file (which
restarts its media), seeking to the start point and pausing; a cut un-pauses it. Roles:
  live    on screen
  spare   the outgoing clip, kept playing for crossfades and stutter
  slot    kNext / kDrop decks waiting for their cut (core/scheduler.h)
*/

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <util/platform.h>
#include <plugin-support.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <thread>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "pd-clock-api.h"
#include "obs/cd-library.h"
#include "levels.h"
#include "picker.h"
#include "scheduler.h"

namespace {

constexpr uint32_t kW = 1920, kH = 1080;
constexpr int kDecks = 4;
constexpr const char *kClockAuto = "";
constexpr const char *kClockFree = "\x01free";

struct Deck {
	obs_source_t *src = nullptr;
	int clip = -1;
	double inpoint = 0.0, speed = 1.0;
	bool loops = false;
	uint64_t load_ns = 0; /* when the load finished (ready) */
	uint64_t gen = 0;     /* bumps on every queued load; stale completions are ignored */
	bool ready = false;   /* load done: file open, seeked, paused */
	gs_texrender_t *tr = nullptr;
	cd::LevelTracker lv;
};

struct Cfg {
	std::vector<cd::Folder> folders;
	std::string folders_key;
	std::string clock; // "" auto, kClockFree, or "source|filter"
	double sync_ms = 0.0, fallback_bpm = 128.0;
	cd::PickerConfig pk;
	cd::SchedulerConfig sc;
	cd::LevelConfig lc;
	int alpha_mode = 1;
	double alpha_lo = 0.02, alpha_hi = 0.20;
};

struct CdSource {
	obs_source_t *self = nullptr;
	std::mutex mx;
	Cfg cfg;
	cdobs::Library lib;
	std::unique_ptr<cd::ClipPicker> picker;
	std::unique_ptr<cd::CutScheduler> sched;
	Deck decks[kDecks];
	int slot_deck[2] = {-1, -1};
	int live = -1, spare = -1;

	bool tr_active = false;
	cd::Transition tr_type = cd::Transition::Cut;
	double tr_start = 0.0, tr_len = 0.0;
	bool show_prev = false;

	double beats = 0.0, bpm = 128.0;
	bool clock_ok = false, clock_was_ok = false;
	double free_beats = 0.0;
	cd::Section section = cd::Section::Groove;

	gs_effect_t *effect = nullptr;

	/* deck loader thread: obs_source_update on an ffmpeg_source re-opens the media (file open,
	 * decoder init), far too slow for the graphics thread */
	struct LoadJob {
		int deck;
		uint64_t gen;
		std::string path;
		double inpoint, speed;
		uint64_t queued_ns;
	};
	std::thread loader;
	std::mutex lq_mx;
	std::condition_variable lq_cv;
	std::deque<LoadJob> lq;
	bool lq_stop = false;
	double load_ms_sum = 0.0, load_ms_max = 0.0;
	int verify_retries = 0, verify_fail = 0;
	int load_n = 0;

	/* a cut whose deck is still loading waits here (fires on the first frame the deck is ready) */
	bool deferred = false;
	cd::SchedEvent deferred_ev;

	uint64_t cuts = 0;
	double stat_t = 0.0;
	double cut_err_sum = 0.0, cut_err_max = 0.0;
	int cut_err_n = 0;
};

const char *sec_name(cd::Section s)
{
	return s == cd::Section::Breakdown ? "breakdown" : (s == cd::Section::Drop ? "drop" : "groove");
}

const char *tr_name(cd::Transition t)
{
	return t == cd::Transition::Crossfade ? "crossfade" : (t == cd::Transition::Dip ? "dip" : "cut");
}

/* ------------------------------------------------------------------------- */
/* settings                                                                  */

cd::Transition tr_from_int(long long v)
{
	return v == 1 ? cd::Transition::Crossfade : (v == 2 ? cd::Transition::Dip : cd::Transition::Cut);
}

Cfg read_cfg(obs_data_t *s)
{
	Cfg c;
	obs_data_array_t *arr = obs_data_get_array(s, "folders");
	const size_t n = obs_data_array_count(arr);
	for (size_t i = 0; i < n; ++i) {
		obs_data_t *it = obs_data_array_item(arr, i);
		const char *v = obs_data_get_string(it, "value");
		if (v && *v) {
			cd::Folder f;
			f.path = v;
			f.enabled = !obs_data_has_user_value(it, "hidden") || !obs_data_get_bool(it, "hidden");
			c.folders.push_back(f);
			c.folders_key += f.path + (f.enabled ? "|1;" : "|0;");
		}
		obs_data_release(it);
	}
	obs_data_array_release(arr);

	c.clock = obs_data_get_string(s, "clock_source");
	c.sync_ms = (double)obs_data_get_int(s, "sync_offset_ms");
	c.fallback_bpm = obs_data_get_double(s, "fallback_bpm");

	c.pk.seed = (uint32_t)obs_data_get_int(s, "seed");
	c.pk.speed_min = obs_data_get_int(s, "speed_min_pct") / 100.0;
	c.pk.speed_max = std::max(c.pk.speed_min, obs_data_get_int(s, "speed_max_pct") / 100.0);
	c.pk.speed_extreme_chance = obs_data_get_double(s, "speed_extreme_chance");

	c.sc.seed = c.pk.seed;
	c.sc.preroll_beats = obs_data_get_double(s, "preroll_beats");
	c.sc.stutter = obs_data_get_bool(s, "stutter");
	c.sc.groove.hold_bars = (int)obs_data_get_int(s, "groove_hold_bars");
	c.sc.groove.transition = tr_from_int(obs_data_get_int(s, "groove_transition"));
	c.sc.groove.transition_beats = obs_data_get_double(s, "groove_transition_beats");
	c.sc.groove.crossfade_chance = obs_data_get_double(s, "groove_crossfade_chance");
	c.sc.breakdown.hold_bars = (int)obs_data_get_int(s, "breakdown_hold_bars");
	c.sc.breakdown.transition = tr_from_int(obs_data_get_int(s, "breakdown_transition"));
	c.sc.breakdown.transition_beats = obs_data_get_double(s, "breakdown_transition_beats");
	c.sc.drop.hold_bars = (int)obs_data_get_int(s, "drop_hold_bars");
	c.sc.drop.transition = tr_from_int(obs_data_get_int(s, "drop_transition"));
	c.sc.drop.transition_beats = obs_data_get_double(s, "drop_transition_beats");

	c.lc.enabled = obs_data_get_bool(s, "normalize");
	c.lc.strength = obs_data_get_double(s, "normalize_strength");
	c.lc.target_mean = obs_data_get_double(s, "target_mean");
	c.lc.max_gain = obs_data_get_double(s, "max_gain");
	c.lc.noise_floor = obs_data_get_double(s, "noise_floor");
	c.lc.adapt = obs_data_get_bool(s, "adapt");
	c.lc.adapt_tau_s = obs_data_get_double(s, "adapt_tau_s");
	c.lc.polarity_threshold = obs_data_get_double(s, "polarity_threshold");

	c.alpha_mode = (int)obs_data_get_int(s, "output_mode");
	c.alpha_lo = obs_data_get_double(s, "alpha_lo");
	c.alpha_hi = std::max(c.alpha_lo + 0.01, obs_data_get_double(s, "alpha_hi"));
	return c;
}

void cd_defaults(obs_data_t *s)
{
	obs_data_set_default_string(s, "clock_source", kClockAuto);
	obs_data_set_default_int(s, "sync_offset_ms", 0);
	obs_data_set_default_double(s, "fallback_bpm", 128.0);
	obs_data_set_default_int(s, "seed", 7);
	obs_data_set_default_int(s, "speed_min_pct", 100);
	obs_data_set_default_int(s, "speed_max_pct", 100);
	obs_data_set_default_double(s, "speed_extreme_chance", 0.0);
	obs_data_set_default_double(s, "preroll_beats", 2.0);
	obs_data_set_default_bool(s, "stutter", true);
	obs_data_set_default_int(s, "groove_hold_bars", 4);
	obs_data_set_default_int(s, "groove_transition", 0);
	obs_data_set_default_double(s, "groove_transition_beats", 1.0);
	obs_data_set_default_double(s, "groove_crossfade_chance", 0.25);
	obs_data_set_default_int(s, "breakdown_hold_bars", 8);
	obs_data_set_default_int(s, "breakdown_transition", 1);
	obs_data_set_default_double(s, "breakdown_transition_beats", 4.0);
	obs_data_set_default_int(s, "drop_hold_bars", 2);
	obs_data_set_default_int(s, "drop_transition", 0);
	obs_data_set_default_double(s, "drop_transition_beats", 1.0);
	obs_data_set_default_bool(s, "normalize", true);
	obs_data_set_default_double(s, "normalize_strength", 0.7);
	obs_data_set_default_double(s, "target_mean", 0.22);
	obs_data_set_default_double(s, "max_gain", 3.0);
	obs_data_set_default_double(s, "noise_floor", 0.02);
	obs_data_set_default_bool(s, "adapt", true);
	obs_data_set_default_double(s, "adapt_tau_s", 4.0);
	obs_data_set_default_double(s, "polarity_threshold", 0.45);
	obs_data_set_default_int(s, "output_mode", 1);
	obs_data_set_default_double(s, "alpha_lo", 0.02);
	obs_data_set_default_double(s, "alpha_hi", 0.20);
}

/* ------------------------------------------------------------------------- */
/* decks                                                                     */

/* worker side (no CdSource::mx held). libobs defers obs_source_update() of video sources to the
 * graphics thread, so re-pointing an existing ffmpeg_source would re-open its media there. Instead
 * each load creates a fresh private ffmpeg_source here (create runs its update directly), seeks and
 * pauses that new media, and hands it to the deck. */
obs_source_t *deck_create(int deck, const std::string &path, double inpoint, double speed, int *attempts,
			  double *err_ms)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_bool(s, "is_local_file", true);
	obs_data_set_string(s, "local_file", path.c_str());
	obs_data_set_bool(s, "looping", true);
	obs_data_set_bool(s, "restart_on_activate", false);
	obs_data_set_bool(s, "close_when_inactive", false);
	obs_data_set_bool(s, "clear_on_media_end", false);
	obs_data_set_bool(s, "hw_decode", true);
	obs_data_set_bool(s, "log_changes", false);
	obs_data_set_int(s, "speed_percent", std::clamp((int)std::lround(speed * 100.0), 1, 200));
	char nm[64];
	snprintf(nm, sizeof(nm), "clip-director deck %d", deck);
	obs_source_t *src = obs_source_create_private("ffmpeg_source", nm, s);
	obs_data_release(s);
	if (!src)
		return nullptr;
	obs_source_inc_active(src);
	obs_source_inc_showing(src);

	/* The media thread opens the file asynchronously and its reset (mp_media_reset) clears any
	 * pause and seeks to 0 once the open completes. So: wait for the open, then seek + pause,
	 * then verify the media time is frozen at the start point; re-issue if it is not. */
	const int64_t target = (int64_t)std::llround(inpoint * 1000.0);
	for (int i = 0; i < 300 && obs_source_media_get_duration(src) <= 0; ++i)
		os_sleep_ms(10);
	/* ...and until it is actually playing (time > 0): only then has the reset run, so a start
	 * point of 0 cannot pass the check below before the reset un-pauses it again */
	for (int i = 0; i < 150 && obs_source_media_get_time(src) <= 0; ++i)
		os_sleep_ms(10);
	*attempts = 0;
	*err_ms = 1e9;
	for (int a = 1; a <= 5; ++a) {
		*attempts = a;
		obs_source_media_set_time(src, target);
		obs_source_media_play_pause(src, true);
		os_sleep_ms(60);
		const int64_t t1 = obs_source_media_get_time(src);
		os_sleep_ms(40);
		const int64_t t2 = obs_source_media_get_time(src);
		*err_ms = (double)(t2 - target);
		if (std::llabs(t2 - t1) < 20 && std::llabs(t2 - target) < 300)
			break;
	}
	return src;
}

void deck_release(obs_source_t *src)
{
	if (!src)
		return;
	obs_source_dec_showing(src);
	obs_source_dec_active(src);
	obs_source_release(src);
}

void loader_main(CdSource *c)
{
	for (;;) {
		CdSource::LoadJob j;
		{
			std::unique_lock<std::mutex> lk(c->lq_mx);
			c->lq_cv.wait(lk, [c] { return c->lq_stop || !c->lq.empty(); });
			if (c->lq_stop)
				return;
			j = c->lq.front();
			c->lq.pop_front();
		}
		{ /* superseded by a newer load for the same deck: skip the slow part */
			std::lock_guard<std::mutex> g(c->mx);
			if (c->decks[j.deck].gen != j.gen)
				continue;
		}
		const uint64_t t0 = os_gettime_ns();
		int attempts = 0;
		double verr = 0.0;
		obs_source_t *fresh = deck_create(j.deck, j.path, j.inpoint, j.speed, &attempts, &verr);
		const uint64_t t1 = os_gettime_ns();
		obs_source_t *old = nullptr;
		{
			std::lock_guard<std::mutex> g(c->mx);
			Deck &k = c->decks[j.deck];
			if (k.gen != j.gen || !fresh) {
				old = fresh; /* superseded meanwhile: discard */
			} else {
				old = k.src; /* not live/spare (it was a free deck), so nothing renders it */
				k.src = fresh;
				k.ready = true;
				k.load_ns = t1;
				const double ms = (t1 - t0) / 1e6;
				c->load_ms_sum += ms;
				c->load_ms_max = std::max(c->load_ms_max, ms);
				c->load_n++;
				c->verify_retries += attempts - 1;
				if (std::fabs(verr) >= 300.0) {
					c->verify_fail++;
					obs_log(LOG_WARNING,
						"deck %d: start point not confirmed after %d tries (%+.0f ms)", j.deck,
						attempts, verr);
				}
			}
		}
		deck_release(old);
	}
}

/* graphics thread (mx held): record the pick and queue the slow part */
void deck_load(CdSource *c, int d, const cd::Pick &p)
{
	Deck &k = c->decks[d];
	k.clip = p.clip;
	k.inpoint = p.inpoint;
	k.speed = p.speed;
	k.loops = p.loops;
	k.ready = false;
	k.gen++;
	{
		std::lock_guard<std::mutex> lk(c->lq_mx);
		c->lq.push_back({d, k.gen, c->lib.clips[p.clip].path, p.inpoint, p.speed, os_gettime_ns()});
	}
	c->lq_cv.notify_one();
}

void deck_pause(CdSource *c, int d)
{
	if (d >= 0)
		obs_source_media_play_pause(c->decks[d].src, true);
}

int free_deck(CdSource *c, int for_slot)
{
	if (c->slot_deck[for_slot] >= 0)
		return c->slot_deck[for_slot]; /* re-prepare in place */
	for (int i = 0; i < kDecks; ++i)
		if (i != c->live && i != c->spare && i != c->slot_deck[0] && i != c->slot_deck[1])
			return i;
	/* all busy: take the spare when nothing shows it */
	if (c->spare >= 0 && !c->tr_active && !c->show_prev) {
		const int d = c->spare;
		c->spare = -1;
		return d;
	}
	return -1;
}

const char *clip_name(CdSource *c, int clip)
{
	return (clip >= 0 && clip < (int)c->lib.names.size()) ? c->lib.names[clip].c_str() : "?";
}

/* ------------------------------------------------------------------------- */
/* scheduler events                                                           */

void do_cut(CdSource *c, const cd::SchedEvent &e)
{
	const int d = c->slot_deck[e.slot];
	if (d < 0) {
		obs_log(LOG_WARNING, "'%s': cut at beat %.2f but slot %d is empty", obs_source_get_name(c->self),
			e.at_beat, e.slot);
		return;
	}
	c->slot_deck[e.slot] = -1;
	if (c->spare >= 0 && c->spare != d)
		deck_pause(c, c->spare);
	c->spare = c->live;
	c->live = d;
	const double media_t = obs_source_media_get_time(c->decks[d].src) / 1000.0; /* before un-pausing */
	obs_source_media_play_pause(c->decks[d].src, false);
	c->decks[d].lv.reset(c->lib.clips[c->decks[d].clip], c->cfg.lc);
	const bool fade = e.transition != cd::Transition::Cut && c->spare >= 0 && e.transition_beats > 0.0;
	c->tr_active = fade;
	c->tr_type = e.transition;
	c->tr_start = e.at_beat;
	c->tr_len = e.transition_beats;
	c->show_prev = false;
	c->cuts++;
	const double err_ms = (c->beats - e.at_beat) * 60000.0 / std::max(1.0, c->bpm);
	c->cut_err_sum += std::fabs(err_ms);
	c->cut_err_max = std::max(c->cut_err_max, std::fabs(err_ms));
	c->cut_err_n++;
	const double ready_s = (os_gettime_ns() - c->decks[d].load_ns) / 1e9;
	obs_log(LOG_INFO,
		"'%s': CUT #%llu slot %d deck %d '%s' target beat %.3f frame beat %.3f err %+.1f ms %s %.2f "
		"(ready %.2fs before) in %.1fs (media at %.2fs) x%.2f%s%s inv %d",
		obs_source_get_name(c->self), (unsigned long long)c->cuts, e.slot, d, clip_name(c, c->decks[d].clip),
		e.at_beat, c->beats, err_ms, tr_name(e.transition), e.transition_beats, ready_s, c->decks[d].inpoint,
		media_t, c->decks[d].speed, c->decks[d].loops ? " loops" : "",
		c->deferred ? " LATE-PREROLL (deck was still loading)" : "", (int)c->decks[d].lv.invert());
}

void on_event(CdSource *c, const cd::SchedEvent &e)
{
	using T = cd::SchedEvent::Type;
	switch (e.type) {
	case T::Prepare: {
		const cd::Pick p = c->picker->next(e.section, e.hold_s);
		if (p.clip < 0) {
			obs_log(LOG_WARNING, "'%s': nothing playable for %s (%s)", obs_source_get_name(c->self),
				sec_name(e.section), p.reason.c_str());
			return;
		}
		const int d = free_deck(c, e.slot);
		if (d < 0) {
			obs_log(LOG_WARNING, "'%s': no free deck for slot %d", obs_source_get_name(c->self), e.slot);
			return;
		}
		deck_load(c, d, p);
		c->slot_deck[e.slot] = d;
		obs_log(LOG_INFO, "'%s': prepare slot %d deck %d '%s' (%s, hold %.1fs) %s",
			obs_source_get_name(c->self), e.slot, d, clip_name(c, p.clip), sec_name(e.section), e.hold_s,
			p.reason.c_str());
		break;
	}
	case T::Cut: {
		const int d = c->slot_deck[e.slot];
		if (d < 0) {
			obs_log(LOG_WARNING, "'%s': cut at beat %.2f but slot %d is empty",
				obs_source_get_name(c->self), e.at_beat, e.slot);
			return;
		}
		if (!c->decks[d].ready) {
			c->deferred = true;
			c->deferred_ev = e;
			return;
		}
		do_cut(c, e);
		break;
	}
	case T::StutterOn:
		obs_log(LOG_INFO, "'%s': stutter on at beat %.2f", obs_source_get_name(c->self), e.at_beat);
		break;
	case T::StutterFlip:
		c->show_prev = e.show_prev && c->spare >= 0;
		break;
	case T::StutterOff:
		c->show_prev = false;
		obs_log(LOG_INFO, "'%s': stutter off at beat %.2f", obs_source_get_name(c->self), e.at_beat);
		break;
	}
}

/* ------------------------------------------------------------------------- */
/* clock                                                                      */

void read_clock(CdSource *c, float seconds, cd::ClockSample &out)
{
	pd_clock k;
	bool ok = false;
	const std::string &sel = c->cfg.clock;
	if (sel != kClockFree) {
		if (sel.empty()) {
			ok = pd_clock_read(0, obs_source_get_name(c->self), nullptr, &k);
			if (!ok)
				ok = pd_clock_read(0, nullptr, nullptr, &k);
		} else {
			const size_t bar = sel.find('|');
			const std::string src = sel.substr(0, bar);
			const std::string flt = bar == std::string::npos ? "" : sel.substr(bar + 1);
			ok = pd_clock_read(0, src.c_str(), flt.c_str(), &k);
		}
	}
	if (ok) {
		c->bpm = k.bpm > 1.0 ? k.bpm : c->cfg.fallback_bpm;
		c->beats = k.beats_now + c->cfg.sync_ms / 1000.0 * c->bpm / 60.0;
		c->section = (cd::Section)std::clamp(k.section, 0, 2);
		out.drop_cycle = k.drop_cycle;
		out.drop_cycle_steps_per_beat = k.drop_cycle_steps_per_beat;
		out.phrase_beats = k.phrase_beats;
		out.paused = k.paused;
		c->free_beats = c->beats;
	} else {
		c->bpm = std::max(30.0, c->cfg.fallback_bpm);
		c->free_beats += seconds * c->bpm / 60.0;
		c->beats = c->free_beats;
		c->section = cd::Section::Groove;
	}
	if (ok != c->clock_was_ok)
		obs_log(LOG_INFO, "'%s': clock %s", obs_source_get_name(c->self),
			ok ? "following Palette Director" : "free-running (Palette Director clock not available)");
	c->clock_was_ok = ok;
	c->clock_ok = ok;
	out.valid = true;
	/* half a frame of lookahead: a cut fires on the frame nearest its beat, not the first one after */
	out.beats = c->beats + 0.5 * std::clamp((double)seconds, 0.0, 0.05) * c->bpm / 60.0;
	out.bpm = c->bpm;
	out.section = c->section;
}

/* ------------------------------------------------------------------------- */
/* source callbacks                                                           */

const char *cd_name(void *)
{
	return obs_module_text("ClipDirector");
}

void apply(CdSource *c, obs_data_t *settings, bool force_reload)
{
	Cfg n = read_cfg(settings);
	const bool reload = force_reload || !c->picker || n.folders_key != c->cfg.folders_key;
	cdobs::Library lib;
	if (reload)
		lib = cdobs::load_library(n.folders);
	std::lock_guard<std::mutex> g(c->mx);
	c->cfg = n;
	if (reload) {
		c->lib = std::move(lib);
		for (const auto &line : c->lib.notes)
			obs_log(LOG_INFO, "'%s': %s", obs_source_get_name(c->self), line.c_str());
		obs_log(LOG_INFO, "'%s': library %zu clips (%d with ingest stats)", obs_source_get_name(c->self),
			c->lib.clips.size(), c->lib.with_stats);
		c->picker = std::make_unique<cd::ClipPicker>(c->lib.folders, c->lib.clips, c->cfg.pk);
		for (int s = 0; s < 2; ++s) /* prepared decks refer to the old library */
			c->slot_deck[s] = -1;
		c->sched = std::make_unique<cd::CutScheduler>(c->cfg.sc);
		if (c->live >= 0 && c->decks[c->live].clip >= (int)c->lib.clips.size())
			c->live = -1;
		if (c->spare >= 0 && c->decks[c->spare].clip >= (int)c->lib.clips.size())
			c->spare = -1;
	} else {
		c->picker->set_config(c->cfg.pk);
		c->sched->set_config(c->cfg.sc);
	}
}

void cd_update(void *data, obs_data_t *settings)
{
	apply(static_cast<CdSource *>(data), settings, false);
}

void *cd_create(obs_data_t *settings, obs_source_t *source)
{
	CdSource *c = new CdSource();
	c->self = source;
	c->loader = std::thread(loader_main, c);
	char *path = obs_module_file("effects/cd-composite.effect");
	obs_enter_graphics();
	char *err = nullptr;
	c->effect = path ? gs_effect_create_from_file(path, &err) : nullptr;
	obs_leave_graphics();
	if (!c->effect)
		obs_log(LOG_ERROR, "could not load effect %s: %s", path ? path : "(null)", err ? err : "unknown error");
	bfree(err);
	bfree(path);
	apply(c, settings, true);
	return c;
}

void cd_destroy(void *data)
{
	CdSource *c = static_cast<CdSource *>(data);
	{
		std::lock_guard<std::mutex> lk(c->lq_mx);
		c->lq_stop = true;
	}
	c->lq_cv.notify_all();
	if (c->loader.joinable())
		c->loader.join();
	for (auto &d : c->decks)
		deck_release(d.src);
	obs_enter_graphics();
	for (auto &d : c->decks)
		gs_texrender_destroy(d.tr);
	gs_effect_destroy(c->effect);
	obs_leave_graphics();
	if (c->cut_err_n)
		obs_log(LOG_INFO, "'%s': %llu cuts, frame error mean %.1f ms max %.1f ms", obs_source_get_name(c->self),
			(unsigned long long)c->cuts, c->cut_err_sum / c->cut_err_n, c->cut_err_max);
	delete c;
}

uint32_t cd_width(void *)
{
	return kW;
}

uint32_t cd_height(void *)
{
	return kH;
}

void cd_tick(void *data, float seconds)
{
	CdSource *c = static_cast<CdSource *>(data);
	std::lock_guard<std::mutex> g(c->mx);
	if (!c->sched || !c->picker)
		return;
	cd::ClockSample clk;
	read_clock(c, seconds, clk);
	for (const auto &e : c->sched->tick(clk))
		on_event(c, e);
	if (c->deferred) {
		const int d = c->slot_deck[c->deferred_ev.slot];
		if (d < 0) {
			c->deferred = false;
		} else if (c->decks[d].ready) {
			do_cut(c, c->deferred_ev);
			c->deferred = false;
		}
	}
	if (c->tr_active && c->beats >= c->tr_start + c->tr_len)
		c->tr_active = false;
	c->stat_t += seconds;
	if (c->stat_t >= 10.0 && c->cut_err_n) {
		c->stat_t = 0.0;
		obs_log(LOG_INFO,
			"'%s': status %s bpm %.1f beat %.2f %s, %llu cuts, frame error mean %.1f ms max %.1f ms, "
			"deck loads %d mean %.0f ms max %.0f ms (off the graphics thread), start-point retries %d, unconfirmed %d",
			obs_source_get_name(c->self), c->clock_ok ? "PD" : "free", c->bpm, c->beats,
			sec_name(c->section), (unsigned long long)c->cuts, c->cut_err_sum / c->cut_err_n,
			c->cut_err_max, c->load_n, c->load_n ? c->load_ms_sum / c->load_n : 0.0, c->load_ms_max,
			c->verify_retries, c->verify_fail);
	}
}

/* draw a deck into its texrender, cover-fitted to kW x kH */
gs_texture_t *deck_texture(CdSource *c, int d)
{
	Deck &k = c->decks[d];
	if (!k.src)
		return nullptr;
	if (!k.tr)
		k.tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	const uint32_t w = obs_source_get_width(k.src), h = obs_source_get_height(k.src);
	gs_texrender_reset(k.tr);
	if (!w || !h)
		return nullptr;
	if (gs_texrender_begin(k.tr, kW, kH)) {
		struct vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)kW, 0.0f, (float)kH, -100.0f, 100.0f);
		const float sc = std::max((float)kW / w, (float)kH / h);
		gs_matrix_push();
		gs_matrix_translate3f((kW - w * sc) * 0.5f, (kH - h * sc) * 0.5f, 0.0f);
		gs_matrix_scale3f(sc, sc, 1.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		obs_source_video_render(k.src);
		gs_blend_state_pop();
		gs_matrix_pop();
		gs_texrender_end(k.tr);
	}
	return gs_texrender_get_texture(k.tr);
}

void set_levels(gs_effect_t *e, const char *lv, const char *inv, const cd::LevelParams &p)
{
	struct vec4 v;
	vec4_set(&v, (float)p.lo, (float)p.scale, (float)p.gamma, (float)p.strength);
	gs_effect_set_vec4(gs_effect_get_param_by_name(e, lv), &v);
	gs_effect_set_float(gs_effect_get_param_by_name(e, inv), p.invert ? 1.0f : 0.0f);
}

void cd_render(void *data, gs_effect_t *)
{
	CdSource *c = static_cast<CdSource *>(data);
	std::lock_guard<std::mutex> g(c->mx);
	if (!c->effect || c->live < 0)
		return;

	int a = c->live, b = c->live;
	float mix = 0.0f, dip = 0.0f;
	if (c->tr_active && c->spare >= 0) {
		const double p = std::clamp((c->beats - c->tr_start) / std::max(0.01, c->tr_len), 0.0, 1.0);
		if (c->tr_type == cd::Transition::Crossfade) {
			a = c->spare;
			b = c->live;
			mix = (float)p;
		} else { /* dip: out to black on the old clip, back in on the new one */
			a = b = p < 0.5 ? c->spare : c->live;
			dip = (float)(p < 0.5 ? p * 2.0 : (1.0 - p) * 2.0);
		}
	} else if (c->show_prev && c->spare >= 0) {
		a = b = c->spare;
	}

	const bool prev_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_texture_t *ta = deck_texture(c, a);
	gs_texture_t *tb = b == a ? ta : deck_texture(c, b);
	if (ta && tb) {
		gs_effect_t *e = c->effect;
		gs_effect_set_texture(gs_effect_get_param_by_name(e, "texA"), ta);
		gs_effect_set_texture(gs_effect_get_param_by_name(e, "texB"), tb);
		gs_effect_set_float(gs_effect_get_param_by_name(e, "mixB"), mix);
		gs_effect_set_float(gs_effect_get_param_by_name(e, "dip"), dip);
		set_levels(e, "lvA", "invA", c->decks[a].lv.params(c->cfg.lc));
		set_levels(e, "lvB", "invB", c->decks[b].lv.params(c->cfg.lc));
		gs_effect_set_float(gs_effect_get_param_by_name(e, "alphaMode"), (float)c->cfg.alpha_mode);
		struct vec2 ar;
		vec2_set(&ar, (float)c->cfg.alpha_lo, (float)c->cfg.alpha_hi);
		gs_effect_set_vec2(gs_effect_get_param_by_name(e, "alphaRange"), &ar);
		while (gs_effect_loop(e, "Draw"))
			gs_draw_sprite(ta, 0, kW, kH);
	}
	gs_enable_framebuffer_srgb(prev_srgb);
}

/* ------------------------------------------------------------------------- */
/* properties                                                                 */

bool on_skip(obs_properties_t *, obs_property_t *, void *data)
{
	CdSource *c = static_cast<CdSource *>(data);
	std::lock_guard<std::mutex> g(c->mx);
	if (c->sched)
		c->sched->skip();
	return false;
}

bool on_hold(obs_properties_t *, obs_property_t *, void *data)
{
	CdSource *c = static_cast<CdSource *>(data);
	std::lock_guard<std::mutex> g(c->mx);
	if (c->sched) {
		c->sched->set_hold(!c->sched->hold());
		obs_log(LOG_INFO, "'%s': hold %s", obs_source_get_name(c->self), c->sched->hold() ? "on" : "off");
	}
	return false;
}

bool on_rescan(obs_properties_t *, obs_property_t *, void *data)
{
	CdSource *c = static_cast<CdSource *>(data);
	obs_data_t *s = obs_source_get_settings(c->self);
	apply(c, s, true);
	obs_data_release(s);
	return false;
}

void add_clock_list(obs_property_t *p)
{
	obs_property_list_add_string(p, obs_module_text("Clock.Auto"), kClockAuto);
	proc_handler_t *ph = obs_get_proc_handler();
	calldata_t cd;
	calldata_init(&cd);
	if (ph && proc_handler_call(ph, PD_CLOCK_PROC_INSTANCES, &cd)) {
		obs_data_t *root = obs_data_create_from_json(
			(std::string("{\"a\":") + (calldata_string(&cd, "json") ? calldata_string(&cd, "json") : "[]") +
			 "}")
				.c_str());
		obs_data_array_t *arr = root ? obs_data_get_array(root, "a") : nullptr;
		for (size_t i = 0; arr && i < obs_data_array_count(arr); ++i) {
			obs_data_t *it = obs_data_array_item(arr, i);
			const std::string src = obs_data_get_string(it, "source");
			const std::string flt = obs_data_get_string(it, "filter");
			const std::string label = src + " / " + flt;
			const std::string value = src + "|" + flt;
			obs_property_list_add_string(p, label.c_str(), value.c_str());
			obs_data_release(it);
		}
		obs_data_array_release(arr);
		obs_data_release(root);
	}
	calldata_free(&cd);
	obs_property_list_add_string(p, obs_module_text("Clock.Free"), kClockFree);
}

void add_transition_list(obs_properties_t *g, const char *key, const char *label)
{
	obs_property_t *p = obs_properties_add_list(g, key, label, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Transition.Cut"), 0);
	obs_property_list_add_int(p, obs_module_text("Transition.Crossfade"), 1);
	obs_property_list_add_int(p, obs_module_text("Transition.Dip"), 2);
}

obs_properties_t *cd_properties(void *data)
{
	obs_properties_t *pr = obs_properties_create();
	obs_properties_add_editable_list(pr, "folders", obs_module_text("Folders"), OBS_EDITABLE_LIST_TYPE_FILES,
					 nullptr, nullptr);
	obs_properties_add_button2(pr, "rescan", obs_module_text("Rescan"), on_rescan, data);
	obs_properties_add_button2(pr, "skip", obs_module_text("Skip"), on_skip, data);
	obs_properties_add_button2(pr, "hold", obs_module_text("Hold"), on_hold, data);

	obs_properties_t *ck = obs_properties_create();
	add_clock_list(obs_properties_add_list(ck, "clock_source", obs_module_text("ClockSource"), OBS_COMBO_TYPE_LIST,
					       OBS_COMBO_FORMAT_STRING));
	obs_properties_add_int_slider(ck, "sync_offset_ms", obs_module_text("SyncOffset"), -200, 200, 1);
	obs_properties_add_float(ck, "fallback_bpm", obs_module_text("FallbackBpm"), 60.0, 200.0, 0.5);
	obs_properties_add_float(ck, "preroll_beats", obs_module_text("Preroll"), 0.5, 4.0, 0.5);
	obs_properties_add_group(pr, "g_clock", obs_module_text("Group.Clock"), OBS_GROUP_NORMAL, ck);

	obs_properties_t *cu = obs_properties_create();
	obs_properties_add_int(cu, "groove_hold_bars", obs_module_text("GrooveHold"), 1, 32, 1);
	add_transition_list(cu, "groove_transition", obs_module_text("GrooveTransition"));
	obs_properties_add_float(cu, "groove_transition_beats", obs_module_text("GrooveTransitionBeats"), 0.25, 16,
				 0.25);
	obs_properties_add_float_slider(cu, "groove_crossfade_chance", obs_module_text("GrooveCrossfadeChance"), 0, 1,
					0.05);
	obs_properties_add_int(cu, "breakdown_hold_bars", obs_module_text("BreakdownHold"), 1, 32, 1);
	add_transition_list(cu, "breakdown_transition", obs_module_text("BreakdownTransition"));
	obs_properties_add_float(cu, "breakdown_transition_beats", obs_module_text("BreakdownTransitionBeats"), 0.25,
				 16, 0.25);
	obs_properties_add_int(cu, "drop_hold_bars", obs_module_text("DropHold"), 1, 32, 1);
	add_transition_list(cu, "drop_transition", obs_module_text("DropTransition"));
	obs_properties_add_float(cu, "drop_transition_beats", obs_module_text("DropTransitionBeats"), 0.25, 16, 0.25);
	obs_properties_add_bool(cu, "stutter", obs_module_text("Stutter"));
	obs_properties_add_group(pr, "g_cuts", obs_module_text("Group.Cuts"), OBS_GROUP_NORMAL, cu);

	obs_properties_t *sh = obs_properties_create();
	obs_properties_add_int(sh, "seed", obs_module_text("Seed"), 0, 99999, 1);
	obs_properties_add_int_slider(sh, "speed_min_pct", obs_module_text("SpeedMin"), 50, 200, 5);
	obs_properties_add_int_slider(sh, "speed_max_pct", obs_module_text("SpeedMax"), 50, 200, 5);
	obs_properties_add_float_slider(sh, "speed_extreme_chance", obs_module_text("SpeedExtreme"), 0, 0.5, 0.01);
	obs_properties_add_group(pr, "g_shuffle", obs_module_text("Group.Shuffle"), OBS_GROUP_NORMAL, sh);

	obs_properties_t *lv = obs_properties_create();
	obs_properties_add_float_slider(lv, "normalize_strength", obs_module_text("Strength"), 0, 1, 0.05);
	obs_properties_add_float_slider(lv, "polarity_threshold", obs_module_text("PolarityThreshold"), 0.2, 1.0, 0.01);
	obs_properties_add_float_slider(lv, "target_mean", obs_module_text("TargetMean"), 0.05, 0.6, 0.01);
	obs_properties_add_float_slider(lv, "max_gain", obs_module_text("MaxGain"), 1, 6, 0.1);
	obs_properties_add_float_slider(lv, "noise_floor", obs_module_text("NoiseFloor"), 0, 0.1, 0.005);
	obs_properties_add_bool(lv, "adapt", obs_module_text("Adapt"));
	obs_properties_add_float(lv, "adapt_tau_s", obs_module_text("AdaptTau"), 0.5, 30, 0.5);
	obs_properties_add_group(pr, "normalize", obs_module_text("Group.Normalize"), OBS_GROUP_CHECKABLE, lv);

	obs_properties_t *out = obs_properties_create();
	obs_property_t *om = obs_properties_add_list(out, "output_mode", obs_module_text("OutputMode"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(om, obs_module_text("Output.Opaque"), 0);
	obs_property_list_add_int(om, obs_module_text("Output.LumaAlpha"), 1);
	obs_properties_add_float_slider(out, "alpha_lo", obs_module_text("AlphaLo"), 0, 0.5, 0.01);
	obs_properties_add_float_slider(out, "alpha_hi", obs_module_text("AlphaHi"), 0.01, 1, 0.01);
	obs_properties_add_group(pr, "g_output", obs_module_text("Group.Output"), OBS_GROUP_NORMAL, out);
	return pr;
}

} // namespace

extern "C" void clip_director_source_load(void)
{
	struct obs_source_info si = {};
	si.id = "clip_director_source";
	si.type = OBS_SOURCE_TYPE_INPUT;
	si.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	si.get_name = cd_name;
	si.create = cd_create;
	si.destroy = cd_destroy;
	si.update = cd_update;
	si.get_defaults = cd_defaults;
	si.get_properties = cd_properties;
	si.get_width = cd_width;
	si.get_height = cd_height;
	si.video_tick = cd_tick;
	si.video_render = cd_render;
	si.icon_type = OBS_ICON_TYPE_MEDIA;
	obs_register_source(&si);
}
