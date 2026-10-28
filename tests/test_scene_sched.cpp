// W2/W3: scene launches are planned by the cut scheduler, on the same beat as the clip
// transition they pair with. Also the regression that clip-only behaviour is unchanged when
// Synesthesia control is off.
#include "doctest.h"

#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "scheduler.h"

using namespace cd;

namespace {

struct Tick {
	double beat = 0.0;
	std::vector<SchedEvent> evs;
};

using SectionAt = std::function<Section(double)>;
using CycleAt = std::function<bool(double)>;

std::vector<Tick> run(CutScheduler &s, double from, double to, const SectionAt &sec, const CycleAt &cyc = nullptr,
		      int phrase_beats = 32, const std::function<void(double)> &hook = nullptr)
{
	std::vector<Tick> out;
	const double frame = 128.0 / 60.0 / 60.0; // one 60 fps frame at 128 BPM
	for (double b = from; b < to; b += frame) {
		if (hook)
			hook(b);
		ClockSample c;
		c.valid = true;
		c.beats = b;
		c.bpm = 128.0;
		c.section = sec(b);
		c.drop_cycle = cyc ? cyc(b) : false;
		c.drop_cycle_steps_per_beat = 2;
		c.phrase_beats = phrase_beats;
		Tick t;
		t.beat = b;
		t.evs = s.tick(c);
		out.push_back(std::move(t));
	}
	return out;
}

std::vector<SchedEvent> of(const std::vector<Tick> &ticks, SchedEvent::Type type)
{
	std::vector<SchedEvent> o;
	for (const Tick &t : ticks)
		for (const SchedEvent &e : t.evs)
			if (e.type == type)
				o.push_back(e);
	return o;
}

std::vector<SchedEvent> launches(const std::vector<Tick> &ticks, Section pool)
{
	std::vector<SchedEvent> o;
	for (const SchedEvent &e : of(ticks, SchedEvent::Type::SceneLaunch))
		if (e.section == pool)
			o.push_back(e);
	return o;
}

/* true when a scene launch for `pool` at `at_beat` and the cut it pairs with came out of the
 * same tick, i.e. the same video frame */
bool paired_in_one_tick(const std::vector<Tick> &ticks, Section pool, double at_beat, int slot)
{
	for (const Tick &t : ticks) {
		bool scene = false, cut = false;
		for (const SchedEvent &e : t.evs) {
			if (e.type == SchedEvent::Type::SceneLaunch && e.section == pool &&
			    std::fabs(e.at_beat - at_beat) < 1e-9)
				scene = true;
			if (e.type == SchedEvent::Type::Cut && e.slot == slot && std::fabs(e.at_beat - at_beat) < 1e-9)
				cut = true;
		}
		if (scene && cut)
			return true;
	}
	return false;
}

Section groove_breakdown_drop(double b)
{
	if (b < 32.0)
		return Section::Groove;
	if (b < 96.0)
		return Section::Breakdown;
	return Section::Drop;
}

} // namespace

TEST_CASE("scene launches: off means the scheduler is exactly the clip scheduler it was")
{
	SchedulerConfig base; // scene.auto_on == false
	SchedulerConfig noisy = base;
	noisy.scene.on_breakdown = true;
	noisy.scene.on_drop = true;
	noisy.scene.groove_phrases = 4;
	noisy.scene.lead_ms = 250.0; // even a lead changes nothing while auto is off
	noisy.scene.bang_on_drop = true;

	CutScheduler a(base), b(noisy);
	const std::vector<Tick> ta = run(a, 0.0, 200.0, groove_breakdown_drop);
	const std::vector<Tick> tb = run(b, 0.0, 200.0, groove_breakdown_drop);

	CHECK(a.log() == b.log());
	CHECK(of(ta, SchedEvent::Type::SceneLaunch).empty());
	CHECK(of(tb, SchedEvent::Type::SceneLaunch).empty());

	// ...and the clip cuts are the ones the pre-W2 scheduler produced
	const std::vector<SchedEvent> cuts = of(tb, SchedEvent::Type::Cut);
	REQUIRE(cuts.size() >= 4u);
	CHECK(cuts[0].at_beat == 4.0);
	bool bd_cut = false, drop_cut = false;
	for (const SchedEvent &c : cuts) {
		if (std::fabs(c.at_beat - 36.0) < 1e-9) {
			bd_cut = true;
			CHECK(c.section == Section::Breakdown);
			CHECK(c.transition == Transition::Crossfade);
			CHECK(c.transition_beats == 4.0);
		}
		if (c.slot == CutScheduler::kDrop) {
			drop_cut = true;
			CHECK(c.at_beat >= 96.0);
			CHECK(c.at_beat - 96.0 < 128.0 / 60.0 / 60.0 + 1e-9);
		}
	}
	CHECK(bd_cut);
	CHECK(drop_cut);
}

