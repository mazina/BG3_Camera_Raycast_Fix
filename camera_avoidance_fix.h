#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

// Independent native correction. No Lua, SE ABI, or camera-event dependency.
namespace CameraAvoidanceFix {
enum class Match { Absent, Original, Corrected, Ambiguous };
Match Find(std::span<std::uint8_t const> text, std::size_t& offset);
class Patch {
public:
    bool Prepare(HMODULE game, std::string& reason);
    // Caller must suspend other game threads through Apply/Rollback.
    // On failure, call Rollback before resuming other threads.
    bool Apply();
    bool Rollback();
    bool AlreadyCorrected() const { return alreadyCorrected_; }
    bool ContainsInstruction(std::uintptr_t ip) const;
    std::uint32_t FunctionRva() const { return functionRva_; }
private:
    std::uint8_t* function_{};
    std::uint32_t functionRva_{};
    bool alreadyCorrected_{};
    bool applied_{};
    DWORD originalProtection_{};
};
}
