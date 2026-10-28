// OSC 1.0 codec: golden byte tests, round trips and malformed-packet rejection.
#include "doctest.h"

#include <cmath>
#include <string>
#include <vector>

#include "osc.h"

using namespace cd;

namespace {

/* every literal is a byte on the wire, zeros included, so the padding rule is pinned down */
std::vector<uint8_t> bytes(std::initializer_list<unsigned int> v)
{
	std::vector<uint8_t> o;
	for (unsigned int b : v)
		o.push_back(static_cast<uint8_t>(b));
	return o;
}

std::vector<uint8_t> encode(const OscMessage &m)
{
	std::vector<uint8_t> out;
	REQUIRE(osc_encode(m, out));
	return out;
}

} // namespace

TEST_CASE("osc: padding is to the next 4-byte boundary")
{
	CHECK(osc_padded(0) == 0);
	CHECK(osc_padded(1) == 4);
	CHECK(osc_padded(4) == 4);
	CHECK(osc_padded(5) == 8);
	CHECK(osc_padded(18) == 20);
}

TEST_CASE("osc: golden bytes for /scenes/{scene} {preset}")
{
	// "/scenes/gridtunnel" (18 + NUL -> 20), ",s\0\0" (4), "acid\0\0\0\0" (8) = 32
	const std::vector<uint8_t> want = bytes({
		'/', 's', 'c',  'e',  'n', 'e', 's',  '/',  'g', 'r', 'i', 'd', 't',  'u',  'n',  'n',
		'e', 'l', 0x00, 0x00, ',', 's', 0x00, 0x00, 'a', 'c', 'i', 'd', 0x00, 0x00, 0x00, 0x00,
	});
	const std::vector<uint8_t> got = encode(osc_scene("Grid Tunnel", "acid"));
	CHECK(got == want);
	CHECK(got.size() == 32u);

	// no preset: Synesthesia then uses its custom gold slot, so there is no second argument
	const std::vector<uint8_t> plain = encode(osc_scene("tunnel", ""));
	CHECK(plain == bytes({'/', 's', 'c', 'e', 'n',  'e',  's', '/',  't',  'u',
			      'n', 'n', 'e', 'l', 0x00, 0x00, ',', 0x00, 0x00, 0x00}));
	CHECK(plain.size() == 20u);
}

TEST_CASE("osc: golden bytes for the bang, meta and playlist routes")
{
	// "/controls/global/bang/1" (23 + NUL -> 24), ",f\0\0", 1.0f = 3F 80 00 00
	CHECK(encode(osc_bang(1)) ==
	      bytes({'/', 'c', 'o', 'n', 't', 'r', 'o', 'l',  's', '/', 'g',  'l',  'o',  'b',  'a',  'l',
		     '/', 'b', 'a', 'n', 'g', '/', '1', 0x00, ',', 'f', 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00}));
	// "/controls/meta/hue" (18 + NUL -> 20), ",f\0\0", 0.5f = 3F 00 00 00
	CHECK(encode(osc_meta("hue", 0.5f)) ==
	      bytes({'/', 'c', 'o', 'n', 't',  'r',  'o', 'l', 's',  '/',  'm',  'e',  't',  'a',
		     '/', 'h', 'u', 'e', 0x00, 0x00, ',', 'f', 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00}));
	// an address-only message: the padded address and an empty type tag string (just the comma)
	CHECK(encode(osc_playlist_next()) == bytes({'/', 'p', 'l', 'a', 'y',  'l',  'i', 's',  't',  '/',
						    'n', 'e', 'x', 't', 0x00, 0x00, ',', 0x00, 0x00, 0x00}));
	// "/presets" (8 + NUL -> 12), ",s\0\0", "dreamy" (7 -> 8)
	CHECK(encode(osc_preset("dreamy")) ==
	      bytes({'/', 'p', 'r',  'e',  's', 'e', 't', 's', 0x00, 0x00, 0x00, 0x00,
		     ',', 's', 0x00, 0x00, 'd', 'r', 'e', 'a', 'm',  'y',  0x00, 0x00}));
}

