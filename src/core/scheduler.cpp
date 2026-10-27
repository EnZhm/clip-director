#include "scheduler.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace cd {

namespace {

const char *sec_name(Section s)
{
	return s == Section::Breakdown ? "breakdown" : (s == Section::Drop ? "drop" : "groove");
}

const char *tr_name(Transition t)
{
	return t == Transition::Crossfade ? "crossfade" : (t == Transition::Dip ? "dip" : "cut");
}

const char *type_name(SchedEvent::Type t)
{
	switch (t) {
	case SchedEvent::Type::Prepare:
		return "prepare";
	case SchedEvent::Type::Cut:
		return "cut";
	case SchedEvent::Type::StutterOn:
		return "stutter-on";
	case SchedEvent::Type::StutterFlip:
		return "stutter-flip";
	default:
		return "stutter-off";
	}
}

} // namespace

CutScheduler::CutScheduler(const SchedulerConfig &cfg) : cfg_(cfg), rng_(cfg.seed ^ 0x9E3779B9u) {}

void CutScheduler::set_config(const SchedulerConfig &cfg)
{
	const uint32_t keep = rng_.state();
	cfg_ = cfg;
	rng_.reseed(keep);
}

const SectionRule &CutScheduler::rule(Section s) const
{
	return s == Section::Breakdown ? cfg_.breakdown : (s == Section::Drop ? cfg_.drop : cfg_.groove);
}

double CutScheduler::hold_seconds(Section s, double bpm) const
{
	const SectionRule &r = rule(s);
	const double beats = std::max(1, r.hold_bars) * 4.0 + std::max(0.0, r.transition_beats) + 1.0; // +1 margin
	return beats * 60.0 / std::max(1.0, bpm);
}

double CutScheduler::next_bar_at_least(double beat) const
{
	return std::ceil(beat / 4.0 - 1e-9) * 4.0;
}

void CutScheduler::emit(const SchedEvent &e, std::vector<SchedEvent> &out)
{
	char buf[200];
	std::snprintf(buf, sizeof(buf), "%9.3f %-12s slot %d %-9s hold %.2fs %s %.2f%s%s | %s", e.at_beat,
		      type_name(e.type), e.slot, sec_name(e.section), e.hold_s, tr_name(e.transition),
		      e.transition_beats,
		      e.type == SchedEvent::Type::StutterFlip ? (e.show_prev ? " prev" : " live") : "", "",
		      e.reason.c_str());
	log_.push_back(buf);
	out.push_back(e);
}

void CutScheduler::prepare(int slot, Section s, double now, double bpm, const char *why, std::vector<SchedEvent> &out)
{
	SchedEvent e;
	e.type = SchedEvent::Type::Prepare;
	e.at_beat = now;
	e.slot = slot;
	e.section = s;
	e.hold_s = hold_seconds(s, bpm);
	e.reason = why;
	if (slot == kNext) {
		next_section_ = s;
		next_prepared_at_ = now;
	}
	emit(e, out);
}

void CutScheduler::skip()
{
	skip_pending_ = true;
}

void CutScheduler::set_hold(bool hold)
{
	if (hold_ && !hold)
		release_pending_ = true;
	hold_ = hold;
}

