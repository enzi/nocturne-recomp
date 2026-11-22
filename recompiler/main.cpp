#include "recomp.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace {

constexpr uint32_t kAllFlags = 0x8d5;

std::string hex8(uint32_t v) {
    char b[12];
    std::snprintf(b, sizeof b, "%08x", v);
    return b;
}

bool loadFunctionList(Program& p, const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream s(line);
        std::string kind, addr;
        s >> kind >> addr;
        uint32_t a = static_cast<uint32_t>(std::stoul(addr, nullptr, 16));
        if (kind == "func") {
            if (p.inCode(a)) {
                p.entries.insert(a);
            }
        } else if (kind == "jump") {
            std::vector<uint32_t> targets;
            std::string t;
            while (s >> t) {
                uint32_t v = static_cast<uint32_t>(std::stoul(t, nullptr, 16));
                if (p.inCode(v)) {
                    targets.push_back(v);
                }
            }
            p.jumpTables[a] = targets;
        }
    }
    return true;
}

bool decode(const Program& p, Insn& in) {
    const uint8_t* bytes = p.pe.ptr(in.addr, 1);
    if (!bytes) {
        return false;
    }
    uint32_t avail = std::min<uint32_t>(p.codeHi - in.addr, ZYDIS_MAX_INSTRUCTION_LENGTH);
    return ZYAN_SUCCESS(ZydisDecoderDecodeFull(&p.decoder, bytes, avail, &in.d, in.op));
}

std::vector<uint32_t> recoverTable(const Program& p, const Insn& in) {
    std::vector<uint32_t> targets;
    const ZydisDecodedOperand& o = in.op[0];
    if (o.type != ZYDIS_OPERAND_TYPE_MEMORY || o.mem.index == ZYDIS_REGISTER_NONE && o.mem.base == ZYDIS_REGISTER_NONE) {
        return targets;
    }
    for (uint32_t a = static_cast<uint32_t>(o.mem.disp.value); p.relocSites.count(a) && targets.size() < 1024; a += 4) {
        const uint8_t* v = p.pe.ptr(a, 4);
        uint32_t target;
        std::memcpy(&target, v, 4);
        if (!p.inCode(target)) {
            break;
        }
        targets.push_back(target);
    }
    return targets;
}

Function build(Program& p, uint32_t entry, std::set<uint32_t>& calls) {
    Function f;
    f.entry = entry;
    auto local = [&](uint32_t t) { return p.inCode(t) && (t == entry || !p.isEntry(t)); };
    std::vector<uint32_t> work{entry};
    while (!work.empty()) {
        uint32_t a = work.back();
        work.pop_back();
        for (;;) {
            if (f.insns.count(a) || (a != entry && p.isEntry(a)) || !p.inCode(a)) {
                break;
            }
            Insn& in = f.insns[a];
            in.addr = a;
            in.ok = decode(p, in);
            if (!in.ok) {
                break;
            }
            Flow fl = classify(p, in);
            if (fl == Flow::JumpIndirect) {
                std::vector<uint32_t> table = recoverTable(p, in);
                if (!table.empty()) {
                    p.jumpTables[a] = table;
                    p.recoveredTables.insert(a);
                    fl = Flow::JumpTable;
                }
            }
            uint32_t t;
            if ((fl == Flow::Branch || fl == Flow::Jump) && branchTarget(in, t) && local(t)) {
                work.push_back(t);
            }
            if (fl == Flow::JumpTable) {
                for (uint32_t x : p.jumpTables.at(a)) {
                    if (local(x)) {
                        work.push_back(x);
                    }
                }
            }
            if (fl == Flow::Call && branchTarget(in, t) && p.inCode(t)) {
                calls.insert(t);
            }
            if (fl == Flow::Jump || fl == Flow::JumpTable || fl == Flow::JumpIndirect || fl == Flow::Return || fl == Flow::Stop) {
                break;
            }
            a += in.d.length;
        }
    }
    return f;
}

