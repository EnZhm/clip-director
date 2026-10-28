#include "doctest.h"

#include <string>
#include <vector>

#include "midi_map.h"

using namespace cd::midi;

namespace {

Target target(const std::string &name, Registerer r = Registerer::Frontend, const std::string &reg = "",
	      const std::string &desc = "a hotkey")
{
	Target t;
	t.name = name;
	t.registerer = r;
	t.registerer_name = reg;
	t.description = desc;
	return t;
}

Control control(Type type, int channel, int number)
{
	Control c;
	c.type = type;
	c.channel = channel;
	c.number = number;
	return c;
}

std::vector<uint8_t> bytes(std::initializer_list<int> v)
{
	std::vector<uint8_t> out;
	for (int b : v)
		out.push_back((uint8_t)b);
	return out;
}

Message parsed(std::initializer_list<int> v)
{
	const std::vector<uint8_t> b = bytes(v);
	Message m;
	REQUIRE(parse_message(b.data(), b.size(), m));
	return m;
}

size_t presses(const std::vector<Event> &events)
{
	size_t n = 0;
	for (const Event &e : events)
		n += e.pressed ? 1 : 0;
	return n;
}

} // namespace

TEST_CASE("short message parsing: note on/off, velocity 0 is off, channel")
{
	const Message on = parsed({0x90, 36, 100});
	CHECK(on.type == Type::Note);
	CHECK(on.channel == 1);
	CHECK(on.number == 36);
	CHECK(on.value == 100);
	CHECK(on.pressed);

	// note on with velocity 0 is the note-off convention
	const Message off0 = parsed({0x90, 36, 0});
	CHECK(off0.type == Type::Note);
	CHECK(off0.number == 36);
	CHECK(off0.value == 0);
	CHECK_FALSE(off0.pressed);

	const Message off = parsed({0x85, 60, 64});
	CHECK(off.type == Type::Note);
	CHECK(off.channel == 6);
	CHECK(off.number == 60);
	CHECK(off.value == 64);
	CHECK_FALSE(off.pressed); // release velocity, still a release

	// the control's identity survives the message: same note on another channel is another control
	CHECK(parsed({0x9F, 36, 1}).channel == 16);
	CHECK_FALSE(Control::of(parsed({0x90, 36, 1})) == Control::of(parsed({0x91, 36, 1})));
}

TEST_CASE("short message parsing: control change presses at 64 and releases below")
{
	const Message on = parsed({0xB0, 7, 127});
	CHECK(on.type == Type::Control);
	CHECK(on.channel == 1);
	CHECK(on.number == 7);
	CHECK(on.value == 127);
	CHECK(on.pressed);

	CHECK(parsed({0xB3, 7, 64}).pressed); // exactly the threshold presses
	CHECK_FALSE(parsed({0xB3, 7, 63}).pressed);
	CHECK_FALSE(parsed({0xB0, 7, 0}).pressed);
	CHECK(parsed({0xB2, 64, 65}).pressed); // controller 64 (sustain) is nothing special here
}

TEST_CASE("short message parsing rejects what we do not act on")
{
	Message m;
	REQUIRE_FALSE(parse_message(nullptr, 3, m));
	REQUIRE_FALSE(parse_message(bytes({0x90, 36, 100}).data(), 2, m));  // truncated
	REQUIRE_FALSE(parse_message(bytes({0x36, 36, 100}).data(), 3, m));  // data byte as status
	REQUIRE_FALSE(parse_message(bytes({0xF0, 0, 0}).data(), 3, m));     // system
	REQUIRE_FALSE(parse_message(bytes({0xE0, 0, 64}).data(), 3, m));    // pitch bend
	REQUIRE_FALSE(parse_message(bytes({0xC0, 5, 0}).data(), 3, m));     // program change
	REQUIRE_FALSE(parse_message(bytes({0x90, 200, 100}).data(), 3, m)); // data out of range
}

