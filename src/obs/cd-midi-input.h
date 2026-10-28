#pragma once
// Clip Director: MIDI input for the MIDI Director dock (W4).
//
// Windows: WinMM (midiInGetNumDevs / midiInGetDevCaps / midiInOpen(CALLBACK_FUNCTION) /
// midiInStart / midiInStop / midiInClose). The device callback runs on WinMM's own thread and does
// one thing: append the bytes it received to a mutex-protected queue. The dock drains that queue
// from a ~10 ms QTimer on the UI thread, so no hotkey is ever triggered off the audio/MIDI thread.
//
// Other platforms: a stub that reports no devices, so the dock builds and the test suite runs
// everywhere.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cdmidi {

struct Device {
	int index = 0; // device index in this backend
	std::string name;
};

// True when this build has a real MIDI backend (Windows only for now).
bool available();

// Devices the backend can open; empty when the platform has none.
std::vector<Device> devices();

class Input {
public:
	Input();
	~Input();
	Input(const Input &) = delete;
	Input &operator=(const Input &) = delete;

	// Opens (and starts) one device. Any previously open device is closed first.
	bool open(int device_index, std::string *error = nullptr);
	void close();
	bool is_open() const { return open_; }
	int device_index() const { return index_; }

	// Appends the bytes received since the last call to `out` (in arrival order).
	void drain(std::vector<uint8_t> &out);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
	bool open_ = false;
	int index_ = -1;
};

} // namespace cdmidi
