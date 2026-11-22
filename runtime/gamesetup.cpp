#include "gamesetup.h"

#include "stb/stb_image.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>

namespace fs = std::filesystem;

namespace {

constexpr std::array<uint8_t, 16> kExeMd5 = {0xcb, 0x98, 0xab, 0x8b, 0xd3, 0xf5, 0x44, 0xbc, 0xf1, 0xfb, 0xed, 0xa8, 0xb9, 0x63, 0xbf, 0x8c};

const char* const kDataGroups[] = {"Program Executable Files", "Act One", "Act Two", "Act Three", "Act Four", "Act Five", "Video", "System"};
const char* const kEnglishGroup = "English OS Independent Files";
const char* const kSharedGroup = "Language Independent OS Independent Files";

struct SetupError {
    std::string message;
    bool needsBothDiscs;
};

[[noreturn]] void fail(const std::string& message, bool needsBothDiscs = false) {
    throw SetupError{message, needsBothDiscs};
}

std::string lower(std::string s) {
    for (char& ch : s) {
        ch = char(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

std::array<uint8_t, 16> md5(const std::vector<uint8_t>& data) {
    static constexpr uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static constexpr int S[4][4] = {{7, 12, 17, 22}, {5, 9, 14, 20}, {4, 11, 16, 23}, {6, 10, 15, 21}};
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    auto block = [&h](const uint8_t* p) {
        uint32_t m[16];
        for (int i = 0; i < 16; i++) {
            m[i] = uint32_t(p[4 * i]) | uint32_t(p[4 * i + 1]) << 8 | uint32_t(p[4 * i + 2]) << 16 | uint32_t(p[4 * i + 3]) << 24;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; i++) {
            uint32_t f;
            int g;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
            }
            f += a + K[i] + m[g];
            a = d;
            d = c;
            c = b;
            b += std::rotl(f, S[i / 16][i % 4]);
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
    };
    size_t full = data.size() / 64 * 64;
    for (size_t i = 0; i < full; i += 64) {
        block(data.data() + i);
    }
    uint8_t tail[128] = {};
    size_t rest = data.size() - full;
    std::memcpy(tail, data.data() + full, rest);
    tail[rest] = 0x80;
    size_t tailSize = rest < 56 ? 64 : 128;
    uint64_t bits = uint64_t(data.size()) * 8;
    for (int i = 0; i < 8; i++) {
        tail[tailSize - 8 + i] = uint8_t(bits >> (8 * i));
    }
    block(tail);
    if (tailSize == 128) {
        block(tail + 64);
    }
    std::array<uint8_t, 16> out;
    for (int i = 0; i < 16; i++) {
        out[i] = uint8_t(h[i / 4] >> (8 * (i % 4)));
    }
    return out;
}

struct IsoFile {
    size_t image;
    uint64_t offset;
    uint32_t size;
};

struct Discs {
    std::vector<fs::path> paths;
    std::vector<std::unique_ptr<std::ifstream>> streams;
    std::map<std::string, IsoFile> files;
};

uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

std::vector<uint8_t> readImage(Discs& discs, size_t image, uint64_t offset, uint64_t size) {
    std::vector<uint8_t> out(size);
    std::ifstream& in = *discs.streams[image];
    in.clear();
    in.seekg(std::streamoff(offset));
    if (!in.read(reinterpret_cast<char*>(out.data()), std::streamsize(size))) {
        fail(discs.paths[image].string() + " is truncated");
    }
    return out;
}

void walkIso(Discs& discs, size_t image, uint32_t block, uint64_t offset, uint32_t size, int depth) {
    if (depth > 8) {
        return;
    }
    std::vector<uint8_t> data = readImage(discs, image, offset, size);
    size_t pos = 0;
    while (pos < data.size()) {
        size_t length = data[pos];
        if (length == 0) {
            pos = (pos / block + 1) * block;
            continue;
        }
        if (length < 34 || pos + length > data.size()) {
            break;
        }
        const uint8_t* entry = data.data() + pos;
        pos += length;
        std::string name(reinterpret_cast<const char*>(entry + 33), std::min<size_t>(entry[32], length - 33));
        if (name.size() == 1 && (name[0] == 0 || name[0] == 1)) {
            continue;
        }
        uint64_t extent = uint64_t(le32(entry + 2)) * block;
        if (entry[25] & 2) {
            walkIso(discs, image, block, extent, le32(entry + 10), depth + 1);
            continue;
        }
        name = name.substr(0, name.find(';'));
        while (!name.empty() && name.back() == '.') {
            name.pop_back();
        }
        for (char& ch : name) {
            ch = char(std::toupper(static_cast<unsigned char>(ch)));
        }
        discs.files.emplace(name, IsoFile{image, extent, le32(entry + 10)});
    }
}

void openDiscs(Discs& discs, const std::vector<fs::path>& paths) {
    for (const fs::path& path : paths) {
        auto in = std::make_unique<std::ifstream>(path, std::ios::binary);
        if (!*in) {
            fail("cannot open " + path.string());
        }
        discs.paths.push_back(path);
        discs.streams.push_back(std::move(in));
        size_t image = discs.streams.size() - 1;
        std::vector<uint8_t> pvd(2048);
        std::ifstream& s = *discs.streams[image];
        s.seekg(16 * 2048);
        if (!s.read(reinterpret_cast<char*>(pvd.data()), 2048) || pvd[0] != 1 || std::memcmp(pvd.data() + 1, "CD001", 5) != 0) {
            fail(path.string() + " is not an ISO 9660 disc image");
        }
        uint32_t block = uint32_t(pvd[128]) | uint32_t(pvd[129]) << 8;
        walkIso(discs, image, block, uint64_t(le32(pvd.data() + 156 + 2)) * block, le32(pvd.data() + 156 + 10), 0);
    }
}

std::vector<uint8_t> readDiscFile(Discs& discs, const std::string& name, uint64_t offset, uint64_t size) {
    auto it = discs.files.find(name);
    if (it == discs.files.end()) {
        fail(name + " is not on the selected disc images.", true);
    }
    if (offset + size > it->second.size) {
        fail(name + " on " + discs.paths[it->second.image].string() + " is truncated");
    }
    return readImage(discs, it->second.image, it->second.offset + offset, size);
}

struct CabFile {
    std::string name;
    uint16_t flags;
    uint32_t expanded;
    uint32_t compressed;
    uint32_t dataOffset;
    std::array<uint8_t, 16> md5;
};

struct Cabinet {
    std::string stem;
    std::vector<CabFile> files;
    std::map<std::string, std::pair<uint32_t, uint32_t>> groups;
    std::vector<std::array<uint32_t, 10>> volumes;
};

struct Header {
    const std::vector<uint8_t>& data;
    std::string name;

    uint32_t u32(size_t offset) const {
        if (offset + 4 > data.size()) {
            fail(name + " is damaged");
        }
        return le32(data.data() + offset);
    }

    std::string str(size_t offset) const {
        auto end = offset < data.size() ? std::find(data.begin() + std::ptrdiff_t(offset), data.end(), 0) : data.end();
        if (end == data.end()) {
            fail(name + " is damaged");
        }
        return std::string(data.begin() + std::ptrdiff_t(offset), end);
    }
};

std::string volumeName(const Cabinet& cab, size_t volume) {
    return cab.stem + std::to_string(volume + 1) + ".CAB";
}

Cabinet openCabinet(Discs& discs, const std::string& stem) {
    Cabinet cab;
    cab.stem = stem;
    std::string hdrName = stem + "1.HDR";
    auto it = discs.files.find(hdrName);
    if (it == discs.files.end()) {
        fail(hdrName + " is not on the selected disc images.", true);
    }
    std::vector<uint8_t> data = readDiscFile(discs, hdrName, 0, it->second.size);
    Header hdr{data, hdrName};
    if (hdr.u32(0) != 0x28635349 || ((hdr.u32(4) >> 12) & 0xf) != 5) {
        fail(hdrName + " is not an InstallShield 5 cabinet header");
    }
    size_t base = hdr.u32(12);
    size_t table = base + hdr.u32(base + 0x0c);
    uint32_t dirCount = hdr.u32(base + 0x1c);
    uint32_t fileCount = hdr.u32(base + 0x28);
    for (uint32_t i = 0; i < fileCount; i++) {
        size_t p = table + hdr.u32(table + 4 * (dirCount + i));
        CabFile f;
        f.name = hdr.str(table + hdr.u32(p));
        if (p + 0x3a > data.size()) {
            fail(hdrName + " is damaged");
        }
        f.flags = uint16_t(data[p + 8] | data[p + 9] << 8);
        f.expanded = hdr.u32(p + 0x0a);
        f.compressed = hdr.u32(p + 0x0e);
        f.dataOffset = hdr.u32(p + 0x26);
        std::copy_n(data.begin() + std::ptrdiff_t(p + 0x2a), 16, f.md5.begin());
        cab.files.push_back(f);
    }
    for (int slot = 0; slot < 71; slot++) {
        for (uint32_t list = hdr.u32(base + 0x3e + 4 * slot); list;) {
            size_t group = base + hdr.u32(base + list + 4);
            cab.groups[hdr.str(base + hdr.u32(group))] = {hdr.u32(group + 0x4c), hdr.u32(group + 0x50)};
            list = hdr.u32(base + list + 8);
        }
    }
    while (discs.files.count(volumeName(cab, cab.volumes.size()))) {
        std::vector<uint8_t> raw = readDiscFile(discs, volumeName(cab, cab.volumes.size()), 0x14, 40);
        std::array<uint32_t, 10> v;
        for (int i = 0; i < 10; i++) {
            v[i] = le32(raw.data() + 4 * i);
        }
        cab.volumes.push_back(v);
    }
    if (cab.volumes.empty() || cab.volumes.back()[3] + 1 < fileCount) {
        fail(volumeName(cab, cab.volumes.size()) + " is not on the selected disc images.", true);
    }
    return cab;
}

std::pair<uint32_t, uint32_t> group(const Cabinet& cab, const std::string& name) {
    auto it = cab.groups.find(name);
    if (it == cab.groups.end() || it->second.second >= cab.files.size()) {
        fail(cab.stem + "1.HDR has no file group \"" + name + "\"");
    }
    return it->second;
}

std::vector<uint8_t> extractFile(Discs& discs, const Cabinet& cab, uint32_t index) {
    const CabFile& f = cab.files[index];
    size_t volume = 0;
    while (volume < cab.volumes.size() && !(cab.volumes[volume][2] <= index && index <= cab.volumes[volume][3])) {
        volume++;
    }
    std::vector<uint8_t> data;
    data.reserve(f.compressed);
    uint64_t remaining = f.compressed;
    uint64_t offset = f.dataOffset;
    while (remaining > 0) {
        if (volume >= cab.volumes.size()) {
            fail(f.name + " continues on " + volumeName(cab, volume) + ", which is not on the selected disc images.", true);
        }
        const std::array<uint32_t, 10>& h = cab.volumes[volume];
        uint64_t size = remaining;
        if (index == h[3] && h[9] && h[9] < remaining) {
            offset = h[7];
            size = h[9];
        }
        std::vector<uint8_t> part = readDiscFile(discs, volumeName(cab, volume), offset, size);
        data.insert(data.end(), part.begin(), part.end());
        remaining -= size;
        volume++;
        if (remaining > 0 && volume < cab.volumes.size()) {
            offset = cab.volumes[volume][4];
        }
    }
    if (f.flags & 4) {
        std::vector<uint8_t> out(f.expanded);
        size_t pos = 0;
        size_t written = 0;
        while (pos + 2 <= data.size()) {
            size_t size = size_t(data[pos]) | size_t(data[pos + 1]) << 8;
            if (pos + 2 + size > data.size()) {
                break;
            }
            int n = stbi_zlib_decode_noheader_buffer(reinterpret_cast<char*>(out.data() + written), int(out.size() - written), reinterpret_cast<const char*>(data.data() + pos + 2), int(size));
            if (n < 0) {
                break;
            }
            written += size_t(n);
            pos += 2 + size;
        }
        out.resize(written);
        data = std::move(out);
    }
    if (data.size() != f.expanded || md5(data) != f.md5) {
        fail(f.name + " failed its checksum. The disc image may be damaged.");
    }
    return data;
}

std::string patchSettings(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (true) {
        size_t end = text.find("\r\n", start);
        lines.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) {
            break;
        }
        start = end + 2;
    }
    std::vector<std::string> out;
    for (const std::string& line : lines) {
        if (line.rfind("MaxSwLatency=", 0) == 0) {
            continue;
        }
        out.push_back(line.rfind("useDirect3D=", 0) == 0 ? "useDirect3D=1" : line);
        if (line == "[Sound]") {
            out.push_back("MaxSwLatency=0.1");
        }
    }
    std::string result;
    for (size_t i = 0; i < out.size(); i++) {
        result += (i ? "\r\n" : "") + out[i];
    }
    return result;
}

bool hasFile(const fs::path& folder, const std::string& name) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(folder, ec)) {
        if (lower(e.path().filename().string()) == lower(name)) {
            return true;
        }
    }
    return false;
}

void writeFile(const fs::path& path, const std::vector<uint8_t>& data) {
    std::ofstream out(path, std::ios::binary);
    if (!out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()))) {
        fail("cannot write " + path.string());
    }
}

