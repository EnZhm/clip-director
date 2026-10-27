#pragma once
// Clip Director core: beat-synced cut scheduler (spec 8). Pure C++17, deterministic.
//
// The scheduler never touches clips or decks. It turns clock samples into events, and the engine
// (OBS layer) acts on them:
//   Prepare(slot, section, hold_s)  load a pick for `section` into `slot`, seek it, pause it
//   Cut(slot, transition, beats)    show `slot`, starting on the beat `at_beat`
//   StutterOn / StutterFlip / StutterOff   alternate between the live and the outgoing clip at
//                                   Palette Director's drop-cycle rate (no seeks needed)
// Slots: kNext = the next normal clip; kDrop = a drop clip held ready during breakdowns, so the
// drop downbeat can cut instantly.
//
// Timing rules:
//   - Cuts land on bar lines (multiples of 4 beats on Palette Director's absolute grid).
//   - Every planned cut is at least `preroll_beats` after its Prepare.
//   - BREAKDOWN start or return to GROOVE: re-prepare for the new section, cut on the next bar
//     line at least `preroll_beats` ahead.
//   - DROP: cut immediately to the prepared drop clip (the drop downbeat), then follow the drop rule.
//   - A clock jump (Palette Director reset or a big tempo correction) re-plans from the new position.
#include <cstdint>
#include <string>
#include <vector>

#include "rng.h"
#include "types.h"

namespace cd {

enum class Transition { Cut = 0, Crossfade = 1, Dip = 2 };

struct SectionRule {
	int hold_bars = 4;
	Transition transition = Transition::Cut;
	double transition_beats = 1.0; // Crossfade / Dip length
	double crossfade_chance = 0.0; // for Transition::Cut: chance per cut to crossfade instead
};

struct SchedulerConfig {
	SectionRule groove{4, Transition::Cut, 1.0, 0.25};
	SectionRule breakdown{8, Transition::Crossfade, 4.0, 0.0};
	SectionRule drop{2, Transition::Cut, 1.0, 0.0};
	bool cut_on_section_change = true;
	bool stutter = true; // follow Palette Director's drop colour cycle
	double preroll_beats = 1.0; // the OBS source defaults to 2 (deck loads take ~0.3-0.5 s)
	uint32_t seed = 7;
};

struct SchedEvent {
	enum class Type { Prepare, Cut, StutterOn, StutterFlip, StutterOff };
	Type type = Type::Prepare;
	double at_beat = 0.0;
	int slot = 0;
	Section section = Section::Groove;
	double hold_s = 0.0; // Prepare: how long the clip will be shown (wall seconds)
	Transition transition = Transition::Cut;
	double transition_beats = 0.0;
	bool show_prev = false; // StutterFlip: true = show the outgoing clip
	std::string reason;
};

class CutScheduler {
public:
	static constexpr int kNext = 0;
	static constexpr int kDrop = 1;

	explicit CutScheduler(const SchedulerConfig &cfg);

	std::vector<SchedEvent> tick(const ClockSample &clk);
	void set_config(const SchedulerConfig &cfg); // keeps RNG and plan
	void skip();                                 // manual: cut on the next beat (preroll permitting)
	void set_hold(bool hold);                    // freeze on the current clip, no cuts
	bool hold() const { return hold_; }
	double next_cut_beat() const { return next_cut_; }

	// Text log of every event so far (determinism oracle for the tests).
	const std::vector<std::string> &log() const { return log_; }

private:
	const SectionRule &rule(Section s) const;
	double hold_seconds(Section s, double bpm) const;
	double next_bar_at_least(double beat) const;
	void prepare(int slot, Section s, double at, double bpm, const char *why, std::vector<SchedEvent> &out);
	void emit(const SchedEvent &e, std::vector<SchedEvent> &out);

	SchedulerConfig cfg_;
	Mulberry32 rng_;
	bool started_ = false;
	Section section_ = Section::Groove;
	Section next_section_ = Section::Groove; // section the kNext slot was prepared for
	double next_cut_ = 0.0;
	double next_prepared_at_ = 0.0;
	bool drop_armed_ = false;
	double last_beats_ = 0.0;
	bool hold_ = false;
	bool release_pending_ = false;
	bool skip_pending_ = false;
	bool stutter_on_ = false;
	long long stutter_step_ = -1;
	double stutter_start_ = 0.0;
	std::vector<std::string> log_;
};

} // namespace cd
