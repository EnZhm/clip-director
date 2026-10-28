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
#include <cstdarg>
#include <deque>
#include <map>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "pd-clock-api.h"
#include "obs/cd-api.h"
#include "obs/cd-library.h"
#include "levels.h"
#include "osc.h"
#include "osc-transport.h"
#include "picker.h"
#include "scene_pool.h"
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
	int pol = 0; /* polarity override the tracker was reset with */
};

struct ClipOverride {
	bool ban = false;
	int polarity = 0; /* cd::Polarity */
};

struct Cfg {
	std::vector<cd::Folder> folders;               /* with tag / weight / enabled from the settings */
	std::string folders_key;                       /* folder paths only: a change reloads the library */
	std::map<std::string, ClipOverride> overrides; /* clip path -> ban / polarity */
	std::string clock;                             // "" auto, kClockFree, or "source|filter"
	double sync_ms = 0.0, fallback_bpm = 128.0;
	cd::PickerConfig pk;
	cd::SchedulerConfig sc;
	cd::LevelConfig lc;
	int alpha_mode = 1;
	double alpha_lo = 0.02, alpha_hi = 0.20;

	/* Synesthesia over OSC (W2/W3): syn_* keys, like folder_meta */
	bool syn_enabled = false;
	std::string syn_host = "127.0.0.1";
	int syn_send_port = 7000;
	int syn_feedback_port = 7001;
	std::string syn_pools_json;
	cd::ScenePools syn_pools;
	bool syn_on_breakdown = true;
	bool syn_on_drop = true;
	bool syn_bang_on_drop = false;
	int syn_groove_phrases = 0;
	double syn_lead_ms = 0.0;
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
	bool stutter_on = false;

	double beats = 0.0, bpm = 128.0;
	bool clock_ok = false, clock_was_ok = false;
	double free_beats = 0.0;
	cd::Section section = cd::Section::Groove;

	gs_effect_t *effect = nullptr;

	/* live level measurement (P5): live deck -> 480x270 -> 120x68 box downsample -> staged
	 * readback (two surfaces, mapped >= 3 frames later so the GPU never stalls) */
	gs_effect_t *measure_fx = nullptr;
	gs_texrender_t *m1 = nullptr, *m2 = nullptr;
	gs_stagesurf_t *stage[2] = {nullptr, nullptr};
	bool stage_full[2] = {false, false};
	int stage_deck[2] = {-1, -1};
	uint64_t stage_gen[2] = {0, 0};
	uint64_t stage_frame[2] = {0, 0};
	uint64_t frame = 0;
	uint64_t last_obs_ns = 0;
	uint64_t meas_n = 0;
	double meas_ms_sum = 0.0, meas_ms_max = 0.0;

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

	/* Synesthesia over OSC (W2/W3) */
	std::unique_ptr<cdnet::OscTransport> syn;
	cd::SceneShuffle syn_shuf[3]; /* one shuffle bag per pool (Groove / Breakdown / Drop) */
	bool syn_hold = false;        /* Hold scene: suspends the automatic launches */
	std::string syn_now, syn_now_preset;
	uint64_t syn_send_ns = 0, syn_echo_ns = 0;
	std::vector<std::string> syn_heard; /* scene names Synesthesia echoed, newest first */
	std::deque<std::string> syn_log;    /* every OSC message with beat position and reason */
	uint64_t syn_log_seq = 0;