TEST_CASE("assembler frames chunks, running status, real-time bytes and sysex")
{
	Assembler a;
	std::vector<Message> out;

	a.feed(bytes({0x90, 36, 100}).data(), 3, out);
	REQUIRE(out.size() == 1);
	CHECK(out[0].number == 36);
	CHECK(out[0].pressed);
	out.clear();

	// a message split across two callbacks
	a.feed(bytes({0x90}).data(), 1, out);
	CHECK(out.empty());
	a.feed(bytes({40}).data(), 1, out);
	CHECK(out.empty());
	a.feed(bytes({1}).data(), 1, out);
	REQUIRE(out.size() == 1);
	CHECK(out[0].number == 40);
	out.clear();

	// running status: two more note messages with only data bytes, plus an interleaved clock byte
	a.feed(bytes({41, 2, 0xF8, 42, 3}).data(), 5, out);
	REQUIRE(out.size() == 2);
	CHECK(out[0].number == 41);
	CHECK(out[1].number == 42);
	out.clear();

	// sysex and its payload are ignored, and so is the data byte after a system common message
	a.feed(bytes({0xF0, 0x7E, 0x00, 0x06, 0x01, 0xF7, 0x90, 44, 9}).data(), 9, out);
	REQUIRE(out.size() == 1);
	CHECK(out[0].number == 44);
	out.clear();

	// a new status byte before the previous message is complete drops the partial one
	a.feed(bytes({0x90, 36, 0xB0, 7, 100}).data(), 5, out);
	REQUIRE(out.size() == 1);
	CHECK(out[0].type == Type::Control);
	CHECK(out[0].number == 7);
	out.clear();

	// reset forgets a partial message
	a.feed(bytes({0x90, 36}).data(), 2, out);
	a.reset();
	a.feed(bytes({100}).data(), 1, out);
	CHECK(out.empty());

	// messages we do not act on (program change) are consumed, the stream keeps its place
	a.feed(bytes({0xC0, 5, 0x90, 50, 10}).data(), 5, out);
	REQUIRE(out.size() == 1);
	CHECK(out[0].number == 50);
}

TEST_CASE("mapping: one control -> one target, rebinding replaces")
{
	Map m;
	const Control c = control(Type::Note, 1, 36);
	m.bind(c, target("cd_skip", Registerer::Frontend, "", "Skip"));
	REQUIRE(m.size() == 1);
	REQUIRE(m.bound(c));
	REQUIRE(m.find(c) != nullptr);
	CHECK(m.find(c)->target.name == "cd_skip");
	CHECK(m.find(c)->mode == Mode::Momentary);

	m.bind(c, target("cd_ban"), Mode::Toggle);
	CHECK(m.size() == 1);
	CHECK(m.find(c)->target.name == "cd_ban");
	CHECK(m.find(c)->mode == Mode::Toggle);

	// a second control may drive the same hotkey
	m.bind(control(Type::Control, 1, 7), target("cd_ban"));
	CHECK(m.size() == 2);

	CHECK(m.unbind(c));
	CHECK_FALSE(m.bound(c));
	CHECK_FALSE(m.unbind(c));
	CHECK(m.size() == 1);
	m.clear();
	CHECK(m.empty());
}

TEST_CASE("notes pass press and release through")
{
	Map m;
	m.bind(control(Type::Note, 1, 36), target("cd_skip"));

	std::vector<Event> e = m.handle(parsed({0x90, 36, 100}));
	REQUIRE(e.size() == 1);
	CHECK(e[0].pressed);
	CHECK(e[0].target.name == "cd_skip");

	e = m.handle(parsed({0x80, 36, 0}));
	REQUIRE(e.size() == 1);
	CHECK_FALSE(e[0].pressed);

	// note on with velocity 0 releases too
	m.handle(parsed({0x90, 36, 100}));
	e = m.handle(parsed({0x90, 36, 0}));
	REQUIRE(e.size() == 1);
	CHECK_FALSE(e[0].pressed);

	// unmapped controls produce nothing
	CHECK(m.handle(parsed({0x90, 37, 100})).empty());
	// and another channel is another control
	CHECK(m.handle(parsed({0x91, 36, 100})).empty());
}

TEST_CASE("control change is edge-triggered at 64")
{
	Map m;
	m.bind(control(Type::Control, 1, 7), target("pd_drop_now"));

	std::vector<Event> e = m.handle(parsed({0xB0, 7, 127}));
	REQUIRE(e.size() == 1);
	CHECK(e[0].pressed);

	// a controller that keeps sending high values does not repeat the press
	CHECK(m.handle(parsed({0xB0, 7, 127})).empty());
	CHECK(m.handle(parsed({0xB0, 7, 64})).empty());

	e = m.handle(parsed({0xB0, 7, 63}));
	REQUIRE(e.size() == 1);
	CHECK_FALSE(e[0].pressed);

	// and a controller that keeps sending low values does not repeat the release
	CHECK(m.handle(parsed({0xB0, 7, 0})).empty());

	e = m.handle(parsed({0xB0, 7, 65}));
	REQUIRE(e.size() == 1);
	CHECK(e[0].pressed);
	CHECK(presses(e) == 1);

	// a fresh device (or a panic) starts from released
	m.reset_runtime();
	e = m.handle(parsed({0xB0, 7, 100}));
	REQUIRE(e.size() == 1);
	CHECK(e[0].pressed);

	// unbinding drops the runtime state with the binding
	m.unbind(control(Type::Control, 1, 7));
	CHECK(m.handle(parsed({0xB0, 7, 100})).empty());
}

