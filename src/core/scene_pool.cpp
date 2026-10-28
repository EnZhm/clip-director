#include "scene_pool.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace cd {

namespace {

const char *kPoolKeys[] = {"groove", "breakdown", "drop"};

int pool_index(const std::string &key)
{
	for (int i = 0; i < 3; ++i)
		if (key == kPoolKeys[i])
			return i;
	return -1;
}

/* JSON writer: only strings and arrays/objects of strings are emitted */
void json_str(std::string &out, const std::string &s)
{
	out.push_back('"');
	for (const char c : s) {
		const unsigned char u = static_cast<unsigned char>(c);
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
			if (u < 0x20) {
				char buf[8];
				std::snprintf(buf, sizeof(buf), "\\u%04x", u);
				out += buf;
			} else {
				out.push_back(c);
			}
			break;
		}
	}
	out.push_back('"');
}

void json_pool(std::string &out, const std::vector<SceneEntry> &pool)
{
	out.push_back('[');
	for (size_t i = 0; i < pool.size(); ++i) {
		if (i)
			out.push_back(',');
		out += "{\"scene\":";
		json_str(out, pool[i].scene);
		out += ",\"preset\":";
		json_str(out, pool[i].preset);
		out.push_back('}');
	}
	out.push_back(']');
}

/* A tolerant reader for the subset scene_pools_to_json emits. Unknown members are skipped,
 * including nested arrays/objects, so a file written by a later version still loads. */
class Reader {
public:
	Reader(const std::string &s) : p_(s.c_str()), end_(s.c_str() + s.size()) {}

	bool parse(ScenePools &out)
	{
		if (!expect('{'))
			return false;
		skip_ws();
		if (peek() == '}') {
			++p_;
			return true;
		}
		for (;;) {
			std::string key;
			if (!string(key))
				return false;
			if (!expect(':'))
				return false;
			skip_ws();
			const int idx = pool_index(key);
			if (idx >= 0 && peek() == '[') {
				std::vector<SceneEntry> pool;
				if (!pool_array(pool))
					return false;
				out.of(static_cast<Section>(idx)) = std::move(pool);
			} else if (!skip_value(0)) {
				return false;
			}
			skip_ws();
			if (peek() == ',') {
				++p_;
				continue;
			}
			return expect('}');
		}
	}

private:
	bool at_end() const { return p_ >= end_; }
	char peek() const { return at_end() ? '\0' : *p_; }
	void skip_ws()
	{
		while (!at_end() && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r'))
			++p_;
	}
	bool expect(char c)
	{
		skip_ws();
		if (peek() != c)
			return false;
		++p_;
		skip_ws();
		return true;
	}
	bool literal(const char *word)
	{
		skip_ws();
		const size_t n = std::strlen(word);
		if (static_cast<size_t>(end_ - p_) < n || std::strncmp(p_, word, n) != 0)
			return false;
		p_ += n;
		return true;
	}
	static void append_utf8(std::string &out, unsigned int cp)
	{
		if (cp < 0x80) {
			out.push_back(static_cast<char>(cp));
		} else if (cp < 0x800) {
			out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		} else {
			out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		}
	}
	bool string(std::string &out)
	{
		skip_ws();
		if (peek() != '"')
			return false;
		++p_;
		out.clear();
		while (!at_end() && *p_ != '"') {
			if (*p_ != '\\') {
				out.push_back(*p_++);
				continue;
			}
			++p_;
			if (at_end())
				return false;
			const char e = *p_++;
			switch (e) {
			case '"':
				out.push_back('"');
				break;
			case '\\':
				out.push_back('\\');
				break;
			case '/':
				out.push_back('/');
				break;
			case 'n':
				out.push_back('\n');
				break;
			case 'r':
				out.push_back('\r');
				break;
			case 't':
				out.push_back('\t');
				break;
			case 'b':
				out.push_back('\b');
				break;
			case 'f':
				out.push_back('\f');
				break;
			case 'u': {
				if (static_cast<size_t>(end_ - p_) < 4)
					return false;
				char hex[5] = {p_[0], p_[1], p_[2], p_[3], 0};
				p_ += 4;
				append_utf8(out, static_cast<unsigned int>(std::strtoul(hex, nullptr, 16)));
				break;
			}
			default:
				return false;
			}
		}
		if (peek() != '"')
			return false;
		++p_;
		skip_ws();
		return true;
	}
	bool number()
	{
		skip_ws();
		const char *start = p_;
		if (peek() == '-' || peek() == '+')
			++p_;
		bool any = false;
		while (!at_end() && ((*p_ >= '0' && *p_ <= '9') || *p_ == '.' || *p_ == 'e' || *p_ == 'E' ||
				     *p_ == '+' || *p_ == '-')) {
			++p_;
			any = true;
		}
		return any && p_ > start;
	}
	bool skip_value(int depth)
	{
		if (depth > 16)
			return false;
		skip_ws();
		const char c = peek();
		if (c == '"') {
			std::string ignored;
			return string(ignored);
		}
		if (c == '{' || c == '[') {
			const char close = c == '{' ? '}' : ']';
			++p_;
			skip_ws();
			if (peek() == close) {
				++p_;
				return true;
			}
			for (;;) {
				if (c == '{') {
					std::string ignored;
					if (!string(ignored) || !expect(':'))
						return false;
				}
				if (!skip_value(depth + 1))
					return false;
				skip_ws();
				if (peek() == ',') {
					++p_;
					continue;
				}
				return expect(close);
			}
		}
		if (literal("true") || literal("false") || literal("null"))
			return true;
		return number();
	}
	bool entry(SceneEntry &out)
	{
		if (!expect('{'))
			return false;
		skip_ws();
		if (peek() == '}') {
			++p_;
			return true;
		}
		for (;;) {
			std::string key;
			if (!string(key) || !expect(':'))
				return false;
			std::string value;
			if (peek() == '"') {
				if (!string(value))
					return false;
			} else if (!skip_value(0)) {
				return false;
			}
			if (key == "scene")
				out.scene = value;
			else if (key == "preset")
				out.preset = value;
			skip_ws();
			if (peek() == ',') {
				++p_;
				continue;
			}
			return expect('}');
		}
	}
	bool pool_array(std::vector<SceneEntry> &out)
	{
		if (!expect('['))
			return false;
		skip_ws();
		if (peek() == ']') {
			++p_;
			return true;
		}
		for (;;) {
			SceneEntry e;
			if (!entry(e))
				return false;
			if (!e.scene.empty())
				out.push_back(std::move(e));
			skip_ws();
			if (peek() == ',') {
				++p_;
				continue;
			}
			return expect(']');
		}
	}

	const char *p_;
	const char *end_;
};

} // namespace

