#include "doctest.h"

#include <cmath>
#include <cstdio>
#include <set>

#include "levels.h"
#include "picker.h"
#include "scheduler.h"

using namespace cd;

namespace {

struct Row {
	double dur, mean, p02, p50, p98;
};
const Row kCyber[] = {
#include "cyber_clips.inc"
};

} // namespace

// End to end on the real "cyber" library (264 clips, measured stats): a 2-hour set at 128 BPM.
TEST_CASE("cyber library: 2-hour simulated set")
{
	std::vector<Clip> clips;
	for (const Row &r : kCyber) {
		Clip c;
		c.duration = r.dur;
		c.keyframe_s = 0.5; // after ingest (GOP 15 @ 30 fps)
		c.stats.has = true;
		c.stats.mean = r.mean;
		c.stats.p02 = r.p02;
		c.stats.p50 = r.p50;
		c.stats.p98 = r.p98;
		clips.push_back(c);
	}
	REQUIRE(clips.size() == 264);
	Folder f;
	ClipPicker picker({f}, clips, PickerConfig{});
	CutScheduler sched(SchedulerConfig{});
	LevelConfig lc;

	// 64 bars groove, 16 breakdown, 32 drop, repeat
	auto section = [](double b) {
		const double m = std::fmod(b, 448.0);
		return m < 256 ? Section::Groove : (m < 320 ? Section::Breakdown : Section::Drop);
	};
	std::vector<int> picks;
	int cuts = 0, inverted = 0, nothing = 0;
	const double per_frame = 128.0 / 60.0 / 30.0; // 30 fps is enough for scheduling
	for (double b = 0; b < 128.0 * 120; b += per_frame) {
		ClockSample c;
		c.valid = true;
		c.beats = b;
		c.bpm = 128.0;
		c.section = section(b);
		for (const auto &e : sched.tick(c)) {
			if (e.type == SchedEvent::Type::Prepare) {
				const Pick p = picker.next(e.section, e.hold_s);
				if (p.clip < 0) {
					nothing++;
					continue;
				}
				picks.push_back(p.clip);
				LevelTracker t;
				t.reset(clips[p.clip], lc);
				inverted += t.invert();
			}
			cuts += e.type == SchedEvent::Type::Cut;
		}
	}
	CHECK(nothing == 0);
	CHECK(cuts > 700);
	// deep shuffle: the first pass plays every clip that fits the holds exactly once
	std::set<int> first;
	std::size_t i = 0;
	for (; i < picks.size() && first.insert(picks[i]).second; ++i) {
	}
	CHECK(i >= 260); // only the 1.9 s clip can never fit (it would loop more than twice)
	// inversion stays the exception (the library is mostly dark)
	CHECK(inverted / double(picks.size()) < 0.10);
	std::printf("cyber set: %zu picks, %d cuts, first repeat after %zu picks, %.1f%% inverted\n", picks.size(),
		    cuts, i, 100.0 * inverted / picks.size());
}
