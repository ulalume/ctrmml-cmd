// Linked into ctrmml-player-heap-probe, a copy of the player with a large
// memory, for tests/player/parity.mjs to measure what the player needs.

#include <malloc.h>
#include <cstddef>

extern "C" unsigned char __heap_base;

//! Where the heap starts: static data and the stack lie below it.
extern "C" __attribute__((export_name("probe_heap_base"))) size_t probe_heap_base()
{
	return reinterpret_cast<size_t>(&__heap_base);
}

//! The most heap memory taken from the system so far.
extern "C" __attribute__((export_name("probe_heap_peak"))) size_t probe_heap_peak()
{
	return mallinfo().usmblks;
}
