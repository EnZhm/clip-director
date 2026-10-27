/*
Palette Director clock API (v1)  --  public, read-only, for other OBS plugins and scripts.

Palette Director registers two procedures on the GLOBAL proc handler (obs_get_proc_handler())
when its module loads. They are plain calldata calls, so plugins (C/C++) and OBS scripts
(Lua/Python: obs.proc_handler_call) can use them. Fields are versioned by name: new fields may be
added in later versions; existing ones keep their meaning. Check `api_version` >= 1.

  void palette_director_instances(out string json, out int api_version)
      json: [{"id":1,"source":"Spout In","filter":"Palette Lock"}, ...]

  void palette_director_clock(in int id, in string source, in string filter,
                              out bool ok, out int api_version, out int id, ...fields below)
      Selector (all optional): id > 0, else source (parent source name, optionally + filter),
      else filter name, else the oldest instance. A selector that matches nothing gives ok=false.

      out float beats           beat position at sample_ts_ns (absolute, fractional)
      out float beats_now       beats extrapolated to the call time (capped at +/-0.5 s;
                                frozen while paused)
      out float bpm             tempo the beat grid runs at (fallback BPM while silent)
      out float confidence      0..1
      out int   beat            integer beat
      out int   phrase          phrase counter
      out int   phrase_beats    16 | 32
      out int   section         PD_CLOCK_SECTION_*
      out int   kicks           monotonic kick counter (change = a kick)
      out bool  drop_cycle      a drop colour cycle is running
      out int   drop_cycle_steps_per_beat   1 | 2 | 4
      out bool  silent          feed silent (fallback BPM in use)
      out bool  paused          Director paused or PANIC latched
      out bool  enabled         Director enabled in the set
      out bool  filter_enabled  the Palette Lock filter itself is enabled in OBS
      out int   sample_ts_ns    os_gettime_ns() time `beats` refers to: the OBS audio timestamp of
                                the last analysed sample, mapped onto the wall clock with a
                                sliding-minimum offset (removes bursty audio delivery), or plain
                                wall time when there is no feed (see ts_source)
      out int   ts_source       1 = from audio timestamps, 0 = wall clock (no feed / silence)
      out int   now_ns          os_gettime_ns() at the call (beats_now refers to this)

The snapshot is refreshed by Palette Director's worker thread about every 5 ms. The call takes
two short mutexes and never blocks on audio or rendering; it is safe from any thread except
inside a Palette Lock filter's own callbacks. Call it from video_tick, not per pixel.
*/

#pragma once

#include <string.h>
#include <obs.h>
#include <callback/calldata.h>
#include <callback/proc.h>

#define PD_CLOCK_API_VERSION 1
#define PD_CLOCK_PROC_CLOCK "palette_director_clock"
#define PD_CLOCK_PROC_INSTANCES "palette_director_instances"

enum pd_clock_section {
	PD_CLOCK_SECTION_GROOVE = 0,
	PD_CLOCK_SECTION_BREAKDOWN = 1,
	PD_CLOCK_SECTION_DROP = 2,
};

/* Consumer-side convenience. This struct is NOT shared with Palette Director (it is filled
 * from named calldata fields), so it can change freely without breaking anyone. */
struct pd_clock {
	bool ok;
	int api_version;
	long long id;
	double beats, beats_now, bpm, confidence;
	long long beat, phrase, kicks, sample_ts_ns, now_ns;
	int phrase_beats, section, drop_cycle_steps_per_beat, ts_source;
	bool drop_cycle, silent, paused, enabled, filter_enabled;
};

/* false if Palette Director is not loaded, too old, or the instance is not found.
 * id > 0 wins; else source/filter names (either may be NULL or ""). */
static inline bool pd_clock_read(long long id, const char *source, const char *filter, struct pd_clock *out)
{
	proc_handler_t *ph = obs_get_proc_handler();
	calldata_t cd;
	bool called;
	memset(out, 0, sizeof(*out));
	if (!ph)
		return false;
	calldata_init(&cd);
	calldata_set_int(&cd, "id", id);
	calldata_set_string(&cd, "source", source ? source : "");
	calldata_set_string(&cd, "filter", filter ? filter : "");
	called = proc_handler_call(ph, PD_CLOCK_PROC_CLOCK, &cd);
	if (called) {
		out->ok = calldata_bool(&cd, "ok");
		out->api_version = (int)calldata_int(&cd, "api_version");
		out->id = calldata_int(&cd, "id");
		out->beats = calldata_float(&cd, "beats");
		out->beats_now = calldata_float(&cd, "beats_now");
		out->bpm = calldata_float(&cd, "bpm");
		out->confidence = calldata_float(&cd, "confidence");
		out->beat = calldata_int(&cd, "beat");
		out->phrase = calldata_int(&cd, "phrase");
		out->phrase_beats = (int)calldata_int(&cd, "phrase_beats");
		out->section = (int)calldata_int(&cd, "section");
		out->kicks = calldata_int(&cd, "kicks");
		out->drop_cycle = calldata_bool(&cd, "drop_cycle");
		out->drop_cycle_steps_per_beat = (int)calldata_int(&cd, "drop_cycle_steps_per_beat");
		out->silent = calldata_bool(&cd, "silent");
		out->paused = calldata_bool(&cd, "paused");
		out->enabled = calldata_bool(&cd, "enabled");
		out->filter_enabled = calldata_bool(&cd, "filter_enabled");
		out->sample_ts_ns = calldata_int(&cd, "sample_ts_ns");
		out->ts_source = (int)calldata_int(&cd, "ts_source");
		out->now_ns = calldata_int(&cd, "now_ns");
	}
	calldata_free(&cd);
	return called && out->ok && out->api_version >= PD_CLOCK_API_VERSION;
}
