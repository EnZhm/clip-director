// W3 gate: a scene launch goes out on the same frame/beat as the clip transition it pairs with.
//
// Palette Director's real Director (built from ../palette-director) analyses a synthetic WAV
// fixture (groove -> breakdown -> drop at 128 BPM). Every video frame (735 samples = 60 fps) its
// snapshot becomes a ClockSample, exactly as the OBS source builds one from the clock API, and
// drives Clip Director's CutScheduler with Synesthesia control on. Checks:
//   - breakdown: the launch from the Breakdown pool is on the beat of the breakdown crossfade cut
//     and in the same frame as it
//   - drop: the launch from the Drop pool is on the drop cut's beat, in the same frame, with bang
//   - groove: launches are on phrase downbeats, sent a Scene lead early (and only there)
#include "doctest.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "scheduler.h"

#include "director.h"
#include "test_helpers.h" // Palette Director's: make_set, gen_breakdown

namespace {

constexpr uint32_t kSr = 44100;
constexpr std::size_t kFrame = 735; // 60 fps
constexpr double kFrameS = static_cast<double>(kFrame) / kSr;
constexpr double kBpm = 128.0;
constexpr int kPhraseBeats = 32;
constexpr double kLeadMs = 120.0;

struct Ev {
	double t = 0.0; // wall time of the frame that produced it
	cd::SchedEvent e;
};

struct Run {
	std::vector<Ev> evs;
	double pd_bd = -1.0;   // wall time Palette Director entered BREAKDOWN
	double pd_drop = -1.0; // wall time Palette Director entered DROP
};

Run run(int groove_bars, int breakdown_bars, int drop_bars)
{
	pd::SetFile set = pdtest::make_set(6, "Pal 5");
	set.director.random_off = false;
	set.director.seed = 7;
	pd::Director d(set);
	const std::vector<float> m = pdtest::gen_breakdown(kBpm, groove_bars, breakdown_bars, drop_bars).to_float();

	cd::SchedulerConfig cfg;
	cfg.preroll_beats = 2.0;
	cfg.scene.auto_on = true;
	cfg.scene.on_breakdown = true;
	cfg.scene.on_drop = true;
	cfg.scene.groove_phrases = 2; // every 2 phrases
	cfg.scene.lead_ms = kLeadMs;
	cfg.scene.bang_on_drop = true;
	cd::CutScheduler sched(cfg);

	Run r;
	pd::Section was = pd::Section::Groove;
	for (std::size_t i = 0; i < m.size(); i += kFrame) {
		const std::size_t n = std::min(kFrame, m.size() - i);
		d.push_audio(m.data() + i, n, kSr);
		const double t = static_cast<double>(i + n) / kSr;

		const pd::DirectorState st = d.snapshot();
		if (st.section == pd::Section::Breakdown && was != pd::Section::Breakdown && r.pd_bd < 0)
			r.pd_bd = t;
		if (st.section == pd::Section::Drop && was != pd::Section::Drop && r.pd_drop < 0)
			r.pd_drop = t;
		was = st.section;

		cd::ClockSample clk;
		clk.valid = true;
		clk.beats = static_cast<double>(st.beat) + st.beat_frac;
		clk.bpm = st.bpm > 1.0f ? st.bpm : kBpm;
		clk.section = static_cast<cd::Section>(static_cast<int>(st.section));
		clk.drop_cycle = st.drop_cycle;
		clk.drop_cycle_steps_per_beat = 2;
		clk.phrase_beats = kPhraseBeats;
		clk.paused = st.paused;
		for (const cd::SchedEvent &e : sched.tick(clk))
			r.evs.push_back({t, e});
	}
	return r;
}

const Ev *first_cut(const Run &r, cd::Section section, int slot)
{
	for (const Ev &e : r.evs)
		if (e.e.type == cd::SchedEvent::Type::Cut && e.e.section == section && (slot < 0 || e.e.slot == slot))
			return &e;
	return nullptr;
}

const Ev *first_launch(const Run &r, cd::Section pool)
{
	for (const Ev &e : r.evs)
		if (e.e.type == cd::SchedEvent::Type::SceneLaunch && e.e.section == pool)
			return &e;
	return nullptr;
}

std::size_t count_launch(const Run &r, cd::Section pool)
{
	std::size_t n = 0;
	for (const Ev &e : r.evs)
		n += (e.e.type == cd::SchedEvent::Type::SceneLaunch && e.e.section == pool) ? 1 : 0;
	return n;
}

} // namespace

TEST_CASE("W3: scene launches land in the same frame and on the same beat as their cut (WAV fixture)")
{
	const Run r = run(8, 8, 16);
	REQUIRE(r.pd_bd > 0.0);   // the fixture really breaks down
	REQUIRE(r.pd_drop > 0.0); // ...and really drops

	/* breakdown: launch from the Breakdown pool on the breakdown crossfade start */
	const Ev *bd_cut = first_cut(r, cd::Section::Breakdown, -1);
	const Ev *bd_scene = first_launch(r, cd::Section::Breakdown);
	REQUIRE(bd_cut);
	REQUIRE(bd_scene);
	CHECK(bd_cut->e.transition == cd::Transition::Crossfade);
	CHECK(bd_scene->e.at_beat == bd_cut->e.at_beat);             // the same beat
	CHECK(std::fabs(bd_scene->t - bd_cut->t) <= kFrameS + 1e-9); // the same frame
	CHECK(bd_scene->e.send_at_beat == bd_scene->e.at_beat);      // detected, not predicted: no lead
	CHECK_FALSE(bd_scene->e.bang);
	CHECK(count_launch(r, cd::Section::Breakdown) == 1u);

	/* drop: launch from the Drop pool on the drop downbeat, in the same frame, with bang 1 */
	const Ev *drop_cut = first_cut(r, cd::Section::Drop, cd::CutScheduler::kDrop);
	const Ev *drop_scene = first_launch(r, cd::Section::Drop);
	REQUIRE(drop_cut);
	REQUIRE(drop_scene);
	CHECK(drop_scene->e.at_beat == drop_cut->e.at_beat);
	CHECK(std::fabs(drop_scene->t - drop_cut->t) <= kFrameS + 1e-9);
	CHECK(drop_scene->e.send_at_beat == drop_scene->e.at_beat);
	CHECK(drop_scene->e.bang); // "also fire scene bang 1"
	CHECK(count_launch(r, cd::Section::Drop) == 1u);
	// the drop lands as one hit: the cut, the scene launch and Palette Director's own drop
	// detection are all within a frame of each other
	CHECK(std::fabs(drop_cut->t - r.pd_drop) <= kFrameS + 1e-9);
	CHECK(std::fabs(drop_scene->t - r.pd_drop) <= kFrameS + 1e-9);

	/* groove: every 2 phrases, on the phrase downbeat, sent a lead early */
	const double lead_beats = kLeadMs / 1000.0 * kBpm / 60.0;
	const double frame_beats = kBpm / 60.0 / 60.0;
	std::size_t groove = 0;
	for (const Ev &e : r.evs) {
		if (e.e.type != cd::SchedEvent::Type::SceneLaunch || e.e.section != cd::Section::Groove)
			continue;
		++groove;
		CHECK(std::fmod(e.e.at_beat, static_cast<double>(kPhraseBeats)) == 0.0);
		CHECK(e.e.at_beat - e.e.send_at_beat <= lead_beats + 1e-9);
		CHECK(e.e.at_beat - e.e.send_at_beat > lead_beats - frame_beats - 1e-9);
	}
	CHECK(groove >= 1u);
}
