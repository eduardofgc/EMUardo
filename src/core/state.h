#pragma once

#include <array>
#include <cstring>
#include <type_traits>
#include <vector>

#include "core/types.h"

namespace gba {

// Save-state serialization helpers. Every {Cpu,Ppu,Timers,Dma,Apu,Bus,...}
// gets a SaveState(StateWriter&)/LoadState(StateReader&) pair that writes/
// reads its own members in a fixed order - there's no versioning or field
// tagging, just a flat byte stream both sides agree on by construction
// (matching Emulator::SaveState()/LoadState()'s call order exactly). That's
// fine for this project's actual use case (a save state only ever gets
// loaded back by the same emulator build that wrote it, right next to the
// ROM it came from) - it deliberately isn't attempting the kind of forward/
// backward format compatibility a shipped save-state format across engine
// versions would need.
class StateWriter {
public:
    void WriteBytes(const void* data, std::size_t len) {
        const auto* bytes = static_cast<const u8*>(data);
        buffer_.insert(buffer_.end(), bytes, bytes + len);
    }

    template <typename T>
    void Write(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>, "Write<T>() needs a plain-data type");
        WriteBytes(&value, sizeof(T));
    }

    template <typename T, std::size_t N>
    void Write(const std::array<T, N>& arr) {
        WriteBytes(arr.data(), sizeof(T) * N);
    }

    template <typename T, std::size_t N>
    void Write(const T (&arr)[N]) {
        WriteBytes(arr, sizeof(T) * N);
    }

    // Length-prefixed, for the handful of fields whose size varies by save
    // chip type (SaveMemory's backing store) rather than being fixed at
    // compile time.
    void WriteVector(const std::vector<u8>& v) {
        const u32 size = static_cast<u32>(v.size());
        Write(size);
        WriteBytes(v.data(), v.size());
    }

    const std::vector<u8>& data() const { return buffer_; }

private:
    std::vector<u8> buffer_;
};

class StateReader {
public:
    explicit StateReader(const std::vector<u8>& data) : data_(data) {}

    bool ReadBytes(void* out, std::size_t len) {
        if (!ok_ || pos_ + len > data_.size()) {
            ok_ = false;
            return false;
        }
        std::memcpy(out, data_.data() + pos_, len);
        pos_ += len;
        return true;
    }

    template <typename T>
    bool Read(T& value) {
        static_assert(std::is_trivially_copyable_v<T>, "Read<T>() needs a plain-data type");
        return ReadBytes(&value, sizeof(T));
    }

    template <typename T, std::size_t N>
    bool Read(std::array<T, N>& arr) {
        return ReadBytes(arr.data(), sizeof(T) * N);
    }

    template <typename T, std::size_t N>
    bool Read(T (&arr)[N]) {
        return ReadBytes(arr, sizeof(T) * N);
    }

    bool ReadVector(std::vector<u8>& v) {
        u32 size = 0;
        if (!Read(size)) {
            return false;
        }
        v.resize(size);
        return ReadBytes(v.data(), size);
    }

    // False once any read has failed (short/corrupt buffer) - every
    // *::LoadState() should check this once at the end rather than after
    // each individual field, so a truncated file fails cleanly instead of
    // reading garbage into later fields.
    bool ok() const { return ok_; }

private:
    const std::vector<u8>& data_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

} // namespace gba