TEST_CASE("osc: encode/decode round trip")
{
	const OscMessage in[] = {
		osc_scene("tunnel", "acid"),   osc_scene("grid", ""), osc_preset("dreamy"), osc_bang(2),
		osc_meta("brightness", 0.25f), osc_playlist_next(),
	};
	for (const OscMessage &m : in) {
		const std::vector<uint8_t> wire = encode(m);
		CHECK(wire.size() % 4u == 0u);
		OscMessage out;
		REQUIRE(osc_decode(wire, out));
		CHECK(out.address == m.address);
		REQUIRE(out.args.size() == m.args.size());
		for (size_t i = 0; i < m.args.size(); ++i) {
			CHECK(out.args[i].type == m.args[i].type);
			CHECK(out.args[i].s == m.args[i].s);
			CHECK(out.args[i].i == m.args[i].i);
			CHECK(out.args[i].f == doctest::Approx(m.args[i].f));
		}
	}
}

TEST_CASE("osc: mixed argument types keep their order and values")
{
	OscMessage m("/mix");
	m.add(7);
	m.add(-2.5f);
	m.add("text");
	const std::vector<uint8_t> wire = encode(m);
	CHECK(wire.size() % 4u == 0u);
	OscMessage out;
	REQUIRE(osc_decode(wire, out));
	CHECK(out.address == "/mix");
	REQUIRE(out.args.size() == 3u);
	CHECK(out.args[0].type == OscArg::Type::Int);
	CHECK(out.args[0].i == 7);
	CHECK(out.args[1].type == OscArg::Type::Float);
	CHECK(out.args[1].f == doctest::Approx(-2.5f));
	CHECK(out.args[2].type == OscArg::Type::String);
	CHECK(out.args[2].s == "text");
}

TEST_CASE("osc: what the encoder refuses")
{
	std::vector<uint8_t> sink;
	CHECK_FALSE(osc_encode(OscMessage(""), sink));                      // no address
	CHECK_FALSE(osc_encode(OscMessage("no-slash"), sink));              // OSC addresses start with '/'
	CHECK_FALSE(osc_encode(OscMessage(std::string("/a\0b", 4)), sink)); // embedded NUL
	OscMessage bad("/x");
	bad.add(std::string("a\0b", 3));
	CHECK_FALSE(osc_encode(bad, sink)); // embedded NUL in an argument
	CHECK(sink.empty());                // nothing is written on failure
}

TEST_CASE("osc: what the decoder rejects")
{
	OscMessage out;
	CHECK_FALSE(osc_decode(static_cast<const uint8_t *>(nullptr), 0, out));
	CHECK_FALSE(osc_decode(bytes({'/', 'x'}), out));                                         // shorter than a field
	CHECK_FALSE(osc_decode(bytes({'x', 0x00, 0x00, 0x00}), out));                            // address without '/'
	CHECK_FALSE(osc_decode(bytes({0x00, 0x00, 0x00, 0x00}), out));                           // empty address
	CHECK_FALSE(osc_decode(bytes({'/', 'x', 0x00, 0x00, 'i', 0x00, 0x00, 0x00}), out));      // no type tag
	CHECK_FALSE(osc_decode(bytes({'/', 'x', 0x00, 0x00, ',', 'i', 0x00, 0x00, 0x01}), out)); // truncated arg
	CHECK_FALSE(osc_decode(bytes({'/', 'x', 0x00, 0x00, ',', 'q', 0x00, 0x00, 1, 2, 3, 4}), out)); // bad tag
	CHECK_FALSE(osc_decode(bytes({'/', 'x', 0x00, 0x00, ',', 's', 0x00, 0x00, 'a', 'b', 'c', 'd'}), out));
	CHECK_FALSE(osc_decode(bytes({'/', 'x', 0x00, 0x00, ',', 's', 0x00, 0x00}), out)); // string past the end

	// an address-only message: legal, no arguments
	REQUIRE(osc_decode(bytes({'/', 'x', 0x00, 0x00, ',', 0x00, 0x00, 0x00}), out));
	CHECK(out.address == "/x");
	CHECK(out.args.empty());

	// a sender that forgot the trailing padding still decodes
	REQUIRE(osc_decode(bytes({'/', 'x', 0x00, 0x00, ',', 's', 0x00, 0x00, 'h', 'i', 0x00}), out));
	REQUIRE(out.args.size() == 1u);
	CHECK(out.args[0].s == "hi");
}

TEST_CASE("osc: scene names are slugged for the wire")
{
	CHECK(osc_slug("Hex Array") == "hexarray");
	CHECK(osc_slug("Grid-Tunnel_x") == "gridtunnelx");
	CHECK(osc_slug("Nebula  Drift") == "nebuladrift");
	CHECK(osc_slug("already") == "already");
	CHECK(osc_scene("Nebula Drift", "").address == "/scenes/nebuladrift");
}
