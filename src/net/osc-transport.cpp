#include "osc-transport.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
using cd_socklen = int;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using cd_socklen = socklen_t;
#endif

namespace cdnet {

namespace {

#ifdef _WIN32
bool wsa_start(std::string *err)
{
	WSADATA wsa;
	const int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
	if (rc != 0) {
		if (err)
			*err = "WSAStartup failed (" + std::to_string(rc) + ")";
		return false;
	}
	return true;
}

void wsa_stop()
{
	WSACleanup();
}

bool sock_interrupted(int e)
{
	return e == WSAEINTR || e == WSAETIMEDOUT || e == WSAEWOULDBLOCK;
}
#else
bool wsa_start(std::string *)
{
	return true;
}

void wsa_stop() {}

bool sock_interrupted(int e)
{
	return e == EAGAIN || e == EWOULDBLOCK || e == EINTR;
}
#endif

void set_timeo(UdpSocket::sock_t fd, int ms)
{
#ifdef _WIN32
	const DWORD t = static_cast<DWORD>(ms < 0 ? 0 : ms);
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&t), static_cast<int>(sizeof(t)));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&t), static_cast<int>(sizeof(t)));
#else
	struct timeval tv;
	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, static_cast<socklen_t>(sizeof(tv)));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, static_cast<socklen_t>(sizeof(tv)));
#endif
}

int last_error()
{
#ifdef _WIN32
	return WSAGetLastError();
#else
	return errno;
#endif
}

} // namespace

bool UdpSocket::open(const std::string &host, int send_port, int recv_port, std::string *err)
{
	close();
	if (!wsa_start(err))
		return false;

	struct addrinfo hints;
	std::memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;

	char port_buf[16];
	std::snprintf(port_buf, sizeof(port_buf), "%d", send_port);
	struct addrinfo *res = nullptr;
	const int rc = getaddrinfo(host.empty() ? "127.0.0.1" : host.c_str(), port_buf, &hints, &res);
	if (rc != 0 || !res) {
		if (err)
			*err = "cannot resolve the Synesthesia host (" + host + ":" + port_buf + ")";
		if (res)
			freeaddrinfo(res);
		wsa_stop();
		return false;
	}
	std::memcpy(&remote_, res->ai_addr, res->ai_addrlen);
	remote_len_ = static_cast<cd_socklen>(res->ai_addrlen);
	const int family = res->ai_family;
	fd_ = socket(family, SOCK_DGRAM, IPPROTO_UDP);
	freeaddrinfo(res);
	if (fd_ == kInvalid) {
		if (err)
			*err = "socket() failed (" + std::to_string(last_error()) + ")";
		wsa_stop();
		return false;
	}

	/* bind locally so Synesthesia's OSC output (the echo) lands on the feedback port */
	struct addrinfo bhints;
	std::memset(&bhints, 0, sizeof(bhints));
	bhints.ai_family = family;
	bhints.ai_socktype = SOCK_DGRAM;
	bhints.ai_protocol = IPPROTO_UDP;
	bhints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
	char rbuf[16];
	std::snprintf(rbuf, sizeof(rbuf), "%d", recv_port < 0 ? 0 : recv_port);
	struct addrinfo *bres = nullptr;
	const int brc = getaddrinfo(nullptr, rbuf, &bhints, &bres);
	bool bound = false;
	if (brc == 0 && bres) {
		for (struct addrinfo *a = bres; a && !bound; a = a->ai_next)
			bound = bind(fd_, a->ai_addr, static_cast<cd_socklen>(a->ai_addrlen)) == 0;
		freeaddrinfo(bres);
	}
	if (!bound) {
		if (err)
			*err = "cannot bind the UDP feedback port " + std::string(rbuf) + " (" +
			       std::to_string(last_error()) + ")";
		close();
		return false;
	}

	set_timeo(fd_, 100);
#ifdef _WIN32
	/* Without this, an ICMP port-unreachable caused by an earlier send (Synesthesia not listening)
	 * fails the next recvfrom on this socket with WSAECONNRESET. */
	BOOL report_reset = FALSE;
	DWORD unused_bytes = 0;
	WSAIoctl(fd_, SIO_UDP_CONNRESET, &report_reset, static_cast<DWORD>(sizeof(report_reset)), nullptr, 0,
		 &unused_bytes, nullptr, nullptr);
#endif

	struct sockaddr_storage ss;
	std::memset(&ss, 0, sizeof(ss));
	cd_socklen sl = static_cast<cd_socklen>(sizeof(ss));
	if (getsockname(fd_, reinterpret_cast<struct sockaddr *>(&ss), &sl) == 0) {
		if (ss.ss_family == AF_INET)
			local_port_ = ntohs(reinterpret_cast<struct sockaddr_in *>(&ss)->sin_port);
		else if (ss.ss_family == AF_INET6)
			local_port_ = ntohs(reinterpret_cast<struct sockaddr_in6 *>(&ss)->sin6_port);
	}
	return true;
}

