// Clip Director: MIDI input (W4). WinMM under _WIN32, a no-device stub elsewhere.
#include "cd-midi-input.h"

#include <mutex>

#ifdef _WIN32
#include <windows.h>

#include <mmsystem.h>
#endif

namespace cdmidi {

struct Input::Impl {
#ifdef _WIN32
	HMIDIIN handle = nullptr;

	// CALLBACK_FUNCTION entry point: append only, nothing that can block or touch OBS.
	static void CALLBACK on_data(HMIDIIN device, UINT message, DWORD_PTR instance, DWORD_PTR param1,
				     DWORD_PTR param2);
#endif

	std::mutex mutex;
	std::vector<uint8_t> queue;
};

#ifdef _WIN32

namespace {

#ifdef UNICODE
std::string to_utf8(const wchar_t *wide)
{
	if (!wide || !*wide)
		return std::string();
	const int need = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
	if (need <= 1)
		return std::string();
	std::string out((size_t)need - 1, '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide, -1, &out[0], need, nullptr, nullptr);
	return out;
}
#endif

} // namespace

void CALLBACK Input::Impl::on_data(HMIDIIN, UINT message, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR)
{
	if (message != MIM_DATA)
		return; /* with no MIDI_IO_STATUS at open there is no long (sysex) data to handle */
	auto *impl = reinterpret_cast<Input::Impl *>(instance);
	if (!impl)
		return;

	const uint8_t status = (uint8_t)(param1 & 0xFFu);
	const uint8_t first = (uint8_t)((param1 >> 8) & 0xFFu);
	const uint8_t second = (uint8_t)((param1 >> 16) & 0xFFu);

	size_t length = 0;
	switch (status & 0xF0u) {
	case 0x80: /* note off */
	case 0x90: /* note on */
	case 0xA0: /* poly aftertouch */
	case 0xB0: /* control change */
	case 0xE0: /* pitch bend */
		length = 3;
		break;
	case 0xC0: /* program change */
	case 0xD0: /* channel aftertouch */
		length = 2;
		break;
	default:
		return; /* system messages are not MIDI Director material */
	}

	std::lock_guard<std::mutex> lock(impl->mutex);
	impl->queue.push_back(status);
	impl->queue.push_back(first);
	if (length > 2)
		impl->queue.push_back(second);
}

#endif

Input::Input() : impl_(new Impl()) {}

Input::~Input()
{
	close();
}

bool available()
{
#ifdef _WIN32
	return true;
#else
	return false;
#endif
}

std::vector<Device> devices()
{
	std::vector<Device> out;
#ifdef _WIN32
	const UINT count = midiInGetNumDevs();
	for (UINT i = 0; i < count; ++i) {
		MIDIINCAPS caps;
		ZeroMemory(&caps, sizeof(caps));
		if (midiInGetDevCaps(i, &caps, (UINT)sizeof(caps)) != MMSYSERR_NOERROR)
			continue;
		Device device;
		device.index = (int)i;
#ifdef UNICODE
		device.name = to_utf8(caps.szPname);
#else
		device.name = caps.szPname;
#endif
		if (device.name.empty())
			device.name = "MIDI device " + std::to_string(device.index);
		out.push_back(device);
	}
#endif
	return out;
}

bool Input::open(int device_index, std::string *error)
{
	close();
#ifdef _WIN32
	const UINT count = midiInGetNumDevs();
	if (device_index < 0 || (UINT)device_index >= count) {
		if (error)
			*error = "MIDI device " + std::to_string(device_index) + " is not available";
		return false;
	}
	HMIDIIN handle = nullptr;
	const MMRESULT opened = midiInOpen(&handle, (UINT)device_index,
					   reinterpret_cast<DWORD_PTR>(&Input::Impl::on_data),
					   reinterpret_cast<DWORD_PTR>(impl_.get()), CALLBACK_FUNCTION);
	if (opened != MMSYSERR_NOERROR || !handle) {
		if (error)
			*error = "midiInOpen failed (code " + std::to_string((int)opened) + ")";
		return false;
	}
	if (midiInStart(handle) != MMSYSERR_NOERROR) {
		midiInClose(handle);
		if (error)
			*error = "midiInStart failed";
		return false;
	}
	impl_->handle = handle;
	open_ = true;
	index_ = device_index;
	return true;
#else
	if (error)
		*error = "MIDI input is only available in the Windows build";
	(void)device_index;
	return false;
#endif
}

void Input::close()
{
#ifdef _WIN32
	if (impl_->handle) {
		/* stop, then reset: midiInReset waits for a running callback, so it cannot touch a
		 * closed handle afterwards */
		midiInStop(impl_->handle);
		midiInReset(impl_->handle);
		midiInClose(impl_->handle);
		impl_->handle = nullptr;
	}
#endif
	std::lock_guard<std::mutex> lock(impl_->mutex);
	impl_->queue.clear();
	open_ = false;
	index_ = -1;
}

void Input::drain(std::vector<uint8_t> &out)
{
	std::lock_guard<std::mutex> lock(impl_->mutex);
	if (impl_->queue.empty())
		return;
	out.insert(out.end(), impl_->queue.begin(), impl_->queue.end());
	impl_->queue.clear();
}

} // namespace cdmidi
