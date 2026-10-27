#pragma once
// Clip Director core: level normalisation + polarity (spec 9, 10). Pure C++17.
//
// Goal: even out highs and lows across the set, not grading. Per clip:
//   1. polarity: invert when the clip is mostly bright (decided once per clip, never per frame)
//   2. levels:   map [lo, hi] -> [0, 1], gain capped at max_gain, a noise floor added to lo so
//                compression noise in near-black areas stays black (= transparent downstream)
//   3. gamma:    move the clip's mean toward target_mean, gamma clamped to [0.5, 2]
//   4. strength: blend with the untouched image
// lo/hi/mean start at the clip's ingest stats (baseline) and, if adapt is on, follow the live
// measurements with an EMA. The tracker is reset at every cut, so one clip's levels never drag
// across into the next.
#include "types.h"

namespace cd {

struct LevelConfig {
	bool enabled = true;
	double strength = 0.7;
	double target_mean = 0.22; // the cyber library's median frame mean is 0.21
	double max_gain = 3.0;
	double noise_floor = 0.02;
	bool adapt = true;
	double adapt_tau_s = 4.0;
	double polarity_threshold = 0.45; // invert when the median luma is above this
};

// What the shader needs (all in the source's encoded 0..1 range).
struct LevelParams {
	bool invert = false;
	double lo = 0.0;       // black point after inversion
	double scale = 1.0;    // 1 / (hi - lo), capped at max_gain
	double gamma = 1.0;    // applied after levels
	double strength = 0.0; // 0 = pass-through
};

// Polarity for a clip. `stats` may be live measurements when the clip has no ingest stats.
// Returns false in `*decided` when Auto has nothing to decide from yet.
bool decide_invert(Polarity p, const ClipStats &stats, double threshold, bool *decided);

// Stats as seen after inversion (p02 <-> p98 swap, everything mirrored).
ClipStats inverted(const ClipStats &s);

// Pure mapping from (already polarity-corrected) lo/hi/mean to shader parameters.
LevelParams compute_params(double lo, double hi, double mean, bool invert, const LevelConfig &cfg);

// Per-clip tracker: baseline at the cut, then optional live adaptation.
class LevelTracker {
public:
	// New clip on screen. Uses ingest stats as the baseline when present.
	void reset(const Clip &clip, const LevelConfig &cfg);
	// One live measurement of the source frame (before inversion), dt seconds since the last one.
	void observe(const ClipStats &measured, double dt, const LevelConfig &cfg);
	LevelParams params(const LevelConfig &cfg) const;

	bool invert() const { return invert_; }
	bool polarity_decided() const { return decided_; }

private:
	Polarity polarity_ = Polarity::Auto;
	bool invert_ = false;
	bool decided_ = false;
	bool have_levels_ = false;
	double lo_ = 0.0, hi_ = 1.0, mean_ = 0.22;
};

} // namespace cd
