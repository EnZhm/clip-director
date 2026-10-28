// Scene pools: JSON round trip through the syn_pools settings string, and the shuffle bag
// (same no-repeat rule as the clip picker, spec 7).
#include "doctest.h"

#include <map>
#include <string>
#include <vector>

#include "scene_pool.h"

using namespace cd;

namespace {

SceneEntry e(const char *scene, const char *preset = "")
{
	SceneEntry s;
	s.scene = scene;
	s.preset = preset;
	return s;
}

} // namespace

TEST_CASE("scene pools: JSON round trip, including an empty pool and an empty preset")
{
	ScenePools p;
	p.groove = {e("tunnel", "acid"), e("grid")};
	p.breakdown = {e("bloom", "dreamy")};
	p.drop = {};

	const std::string json = scene_pools_to_json(p);
	CHECK(json == "{\"groove\":[{\"scene\":\"tunnel\",\"preset\":\"acid\"},{\"scene\":\"grid\",\"preset\":\"\"}],"
		      "\"breakdown\":[{\"scene\":\"bloom\",\"preset\":\"dreamy\"}],\"drop\":[]}");

	ScenePools back;
	REQUIRE(scene_pools_from_json(json, back));
	CHECK(back.groove == p.groove);
	CHECK(back.breakdown == p.breakdown);
	CHECK(back.drop.empty());
	CHECK(back.total() == 3u);
	CHECK(back.of(Section::Groove).size() == 2u);
	CHECK(back.of(Section::Breakdown).size() == 1u);
	CHECK(back.of(Section::Drop).empty());

	// quotes and backslashes survive
	ScenePools q;
	q.groove = {e("a\"b", "c\\d")};
	ScenePools q2;
	REQUIRE(scene_pools_from_json(scene_pools_to_json(q), q2));
	CHECK(q2.groove == q.groove);
	// and the same shape re-serialises identically (stable settings string)
	CHECK(scene_pools_to_json(q2) == scene_pools_to_json(q));
}

TEST_CASE("scene pools: reader is tolerant of a later version's file and of whitespace")
{
	ScenePools p;
	REQUIRE(scene_pools_from_json("  {\n \"groove\" : [ {\"scene\":\"tunnel\", \"preset\":\"acid\", \"zz\":1} ],"
				      " \"unknown\":{\"a\":[1,2,\"x\"]}, \"drop\":[], \"breakdown\":[] } ",
				      p));
	REQUIRE(p.groove.size() == 1u);
	CHECK(p.groove[0].scene == "tunnel");
	CHECK(p.groove[0].preset == "acid");
	CHECK(p.breakdown.empty());
	CHECK(p.drop.empty());

	// entries without a scene name are dropped; preset-only entries are not usable
	ScenePools q;
	REQUIRE(scene_pools_from_json("{\"groove\":[{\"preset\":\"x\"},{\"scene\":\"ok\"}]}", q));
	REQUIRE(q.groove.size() == 1u);
	CHECK(q.groove[0].scene == "ok");
}

TEST_CASE("scene pools: malformed JSON is refused and leaves the pools alone")
{
	ScenePools p;
	p.groove = {e("keep")};
	CHECK_FALSE(scene_pools_from_json("", p));
	CHECK_FALSE(scene_pools_from_json("not json", p));
	CHECK_FALSE(scene_pools_from_json("{\"groove\":[", p));
	CHECK_FALSE(scene_pools_from_json("{\"groove\":[\"tunnel\"]}", p));        // array of strings, not entries
	CHECK_FALSE(scene_pools_from_json("{\"groove\":[{\"scene\" \"x\"}]}", p)); // missing colon
	CHECK_FALSE(scene_pools_from_json("[]", p));
	CHECK(p.groove.size() == 1u);
	CHECK(p.groove[0].scene == "keep");
}

TEST_CASE("scene shuffle: no entry repeats until the pool is exhausted")
{
	SceneShuffle sh(11);
	sh.set_entries({e("a"), e("b"), e("c"), e("d")});
	std::vector<std::vector<SceneEntry>> passes;
	std::vector<SceneEntry> pass;
	for (int i = 0; i < 40; ++i) {
		SceneEntry s;
		REQUIRE(sh.next(s));
		pass.push_back(s);
		if (pass.size() == 4) {
			passes.push_back(pass);
			pass.clear();
		}
	}
	REQUIRE(passes.size() == 10u);
	for (const auto &p : passes) {
		std::map<std::string, int> seen;
		for (const SceneEntry &s : p)
			seen[s.scene]++;
		CHECK(seen.size() == 4u); // every entry exactly once per pass
		for (const auto &kv : seen)
			CHECK(kv.second == 1);
	}
	// a refilled bag never starts with the entry that ended the previous pass
	for (size_t i = 1; i < passes.size(); ++i)
		CHECK(passes[i][0] != passes[i - 1][3]);
}

TEST_CASE("scene shuffle: same seed gives the same order, different seeds differ")
{
	SceneShuffle a(7), b(7), c(8);
	const std::vector<SceneEntry> pool = {e("a"), e("b"), e("c"), e("d"), e("e")};
	a.set_entries(pool);
	b.set_entries(pool);
	c.set_entries(pool);
	std::vector<std::string> oa, ob, oc;
	for (int i = 0; i < 30; ++i) {
		SceneEntry s;
		REQUIRE(a.next(s));
		oa.push_back(s.scene);
		REQUIRE(b.next(s));
		ob.push_back(s.scene);
		REQUIRE(c.next(s));
		oc.push_back(s.scene);
	}
	CHECK(oa == ob);
	CHECK(oa != oc);
}

TEST_CASE("scene shuffle: peek shows the next entry without consuming it, and small pools work")
{
	SceneShuffle sh(3);
	sh.set_entries({e("a"), e("b")});
	SceneEntry peeked, drawn;
	REQUIRE(sh.peek(peeked));
	REQUIRE(sh.next(drawn));
	CHECK(peeked == drawn);
	REQUIRE(sh.peek(peeked));
	CHECK(peeked != drawn); // the bag moved on

	// one entry: it repeats, as a pool of one must
	SceneShuffle one(3);
	one.set_entries({e("only")});
	for (int i = 0; i < 5; ++i) {
		SceneEntry s;
		REQUIRE(one.next(s));
		CHECK(s.scene == "only");
	}

	// empty pool: nothing to draw
	SceneShuffle none(3);
	none.set_entries({});
	SceneEntry s;
	CHECK_FALSE(none.next(s));
	CHECK_FALSE(none.peek(s));

	// editing the pool rebuilds the bag and keeps drawing
	none.set_entries({e("x", "gold")});
	REQUIRE(none.next(s));
	CHECK(s.label() == "x \"gold\"");
	CHECK(e("x").label() == "x");
}