std::vector<SchedEvent> CutScheduler::tick(const ClockSample &clk)
{
	std::vector<SchedEvent> out;
	if (!clk.valid)
		return out;
	const double b = clk.beats;
	const double bpm = clk.bpm > 1.0 ? clk.bpm : 128.0;
	const double pre = std::max(0.0, cfg_.preroll_beats);

	if (!started_) {
		started_ = true;
		section_ = clk.section;
		last_beats_ = b;
		next_cut_ = next_bar_at_least(b + pre);
		prepare(kNext, section_, b, bpm, "start", out);
		if (section_ == Section::Breakdown) {
			prepare(kDrop, Section::Drop, b, bpm, "breakdown: arm drop clip", out);
			drop_armed_ = true;
		}
		return out;
	}

	/* clock jump (Palette Director reset, big correction): re-plan from here */
	if (b < last_beats_ - 1.0 || b > last_beats_ + 8.0) {
		next_cut_ = next_bar_at_least(b + pre);
		char why[96];
		std::snprintf(why, sizeof(why), "clock jump %.2f -> %.2f: re-plan", last_beats_, b);
		log_.push_back(why);
	}
	last_beats_ = b;

	/* section changes */
	if (clk.section != section_) {
		section_ = clk.section;
		if (section_ == Section::Drop && drop_armed_ && !hold_) {
			SchedEvent c;
			c.type = SchedEvent::Type::Cut;
			c.at_beat = b;
			c.slot = kDrop;
			c.section = Section::Drop;
			c.transition = Transition::Cut;
			c.reason = "drop downbeat";
			emit(c, out);
			drop_armed_ = false;
			const double downbeat = std::floor(b + 1e-9);
			next_cut_ = downbeat + std::max(1, cfg_.drop.hold_bars) * 4.0;
			prepare(kNext, Section::Drop, b, bpm, "after drop", out);
		} else {
			if (cfg_.cut_on_section_change)
				next_cut_ = next_bar_at_least(b + pre);
			else
				next_cut_ = std::max(next_cut_, next_bar_at_least(b + pre));
			prepare(kNext, section_, b, bpm, "section change", out);
			if (section_ == Section::Breakdown) {
				prepare(kDrop, Section::Drop, b, bpm, "breakdown: arm drop clip", out);
				drop_armed_ = true;
			} else {
				drop_armed_ = false;
			}
		}
	}

	/* stutter: follow Palette Director's drop colour cycle */
	if (cfg_.stutter && clk.drop_cycle && !hold_) {
		if (!stutter_on_) {
			stutter_on_ = true;
			stutter_start_ = std::floor(b + 1e-9);
			stutter_step_ = -1;
			SchedEvent e;
			e.type = SchedEvent::Type::StutterOn;
			e.at_beat = b;
			e.section = section_;
			e.reason = "drop colour cycle";
			emit(e, out);
		}
		const int steps = std::max(1, clk.drop_cycle_steps_per_beat);
		const long long k = static_cast<long long>(std::floor((b - stutter_start_) * steps + 1e-9));
		if (k != stutter_step_ && k >= 0) {
			stutter_step_ = k;
			SchedEvent e;
			e.type = SchedEvent::Type::StutterFlip;
			e.at_beat = stutter_start_ + static_cast<double>(k) / steps;
			e.section = section_;
			e.show_prev = (k % 2) == 1;
			e.reason = "step";
			emit(e, out);
		}
	} else if (stutter_on_) {
		stutter_on_ = false;
		SchedEvent e;
		e.type = SchedEvent::Type::StutterOff;
		e.at_beat = b;
		e.section = section_;
		e.reason = "cycle over";
		emit(e, out);
	}

	/* hold / skip / release */
	if (hold_) {
		skip_pending_ = false;
		return out;
	}
	if (release_pending_) {
		release_pending_ = false;
		next_cut_ = next_bar_at_least(b + pre);
	}
	if (skip_pending_) {
		skip_pending_ = false;
		next_cut_ = std::max(std::ceil(b - 1e-9), next_prepared_at_ + pre);
	}

	/* the planned cut */
	if (b >= next_cut_) {
		if (stutter_on_) { // never cut under a stutter: it would swap the clips being alternated
			next_cut_ += 4.0;
			return out;
		}
		const SectionRule &r = rule(section_);
		Transition tr = r.transition;
		double len = tr == Transition::Cut ? 0.0 : std::max(0.25, r.transition_beats);
		if (tr == Transition::Cut && r.crossfade_chance > 0.0 && rng_.next() < r.crossfade_chance) {
			tr = Transition::Crossfade;
			len = std::max(0.25, r.transition_beats);
		}
		SchedEvent c;
		c.type = SchedEvent::Type::Cut;
		c.at_beat = next_cut_;
		c.slot = kNext;
		c.section = next_section_;
		c.transition = tr;
		c.transition_beats = len;
		c.reason = b - next_cut_ > 1.0 ? "late (clock/preroll)" : "hold elapsed";
		emit(c, out);
		const double prev = next_cut_;
		next_cut_ = prev + std::max(1, r.hold_bars) * 4.0;
		if (next_cut_ <= b + pre)
			next_cut_ = next_bar_at_least(b + pre);
		prepare(kNext, section_, b, bpm, "next", out);
	}
	return out;
}

} // namespace cd
