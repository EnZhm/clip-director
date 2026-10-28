#pragma once
// Clip Director core: MIDI -> hotkey mapping (W4 "MIDI Director"). Pure C++17, no libobs, no Qt.
//
// The dock never talks to the sources directly: a MIDI control is bound to a *hotkey identity*
// (name + registerer type/name + description) and the OBS layer triggers that hotkey. This keeps
// the dock decoupled from every plugin whose hotkeys it can drive.
//
//   Message          one short MIDI message we act on (note on/off, control change)
//   Control          a control's identity: type + channel + number (the map key)
//   Target           a hotkey identity, as reported by obs_enum_hotkeys
//   Map              control -> target bindings + the runtime state that makes CC edge-triggered
//   Learn            arm a target, then the next control that moves binds it
//
// Trigger rules (see Map::handle):
//   - Note on (velocity > 0) / note off (or note on with velocity 0): press / release, passed
//     through 1:1, so note length is preserved.
//   - Control change: value >= 64 presses, value < 64 releases, edge-triggered (a controller that
//     keeps sending 127 does not repeat the press).
//   - Mode::Toggle flips the target on every press edge instead of tracking the control's own edge.
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cd {
namespace midi {

// ---------------------------------------------------------------------------------------------
// short messages

enum class Type { Note = 0, Control = 1 };

const char *type_to_string(Type t);
bool type_from_string(const std::string &s, Type &out);

struct Message {
	Type type = Type::Note;
	int channel = 1;      // 1..16 (MIDI channel 0..15, as musicians count it)
	int number = 0;       // note number / controller number, 0..127
	int value = 0;        // velocity / controller value, 0..127 (note off keeps its release velocity)
	bool pressed = false; // note on with velocity > 0, or CC value >= 64

	bool operator==(const Message &o) const
	{
		return type == o.type && channel == o.channel && number == o.number && value == o.value &&
		       pressed == o.pressed;
	}

	// 0x80 Note Off, 0x90 Note On (velocity 0 = off), 0xB0 Control Change.
	static constexpr uint8_t kNoteOffBase = 0x80;
	static constexpr uint8_t kNoteOnBase = 0x90;
	static constexpr uint8_t kControlBase = 0xB0;
	static constexpr int kPressThreshold = 64; // CC >= 64 presses, < 64 releases
};

// Parses one complete short message. `len` bytes are read; other statuses (pitch bend, aftertouch,
// program change, system) return false. Everything is validated: status above 0x7f, data 0..127.
bool parse_message(const uint8_t *bytes, size_t len, Message &out);

// Frames a byte stream (what a device driver hands over, in arbitrary chunks) into messages:
// running status, split messages across chunks, interleaved real-time bytes and sysex (ignored).
class Assembler {
public:
	void feed(const uint8_t *bytes, size_t len, std::vector<Message> &out);
	void reset();

private:
	uint8_t status_ = 0; // current channel-message status byte, 0 = none
	std::vector<uint8_t> data_;
	bool sysex_ = false;
};

// ---------------------------------------------------------------------------------------------
// mapping model

struct Control {
	Type type = Type::Note;
	int channel = 1;
	int number = 0;

	bool operator==(const Control &o) const { return type == o.type && channel == o.channel && number == o.number; }
	bool operator<(const Control &o) const
	{
		if (type != o.type)
			return type < o.type;
		if (channel != o.channel)
			return channel < o.channel;
		return number < o.number;
	}

	static Control of(const Message &m) { return Control{m.type, m.channel, m.number}; }
	// Human label used by the dock, e.g. "Note 36 ch 1" / "CC 7 ch 16".
	std::string label() const;
};

enum class Registerer { Frontend = 0, Source = 1, Output = 2, Encoder = 3, Service = 4 };

const char *registerer_to_string(Registerer r);
bool registerer_from_string(const std::string &s, Registerer &out);

struct Target {
	std::string name; // hotkey name, e.g. "cd_skip" (unique per registerer)
	Registerer registerer = Registerer::Frontend;
	std::string registerer_name; // source/output/encoder name; empty for the frontend ones
	std::string description;     // localised description, shown in the picker

	bool operator==(const Target &o) const
	{
		return name == o.name && registerer == o.registerer && registerer_name == o.registerer_name;
	}
	// Same hotkey, ignoring the description (it is localised and may change between releases).
	bool same_hotkey(const Target &o) const { return *this == o; }
};

enum class Mode { Momentary = 0, Toggle = 1 };

const char *mode_to_string(Mode m);
bool mode_from_string(const std::string &s, Mode &out);

struct Mapping {
	Control control;
	Target target;
	Mode mode = Mode::Momentary;

	bool operator==(const Mapping &o) const { return control == o.control && target == o.target && mode == o.mode; }
};

// What the OBS layer has to do when a message comes in: trigger `target` pressed/released.
struct Event {
	Target target;
	bool pressed = false;
};

class Map {
public:
	// One control maps to one target: binding an already-bound control replaces it.
	void bind(const Control &control, const Target &target, Mode mode = Mode::Momentary);
	bool unbind(const Control &control);
	bool bound(const Control &control) const;
	const Mapping *find(const Control &control) const;

	void clear();
	bool empty() const { return map_.empty(); }
	size_t size() const { return map_.size(); }
	// Sorted by control (type, channel, number), so the dock table is stable.
	std::vector<Mapping> mappings() const;

	// Runs one message through the map. Returns 0 or 1 events; updates the held/toggled state.
	std::vector<Event> handle(const Message &message);
	// Forgets held/toggled state without touching the bindings (device change, panic).
	void reset_runtime();

	// JSON, version 1. from_json returns false (and leaves the map untouched) on a syntax error;
	// individual malformed entries are skipped so one bad row never loses the rest.
	std::string to_json() const;
	bool from_json(const std::string &text, std::string *error = nullptr);

	static constexpr int kFormatVersion = 1;

private:
	std::map<Control, Mapping> map_;
	std::map<Control, bool> state_;  // momentary: is the control held
	std::map<Control, bool> toggle_; // toggle: is the target currently pressed
};

// Learn mode: arm a target, then the next control that moves binds it (and disarms).
class Learn {
public:
	void arm(const Target &target, Mode mode = Mode::Momentary);
	void cancel();
	bool armed() const { return armed_; }
	const Target *target() const { return armed_ ? &target_ : nullptr; }

	// Feeds one message. When armed, the first message that reports a press binds its control -
	// in `map`, with the armed target and mode - and returns that control. Anything else returns
	// nullptr.
	const Control *feed(const Message &message, Map &map);

private:
	bool armed_ = false;
	bool learnt_ = false;
	Target target_;
	Mode mode_ = Mode::Momentary;
	Control control_;
};

} // namespace midi
} // namespace cd
