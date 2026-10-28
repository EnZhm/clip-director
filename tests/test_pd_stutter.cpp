// P6 gate: Clip Director's stutter is locked to Palette Director's drop colour cycle.
//
// Palette Director's real Director (built from ../palette-director) analyses a synthetic WAV
// fixture (groove -> breakdown -> drop at 128 BPM). Every video frame (735 samples = 60 fps)
// its snapshot becomes a ClockSample, exactly as the OBS source builds one from the v1.1 clock
// API, and drives Clip Director's CutScheduler. Checks, per drop-cycle rate and length:
//   - one stutter flip per palette step of the cycle, each within one frame of the step
//   - stutter starts with the drop cut and ends when the cycle ends
//   - the drop clip cuts in on the drop downbeat; no other cut fires under the stutter
#include "doctest.h"

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

struct Run {
	std::vector<double> pd_steps;   // wall time of each drop-cycle palette step
	double pd_drop = -1.0;          // wall time Palette Director entered DROP
	double pd_cycle_end = -1.0;     // wall time drop_cycle went false again
	std::vector<double> cd_flips;   // wall time of each StutterFlip
	std::vector<bool> cd_flip_prev; // which clip each flip shows
	double cd_on = -1.0, cd_off = -1.0;
	std::vector<double> cd_cuts; // wall time of every Cut
	std::vector<int> cd_cut_slot;
};

pd::SetFile cycle_set(int bars, int spb)
{
	pd::SetFile s = pdtest::make_set(6, "Pal 5");
	s.director.random_off = false;
	s.director.seed = 7;
	s.director.drop_cycle = true;
	s.director.drop_cycle_chance = 1.0f;
	s.director.drop_cycle_bars = bars;
	s.director.drop_cycle_steps_per_beat = spb;
	return s;
}

Run run(int bars, int spb)
{
	Run r;
	pd::Director d(cycle_set(bars, spb));
	const std::vector<float> m = pdtest::gen_breakdown(128.0, 8, 4, 8 + 4 * bars).to_float();

	cd::SchedulerConfig cfg;
	cfg.preroll_beats = 2.0;
	cd::CutScheduler sched(cfg);

	std::size_t seen = 0;
	bool was_cycle = false;
	pd::Section was_sec = pd::Section::Groove;
	for (std::size_t i = 0; i < m.size(); i += kFrame) {
		const std::size_t n = std::min(kFrame, m.size() - i);
		d.push_audio(m.data() + i, n, kSr);
		const double t = static_cast<double>(i + n) / kSr;

		// Palette Director's side
		const auto evs = d.events();
		for (; seen < evs.size(); ++seen) {
			const auto &e = evs[seen];
			if (e.type == pd::Event::Type::PaletteChange &&
			    e.detail.find("drop cycle") != std::string::npos)
				r.pd_steps.push_back(t);
		}
		const pd::DirectorState st = d.snapshot();
		if (st.section == pd::Section::Drop && was_sec != pd::Section::Drop && r.pd_drop < 0)
			r.pd_drop = t;
		if (was_cycle && !st.drop_cycle && r.pd_cycle_end < 0)
			r.pd_cycle_end = t;
		was_sec = st.section;
		was_cycle = st.drop_cycle;

		// the clock sample the OBS source would read this frame
		cd::ClockSample clk;
		clk.valid = true;
		clk.beats = static_cast<double>(st.beat) + st.beat_frac;
		clk.bpm = st.bpm > 1.0f ? st.bpm : 128.0;
		clk.section = static_cast<cd::Section>(static_cast<int>(st.section));
		clk.drop_cycle = st.drop_cycle;
		clk.drop_cycle_steps_per_beat = spb;
		clk.paused = st.paused;
		for (const auto &e : sched.tick(clk)) {
			using T = cd::SchedEvent::Type;
			if (e.type == T::StutterOn)
				r.cd_on = t;
			else if (e.type == T::StutterOff)
				r.cd_off = t;
			else if (e.type == T::StutterFlip) {
				r.cd_flips.push_back(t);
				r.cd_flip_prev.push_back(e.show_prev);
			} else if (e.type == T::Cut) {
				r.cd_cuts.push_back(t);
				r.cd_cut_slot.push_back(e.slot);
			}
		}
	}
	return r;
}

} // namespace

TEST_CASE("P6: stutter is locked to Palette Director's drop colour cycle (WAV fixture, 60 fps)")
{
	struct Case {
		int bars, spb, steps;
	};
	for (const Case c : {Case{1, 2, 8}, Case{1, 1, 4}, Case{1, 4, 16}, Case{2, 2, 16}, Case{4, 1, 16}}) {
		INFO("bars " << c.bars << ", steps/beat " << c.spb);
		const Run r = run(c.bars, c.spb);

		REQUIRE(static_cast<int>(r.pd_steps.size()) == c.steps); // the fixture really cycles
		REQUIRE(r.pd_drop > 0.0);
		REQUIRE(r.pd_cycle_end > 0.0);

		// one flip per palette step, each within one frame, alternating drop clip / outgoing clip
		REQUIRE(r.cd_flips.size() == r.pd_steps.size());
		double worst = 0.0;
		for (std::size_t k = 0; k < r.cd_flips.size(); ++k) {
			worst = std::max(worst, std::fabs(r.cd_flips[k] - r.pd_steps[k]));
			CHECK(r.cd_flip_prev[k] == (k % 2 == 1));
		}
		CHECK(worst <= kFrameS + 1e-9);
		MESSAGE("bars " << c.bars << " 1/" << 4 * c.spb << ": " << r.cd_flips.size()
				<< " flips, worst offset to the palette step " << worst * 1000.0 << " ms");

		// stutter spans the cycle
		CHECK(std::fabs(r.cd_on - r.pd_drop) <= kFrameS + 1e-9);
		CHECK(std::fabs(r.cd_off - r.pd_cycle_end) <= kFrameS + 1e-9);

		// the drop clip cuts in on the drop downbeat ...
		bool drop_cut = false;
		for (std::size_t k = 0; k < r.cd_cuts.size(); ++k)
			if (r.cd_cut_slot[k] == cd::CutScheduler::kDrop &&
			    std::fabs(r.cd_cuts[k] - r.pd_drop) <= kFrameS + 1e-9)
				drop_cut = true;
		CHECK(drop_cut);
		// ... and nothing else cuts while the stutter runs
		for (double t : r.cd_cuts)
			CHECK_FALSE((t > r.cd_on + 1e-9 && t < r.cd_off - 1e-9));
	}
}
