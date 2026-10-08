#pragma once

#include <cstdint>
#include <string>

#include "wp/input.h"

// Input movies: WP_RECORD=file writes every controller sample the game reads, WP_REPLAY=file
// feeds them back. Both imply deterministic mode (wp/deterministic.h), so the game reads its
// samples in the same order and a replay reproduces the run exactly, given the same starting
// save data.
namespace wp::movie {

bool recording();
bool replaying();
void record(uint32_t channel, const input::Sample& sample);
// The next recorded sample; false (and an empty sample) once the movie has ended.
bool next(uint32_t channel, input::Sample& sample);
uint64_t samples_used();
// The replayed movie has run out of samples.
bool ended();
// Feature set the replayed movie was recorded with (comma-separated; empty = original game).
std::string recorded_features();

}
