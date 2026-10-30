// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/bbnet/include/ip_mask.h"
#include <cstdio>
#include <string>

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}
std::uint32_t Net(unsigned a, unsigned b, unsigned c, unsigned d) {
    std::uint32_t v;
    const unsigned char bytes[4] = {static_cast<unsigned char>(a), static_cast<unsigned char>(b),
                                    static_cast<unsigned char>(c), static_cast<unsigned char>(d)};
    __builtin_memcpy(&v, bytes, 4);
    return v;
}
bool Starts(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }
}

int main() {
    using namespace BbNet;
    const std::string pub = MaskIpv4(Net(203, 0, 113, 77));
    Check(Starts(pub, "203.x.x.x#") && pub.size() == 16, "a public address keeps its first octet and a tag");
    Check(pub.find("113") == std::string::npos && pub.find("77") == std::string::npos, "no other octet shows");
    Check(MaskIpv4(Net(203, 0, 113, 77)) == pub, "the same address gets the same tag all session");
    Check(MaskIpv4(Net(203, 0, 113, 78)) != pub, "a neighbour gets another tag");
    Check(MaskIpText("203.0.113.77") == pub, "text and binary forms agree");
    Check(MaskIpText("203.0.113.77:3658") == pub + ":3658", "a port after the address stays");
    Check(MaskIpv4Host(0xCB00714Du) == pub, "host order agrees");

    Check(Starts(MaskIpv4(Net(192, 168, 1, 20)), "192.168.x.x#"), "a home LAN shows as one");
    Check(Starts(MaskIpv4(Net(172, 20, 3, 4)), "172.20.x.x#"), "a private 172 range shows as one");
    Check(Starts(MaskIpv4(Net(100, 72, 3, 4)), "100.72.x.x#"), "carrier NAT shows as one");
    Check(Starts(MaskIpv4(Net(10, 1, 2, 3)), "10.x.x.x#"), "10/8 keeps the first octet only");
    Check(Starts(MaskIpv4(Net(172, 40, 3, 4)), "172.x.x.x#"), "a public 172 address hides the second octet");
    Check(MaskIpv4(Net(127, 0, 0, 1)) == "127.0.0.1", "loopback is not a player's address");
    Check(MaskIpv4(0) == "0.0.0.0", "no address stays readable");

    const std::string v6 = MaskIpText("2001:db8::1");
    Check(Starts(v6, "v6#") && v6.find("db8") == std::string::npos, "IPv6 is a tag only");
    Check(MaskIpText("thehuntersdream.com") == "thehuntersdream.com", "host names stay");
    Check(MaskIpText("").empty(), "empty stays empty");
    Check(MaskIpText(":0") == ":0", "no mapped address yet stays readable");
    Check(MaskIpText("relay.example:3478") == "relay.example:3478", "a host name with a port stays");
    Check(MaskIpText("1.2.3") == "1.2.3", "not an address stays");

    if (!failures) std::printf("ip mask: all checks passed\n");
    return failures ? 1 : 0;
}
