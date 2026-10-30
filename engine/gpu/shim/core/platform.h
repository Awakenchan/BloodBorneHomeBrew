// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <functional>
#include <mutex>
#include <queue>
#include <unordered_map>
#include "common/assert.h"
#include "common/singleton.h"
#include "common/types.h"

namespace Platform {

enum class InterruptId : u32 {
    Compute0RelMem = 0x00,
    Compute1RelMem = 0x01,
    Compute2RelMem = 0x02,
    Compute3RelMem = 0x03,
    Compute4RelMem = 0x04,
    Compute5RelMem = 0x05,
    Compute6RelMem = 0x06,
    GfxFlip = 0x08,
    GpuIdle = 0x09,
    GfxEop = 0x40,
    InterruptIdMax = 0x40,
};

using IrqHandler = std::function<void(InterruptId)>;

class IrqController {
public:
    void RegisterOnce(InterruptId irq, IrqHandler handler) {
        Slot& slot = SlotOf(irq);
        std::scoped_lock lock{slot.mutex};
        slot.once.push(std::move(handler));
    }

    void Register(InterruptId irq, IrqHandler handler, void* owner) {
        Slot& slot = SlotOf(irq);
        std::scoped_lock lock{slot.mutex};
        ASSERT_MSG(!slot.persistent.contains(owner), "The handler is already registered!");
        slot.persistent.emplace(owner, std::move(handler));
    }

    void Unregister(InterruptId irq, void* owner) {
        Slot& slot = SlotOf(irq);
        std::scoped_lock lock{slot.mutex};
        slot.persistent.erase(owner);
    }

    void Signal(InterruptId irq) {
        Slot& slot = SlotOf(irq);
        std::scoped_lock lock{slot.mutex};
        for (auto& [owner, handler] : slot.persistent) {
            handler(irq);
        }
        if (!slot.once.empty()) {
            slot.once.front()(irq);
            slot.once.pop();
        }
    }

private:
    struct Slot {
        std::mutex mutex;
        std::unordered_map<void*, IrqHandler> persistent;
        std::queue<IrqHandler> once;
    };

    Slot& SlotOf(InterruptId irq) {
        const u32 index = static_cast<u32>(irq);
        ASSERT_MSG(index <= static_cast<u32>(InterruptId::InterruptIdMax), "Invalid IRQ number");
        return slots[index];
    }

    std::array<Slot, static_cast<u32>(InterruptId::InterruptIdMax) + 1> slots;
};

using IrqC = Common::Singleton<IrqController>;

} // namespace Platform
