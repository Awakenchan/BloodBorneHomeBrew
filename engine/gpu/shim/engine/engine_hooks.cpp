// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine_hooks.h"
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace BbEngine {
namespace {
constexpr std::size_t kPage = 4096;
constexpr std::size_t kReservation = 64 * 1024;

constexpr std::uint32_t kFrame = 0x98;
constexpr std::uint32_t kSavedRax = 0x88;
constexpr std::uint32_t kReturnSlot = 0x90;
static_assert(kFrame == kSavedXmm, "SavedXmm() reads the stub's frame");
static_assert(kReturnSlot + 8 == kFrame, "saved[-1] sits just below saved[0]");
static_assert((8 + 6 * 8 + kFrame) % 16 == 0, "the host call needs a 16-byte aligned stack");

enum Reg : std::uint8_t { RAX = 0, RCX = 1, RDX = 2, RSI = 6, RDI = 7, R8 = 8, R9 = 9 };
constexpr Reg kArgs[] = {RDI, RSI, RDX, RCX, R8, R9};

class Emitter {
public:
    explicit Emitter(std::uint8_t* at) : at_(at) {}
    std::uint8_t* Here() const { return at_; }
    void Put(std::initializer_list<std::uint8_t> bytes) { for (auto b : bytes) *at_++ = b; }
    void Copy(const std::uint8_t* bytes, std::size_t n) { std::memcpy(at_, bytes, n); at_ += n; }
    template <typename T> void Value(T v) { std::memcpy(at_, &v, sizeof v); at_ += sizeof v; }

    void Push(Reg r) { if (r >= 8) Put({0x41}); Put({std::uint8_t(0x50 + (r & 7))}); }
    void Pop(Reg r) { if (r >= 8) Put({0x41}); Put({std::uint8_t(0x58 + (r & 7))}); }
    void SubRsp(std::uint32_t n) { Put({0x48, 0x81, 0xec}); Value(n); }
    void AddRsp(std::uint32_t n) { Put({0x48, 0x81, 0xc4}); Value(n); }
    void Store(std::uint32_t disp, Reg r) { Put({0x48, 0x89, std::uint8_t(0x84 | (r << 3)), 0x24}); Value(disp); }
    void Load(Reg r, std::uint32_t disp) { Put({0x48, 0x8b, std::uint8_t(0x84 | (r << 3)), 0x24}); Value(disp); }
    void StoreZero(std::uint32_t disp) { Put({0x48, 0xc7, 0x84, 0x24}); Value(disp); Value(std::uint32_t(0)); }
    void StoreXmm(std::uint8_t disp, unsigned n) { Put({0x0f, 0x11, std::uint8_t(0x44 | (n << 3)), 0x24, disp}); }
    void LoadXmm(unsigned n, std::uint8_t disp) { Put({0x0f, 0x10, std::uint8_t(0x44 | (n << 3)), 0x24, disp}); }
    void LeaRsi(std::uint32_t disp) { Put({0x48, 0x8d, 0xb4, 0x24}); Value(disp); }
    void ZeroEdi() { Put({0x31, 0xff}); }
    void MovRax(std::uint64_t v) { Put({0x48, 0xb8}); Value(v); }
    void CallRax() { Put({0xff, 0xd0}); }
    void TestRax() { Put({0x48, 0x85, 0xc0}); }
    std::uint8_t* Jz8() { Put({0x74, 0x00}); return at_ - 1; }
    void Land(std::uint8_t* rel8) { *rel8 = std::uint8_t(at_ - (rel8 + 1)); }
    void Ret() { Put({0xc3}); }
    void JumpTo(std::uint64_t target) { Put({0xff, 0x25, 0, 0, 0, 0}); Value(target); }

private:
    std::uint8_t* at_;
};
constexpr std::size_t kJumpBytes = 14;

std::size_t WriteStub(std::uint8_t* at, Callback callback, std::uint64_t resume,
                      const std::uint8_t* moved, std::size_t n) {
    Emitter e(at);
    for (Reg r : kArgs) e.Push(r);
    e.SubRsp(kFrame);
    e.Store(kSavedRax, RAX);
    for (unsigned x = 0; x < 8; ++x) e.StoreXmm(std::uint8_t(16 * x), x);
    e.StoreZero(kReturnSlot);
    e.ZeroEdi();
    e.LeaRsi(kFrame);
    e.MovRax(reinterpret_cast<std::uint64_t>(callback));
    e.CallRax();
    e.TestRax();
    std::uint8_t* run = e.Jz8();
    e.Load(RAX, kReturnSlot);
    e.AddRsp(kFrame);
    for (int i = 5; i >= 0; --i) e.Pop(kArgs[i]);
    e.Ret();
    e.Land(run);
    for (unsigned x = 0; x < 8; ++x) e.LoadXmm(x, std::uint8_t(16 * x));
    e.Load(RAX, kSavedRax);
    e.AddRsp(kFrame);
    for (int i = 5; i >= 0; --i) e.Pop(kArgs[i]);
    if (n) e.Copy(moved, n);
    e.JumpTo(resume);
    return std::size_t(e.Here() - at);
}

class StubPages {
public:
    template <typename W> std::uint8_t* Make(W&& write) {
        std::scoped_lock lock(mutex_);
        std::uint8_t* page = Take();
        if (!page) return nullptr;
        if (!Writable(page, true)) { free_.push_back(page); return nullptr; }
        write(page);
        if (!Writable(page, false)) { Give(page); return nullptr; }
        return page;
    }
    void Free(std::uint8_t* page) {
        if (!page) return;
        std::scoped_lock lock(mutex_);
        Give(page);
    }

private:
    std::uint8_t* Take() {
        if (!free_.empty()) { auto* p = free_.back(); free_.pop_back(); return p; }
        if (!next_ || next_ == end_) {
#ifdef _WIN32
            next_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kReservation, MEM_RESERVE, PAGE_NOACCESS));
#else
            void* r = mmap(nullptr, kReservation, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            next_ = r == MAP_FAILED ? nullptr : static_cast<std::uint8_t*>(r);
#endif
            if (!next_) { end_ = nullptr; return nullptr; }
            end_ = next_ + kReservation;
        }
        std::uint8_t* page = next_;
        next_ += kPage;
        return page;
    }
    void Give(std::uint8_t* page) {
#ifdef _WIN32
        VirtualFree(page, kPage, MEM_DECOMMIT);
#else
        mprotect(page, kPage, PROT_NONE);
        madvise(page, kPage, MADV_DONTNEED);
#endif
        free_.push_back(page);
    }
    static bool Writable(std::uint8_t* page, bool writable) {
#ifdef _WIN32
        if (writable) return VirtualAlloc(page, kPage, MEM_COMMIT, PAGE_READWRITE) != nullptr;
        DWORD before = 0;
        return VirtualProtect(page, kPage, PAGE_EXECUTE_READ, &before) &&
               FlushInstructionCache(GetCurrentProcess(), page, kPage);
#else
        if (mprotect(page, kPage, writable ? PROT_READ | PROT_WRITE : PROT_READ | PROT_EXEC)) return false;
        if (!writable) __builtin___clear_cache(reinterpret_cast<char*>(page), reinterpret_cast<char*>(page + kPage));
        return true;
#endif
    }

    std::mutex mutex_;
    std::uint8_t* next_ = nullptr;
    std::uint8_t* end_ = nullptr;
    std::vector<std::uint8_t*> free_;
};
StubPages& Pages() {
    static StubPages pages;
    return pages;
}

void FlushCode(void* at, std::size_t n) {
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), at, n);
#else
    __builtin___clear_cache(static_cast<char*>(at), static_cast<char*>(at) + n);