TEST_CASE("scene launches: breakdown start and drop downbeat land on the beat of their cut")
{
	SchedulerConfig cfg;
	cfg.scene.auto_on = true;
	cfg.scene.bang_on_drop = true;
	CutScheduler s(cfg);
	const std::vector<Tick> ticks = run(s, 0.0, 140.0, groove_breakdown_drop);

	// breakdown: one launch, exactly on the breakdown crossfade start (beat 36), in that frame
	const std::vector<SchedEvent> bd = launches(ticks, Section::Breakdown);
	REQUIRE(bd.size() == 1u);
	CHECK(bd[0].at_beat == 36.0);
	CHECK(bd[0].send_at_beat == bd[0].at_beat); // detected, never predicted: no lead
	CHECK_FALSE(bd[0].bang);
	CHECK(paired_in_one_tick(ticks, Section::Breakdown, 36.0, CutScheduler::kNext));

	// drop: one launch, exactly on the drop cut's beat (the drop downbeat), in that frame
	const std::vector<SchedEvent> dr = launches(ticks, Section::Drop);
	REQUIRE(dr.size() == 1u);
	CHECK(dr[0].send_at_beat == dr[0].at_beat);
	CHECK(dr[0].bang); // "also fire scene bang 1" is on
	CHECK(dr[0].at_beat >= 96.0);
	CHECK(paired_in_one_tick(ticks, Section::Drop, dr[0].at_beat, CutScheduler::kDrop));

	// no groove launches: never
	CHECK(launches(ticks, Section::Groove).empty());
	// and no launch is ever emitted without auto_on
	CHECK(of(ticks, SchedEvent::Type::SceneLaunch).size() == 2u);
}

TEST_CASE("scene launches: breakdown and drop options can be switched off")
{
	SchedulerConfig cfg;
	cfg.scene.auto_on = true;
	cfg.scene.on_breakdown = false;
	cfg.scene.on_drop = false;
	CutScheduler s(cfg);
	const std::vector<Tick> ticks = run(s, 0.0, 140.0, groove_breakdown_drop);
	CHECK(of(ticks, SchedEvent::Type::SceneLaunch).empty());
	// the clip cuts are untouched by that
	CHECK(of(ticks, SchedEvent::Type::Cut).size() >= 4u);
}

TEST_CASE("scene launches: groove is every 2 or 4 phrases, on the phrase downbeat, with a lead")
{
	SchedulerConfig cfg;
	cfg.scene.auto_on = true;
	cfg.scene.groove_phrases = 2;
	cfg.scene.lead_ms = 120.0;
	CutScheduler s(cfg);
	const std::vector<Tick> ticks = run(s, 0.0, 320.0, [](double) { return Section::Groove; }, nullptr, 32);

	const std::vector<SchedEvent> g = launches(ticks, Section::Groove);
	REQUIRE(g.size() >= 4u);
	const double lead_beats = 120.0 / 1000.0 * 128.0 / 60.0;
	const double frame = 128.0 / 60.0 / 60.0;
	for (size_t i = 0; i < g.size(); ++i) {
		CHECK(std::fmod(g[i].at_beat, 32.0) == 0.0);                   // a phrase downbeat
		CHECK(std::fabs(std::fmod(g[i].at_beat, 64.0) - 32.0) < 1e-9); // every second phrase
		if (i)
			CHECK(g[i].at_beat - g[i - 1].at_beat == 64.0);
		// the OSC goes out a lead early: the first frame at or past (downbeat - lead)
		CHECK(g[i].at_beat - g[i].send_at_beat <= lead_beats + 1e-9);
		CHECK(g[i].at_beat - g[i].send_at_beat > lead_beats - frame - 1e-9);
	}
	CHECK(g[0].at_beat == 32.0);

	// every 4 phrases of 16 beats, no lead: the send is on the downbeat itself
	SchedulerConfig q;
	q.scene.auto_on = true;
	q.scene.groove_phrases = 4;
	CutScheduler s2(q);
	const std::vector<SchedEvent> g4 =
		launches(run(s2, 0.0, 320.0, [](double) { return Section::Groove; }, nullptr, 16), Section::Groove);
	REQUIRE(g4.size() >= 3u);
	for (size_t i = 0; i < g4.size(); ++i) {
		CHECK(std::fmod(g4[i].at_beat - 16.0, 64.0) == 0.0); // every 4 phrases of 16 beats
		if (i)
			CHECK(g4[i].at_beat - g4[i - 1].at_beat == 64.0);
		CHECK(g4[i].at_beat - g4[i].send_at_beat <= 1e-9);
		CHECK(g4[i].at_beat - g4[i].send_at_beat > -frame - 1e-9);
	}
}

