#ifndef JOKER_CONFIG_H
#define JOKER_CONFIG_H

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "qtng/io_utils.h"
#include "qtng/udp.h"
#include "qtng/utils/string_utils.h"

class IniConfig
{
public:
    static bool load(const std::string &path, IniConfig *config, std::string *errorMessage)
    {
        qtng::PosixPath filePath(path);
        if (!filePath.isReadable()) {
            if (errorMessage) {
                *errorMessage = "configure file `" + path + "` is not readable.";
            }
            return false;
        }

        std::ifstream in(path.c_str());
        if (!in) {
            if (errorMessage) {
                *errorMessage = "configure file `" + path + "` is not readable.";
            }
            return false;
        }

        std::string currentSection;
        std::string line;
        while (std::getline(in, line)) {
            line = qtng::utils::trimmed(line);
            if (line.empty() || line[0] == '#' || line[0] == ';') {
                continue;
            }
            if (line[0] == '[') {
                const std::size_t end = line.find(']');
                if (end == std::string::npos) {
                    if (errorMessage) {
                        *errorMessage = "invalid section in configure file `" + path + "`.";
                    }
                    return false;
                }
                currentSection = qtng::utils::trimmed(line.substr(1, end - 1));
                continue;
            }

            const std::size_t equalPos = line.find('=');
            if (equalPos == std::string::npos) {
                continue;
            }
            std::string key = qtng::utils::trimmed(line.substr(0, equalPos));
            std::string value = qtng::utils::trimmed(line.substr(equalPos + 1));
            if (!key.empty()) {
                config->sections[currentSection][key] = value;
            }
        }
        return true;
    }

    std::string value(const std::string &section, const std::string &key,
                      const std::string &defaultValue = std::string()) const
    {
        const auto sectionIt = sections.find(section);
        if (sectionIt == sections.end()) {
            return defaultValue;
        }
        const auto valueIt = sectionIt->second.find(key);
        if (valueIt == sectionIt->second.end()) {
            return defaultValue;
        }
        return valueIt->second;
    }

    std::vector<std::string> childGroups(const std::string &prefix) const
    {
        std::vector<std::string> groups;
        const std::string marker = prefix + "/";
        for (const auto &entry : sections) {
            const std::string &section = entry.first;
            if (section.size() <= marker.size()) {
                continue;
            }
            if (section.compare(0, marker.size(), marker) != 0) {
                continue;
            }
            const std::string child = section.substr(marker.size());
            if (child.find('/') != std::string::npos) {
                continue;
            }
            groups.push_back(child);
        }
        std::sort(groups.begin(), groups.end());
        return groups;
    }

private:
    std::map<std::string, std::map<std::string, std::string>> sections;
};

inline bool parseUInt16(const std::string &text, std::uint16_t *value)
{
    if (text.empty()) {
        return false;
    }
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || parsed > 65535UL) {
        return false;
    }
    *value = static_cast<std::uint16_t>(parsed);
    return true;
}

// Bytes as integer, optionally with K/M/G suffix (1024-based), e.g. 32768, 32K, 32M.
// Result must fit in int (Socket::setOption contract).
inline bool parseByteSize(const std::string &text, int *value, std::string *errorMessage = nullptr)
{
    if (text.empty()) {
        if (errorMessage) {
            *errorMessage = "byte size is empty.";
        }
        return false;
    }
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (end == text.c_str()) {
        if (errorMessage) {
            *errorMessage = "byte size `" + text + "` is invalid.";
        }
        return false;
    }
    unsigned long long multiplier = 1;
    const std::string suffix = qtng::utils::toUpper(qtng::utils::trimmed(std::string(end)));
    if (suffix == "K" || suffix == "KB") {
        multiplier = 1024ULL;
    } else if (suffix == "M" || suffix == "MB") {
        multiplier = 1024ULL * 1024ULL;
    } else if (suffix == "G" || suffix == "GB") {
        multiplier = 1024ULL * 1024ULL * 1024ULL;
    } else if (!suffix.empty()) {
        if (errorMessage) {
            *errorMessage = "byte size `" + text + "` has unknown suffix.";
        }
        return false;
    }
    if (parsed > (static_cast<unsigned long long>(INT_MAX) / multiplier)) {
        if (errorMessage) {
            *errorMessage = "byte size `" + text + "` is too large.";
        }
        return false;
    }
    *value = static_cast<int>(parsed * multiplier);
    return true;
}

inline bool isDomainName(const std::string &str)
{
    static const std::string valid = "abcdefghijklmnopqrstuvwxyz-ABCDEFGHIJKLMNOPQRSTUVWXYZ.1234567890";
    if (str.empty() || str[0] == '-' || str[0] == '.') {
        return false;
    }
    for (char ch : str) {
        if (valid.find(ch) == std::string::npos) {
            return false;
        }
    }
    return true;
}

inline bool isHostAddress(const std::string &str)
{
    qtng::HostAddress addr(str);
    if (!addr.isNull()) {
        return true;
    }
    return isDomainName(str);
}

inline bool parseKcpMode(const std::string &modeStr, qtng::KcpSocket::Mode *mode, std::string *errorMessage)
{
    const std::string normalized = qtng::utils::toLower(modeStr);
    if (normalized.empty() || normalized == "normal") {
        *mode = qtng::KcpSocket::Internet;
        return true;
    }
    if (normalized == "slow") {
        *mode = qtng::KcpSocket::LargeDelayInternet;
        return true;
    }
    if (normalized == "fast") {
        *mode = qtng::KcpSocket::FastInternet;
        return true;
    }
    if (normalized == "fast2") {
        *mode = qtng::KcpSocket::Ethernet;
        return true;
    }
    if (normalized == "fast3") {
        *mode = qtng::KcpSocket::Loopback;
        return true;
    }
    if (normalized == "asymmetric") {
        *mode = qtng::KcpSocket::AsymmetricInternet;
        return true;
    }
    if (errorMessage) {
        *errorMessage = "kcp mode `" + modeStr + "` is unknown.";
    }
    return false;
}

#endif
