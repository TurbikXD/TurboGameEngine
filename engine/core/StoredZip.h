#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/core/AtomicFile.h"

namespace engine::core {
namespace stored_zip {

inline constexpr auto CrcTable = [] {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t index = 0; index < table.size(); ++index) {
        auto crc = index;
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1U) ^ ((crc & 1U) != 0 ? 0xedb88320U : 0U);
        table[index] = crc;
    }
    return table;
}();

inline std::uint32_t crc32(std::string_view bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const unsigned char byte : bytes) crc = (crc >> 8U) ^ CrcTable[(crc ^ byte) & 0xffU];
    return ~crc;
}

inline void append16(std::string& zip, std::uint16_t value) {
    zip.push_back(static_cast<char>(value & 0xffU));
    zip.push_back(static_cast<char>(value >> 8U));
}

inline void append32(std::string& zip, std::uint32_t value) {
    append16(zip, static_cast<std::uint16_t>(value));
    append16(zip, static_cast<std::uint16_t>(value >> 16U));
}

struct Entry {
    std::u8string name;
    std::uint32_t size;
    std::uint32_t offset;
    std::uint32_t checksum;
};

inline void appendEntryMetadata(std::string& zip, const Entry& entry) {
    append16(zip, 20);
    append16(zip, 1U << 11U);
    append16(zip, 0);
    append16(zip, 0);
    append16(zip, 0x0021);
    append32(zip, entry.checksum);
    append32(zip, entry.size);
    append32(zip, entry.size);
    append16(zip, static_cast<std::uint16_t>(entry.name.size()));
    append16(zip, 0);
}

inline void appendName(std::string& zip, const Entry& entry) {
    zip.append(reinterpret_cast<const char*>(entry.name.data()), entry.name.size());
}

}

[[nodiscard]] inline std::string encodeStoredZip(std::span<const NewFileContents> files) {
    constexpr auto maxEntryCount = std::numeric_limits<std::uint16_t>::max() - 1U;
    constexpr auto maxArchiveSize = std::numeric_limits<std::uint32_t>::max() - 1U;
    constexpr std::uint64_t localHeaderSize = 30;
    constexpr std::uint64_t directoryHeaderSize = 46;
    constexpr std::uint64_t endRecordSize = 22;
    if (files.size() > maxEntryCount) throw std::length_error("Too many files for a ZIP archive.");

    std::set<std::u8string> names;
    std::vector<stored_zip::Entry> entries;
    entries.reserve(files.size());
    std::uint64_t directoryOffset{};
    std::uint64_t directorySize{};
    for (const auto& file : files) {
        auto name = file.path.filename().u8string();
        if (file.path.has_root_path() || file.path != file.path.filename() || name.empty() || name == u8"." || name == u8".." ||
            name.find_first_of(u8"\\/:*?\"<>|") != std::u8string::npos || name.find(u8'\0') != std::u8string::npos) {
            throw std::invalid_argument("A ZIP entry must have a plain filename.");
        }
        if (name.size() > std::numeric_limits<std::uint16_t>::max() || file.contents.size() > maxArchiveSize) {
            throw std::length_error("A file is too large for this ZIP archive.");
        }
        if (!names.insert(name).second) throw std::invalid_argument("Duplicate filename in ZIP archive.");

        const auto offset = static_cast<std::uint32_t>(directoryOffset);
        directoryOffset += localHeaderSize + name.size() + file.contents.size();
        directorySize += directoryHeaderSize + name.size();
        if (directoryOffset + directorySize + endRecordSize > maxArchiveSize) {
            throw std::length_error("The ZIP archive exceeds the size limit.");
        }
        entries.push_back({std::move(name), static_cast<std::uint32_t>(file.contents.size()), offset, stored_zip::crc32(file.contents)});
    }

    std::string zip;
    zip.reserve(static_cast<std::size_t>(directoryOffset + directorySize + endRecordSize));
    for (std::size_t index = 0; index < files.size(); ++index) {
        const auto& entry = entries[index];
        stored_zip::append32(zip, 0x04034b50U);
        stored_zip::appendEntryMetadata(zip, entry);
        stored_zip::appendName(zip, entry);
        zip += files[index].contents;
    }
    for (const auto& entry : entries) {
        stored_zip::append32(zip, 0x02014b50U);
        stored_zip::append16(zip, 20);
        stored_zip::appendEntryMetadata(zip, entry);
        stored_zip::append16(zip, 0);
        stored_zip::append16(zip, 0);
        stored_zip::append16(zip, 0);
        stored_zip::append32(zip, 0);
        stored_zip::append32(zip, entry.offset);
        stored_zip::appendName(zip, entry);
    }
    const auto count = static_cast<std::uint16_t>(entries.size());
    stored_zip::append32(zip, 0x06054b50U);
    stored_zip::append16(zip, 0);
    stored_zip::append16(zip, 0);
    stored_zip::append16(zip, count);
    stored_zip::append16(zip, count);
    stored_zip::append32(zip, static_cast<std::uint32_t>(directorySize));
    stored_zip::append32(zip, static_cast<std::uint32_t>(directoryOffset));
    stored_zip::append16(zip, 0);
    return zip;
}

}
