#pragma once
// Clip Director: in-process API between the source and the dock (same module).
// Every call takes the source by obs_source_t (the caller holds a strong reference) and is
// thread-safe. Edits are written into the source's settings, so they are saved with the scene
// collection and survive restarts.
#include <obs.h>

#include <cstdint>
#include <string>
#include <vector>

#include "scene_pool.h"

namespace cdapi {

struct FolderInfo {
	std::string path;
	int tag = 0; // cd::Tag: 0 Any, 1 Groove, 2 Breakdown, 3 Drop
	double weight = 1.0;
	bool enabled = true;
	int clips = 0;
	bool has_index = false; // ingest index found (stats + thumbnails)
};

struct ClipInfo {
	bool valid = false;
	std::string path, name, folder, thumb;
	double inpoint = 0.0, speed = 1.0;
	bool invert = false, loops = false, ready = false;
	int polarity = 0; // cd::Polarity override: 0 Auto, 1 Always, 2 Never
};

// Synesthesia over OSC (W2/W3).
struct SynInfo {
	bool enabled = false; // Synesthesia control switched on in the source settings
	bool running = false; // the OSC transport started (host + ports usable)
	bool linked = false;  // Synesthesia echoed a launch recently (the link chip goes green)
	std::string error;    // why the transport did not start, empty when it did
	std::string host;
	int send_port = 0, feedback_port = 0;
	bool auto_on = false;
	bool on_breakdown = true, on_drop = true, bang_on_drop = false;
	int groove_phrases = 0;
	double lead_ms = 0.0;
	bool hold = false;                   // Hold scene: auto launches suspended
	std::string now_scene, now_preset;   // the scene Synesthesia last confirmed (or we sent)
	std::string next_scene, next_preset; // what the current section's pool would launch next
	double next_scene_beat = 0.0;        // planned beat of the next groove launch, 0 = none
	uint64_t sent = 0, received = 0;
	int since_echo_ms = -1;         // ms since Synesthesia last said anything, -1 = never
	int since_send_ms = -1;         // ms since we last sent, -1 = never
	std::string pools_json;         // the pools as saved in the settings (syn_pools)
	std::vector<std::string> heard; // scene names heard from Synesthesia, newest first
	uint64_t log_seq = 0;
	std::vector<std::string> log; // every OSC message, oldest first (up to 200)
};

struct Status {
	bool clock_pd = false; // following Palette Director (else free-running)
	double bpm = 0.0, beats = 0.0, next_cut_beat = 0.0;
	int section = 0; // cd::Section
	bool hold = false, stutter = false;
	ClipInfo live, next, drop;
	std::vector<FolderInfo> folders;
	size_t clips = 0;
	int with_stats = 0, banned = 0;
	uint64_t cuts = 0;
	double cut_err_mean_ms = 0.0;
	uint64_t log_seq = 0;         // number of log lines ever written
	std::vector<std::string> log; // the newest lines (up to 200), oldest first
	SynInfo syn;
};

bool is_clip_director(obs_source_t *src);
bool status(obs_source_t *src, Status &out);
void skip(obs_source_t *src);
void set_hold(obs_source_t *src, bool hold);
void ban_live(obs_source_t *src); // ban the clip on screen, then skip
void set_polarity(obs_source_t *src, const std::string &clip_path, int polarity);
void set_folder(obs_source_t *src, int index, int tag, double weight, bool enabled);

/* Synesthesia: settings (saved) and manual controls (live). */
void syn_set_enabled(obs_source_t *src, bool enabled);
void syn_set_link(obs_source_t *src, const std::string &host, int send_port, int feedback_port);
void syn_set_trigger(obs_source_t *src, bool on_breakdown, bool on_drop, int groove_phrases, double lead_ms,
		     bool bang_on_drop);
void syn_set_pools(obs_source_t *src, const std::string &pools_json);
void syn_next(obs_source_t *src);                        // launch the next scene of the current pool
void syn_hold(obs_source_t *src, bool hold);             // suspend / resume the automatic launches
void syn_hold_toggle(obs_source_t *src);                 // the Hold-scene hotkey
void syn_bang(obs_source_t *src);                        // fire scene bang 1
void syn_launch(obs_source_t *src, int pool, int index); // launch one pool entry (double-click)

} // namespace cdapi
