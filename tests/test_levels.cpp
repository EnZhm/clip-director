#include "doctest.h"

#include <algorithm>
#include <cmath>

#include "levels.h"

using namespace cd;

namespace {

Clip clip_with(double mean, double p02, double p50, double p98, Polarity pol = Polarity::Auto)
{
	Clip c;
	c.duration = 60;
	c.stats.has = true;
	c.stats.mean = mean;
	c.stats.p02 = p02;
	c.stats.p50 = p50;
	c.stats.p98 = p98;
	c.polarity = pol;
	return c;
}

// The shader's per-pixel maths, for checking parameter effects end to end.
double apply(const LevelParams &p, double x)
{
	const double y = p.invert ? 1.0 - x : x;
	const double n = std::pow(std::clamp((y - p.lo) * p.scale, 0.0, 1.0), p.gamma);
	return y + (n - y) * p.strength;
}

} // namespace

TEST_CASE("polarity from real cyber stats: bright firegrid inverts, dark clips do not")
{
	const LevelConfig cfg;
	LevelTracker t;
	t.reset(clip_with(0.83, 0.30, 0.96, 1.00), cfg); // firegrid.mp4
	CHECK(t.invert());
	t.reset(clip_with(0.21, 0.01, 0.13, 0.73), cfg); // library median clip
	CHECK(!t.invert());
	t.reset(clip_with(0.21, 0.01, 0.13, 0.73, Polarity::Always), cfg);
	CHECK(t.invert());
	t.reset(clip_with(0.83, 0.30, 0.96, 1.00, Polarity::Never), cfg);
	CHECK(!t.invert());
}

TEST_CASE("gain cap: sparse scan-line overlay is not blown up; dim clip is lifted")
{
	LevelConfig cfg;
	cfg.strength = 1.0;
	// "3D Scan Lines" overlay: mean 0.01, p98 0.06
	const LevelParams s = compute_params(0.0, 0.06, 0.01, false, cfg);
	CHECK(s.scale <= cfg.max_gain + 1e-9);
	CHECK(apply(s, 0.06) < 0.5);   // its brightest lines stay far from white at the 3x cap
	CHECK(apply(s, 0.015) == 0.0); // noise floor: near-black compression noise -> black
	// a dim but real clip (p98 0.45): stretched so its highlights reach near full range
	const LevelParams d = compute_params(0.01, 0.45, 0.12, false, cfg);
	CHECK(apply(d, 0.45) > 0.95);
	CHECK(d.gamma >= 0.5);
	CHECK(d.gamma <= 2.0);
	// mean lands on target (before strength blending)
	const double m = std::pow(std::clamp((0.12 - d.lo) * d.scale, 0.0, 1.0), d.gamma);
	CHECK(m == doctest::Approx(cfg.target_mean).epsilon(0.01));
}

TEST_CASE("normalise off keeps polarity; full-range clip at target is near identity")
{
	LevelConfig cfg;
	cfg.enabled = false;
	const LevelParams p = compute_params(0.0, 1.0, 0.22, true, cfg);
	CHECK(p.invert);
	CHECK(p.strength == 0.0);
	CHECK(apply(p, 0.8) == doctest::Approx(0.2));
	cfg.enabled = true;
	cfg.strength = 1.0;
	cfg.noise_floor = 0.0;
	const LevelParams q = compute_params(0.0, 1.0, 0.22, false, cfg);
	CHECK(q.scale == doctest::Approx(1.0));
	CHECK(q.gamma == doctest::Approx(1.0));
}

TEST_CASE("tracker resets to the new clip's baseline at a cut; adapts slowly within a clip")
{
	LevelConfig cfg;
	LevelTracker t;
	t.reset(clip_with(0.20, 0.00, 0.10, 0.50), cfg);
	const LevelParams a0 = t.params(cfg);
	ClipStats bright = clip_with(0.6, 0.2, 0.6, 1.0).stats; // the clip brightens mid-way
	t.observe(bright, 1.0, cfg);                            // 1 s with tau 4 s: moves ~22 %, not all the way
	const LevelParams a1 = t.params(cfg);
	CHECK(a1.lo > a0.lo);
	CHECK(a1.lo < 0.2 * 0.5 + cfg.noise_floor);
	CHECK(!t.invert()); // polarity never flips within a clip
	t.reset(clip_with(0.20, 0.00, 0.10, 0.50), cfg);
	const LevelParams b0 = t.params(cfg);
	CHECK(b0.lo == a0.lo);
	CHECK(b0.scale == a0.scale);
	CHECK(b0.gamma == a0.gamma);
	cfg.adapt = false;
	t.observe(bright, 10.0, cfg);
	CHECK(t.params(cfg).scale == b0.scale);
}

TEST_CASE("clip without ingest stats: polarity decided once from the first frame")
{
	LevelConfig cfg;
	Clip c;
	c.duration = 30;
	LevelTracker t;
	t.reset(c, cfg);
	CHECK(!t.polarity_decided());
	t.observe(clip_with(0.8, 0.3, 0.9, 1.0).stats, 0.016, cfg);
	CHECK(t.polarity_decided());
	CHECK(t.invert());
	t.observe(clip_with(0.1, 0.0, 0.05, 0.4).stats, 0.016, cfg); // later dark frames: no flip
	CHECK(t.invert());
}