TEST_CASE("toggle mode flips the target on press edges")
{
	Map m;
	m.bind(control(Type::Control, 1, 20), target("cd_hold"), Mode::Toggle);

	std::vector<Event> e = m.handle(parsed({0xB0, 20, 127}));
	REQUIRE(e.size() == 1);
	CHECK(e[0].pressed);
	CHECK(m.handle(parsed({0xB0, 20, 127})).empty()); // still held: no second flip
	CHECK(m.handle(parsed({0xB0, 20, 0})).empty());   // release edge does not flip

	e = m.handle(parsed({0xB0, 20, 64}));
	REQUIRE(e.size() == 1);
	CHECK_FALSE(e[0].pressed);
	CHECK(m.handle(parsed({0xB0, 20, 0})).empty()); // release edge: nothing to flip
	e = m.handle(parsed({0xB0, 20, 80}));
	REQUIRE(e.size() == 1);
	CHECK(e[0].pressed);

	// a note in toggle mode flips on note-on and ignores note-off
	Map n;
	n.bind(control(Type::Note, 1, 36), target("cd_hold"), Mode::Toggle);
	e = n.handle(parsed({0x90, 36, 1}));
	REQUIRE(e.size() == 1);
	CHECK(e[0].pressed);
	CHECK(n.handle(parsed({0x80, 36, 0})).empty());
	e = n.handle(parsed({0x90, 36, 1}));
	REQUIRE(e.size() == 1);
	CHECK_FALSE(e[0].pressed);
}

TEST_CASE("json round trip keeps control, target identity and mode")
{
	Map m;
	m.bind(control(Type::Note, 1, 36), target("cd_skip", Registerer::Frontend, "", "Skip to the next clip"));
	m.bind(control(Type::Control, 3, 7), target("pd_tap_tempo", Registerer::Frontend, "", "Tap tempo"),
	       Mode::Toggle);
	m.bind(control(Type::Note, 16, 60),
	       target("pd_stutter", Registerer::Source, "decks/left", "Stutter \"quote\" \\ slash\nnewline"));

	const std::string json = m.to_json();
	CHECK(json.find("\"version\":1") != std::string::npos);

	Map back;
	REQUIRE(back.from_json(json));
	REQUIRE(back.size() == 3);
	CHECK(back.mappings() == m.mappings());
	CHECK(back.find(control(Type::Control, 3, 7))->mode == Mode::Toggle);
	CHECK(back.find(control(Type::Note, 16, 60))->target.registerer == Registerer::Source);
	CHECK(back.find(control(Type::Note, 16, 60))->target.registerer_name == "decks/left");
	CHECK(back.find(control(Type::Note, 16, 60))->target.description == "Stutter \"quote\" \\ slash\nnewline");

	// the mapping order is stable (sorted by control), so saved files do not churn
	CHECK(back.mappings()[0].control == control(Type::Note, 1, 36));
	CHECK(back.mappings()[1].control == control(Type::Note, 16, 60));
	CHECK(back.mappings()[2].control == control(Type::Control, 3, 7));

	// and the runtime state is fresh after a load
	CHECK(back.handle(parsed({0x90, 36, 100})).size() == 1);
}

