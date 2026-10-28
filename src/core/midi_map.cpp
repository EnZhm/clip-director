// Clip Director core: MIDI -> hotkey mapping (W4 "MIDI Director"). See midi_map.h for the model.
#include "midi_map.h"

#include <cmath>
#include <cstdlib>

namespace cd {
namespace midi {

const char *type_to_string(Type t)
{
	switch (t) {
	case Type::Note:
		return "note";
	case Type::Control:
		return "cc";
	}
	return "note";
}

bool type_from_string(const std::string &s, Type &out)
{
	if (s == "note") {
		out = Type::Note;
		return true;
	}
	if (s == "cc" || s == "control") {
		out = Type::Control;
		return true;
	}
	return false;
}

const char *registerer_to_string(Registerer r)
{
	switch (r) {
	case Registerer::Frontend:
		return "frontend";
	case Registerer::Source:
		return "source";
	case Registerer::Output:
		return "output";
	case Registerer::Encoder:
		return "encoder";
	case Registerer::Service:
		return "service";
	}
	return "frontend";
}

bool registerer_from_string(const std::string &s, Registerer &out)
{
	if (s == "frontend") {
		out = Registerer::Frontend;
		return true;
	}
	if (s == "source") {
		out = Registerer::Source;
		return true;
	}
	if (s == "output") {
		out = Registerer::Output;
		return true;
	}
	if (s == "encoder") {
		out = Registerer::Encoder;
		return true;
	}
	if (s == "service") {
		out = Registerer::Service;
		return true;
	}
	return false;
}

const char *mode_to_string(Mode m)
{
	switch (m) {
	case Mode::Momentary:
		return "momentary";
	case Mode::Toggle:
		return "toggle";
	}
	return "momentary";
}

bool mode_from_string(const std::string &s, Mode &out)
{
	if (s == "momentary" || s == "hold") {
		out = Mode::Momentary;
		return true;
	}
	if (s == "toggle") {
		out = Mode::Toggle;
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------------------------
// short messages

namespace {

// Data bytes a status byte expects (everything we act on takes two).
size_t expected_data(uint8_t status)
{
	switch (status & 0xF0) {
	case 0xC0:
	case 0xD0:
		return 1;
	default:
		return 2;
	}
}

} // namespace

bool parse_message(const uint8_t *bytes, size_t len, Message &out)
{
	if (!bytes || len < 3)
		return false;
	const uint8_t status = bytes[0];
	if (status < 0x80 || status >= 0xF0)
		return false;
	const uint8_t kind = status & 0xF0;
	if (kind != Message::kNoteOffBase && kind != Message::kNoteOnBase && kind != Message::kControlBase)
		return false;
	const uint8_t number = bytes[1], value = bytes[2];
	if (number > 127 || value > 127)
		return false;

	Message m;
	m.channel = (int)(status & 0x0F) + 1;
	m.number = number;
	m.value = value;
	if (kind == Message::kControlBase) {
		m.type = Type::Control;
		m.pressed = value >= (uint8_t)Message::kPressThreshold;
	} else {
		m.type = Type::Note;
		/* note on with velocity 0 is the note-off convention */
		m.pressed = kind == Message::kNoteOnBase && value > 0;
	}
	out = m;
	return true;
}

void Assembler::reset()
{
	status_ = 0;
	data_.clear();
	sysex_ = false;
}

void Assembler::feed(const uint8_t *bytes, size_t len, std::vector<Message> &out)
{
	if (!bytes)
		return;
	for (size_t i = 0; i < len; ++i) {
		const uint8_t b = bytes[i];
		if (b >= 0xF8) /* system real-time: stands alone, running status survives it */
			continue;
		if (b >= 0x80) {
			sysex_ = b == 0xF0;
			status_ = b < 0xF0 ? b : 0;
			data_.clear();
			continue;
		}
		if (sysex_ || status_ == 0 || data_.size() >= 2)
			continue; /* data without a status, inside sysex, or a message we do not act on */
		data_.push_back(b);
		if (data_.size() < expected_data(status_))
			continue;
		const uint8_t raw[3] = {status_, data_[0], data_.size() > 1 ? data_[1] : (uint8_t)0};
		Message m;
		if (parse_message(raw, sizeof(raw), m))
			out.push_back(m);
		data_.clear(); /* running status: the status byte stays for the next message */
	}
}

// ---------------------------------------------------------------------------------------------
// mapping model

std::string Control::label() const
{
	std::string s = type == Type::Control ? "CC " : "Note ";
	s += std::to_string(number);
	s += " ch ";
	s += std::to_string(channel);
	return s;
}

void Map::bind(const Control &control, const Target &target, Mode mode)
{
	Mapping m;
	m.control = control;
	m.target = target;
	m.mode = mode;
	map_[control] = m; /* one control -> one target: the old binding is replaced */
	state_.erase(control);
	toggle_.erase(control);
}

bool Map::unbind(const Control &control)
{
	state_.erase(control);
	toggle_.erase(control);
	return map_.erase(control) > 0;
}

bool Map::bound(const Control &control) const
{
	return map_.find(control) != map_.end();
}

const Mapping *Map::find(const Control &control) const
{
	auto it = map_.find(control);
	return it == map_.end() ? nullptr : &it->second;
}

void Map::clear()
{
	map_.clear();
	state_.clear();
	toggle_.clear();
}

std::vector<Mapping> Map::mappings() const
{
	std::vector<Mapping> out;
	out.reserve(map_.size());
	for (const auto &kv : map_)
		out.push_back(kv.second);
	return out;
}

std::vector<Event> Map::handle(const Message &message)
{
	std::vector<Event> events;
	auto it = map_.find(Control::of(message));
	if (it == map_.end())
		return events;
	const Mapping &m = it->second;
	bool &held = state_[it->first];

	if (message.type == Type::Control) {
		/* edge-triggered: a controller that keeps sending >= 64 does not repeat the press */
		if (message.pressed == held)
			return events;
		held = message.pressed;
	} else {
		/* a note message is an edge in itself; note length is passed through */
		held = message.pressed;
	}
	if (m.mode == Mode::Toggle) {
		if (!message.pressed)
			return events; /* toggle on press edges only */
		bool &on = toggle_[it->first];
		on = !on;
		events.push_back(Event{m.target, on});
		return events;
	}
	events.push_back(Event{m.target, message.pressed});
	return events;
}

void Map::reset_runtime()
{
	state_.clear();
	toggle_.clear();
}

void Learn::arm(const Target &target, Mode mode)
{
	target_ = target;
	mode_ = mode;
	armed_ = true;
	learnt_ = false;
}

void Learn::cancel()
{
	armed_ = false;
	learnt_ = false;
}

const Control *Learn::feed(const Message &message, Map &map)
{
	learnt_ = false;
	if (!armed_ || !message.pressed)
		return nullptr;
	control_ = Control::of(message);
	map.bind(control_, target_, mode_);
	armed_ = false;
	learnt_ = true;
	return &control_;
}

// ---------------------------------------------------------------------------------------------
// JSON

namespace {

void escape(const std::string &s, std::string &out)
{
	static const char *hex = "0123456789abcdef";
	for (const unsigned char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c < 0x20) {
				out += "\\u00";
				out += hex[(c >> 4) & 0xF];
				out += hex[c & 0xF];
			} else {
				out += (char)c;
			}
		}
	}
}

// Minimal, strict JSON reader: objects, arrays, strings with escapes, numbers, true/false/null.
class Reader {
public:
	explicit Reader(const std::string &text) : s_(text) {}

	bool fail(const std::string &what)
	{
		if (error_.empty())
			error_ = what + " at byte " + std::to_string(i_);
		return false;
	}
	const std::string &error() const { return error_; }

	void ws()
	{
		while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r'))
			++i_;
	}
	bool at_end()
	{
		ws();
		return i_ >= s_.size();
	}
	char peek()
	{
		ws();
		return i_ < s_.size() ? s_[i_] : '\0';
	}
	bool eat(char c)
	{
		ws();
		if (i_ < s_.size() && s_[i_] == c) {
			++i_;
			return true;
		}
		return false;
	}

	bool string(std::string &out)
	{
		if (!eat('"'))
			return fail("expected a string");
		out.clear();
		while (i_ < s_.size()) {
			const unsigned char c = s_[i_++];
			if (c == '"')
				return true;
			if (c != '\\') {
				if (c < 0x20)
					return fail("control character in a string");
				out += (char)c;
				continue;
			}
			if (i_ >= s_.size())
				return fail("truncated escape");
			const char e = s_[i_++];
			switch (e) {
			case '"':
				out += '"';
				break;
			case '\\':
				out += '\\';
				break;
			case '/':
				out += '/';
				break;
			case 'b':
				out += '\b';
				break;
			case 'f':
				out += '\f';
				break;
			case 'n':
				out += '\n';
				break;
			case 'r':
				out += '\r';
				break;
			case 't':
				out += '\t';
				break;
			case 'u': {
				unsigned cp = 0;
				if (!hex4(cp))
					return false;
				if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 1 < s_.size() && s_[i_] == '\\' &&
				    s_[i_ + 1] == 'u') {
					i_ += 2;
					unsigned lo = 0;
					if (!hex4(lo))
						return false;
					if (lo >= 0xDC00 && lo <= 0xDFFF)
						cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
				}
				utf8(cp, out);
				break;
			}
			default:
				return fail("unknown escape");
			}
		}
		return fail("unterminated string");
	}

	bool integer(int &out)
	{
		double d = 0.0;
		if (!number(d))
			return false;
		if (d != std::floor(d) || d < -2147483648.0 || d > 2147483647.0)
			return fail("expected an integer");
		out = (int)d;
		return true;
	}

	bool boolean(bool &out)
	{
		if (eat_word("true")) {
			out = true;
			return true;
		}
		if (eat_word("false")) {
			out = false;
			return true;
		}
		return fail("expected a boolean");
	}

	// Skips any value (forward compatibility with fields a newer build added).
	bool skip_value(int depth = 0)
	{
		if (depth > 32)
			return fail("too deeply nested");
		switch (peek()) {
		case '{':
			eat('{');
			if (eat('}'))
				return true;
			do {
				std::string key;
				if (!string(key) || !eat(':') || !skip_value(depth + 1))
					return false;
			} while (eat(','));
			return eat('}') ? true : fail("expected }");
		case '[':
			eat('[');
			if (eat(']'))
				return true;
			do {
				if (!skip_value(depth + 1))
					return false;
			} while (eat(','));
			return eat(']') ? true : fail("expected ]");
		case '"': {
			std::string ignored;
			return string(ignored);
		}
		case 't':
		case 'f': {
			bool ignored = false;
			return boolean(ignored);
		}
		case 'n':
			return eat_word("null") ? true : fail("expected null");
		default:
			return number(sink_);
		}
	}

private:
	bool number(double &out)
	{
		ws();
		const size_t start = i_;
		while (i_ < s_.size() && (is_digit((unsigned char)s_[i_]) || s_[i_] == '-' || s_[i_] == '+' ||
					  s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E'))
			++i_;
		if (i_ == start)
			return fail("expected a number");
		const std::string num = s_.substr(start, i_ - start);
		char *end = nullptr;
		out = std::strtod(num.c_str(), &end);
		if (!end || *end != '\0')
			return fail("bad number");
		return true;
	}

	bool hex4(unsigned &out)
	{
		if (i_ + 4 > s_.size())
			return fail("truncated \\u escape");
		out = 0;
		for (int k = 0; k < 4; ++k) {
			const char c = s_[i_++];
			out <<= 4;
			if (c >= '0' && c <= '9')
				out |= (unsigned)(c - '0');
			else if (c >= 'a' && c <= 'f')
				out |= (unsigned)(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F')
				out |= (unsigned)(c - 'A' + 10);
			else
				return fail("bad \\u escape");
		}
		return true;
	}

	static void utf8(unsigned cp, std::string &out)
	{
		if (cp < 0x80) {
			out += (char)cp;
		} else if (cp < 0x800) {
			out += (char)(0xC0 | (cp >> 6));
			out += (char)(0x80 | (cp & 0x3F));
		} else if (cp < 0x10000) {
			out += (char)(0xE0 | (cp >> 12));
			out += (char)(0x80 | ((cp >> 6) & 0x3F));
			out += (char)(0x80 | (cp & 0x3F));
		} else {
			out += (char)(0xF0 | (cp >> 18));
			out += (char)(0x80 | ((cp >> 12) & 0x3F));
			out += (char)(0x80 | ((cp >> 6) & 0x3F));
			out += (char)(0x80 | (cp & 0x3F));
		}
	}

	bool eat_word(const char *w)
	{
		ws();
		const size_t n = std::char_traits<char>::length(w);
		if (i_ + n > s_.size() || s_.compare(i_, n, w) != 0)
			return false;
		i_ += n;
		return true;
	}

	static bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }

	const std::string &s_;
	size_t i_ = 0;
	std::string error_;
	double sink_ = 0.0;
};

// One mapping entry: returns false on a syntax error, sets `valid` false for one skipped row.
bool read_entry(Reader &r, Mapping &out, bool &valid)
{
	valid = false;
	if (!r.eat('{'))
		return r.fail("expected {");
	bool have_type = false, have_channel = false, have_number = false, have_name = false, have_mode = true;
	int channel = 0, number = 0;
	Mode mode = Mode::Momentary;
	Registerer registerer = Registerer::Frontend;
	if (!r.eat('}')) {
		do {
			std::string key;
			if (!r.string(key) || !r.eat(':'))
				return false;
			if (key == "type") {
				std::string v;
				if (!r.string(v))
					return false;
				have_type = type_from_string(v, out.control.type);
			} else if (key == "channel") {
				if (!r.integer(channel))
					return false;
				have_channel = true;
			} else if (key == "number") {
				if (!r.integer(number))
					return false;
				have_number = true;
			} else if (key == "mode") {
				std::string v;
				if (!r.string(v))
					return false;
				have_mode = mode_from_string(v, mode);
			} else if (key == "name") {
				if (!r.string(out.target.name))
					return false;
				have_name = !out.target.name.empty();
			} else if (key == "registerer") {
				std::string v;
				if (!r.string(v))
					return false;
				if (!registerer_from_string(v, registerer))
					registerer = Registerer::Frontend; /* unknown: keep the row, fall back */
			} else if (key == "registerer_name") {
				if (!r.string(out.target.registerer_name))
					return false;
			} else if (key == "description") {
				if (!r.string(out.target.description))
					return false;
			} else if (!r.skip_value()) {
				return false;
			}
		} while (r.eat(','));
		if (!r.eat('}'))
			return r.fail("expected }");
	}
	if (!have_type || !have_channel || !have_number || !have_name || !have_mode || channel < 1 || channel > 16 ||
	    number < 0 || number > 127)
		return true; /* parseable but out of range: the row is skipped, the rest is kept */
	out.control.channel = channel;
	out.control.number = number;
	out.mode = mode;
	out.target.registerer = registerer;
	valid = true;
	return true;
}

} // namespace

std::string Map::to_json() const
{
	std::string out = "{\"version\":";
	out += std::to_string(kFormatVersion);
	out += ",\"mappings\":[";
	bool first = true;
	for (const auto &kv : map_) {
		const Mapping &m = kv.second;
		out += first ? "\n  " : ",\n  ";
		first = false;
		out += "{\"type\":\"";
		out += type_to_string(m.control.type);
		out += "\",\"channel\":";
		out += std::to_string(m.control.channel);
		out += ",\"number\":";
		out += std::to_string(m.control.number);
		out += ",\"mode\":\"";
		out += mode_to_string(m.mode);
		out += "\",\"name\":\"";
		escape(m.target.name, out);
		out += "\",\"registerer\":\"";
		out += registerer_to_string(m.target.registerer);
		out += "\",\"registerer_name\":\"";
		escape(m.target.registerer_name, out);
		out += "\",\"description\":\"";
		escape(m.target.description, out);
		out += "\"}";
	}
	out += map_.empty() ? "]}" : "\n]}";
	return out;
}

bool Map::from_json(const std::string &text, std::string *error)
{
	Reader r(text);
	auto give_up = [&](const char *what) {
		if (what)
			r.fail(what);
		if (error)
			*error = r.error();
		return false;
	};
	std::map<Control, Mapping> parsed;
	if (!r.eat('{'))
		return give_up("expected {");
	if (!r.eat('}')) {
		do {
			std::string key;
			if (!r.string(key) || !r.eat(':'))
				return give_up(nullptr);
			if (key == "version") {
				int version = 0;
				if (!r.integer(version))
					return give_up(nullptr);
			} else if (key == "mappings") {
				if (!r.eat('['))
					return give_up("expected [");
				if (!r.eat(']')) {
					do {
						Mapping m;
						bool valid = false;
						if (!read_entry(r, m, valid))
							return give_up(nullptr);
						if (valid)
							parsed[m.control] = m; /* one control -> one target */
					} while (r.eat(','));
					if (!r.eat(']'))
						return give_up("expected ]");
				}
			} else if (!r.skip_value()) {
				return give_up(nullptr);
			}
		} while (r.eat(','));
		if (!r.eat('}'))
			return give_up("expected }");
	}
	if (!r.at_end())
		return give_up("trailing data");
	if (error)
		error->clear();
	map_ = parsed;
	state_.clear();
	toggle_.clear();
	return true;
}

} // namespace midi
} // namespace cd
