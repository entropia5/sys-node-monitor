#pragma once
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// Uses AF_INET ioctls instead of getifaddrs/hostname -I (which require AF_NETLINK).
inline std::string localIPv4Addresses(bool firstOnly = false) {
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return "N/A";
    std::ifstream interfaces("/proc/net/dev");
    std::vector<std::string> addresses;
    std::string line;
    while (std::getline(interfaces, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        auto name = line.substr(0, colon);
        const auto start = name.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        name = name.substr(start);
        if (name.size() >= IFNAMSIZ) continue;
        ifreq request{};
        std::memcpy(request.ifr_name, name.c_str(), name.size() + 1);
        if (ioctl(fd, SIOCGIFFLAGS, &request) < 0 ||
            !(request.ifr_flags & IFF_UP) || (request.ifr_flags & IFF_LOOPBACK)) continue;
        if (ioctl(fd, SIOCGIFADDR, &request) < 0) continue;
        char text[INET_ADDRSTRLEN]{};
        const auto* address = reinterpret_cast<const sockaddr_in*>(&request.ifr_addr);
        if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)) &&
            std::find(addresses.begin(), addresses.end(), text) == addresses.end())
            addresses.emplace_back(text);
    }
    close(fd);
    std::string result;
    for (const auto& address : addresses) {
        if (!result.empty()) result += " ";
        result += address;
        if (firstOnly) break;
    }
    return result.empty() ? "N/A" : result;
}
