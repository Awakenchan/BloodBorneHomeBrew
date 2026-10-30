// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace BbEngine {
using Callback = std::int64_t (__attribute__((sysv_abi)) *)(std::uint64_t, const std::uint64_t*);
constexpr std::ptrdiff_t kSavedXmm = 152;
inline const std::uint8_t* SavedXmm(const std::uint64_t* saved, unsigned n) {
    return reinterpret_cast<const std::uint8_t*>(saved) - kSavedXmm + 16 * n;
}

struct Hook {
    std::uint8_t* site = nullptr;
    std::uint8_t* stub = nullptr;
    std::size_t bytes = 0;
};
bool Prepare(Hook& hook, std::uint8_t* image, std::size_t size, std::size_t offset,
             std::span<const std::uint8_t> expected, Callback callback);
std::uint8_t* OriginalStub(const Hook& hook);
void Discard(Hook& hook);
std::uint8_t* TailStub(Callback callback, std::uint64_t resume);
void FreeStub(std::uint8_t* stub);
void Commit(std::span<Hook> hooks);
bool WriteCode(void* address, const void* bytes, std::size_t size);

template <typename T> std::int64_t Word(T value) {
    if constexpr (std::is_pointer_v<T>) return static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(value));
    else return static_cast<std::int64_t>(value);
}
template <typename... A> std::int64_t Call(std::uint64_t fn, A... args) {
    static_assert(sizeof...(A) <= 7, "the guest bridge passes at most seven arguments");
    using W = std::int64_t;
    if constexpr (sizeof...(A) == 7) {
        using Fn = W (__attribute__((sysv_abi)) *)(W, W, W, W, W, W, W);
        return reinterpret_cast<Fn>(fn)(Word(args)...);
    } else {
        std::array<W, 6> r{};
        std::size_t i = 0;
        ((r[i++] = Word(args)), ...);
        using Fn = W (__attribute__((sysv_abi)) *)(W, W, W, W, W, W);
        return reinterpret_cast<Fn>(fn)(r[0], r[1], r[2], r[3], r[4], r[5]);
    }
}
void Install(std::uint8_t* image, std::size_t size);
}
