#include "recomp.h"

#include <cstdio>
#include <iterator>
#include <set>
#include <sstream>

namespace {

std::string hex(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%llxu", static_cast<unsigned long long>(v));
    return b;
}

std::string hex8(uint32_t v) {
    char b[12];
    std::snprintf(b, sizeof b, "%08x", v);
    return b;
}

const char* utype(unsigned bits) {
    switch (bits) {
    case 8: return "uint8_t";
    case 16: return "uint16_t";
    case 64: return "uint64_t";
    default: return "uint32_t";
    }
}

const char* stype(unsigned bits) {
    switch (bits) {
    case 8: return "int8_t";
    case 16: return "int16_t";
    case 64: return "int64_t";
    default: return "int32_t";
    }
}

uint64_t mask(unsigned bits) {
    return bits >= 64 ? ~uint64_t(0) : (uint64_t(1) << bits) - 1;
}

bool isMnemonic(const Insn& in, std::initializer_list<ZydisMnemonic> list) {
    for (ZydisMnemonic m : list) {
        if (in.d.mnemonic == m) {
            return true;
        }
    }
    return false;
}

const char* condition(const std::string& suffix) {
    static const std::map<std::string, const char*> table = {
        {"o", "c->of"}, {"no", "!c->of"}, {"b", "c->cf"}, {"nb", "!c->cf"},
        {"z", "c->zf"}, {"nz", "!c->zf"}, {"be", "c->cf | c->zf"}, {"nbe", "!(c->cf | c->zf)"},
        {"s", "c->sf"}, {"ns", "!c->sf"}, {"p", "c->pf"}, {"np", "!c->pf"},
        {"l", "c->sf != c->of"}, {"nl", "c->sf == c->of"},
        {"le", "c->zf || c->sf != c->of"}, {"nle", "!c->zf && c->sf == c->of"},
    };
    auto it = table.find(suffix);
    return it == table.end() ? nullptr : it->second;
}

const char* conditionFor(const Insn& in, const char* prefix) {
    std::string name = ZydisMnemonicGetString(in.d.mnemonic);
    std::string pre = prefix;
    if (name.compare(0, pre.size(), pre) != 0) {
        return nullptr;
    }
    return condition(name.substr(pre.size()));
}

class Translator {
public:
    Translator(const Program& p, const Function& f, const std::string& name, EmitStats& stats) : p_(p), f_(f), name_(name), stats_(stats) {}

    std::string run();

private:
    const Program& p_;
    const Function& f_;
    std::string name_;
    EmitStats& stats_;
    std::ostringstream o_;
    std::set<uint32_t> labels_;

    void collectLabels();
    void emit(const Insn& in);
    bool fallsThrough(const Insn& in) const;
    std::string jumpTo(uint32_t from, uint32_t target);
    void unsupported(const Insn& in, const char* why = nullptr);

    std::string reg(ZydisRegister r) const;
    std::string ea(const ZydisDecodedOperand& o, bool withSegment) const;
    int memOperand(const Insn& in) const;
    std::string read(const Insn& in, int i, unsigned bits) const;
    std::string write(const Insn& in, int i, const std::string& value) const;
    std::string fpuValue(const Insn& in, int i) const;
    std::string fpuInt(const Insn& in, int i) const;
    bool regClass(const ZydisDecodedOperand& o, ZydisRegisterClass cls) const;
    std::string mask(const Insn& in) const { return hex(in.liveOut & in.writes); }

