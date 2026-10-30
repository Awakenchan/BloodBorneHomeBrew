// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <condition_variable>
#include <mutex>
#include "common/types.h"
#include "core/libraries/videoout/buffer.h"
#include "sys/video_out.h"

namespace Libraries::VideoOut {

struct VideoOutPort {
    std::mutex label_mutex;
    std::condition_variable label_changed;

    bool IsVoLabel(const u64* address) const {
        const u64* first = sys_video_out_labels();
        return address >= first && address < first + MaxDisplayBuffers;
    }

    void WaitVoLabel(auto&& ready) {
        std::unique_lock lock{label_mutex};
        label_changed.wait(lock, ready);
    }

    void SignalVoLabel() {
        std::scoped_lock lock{label_mutex};
        label_changed.notify_one();
    }
};

s32 SubmitEopFlip(s32 handle, u32 buffer, s64 argument);
s32 BufferLabelAddress(s32 handle, uintptr_t* address);
void Start();

} // namespace Libraries::VideoOut
