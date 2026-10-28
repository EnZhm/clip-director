#pragma once
// Clip Director core: OSC 1.0 codec. Pure C++17, no libobs/Qt, no dependencies.
//
// Open Sound Control 1.0 messages are big-endian and everything is padded to a 4-byte
// boundary: an address pattern (NUL-terminated string), a type tag string that starts with
// ',' and names the argument types, then the packed arguments. Only the three types Clip
// Director needs are supported (int32, float32, string) -- Synesthesia's routes use nothing
// else: /scenes/{name} {preset}, /presets, /controls/global/{type}/{n} {value},
// /controls/meta/{name} {value} and /playlist/next.
#include <cstdint>
#include <string>
#include <vector>

namespace cd {

struct OscArg {
	enum class Type { Int = 0, Float = 1, String = 2 };

	Type type = Type::Int;
	int32_t i = 0;
	float f = 0.0f;
	std::string s;

	static OscArg of_int(int32_t v);
	static OscArg of_float(float v);
	static OscArg of_string(std::string v);
};

struct OscMessage {
	std::string address;
	std::vector<OscArg> args;

	OscMessage() = default;
	explicit OscMessage(std::string addr) : address(std::move(addr)) {}
	OscMessage &add(int32_t v);
	OscMessage &add(float v);
	OscMessage &add(std::string v);
};

// A field is padded to the next multiple of 4 bytes (a string's NUL is part of its length).
size_t osc_padded(size_t n);

// false when there is nothing to send (empty address or an address with an embedded NUL).
bool osc_encode(const OscMessage &m, std::vector<uint8_t> &out);
// false on a malformed packet: short, address not starting with '/', unterminated string,
// truncated argument or a tag that does not name the arguments present.
bool osc_decode(const uint8_t *data, size_t len, OscMessage &out);
bool osc_decode(const std::vector<uint8_t> &data, OscMessage &out);

// Synesthesia scene names are lowercase with no spaces, underscores or hyphens:
// /scenes/{sceneName}, so "Grid Tunnel" goes out as /scenes/gridtunnel.
std::string osc_slug(const std::string &name);

// Routes this plugin sends (Synesthesia OSC docs, manual/osc.html).
OscMessage osc_scene(const std::string &scene, const std::string &preset);
OscMessage osc_preset(const std::string &preset);
OscMessage osc_bang(int position);                         // /controls/global/bang/{n} 1.0
OscMessage osc_meta(const std::string &name, float value); // /controls/meta/{name} {v}
OscMessage osc_playlist_next();                            // /playlist/next

} // namespace cd
