#pragma once

#include "pe.h"

#include <Zydis/Zydis.h>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct Insn {
    uint32_t addr = 0;
    bool ok = false;
    ZydisDecodedInstruction d{};
    ZydisDecodedOperand op[ZYDIS_MAX_OPERAND_COUNT]{};
    uint32_t use = 0;
    uint32_t def = 0;
    uint32_t writes = 0;
    uint32_t liveIn = 0;
    uint32_t liveOut = 0;
};

struct Function {
    uint32_t entry = 0;
    std::map<uint32_t, Insn> insns;
};

struct Program {
    PeImage pe;
    uint32_t codeLo = 0;
    uint32_t codeHi = 0;
    std::set<uint32_t> entries;
    std::unordered_map<uint32_t, std::vector<uint32_t>> jumpTables;
    std::unordered_set<uint32_t> relocSites;
    std::set<uint32_t> recoveredTables;
    ZydisDecoder decoder{};
    ZydisFormatter formatter{};

    bool inCode(uint32_t a) const { return a >= codeLo && a < codeHi; }
    bool isEntry(uint32_t a) const { return entries.count(a) != 0; }
    std::string disasm(const Insn& in) const;
};

enum class Flow { Next, Branch, Jump, JumpTable, JumpIndirect, Call, CallIndirect, Return, Stop };

Flow classify(const Program& p, const Insn& in);
bool branchTarget(const Insn& in, uint32_t& target);

struct EmitStats {
    std::map<std::string, unsigned> unsupported;
    unsigned indirectJumps = 0;
    unsigned badTargets = 0;
};

std::string translate(const Program& p, const Function& f, const std::string& name, EmitStats& stats);
