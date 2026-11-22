#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct PeSection {
    std::string name;
    uint32_t va;
    uint32_t size;
    uint32_t rawSize;
    uint32_t rawPtr;
    uint32_t characteristics;
};

struct PeImport {
    std::string dll;
    std::string name;
    uint16_t ordinal;
    uint32_t iatSlot;
};

struct PeImage {
    std::vector<uint8_t> file;
    uint32_t imageBase = 0;
    uint32_t imageSize = 0;
    uint32_t headerSize = 0;
    uint32_t entry = 0;
    std::vector<PeSection> sections;
    std::vector<PeImport> imports;
    std::vector<uint32_t> relocs;

    bool load(const std::string& path, std::string& err);
    const uint8_t* ptr(uint32_t va, uint32_t len) const;
    const PeSection* section(const std::string& name) const;
};
