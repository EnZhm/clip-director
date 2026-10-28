#include "osc.h"

#include <cctype>
#include <cstring>

namespace cd {

namespace {

void put_u32(std::vector<uint8_t> &out, uint32_t v)
{
	out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
	out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
	out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
	out.push_back(static_cast<uint8_t>(v & 0xFFu));
}

/* string + NUL, padded to a 4-byte boundary from the start of the string */
void put_str(std::vector<uint8_t> &out, const std::string &s)
{
	out.insert(out.end(), s.begin(), s.end());
	out.push_back(0);
	const size_t n = s.size() + 1;
	for (size_t i = n % 4; i != 0 && i < 4; ++i)
		out.push_back(0);
}

uint32_t get_u32(const uint8_t *d, size_t off)
{
	return (static_cast<uint32_t>(d[off]) << 24) | (static_cast<uint32_t>(d[off + 1]) << 16) |
	       (static_cast<uint32_t>(d[off + 2]) << 8) | static_cast<uint32_t>(d[off + 3]);
}

float bits_to_float(uint32_t bits)
{
	float f = 0.0f;
	std::memcpy(&f, &bits, sizeof(f));
	return f;
}

uint32_t float_to_bits(float f)
{
	uint32_t bits = 0;
	std::memcpy(&bits, &f, sizeof(bits));
	return bits;
}

bool read_str(const uint8_t *d, size_t len, size_t &off, std::string &out)
{
	const size_t start = off;
	while (off < len && d[off] != 0)
		++off;
	if (off >= len)
		return false; /* not NUL-terminated */
	out.assign(reinterpret_cast<const char *>(d + start), off - start);
	off = (off + 1 + 3u) & ~static_cast<size_t>(3u);
	if (off > len) /* tolerate a sender that dropped the trailing padding */
		off = len;
	return true;
}

} // namespace

size_t osc_padded(size_t n)
{
	return (n + 3u) & ~static_cast<size_t>(3u);
}

OscArg OscArg::of_int(int32_t v)
{
	OscArg a;
	a.type = Type::Int;
	a.i = v;
	return a;
}

OscArg OscArg::of_float(float v)
{
	OscArg a;
	a.type = Type::Float;
	a.f = v;
	return a;
}

OscArg OscArg::of_string(std::string v)
{
	OscArg a;
	a.type = Type::String;
	a.s = std::move(v);
	return a;
}

OscMessage &OscMessage::add(int32_t v)
{
	args.push_back(OscArg::of_int(v));
	return *this;
}

OscMessage &OscMessage::add(float v)
{
	args.push_back(OscArg::of_float(v));
	return *this;
}

OscMessage &OscMessage::add(std::string v)
{
	args.push_back(OscArg::of_string(std::move(v)));
	return *this;
}

bool osc_encode(const OscMessage &m, std::vector<uint8_t> &out)
{
	if (m.address.empty() || m.address[0] != '/')
		return false;
	if (m.address.find('\0') != std::string::npos)
		return false;
	for (const OscArg &a : m.args)
		if (a.type == OscArg::Type::String && a.s.find('\0') != std::string::npos)
			return false;

	out.clear();
	put_str(out, m.address);
	std::string tags = ",";
	for (const OscArg &a : m.args)
		tags.push_back(a.type == OscArg::Type::Int ? 'i' : (a.type == OscArg::Type::Float ? 'f' : 's'));
	put_str(out, tags);
	for (const OscArg &a : m.args) {
		switch (a.type) {
		case OscArg::Type::Int:
			put_u32(out, static_cast<uint32_t>(a.i));
			break;
		case OscArg::Type::Float:
			put_u32(out, float_to_bits(a.f));
			break;
		case OscArg::Type::String:
			put_str(out, a.s);
			break;
		}
	}
	return true;
}

bool osc_decode(const uint8_t *data, size_t len, OscMessage &out)
{
	out = OscMessage();
	std::string tags;
	size_t off = 0;
	if (!data || len < 4)
		return false;
	if (!read_str(data, len, off, out.address))
		return false;
	if (out.address.empty() || out.address[0] != '/')
		return false;
	if (off >= len)
		return true; /* an address-only message is legal */
	if (!read_str(data, len, off, tags))
		return false;
	if (tags.empty() || tags[0] != ',')
		return false;

	for (size_t i = 1; i < tags.size(); ++i) {
		const char t = tags[i];
		if (off >= len)
			return false; /* fewer arguments than the tag string promises */
		if (t == 'i') {
			if (len - off < 4)
				return false;
			out.args.push_back(OscArg::of_int(static_cast<int32_t>(get_u32(data, off))));
			off += 4;
		} else if (t == 'f') {
			if (len - off < 4)
				return false;
			out.args.push_back(OscArg::of_float(bits_to_float(get_u32(data, off))));
			off += 4;
		} else if (t == 's') {
			std::string s;
			if (!read_str(data, len, off, s))
				return false;
			out.args.push_back(OscArg::of_string(std::move(s)));
		} else {
			return false; /* unsupported type */
		}
	}
	return true;
}

bool osc_decode(const std::vector<uint8_t> &data, OscMessage &out)
{
	return osc_decode(data.empty() ? nullptr : data.data(), data.size(), out);
}

std::string osc_slug(const std::string &name)
{
	std::string s;
	s.reserve(name.size());
	for (const char c : name) {
		const unsigned char u = static_cast<unsigned char>(c);
		if (c == ' ' || c == '_' || c == '-' || c == '\t')
			continue;
		s.push_back(static_cast<char>(std::tolower(u)));
	}
	return s;
}

OscMessage osc_scene(const std::string &scene, const std::string &preset)
{
	OscMessage m("/scenes/" + osc_slug(scene));
	if (!preset.empty())
		m.add(preset);
	return m;
}

OscMessage osc_preset(const std::string &preset)
{
	OscMessage m("/presets");
	if (!preset.empty())
		m.add(preset);
	return m;
}

OscMessage osc_bang(int position)
{
	OscMessage m("/controls/global/bang/" + std::to_string(position < 1 ? 1 : position));
	m.add(1.0f);
	return m;
}

OscMessage osc_meta(const std::string &name, float value)
{
	OscMessage m("/controls/meta/" + osc_slug(name));
	m.add(value);
	return m;
}

OscMessage osc_playlist_next()
{
	return OscMessage("/playlist/next");
}

} // namespace cd
