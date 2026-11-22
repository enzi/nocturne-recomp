#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

using SetupProgress = std::function<void(uint64_t done, uint64_t total, const std::string& file)>;

bool setup_installed(const std::filesystem::path& gameDir);
bool setup_extract(const std::vector<std::filesystem::path>& images, const std::filesystem::path& gameDir, const SetupProgress& progress, std::string& error);
