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
	case SchedEvent::Type::SceneLaunch:
		return "scene-launch";
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

double CutScheduler::next_phrase_after(double beat, int phrase_beats) const
{
	const double pb = static_cast<double>(std::max(1, phrase_beats));
	return (std::floor(beat / pb + 1e-9) + 1.0) * pb;
}

void CutScheduler::emit(const SchedEvent &e, std::vector<SchedEvent> &out)
{
	char buf[200];
	if (e.type == SchedEvent::Type::SceneLaunch) {
		std::snprintf(buf, sizeof(buf), "%9.3f %-12s pool %-9s send %8.3f%s | %s", e.at_beat, type_name(e.type),
			      sec_name(e.section), e.send_at_beat, e.bang ? " +bang" : "", e.reason.c_str());
	} else {
		std::snprintf(buf, sizeof(buf), "%9.3f %-12s slot %d %-9s hold %.2fs %s %.2f%s%s | %s", e.at_beat,
			      type_name(e.type), e.slot, sec_name(e.section), e.hold_s, tr_name(e.transition),
			      e.transition_beats,
			      e.type == SchedEvent::Type::StutterFlip ? (e.show_prev ? " prev" : " live") : "", "",
			      e.reason.c_str());
	}
	log_.push_back(buf);
	out.push_back(e);
}

void CutScheduler::scene_launch(Section pool, double at_beat, double send_at_beat, bool bang, const char *why,
				std::vector<SchedEvent> &out)
{
	if (!cfg_.scene.auto_on)
		return;
	SchedEvent e;
	e.type = SchedEvent::Type::SceneLaunch;
	e.slot = -1; // a scene, not a deck
	e.section = pool;
	e.at_beat = at_beat;
	e.send_at_beat = send_at_beat;
	e.bang = bang;
	e.reason = why;
	emit(e, out);
}

void CutScheduler::plan_groove_scene(double beat, int phrase_beats)
{
	if (!cfg_.scene.auto_on || cfg_.scene.groove_phrases <= 0) {
		scene_planned_ = false;
		return;
	}
	next_scene_beat_ = next_phrase_after(beat, phrase_beats);
	scene_planned_ = true;
}

void CutScheduler::scene_step(const ClockSample &clk, std::vector<SchedEvent> &out)
{
	if (section_ != Section::Groove || cfg_.scene.groove_phrases <= 0) {
		scene_planned_ = false;
		return;
	}
	if (!cfg_.scene.auto_on)
		return;
	if (scene_hold_) {
		scene_planned_ = false; /* Hold scene: re-anchor the plan on the next phrase downbeat */
		return;
	}
	const double b = clk.beats;
	const int pb = std::max(1, clk.phrase_beats);
	if (!scene_planned_)
		plan_groove_scene(b, pb);
	if (!scene_planned_)
		return;
	const double bpm = clk.bpm > 1.0 ? clk.bpm : 128.0;
	const double step = std::max(1, cfg_.scene.groove_phrases) * static_cast<double>(pb);
	const double lead = std::clamp(cfg_.scene.lead_ms, 0.0, 500.0) / 1000.0 * bpm / 60.0;
	if (b < next_scene_beat_ - lead)
		return;
	if (stutter_on_) {
		/* a drop colour cycle is running: planned launches wait for it to end (as cuts do) */
		while (next_scene_beat_ - lead <= b)
			next_scene_beat_ += step;
		return;
	}
	scene_launch(Section::Groove, next_scene_beat_, b, false, "groove phrase", out);
	next_scene_beat_ += step;
	while (next_scene_beat_ - lead <= b) /* never burst after a stall */
		next_scene_beat_ += step;
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
			bd_cut_pending_ = true; /* the first cut in this breakdown pairs with a scene launch */
		} else if (section_ == Section::Groove) {
			plan_groove_scene(b, clk.phrase_beats);
		}
		return out;
	}

	/* clock jump (Palette Director reset, big correction): re-plan from here */
	if (b < last_beats_ - 1.0 || b > last_beats_ + 8.0) {
		next_cut_ = next_bar_at_least(b + pre);
		scene_planned_ = false; /* re-anchor the groove scene plan */
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
			/* W3: the drop scene goes out on the same beat as the drop cut, so the drop
			 * lands as one hit: clip cut + Palette Director's drop cycle + Synesthesia */
			if (cfg_.scene.on_drop)
				scene_launch(Section::Drop, b, b, cfg_.scene.bang_on_drop, "drop downbeat", out);
			bd_cut_pending_ = false;
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
				bd_cut_pending_ = true;
				scene_planned_ = false;
			} else {
				drop_armed_ = false;
				bd_cut_pending_ = false;
				if (section_ == Section::Groove)
					plan_groove_scene(b, clk.phrase_beats);
				else
					scene_planned_ = false;
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
		scene_planned_ = false; /* re-anchor the groove scene plan on the next phrase downbeat */
	}
	if (skip_pending_) {
		skip_pending_ = false;
		next_cut_ = std::max(std::ceil(b - 1e-9), next_prepared_at_ + pre);
	}

	/* groove scene launches: planned on phrase downbeats, before the planned cut so a stutter
	 * that defers the cut does not swallow the scene step */
	scene_step(clk, out);

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
		/* the breakdown crossfade start pairs with a launch from the Breakdown pool */
		if (bd_cut_pending_) {
			bd_cut_pending_ = false;
			if (c.section == Section::Breakdown && cfg_.scene.on_breakdown)
				scene_launch(Section::Breakdown, c.at_beat, c.at_beat, false, "breakdown start", out);
		}
		const double prev = next_cut_;
		next_cut_ = prev + std::max(1, r.hold_bars) * 4.0;
		if (next_cut_ <= b + pre)
			next_cut_ = next_bar_at_least(b + pre);
		prepare(kNext, section_, b, bpm, "next", out);
	}
	return out;
}

} // namespace cd