TEST_CASE("json: empty map, bad input, skipped rows")
{
	Map empty;
	CHECK(empty.to_json() == "{\"version\":1,\"mappings\":[]}");
	REQUIRE(empty.from_json(empty.to_json()));
	CHECK(empty.empty());

	Map m;
	m.bind(control(Type::Note, 1, 36), target("cd_skip"));
	const std::string good = m.to_json();

	std::string err;
	CHECK_FALSE(m.from_json("{", &err));
	CHECK_FALSE(err.empty());
	CHECK_FALSE(m.from_json("{\"mappings\":[}", &err));
	CHECK_FALSE(m.from_json("{\"version\":1,\"mappings\":[]} trailing", &err));
	CHECK_FALSE(m.from_json("{\"mappings\":[{\"type\":\"note\",\"channel\":1,\"number\":36,}]}", &err));
	// a failed parse leaves the map untouched
	REQUIRE(m.size() == 1);
	CHECK(m.find(control(Type::Note, 1, 36)) != nullptr);

	// rows that are out of range or incomplete are dropped, the rest survives
	REQUIRE(m.from_json("{\"version\":1,\"mappings\":["
			    "{\"type\":\"note\",\"channel\":1,\"number\":36,\"name\":\"cd_skip\"},"
			    "{\"type\":\"cc\",\"channel\":17,\"number\":7,\"name\":\"x\"},"
			    "{\"type\":\"cc\",\"channel\":1,\"number\":200,\"name\":\"x\"},"
			    "{\"type\":\"wave\",\"channel\":1,\"number\":1,\"name\":\"x\"},"
			    "{\"type\":\"note\",\"channel\":1,\"name\":\"x\"},"
			    "{\"type\":\"note\",\"channel\":2,\"number\":40},"
			    "{\"type\":\"note\",\"channel\":3,\"number\":41,\"name\":\"\",\"mode\":\"sideways\"},"
			    "{\"type\":\"note\",\"channel\":4,\"number\":42,\"name\":\"ok\",\"future\":{\"a\":[1,2]}}"
			    "]}",
			    &err));
	CHECK(err.empty());
	REQUIRE(m.size() == 2);
	CHECK(m.find(control(Type::Note, 1, 36)) != nullptr);
	CHECK(m.find(control(Type::Note, 4, 42)) != nullptr);

	// a later row for the same control replaces the earlier one
	REQUIRE(m.from_json("{\"version\":1,\"mappings\":["
			    "{\"type\":\"note\",\"channel\":1,\"number\":36,\"name\":\"first\"},"
			    "{\"type\":\"note\",\"channel\":1,\"number\":36,\"name\":\"second\"}]}"));
	REQUIRE(m.size() == 1);
	CHECK(m.find(control(Type::Note, 1, 36))->target.name == "second");

	// an unknown registerer falls back to the frontend instead of losing the row
	REQUIRE(m.from_json("{\"version\":1,\"mappings\":[{\"type\":\"note\",\"channel\":1,\"number\":36,"
			    "\"name\":\"cd_skip\",\"registerer\":\"astral\"}]}"));
	CHECK(m.find(control(Type::Note, 1, 36))->target.registerer == Registerer::Frontend);

	// non-ascii descriptions survive (localised hotkey names)
	Map u;
	u.bind(control(Type::Note, 1, 36), target("cd_skip", Registerer::Frontend, "", "Überspringen – 次へ"));
	Map u2;
	REQUIRE(u2.from_json(u.to_json()));
	CHECK(u2.find(control(Type::Note, 1, 36))->target.description ==
	      u.find(control(Type::Note, 1, 36))->target.description);
}

TEST_CASE("learn: arm a target, the next control press binds it")
{
	Map m;
	Learn learn;
	CHECK_FALSE(learn.armed());
	CHECK(learn.target() == nullptr);

	learn.arm(target("cd_skip", Registerer::Frontend, "", "Skip"));
	REQUIRE(learn.armed());
	REQUIRE(learn.target() != nullptr);
	CHECK(learn.target()->name == "cd_skip");

	// a release does not bind
	CHECK(learn.feed(parsed({0x90, 36, 0}), m) == nullptr);
	CHECK(learn.armed());

	const Control *bound = learn.feed(parsed({0x90, 36, 100}), m);
	REQUIRE(bound != nullptr);
	CHECK(*bound == control(Type::Note, 1, 36));
	CHECK_FALSE(learn.armed());
	REQUIRE(m.size() == 1);
	CHECK(m.find(control(Type::Note, 1, 36))->target.name == "cd_skip");

	// once armed again, the next press lands wherever the user moves a control
	learn.arm(target("cd_hold"), Mode::Toggle);
	REQUIRE(learn.armed());
	bound = learn.feed(parsed({0xB0, 1, 127}), m);
	REQUIRE(bound != nullptr);
	CHECK(*bound == control(Type::Control, 1, 1));
	CHECK(m.find(control(Type::Control, 1, 1))->mode == Mode::Toggle);

	// learning the same control again replaces the binding (one control -> one target)
	learn.arm(target("cd_ban"));
	REQUIRE(learn.feed(parsed({0x90, 36, 5}), m) != nullptr);
	CHECK(m.size() == 2);
	CHECK(m.find(control(Type::Note, 1, 36))->target.name == "cd_ban");

	// cancel leaves nothing armed
	learn.arm(target("cd_skip"));
	learn.cancel();
	CHECK_FALSE(learn.armed());
	CHECK(learn.feed(parsed({0x90, 42, 100}), m) == nullptr);
	CHECK(m.size() == 2);
}

TEST_CASE("labels name the control the way a picker would")
{
	CHECK(control(Type::Note, 1, 36).label() == "Note 36 ch 1");
	CHECK(control(Type::Control, 16, 7).label() == "CC 7 ch 16");
	CHECK(std::string(type_to_string(Type::Note)) == "note");
	CHECK(std::string(type_to_string(Type::Control)) == "cc");
	CHECK(std::string(mode_to_string(Mode::Toggle)) == "toggle");
	CHECK(std::string(registerer_to_string(Registerer::Source)) == "source");
}