    bool emitAlu(const Insn& in);
    bool emitData(const Insn& in);
    bool emitControl(const Insn& in);
    bool emitString(const Insn& in);
    bool emitFpu(const Insn& in);
    bool emitMmx(const Insn& in);
};

std::string Translator::reg(ZydisRegister r) const {
    switch (ZydisRegisterGetClass(r)) {
    case ZYDIS_REGCLASS_GPR8:
    case ZYDIS_REGCLASS_GPR16:
    case ZYDIS_REGCLASS_GPR32:
        return std::string("c->") + ZydisRegisterGetString(r);
    case ZYDIS_REGCLASS_MMX:
        return "c->mm[" + std::to_string(r - ZYDIS_REGISTER_MM0) + "]";
    case ZYDIS_REGCLASS_X87:
        return "ST(" + std::to_string(r - ZYDIS_REGISTER_ST0) + ")";
    default:
        return {};
    }
}

bool Translator::regClass(const ZydisDecodedOperand& o, ZydisRegisterClass cls) const {
    return o.type == ZYDIS_OPERAND_TYPE_REGISTER && ZydisRegisterGetClass(o.reg.value) == cls;
}

std::string Translator::ea(const ZydisDecodedOperand& o, bool withSegment) const {
    std::string s;
    if (o.mem.base != ZYDIS_REGISTER_NONE) {
        s = reg(o.mem.base);
    }
    if (o.mem.index != ZYDIS_REGISTER_NONE) {
        if (!s.empty()) {
            s += " + ";
        }
        s += reg(o.mem.index);
        if (o.mem.scale > 1) {
            s += " * " + std::to_string(o.mem.scale);
        }
    }
    uint32_t disp = static_cast<uint32_t>(o.mem.disp.value);
    if (disp || s.empty()) {
        if (!s.empty()) {
            s += " + ";
        }
        s += hex(disp);
    }
    if (withSegment && o.mem.segment == ZYDIS_REGISTER_FS) {
        s = "c->fs_base + " + s;
    }
    return "uint32_t(" + s + ")";
}

int Translator::memOperand(const Insn& in) const {
    for (int i = 0; i < in.d.operand_count_visible; i++) {
        if (in.op[i].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            return i;
        }
    }
    return -1;
}

std::string Translator::read(const Insn& in, int i, unsigned bits) const {
    const ZydisDecodedOperand& o = in.op[i];
    switch (o.type) {
    case ZYDIS_OPERAND_TYPE_REGISTER:
        if (ZydisRegisterGetClass(o.reg.value) == ZYDIS_REGCLASS_SEGMENT) {
            return hex(o.reg.value == ZYDIS_REGISTER_CS ? 0x1b : o.reg.value == ZYDIS_REGISTER_FS ? 0x3b : 0x23);
        }
        return reg(o.reg.value);
    case ZYDIS_OPERAND_TYPE_MEMORY:
        return std::string("rd<") + utype(o.size) + ">(m, ea)";
    case ZYDIS_OPERAND_TYPE_IMMEDIATE:
        return std::string(utype(bits)) + "(" + hex(o.imm.value.u & ::mask(bits)) + ")";
    default:
        return "0";
    }
}

std::string Translator::write(const Insn& in, int i, const std::string& value) const {
    const ZydisDecodedOperand& o = in.op[i];
    if (o.type == ZYDIS_OPERAND_TYPE_MEMORY) {
        return std::string("wr<") + utype(o.size) + ">(m, ea, " + utype(o.size) + "(" + value + "));";
    }
    if (regClass(o, ZYDIS_REGCLASS_SEGMENT)) {
        return ";";
    }
    return reg(o.reg.value) + " = " + value + ";";
}

std::string Translator::fpuValue(const Insn& in, int i) const {
    const ZydisDecodedOperand& o = in.op[i];
    if (o.type == ZYDIS_OPERAND_TYPE_REGISTER) {
        return reg(o.reg.value);
    }
    switch (o.size) {
    case 32: return "rdf32(m, ea)";
    case 64: return "rdf64(m, ea)";
    default: return "f80_load(m + ea)";
    }
}

std::string Translator::fpuInt(const Insn& in, int i) const {
    const ZydisDecodedOperand& o = in.op[i];
    return std::string("(long double)(") + stype(o.size) + "(rd<" + utype(o.size) + ">(m, ea)))";
}

void Translator::unsupported(const Insn& in, const char* why) {
    std::string text = p_.disasm(in);
    stats_.unsupported[why ? why : ZydisMnemonicGetString(in.d.mnemonic)]++;
    o_ << "unimplemented(c, " << hex(in.addr) << ", \"" << text << "\");\n";
}

std::string Translator::jumpTo(uint32_t from, uint32_t target) {
    if (f_.insns.count(target) && (target == f_.entry || !p_.isEntry(target))) {
        return "goto L_" + hex8(target) + ";";
    }
    if (p_.isEntry(target)) {
        return "{ f_" + hex8(target) + "(c, m); return; }";
    }
    stats_.badTargets++;
    return "bad_jump(c, " + hex(from) + ", " + hex(target) + ");";
}

bool Translator::fallsThrough(const Insn& in) const {
    if (!in.ok) {
        return false;
    }
    switch (classify(p_, in)) {
    case Flow::Next:
    case Flow::Branch:
    case Flow::Call:
    case Flow::CallIndirect:
        return true;
    default:
        return false;
    }
}

void Translator::collectLabels() {
    if (f_.insns.begin()->first != f_.entry) {
        labels_.insert(f_.entry);
    }
    for (auto it = f_.insns.begin(); it != f_.insns.end(); ++it) {
        const Insn& in = it->second;
        if (!in.ok) {
            continue;
        }
        Flow fl = classify(p_, in);
        uint32_t t;
        if ((fl == Flow::Branch || fl == Flow::Jump) && branchTarget(in, t)) {
            labels_.insert(t);
        }
        if (fl == Flow::JumpTable) {
            for (uint32_t x : p_.jumpTables.at(in.addr)) {
                labels_.insert(x);
            }
        }
        if (fallsThrough(in)) {
            auto next = std::next(it);
            uint32_t n = in.addr + in.d.length;
            if (next == f_.insns.end() || next->first != n) {
                labels_.insert(n);
            }
        }
    }
}

std::string Translator::run() {
    collectLabels();
    o_ << "void " << name_ << "(Ctx* __restrict c, uint8_t* __restrict m) {\n";
    if (f_.insns.begin()->first != f_.entry) {
        o_ << "goto L_" << hex8(f_.entry) << ";\n";
    }
    for (auto it = f_.insns.begin(); it != f_.insns.end(); ++it) {
        const Insn& in = it->second;
        if (labels_.count(in.addr)) {
            o_ << "L_" << hex8(in.addr) << ":;\n";
        }
        if (!in.ok) {
            stats_.unsupported["(decode error)"]++;
            o_ << "// " << hex8(in.addr) << ": decode error\n";
            o_ << "unimplemented(c, " << hex(in.addr) << ", \"decode error\");\n";
            continue;
        }
        o_ << "// " << hex8(in.addr) << ": " << p_.disasm(in) << "\n";
        emit(in);
        if (fallsThrough(in)) {
            auto next = std::next(it);
            uint32_t n = in.addr + in.d.length;
            if (next == f_.insns.end() || next->first != n) {
                o_ << jumpTo(in.addr, n) << "\n";
            }
        }
    }
    o_ << "}\n\n";
    return o_.str();
}

void Translator::emit(const Insn& in) {
    if (in.d.address_width == 16) {
        unsupported(in, "16-bit addressing");
        return;
    }
    if (emitControl(in) || emitAlu(in) || emitData(in) || emitString(in) || emitFpu(in) || emitMmx(in)) {
        return;
    }
    unsupported(in);
}

bool Translator::emitControl(const Insn& in) {
    Flow fl = classify(p_, in);
    uint32_t next = in.addr + in.d.length;
    uint32_t t = 0;
    int mi = memOperand(in);
    std::string eaDecl = mi >= 0 ? "const uint32_t ea = " + ea(in.op[mi], true) + "; " : "";
    switch (fl) {
    case Flow::Branch: {
        branchTarget(in, t);
        std::string cond;
        if (const char* cc = conditionFor(in, "j")) {
            cond = cc;
        }
        switch (in.d.mnemonic) {
        case ZYDIS_MNEMONIC_JECXZ: cond = "c->ecx == 0"; break;
        case ZYDIS_MNEMONIC_JCXZ: cond = "c->cx == 0"; break;
        case ZYDIS_MNEMONIC_LOOP: cond = "--c->ecx != 0"; break;
        case ZYDIS_MNEMONIC_LOOPE: cond = "--c->ecx != 0 && c->zf"; break;
        case ZYDIS_MNEMONIC_LOOPNE: cond = "--c->ecx != 0 && !c->zf"; break;
        default: break;
        }
        if (cond.empty()) {
            unsupported(in);
            return true;
        }
        o_ << "if (" << cond << ") " << jumpTo(in.addr, t) << "\n";
        return true;
    }
    case Flow::Jump:
        branchTarget(in, t);
        o_ << jumpTo(in.addr, t) << "\n";
        return true;
    case Flow::JumpTable:
        o_ << "{ " << eaDecl << "uint32_t t = " << read(in, 0, 32) << ";\nswitch (t) {\n";
        for (uint32_t x : std::set<uint32_t>(p_.jumpTables.at(in.addr).begin(), p_.jumpTables.at(in.addr).end())) {
            o_ << "case " << hex(x) << ": " << jumpTo(in.addr, x) << "\n";
        }
        o_ << "default: call_indirect(c, m, t); return;\n}\n}\n";
        return true;
    case Flow::JumpIndirect:
        if (in.op[0].type == ZYDIS_OPERAND_TYPE_POINTER || in.d.meta.branch_type == ZYDIS_BRANCH_TYPE_FAR) {
            unsupported(in, "far jmp");
            return true;
        }
        stats_.indirectJumps++;
        o_ << "{ " << eaDecl << "uint32_t t = " << read(in, 0, 32) << "; call_indirect(c, m, t); return; }\n";
        return true;
    case Flow::Call:
        branchTarget(in, t);
        if (!p_.inCode(t)) {
            stats_.badTargets++;
            o_ << "bad_jump(c, " << hex(in.addr) << ", " << hex(t) << ");\n";
            return true;
        }
        o_ << "push32(c, m, " << hex(next) << "); f_" << hex8(t) << "(c, m);\n";
        return true;
    case Flow::CallIndirect:
        if (in.op[0].type == ZYDIS_OPERAND_TYPE_POINTER || in.d.meta.branch_type == ZYDIS_BRANCH_TYPE_FAR) {
            unsupported(in, "far call");
            return true;
        }
        o_ << "{ " << eaDecl << "uint32_t t = " << read(in, 0, 32) << "; push32(c, m, " << hex(next) << "); call_indirect(c, m, t); }\n";
        return true;
    case Flow::Return:
        if (in.d.mnemonic != ZYDIS_MNEMONIC_RET || in.d.meta.branch_type == ZYDIS_BRANCH_TYPE_FAR) {
            unsupported(in);
            return true;
        }
        o_ << "c->esp += " << (in.d.operand_count_visible ? 4 + in.op[0].imm.value.u : 4) << "; return;\n";
        return true;
    case Flow::Stop:
        unsupported(in);
        return true;
    default:
        return false;
    }
}

bool Translator::emitAlu(const Insn& in) {
    unsigned w = in.d.operand_width;
    std::string T = utype(w);
    int mi = memOperand(in);
    std::string eaDecl = mi >= 0 ? "const uint32_t ea = " + ea(in.op[mi], true) + "; " : "";
    std::string M = mask(in);
    const char* fn = nullptr;
    bool store = true;
    bool logic = false;
    switch (in.d.mnemonic) {
    case ZYDIS_MNEMONIC_ADD: fn = "op_add"; break;
    case ZYDIS_MNEMONIC_ADC: fn = "op_adc"; break;
    case ZYDIS_MNEMONIC_SUB: fn = "op_sub"; break;
    case ZYDIS_MNEMONIC_SBB: fn = "op_sbb"; break;
    case ZYDIS_MNEMONIC_CMP: fn = "op_sub"; store = false; break;
    case ZYDIS_MNEMONIC_AND: fn = "&"; logic = true; break;
    case ZYDIS_MNEMONIC_OR: fn = "|"; logic = true; break;
    case ZYDIS_MNEMONIC_XOR: fn = "^"; logic = true; break;
    case ZYDIS_MNEMONIC_TEST: fn = "&"; logic = true; store = false; break;
    default: break;
    }
    if (fn) {
        if (!store && (in.liveOut & in.writes) == 0) {
            return true;
        }
        std::string a = read(in, 0, w);
        std::string b = read(in, 1, w);
        std::string r = logic ? "op_logic<" + M + ">(c, " + T + "(a " + fn + " b))" : std::string(fn) + "<" + M + ">(c, a, b)";
        o_ << "{ " << eaDecl << T << " a = " << a << "; " << T << " b = " << T << "(" << b << "); ";
        o_ << (store ? write(in, 0, r) : r + ";") << " }\n";
        return true;
    }

    const char* un = nullptr;
    switch (in.d.mnemonic) {
    case ZYDIS_MNEMONIC_INC: un = "op_inc"; break;
    case ZYDIS_MNEMONIC_DEC: un = "op_dec"; break;
    case ZYDIS_MNEMONIC_NEG: un = "op_neg"; break;
    default: break;
    }
    if (un) {
        o_ << "{ " << eaDecl << T << " a = " << read(in, 0, w) << "; " << write(in, 0, std::string(un) + "<" + M + ">(c, a)") << " }\n";
        return true;
    }
    if (in.d.mnemonic == ZYDIS_MNEMONIC_NOT) {
        o_ << "{ " << eaDecl << T << " a = " << read(in, 0, w) << "; " << write(in, 0, T + "(~a)") << " }\n";
        return true;
    }

    const char* sh = nullptr;
    switch (in.d.mnemonic) {
    case ZYDIS_MNEMONIC_SHL: sh = "op_shl"; break;
    case ZYDIS_MNEMONIC_SHR: sh = "op_shr"; break;
    case ZYDIS_MNEMONIC_SAR: sh = "op_sar"; break;
    case ZYDIS_MNEMONIC_ROL: sh = "op_rol"; break;
    case ZYDIS_MNEMONIC_ROR: sh = "op_ror"; break;
    case ZYDIS_MNEMONIC_RCL: sh = "op_rcl"; break;
    case ZYDIS_MNEMONIC_RCR: sh = "op_rcr"; break;
    default: break;
    }
    if (sh) {
        o_ << "{ " << eaDecl << T << " a = " << read(in, 0, w) << "; uint8_t n = uint8_t(" << read(in, 1, 8) << "); ";
        o_ << write(in, 0, std::string(sh) + "<" + M + ">(c, a, n)") << " }\n";
        return true;
    }
    if (in.d.mnemonic == ZYDIS_MNEMONIC_SHLD || in.d.mnemonic == ZYDIS_MNEMONIC_SHRD) {
        const char* name = in.d.mnemonic == ZYDIS_MNEMONIC_SHLD ? "op_shld" : "op_shrd";
        o_ << "{ " << eaDecl << T << " a = " << read(in, 0, w) << "; " << T << " b = " << read(in, 1, w) << "; uint8_t n = uint8_t(" << read(in, 2, 8) << "); ";
        o_ << write(in, 0, std::string(name) + "<" + M + ">(c, a, b, n)") << " }\n";
        return true;
    }

    if (in.d.mnemonic == ZYDIS_MNEMONIC_IMUL && in.d.operand_count_visible >= 2) {
        int s1 = in.d.operand_count_visible == 2 ? 0 : 1;
        int s2 = in.d.operand_count_visible == 2 ? 1 : 2;
        o_ << "{ " << eaDecl << T << " a = " << read(in, s1, w) << "; " << T << " b = " << T << "(" << read(in, s2, w) << "); ";
        o_ << write(in, 0, "op_imul<" + M + ">(c, a, b)") << " }\n";
        return true;
    }
    const char* md = nullptr;
    bool flags = true;
    switch (in.d.mnemonic) {
    case ZYDIS_MNEMONIC_MUL: md = "op_mul"; break;
    case ZYDIS_MNEMONIC_IMUL: md = "op_imul"; break;
    case ZYDIS_MNEMONIC_DIV: md = "op_div"; flags = false; break;
    case ZYDIS_MNEMONIC_IDIV: md = "op_idiv"; flags = false; break;
    default: break;
    }
    if (md) {
        unsigned bits = in.op[0].size;
        o_ << "{ " << eaDecl << md << bits << (flags ? "<" + M + ">" : "") << "(c, " << read(in, 0, bits) << "); }\n";
        return true;
    }

    switch (in.d.mnemonic) {
    case ZYDIS_MNEMONIC_CDQ: o_ << "c->edx = uint32_t(int32_t(c->eax) >> 31);\n"; return true;
    case ZYDIS_MNEMONIC_CWDE: o_ << "c->eax = uint32_t(int32_t(int16_t(c->ax)));\n"; return true;
    case ZYDIS_MNEMONIC_CBW: o_ << "c->ax = uint16_t(int16_t(int8_t(c->al)));\n"; return true;
    case ZYDIS_MNEMONIC_CWD: o_ << "c->dx = uint16_t(int16_t(c->ax) >> 15);\n"; return true;
    case ZYDIS_MNEMONIC_CLC: o_ << "c->cf = 0;\n"; return true;
    case ZYDIS_MNEMONIC_STC: o_ << "c->cf = 1;\n"; return true;
    case ZYDIS_MNEMONIC_CMC: o_ << "c->cf ^= 1;\n"; return true;
    case ZYDIS_MNEMONIC_CLD: o_ << "c->df = 0;\n"; return true;
    case ZYDIS_MNEMONIC_STD: o_ << "c->df = 1;\n"; return true;
    case ZYDIS_MNEMONIC_SAHF: o_ << "op_sahf(c);\n"; return true;
    case ZYDIS_MNEMONIC_LAHF: o_ << "op_lahf(c);\n"; return true;
    default: break;
    }

    if (const char* cc = conditionFor(in, "set")) {
        o_ << "{ " << eaDecl << write(in, 0, std::string("uint8_t(") + cc + ")") << " }\n";
        return true;
    }
    if (const char* cc = conditionFor(in, "cmov")) {
        o_ << "{ " << eaDecl << T << " v = " << read(in, 1, w) << "; if (" << cc << ") " << write(in, 0, "v") << " }\n";
        return true;
    }

    switch (in.d.mnemonic) {
    case ZYDIS_MNEMONIC_BT:
    case ZYDIS_MNEMONIC_BTS:
    case ZYDIS_MNEMONIC_BTR:
    case ZYDIS_MNEMONIC_BTC: {
        if (in.op[0].type == ZYDIS_OPERAND_TYPE_MEMORY && in.op[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            unsupported(in, "bt mem, reg");
            return true;
        }
        o_ << "{ " << eaDecl << T << " a = " << read(in, 0, w) << "; unsigned k = unsigned(" << read(in, 1, 8) << ") % " << w << "; c->cf = (a >> k) & 1; ";
        if (in.d.mnemonic == ZYDIS_MNEMONIC_BTS) o_ << write(in, 0, T + "(a | (" + T + "(1) << k))");
        if (in.d.mnemonic == ZYDIS_MNEMONIC_BTR) o_ << write(in, 0, T + "(a & ~(" + T + "(1) << k))");
        if (in.d.mnemonic == ZYDIS_MNEMONIC_BTC) o_ << write(in, 0, T + "(a ^ (" + T + "(1) << k))");
        o_ << " }\n";
        return true;
    }
    case ZYDIS_MNEMONIC_BSF:
    case ZYDIS_MNEMONIC_BSR: {
        std::string bit = in.d.mnemonic == ZYDIS_MNEMONIC_BSF ? "__builtin_ctz(v)" : std::to_string(31) + " - __builtin_clz(v)";
        o_ << "{ " << eaDecl << "uint32_t v = " << read(in, 1, w) << "; c->zf = v == 0; if (v) " << write(in, 0, T + "(" + bit + ")") << " }\n";
        return true;
    }
    case ZYDIS_MNEMONIC_BSWAP:
        o_ << reg(in.op[0].reg.value) << " = __builtin_bswap32(" << reg(in.op[0].reg.value) << ");\n";
        return true;
    case ZYDIS_MNEMONIC_XADD:
        o_ << "{ " << eaDecl << T << " a = " << read(in, 0, w) << "; " << T << " b = " << read(in, 1, w) << "; ";
        o_ << write(in, 1, "a") << " " << write(in, 0, "op_add<" + M + ">(c, a, b)") << " }\n";
        return true;
    case ZYDIS_MNEMONIC_CMPXCHG: {
        std::string acc = w == 8 ? "c->al" : w == 16 ? "c->ax" : "c->eax";
        o_ << "{ " << eaDecl << T << " a = " << read(in, 0, w) << "; op_sub<" << hex(0x8d5) << ">(c, " << acc << ", a); ";
        o_ << "if (c->zf) " << write(in, 0, read(in, 1, w)) << " else " << acc << " = a; }\n";
        return true;
    }
    default:
        return false;
    }
}

bool Translator::emitData(const Insn& in) {
    unsigned w = in.d.operand_width;
    std::string T = utype(w);
    int mi = memOperand(in);
    std::string eaDecl = mi >= 0 ? "const uint32_t ea = " + ea(in.op[mi], true) + "; " : "";
    switch (in.d.mnemonic) {
    case ZYDIS_MNEMONIC_MOV:
        if (regClass(in.op[0], ZYDIS_REGCLASS_CONTROL) || regClass(in.op[0], ZYDIS_REGCLASS_DEBUG) ||
            regClass(in.op[1], ZYDIS_REGCLASS_CONTROL) || regClass(in.op[1], ZYDIS_REGCLASS_DEBUG)) {
            unsupported(in, "mov cr/dr");
            return true;
        }
        if (regClass(in.op[0], ZYDIS_REGCLASS_SEGMENT)) {
            o_ << ";\n";
            return true;
        }
        o_ << "{ " << eaDecl << write(in, 0, read(in, 1, in.op[0].size)) << " }\n";
        return true;
    case ZYDIS_MNEMONIC_MOVZX:
        o_ << "{ " << eaDecl << write(in, 0, std::string(utype(in.op[0].size)) + "(" + read(in, 1, in.op[1].size) + ")") << " }\n";
        return true;
    case ZYDIS_MNEMONIC_MOVSX:
        o_ << "{ " << eaDecl << write(in, 0, std::string(utype(in.op[0].size)) + "(" + stype(in.op[1].size) + "(" + read(in, 1, in.op[1].size) + "))") << " }\n";
        return true;
    case ZYDIS_MNEMONIC_LEA:
        o_ << write(in, 0, std::string(utype(in.op[0].size)) + "(" + ea(in.op[1], false) + ")") << "\n";
        return true;
    case ZYDIS_MNEMONIC_XCHG:
        o_ << "{ " << eaDecl << T << " a = " << read(in, 0, w) << "; " << T << " b = " << read(in, 1, w) << "; " << write(in, 0, "b") << " " << write(in, 1, "a") << " }\n";
        return true;
    case ZYDIS_MNEMONIC_PUSH:
        if (w == 16) {
            o_ << "{ " << eaDecl << "uint16_t v = uint16_t(" << read(in, 0, 16) << "); c->esp -= 2; wr16(m, c->esp, v); }\n";
        } else {
            o_ << "{ " << eaDecl << "uint32_t v = uint32_t(" << read(in, 0, 32) << "); push32(c, m, v); }\n";
        }
        return true;
    case ZYDIS_MNEMONIC_POP:
        if (w == 16) {
            o_ << "{ uint16_t v = rd16(m, c->esp); c->esp += 2; ";
        } else {
            o_ << "{ uint32_t v = pop32(c, m); ";
        }
        o_ << eaDecl << write(in, 0, "v") << " }\n";
        return true;
    case ZYDIS_MNEMONIC_PUSHAD:
        o_ << "{ uint32_t t = c->esp; push32(c, m, c->eax); push32(c, m, c->ecx); push32(c, m, c->edx); push32(c, m, c->ebx); "
              "push32(c, m, t); push32(c, m, c->ebp); push32(c, m, c->esi); push32(c, m, c->edi); }\n";
        return true;
    case ZYDIS_MNEMONIC_POPAD:
        o_ << "{ c->edi = pop32(c, m); c->esi = pop32(c, m); c->ebp = pop32(c, m); c->esp += 4; "
              "c->ebx = pop32(c, m); c->edx = pop32(c, m); c->ecx = pop32(c, m); c->eax = pop32(c, m); }\n";
        return true;
    case ZYDIS_MNEMONIC_PUSHFD:
        o_ << "push32(c, m, get_eflags(c));\n";
        return true;
    case ZYDIS_MNEMONIC_POPFD:
        o_ << "set_eflags(c, pop32(c, m));\n";
        return true;
    case ZYDIS_MNEMONIC_LEAVE:
        o_ << "c->esp = c->ebp; c->ebp = pop32(c, m);\n";
        return true;
    case ZYDIS_MNEMONIC_ENTER:
        if (in.op[1].imm.value.u != 0) {
            unsupported(in, "enter with nesting");
            return true;
        }
        o_ << "push32(c, m, c->ebp); c->ebp = c->esp; c->esp -= " << hex(in.op[0].imm.value.u) << ";\n";
        return true;
    case ZYDIS_MNEMONIC_XLAT:
        o_ << "c->al = rd8(m, uint32_t(c->ebx + c->al));\n";
        return true;
    case ZYDIS_MNEMONIC_CPUID:
        o_ << "cpuid(c);\n";
        return true;
    case ZYDIS_MNEMONIC_NOP:
    case ZYDIS_MNEMONIC_FNOP:
    case ZYDIS_MNEMONIC_FWAIT:
    case ZYDIS_MNEMONIC_EMMS:
    case ZYDIS_MNEMONIC_PREFETCH:
    case ZYDIS_MNEMONIC_PREFETCHW:
    case ZYDIS_MNEMONIC_PREFETCHT0:
    case ZYDIS_MNEMONIC_PREFETCHT1:
    case ZYDIS_MNEMONIC_PREFETCHT2:
    case ZYDIS_MNEMONIC_PREFETCHNTA:
        return true;
    default:
        return false;
    }
}

bool Translator::emitString(const Insn& in) {
    const char* op = nullptr;
    bool flags = false;
    switch (in.d.mnemonic) {
    case ZYDIS_MNEMONIC_MOVSB:
    case ZYDIS_MNEMONIC_MOVSW:
    case ZYDIS_MNEMONIC_MOVSD: op = "movs"; break;
    case ZYDIS_MNEMONIC_STOSB:
    case ZYDIS_MNEMONIC_STOSW:
    case ZYDIS_MNEMONIC_STOSD: op = "stos"; break;
    case ZYDIS_MNEMONIC_LODSB:
    case ZYDIS_MNEMONIC_LODSW:
    case ZYDIS_MNEMONIC_LODSD: op = "lods"; break;
    case ZYDIS_MNEMONIC_SCASB:
    case ZYDIS_MNEMONIC_SCASW:
    case ZYDIS_MNEMONIC_SCASD: op = "scas"; flags = true; break;
    case ZYDIS_MNEMONIC_CMPSB:
    case ZYDIS_MNEMONIC_CMPSW:
    case ZYDIS_MNEMONIC_CMPSD: op = "cmps"; flags = true; break;
    default: return false;
    }
    if (in.d.operand_count_visible != 0) {
        return false;
    }
    std::string T = utype(in.d.operand_width);
    uint64_t a = in.d.attributes;
    bool rep = a & (ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE | ZYDIS_ATTRIB_HAS_REPNE);
    if (!rep) {
        o_ << "s_" << op << "<" << T << ">(c, m);\n";
    } else if (!flags) {
        o_ << "s_rep_" << op << "<" << T << ">(c, m);\n";
    } else {
        bool z = !(a & ZYDIS_ATTRIB_HAS_REPNE);
        o_ << "s_rep_" << op << "<" << T << ", " << (z ? "true" : "false") << ">(c, m);\n";
    }
    return true;
}

bool Translator::emitFpu(const Insn& in) {
    int mi = memOperand(in);
    std::string eaDecl = mi >= 0 ? "const uint32_t ea = " + ea(in.op[mi], true) + "; " : "";
    switch (in.d.mnemonic) {
    case ZYDIS_MNEMONIC_FLD:
        o_ << "{ " << eaDecl << "long double v = " << fpuValue(in, 0) << "; fpush(c, v); }\n";
        return true;
    case ZYDIS_MNEMONIC_FILD:
        o_ << "{ " << eaDecl << "fpush(c, " << fpuInt(in, 0) << "); }\n";
        return true;
    case ZYDIS_MNEMONIC_FLDZ: o_ << "fpush(c, 0.0L);\n"; return true;
    case ZYDIS_MNEMONIC_FLD1: o_ << "fpush(c, 1.0L);\n"; return true;
    case ZYDIS_MNEMONIC_FLDPI: o_ << "fpush(c, 3.141592653589793238462643383279502884L);\n"; return true;
    case ZYDIS_MNEMONIC_FLDL2E: o_ << "fpush(c, 1.442695040888963407359924681001892137L);\n"; return true;
    case ZYDIS_MNEMONIC_FLDL2T: o_ << "fpush(c, 3.321928094887362347870319429489390175L);\n"; return true;
    case ZYDIS_MNEMONIC_FLDLN2: o_ << "fpush(c, 0.693147180559945309417232121458176568L);\n"; return true;
    case ZYDIS_MNEMONIC_FLDLG2: o_ << "fpush(c, 0.301029995663981195213738894724493027L);\n"; return true;
    case ZYDIS_MNEMONIC_FST:
    case ZYDIS_MNEMONIC_FSTP: {
        const ZydisDecodedOperand& d = in.op[0];
        o_ << "{ " << eaDecl;
        if (d.type == ZYDIS_OPERAND_TYPE_REGISTER) {
            o_ << reg(d.reg.value) << " = ST(0);";
        } else if (d.size == 32) {
            o_ << "wrf32(m, ea, ST(0));";
        } else if (d.size == 64) {
            o_ << "wrf64(m, ea, ST(0));";
        } else {
            o_ << "f80_store(m + ea, ST(0));";
        }
        o_ << (in.d.mnemonic == ZYDIS_MNEMONIC_FSTP ? " fpop(c);" : "") << " }\n";
        return true;
    }
    case ZYDIS_MNEMONIC_FIST:
    case ZYDIS_MNEMONIC_FISTP: {
        unsigned bits = in.op[0].size;
        o_ << "{ " << eaDecl << "wr<" << utype(bits) << ">(m, ea, fist" << bits << "(c, ST(0)));" << (in.d.mnemonic == ZYDIS_MNEMONIC_FISTP ? " fpop(c);" : "") << " }\n";
        return true;
    }
    default:
        break;
    }

    struct Arith {
        ZydisMnemonic mn;
        const char* op;
        bool reverse;
        bool pop;
        bool integer;
    };
    static const Arith arith[] = {
        {ZYDIS_MNEMONIC_FADD, "+", false, false, false}, {ZYDIS_MNEMONIC_FADDP, "+", false, true, false}, {ZYDIS_MNEMONIC_FIADD, "+", false, false, true},
        {ZYDIS_MNEMONIC_FSUB, "-", false, false, false}, {ZYDIS_MNEMONIC_FSUBP, "-", false, true, false}, {ZYDIS_MNEMONIC_FISUB, "-", false, false, true},
        {ZYDIS_MNEMONIC_FSUBR, "-", true, false, false}, {ZYDIS_MNEMONIC_FSUBRP, "-", true, true, false}, {ZYDIS_MNEMONIC_FISUBR, "-", true, false, true},
        {ZYDIS_MNEMONIC_FMUL, "*", false, false, false}, {ZYDIS_MNEMONIC_FMULP, "*", false, true, false}, {ZYDIS_MNEMONIC_FIMUL, "*", false, false, true},
        {ZYDIS_MNEMONIC_FDIV, "/", false, false, false}, {ZYDIS_MNEMONIC_FDIVP, "/", false, true, false}, {ZYDIS_MNEMONIC_FIDIV, "/", false, false, true},
        {ZYDIS_MNEMONIC_FDIVR, "/", true, false, false}, {ZYDIS_MNEMONIC_FDIVRP, "/", true, true, false}, {ZYDIS_MNEMONIC_FIDIVR, "/", true, false, true},
    };
    for (const Arith& a : arith) {
        if (in.d.mnemonic != a.mn) {
            continue;
        }
        std::string dst;
        std::string src;
        if (mi >= 0) {
            dst = "ST(0)";
            src = a.integer ? fpuInt(in, mi) : fpuValue(in, mi);
        } else if (regClass(in.op[0], ZYDIS_REGCLASS_X87) && regClass(in.op[1], ZYDIS_REGCLASS_X87)) {
            dst = reg(in.op[0].reg.value);
            src = reg(in.op[1].reg.value);
        } else {
            unsupported(in, "x87 operand form");
            return true;
        }
        o_ << "{ " << eaDecl << "long double s = " << src << "; " << dst << " = " << (a.reverse ? "s " + std::string(a.op) + " " + dst : dst + " " + a.op + " s") << ";";
        o_ << (a.pop ? " fpop(c);" : "") << " }\n";
        return true;
    }

    switch (in.d.mnemonic) {
    case ZYDIS_MNEMONIC_FCHS: o_ << "ST(0) = -ST(0);\n"; return true;
    case ZYDIS_MNEMONIC_FABS: o_ << "ST(0) = std::fabs(ST(0));\n"; return true;
    case ZYDIS_MNEMONIC_FSQRT: o_ << "ST(0) = std::sqrt(ST(0));\n"; return true;
    case ZYDIS_MNEMONIC_FSIN: o_ << "ST(0) = std::sin(ST(0)); c->fsw &= ~0x400;\n"; return true;
    case ZYDIS_MNEMONIC_FCOS: o_ << "ST(0) = std::cos(ST(0)); c->fsw &= ~0x400;\n"; return true;
    case ZYDIS_MNEMONIC_FSINCOS: o_ << "{ long double v = ST(0); ST(0) = std::sin(v); fpush(c, std::cos(v)); c->fsw &= ~0x400; }\n"; return true;
    case ZYDIS_MNEMONIC_FPTAN: o_ << "ST(0) = std::tan(ST(0)); fpush(c, 1.0L); c->fsw &= ~0x400;\n"; return true;
    case ZYDIS_MNEMONIC_FPATAN: o_ << "ST(1) = std::atan2(ST(1), ST(0)); fpop(c);\n"; return true;
    case ZYDIS_MNEMONIC_FYL2X: o_ << "ST(1) = ST(1) * std::log2(ST(0)); fpop(c);\n"; return true;
    case ZYDIS_MNEMONIC_FYL2XP1: o_ << "ST(1) = ST(1) * std::log2(ST(0) + 1.0L); fpop(c);\n"; return true;
    case ZYDIS_MNEMONIC_F2XM1: o_ << "ST(0) = std::exp2(ST(0)) - 1.0L;\n"; return true;
    case ZYDIS_MNEMONIC_FSCALE: o_ << "ST(0) = std::ldexp(ST(0), int(std::trunc(ST(1))));\n"; return true;
    case ZYDIS_MNEMONIC_FRNDINT: o_ << "ST(0) = fround(c, ST(0));\n"; return true;
    case ZYDIS_MNEMONIC_FPREM: o_ << "fpu_prem(c);\n"; return true;
    case ZYDIS_MNEMONIC_FXCH: {
        std::string other = in.d.operand_count_visible ? reg(in.op[0].reg.value) : "ST(1)";
        o_ << "{ long double t = ST(0); ST(0) = " << other << "; " << other << " = t; }\n";
        return true;
    }
    case ZYDIS_MNEMONIC_FCOM:
    case ZYDIS_MNEMONIC_FCOMP:
    case ZYDIS_MNEMONIC_FCOMPP:
    case ZYDIS_MNEMONIC_FUCOM:
    case ZYDIS_MNEMONIC_FUCOMP:
    case ZYDIS_MNEMONIC_FUCOMPP:
    case ZYDIS_MNEMONIC_FICOM:
    case ZYDIS_MNEMONIC_FICOMP:
    case ZYDIS_MNEMONIC_FTST: {
        std::string src = "ST(1)";
        bool integer = in.d.mnemonic == ZYDIS_MNEMONIC_FICOM || in.d.mnemonic == ZYDIS_MNEMONIC_FICOMP;
        if (in.d.mnemonic == ZYDIS_MNEMONIC_FTST) {
            src = "0.0L";
        } else if (mi >= 0) {
            src = integer ? fpuInt(in, mi) : fpuValue(in, mi);
        } else {
            for (int i = 0; i < in.d.operand_count_visible; i++) {
                if (regClass(in.op[i], ZYDIS_REGCLASS_X87) && in.op[i].reg.value != ZYDIS_REGISTER_ST0) {
                    src = reg(in.op[i].reg.value);
                }
            }
        }
        int pops = 0;
        switch (in.d.mnemonic) {
        case ZYDIS_MNEMONIC_FCOMP:
        case ZYDIS_MNEMONIC_FUCOMP:
        case ZYDIS_MNEMONIC_FICOMP: pops = 1; break;
        case ZYDIS_MNEMONIC_FCOMPP:
        case ZYDIS_MNEMONIC_FUCOMPP: pops = 2; break;
        default: break;
        }
        o_ << "{ " << eaDecl << "fcompare(c, ST(0), " << src << ");";
        for (int i = 0; i < pops; i++) {
            o_ << " fpop(c);";
        }
        o_ << " }\n";
        return true;
    }
    case ZYDIS_MNEMONIC_FCOMI:
    case ZYDIS_MNEMONIC_FCOMIP:
    case ZYDIS_MNEMONIC_FUCOMI:
    case ZYDIS_MNEMONIC_FUCOMIP: {
        bool pop = in.d.mnemonic == ZYDIS_MNEMONIC_FCOMIP || in.d.mnemonic == ZYDIS_MNEMONIC_FUCOMIP;
        o_ << "{ long double a = ST(0); long double b = " << reg(in.op[1].reg.value) << "; c->zf = !(a < b || a > b); c->pf = std::isnan(a) || std::isnan(b); c->cf = !(a >= b); c->of = c->sf = c->af = 0;";
        o_ << (pop ? " fpop(c);" : "") << " }\n";
        return true;
    }
    case ZYDIS_MNEMONIC_FNSTSW:
        if (in.op[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            o_ << "c->ax = fstatus(c);\n";
        } else {
            o_ << "{ " << eaDecl << "wr16(m, ea, fstatus(c)); }\n";
        }
        return true;
    case ZYDIS_MNEMONIC_FNSTCW: o_ << "{ " << eaDecl << "wr16(m, ea, c->fcw); }\n"; return true;
    case ZYDIS_MNEMONIC_FLDCW: o_ << "{ " << eaDecl << "c->fcw = rd16(m, ea); }\n"; return true;
    case ZYDIS_MNEMONIC_FNINIT: o_ << "fpu_init(c);\n"; return true;
    case ZYDIS_MNEMONIC_FNCLEX: o_ << "c->fsw &= 0x7f00;\n"; return true;
    case ZYDIS_MNEMONIC_FNSAVE: o_ << "{ " << eaDecl << "fpu_save(c, m, ea); }\n"; return true;
    case ZYDIS_MNEMONIC_FRSTOR: o_ << "{ " << eaDecl << "fpu_restore(c, m, ea); }\n"; return true;
    case ZYDIS_MNEMONIC_FFREE: return true;
    case ZYDIS_MNEMONIC_FFREEP: o_ << "fpop(c);\n"; return true;
    case ZYDIS_MNEMONIC_FDECSTP: o_ << "c->ftop = (c->ftop - 1) & 7;\n"; return true;
    case ZYDIS_MNEMONIC_FINCSTP: o_ << "c->ftop = (c->ftop + 1) & 7;\n"; return true;
    default:
        return false;
    }
}

bool Translator::emitMmx(const Insn& in) {
    int mi = memOperand(in);
    std::string eaDecl = mi >= 0 ? "const uint32_t ea = " + ea(in.op[mi], true) + "; " : "";
    auto mmxSource = [&](int i) -> std::string {
        const ZydisDecodedOperand& o = in.op[i];
        if (o.type == ZYDIS_OPERAND_TYPE_MEMORY) return "rd64(m, ea)";
        if (o.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) return hex(o.imm.value.u & 0xff);
        if (regClass(o, ZYDIS_REGCLASS_MMX)) return reg(o.reg.value);
        return {};
    };
    if (in.d.mnemonic == ZYDIS_MNEMONIC_MOVD) {
        if (regClass(in.op[0], ZYDIS_REGCLASS_MMX)) {
            o_ << "{ " << eaDecl << reg(in.op[0].reg.value) << " = uint64_t(" << read(in, 1, 32) << "); }\n";
            return true;
        }
        if (regClass(in.op[1], ZYDIS_REGCLASS_MMX)) {
            o_ << "{ " << eaDecl << write(in, 0, "uint32_t(" + reg(in.op[1].reg.value) + ")") << " }\n";
            return true;
        }
        unsupported(in, "movd xmm");
        return true;
    }
    if (in.d.mnemonic == ZYDIS_MNEMONIC_MOVQ) {
        if (regClass(in.op[0], ZYDIS_REGCLASS_MMX) && !mmxSource(1).empty()) {
            o_ << "{ " << eaDecl << reg(in.op[0].reg.value) << " = " << mmxSource(1) << "; }\n";
            return true;
        }
        if (in.op[0].type == ZYDIS_OPERAND_TYPE_MEMORY && regClass(in.op[1], ZYDIS_REGCLASS_MMX)) {
            o_ << "{ " << eaDecl << "wr64(m, ea, " << reg(in.op[1].reg.value) << "); }\n";
            return true;
        }
        unsupported(in, "movq xmm");
        return true;
    }
    static const std::set<std::string> helpers = {
        "paddb", "paddw", "paddd", "paddusb", "paddusw", "psubb", "psubw", "psubd", "psubusb", "psubusw",
        "pmullw", "pmulhw", "pmaddwd", "pcmpeqb", "pcmpeqw", "pcmpeqd", "pcmpgtb", "pcmpgtw", "pcmpgtd",
        "psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq", "psraw", "psrad",
        "packuswb", "packsswb", "packssdw", "punpcklbw", "punpcklwd", "punpckldq", "punpckhbw", "punpckhwd", "punpckhdq",
    };
    static const std::map<std::string, std::string> logic = {
        {"pand", "a & b"}, {"pandn", "~a & b"}, {"por", "a | b"}, {"pxor", "a ^ b"},
    };
    std::string name = ZydisMnemonicGetString(in.d.mnemonic);
    bool isHelper = helpers.count(name) != 0;
    auto lg = logic.find(name);
    if (!isHelper && lg == logic.end()) {
        return false;
    }
    if (!regClass(in.op[0], ZYDIS_REGCLASS_MMX) || mmxSource(1).empty()) {
        unsupported(in, "sse integer");
        return true;
    }
    std::string dst = reg(in.op[0].reg.value);
    o_ << "{ " << eaDecl << "uint64_t a = " << dst << "; uint64_t b = " << mmxSource(1) << "; " << dst << " = ";
    o_ << (isHelper ? "mmx::" + name + "(a, b)" : lg->second) << "; }\n";
    return true;
}

}

Flow classify(const Program& p, const Insn& in) {
    switch (in.d.mnemonic) {
    case ZYDIS_MNEMONIC_RET:
    case ZYDIS_MNEMONIC_IRET:
    case ZYDIS_MNEMONIC_IRETD:
        return Flow::Return;
    case ZYDIS_MNEMONIC_JMP:
        if (in.op[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && in.op[0].imm.is_relative) {
            return Flow::Jump;
        }
        if (auto it = p.jumpTables.find(in.addr); it != p.jumpTables.end() && !it->second.empty()) {
            return Flow::JumpTable;
        }
        return Flow::JumpIndirect;
    case ZYDIS_MNEMONIC_CALL:
        if (in.op[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && in.op[0].imm.is_relative) {
            return Flow::Call;
        }
        return Flow::CallIndirect;
    case ZYDIS_MNEMONIC_HLT:
    case ZYDIS_MNEMONIC_INT:
    case ZYDIS_MNEMONIC_INT1:
    case ZYDIS_MNEMONIC_INT3:
    case ZYDIS_MNEMONIC_INTO:
    case ZYDIS_MNEMONIC_UD0:
    case ZYDIS_MNEMONIC_UD1:
    case ZYDIS_MNEMONIC_UD2:
        return Flow::Stop;
    default:
        break;
    }
    if (in.d.meta.category == ZYDIS_CATEGORY_COND_BR) {
        return Flow::Branch;
    }
    return Flow::Next;
}

bool branchTarget(const Insn& in, uint32_t& target) {
    if (in.op[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE || !in.op[0].imm.is_relative) {
        return false;
    }
    ZyanU64 t = 0;
    if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&in.d, &in.op[0], in.addr, &t))) {
        return false;
    }
    target = static_cast<uint32_t>(t);
    return true;
}

std::string Program::disasm(const Insn& in) const {
    char buf[256];
    ZydisFormatterFormatInstruction(&formatter, &in.d, in.op, in.d.operand_count_visible, buf, sizeof buf, in.addr, nullptr);
    return buf;
}

std::string translate(const Program& p, const Function& f, const std::string& name, EmitStats& stats) {
    return Translator(p, f, name, stats).run();
}
