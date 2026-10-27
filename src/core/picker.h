#pragma once
// Clip Director core: deep shuffle (spec 7). Pure C++17, deterministic for a given seed.
//
// Layers, in the order a pick applies them:
//   1. Section pool: folders tagged for the current section; if there are none, the Any
//      folders; if there are none either, every enabled folder. Groove additionally always
//      includes the Any folders (so tagging one Groove folder never shrinks groove to it).
//   2. Folder choice: weighted random over the pool's folders that still have playable clips,
//      with a run limit (no more than `max_folder_run` picks in a row from one folder while
//      another folder is available).
//   3. Clip choice: a shuffle bag per folder: no clip repeats until every clip of that folder
//      has played; a refilled bag never starts with the clip that ended the previous pass.
//   4. Start point: random, snapped to the clip's keyframe grid, avoiding the last
//      `inpoint_memory` start points used for that clip (within one hold length).
//   5. Speed: uniform in [speed_min, speed_max], with `speed_extreme_chance` of 0.5x or 2x,
//      quantised to 5 %.
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "rng.h"
#include "types.h"

namespace cd {

struct PickerConfig {
	uint32_t seed = 7;
	int max_folder_run = 1;
	int inpoint_memory = 4;
	double speed_min = 1.0, speed_max = 1.0; // 1.0/1.0 = speed variation off
	double speed_extreme_chance = 0.0;
	int max_loops = 2; // a clip whose content would loop more than this for the hold is skipped
};

struct Pick {
	int clip = -1; // index into the clip list, -1 = nothing playable
	double inpoint = 0.0;
	double speed = 1.0;
	bool loops = false; // the hold is longer than the remaining content: the deck must loop
	std::string reason;
};

class ClipPicker {
public:
	ClipPicker(const std::vector<Folder> &folders, const std::vector<Clip> &clips, const PickerConfig &cfg);

	// Next clip for `section`, to be shown for `hold_s` seconds of wall time.
	Pick next(Section section, double hold_s);

	// Library edits while running (ban, folder weight/tag/enable). Bags are rebuilt lazily.
	void set_library(const std::vector<Folder> &folders, const std::vector<Clip> &clips);
	void set_config(const PickerConfig &cfg); // keeps the RNG stream and history

	std::vector<int> pool_folders(Section section) const; // layer 1, for the dock and tests

private:
	bool playable(int clip, double hold_s, double speed) const;
	int draw_from_folder(int folder, double hold_s, double speed);
	double pick_speed();
	double pick_inpoint(int clip, double hold_s, double speed);

	std::vector<Folder> folders_;
	std::vector<Clip> clips_;
	PickerConfig cfg_;
	Mulberry32 rng_;

	std::map<int, std::vector<int>> bag_;        // folder -> remaining clips this pass (front = next)
	std::map<int, int> last_of_pass_;            // folder -> clip that ended the previous pass
	std::map<int, std::deque<double>> inpoints_; // clip -> recent start points
	int last_folder_ = -1;
	int folder_run_ = 0;
};

} // namespace cd
