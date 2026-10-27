#include "doctest.h"

#include <cmath>
#include <functional>

#include "scheduler.h"

using namespace cd;

namespace {

using SectionAt = std::function<Section(double beat)>;
using CycleAt = std::function<bool(double beat)>;

// Run the scheduler at 60 fps on a steady 128 BPM clock from `from` to `to` beats.
std::vector<SchedEvent> run(CutScheduler &s, double from, double to, SectionAt sec, CycleAt cyc = nullptr,
			    std::function<void(double)> hook = nullptr)
{
	std::vector<SchedEvent> all;
	const double per_frame = 128.0 / 60.0 / 60.0;
	for (double b = from; b < to; b += per_frame) {
		if (hook)
			hook(b);
		ClockSample c;
		c.valid = true;
		c.beats = b;
		c.bpm = 128.0;
		c.section = sec(b);
		c.drop_cycle = cyc ? cyc(b) : false;
		c.drop_cycle_steps_per_beat = 2;
		auto ev = s.tick(c);
		for (auto &e : ev) {
			e.reason += " @" + std::to_string(b); // remember the tick beat for timing checks
			all.push_back(e);
		}
	}
	return all;
}

double tick_beat(const SchedEvent &e)
{
	return std::stod(e.reason.substr(e.reason.rfind('@') + 1));
}

std::vector<SchedEvent> of(const std::vector<SchedEvent> &v, SchedEvent::Type t)
{
	std::vector<SchedEvent> o;
	for (const auto &e : v)
		if (e.type == t)
			o.push_back(e);
	return o;
}

const double kFrame = 128.0 / 60.0 / 60.0;

} // namespace

TEST_CASE("groove: cuts on bar lines every 4 bars, each prepared >= preroll ahead, within a frame")
{
	SchedulerConfig cfg;
	cfg.groove.crossfade_chance = 0.0;
	CutScheduler s(cfg);
	auto ev = run(s, 1.3, 200.0, [](double) { return Section::Groove; });
	auto cuts = of(ev, SchedEvent::Type::Cut);
	REQUIRE(cuts.size() >= 11);
	CHECK(cuts[0].at_beat == 4.0); // first bar line at least 1 beat ahead of 1.3
	for (std::size_t i = 0; i < cuts.size(); ++i) {
		CHECK(std::fmod(cuts[i].at_beat, 4.0) == 0.0);
		if (i)
			CHECK(cuts[i].at_beat - cuts[i - 1].at_beat == 16.0);
		CHECK(tick_beat(cuts[i]) - cuts[i].at_beat < kFrame + 1e-9); // fired on the first frame past
		CHECK(cuts[i].transition == Transition::Cut);
	}
	// every cut's slot was prepared at least 1 beat before it
	double last_prep = -1;
	std::size_t ci = 0;
	for (const auto &e : ev) {
		if (e.type == SchedEvent::Type::Prepare && e.slot == CutScheduler::kNext)
			last_prep = e.at_beat;
		if (e.type == SchedEvent::Type::Cut && ci++ > 0)
			CHECK(e.at_beat - last_prep >= 1.0);
	}
}

TEST_CASE("breakdown -> drop: re-prepare, crossfade, armed drop clip cuts on the drop downbeat")
{
	CutScheduler s(SchedulerConfig{});
	auto sec = [](double b) {
		if (b < 32.0)
			return Section::Groove;
		if (b < 96.0)
			return Section::Breakdown;
		return Section::Drop;
	};
	auto ev = run(s, 0.0, 140.0, sec);
	// breakdown start: prepares for breakdown and arms a drop clip
	bool armed = false, bd_prep = false;
	for (const auto &e : ev)
		if (e.type == SchedEvent::Type::Prepare && tick_beat(e) >= 32.0 && tick_beat(e) < 32.1) {
			armed |= e.slot == CutScheduler::kDrop && e.section == Section::Drop;
			bd_prep |= e.slot == CutScheduler::kNext && e.section == Section::Breakdown;
		}
	CHECK(armed);
	CHECK(bd_prep);
	auto cuts = of(ev, SchedEvent::Type::Cut);
	// breakdown cut: next bar line >= 33 -> 36, crossfade 4 beats; then every 8 bars
	bool saw36 = false, saw68 = false;
	for (const auto &c : cuts) {
		if (c.at_beat == 36.0) {
			saw36 = true;
			CHECK(c.transition == Transition::Crossfade);
			CHECK(c.transition_beats == 4.0);
		}
		saw68 |= c.at_beat == 68.0;
	}
	CHECK(saw36);
	CHECK(saw68);
	// drop: immediate cut to the drop slot on the drop downbeat (within a frame), then +8 beats
	const SchedEvent *drop = nullptr;
	for (const auto &c : cuts)
		if (c.slot == CutScheduler::kDrop)
			drop = &c;
	REQUIRE(drop);
	CHECK(drop->at_beat >= 96.0);
	CHECK(drop->at_beat - 96.0 < kFrame + 1e-9);
	CHECK(drop->transition == Transition::Cut);
	bool after = false;
	for (const auto &c : cuts)
		after |= c.slot == CutScheduler::kNext && c.at_beat == 104.0;
	CHECK(after);
}