	/* dock event log */
	std::deque<std::string> log;
	uint64_t log_seq = 0;
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

/* a folder named like a section plays in that section (until the dock sets a tag) */
cd::Tag auto_tag(const std::string &path)
{
	std::string p = path;
	while (!p.empty() && (p.back() == '/' || p.back() == '\\'))
		p.pop_back();
	const size_t cut = p.find_last_of("/\\");
	std::string leaf = cut == std::string::npos ? p : p.substr(cut + 1);
	std::transform(leaf.begin(), leaf.end(), leaf.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
	if (leaf.find("breakdown") != std::string::npos)
		return cd::Tag::Breakdown;
	if (leaf.find("drop") != std::string::npos)
		return cd::Tag::Drop;
	if (leaf.find("groove") != std::string::npos)
		return cd::Tag::Groove;
	return cd::Tag::Any;
}

Cfg read_cfg(obs_data_t *s)
{
	Cfg c;
	/* per-folder tag / weight, keyed by path (the editable list only holds paths) */
	std::map<std::string, std::pair<int, double>> meta;
	obs_data_array_t *fm = obs_data_get_array(s, "folder_meta");
	for (size_t i = 0, n = obs_data_array_count(fm); i < n; ++i) {
		obs_data_t *it = obs_data_array_item(fm, i);
		meta[obs_data_get_string(it, "path")] = {(int)obs_data_get_int(it, "tag"),
							 obs_data_get_double(it, "weight")};
		obs_data_release(it);
	}
	obs_data_array_release(fm);

	obs_data_array_t *arr = obs_data_get_array(s, "folders");
	const size_t n = obs_data_array_count(arr);
	for (size_t i = 0; i < n; ++i) {
		obs_data_t *it = obs_data_array_item(arr, i);
		const char *v = obs_data_get_string(it, "value");
		if (v && *v) {
			cd::Folder f;
			f.path = v;
			f.enabled = !obs_data_has_user_value(it, "hidden") || !obs_data_get_bool(it, "hidden");
			const auto m = meta.find(f.path);
			if (m != meta.end()) {
				f.tag = (cd::Tag)std::clamp(m->second.first, 0, 3);
				f.weight = std::clamp(m->second.second, 0.0, 10.0);
			} else {
				f.tag = auto_tag(f.path);
			}
			c.folders.push_back(f);
			c.folders_key += f.path + ";";
		}
		obs_data_release(it);
	}
	obs_data_array_release(arr);

	obs_data_array_t *ov = obs_data_get_array(s, "clip_overrides");
	for (size_t i = 0, m = obs_data_array_count(ov); i < m; ++i) {
		obs_data_t *it = obs_data_array_item(ov, i);
		ClipOverride o;
		o.ban = obs_data_get_bool(it, "ban");
		o.polarity = std::clamp((int)obs_data_get_int(it, "polarity"), 0, 2);
		c.overrides[obs_data_get_string(it, "path")] = o;
		obs_data_release(it);
	}
	obs_data_array_release(ov);

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

	c.syn_enabled = obs_data_get_bool(s, "syn_enabled");
	const std::string host = obs_data_get_string(s, "syn_host");
	if (!host.empty())
		c.syn_host = host;
	c.syn_send_port = std::clamp((int)obs_data_get_int(s, "syn_send_port"), 1, 65535);
	c.syn_feedback_port = std::clamp((int)obs_data_get_int(s, "syn_feedback_port"), 1, 65535);
	c.syn_pools_json = obs_data_get_string(s, "syn_pools");
	if (!c.syn_pools_json.empty() && !cd::scene_pools_from_json(c.syn_pools_json, c.syn_pools)) {
		obs_log(LOG_WARNING, "syn_pools is not readable JSON; the scene pools stay empty");
		c.syn_pools = cd::ScenePools();
	}
	c.syn_on_breakdown = obs_data_get_bool(s, "syn_on_breakdown");
	c.syn_on_drop = obs_data_get_bool(s, "syn_on_drop");
	c.syn_bang_on_drop = obs_data_get_bool(s, "syn_bang_on_drop");
	c.syn_groove_phrases = std::clamp((int)obs_data_get_int(s, "syn_groove_phrases"), 0, 4);
	c.syn_lead_ms = std::clamp(obs_data_get_double(s, "syn_lead_ms"), 0.0, 500.0);
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
	/* Synesthesia (the only receiver) treats alpha as black, so black RGB is what reads as
	 * transparent there: opaque output is the default; luma-to-alpha stays for other receivers */
	obs_data_set_default_int(s, "output_mode", 0);
	obs_data_set_default_double(s, "alpha_lo", 0.02);
	obs_data_set_default_double(s, "alpha_hi", 0.20);

	/* Synesthesia over OSC: ports must match Settings > OSC in Synesthesia (7000/7001 are
	 * the defaults this dock ships with; Synesthesia's own ports are user-set) */
	obs_data_set_default_bool(s, "syn_enabled", false);
	obs_data_set_default_string(s, "syn_host", "127.0.0.1");
	obs_data_set_default_int(s, "syn_send_port", 7000);
	obs_data_set_default_int(s, "syn_feedback_port", 7001);
	obs_data_set_default_string(s, "syn_pools", "");
	obs_data_set_default_bool(s, "syn_on_breakdown", true);
	obs_data_set_default_bool(s, "syn_on_drop", true);
	obs_data_set_default_bool(s, "syn_bang_on_drop", false);
	obs_data_set_default_int(s, "syn_groove_phrases", 0);
	obs_data_set_default_double(s, "syn_lead_ms", 0.0);
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

/* mx held: one line for the dock log (and the OBS log) */
void note(CdSource *c, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	obs_log(LOG_INFO, "'%s': %s", obs_source_get_name(c->self), buf);
	char ts[16];
	const double b = c->beats;
	snprintf(ts, sizeof(ts), "%7.2f  ", b);
	c->log.push_back(std::string(ts) + buf);
	if (c->log.size() > 200)
		c->log.pop_front();
	c->log_seq++;
}

/* ------------------------------------------------------------------------- */
/* Synesthesia over OSC (W2/W3)                                              */

/* mx held: one line for the OSC log in the dock (beat position + reason) */
void syn_note(CdSource *c, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	char ts[16];
	snprintf(ts, sizeof(ts), "%7.2f  ", c->beats);
	c->syn_log.push_back(std::string(ts) + buf);
	if (c->syn_log.size() > 200)
		c->syn_log.pop_front();
	c->syn_log_seq++;
}

/* "tunnel \"acid\"" for the log: what the message carries */
std::string osc_args_text(const cd::OscMessage &m)
{
	std::string s;
	for (const cd::OscArg &a : m.args) {
		s += " ";
		if (a.type == cd::OscArg::Type::String)
			s += "\"" + a.s + "\"";
		else if (a.type == cd::OscArg::Type::Float) {
			char buf[32];
			snprintf(buf, sizeof(buf), "%.2f", a.f);
			s += buf;
		} else
			s += std::to_string(a.i);
	}
	return s;
}

/* mx held: encode, queue, and log with the beat the message is aimed at */
bool syn_send(CdSource *c, const cd::OscMessage &m, const char *why, double at_beat, double send_at_beat)
{
	std::vector<uint8_t> wire;
	if (!cd::osc_encode(m, wire))
		return false;
	const bool queued = c->syn && c->syn->send(wire.data(), wire.size());
	if (queued)
		c->syn_send_ns = os_gettime_ns();
	char lead[40] = "";
	if (send_at_beat < at_beat - 1e-9)
		snprintf(lead, sizeof(lead), " (%+.0f ms early)",
			 (at_beat - send_at_beat) * 60000.0 / std::max(1.0, c->bpm));
	const std::string tag = queued ? ("  [" + std::string(why) + "]") : std::string("  [NOT SENT]");
	syn_note(c, "-> %s%s%s%s", m.address.c_str(), osc_args_text(m).c_str(), lead, tag.c_str());
	return queued;
}

/* mx held: draw a scene from one pool's shuffle bag and send it */
void syn_launch_pool(CdSource *c, cd::Section pool, const char *why, double at_beat, double send_at_beat, bool bang)
{
	const int idx = std::clamp(static_cast<int>(pool), 0, 2);
	cd::SceneEntry entry;
	if (!c->syn_shuf[idx].next(entry)) {
		syn_note(c, "-- %s pool is empty, nothing to launch [%s]", sec_name(pool), why);
		return;
	}
	const cd::OscMessage m = cd::osc_scene(entry.scene, entry.preset);
	if (syn_send(c, m, why, at_beat, send_at_beat)) {
		c->syn_now = entry.scene;
		c->syn_now_preset = entry.preset;
		note(c, "OSC scene '%s' at beat %.2f (%s)", entry.label().c_str(), at_beat, why);
	}
	if (bang)
		syn_send(c, cd::osc_bang(1), "scene bang 1", at_beat, send_at_beat);
}

/* mx held: everything Synesthesia said back on the feedback port */
void syn_drain(CdSource *c)
{
	std::vector<uint8_t> wire;
	for (int budget = 0; budget < 16 && c->syn && c->syn->pop(wire); ++budget) {
		cd::OscMessage m;
		if (!cd::osc_decode(wire, m))
			continue;
		c->syn_echo_ns = os_gettime_ns();
		if (m.address.rfind("/scenes/", 0) == 0 && m.address.size() > 8) {
			const std::string name = m.address.substr(8);
			syn_note(c, "<- %s%s", m.address.c_str(), osc_args_text(m).c_str());
			c->syn_now = name; /* Synesthesia is the authority on what is live */
			c->syn_now_preset.clear();
			if (std::find(c->syn_heard.begin(), c->syn_heard.end(), name) == c->syn_heard.end()) {
				c->syn_heard.insert(c->syn_heard.begin(), name);
				if (c->syn_heard.size() > 64)
					c->syn_heard.pop_back();
			}
		} else {
			syn_note(c, "<- %s%s", m.address.c_str(), osc_args_text(m).c_str());
		}
	}
}

bool syn_linked(const CdSource *c)
{
	return c->syn_echo_ns && os_gettime_ns() - c->syn_echo_ns < 10ull * 1000000000ull;
}

/* mx held: folder tags/weights/enabled and clip overrides onto the loaded library */
void apply_meta(CdSource *c)
{
	for (size_t i = 0; i < c->lib.folders.size() && i < c->cfg.folders.size(); ++i)
		c->lib.folders[i] = c->cfg.folders[i];
	for (auto &clip : c->lib.clips) {
		const auto o = c->cfg.overrides.find(clip.path);
		clip.banned = o != c->cfg.overrides.end() && o->second.ban;
		clip.polarity = o != c->cfg.overrides.end() ? (cd::Polarity)o->second.polarity : cd::Polarity::Auto;
	}
	/* a polarity override on a clip that is showing applies at once */
	for (int d : {c->live, c->spare}) {
		if (d < 0 || c->decks[d].clip < 0 || c->decks[d].clip >= (int)c->lib.clips.size())
			continue;
		const cd::Clip &clip = c->lib.clips[c->decks[d].clip];
		if ((int)clip.polarity != c->decks[d].pol) {
			c->decks[d].lv.reset(clip, c->cfg.lc);
			c->decks[d].pol = (int)clip.polarity;
		}
	}
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
	c->decks[d].pol = (int)c->lib.clips[c->decks[d].clip].polarity;
	c->last_obs_ns = 0;
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
	note(c, "%s%s '%s' from %.1fs x%.2f%s%s", e.slot == cd::CutScheduler::kDrop ? "DROP " : "",
	     tr_name(e.transition), clip_name(c, c->decks[d].clip), c->decks[d].inpoint, c->decks[d].speed,
	     c->decks[d].lv.invert() ? " inverted" : "", c->deferred ? " (late: deck was loading)" : "");
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
			note(c, "nothing playable for %s", sec_name(e.section));
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
		c->stutter_on = true;
		note(c, "stutter on (drop colour cycle)");
		break;
	case T::StutterFlip:
		c->show_prev = e.show_prev && c->spare >= 0;
		break;
	case T::StutterOff:
		c->show_prev = false;
		c->stutter_on = false;
		note(c, "stutter off");
		break;
	case T::SceneLaunch:
		/* W3: the launch the scheduler planned on the beat of this transition */
		syn_launch_pool(c, e.section, e.reason.c_str(), e.at_beat, e.send_at_beat, e.bang);
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

	/* the OSC transport is switched on/off by the settings; the old one is stopped outside
	 * the lock, so turning Synesthesia off never holds up the graphics thread for long */
	std::unique_ptr<cdnet::OscTransport> recycle;
	{
		std::lock_guard<std::mutex> g(c->mx);
		const bool link_changed = !c->syn || n.syn_host != c->cfg.syn_host ||
					  n.syn_send_port != c->cfg.syn_send_port ||
					  n.syn_feedback_port != c->cfg.syn_feedback_port;
		if (n.syn_enabled && link_changed) {
			auto fresh = std::make_unique<cdnet::OscTransport>();
			std::string err;
			if (fresh->start(n.syn_host, n.syn_send_port, n.syn_feedback_port, &err)) {
				recycle = std::move(c->syn);
				c->syn = std::move(fresh);
				note(c, "OSC %s:%d -> feedback port %d", n.syn_host.c_str(), n.syn_send_port,
				     n.syn_feedback_port);
			} else {
				note(c, "OSC transport did not start: %s", err.c_str());
			}
		} else if (!n.syn_enabled && c->syn) {
			recycle = std::move(c->syn);
			note(c, "OSC transport stopped");
		}
		if (n.syn_pools_json != c->cfg.syn_pools_json) {
			for (int i = 0; i < 3; ++i)
				c->syn_shuf[i].set_entries(n.syn_pools.of(static_cast<cd::Section>(i)));
		}
	}
	recycle.reset();

	std::lock_guard<std::mutex> g(c->mx);
	c->cfg = n;
	c->cfg.sc.scene.auto_on = n.syn_enabled; /* Synesthesia off: the scheduler plans no launches */
	if (reload) {
		c->lib = std::move(lib);
		apply_meta(c);
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
		apply_meta(c);
		c->picker->set_library(c->lib.folders, c->lib.clips);
		c->picker->set_config(c->cfg.pk);
		c->sched->set_config(c->cfg.sc);
	}
	if (c->sched)
		c->sched->set_scene_hold(c->syn_hold); /* a reload replaces the scheduler: keep Hold */
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
	char *mpath = obs_module_file("effects/cd-measure.effect");
	obs_enter_graphics();
	char *err = nullptr;
	c->effect = path ? gs_effect_create_from_file(path, &err) : nullptr;
	char *merr = nullptr;
	c->measure_fx = mpath ? gs_effect_create_from_file(mpath, &merr) : nullptr;
	obs_leave_graphics();
	if (!c->measure_fx)
		obs_log(LOG_ERROR, "could not load effect %s: %s", mpath ? mpath : "(null)",
			merr ? merr : "unknown error");
	bfree(merr);
	bfree(mpath);
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
	c->syn.reset(); /* joins the OSC send/receive threads before anything else is freed */
	for (auto &d : c->decks)
		deck_release(d.src);
	obs_enter_graphics();
	for (auto &d : c->decks)
		gs_texrender_destroy(d.tr);
	gs_effect_destroy(c->effect);
	gs_effect_destroy(c->measure_fx);
	gs_texrender_destroy(c->m1);
	gs_texrender_destroy(c->m2);
	gs_stagesurface_destroy(c->stage[0]);
	gs_stagesurface_destroy(c->stage[1]);
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
	syn_drain(c); /* Synesthesia's OSC output: the link signal, and the scene names to import */
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
		cd::LevelParams lp;
		if (c->live >= 0)
			lp = c->decks[c->live].lv.params(c->cfg.lc);
		obs_log(LOG_INFO,
			"'%s': status %s bpm %.1f beat %.2f %s, %llu cuts, frame error mean %.1f ms max %.1f ms, "
			"deck loads %d mean %.0f ms max %.0f ms, start-point retries %d unconfirmed %d | levels: "
			"%llu samples, measure mean %.3f ms max %.3f ms, live lo %.3f scale %.2f gamma %.2f str %.2f inv %d",
			obs_source_get_name(c->self), c->clock_ok ? "PD" : "free", c->bpm, c->beats,
			sec_name(c->section), (unsigned long long)c->cuts, c->cut_err_sum / c->cut_err_n,
			c->cut_err_max, c->load_n, c->load_n ? c->load_ms_sum / c->load_n : 0.0, c->load_ms_max,
			c->verify_retries, c->verify_fail, (unsigned long long)c->meas_n,
			c->meas_n ? c->meas_ms_sum / c->meas_n : 0.0, c->meas_ms_max, lp.lo, lp.scale, lp.gamma,
			lp.strength, (int)lp.invert);
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

/* one box-downsample pass into `tr` (w x h) */
bool box_pass(CdSource *c, gs_texrender_t *tr, gs_texture_t *src, uint32_t w, uint32_t h)
{
	const uint32_t sw = gs_texture_get_width(src), sh = gs_texture_get_height(src);
	gs_texrender_reset(tr);
	if (!gs_texrender_begin(tr, w, h))
		return false;
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	struct vec2 tx;
	vec2_set(&tx, 1.0f / (float)sw, 1.0f / (float)sh);
	gs_effect_set_texture(gs_effect_get_param_by_name(c->measure_fx, "image"), src);
	gs_effect_set_vec2(gs_effect_get_param_by_name(c->measure_fx, "texel"), &tx);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	while (gs_effect_loop(c->measure_fx, "Box4"))
		gs_draw_sprite(src, 0, w, h);
	gs_blend_state_pop();
	gs_texrender_end(tr);
	return true;
}

/* luma percentiles of a mapped RGBA8 surface */
cd::ClipStats stats_from(const uint8_t *data, uint32_t linesize, uint32_t w, uint32_t h)
{
	uint32_t hist[256] = {0};
	double sum = 0.0;
	for (uint32_t y = 0; y < h; ++y) {
		const uint8_t *row = data + (size_t)y * linesize;
		for (uint32_t x = 0; x < w; ++x) {
			const uint8_t *p = row + x * 4;
			const double l = 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
			const int b = std::clamp((int)std::lround(l), 0, 255);
			hist[b]++;
			sum += l;
		}
	}
	const uint32_t n = w * h;
	auto pct = [&](double q) {
		const uint32_t target = (uint32_t)(q * (n - 1));
		uint32_t acc = 0;
		for (int i = 0; i < 256; ++i) {
			acc += hist[i];
			if (acc > target)
				return i / 255.0;
		}
		return 1.0;
	};
	cd::ClipStats st;
	st.has = true;
	st.mean = sum / n / 255.0;
	st.p02 = pct(0.02);
	st.p50 = pct(0.50);
	st.p98 = pct(0.98);
	return st;
}

constexpr uint32_t kM1W = 480, kM1H = 270, kM2W = 120, kM2H = 68;

/* graphics thread, mx held, framebuffer sRGB off. `live_tex` = this frame's live deck texture. */
void measure_step(CdSource *c, gs_texture_t *live_tex)
{
	if (!c->measure_fx)
		return;
	const uint64_t t0 = os_gettime_ns();
	c->frame++;
	bool worked = false;
	/* 1. collect staged samples that are old enough not to stall */
	for (int k = 0; k < 2; ++k) {
		if (!c->stage_full[k] || c->frame - c->stage_frame[k] < 3)
			continue;
		uint8_t *data = nullptr;
		uint32_t ls = 0;
		if (gs_stagesurface_map(c->stage[k], &data, &ls)) {
			const cd::ClipStats st = stats_from(data, ls, kM2W, kM2H);
			gs_stagesurface_unmap(c->stage[k]);
			const int d = c->stage_deck[k];
			if (d >= 0 && d == c->live && c->decks[d].gen == c->stage_gen[k]) {
				const uint64_t now = os_gettime_ns();
				const double dt = c->last_obs_ns ? std::min(1.0, (now - c->last_obs_ns) / 1e9) : 0.1;
				c->last_obs_ns = now;
				c->decks[d].lv.observe(st, dt, c->cfg.lc);
				c->meas_n++;
			}
		}
		c->stage_full[k] = false;
		worked = true;
	}
	/* 2. every 6th frame (10 Hz at 60 fps): downsample the live deck and stage it */
	if (live_tex && c->live >= 0 && c->frame % 6 == 0) {
		int k = !c->stage_full[0] ? 0 : (!c->stage_full[1] ? 1 : -1);
		if (k >= 0) {
			if (!c->m1)
				c->m1 = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
			if (!c->m2)
				c->m2 = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
			if (!c->stage[k])
				c->stage[k] = gs_stagesurface_create(kM2W, kM2H, GS_RGBA);
			if (c->stage[k] && box_pass(c, c->m1, live_tex, kM1W, kM1H) &&
			    box_pass(c, c->m2, gs_texrender_get_texture(c->m1), kM2W, kM2H)) {
				gs_stage_texture(c->stage[k], gs_texrender_get_texture(c->m2));
				c->stage_full[k] = true;
				c->stage_deck[k] = c->live;
				c->stage_gen[k] = c->decks[c->live].gen;
				c->stage_frame[k] = c->frame;
				worked = true;
			}
		}
	}
	if (worked) {
		const double ms = (os_gettime_ns() - t0) / 1e6;
		c->meas_ms_sum += ms;
		c->meas_ms_max = std::max(c->meas_ms_max, ms);
	}
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
		measure_step(c, a == c->live ? ta : (b == c->live ? tb : nullptr));
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

	/* Synesthesia over OSC: the dock is the main control surface, this is the same settings */
	obs_properties_t *syn = obs_properties_create();
	obs_properties_add_bool(syn, "syn_enabled", obs_module_text("Syn.Enabled"));
	obs_properties_add_text(syn, "syn_host", obs_module_text("Syn.Host"), OBS_TEXT_DEFAULT);
	obs_properties_add_int(syn, "syn_send_port", obs_module_text("Syn.SendPort"), 1, 65535, 1);
	obs_properties_add_int(syn, "syn_feedback_port", obs_module_text("Syn.FeedbackPort"), 1, 65535, 1);
	obs_properties_add_bool(syn, "syn_on_breakdown", obs_module_text("Syn.OnBreakdown"));
	obs_properties_add_bool(syn, "syn_on_drop", obs_module_text("Syn.OnDrop"));
	obs_property_t *gp = obs_properties_add_list(syn, "syn_groove_phrases", obs_module_text("Syn.DuringGroove"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(gp, obs_module_text("Syn.Never"), 0);
	obs_property_list_add_int(gp, obs_module_text("Syn.Every2Phrases"), 2);
	obs_property_list_add_int(gp, obs_module_text("Syn.Every4Phrases"), 4);
	obs_properties_add_bool(syn, "syn_bang_on_drop", obs_module_text("Syn.BangOnDrop"));
	obs_properties_add_int_slider(syn, "syn_lead_ms", obs_module_text("Syn.Lead"), 0, 500, 10);
	obs_properties_add_text(syn, "syn_pools", obs_module_text("Syn.Pools"), OBS_TEXT_MULTILINE);
	obs_properties_add_group(pr, "g_syn", obs_module_text("Group.Synesthesia"), OBS_GROUP_CHECKABLE, syn);
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

/* ------------------------------------------------------------------------- */
/* dock API (cd-api.h)                                                        */

namespace {

CdSource *cd_of(obs_source_t *src)
{
	if (!cdapi::is_clip_director(src))
		return nullptr;
	return static_cast<CdSource *>(obs_obj_get_data(src));
}

void clip_info(CdSource *c, int d, cdapi::ClipInfo &o)
{
	if (d < 0 || c->decks[d].clip < 0 || c->decks[d].clip >= (int)c->lib.clips.size())
		return;
	const Deck &k = c->decks[d];
	const cd::Clip &clip = c->lib.clips[k.clip];
	o.valid = true;
	o.path = clip.path;
	o.name = c->lib.names[k.clip];
	o.folder = clip.folder >= 0 && clip.folder < (int)c->lib.folders.size() ? c->lib.folders[clip.folder].path : "";
	o.thumb = k.clip < (int)c->lib.thumbs.size() ? c->lib.thumbs[k.clip] : "";
	o.inpoint = k.inpoint;
	o.speed = k.speed;
	o.invert = k.lv.invert();
	o.loops = k.loops;
	o.ready = k.ready;
	o.polarity = (int)clip.polarity;
}

/* edit one entry of an object array in the source settings (creating the array if needed) */
template<typename F> void edit_array(obs_data_t *settings, const char *key, const std::string &path, F &&fn)
{
	obs_data_array_t *arr = obs_data_get_array(settings, key);
	if (!arr) {
		arr = obs_data_array_create();
		obs_data_set_array(settings, key, arr);
	}
	obs_data_t *hit = nullptr;
	for (size_t i = 0, n = obs_data_array_count(arr); i < n && !hit; ++i) {
		obs_data_t *it = obs_data_array_item(arr, i);
		if (path == obs_data_get_string(it, "path"))
			hit = it;
		else
			obs_data_release(it);
	}
	if (!hit) {
		hit = obs_data_create();
		obs_data_set_string(hit, "path", path.c_str());
		fn(hit);
		obs_data_array_push_back(arr, hit);
	} else {
		fn(hit);
	}
	obs_data_release(hit);
	obs_data_array_release(arr);
}

} // namespace

namespace cdapi {

bool is_clip_director(obs_source_t *src)
{
	const char *id = src ? obs_source_get_unversioned_id(src) : nullptr;
	return id && strcmp(id, "clip_director_source") == 0;
}

bool status(obs_source_t *src, Status &o)
{
	CdSource *c = cd_of(src);
	if (!c)
		return false;
	std::lock_guard<std::mutex> g(c->mx);
	o = Status();
	o.clock_pd = c->clock_ok;
	o.bpm = c->bpm;
	o.beats = c->beats;
	o.section = (int)c->section;
	o.hold = c->sched && c->sched->hold();
	o.next_cut_beat = c->sched ? c->sched->next_cut_beat() : 0.0;
	o.stutter = c->stutter_on;
	clip_info(c, c->live, o.live);
	clip_info(c, c->slot_deck[cd::CutScheduler::kNext], o.next);
	clip_info(c, c->slot_deck[cd::CutScheduler::kDrop], o.drop);
	std::vector<int> counts(c->lib.folders.size(), 0);
	for (const auto &clip : c->lib.clips) {
		if (clip.folder >= 0 && clip.folder < (int)counts.size())
			counts[clip.folder]++;
		o.banned += clip.banned ? 1 : 0;
	}
	for (size_t i = 0; i < c->lib.folders.size(); ++i) {
		const cd::Folder &f = c->lib.folders[i];
		FolderInfo fi;
		fi.path = f.path;
		fi.tag = (int)f.tag;
		fi.weight = f.weight;
		fi.enabled = f.enabled;
		fi.clips = counts[i];
		for (const auto &note_line : c->lib.notes)
			if (note_line.rfind("folder " + std::to_string(i) + ":", 0) == 0)
				fi.has_index = note_line.find("ingest index") != std::string::npos;
		o.folders.push_back(fi);
	}
	o.clips = c->lib.clips.size();
	o.with_stats = c->lib.with_stats;
	o.cuts = c->cuts;
	o.cut_err_mean_ms = c->cut_err_n ? c->cut_err_sum / c->cut_err_n : 0.0;
	o.log_seq = c->log_seq;
	o.log.assign(c->log.begin(), c->log.end());

	/* Synesthesia over OSC */
	cdapi::SynInfo &sy = o.syn;
	sy.enabled = c->cfg.syn_enabled;
	sy.running = c->syn != nullptr;
	sy.linked = syn_linked(c);
	sy.host = c->cfg.syn_host;
	sy.send_port = c->cfg.syn_send_port;
	sy.feedback_port = c->cfg.syn_feedback_port;
	sy.auto_on = c->cfg.syn_enabled;
	sy.on_breakdown = c->cfg.syn_on_breakdown;
	sy.on_drop = c->cfg.syn_on_drop;
	sy.bang_on_drop = c->cfg.syn_bang_on_drop;
	sy.groove_phrases = c->cfg.syn_groove_phrases;
	sy.lead_ms = c->cfg.syn_lead_ms;
	sy.hold = c->syn_hold;
	sy.now_scene = c->syn_now;
	sy.now_preset = c->syn_now_preset;
	sy.next_scene_beat = c->sched ? c->sched->next_scene_beat() : 0.0;
	{
		const int sec = std::clamp((int)c->section, 0, 2);
		cd::SceneEntry nxt;
		bool have = c->syn_shuf[sec].peek(nxt);
		for (int i = 0; i < 3 && !have; ++i) { /* the current pool can be empty */
			if (i != sec)
				have = c->syn_shuf[i].peek(nxt);
		}
		if (have) {
			sy.next_scene = nxt.scene;
			sy.next_preset = nxt.preset;
		}
	}
	sy.sent = c->syn ? c->syn->sent() : 0;
	sy.received = c->syn ? c->syn->received() : 0;
	if (c->syn_send_ns)
		sy.since_send_ms = (int)((os_gettime_ns() - c->syn_send_ns) / 1000000ull);
	if (c->syn_echo_ns)
		sy.since_echo_ms = (int)((os_gettime_ns() - c->syn_echo_ns) / 1000000ull);
	sy.pools_json = c->cfg.syn_pools_json;
	sy.heard = c->syn_heard;
	sy.log_seq = c->syn_log_seq;
	sy.log.assign(c->syn_log.begin(), c->syn_log.end());
	return true;
}

void skip(obs_source_t *src)
{
	CdSource *c = cd_of(src);
	if (!c)
		return;
	std::lock_guard<std::mutex> g(c->mx);
	if (c->sched) {
		c->sched->skip();
		note(c, "skip (dock)");
	}
}

void set_hold(obs_source_t *src, bool hold)
{
	CdSource *c = cd_of(src);
	if (!c)
		return;
	std::lock_guard<std::mutex> g(c->mx);
	if (c->sched && c->sched->hold() != hold) {
		c->sched->set_hold(hold);
		note(c, "hold %s", hold ? "on" : "off");
	}
}

void ban_live(obs_source_t *src)
{
	CdSource *c = cd_of(src);
	if (!c)
		return;
	std::string path, name;
	{
		std::lock_guard<std::mutex> g(c->mx);
		if (c->live < 0 || c->decks[c->live].clip < 0)
			return;
		path = c->lib.clips[c->decks[c->live].clip].path;
		name = c->lib.names[c->decks[c->live].clip];
		note(c, "banned '%s'", name.c_str());
		if (c->sched && !c->sched->hold())
			c->sched->skip();
	}
	obs_data_t *st = obs_source_get_settings(src);
	edit_array(st, "clip_overrides", path, [](obs_data_t *e) { obs_data_set_bool(e, "ban", true); });
	obs_source_update(src, nullptr);
	obs_data_release(st);
}

void set_polarity(obs_source_t *src, const std::string &clip_path, int polarity)
{
	CdSource *c = cd_of(src);
	if (!c || clip_path.empty())
		return;
	obs_data_t *st = obs_source_get_settings(src);
	edit_array(st, "clip_overrides", clip_path,
		   [&](obs_data_t *e) { obs_data_set_int(e, "polarity", std::clamp(polarity, 0, 2)); });
	obs_source_update(src, nullptr);
	obs_data_release(st);
	std::lock_guard<std::mutex> g(c->mx);
	note(c, "polarity %s for '%s'", polarity == 1 ? "always invert" : (polarity == 2 ? "never invert" : "auto"),
	     clip_path.substr(clip_path.find_last_of("/\\") + 1).c_str());
}

void set_folder(obs_source_t *src, int index, int tag, double weight, bool enabled)
{
	CdSource *c = cd_of(src);
	if (!c)
		return;
	obs_data_t *st = obs_source_get_settings(src);
	obs_data_array_t *arr = obs_data_get_array(st, "folders");
	std::string path;
	/* `index` counts non-empty entries, like read_cfg */
	int k = 0;
	for (size_t i = 0, n = obs_data_array_count(arr); i < n; ++i) {
		obs_data_t *it = obs_data_array_item(arr, i);
		const char *v = obs_data_get_string(it, "value");
		if (v && *v) {
			if (k == index) {
				path = v;
				obs_data_set_bool(it, "hidden", !enabled);
			}
			k++;
		}
		obs_data_release(it);
	}
	obs_data_array_release(arr);
	if (!path.empty()) {
		edit_array(st, "folder_meta", path, [&](obs_data_t *e) {
			obs_data_set_int(e, "tag", std::clamp(tag, 0, 3));
			obs_data_set_double(e, "weight", std::clamp(weight, 0.0, 10.0));
		});
		obs_source_update(src, nullptr);
	}
	obs_data_release(st);
}

/* ---- Synesthesia over OSC (W2/W3) ------------------------------------------------------- */

namespace {

/* write one or more settings keys and let the source pick them up (apply() restarts the
 * transport when the link changed, and rebuilds a pool's shuffle bag when its entries did) */
template<typename F> void syn_update(obs_source_t *src, F &&fn)
{
	obs_data_t *st = obs_source_get_settings(src);
	fn(st);
	obs_source_update(src, nullptr);
	obs_data_release(st);
}

} // namespace

void syn_set_enabled(obs_source_t *src, bool enabled)
{
	syn_update(src, [enabled](obs_data_t *s) { obs_data_set_bool(s, "syn_enabled", enabled); });
	CdSource *c = cd_of(src);
	if (!c)
		return;
	std::lock_guard<std::mutex> g(c->mx);
	if (c->sched)
		c->sched->set_scene_hold(c->syn_hold);
	note(c, "Synesthesia control %s", enabled ? "on" : "off");
}

void syn_set_link(obs_source_t *src, const std::string &host, int send_port, int feedback_port)
{
	syn_update(src, [&](obs_data_t *s) {
		obs_data_set_string(s, "syn_host", host.c_str());
		obs_data_set_int(s, "syn_send_port", std::clamp(send_port, 1, 65535));
		obs_data_set_int(s, "syn_feedback_port", std::clamp(feedback_port, 1, 65535));
	});
}

void syn_set_trigger(obs_source_t *src, bool on_breakdown, bool on_drop, int groove_phrases, double lead_ms,
		     bool bang_on_drop)
{
	syn_update(src, [&](obs_data_t *s) {
		obs_data_set_bool(s, "syn_on_breakdown", on_breakdown);
		obs_data_set_bool(s, "syn_on_drop", on_drop);
		obs_data_set_int(s, "syn_groove_phrases", std::clamp(groove_phrases, 0, 4));
		obs_data_set_double(s, "syn_lead_ms", std::clamp(lead_ms, 0.0, 500.0));
		obs_data_set_bool(s, "syn_bang_on_drop", bang_on_drop);
	});
}

void syn_set_pools(obs_source_t *src, const std::string &pools_json)
{
	syn_update(src, [&](obs_data_t *s) { obs_data_set_string(s, "syn_pools", pools_json.c_str()); });
}

void syn_next(obs_source_t *src)
{
	CdSource *c = cd_of(src);
	if (!c)
		return;
	std::lock_guard<std::mutex> g(c->mx);
	if (!c->syn)
		return;
	syn_launch_pool(c, c->section, "manual: next scene", c->beats, c->beats, false);
}

void syn_hold(obs_source_t *src, bool hold)
{
	CdSource *c = cd_of(src);
	if (!c)
		return;
	std::lock_guard<std::mutex> g(c->mx);
	c->syn_hold = hold;
	if (c->sched)
		c->sched->set_scene_hold(hold);
	syn_note(c, "-- Hold scene %s", hold ? "on" : "off");
	note(c, "hold scene %s (dock)", hold ? "on" : "off");
}

void syn_hold_toggle(obs_source_t *src)
{
	CdSource *c = cd_of(src);
	if (!c)
		return;
	std::lock_guard<std::mutex> g(c->mx);
	c->syn_hold = !c->syn_hold;
	if (c->sched)
		c->sched->set_scene_hold(c->syn_hold);
	syn_note(c, "-- Hold scene %s", c->syn_hold ? "on" : "off");
	note(c, "hold scene %s (hotkey)", c->syn_hold ? "on" : "off");
}

void syn_bang(obs_source_t *src)
{
	CdSource *c = cd_of(src);
	if (!c)
		return;
	std::lock_guard<std::mutex> g(c->mx);
	if (!c->syn)
		return;
	syn_send(c, cd::osc_bang(1), "manual: fire bang 1", c->beats, c->beats);
}

void syn_launch(obs_source_t *src, int pool, int index)
{
	CdSource *c = cd_of(src);
	if (!c)
		return;
	std::lock_guard<std::mutex> g(c->mx);
	if (!c->syn)
		return;
	const int p = std::clamp(pool, 0, 2);
	const std::vector<cd::SceneEntry> &entries = c->cfg.syn_pools.of(static_cast<cd::Section>(p));
	if (index < 0 || index >= (int)entries.size())
		return;
	const cd::SceneEntry &en = entries[(size_t)index];
	syn_send(c, cd::osc_scene(en.scene, en.preset), "manual: pool slot", c->beats, c->beats);
	c->syn_now = en.scene;
	c->syn_now_preset = en.preset;
}

} // namespace cdapi
