#include "picker.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace cd {

namespace {

bool tag_matches(Tag t, Section s)
{
	return (t == Tag::Groove && s == Section::Groove) || (t == Tag::Breakdown && s == Section::Breakdown) ||
	       (t == Tag::Drop && s == Section::Drop);
}

const char *section_name(Section s)
{
	switch (s) {
	case Section::Breakdown:
		return "breakdown";
	case Section::Drop:
		return "drop";
	default:
		return "groove";
	}
}

} // namespace

ClipPicker::ClipPicker(const std::vector<Folder> &folders, const std::vector<Clip> &clips, const PickerConfig &cfg)
	: folders_(folders),
	  clips_(clips),
	  cfg_(cfg),
	  rng_(cfg.seed)
{
}

void ClipPicker::set_library(const std::vector<Folder> &folders, const std::vector<Clip> &clips)
{
	const bool same_clips = clips.size() == clips_.size();
	folders_ = folders;
	clips_ = clips;
	if (!same_clips) { // indices changed: history no longer refers to the same clips
		bag_.clear();
		last_of_pass_.clear();
		inpoints_.clear();
	}
}

void ClipPicker::set_config(const PickerConfig &cfg)
{
	const uint32_t keep = rng_.state();
	cfg_ = cfg;
	rng_.reseed(keep);
}

std::vector<int> ClipPicker::pool_folders(Section section) const
{
	std::vector<int> tagged, any, all;
	for (int i = 0; i < static_cast<int>(folders_.size()); ++i) {
		const Folder &f = folders_[i];
		if (!f.enabled || f.weight <= 0.0)
			continue;
		all.push_back(i);
		if (f.tag == Tag::Any)
			any.push_back(i);
		else if (tag_matches(f.tag, section))
			tagged.push_back(i);
	}
	if (!tagged.empty()) {
		if (section == Section::Groove)
			for (int f : any) // a loop, not insert(): gcc 13 -O2 flags a false -Wstringop-overflow
				tagged.push_back(f);
		std::sort(tagged.begin(), tagged.end());
		return tagged;
	}
	if (!any.empty())
		return any;
	return all;
}

bool ClipPicker::playable(int clip, double hold_s, double speed) const
{
	if (clip < 0 || clip >= static_cast<int>(clips_.size()))
		return false;
	const Clip &c = clips_[clip];
	if (c.banned)
		return false;
	if (c.folder < 0 || c.folder >= static_cast<int>(folders_.size()))
		return false;
	const Folder &f = folders_[c.folder];
	if (!f.enabled || f.weight <= 0.0)
		return false;
	if (c.duration <= 0.0) // unknown length (raw clip without ingest): playable, starts at 0, loops
		return true;
	const double content = hold_s * speed;
	const double loops = content / c.duration - 1.0; // 0 = plays once without looping
	return loops <= static_cast<double>(cfg_.max_loops) + 1e-9;
}

int ClipPicker::draw_from_folder(int folder, double hold_s, double speed)
{
	for (int pass = 0; pass < 2; ++pass) {
		std::vector<int> &bag = bag_[folder];
		if (bag.empty()) {
			for (int i = 0; i < static_cast<int>(clips_.size()); ++i)
				if (clips_[i].folder == folder && !clips_[i].banned)
					bag.push_back(i);
			for (int i = static_cast<int>(bag.size()) - 1; i > 0; --i) { // Fisher-Yates
				const int j = static_cast<int>(rng_.next() * (i + 1));
				std::swap(bag[i], bag[j]);
			}
			auto lp = last_of_pass_.find(folder);
			if (bag.size() > 1 && lp != last_of_pass_.end() && bag.front() == lp->second)
				std::swap(bag[0], bag[1 + static_cast<int>(rng_.next() * (bag.size() - 1))]);
		}
		for (std::size_t k = 0; k < bag.size(); ++k) {
			const int c = bag[k];
			if (!playable(c, hold_s, speed))
				continue;
			bag.erase(bag.begin() + static_cast<std::ptrdiff_t>(k));
			if (bag.empty())
				last_of_pass_[folder] = c;
			return c;
		}
		// nothing left in this pass suits the hold: start the next pass early
		if (!bag.empty())
			last_of_pass_[folder] = bag.back();
		bag.clear();
	}
	return -1;
}

