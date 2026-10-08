#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wp::options {

enum class Action { Up, Down, Previous, Next, Close };

struct Row {
    std::string key;
    std::string label;
    std::string value;
    bool live = false;
};

bool live(const std::string& key);
std::vector<std::string> choices(const std::string& key);
std::string shown_value(const std::string& key, const std::string& value);
std::string step_value(const std::string& key, const std::string& value, int step);

class Menu {
public:
    bool open() const { return open_; }
    void set_open(bool open);
    size_t selection() const { return selection_; }
    void select(size_t index);
    std::vector<Row> rows() const;
    std::string handle(Action action);

private:
    bool open_ = false;
    size_t selection_ = 0;
};

class Repeat {
public:
    Repeat(uint32_t repeating, int64_t delay, int64_t interval) : repeating_(repeating), delay_(delay), interval_(interval) {}
    uint32_t update(uint32_t held, int64_t now);

private:
    uint32_t repeating_;
    int64_t delay_;
    int64_t interval_;
    uint32_t previous_ = 0;
    int64_t next_ = 0;
};

struct Layout {
    int unit = 0;
    int width = 0;
    int height = 0;
    int padding = 0;
    int title_height = 0;
    int row_height = 0;
    int footer_height = 0;
    size_t rows = 0;
};

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

Layout layout(int client_height, size_t rows);
Rect place(int client_width, int client_height, int width, int height);
int row_at(const Layout& layout, const Rect& placed, int x, int y);

struct Image {
    std::vector<uint32_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
};

void show_overlay(Image image);
void hide_overlay();
bool take_overlay(uint64_t& version, Image& image, bool& visible);

}
