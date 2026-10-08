#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace wp::gx {

void process();
bool take_finish_interrupt();
void request_capture();
uint64_t frames_drawn();
void run_frame_task(std::function<void()> task);
// Divert command bytes written to the FIFO into out (nullptr: back to the FIFO). CPU thread only.
void capture_fifo(std::vector<uint8_t>* out);

}
