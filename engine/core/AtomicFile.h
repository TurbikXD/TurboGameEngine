#pragma once

#include <filesystem>
#include <span>
#include <string>

namespace engine::core {

bool writeFileAtomically(const std::filesystem::path& target, const std::string& contents,
                         std::string& error);

struct NewFileContents {
    std::filesystem::path path;
    std::string contents;
};

bool writeNewFiles(std::span<const NewFileContents> files, std::string& error);

bool writeNewFilePair(const std::filesystem::path& first, const std::string& firstContents,
                      const std::filesystem::path& second, const std::string& secondContents,
                      std::string& error);

}