struct Job {
    const Cabinet* cab;
    uint32_t index;
    fs::path folder;
};

}

bool setup_installed(const fs::path& gameDir) {
    return hasFile(gameDir, "nocturne.exe");
}

bool setup_extract(const std::vector<fs::path>& images, const fs::path& gameDir, const SetupProgress& progress, std::string& error) {
    try {
        Discs discs;
        openDiscs(discs, images);
        Cabinet data = openCabinet(discs, "DATA");
        Cabinet user = openCabinet(discs, "_USER");
        std::vector<Job> jobs;
        for (const char* name : kDataGroups) {
            auto [first, last] = group(data, name);
            for (uint32_t i = first; i <= last; i++) {
                std::string file = lower(data.files[i].name);
                if (file == "nocturne.exe") {
                    if (data.files[i].md5 != kExeMd5) {
                        fail("The discs contain a version of nocturne.exe that this recompilation does not support.");
                    }
                    jobs.push_back({&data, i, gameDir});
                } else if (file.ends_with(".pod")) {
                    jobs.push_back({&data, i, gameDir});
                } else if (file == "opening.avi") {
                    jobs.push_back({&data, i, gameDir / "video"});
                } else if (file == "render.ini") {
                    jobs.push_back({&data, i, gameDir / "system"});
                }
            }
        }
        for (bool shared : {false, true}) {
            auto [first, last] = group(user, shared ? kSharedGroup : kEnglishGroup);
            for (uint32_t i = first; i <= last; i++) {
                std::string file = lower(user.files[i].name);
                if (file == "nocturne.ini") {
                    jobs.push_back({&user, i, gameDir / "system"});
                } else if (shared || file.ends_with(".pod")) {
                    jobs.push_back({&user, i, gameDir});
                }
            }
        }
        auto exeJob = std::find_if(jobs.begin(), jobs.end(), [](const Job& j) { return lower(j.cab->files[j.index].name) == "nocturne.exe"; });
        if (exeJob == jobs.end()) {
            fail("nocturne.exe is not on the selected disc images.", true);
        }
        std::rotate(exeJob, exeJob + 1, jobs.end());
        std::error_code ec;
        fs::create_directories(gameDir / "system", ec);
        fs::create_directories(gameDir / "video", ec);
        uint64_t total = 0;
        for (const Job& j : jobs) {
            total += j.cab->files[j.index].expanded;
        }
        uint64_t done = 0;
        for (const Job& j : jobs) {
            const CabFile& f = j.cab->files[j.index];
            if (progress) {
                progress(done, total, f.name);
            }
            if (j.folder.filename() != "system" || !hasFile(j.folder, f.name)) {
                std::vector<uint8_t> content = extractFile(discs, *j.cab, j.index);
                if (lower(f.name) == "nocturne.ini") {
                    std::string text = patchSettings(std::string(content.begin(), content.end()));
                    content.assign(text.begin(), text.end());
                }
                writeFile(j.folder / f.name, content);
            }
            done += f.expanded;
        }
        if (progress) {
            progress(total, total, "");
        }
        return true;
    } catch (const SetupError& e) {
        error = e.message;
        if (e.needsBothDiscs) {
            error += " Select the images of both Nocturne discs.";
        }
        return false;
    }
}