bool conditionalFlagWriter(const Insn& in) {
    switch (in.d.mnemonic) {
    case ZYDIS_MNEMONIC_SHL:
    case ZYDIS_MNEMONIC_SHR:
    case ZYDIS_MNEMONIC_SAR:
    case ZYDIS_MNEMONIC_ROL:
    case ZYDIS_MNEMONIC_ROR:
    case ZYDIS_MNEMONIC_RCL:
    case ZYDIS_MNEMONIC_RCR:
    case ZYDIS_MNEMONIC_SHLD:
    case ZYDIS_MNEMONIC_SHRD: {
        const ZydisDecodedOperand& n = in.op[in.d.mnemonic == ZYDIS_MNEMONIC_SHLD || in.d.mnemonic == ZYDIS_MNEMONIC_SHRD ? 2 : 1];
        return n.type != ZYDIS_OPERAND_TYPE_IMMEDIATE || (n.imm.value.u & 31) == 0;
    }
    case ZYDIS_MNEMONIC_SCASB:
    case ZYDIS_MNEMONIC_SCASW:
    case ZYDIS_MNEMONIC_SCASD:
    case ZYDIS_MNEMONIC_CMPSB:
    case ZYDIS_MNEMONIC_CMPSW:
    case ZYDIS_MNEMONIC_CMPSD:
        return in.d.attributes & (ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE | ZYDIS_ATTRIB_HAS_REPNE);
    default:
        return false;
    }
}

void liveness(const Program& p, Function& f) {
    struct Node {
        Insn* in;
        std::vector<Insn*> succ;
        bool exit;
    };
    std::vector<Node> nodes;
    nodes.reserve(f.insns.size());
    auto localInsn = [&](uint32_t t) -> Insn* {
        auto it = f.insns.find(t);
        if (it == f.insns.end() || (t != f.entry && p.isEntry(t))) {
            return nullptr;
        }
        return &it->second;
    };
    for (auto& [addr, in] : f.insns) {
        Node n{&in, {}, false};
        if (!in.ok) {
            n.exit = true;
            nodes.push_back(n);
            continue;
        }
        const ZydisAccessedFlags* fl = in.d.cpu_flags;
        uint32_t written = fl ? (fl->modified | fl->set_0 | fl->set_1 | fl->undefined) & kAllFlags : 0;
        in.use = fl ? fl->tested & kAllFlags : 0;
        in.writes = written;
        in.def = conditionalFlagWriter(in) ? 0 : written;
        Flow flow = classify(p, in);
        if (flow == Flow::Call || flow == Flow::CallIndirect) {
            in.use = kAllFlags;
            in.def = 0;
        }
        auto addSucc = [&](uint32_t t) {
            if (Insn* s = localInsn(t)) {
                n.succ.push_back(s);
            } else {
                n.exit = true;
            }
        };
        uint32_t next = addr + in.d.length;
        uint32_t t;
        switch (flow) {
        case Flow::Next:
        case Flow::Call:
        case Flow::CallIndirect:
            addSucc(next);
            break;
        case Flow::Branch:
            addSucc(next);
            if (branchTarget(in, t)) {
                addSucc(t);
            } else {
                n.exit = true;
            }
            break;
        case Flow::Jump:
            if (branchTarget(in, t)) {
                addSucc(t);
            } else {
                n.exit = true;
            }
            break;
        case Flow::JumpTable:
            for (uint32_t x : p.jumpTables.at(addr)) {
                addSucc(x);
            }
            n.exit = true;
            break;
        default:
            n.exit = true;
            break;
        }
        nodes.push_back(n);
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
            uint32_t out = it->exit ? kAllFlags : 0;
            for (Insn* s : it->succ) {
                out |= s->liveIn;
            }
            uint32_t in = it->in->use | (out & ~it->in->def);
            if (out != it->in->liveOut || in != it->in->liveIn) {
                it->in->liveOut = out;
                it->in->liveIn = in;
                changed = true;
            }
        }
    }
}

bool writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return false;
    }
    out << text;
    return bool(out);
}

}

