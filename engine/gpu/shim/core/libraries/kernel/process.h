// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common/types.h"

namespace Libraries::Kernel {

s32 PS4_SYSV_ABI sceKernelIsNeoMode();
s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32* version);

} // namespace Libraries::Kernel
