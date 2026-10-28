#pragma once
// Clip Director: OSC transport (spec W2). One small interface over UDP, Winsock on Windows
// (_WIN32) and POSIX sockets elsewhere, so the same code that runs in the plugin also runs in
// a Linux unit test that does a real loopback round trip.
//
// Neither the send nor the receive path is ever allowed to block the graphics or audio thread:
// send() only queues a datagram for the sender thread, and the receiver thread drains the
// socket into a queue that pop() reads without waiting. Both threads are joined by stop().
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <netinet/in.h>
#endif

namespace cdnet {

// One UDP socket, bound locally (for the feedback port) and pointed at a remote host:port.
class UdpSocket {
public:
#ifdef _WIN32
	using sock_t = uintptr_t;
	static constexpr sock_t kInvalid = static_cast<sock_t>(~static_cast<uintptr_t>(0));
#else
	using sock_t = int;
	static constexpr sock_t kInvalid = -1;
#endif

	UdpSocket() = default;
	~UdpSocket() { close(); }
	UdpSocket(const UdpSocket &) = delete;
	UdpSocket &operator=(const UdpSocket &) = delete;

	// recv_port 0 binds an ephemeral local port (a test reads it back with local_port()).
	bool open(const std::string &host, int send_port, int recv_port, std::string *err);
	void close();
	bool valid() const;
	int local_port() const { return local_port_; }
	// One datagram. Both wait at most `timeout_ms`; false on timeout or error.
	bool send(const uint8_t *data, size_t len, int timeout_ms = 200);
	bool recv(uint8_t *buf, size_t cap, size_t *len, int timeout_ms = 200);

private:
#ifdef _WIN32
	sock_t fd_ = kInvalid;
	char remote_[128] = {0};
	int remote_len_ = 0;
#else
	sock_t fd_ = kInvalid;
	struct sockaddr_storage remote_ = {};
	socklen_t remote_len_ = 0;
#endif
	int local_port_ = 0;
};

// Sender thread + receiver thread around one UdpSocket.
class OscTransport {
public:
	OscTransport() = default;
	~OscTransport() { stop(); }
	OscTransport(const OscTransport &) = delete;
	OscTransport &operator=(const OscTransport &) = delete;

	bool start(const std::string &host, int send_port, int feedback_port, std::string *err);
	void stop();
	bool running() const { return running_; }
	// The bound feedback port (a test uses it as the echo target).
	int local_port() const { return sock_.local_port(); }

	// Queue one datagram to send (never blocks); false when not running or the queue is full.
	bool send(const uint8_t *data, size_t len);
	// One received datagram, or false when none is waiting. Never blocks.
	bool pop(std::vector<uint8_t> &msg);

	uint64_t sent() const;
	uint64_t received() const;
	uint64_t dropped() const;

	static constexpr size_t kMaxQueued = 256;

private:
	void send_loop();
	void recv_loop();

	mutable std::mutex mx_;
	std::condition_variable cv_;
	std::deque<std::vector<uint8_t>> out_; // queued datagrams
	std::deque<std::vector<uint8_t>> in_;  // received datagrams
	UdpSocket sock_;
	std::thread tx_, rx_;
	bool running_ = false, stop_ = false;
	uint64_t sent_ = 0, recv_ = 0, dropped_ = 0;
};

} // namespace cdnet