TEST_CASE("stutter follows the drop colour cycle: alternate live/prev at 1/8, no cut meanwhile")
{
	CutScheduler s(SchedulerConfig{});
	auto sec = [](double b) {
		return b < 64.0 ? Section::Breakdown : Section::Drop;
	};
	auto cyc = [](double b) {
		return b >= 64.0 && b < 68.0;
	}; // 1 bar
	auto ev = run(s, 0.0, 80.0, sec, cyc);
	auto flips = of(ev, SchedEvent::Type::StutterFlip);
	REQUIRE(flips.size() == 8); // 4 beats x 2 steps
	for (std::size_t i = 0; i < flips.size(); ++i) {
		CHECK(flips[i].at_beat == doctest::Approx(64.0 + 0.5 * i));
		CHECK(flips[i].show_prev == (i % 2 == 1));
	}
	CHECK(of(ev, SchedEvent::Type::StutterOn).size() == 1);
	CHECK(of(ev, SchedEvent::Type::StutterOff).size() == 1);
	for (const auto &c : of(ev, SchedEvent::Type::Cut))
		CHECK(!(tick_beat(c) > 64.1 && tick_beat(c) < 68.0));
}

TEST_CASE("hold freezes cuts (drop and stutter included); release and skip re-plan on the grid")
{
	CutScheduler s(SchedulerConfig{});
	auto ev = run(
		s, 0.0, 100.0, [](double) { return Section::Groove; }, nullptr,
		[&](double b) {
			if (b >= 10.0 && b < 10.0 + kFrame)
				s.set_hold(true);
			if (b >= 50.5 && b < 50.5 + kFrame)
				s.set_hold(false);
			if (b >= 70.2 && b < 70.2 + kFrame)
				s.skip();
		});
	auto cuts = of(ev, SchedEvent::Type::Cut);
	for (const auto &c : cuts)
		CHECK(!(c.at_beat > 10.0 && c.at_beat < 52.0));
	bool released = false, skipped = false;
	for (const auto &c : cuts) {
		released |= c.at_beat == 52.0; // first bar line >= 50.5 + 1
		skipped |= c.at_beat == 71.0;  // next beat after 70.2 (prepared long before)
	}
	CHECK(released);
	CHECK(skipped);

	CutScheduler h(SchedulerConfig{});
	h.set_hold(true);
	auto ev2 = run(
		h, 0.0, 80.0, [](double b) { return b < 40.0 ? Section::Breakdown : Section::Drop; },
		[](double b) { return b >= 40.0 && b < 44.0; });
	CHECK(of(ev2, SchedEvent::Type::Cut).empty());
	CHECK(of(ev2, SchedEvent::Type::StutterFlip).empty());
}

TEST_CASE("clock jump re-plans instead of bursting cuts; same seed gives the same log")
{
	CutScheduler s(SchedulerConfig{});
	run(s, 0.0, 40.0, [](double) { return Section::Groove; });
	auto ev = run(s, 5.0, 40.0, [](double) { return Section::Groove; }); // clock went back 35 beats
	auto cuts = of(ev, SchedEvent::Type::Cut);
	REQUIRE(!cuts.empty());
	CHECK(cuts[0].at_beat == 8.0); // first bar line >= 5 + 1
	CHECK(cuts.size() <= 3);

	SchedulerConfig cfg;
	cfg.groove.crossfade_chance = 0.5;
	CutScheduler a(cfg), b(cfg);
	cfg.seed = 99;
	CutScheduler c(cfg);
	auto sec = [](double x) {
		return x < 128 ? Section::Groove : (x < 192 ? Section::Breakdown : Section::Drop);
	};
	run(a, 0.0, 400.0, sec);
	run(b, 0.0, 400.0, sec);
	run(c, 0.0, 400.0, sec);
	CHECK(a.log() == b.log());
	CHECK(a.log() != c.log());
}
