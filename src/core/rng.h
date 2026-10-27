#pragma once
#include <cstdint>

namespace cd {

// mulberry32 (copied from palette-director src/director/rng.h, same author/licence).
// One seeded RNG drives random-off, random palette changes and shuffle order.
class Mulberry32 {
public:
	explicit Mulberry32(uint32_t seed) : state_(seed) {}

	// Next value in [0, 1). Advances the 32-bit state in place.
	double next()
	{
		state_ += 0x6D2B79F5u; // a = a + 0x6D2B79F5 (mod 2^32)
		uint32_t t = state_;
		t = (t ^ (t >> 15)) * (1u | t);             // Math.imul(a ^ a>>>15, 1|a)
		t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t; // t + Math.imul(t^t>>>7, 61|t) ^ t
		return static_cast<double>(t ^ (t >> 14)) / 4294967296.0;
	}

	uint32_t state() const { return state_; }
	void reseed(uint32_t seed) { state_ = seed; }

private:
	uint32_t state_;
};

} // namespace cd
