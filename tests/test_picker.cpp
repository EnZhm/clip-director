#include "doctest.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "picker.h"

using namespace cd;

namespace {

std::vector<Clip> make_clips(int folder, int n, double dur = 60.0, double kf = 0.5, int first = 0)
{
	std::vector<Clip> v;
	for (int i = 0; i < n; ++i) {
		Clip c;
		c.path = "f" + std::to_string(folder) + "/c" + std::to_string(first + i) + ".mp4";
		c.folder = folder;
		c.duration = dur;
		c.keyframe_s = kf;
		v.push_back(c);
	}
	return v;
}

Folder fold(double w = 1.0, Tag t = Tag::Any)
{
	Folder f;
	f.weight = w;
	f.tag = t;
	return f;
}

} // namespace

TEST_CASE("shuffle bag: no repeat until the folder has played through, no repeat across passes")
{
	ClipPicker p({fold()}, make_clips(0, 10), PickerConfig{});
	std::map<int, int> count;
	int prev = -1;
	for (int pass = 0; pass < 3; ++pass) {
		std::set<int> seen;
		for (int i = 0; i < 10; ++i) {
			const Pick k = p.next(Section::Groove, 8.0);
			REQUIRE(k.clip >= 0);
			CHECK(seen.insert(k.clip).second); // unique within the pass
			CHECK(k.clip != prev);             // also holds across the pass boundary
			prev = k.clip;
			count[k.clip]++;
		}
	}
	for (const auto &kv : count)
		CHECK(kv.second == 3);
}

TEST_CASE("folder run limit and weights")
{
	std::vector<Clip> clips = make_clips(0, 20);
	auto b = make_clips(1, 20, 60.0, 0.5, 100);
	clips.insert(clips.end(), b.begin(), b.end());
	{
		ClipPicker p({fold(), fold()}, clips, PickerConfig{});
		int last = -1;
		for (int i = 0; i < 200; ++i) {
			const int f = clips[p.next(Section::Groove, 8.0).clip].folder;
			CHECK(f != last);
			last = f;
		}
	}
	{
		PickerConfig cfg;
		cfg.max_folder_run = 1000; // weights only
		ClipPicker p({fold(3.0), fold(1.0)}, clips, cfg);
		int n0 = 0;
		const int n = 4000;
		for (int i = 0; i < n; ++i)
			n0 += clips[p.next(Section::Groove, 8.0).clip].folder == 0;
		CHECK(n0 / double(n) == doctest::Approx(0.75).epsilon(0.05));
	}
}

TEST_CASE("section pools")
{
	std::vector<Folder> f = {fold(1, Tag::Any), fold(1, Tag::Breakdown), fold(1, Tag::Groove)};
	ClipPicker p(f, {}, PickerConfig{});
	CHECK(p.pool_folders(Section::Breakdown) == std::vector<int>{1}); // exclusive when tagged
	CHECK(p.pool_folders(Section::Groove) == std::vector<int>{0, 2}); // groove always adds Any
	CHECK(p.pool_folders(Section::Drop) == std::vector<int>{0});      // no drop folder: Any
	std::vector<Folder> g = {fold(1, Tag::Breakdown)};
	CHECK(ClipPicker(g, {}, PickerConfig{}).pool_folders(Section::Drop) == std::vector<int>{0}); // last resort: all
	f[1].enabled = false;
	ClipPicker q(f, {}, PickerConfig{});
	CHECK(q.pool_folders(Section::Breakdown) == std::vector<int>{0});
}

TEST_CASE("banned clips, disabled folders and too-short clips are never picked")
{
	std::vector<Clip> clips = make_clips(0, 4);
	clips[2].banned = true;
	auto shorts = make_clips(1, 3, 2.0); // 2 s clips
	clips.insert(clips.end(), shorts.begin(), shorts.end());
	PickerConfig cfg;
	cfg.max_folder_run = 1000;
	ClipPicker p({fold(), fold(100.0)}, clips, cfg);
	for (int i = 0; i < 100; ++i) {
		const Pick k = p.next(Section::Groove, 8.0); // 8 s hold: a 2 s clip would loop 3x > 2
		REQUIRE(k.clip >= 0);
		CHECK(k.clip != 2);
		CHECK(clips[k.clip].folder == 0);
	}
	// 6 s clip at an 8 s hold loops once: allowed, flagged
	std::vector<Clip> one = make_clips(0, 1, 6.0);
	ClipPicker q({fold()}, one, PickerConfig{});
	const Pick k = q.next(Section::Groove, 8.0);
	CHECK(k.clip == 0);
	CHECK(k.loops);
	CHECK(k.inpoint == 0.0);
	// nothing playable at all
	std::vector<Folder> off = {fold()};
	off[0].enabled = false;
	CHECK(ClipPicker(off, make_clips(0, 3), PickerConfig{}).next(Section::Groove, 8.0).clip == -1);
}

TEST_CASE("start points sit on the keyframe grid and avoid recently used segments")
{
	ClipPicker p({fold()}, make_clips(0, 1, 60.0, 0.5), PickerConfig{});
	std::vector<double> ins;
	for (int i = 0; i < 4; ++i) {
		const Pick k = p.next(Section::Groove, 8.0);
		CHECK(std::fabs(k.inpoint / 0.5 - std::round(k.inpoint / 0.5)) < 1e-9);
		CHECK(k.inpoint + 8.0 <= 60.0 + 1e-9);
		for (double r : ins)
			CHECK(std::fabs(k.inpoint - r) >= 8.0); // 60 s leaves room for 4 disjoint segments
		ins.push_back(k.inpoint);
	}
}

TEST_CASE("speed variation is quantised and in range; same seed, same sequence")
{
	PickerConfig cfg;
	cfg.speed_min = 0.75;
	cfg.speed_max = 1.25;
	cfg.speed_extreme_chance = 0.1;
	ClipPicker a({fold()}, make_clips(0, 30), cfg), b({fold()}, make_clips(0, 30), cfg);
	cfg.seed = 8;
	ClipPicker c({fold()}, make_clips(0, 30), cfg);
	int extremes = 0, diff = 0;
	for (int i = 0; i < 300; ++i) {
		const Pick x = a.next(Section::Groove, 8.0), y = b.next(Section::Groove, 8.0),
			   z = c.next(Section::Groove, 8.0);
		CHECK(std::fabs(x.speed / 0.05 - std::round(x.speed / 0.05)) < 1e-9);
		const bool extreme = x.speed == 0.5 || x.speed == 2.0;
		extremes += extreme;
		if (!extreme)
			CHECK((x.speed >= 0.75 - 1e-9 && x.speed <= 1.25 + 1e-9));
		CHECK(x.clip == y.clip);
		CHECK(x.inpoint == y.inpoint);
		CHECK(x.speed == y.speed);
		diff += x.clip != z.clip;
	}
	CHECK(extremes > 10);
	CHECK(extremes < 60);
	CHECK(diff > 200);
}

TEST_CASE("unknown duration (raw clip, no ingest): playable, starts at 0, loops")
{
	std::vector<Clip> clips = make_clips(0, 3, 0.0, 0.0);
	ClipPicker p({fold()}, clips, PickerConfig{});
	for (int i = 0; i < 6; ++i) {
		const Pick k = p.next(Section::Groove, 30.0);
		REQUIRE(k.clip >= 0);
		CHECK(k.inpoint == 0.0);
		CHECK(k.loops);
	}
}
