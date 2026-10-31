// SPDX-License-Identifier: GPL-3.0-or-later
#include "start/toml_patch.h"
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace start;

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}
bool Refused(std::string_view text) {
    try {
        TomlPatchAsXml(text, "x");
    } catch (const Error&) {
        return true;
    }
    return false;
}
}

int main(int argc, char** argv) {
    const char* good = "# a patch\n[patch]\nname = \"one\"\ndescription = \"d\"\norder = 7\ngroup = \"g\"\n\n"
                       "[site.a]\naddress = \"0x00401000\"\nexpect = \"e8 5b\"\nbytes = \"E8 FB\"\n\n[site.b]\naddress = \"0x00402000\"\nbytes = \"90\"\n";
    const auto root = TomlPatchAsXml(good, "fallback");
    const auto metas = root->Iter("Metadata");
    Check(metas.size() == 1, "one patch");
    Check(metas[0]->Get("Name", "") == "one" && metas[0]->Get("Note", "") == "d", "name and description");
    Check(metas[0]->Get("Order", "") == "7" && metas[0]->Get("Group", "") == "g", "order and group");
    Check(metas[0]->Get("isEnabled", "") == "false", "off until chosen");
    const auto lines = root->Iter("Line");
    Check(lines.size() == 2, "two sites");
    Check(lines[0]->Get("Address", "") == "0x00401000" && lines[0]->Get("Value", "") == "E8FB" && lines[0]->Get("Expect", "") == "e85b", "first site");
    Check(!lines[1]->Get("Expect").has_value(), "expect is optional");
    Check(TomlPatchAsXml("[site.a]\naddress = \"0x1\"\nbytes = \"90\"", "stem")->Iter("Metadata")[0]->Get("Name", "") == "stem", "the file's name without a name");
    Check(TomlPatchAsXml("[site.a]\r\naddress = \"0x1\"\r\nbytes = \"90\"\r\n", "x")->Iter("Line").size() == 1, "Windows line ends");
    Check(Refused(""), "no sites");
    Check(Refused("[patch]\nname = \"x\"\n"), "a patch with no sites");
    Check(Refused("[site.a]\naddress = \"0x1\"\n"), "a site without bytes");
    Check(Refused("[site.a]\nbytes = \"90\"\n"), "a site without an address");
    Check(Refused("[site.a]\naddress = \"0x1\"\nbytes = \"9\"\n"), "half a byte");
    Check(Refused("[site.a]\naddress = \"0x1\"\nbytes = \"zz\"\n"), "not hexadecimal");
    Check(Refused("[site.a]\naddress = \"0x1\"\nexpect = \"90 90\"\nbytes = \"90\"\n"), "expect of another length");
    Check(Refused("[other]\nx = \"y\"\n"), "an unknown section");
    Check(Refused("name = \"x\"\n"), "a value outside any section");
    Check(Refused("[patch]\nname = \"x\n"), "an unclosed string");
    Check(Refused(std::string(17 * 1024 * 1024, ' ')), "too large");
    for (int i = 1; i < argc; ++i) {
        std::ifstream in(argv[i], std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        const auto file = TomlPatchAsXml(text.str(), "file");
        std::printf("%s: %s, order %s, group %s, %zu sites\n", argv[i], file->Iter("Metadata")[0]->Get("Name", "").c_str(),
                    file->Iter("Metadata")[0]->Get("Order", "").c_str(), file->Iter("Metadata")[0]->Get("Group", "").c_str(), file->Iter("Line").size());
    }
    if (failures) return 1;
    std::printf("toml-patch-test: ok\n");
    return 0;
}
