#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

class PointerWrap {
public:
    enum class Mode { Measure, Write, Read };

    PointerWrap() = default;
    PointerWrap(std::vector<uint8_t>& buffer, Mode mode) : m_buffer(&buffer), m_mode(mode) {}

    template <typename T>
    void Do(T& value) {
        Bytes(&value, sizeof value);
    }

    template <typename T>
    void DoArray(T* pointer, size_t count) {
        Bytes(pointer, sizeof(T) * count);
    }

    template <typename T, size_t N>
    void DoArray(T (&array)[N]) {
        Bytes(array, sizeof array);
    }

    template <typename T, size_t N>
    void DoArray(std::array<T, N>& array) {
        Bytes(array.data(), sizeof(T) * N);
    }

    bool IsReadMode() const { return m_mode == Mode::Read; }

private:
    void Bytes(void* data, size_t size) {
        if (!m_buffer) {
            return;
        }
        if (m_mode == Mode::Write) {
            const uint8_t* bytes = static_cast<const uint8_t*>(data);
            m_buffer->insert(m_buffer->end(), bytes, bytes + size);
        } else if (m_mode == Mode::Read && m_position + size <= m_buffer->size()) {
            std::memcpy(data, m_buffer->data() + m_position, size);
        }
        m_position += size;
    }

    std::vector<uint8_t>* m_buffer = nullptr;
    Mode m_mode = Mode::Measure;
    size_t m_position = 0;
};
