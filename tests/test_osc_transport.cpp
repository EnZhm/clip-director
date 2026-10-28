// Transport: a real UDP loopback round trip through the same code the plugin ships (POSIX
// sockets here, Winsock on Windows), plus a clean shutdown.
#include "doctest.h"

#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

#include "osc.h"
#include "osc-transport.h"

using namespace cd;

namespace {

/* poll a transport's receive queue for up to `ms`, returning the first datagram */
bool wait_pop(cdnet::OscTransport &t, std::vector<uint8_t> &msg, int ms = 2000)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	for (;;) {
		if (t.pop(msg))
			return true;
		if (std::chrono::steady_clock::now() > deadline)
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

bool wait_pop_socket(cdnet::UdpSocket &s, std::vector<uint8_t> &msg, int ms = 2000)
{
	uint8_t buf[2048];
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	for (;;) {
		size_t n = 0;
		if (s.recv(buf, sizeof(buf), &n, 50)) {
			msg.assign(buf, buf + n);
			return true;
		}
		if (std::chrono::steady_clock::now() > deadline)
			return false;
	}
}

void check_scene_message(const std::vector<uint8_t> &wire, const char *scene, const char *preset)
{
	OscMessage m;
	REQUIRE(osc_decode(wire, m));
	CHECK(m.address == std::string("/scenes/") + scene);
	if (preset && *preset) {
		REQUIRE(m.args.size() == 1u);
		CHECK(m.args[0].s == preset);
	} else {
		CHECK(m.args.empty());
	}
}

} // namespace

TEST_CASE("transport: send /scenes/x and receive it on a feedback socket")
{
	// the peer binds an ephemeral port we can echo back to
	cdnet::UdpSocket peer;
	std::string err;
	REQUIRE(peer.open("127.0.0.1", 40001, 0, &err));
	REQUIRE(peer.valid());
	REQUIRE(peer.local_port() > 0);

	// the transport sends to the peer and listens on its own ephemeral feedback port
	cdnet::OscTransport link;
	REQUIRE(link.start("127.0.0.1", peer.local_port(), 0, &err));
	REQUIRE(link.running());
	REQUIRE(link.local_port() > 0);

	const std::vector<uint8_t> wire = [] {
		std::vector<uint8_t> v;
		REQUIRE(osc_encode(osc_scene("tunnel", "acid"), v));
		return v;
	}();
	REQUIRE(link.send(wire.data(), wire.size()));

	// 1. the message arrives on the peer (this is what Synesthesia would receive)
	std::vector<uint8_t> got;
	REQUIRE(wait_pop_socket(peer, got));
	check_scene_message(got, "tunnel", "acid");
	CHECK(got == wire);

	// 2. Synesthesia's echo comes back on the feedback port and is queued for the dock.
	//    (Synesthesia answers from its own socket, so a second one stands in for it here.)
	const std::vector<uint8_t> echo = [] {
		std::vector<uint8_t> v;
		REQUIRE(osc_encode(osc_scene("tunnel", ""), v));
		return v;
	}();
	cdnet::UdpSocket syn_out;
	REQUIRE(syn_out.open("127.0.0.1", link.local_port(), 0, &err));
	REQUIRE(syn_out.send(echo.data(), echo.size()));
	std::vector<uint8_t> back;
	REQUIRE(wait_pop(link, back));
	check_scene_message(back, "tunnel", "");
	CHECK(link.sent() >= 1u);
	CHECK(link.received() >= 1u);

	link.stop();
	CHECK_FALSE(link.running());
	syn_out.close();
	peer.close();
}

TEST_CASE("transport: an address-only message survives the round trip, and stop() is clean")
{
	cdnet::UdpSocket peer;
	std::string err;
	REQUIRE(peer.open("localhost", 40002, 0, &err));

	cdnet::OscTransport link;
	REQUIRE(link.start("localhost", peer.local_port(), 0, &err));
	const std::vector<uint8_t> wire = [] {
		std::vector<uint8_t> v;
		REQUIRE(osc_encode(osc_playlist_next(), v));
		return v;
	}();
	REQUIRE(link.send(wire.data(), wire.size()));
	std::vector<uint8_t> got;
	REQUIRE(wait_pop_socket(peer, got));
	OscMessage m;
	REQUIRE(osc_decode(got, m));
	CHECK(m.address == "/playlist/next");
	CHECK(m.args.empty());

	// several stops in a row, and send after stop, are all safe
	link.stop();
	link.stop();
	CHECK_FALSE(link.running());
	CHECK_FALSE(link.send(wire.data(), wire.size()));
	peer.close();
}

TEST_CASE("transport: a bound feedback port receives, and a bad host fails to start")
{
	std::string err;
	cdnet::OscTransport a;
	REQUIRE(a.start("127.0.0.1", 40003, 0, &err));
	const int port = a.local_port();
	REQUIRE(port > 0);

	// a second transport pointed at the first one's feedback port
	cdnet::OscTransport b;
	REQUIRE(b.start("127.0.0.1", port, 0, &err));
	const std::vector<uint8_t> wire = [] {
		std::vector<uint8_t> v;
		REQUIRE(osc_encode(osc_bang(1), v));
		return v;
	}();
	REQUIRE(b.send(wire.data(), wire.size()));
	std::vector<uint8_t> got;
	REQUIRE(wait_pop(a, got));
	OscMessage m;
	REQUIRE(osc_decode(got, m));
	CHECK(m.address == "/controls/global/bang/1");
	REQUIRE(m.args.size() == 1u);
	CHECK(m.args[0].f == doctest::Approx(1.0f));
	a.stop();
	b.stop();

	// a host that cannot resolve fails cleanly, leaving the transport stopped
	cdnet::OscTransport bad;
	CHECK_FALSE(bad.start("host.invalid.", 7000, 0, &err));
	CHECK_FALSE(bad.running());
	CHECK_FALSE(err.empty());
	CHECK_FALSE(bad.send(wire.data(), wire.size()));
}

TEST_CASE("transport: a socket reports its bound port and refuses a nonsensical destination")
{
	std::string err;
	cdnet::UdpSocket s;
	CHECK_FALSE(s.valid());
	CHECK(s.local_port() == 0);
	CHECK_FALSE(s.send(nullptr, 0));
	uint8_t buf[8];
	size_t n = 0;
	CHECK_FALSE(s.recv(buf, sizeof(buf), &n, 10));

	REQUIRE(s.open("127.0.0.1", 40004, 0, &err));
	CHECK(s.valid());
	CHECK(s.local_port() > 0);
	// nothing is listening on that port here, but the send itself is fine, and recv times out
	const std::vector<uint8_t> wire = {'/', 'x', 0, 0, ',', 0, 0, 0};
	CHECK(s.send(wire.data(), wire.size(), 50));
	CHECK_FALSE(s.recv(buf, sizeof(buf), &n, 60));
	s.close();
	CHECK_FALSE(s.valid());
}