std::string SceneEntry::label() const
{
	if (preset.empty())
		return scene;
	return scene + " \"" + preset + "\"";
}

const std::vector<SceneEntry> &ScenePools::of(Section s) const
{
	return s == Section::Breakdown ? breakdown : (s == Section::Drop ? drop : groove);
}

std::vector<SceneEntry> &ScenePools::of(Section s)
{
	return s == Section::Breakdown ? breakdown : (s == Section::Drop ? drop : groove);
}

std::string scene_pools_to_json(const ScenePools &p)
{
	std::string out = "{\"groove\":";
	json_pool(out, p.groove);
	out += ",\"breakdown\":";
	json_pool(out, p.breakdown);
	out += ",\"drop\":";
	json_pool(out, p.drop);
	out.push_back('}');
	return out;
}

bool scene_pools_from_json(const std::string &json, ScenePools &out)
{
	if (json.empty())
		return false;
	Reader r(json);
	ScenePools parsed;
	if (!r.parse(parsed))
		return false;
	out = std::move(parsed);
	return true;
}

SceneShuffle::SceneShuffle(uint32_t seed) : rng_(seed ^ 0x85EBCA6Bu) {}

void SceneShuffle::set_entries(std::vector<SceneEntry> entries)
{
	entries_ = std::move(entries);
	bag_.clear();
	last_ = -1;
}

void SceneShuffle::refill()
{
	const size_t n = entries_.size();
	bag_.resize(n);
	for (size_t i = 0; i < n; ++i)
		bag_[i] = static_cast<int>(i);
	for (size_t i = n; i > 1; --i) {
		const size_t j = static_cast<size_t>(rng_.next() * static_cast<double>(i));
		std::swap(bag_[i - 1], bag_[j < i ? j : i - 1]);
	}
	/* a refilled bag never starts with the entry that ended the previous pass */
	if (n > 1 && bag_[n - 1] == last_) {
		const size_t k = static_cast<size_t>(rng_.next() * static_cast<double>(n - 1));
		std::swap(bag_[n - 1], bag_[k < n - 1 ? k : 0]);
	}
}

void SceneShuffle::draw(std::vector<int> &bag, int &pick)
{
	pick = -1;
	if (bag.empty())
		return;
	if (bag.size() > 1 && bag.back() == last_) {
		const size_t n = bag.size() - 1;
		const size_t k = static_cast<size_t>(rng_.next() * static_cast<double>(n));
		std::swap(bag.back(), bag[k < n ? k : n - 1]);
	}
	pick = bag.back();
	bag.pop_back();
}

bool SceneShuffle::next(SceneEntry &out)
{
	if (entries_.empty())
		return false;
	if (bag_.empty())
		refill();
	int pick = -1;
	draw(bag_, pick);
	if (pick < 0)
		return false;
	out = entries_[static_cast<size_t>(pick)];
	last_ = pick;
	return true;
}

bool SceneShuffle::peek(SceneEntry &out) const
{
	if (entries_.empty())
		return false;
	/* draw from a copy, so the original bag and RNG stream are untouched */
	SceneShuffle tmp = *this;
	return tmp.next(out);
}

} // namespace cd
