#pragma once

#include <filesystem>
#include <string>

namespace engine::core {

// Write through an exclusively created sibling, then replace the target entry.
// The parent directory must already exist. Failed writes preserve the target.
bool writeFileAtomically(const std::filesystem::path& target, const std::string& contents,
                         std::string& error);

} // namespace engine::core
