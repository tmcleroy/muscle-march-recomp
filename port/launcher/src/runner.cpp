#include "runner.h"

#include <cstdlib>

#include "texts.h"

namespace {

bool ninja_progress(const std::string& line, float& fraction) {
    if (line.size() < 5 || line[0] != '[') {
        return false;
    }
    char* end = nullptr;
    long done = std::strtol(line.c_str() + 1, &end, 10);
    if (!end || *end != '/') {
        return false;
    }
    long total = std::strtol(end + 1, &end, 10);
    if (!end || *end != ']' || total <= 0) {
        return false;
    }
    fraction = static_cast<float>(done) / static_cast<float>(total);
    return true;
}

}

Runner::~Runner() {
    cancel();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void Runner::start(std::vector<Step> steps, std::filesystem::path folder, std::function<void()> wake) {
    if (running_) {
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    steps_ = std::move(steps);
    states_.assign(steps_.size(), StepState::Waiting);
    folder_ = std::move(folder);
    wake_ = std::move(wake);
    pending_.clear();
    partial_.clear();
    current_ = -1;
    step_fraction_ = 0.0f;
    outcome_ = Outcome::None;
    cancelled_ = false;
    running_ = true;
    thread_ = std::thread([this] { run(); });
}

void Runner::cancel() {
    cancelled_ = true;
    std::lock_guard<std::mutex> lock(mutex_);
    if (process_) {
        process_->kill();
    }
}

bool Runner::running() const {
    return running_;
}

RunnerView Runner::view() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RunnerView view;
    view.states = states_;
    view.current = current_;
    view.outcome = outcome_;
    float total = 0.0f;
    float done = 0.0f;
    for (size_t i = 0; i < steps_.size(); i++) {
        view.titles.push_back(steps_[i].title);
        total += steps_[i].weight;
        if (states_[i] == StepState::Done || states_[i] == StepState::Skipped) {
            done += steps_[i].weight;
        } else if (states_[i] == StepState::Running) {
            done += steps_[i].weight * step_fraction_;
        }
    }
    view.progress = total > 0.0f ? done / total : 0.0f;
    if (step_fraction_ > 0.05f && step_fraction_ < 1.0f) {
        double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - fraction_start_).count();
        view.remaining_seconds = elapsed * (1.0 - step_fraction_) / step_fraction_;
    }
    return view;
}

void Runner::take_output(std::vector<std::string>& lines) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::string& line : pending_) {
        lines.push_back(std::move(line));
    }
    pending_.clear();
}

void Runner::emit(const std::string& text) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        partial_ += text;
    }
    feed("", 0);
}

void Runner::feed(const char* data, size_t size) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        partial_.append(data, size);
        size_t start = 0;
        for (size_t i = 0; i < partial_.size(); i++) {
            if (partial_[i] != '\n' && partial_[i] != '\r') {
                continue;
            }
            if (i > start) {
                std::string line = partial_.substr(start, i - start);
                float fraction = 0.0f;
                if (ninja_progress(line, fraction)) {
                    if (step_fraction_ == 0.0f) {
                        fraction_start_ = std::chrono::steady_clock::now();
                    }
                    step_fraction_ = fraction;
                }
                pending_.push_back(std::move(line));
            }
            start = i + 1;
        }
        partial_.erase(0, start);
    }
    if (wake_) {
        wake_();
    }
}

bool Runner::run_program(size_t index) {
    const Step& step = steps_[index];
    std::string program = find_program(step.program);
    if (program.empty()) {
        emit(format(texts().missing_tool, step.program) + "\n");
        return false;
    }
    std::string line = "> " + step.program;
    for (const std::string& argument : step.arguments) {
        line += " " + argument;
    }
    emit(line + "\n");
    Process process;
    std::string error;
    if (!process.start(program, step.arguments, folder_, true, error)) {
        emit(error + "\n");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        process_ = &process;
    }
    if (cancelled_) {
        process.kill();
    }
    char buffer[8192];
    while (size_t count = process.read(buffer, sizeof(buffer))) {
        feed(buffer, count);
    }
    int code = process.wait();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        process_ = nullptr;
        if (!partial_.empty()) {
            partial_ += '\n';
        }
    }
    feed("", 0);
    return code == 0 && !cancelled_;
}

void Runner::run() {
    bool success = true;
    for (size_t i = 0; i < steps_.size() && success; i++) {
        if (cancelled_) {
            success = false;
            break;
        }
        const Step& step = steps_[i];
        {
            std::lock_guard<std::mutex> lock(mutex_);
            current_ = static_cast<int>(i);
            step_fraction_ = 0.0f;
            states_[i] = StepState::Running;
        }
        if (wake_) {
            wake_();
        }
        if (step.skip && step.skip()) {
            std::lock_guard<std::mutex> lock(mutex_);
            states_[i] = StepState::Skipped;
            continue;
        }
        bool done = false;
        if (step.action) {
            std::string message;
            done = step.action(message);
            if (!message.empty()) {
                emit(message + "\n");
            }
        } else {
            done = run_program(i);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        states_[i] = done ? StepState::Done : StepState::Failed;
        success = done;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_ = -1;
        step_fraction_ = 0.0f;
        outcome_ = success ? Outcome::Success : cancelled_ ? Outcome::Cancelled : Outcome::Failure;
    }
    running_ = false;
    if (wake_) {
        wake_();
    }
}