void UdpSocket::close()
{
	if (fd_ != kInvalid) {
#ifdef _WIN32
		closesocket(fd_);
#else
		::close(fd_);
#endif
		wsa_stop();
	}
	fd_ = kInvalid;
	local_port_ = 0;
	remote_len_ = 0;
}

bool UdpSocket::valid() const
{
	return fd_ != kInvalid;
}

bool UdpSocket::send(const uint8_t *data, size_t len, int timeout_ms)
{
	if (fd_ == kInvalid || !data || len == 0 || len > 65507)
		return false;
	set_timeo(fd_, timeout_ms);
	const char *p = reinterpret_cast<const char *>(data);
#ifdef _WIN32
	const int n = sendto(fd_, p, static_cast<int>(len), 0, reinterpret_cast<const struct sockaddr *>(&remote_),
			     remote_len_);
#else
	const ssize_t n = sendto(fd_, p, len, 0, reinterpret_cast<const struct sockaddr *>(&remote_), remote_len_);
#endif
	return n >= 0 && static_cast<size_t>(n) == len;
}

bool UdpSocket::recv(uint8_t *buf, size_t cap, size_t *len, int timeout_ms)
{
	if (fd_ == kInvalid || !buf || cap == 0)
		return false;
	set_timeo(fd_, timeout_ms);
	char *p = reinterpret_cast<char *>(buf);
#ifdef _WIN32
	const int n = recvfrom(fd_, p, static_cast<int>(cap), 0, nullptr, nullptr);
#else
	const ssize_t n = recvfrom(fd_, p, cap, 0, nullptr, nullptr);
#endif
	if (n < 0) {
		if (sock_interrupted(last_error()))
			return false;
		return false;
	}
	if (n == 0)
		return false;
	if (len)
		*len = static_cast<size_t>(n);
	return true;
}

bool OscTransport::start(const std::string &host, int send_port, int feedback_port, std::string *err)
{
	stop();
	if (!sock_.open(host, send_port, feedback_port, err))
		return false;
	{
		std::lock_guard<std::mutex> g(mx_);
		out_.clear();
		in_.clear();
		stop_ = false;
		running_ = true;
	}
	tx_ = std::thread(&OscTransport::send_loop, this);
	rx_ = std::thread(&OscTransport::recv_loop, this);
	return true;
}

void OscTransport::stop()
{
	{
		std::lock_guard<std::mutex> g(mx_);
		if (!running_ && !tx_.joinable() && !rx_.joinable()) {
			sock_.close();
			return;
		}
		stop_ = true;
		running_ = false;
		out_.clear();
	}
	cv_.notify_all();
	if (tx_.joinable())
		tx_.join();
	if (rx_.joinable())
		rx_.join(); /* its recv waits at most 100 ms, so this cannot hang */
	sock_.close();
	{
		std::lock_guard<std::mutex> g(mx_);
		in_.clear();
		stop_ = false;
	}
}

bool OscTransport::send(const uint8_t *data, size_t len)
{
	if (!data || len == 0)
		return false;
	std::lock_guard<std::mutex> g(mx_);
	if (!running_)
		return false;
	if (out_.size() >= kMaxQueued) {
		dropped_++;
		return false;
	}
	out_.push_back(std::vector<uint8_t>(data, data + len));
	cv_.notify_one();
	return true;
}

bool OscTransport::pop(std::vector<uint8_t> &msg)
{
	std::lock_guard<std::mutex> g(mx_);
	if (in_.empty())
		return false;
	msg = std::move(in_.front());
	in_.pop_front();
	return true;
}

uint64_t OscTransport::sent() const
{
	std::lock_guard<std::mutex> g(mx_);
	return sent_;
}

uint64_t OscTransport::received() const
{
	std::lock_guard<std::mutex> g(mx_);
	return recv_;
}

uint64_t OscTransport::dropped() const
{
	std::lock_guard<std::mutex> g(mx_);
	return dropped_;
}

void OscTransport::send_loop()
{
	for (;;) {
		std::vector<uint8_t> msg;
		{
			std::unique_lock<std::mutex> lk(mx_);
			cv_.wait(lk, [this] { return stop_ || !out_.empty(); });
			if (out_.empty())
				return; /* stop_ and nothing left to send */
			msg = std::move(out_.front());
			out_.pop_front();
		}
		if (sock_.send(msg.data(), msg.size(), 200)) {
			std::lock_guard<std::mutex> lk(mx_);
			sent_++;
		}
	}
}

void OscTransport::recv_loop()
{
	uint8_t buf[8192];
	for (;;) {
		{
			std::lock_guard<std::mutex> lk(mx_);
			if (stop_)
				return;
		}
		size_t n = 0;
		if (!sock_.recv(buf, sizeof(buf), &n, 100))
			continue; /* timeout or error: check stop_ again */
		std::lock_guard<std::mutex> lk(mx_);
		if (in_.size() < kMaxQueued)
			in_.push_back(std::vector<uint8_t>(buf, buf + n));
		recv_++;
	}
}

} // namespace cdnet
