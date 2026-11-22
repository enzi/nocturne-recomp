#include "pe.h"

#include <cstring>
#include <fstream>
#include <iterator>

namespace {

template <typename T>
T get(const std::vector<uint8_t>& f, size_t off) {
    T v;
    std::memcpy(&v, f.data() + off, sizeof(T));
    return v;
}

}

const uint8_t* PeImage::ptr(uint32_t va, uint32_t len) const {
    for (const PeSection& s : sections) {
        if (s.rawPtr == 0 || va < s.va || va - s.va + uint64_t(len) > s.rawSize) {
            continue;
        }
        if (s.rawPtr + uint64_t(va - s.va) + len > file.size()) {
            return nullptr;
        }
        return file.data() + s.rawPtr + (va - s.va);
    }
    return nullptr;
}

const PeSection* PeImage::section(const std::string& name) const {
    for (const PeSection& s : sections) {
        if (s.name == name) {
            return &s;
        }
    }
    return nullptr;
}

bool PeImage::load(const std::string& path, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "cannot open " + path;
        return false;
    }
    file.assign(std::istreambuf_iterator<char>(in), {});
    if (file.size() < 0x40 || get<uint16_t>(file, 0) != 0x5a4d) {
        err = "not an MZ executable";
        return false;
    }
    uint32_t pe = get<uint32_t>(file, 0x3c);
    if (pe + 0xf8 > file.size() || get<uint32_t>(file, pe) != 0x4550 || get<uint16_t>(file, pe + 4) != 0x14c) {
        err = "not a PE32 i386 executable";
        return false;
    }
    uint16_t nsec = get<uint16_t>(file, pe + 6);
    uint16_t optSize = get<uint16_t>(file, pe + 20);
    uint32_t opt = pe + 24;
    entry = get<uint32_t>(file, opt + 16);
    imageBase = get<uint32_t>(file, opt + 28);
    imageSize = get<uint32_t>(file, opt + 56);
    headerSize = get<uint32_t>(file, opt + 60);
    entry += imageBase;
    uint32_t importRva = get<uint32_t>(file, opt + 96 + 1 * 8);
    uint32_t relocRva = get<uint32_t>(file, opt + 96 + 5 * 8);
    uint32_t relocSize = get<uint32_t>(file, opt + 96 + 5 * 8 + 4);

    uint32_t sh = opt + optSize;
    for (uint16_t i = 0; i < nsec; i++) {
        uint32_t o = sh + 40 * i;
        if (o + 40 > file.size()) {
            err = "truncated section table";
            return false;
        }
        PeSection s;
        char name[9] = {};
        std::memcpy(name, file.data() + o, 8);
        s.name = name;
        uint32_t vsize = get<uint32_t>(file, o + 8);
        s.va = imageBase + get<uint32_t>(file, o + 12);
        s.rawSize = get<uint32_t>(file, o + 16);
        s.rawPtr = get<uint32_t>(file, o + 20);
        s.characteristics = get<uint32_t>(file, o + 36);
        s.size = vsize ? vsize : s.rawSize;
        if (s.rawPtr == 0) {
            s.rawSize = 0;
        }
        sections.push_back(s);
    }

    if (importRva) {
        for (uint32_t d = imageBase + importRva;; d += 20) {
            const uint8_t* desc = ptr(d, 20);
            if (!desc) {
                err = "bad import descriptor";
                return false;
            }
            uint32_t ilt, nameRva, iat;
            std::memcpy(&ilt, desc, 4);
            std::memcpy(&nameRva, desc + 12, 4);
            std::memcpy(&iat, desc + 16, 4);
            if (!nameRva && !iat) {
                break;
            }
            const char* dll = reinterpret_cast<const char*>(ptr(imageBase + nameRva, 1));
            uint32_t names = ilt ? ilt : iat;
            for (uint32_t k = 0;; k++) {
                const uint8_t* t = ptr(imageBase + names + 4 * k, 4);
                if (!t) {
                    err = "bad import thunk";
                    return false;
                }
                uint32_t v;
                std::memcpy(&v, t, 4);
                if (!v) {
                    break;
                }
                PeImport imp;
                imp.dll = dll ? dll : "";
                imp.iatSlot = imageBase + iat + 4 * k;
                imp.ordinal = 0;
                if (v & 0x80000000u) {
                    imp.ordinal = uint16_t(v);
                } else {
                    const char* n = reinterpret_cast<const char*>(ptr(imageBase + v + 2, 1));
                    imp.name = n ? n : "";
                }
                imports.push_back(imp);
            }
        }
    }

    if (relocRva) {
        uint32_t off = 0;
        while (off + 8 <= relocSize) {
            const uint8_t* b = ptr(imageBase + relocRva + off, 8);
            if (!b) {
                err = "bad relocation block";
                return false;
            }
            uint32_t page, blockSize;
            std::memcpy(&page, b, 4);
            std::memcpy(&blockSize, b + 4, 4);
            if (blockSize < 8) {
                break;
            }
            const uint8_t* e = ptr(imageBase + relocRva + off + 8, blockSize - 8);
            for (uint32_t i = 0; e && i < (blockSize - 8) / 2; i++) {
                uint16_t v;
                std::memcpy(&v, e + 2 * i, 2);
                if ((v >> 12) == 3) {
                    relocs.push_back(imageBase + page + (v & 0xfff));
                }
            }
            off += blockSize;
        }
    }
    return true;
}
