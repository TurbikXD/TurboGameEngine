#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace engine::net {

inline constexpr std::size_t MaxPeerNameBytes = 128;

inline std::optional<std::string_view> normalizePeerName(std::string_view name, bool truncate = false) noexcept {
    const auto start = name.find_first_not_of(' ');
    if (start == std::string_view::npos) return std::string_view{};
    name = name.substr(start, name.find_last_not_of(' ') - start + 1);
    if (!truncate && name.size() > MaxPeerNameBytes) return {};

    std::size_t end = 0;
    for (std::size_t offset = 0; offset < name.size();) {
        const auto first = static_cast<unsigned char>(name[offset]);
        const std::size_t width = first <= 0x7f ? 1 : first >= 0xc2 && first <= 0xdf ? 2 :
            first >= 0xe0 && first <= 0xef ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
        if (!width || name.size() - offset < width) return {};
        uint32_t point = first & (0xff >> (width == 1 ? 1 : width + 1));
        for (std::size_t next = 1; next < width; ++next) {
            const auto continuation = static_cast<unsigned char>(name[offset + next]);
            if ((continuation & 0xc0) != 0x80) return {};
            point = (point << 6) | (continuation & 0x3f);
        }
        if ((width == 3 && point < 0x800) || (width == 4 && point < 0x10000) || point > 0x10ffff ||
            (point >= 0xd800 && point <= 0xdfff) || point < 0x20 || (point >= 0x7f && point <= 0x9f) ||
            point == 0x061c || (point >= 0x200e && point <= 0x200f) || (point >= 0x2028 && point <= 0x202e) ||
            (point >= 0x2066 && point <= 0x2069) || point == 0xfeff) {
            return {};
        }
        offset += width;
        if (offset <= MaxPeerNameBytes) end = offset;
    }
    name = name.substr(0, end);
    return name.substr(0, name.find_last_not_of(' ') + 1);
}

}