double ClipPicker::pick_speed()
{
	double s;
	if (cfg_.speed_extreme_chance > 0.0 && rng_.next() < cfg_.speed_extreme_chance)
		s = rng_.next() < 0.5 ? 0.5 : 2.0;
	else
		s = cfg_.speed_min + (cfg_.speed_max - cfg_.speed_min) * rng_.next();
	s = std::round(s / 0.05) * 0.05;
	return std::max(0.05, s);
}

double ClipPicker::pick_inpoint(int clip, double hold_s, double speed)
{
	const Clip &c = clips_[clip];
	if (c.duration <= 0.0)
		return 0.0;
	const double content = hold_s * speed;
	const double max_start = c.duration - content;
	if (max_start <= 0.0)
		return 0.0;
	const double grid = c.keyframe_s > 0.0 ? c.keyframe_s : 1.0;
	const int slots = static_cast<int>(std::floor(max_start / grid + 1e-9)) + 1;
	std::deque<double> &recent = inpoints_[clip];
	double best = 0.0, best_dist = -1.0;
	for (int attempt = 0; attempt < 8; ++attempt) {
		const double cand = grid * static_cast<int>(rng_.next() * slots);
		double dist = 1e300;
		for (double r : recent)
			dist = std::min(dist, std::fabs(cand - r));
		if (dist >= content) { // no overlap with a recently used segment
			best = cand;
			best_dist = dist;
			break;
		}
		if (dist > best_dist) {
			best = cand;
			best_dist = dist;
		}
	}
	recent.push_back(best);
	while (static_cast<int>(recent.size()) > std::max(0, cfg_.inpoint_memory))
		recent.pop_front();
	return best;
}

Pick ClipPicker::next(Section section, double hold_s)
{
	Pick p;
	const double speed = pick_speed();
	std::vector<int> pool = pool_folders(section);

	// folders of the pool with at least one playable clip
	std::vector<int> cand;
	for (int f : pool) {
		for (int i = 0; i < static_cast<int>(clips_.size()); ++i)
			if (clips_[i].folder == f && playable(i, hold_s, speed)) {
				cand.push_back(f);
				break;
			}
	}
	if (cand.size() > 1 && folder_run_ >= cfg_.max_folder_run) {
		auto it = std::find(cand.begin(), cand.end(), last_folder_);
		if (it != cand.end())
			cand.erase(it);
	}

	while (!cand.empty()) {
		double total = 0.0;
		for (int f : cand)
			total += folders_[f].weight;
		double r = rng_.next() * total;
		std::size_t pick = cand.size() - 1;
		for (std::size_t i = 0; i < cand.size(); ++i) {
			r -= folders_[cand[i]].weight;
			if (r < 0.0) {
				pick = i;
				break;
			}
		}
		const int folder = cand[pick];
		const int clip = draw_from_folder(folder, hold_s, speed);
		if (clip < 0) {
			cand.erase(cand.begin() + static_cast<std::ptrdiff_t>(pick));
			continue;
		}
		folder_run_ = folder == last_folder_ ? folder_run_ + 1 : 1;
		last_folder_ = folder;
		p.clip = clip;
		p.speed = speed;
		p.inpoint = pick_inpoint(clip, hold_s, speed);
		p.loops = clips_[clip].duration <= 0.0 || hold_s * speed > clips_[clip].duration - p.inpoint + 1e-9;
		char buf[160];
		std::snprintf(buf, sizeof(buf), "%s pool, folder %d, %zu left in bag, in %.1fs, x%.2f%s",
			      section_name(section), folder, bag_[folder].size(), p.inpoint, p.speed,
			      p.loops ? ", loops" : "");
		p.reason = buf;
		return p;
	}
	p.reason = std::string(section_name(section)) + " pool: nothing playable";
	return p;
}

} // namespace cd