TEST_CASE("scene launches: Hold scene suspends the automatic launches, clips keep cutting")
{
	SchedulerConfig cfg;
	cfg.scene.auto_on = true;
	cfg.scene.groove_phrases = 1;
	CutScheduler s(cfg);
	s.set_scene_hold(true);
	const std::vector<Tick> ticks = run(
		s, 0.0, 260.0, [](double) { return Section::Groove; }, nullptr, 32,
		[&](double b) {
			if (b >= 100.0 && b < 100.0 + 128.0 / 60.0 / 60.0 * 2)
				s.set_scene_hold(false);
		});
	const std::vector<SchedEvent> g = launches(ticks, Section::Groove);
	REQUIRE(!g.empty());
	// nothing while the scene was held; the grid re-anchors on release (the next phrase downbeat)
	CHECK(g[0].at_beat == 128.0);
	for (const SchedEvent &e : g) {
		CHECK(e.at_beat > 100.0);
		CHECK(std::fmod(e.at_beat, 32.0) == 0.0);
	}
	// the clip cuts keep coming while the scene is held
	CHECK(of(ticks, SchedEvent::Type::Cut).size() >= 10u);

	// clip Hold suspends both, as before
	SchedulerConfig c2;
	c2.scene.auto_on = true;
	c2.scene.groove_phrases = 1;
	CutScheduler s2(c2);
	s2.set_hold(true);
	CHECK(launches(run(s2, 0.0, 200.0, [](double) { return Section::Groove; }), Section::Groove).empty());
}

TEST_CASE("scene launches: a planned groove launch waits for the drop colour cycle to end")
{
	SchedulerConfig cfg;
	cfg.scene.auto_on = true;
	cfg.scene.groove_phrases = 1;
	CutScheduler s(cfg);
	const std::vector<Tick> ticks = run(
		s, 0.0, 200.0, [](double) { return Section::Groove; }, [](double b) { return b >= 64.0 && b < 96.0; },
		32);
	const std::vector<SchedEvent> g = launches(ticks, Section::Groove);
	REQUIRE(g.size() >= 2u);
	CHECK(g[0].at_beat == 32.0);
	CHECK(g[1].at_beat == 96.0); // the slot inside the cycle was skipped, not sent late
	for (const SchedEvent &e : g) {
		const bool inside = e.at_beat > 64.0 && e.at_beat < 96.0;
		CHECK_FALSE(inside);
	}
	bool saw_off = false;
	for (const SchedEvent &e : of(ticks, SchedEvent::Type::StutterOff))
		saw_off = true;
	CHECK(saw_off);
}

TEST_CASE("scene launches: the settings gate them, and a burst never fires two at once")
{
	// during groove: never
	SchedulerConfig off;
	off.scene.auto_on = true;
	off.scene.groove_phrases = 0;
	CutScheduler s(off);
	CHECK(launches(run(s, 0.0, 200.0, [](double) { return Section::Groove; }), Section::Groove).empty());

	// a stall (the tick loop skips 200 beats) must not replay the missed launches
	SchedulerConfig cfg;
	cfg.scene.auto_on = true;
	cfg.scene.groove_phrases = 1;
	CutScheduler s2(cfg);
	run(s2, 0.0, 32.0, [](double) { return Section::Groove; });
	const std::vector<Tick> jumped = run(s2, 232.0, 264.0, [](double) { return Section::Groove; });
	const std::vector<SchedEvent> g = launches(jumped, Section::Groove);
	REQUIRE(g.size() == 1u); // exactly one after the jump, not the five that were missed
}