int main(int argc, char** argv) {
    if (argc != 6) {
        std::fprintf(stderr, "usage: %s <nocturne.exe> <function list> <replaced functions> <output dir> <chunk count>\n", argv[0]);
        return 2;
    }
    Program p;
    std::string err;
    if (!p.pe.load(argv[1], err)) {
        std::fprintf(stderr, "%s: %s\n", argv[1], err.c_str());
        return 1;
    }
    for (const PeSection& s : p.pe.sections) {
        if (s.characteristics & 0x20) {
            p.codeLo = s.va;
            p.codeHi = s.va + s.rawSize;
            break;
        }
    }
    if (!p.codeHi) {
        std::fprintf(stderr, "no code section\n");
        return 1;
    }
    if (!loadFunctionList(p, argv[2])) {
        return 1;
    }
    size_t listed = p.entries.size();
    p.entries.insert(p.pe.entry);
    p.relocSites.insert(p.pe.relocs.begin(), p.pe.relocs.end());
    size_t fromData = 0;
    for (uint32_t site : p.pe.relocs) {
        const uint8_t* v = p.pe.ptr(site, 4);
        if (!v || p.inCode(site)) {
            continue;
        }
        uint32_t target;
        std::memcpy(&target, v, 4);
        if (p.inCode(target) && p.entries.insert(target).second) {
            fromData++;
        }
    }
    std::printf("function entries: %zu from the function list, %zu more from code pointers in data\n", listed, fromData);
    ZydisDecoderInit(&p.decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
    ZydisFormatterInit(&p.formatter, ZYDIS_FORMATTER_STYLE_INTEL);

    std::map<uint32_t, Function> funcs;
    for (int round = 1;; round++) {
        std::set<uint32_t> calls;
        funcs.clear();
        for (uint32_t e : p.entries) {
            funcs.emplace(e, build(p, e, calls));
        }
        size_t before = p.entries.size();
        p.entries.insert(calls.begin(), calls.end());
        if (p.entries.size() == before) {
            break;
        }
        std::printf("discovery round %d: %zu new call targets\n", round, p.entries.size() - before);
    }

    size_t insnCount = 0;
    std::vector<uint8_t> covered(p.codeHi - p.codeLo);
    for (auto& [e, f] : funcs) {
        liveness(p, f);
        insnCount += f.insns.size();
        for (auto& [a, in] : f.insns) {
            uint32_t len = in.ok ? in.d.length : 1;
            for (uint32_t k = 0; k < len && a + k < p.codeHi; k++) {
                covered[a + k - p.codeLo] = 1;
            }
        }
    }

    std::set<uint32_t> replaced;
    {
        std::ifstream in(argv[3]);
        if (!in) {
            std::fprintf(stderr, "cannot open %s\n", argv[3]);
            return 1;
        }
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream s(line);
            std::string addr;
            if (!(s >> addr)) {
                continue;
            }
            uint32_t a = static_cast<uint32_t>(std::stoul(addr, nullptr, 16));
            if (!funcs.count(a)) {
                std::fprintf(stderr, "replaced function 0x%08x is not a function entry\n", a);
                return 1;
            }
            replaced.insert(a);
        }
    }

    std::string outDir = argv[4];
    unsigned chunks = static_cast<unsigned>(std::stoul(argv[5]));
    EmitStats stats;
    std::vector<std::string> chunkText(chunks, "#include \"funcs.h\"\n\n");
    size_t perChunk = insnCount / chunks + 1;
    size_t acc = 0;
    std::string header = "#pragma once\n\n#include \"x86.h\"\n\n";
    std::string table = "#include \"funcs.h\"\n\nconst FuncEntry g_func_table[] = {\n";
    for (auto& [e, f] : funcs) {
        unsigned idx = std::min<unsigned>(static_cast<unsigned>(acc / perChunk), chunks - 1);
        acc += f.insns.size();
        std::string name = "f_" + hex8(e) + (replaced.count(e) ? "_orig" : "");
        chunkText[idx] += translate(p, f, name, stats);
        header += "void f_" + hex8(e) + "(Ctx* __restrict c, uint8_t* __restrict m);\n";
        if (replaced.count(e)) {
            header += "void " + name + "(Ctx* __restrict c, uint8_t* __restrict m);\n";
        }
        table += "    {0x" + hex8(e) + "u, f_" + hex8(e) + "},\n";
    }
    table += "};\n\nconst uint32_t g_func_count = " + std::to_string(funcs.size()) + ";\n";
    bool ok = writeFile(outDir + "/funcs.h", header) && writeFile(outDir + "/func_table.cpp", table);
    for (unsigned i = 0; i < chunks && ok; i++) {
        ok = writeFile(outDir + "/chunk_" + std::to_string(i) + ".cpp", chunkText[i]);
    }
    if (!ok) {
        return 1;
    }

    size_t cov = std::count(covered.begin(), covered.end(), 1);
    std::printf("functions: %zu (%zu from the function list, %zu from code pointers in data, %zu found as call targets)\n", funcs.size(), listed, fromData, funcs.size() - listed - fromData);
    std::printf("jump tables recovered from relocations: %zu\n", p.recoveredTables.size());
    std::printf("instructions: %zu, code bytes covered: %zu of %u (%.1f%%)\n", insnCount, cov, p.codeHi - p.codeLo, 100.0 * cov / (p.codeHi - p.codeLo));
    std::printf("indirect jumps without a table: %u, branches to addresses outside code: %u\n", stats.indirectJumps, stats.badTargets);
    std::vector<std::pair<unsigned, std::string>> unsup;
    for (auto& [name, n] : stats.unsupported) {
        unsup.emplace_back(n, name);
    }
    std::sort(unsup.rbegin(), unsup.rend());
    std::printf("unsupported instructions: %zu kinds\n", unsup.size());
    for (auto& [n, name] : unsup) {
        std::printf("  %6u %s\n", n, name.c_str());
    }
    return 0;
}
