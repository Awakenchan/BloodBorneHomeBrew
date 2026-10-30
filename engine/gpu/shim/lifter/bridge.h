// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <span>
#include "lifter/lift.h"

namespace Shader {
struct Info;
struct RuntimeInfo;
struct Profile;
namespace Backend {
struct Bindings;
}
} // namespace Shader

namespace Lifter {

enum class Mode : u8 { Off, On, Compare };

Mode GetMode();

Input MakeInput(const Shader::Info& info, const Shader::RuntimeInfo& runtime_before,
                const Shader::Profile& profile, const Shader::Backend::Bindings& start,
                std::span<const u32> code);

} // namespace Lifter
