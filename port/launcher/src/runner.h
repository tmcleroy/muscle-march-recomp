#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "subprocess.h"

enum class StepState { Waiting, Running, Done, Skipped, Failed };
enum class Outcome { None, Success, Failure, Cancelled };

struct Step {
    std::string title;
    std::string program;
    std::vector<std::string> arguments;
    std::function<bool()> skip;
    std::function<bool(std::string& message)> action;
    float weight = 1.0f;
};

struct RunnerView {
    std::vector<std::string> titles;
    std::vector<StepState> states;
    int current = -1;
    float progress = 0.0f;
    double remaining_seconds = -1.0;
    Outcome outcome = Outcome::None;
};

class Runner {
public:
    ~Runner();

    void start(std::vector<Step> steps, std::filesystem::path folder, std::function<void()> wake);
    void cancel();
    bool running() const;
    RunnerView view() const;
    void take_output(std::vector<std::string>& lines);

private:
    void run();
    bool run_program(size_t index);
    void emit(const std::string& text);
    void feed(const char* data, size_t size);

    std::vector<Step> steps_;
    std::vector<StepState> states_;
    std::filesystem::path folder_;
    std::function<void()> wake_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::vector<std::string> pending_;
    std::string partial_;
    Process* process_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancelled_{false};
    int current_ = -1;
    float step_fraction_ = 0.0f;
    std::chrono::steady_clock::time_point fraction_start_;
    Outcome outcome_ = Outcome::None;
};
