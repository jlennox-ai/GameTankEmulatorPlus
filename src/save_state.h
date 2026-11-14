#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <vector>

// A save state's bytes, read or written field by field in a fixed order. Each part of the
// emulator has one SerializeState(StateFile&) that lists its fields, used both to save and to
// load, so the two can't drift apart.
class StateFile {
public:
    enum class Mode { Save, Load };

    // Saving appends to bytes; loading reads them from the start.
    StateFile(Mode mode, std::vector<uint8_t>& bytes) : mode(mode), bytes(bytes) {}

    bool Loading() const { return mode == Mode::Load; }

    template<typename T>
    void Value(T& value) {
        static_assert(std::is_trivially_copyable<T>::value, "only plain values can be saved as bytes");
        Bytes(&value, sizeof(T));
    }

    void Bytes(void* data, size_t size) {
        if(mode == Mode::Save) {
            const uint8_t* source = (const uint8_t*) data;
            bytes.insert(bytes.end(), source, source + size);
            return;
        }
        if(position + size > bytes.size()) {
            throw std::runtime_error("the save state ends early");
        }
        std::memcpy(data, bytes.data() + position, size);
        position += size;
    }

    // Loading: whether every byte was read (a longer file is from a different emulator build).
    bool AtEnd() const { return position == bytes.size(); }

private:
    Mode mode;
    std::vector<uint8_t>& bytes;
    size_t position = 0;
};
