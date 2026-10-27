#include "levels.h"

#include <algorithm>
#include <cmath>

namespace cd {

bool decide_invert(Polarity p, const ClipStats &stats, double threshold, bool *decided)
{
	*decided = true;
	if (p == Polarity::Always)
		return true;
	if (p == Polarity::Never)
		return false;
	if (!stats.has) {
		*decided = false;
		return false;
	}
	return stats.p50 > threshold;
}

ClipStats inverted(const ClipStats &s)
{
	ClipStats o = s;
	o.mean = 1.0 - s.mean;
	o.p50 = 1.0 - s.p50;
	o.p02 = 1.0 - s.p98;
	o.p98 = 1.0 - s.p02;
	o.dark = 1.0 - s.dark; // approximate: "dark" becomes "bright" fraction
	return o;
}

LevelParams compute_params(double lo, double hi, double mean, bool invert, const LevelConfig &cfg)
{
	LevelParams p;
	p.invert = invert; // polarity applies even when normalisation is off
	const double max_gain = std::max(1.0, cfg.max_gain);
	const double lo_eff = std::clamp(lo + std::max(0.0, cfg.noise_floor), 0.0, 0.9);
	const double hi_eff = std::max(hi, lo_eff + 1.0 / max_gain);
	p.lo = lo_eff;
	p.scale = 1.0 / (hi_eff - lo_eff);
	const double m = std::clamp((mean - lo_eff) * p.scale, 0.01, 0.99);
	const double target = std::clamp(cfg.target_mean, 0.02, 0.98);
	p.gamma = std::clamp(std::log(target) / std::log(m), 0.5, 2.0);
	p.strength = cfg.enabled ? std::clamp(cfg.strength, 0.0, 1.0) : 0.0;
	return p;
}

void LevelTracker::reset(const Clip &clip, const LevelConfig &cfg)
{
	polarity_ = clip.polarity;
	invert_ = decide_invert(polarity_, clip.stats, cfg.polarity_threshold, &decided_);
	have_levels_ = clip.stats.has;
	if (have_levels_) {
		const ClipStats s = invert_ ? inverted(clip.stats) : clip.stats;
		lo_ = s.p02;
		hi_ = s.p98;
		mean_ = s.mean;
	} else {
		lo_ = 0.0;
		hi_ = 1.0;
		mean_ = cfg.target_mean;
	}
}

void LevelTracker::observe(const ClipStats &measured, double dt, const LevelConfig &cfg)
{
	if (!measured.has)
		return;
	if (!decided_) // no ingest stats: decide polarity once, from the first frame we see
		invert_ = decide_invert(polarity_, measured, cfg.polarity_threshold, &decided_);
	const ClipStats s = invert_ ? inverted(measured) : measured;
	if (!have_levels_) {
		lo_ = s.p02;
		hi_ = s.p98;
		mean_ = s.mean;
		have_levels_ = true;
		return;
	}
	if (!cfg.adapt)
		return;
	const double a = 1.0 - std::exp(-std::max(0.0, dt) / std::max(0.05, cfg.adapt_tau_s));
	lo_ += a * (s.p02 - lo_);
	hi_ += a * (s.p98 - hi_);
	mean_ += a * (s.mean - mean_);
}

LevelParams LevelTracker::params(const LevelConfig &cfg) const
{
	return compute_params(lo_, hi_, mean_, invert_, cfg);
}

} // namespace cd