#endif
}
}

bool Prepare(Hook& hook, std::uint8_t* image, std::size_t size, std::size_t offset,
             std::span<const std::uint8_t> expected, Callback callback) {
    hook = {};
    const std::size_t n = expected.size();
    if (!image || !callback || n < kJumpBytes || n > 128 || offset > size || n > size - offset) return false;
    std::uint8_t* site = image + offset;
    if (std::memcmp(site, expected.data(), n) != 0) return false;
    const auto resume = reinterpret_cast<std::uint64_t>(site + n);
    std::uint8_t* stub = Pages().Make([&](std::uint8_t* page) { WriteStub(page, callback, resume, expected.data(), n); });
    if (!stub) return false;
    hook = {site, stub, n};
    return true;
}

std::uint8_t* OriginalStub(const Hook& hook) {
    if (!hook.site || !hook.stub) return nullptr;
    return Pages().Make([&](std::uint8_t* page) {
        Emitter e(page);
        e.Copy(hook.site, hook.bytes);
        e.JumpTo(reinterpret_cast<std::uint64_t>(hook.site + hook.bytes));
    });
}

void Discard(Hook& hook) {
    Pages().Free(hook.stub);
    hook = {};
}

std::uint8_t* TailStub(Callback callback, std::uint64_t resume) {
    if (!callback || !resume) return nullptr;
    return Pages().Make([&](std::uint8_t* page) { WriteStub(page, callback, resume, nullptr, 0); });
}

void FreeStub(std::uint8_t* stub) {
    Pages().Free(stub);
}

void Commit(std::span<Hook> hooks) {
    for (Hook& hook : hooks) {
        if (!hook.site || !hook.stub) continue;
#ifndef _WIN32
        const auto at = reinterpret_cast<std::uintptr_t>(hook.site);
        const std::uintptr_t first = at & ~std::uintptr_t(kPage - 1);
        const std::size_t span = ((at + hook.bytes + kPage - 1) & ~std::uintptr_t(kPage - 1)) - first;
        if (mprotect(reinterpret_cast<void*>(first), span, PROT_READ | PROT_WRITE | PROT_EXEC)) continue;
#endif
        Emitter e(hook.site);
        e.JumpTo(reinterpret_cast<std::uint64_t>(hook.stub));
        std::memset(e.Here(), 0xcc, hook.bytes - kJumpBytes);
        FlushCode(hook.site, hook.bytes);
    }
}

bool WriteCode(void* address, const void* bytes, std::size_t size) {
    if (!address || !bytes || !size) return false;
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    const std::uintptr_t first = at & ~std::uintptr_t(kPage - 1);
    const std::size_t span = ((at + size + kPage - 1) & ~std::uintptr_t(kPage - 1)) - first;
    auto* pages = reinterpret_cast<void*>(first);
#ifdef _WIN32
    DWORD before = 0, ignored = 0;
    if (!VirtualProtect(pages, span, PAGE_EXECUTE_READWRITE, &before)) return false;
    std::memcpy(address, bytes, size);
    const bool restored = VirtualProtect(pages, span, before, &ignored) != 0;
    FlushCode(address, size);
    return restored;
#else
    if (mprotect(pages, span, PROT_READ | PROT_WRITE | PROT_EXEC)) return false;
    std::memcpy(address, bytes, size);
    FlushCode(address, size);
    return mprotect(pages, span, PROT_READ | PROT_EXEC) == 0;
#endif
}
}
