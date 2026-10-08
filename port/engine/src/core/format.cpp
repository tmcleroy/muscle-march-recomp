#include "wp/format.h"

#include <cstdio>
#include <cstring>

namespace wp {

namespace {

constexpr uint32_t kFirstArgumentRegister = 3;
constexpr uint32_t kLastArgumentRegister = 10;
constexpr uint32_t kStackArgumentOffset = 8;
constexpr uint32_t kFirstFloatRegister = 1;
constexpr uint32_t kLastFloatRegister = 8;
constexpr size_t kMaxText = 4096;

std::string read_string(uint32_t address) {
    std::string text;
    for (char ch; (ch = static_cast<char>(rd8(address))) != 0 && text.size() < kMaxText; address++) {
        text.push_back(ch);
    }
    return text;
}

class Arguments {
public:
    Arguments(const Cpu& c, uint32_t first_register)
        : c_(c), next_register_(first_register), next_float_(kFirstFloatRegister), stack_(c.r[1] + kStackArgumentOffset) {}

    uint32_t integer() {
        if (next_register_ <= kLastArgumentRegister) {
            return c_.r[next_register_++];
        }
        uint32_t value = rd32(stack_);
        stack_ += 4;
        return value;
    }

    uint64_t wide() {
        if (next_register_ <= kLastArgumentRegister) {
            if ((next_register_ - kFirstArgumentRegister) % 2 != 0) {
                next_register_++;
            }
        } else if (stack_ % 8 != 0) {
            stack_ += 4;
        }
        uint64_t high = integer();
        uint64_t low = integer();
        return (high << 32) | low;
    }

    double floating() {
        if (next_float_ <= kLastFloatRegister) {
            return c_.f[next_float_++];
        }
        if (stack_ % 8 != 0) {
            stack_ += 4;
        }
        double value = rdf64(stack_);
        stack_ += 8;
        return value;
    }

private:
    const Cpu& c_;
    uint32_t next_register_;
    uint32_t next_float_;
    uint32_t stack_;
};

}

std::string format_guest(const Cpu& c, uint32_t format, uint32_t first_argument_register) {
    Arguments arguments(c, first_argument_register);
    std::string text = read_string(format);
    std::string output;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] != '%') {
            output.push_back(text[i]);
            continue;
        }
        std::string spec = "%";
        int length = 0;
        while (++i < text.size() && std::strchr("-+ #0123456789.*hlLzjt", text[i])) {
            if (text[i] == '*') {
                spec += std::to_string(static_cast<int32_t>(arguments.integer()));
            } else if (text[i] == 'l') {
                length++;
            } else if (text[i] != 'h' && text[i] != 'z' && text[i] != 'j' && text[i] != 't' && text[i] != 'L') {
                spec.push_back(text[i]);
            }
        }
        if (i >= text.size()) {
            break;
        }
        char conversion = text[i];
        char buffer[512];
        switch (conversion) {
        case '%':
            output.push_back('%');
            break;
        case 'd':
        case 'i':
            if (length >= 2) {
                std::snprintf(buffer, sizeof buffer, (spec + "lld").c_str(), static_cast<long long>(arguments.wide()));
            } else {
                std::snprintf(buffer, sizeof buffer, (spec + "d").c_str(), static_cast<int32_t>(arguments.integer()));
            }
            output += buffer;
            break;
        case 'u':
        case 'x':
        case 'X':
        case 'o':
            if (length >= 2) {
                std::snprintf(buffer, sizeof buffer, (spec + "ll" + conversion).c_str(),
                              static_cast<unsigned long long>(arguments.wide()));
            } else {
                std::snprintf(buffer, sizeof buffer, (spec + conversion).c_str(), arguments.integer());
            }
            output += buffer;
            break;
        case 'c':
            std::snprintf(buffer, sizeof buffer, (spec + "c").c_str(), static_cast<int>(arguments.integer() & 0xFF));
            output += buffer;
            break;
        case 's': {
            uint32_t address = arguments.integer();
            std::snprintf(buffer, sizeof buffer, (spec + "s").c_str(), address ? read_string(address).c_str() : "(null)");
            output += buffer;
            break;
        }
        case 'p':
            std::snprintf(buffer, sizeof buffer, "0x%08x", arguments.integer());
            output += buffer;
            break;
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
            std::snprintf(buffer, sizeof buffer, (spec + conversion).c_str(), arguments.floating());
            output += buffer;
            break;
        default:
            output += spec + conversion;
            break;
        }
    }
    return output;
}

}
