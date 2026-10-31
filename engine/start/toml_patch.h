// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "xml.h"

namespace start {

std::unique_ptr<XmlElement> TomlPatchAsXml(std::string_view text, const std::string& fallback_name);

}
