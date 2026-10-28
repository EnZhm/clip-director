#pragma once
// Clip Director core: Synesthesia scene pools (spec W2/W3).
//
// Three pools -- Groove, Breakdown, Drop -- hold `scene + optional preset` entries, one pool
// per section, the same shape as the clip pools. The pools live in the source settings as a
// JSON string (syn_pools), so they are saved with the scene collection.
//
// Picking uses the clip rule (spec 7): a shuffle bag per pool -- no entry repeats until every
// entry of that pool has been picked, and a refilled bag never starts with the entry that
// ended the previous pass.
#include <cstdint>
#include <string>
#include <vector>

#include "rng.h"
#include "types.h"

namespace cd {

struct SceneEntry {
	std::string scene;  // as typed, or as heard from Synesthesia
	std::string preset; // optional; case-sensitive on the wire, empty = Synesthesia's gold slot

	bool operator==(const SceneEntry &o) const { return scene == o.scene && preset == o.preset; }
	bool operator!=(const SceneEntry &o) const { return !(*this == o); }
	bool empty() const { return scene.empty(); }
	// "tunnel \"acid\"" / "tunnel" for the dock and the log
	std::string label() const;
};

struct ScenePools {
	std::vector<SceneEntry> groove, breakdown, drop;

	const std::vector<SceneEntry> &of(Section s) const;
	std::vector<SceneEntry> &of(Section s);
	size_t total() const { return groove.size() + breakdown.size() + drop.size(); }
};

// {"groove":[{"scene":"tunnel","preset":"acid"}],"breakdown":[],"drop":[]}
std::string scene_pools_to_json(const ScenePools &p);
// Tolerant reader for the shape above: unknown keys and extra fields are skipped, a malformed
// string leaves `out` untouched and returns false.
bool scene_pools_from_json(const std::string &json, ScenePools &out);

class SceneShuffle {
public:
	explicit SceneShuffle(uint32_t seed = 7);

	// Entries changed: keeps the RNG stream, rebuilds the bag.
	void set_entries(std::vector<SceneEntry> entries);
	// Next entry (consumes it); false when the pool is empty.
	bool next(SceneEntry &out);
	// What next() would return, without consuming it.
	bool peek(SceneEntry &out) const;

	size_t size() const { return entries_.size(); }
	const std::vector<SceneEntry> &entries() const { return entries_; }

private:
	void refill();
	void draw(std::vector<int> &bag, int &pick);

	std::vector<SceneEntry> entries_;
	std::vector<int> bag_; // indices still in this pass
	int last_ = -1;        // index that ended the previous pass
	Mulberry32 rng_;
};

} // namespace cd
