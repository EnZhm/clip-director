#pragma once
// Clip Director: in-process API between the source and the dock (same module).
// Every call takes the source by obs_source_t (the caller holds a strong reference) and is
// thread-safe. Edits are written into the source's settings, so they are saved with the scene
// collection and survive restarts.
#include <obs.h>

#include <cstdint>
#include <string>
#include <vector>

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
};

bool is_clip_director(obs_source_t *src);
bool status(obs_source_t *src, Status &out);
void skip(obs_source_t *src);
void set_hold(obs_source_t *src, bool hold);
void ban_live(obs_source_t *src); // ban the clip on screen, then skip
void set_polarity(obs_source_t *src, const std::string &clip_path, int polarity);
void set_folder(obs_source_t *src, int index, int tag, double weight, bool enabled);

} // namespace cdapi
