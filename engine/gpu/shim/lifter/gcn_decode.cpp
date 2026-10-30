// SPDX-License-Identifier: GPL-3.0-or-later
#include "lifter/gcn.h"

namespace Lifter {

namespace {

bool IsVop3b(u16 op) {
    return op == Op::V_ADD_I32 || op == Op::V_SUB_I32 || op == Op::V_SUBREV_I32 ||
           op == Op::V_ADDC_U32 || op == Op::V_SUBB_U32 || op == Op::V_SUBBREV_U32;
}

void Mubuf(Inst& inst, u32 word0, u32 word1) {
    inst.offset = word0 & 0xfff;
    inst.offen = (word0 >> 12) & 1;
    inst.idxen = (word0 >> 13) & 1;
    inst.glc = (word0 >> 14) & 1;
    inst.src[0] = word1 & 0xff;
    inst.src[1] = (word1 >> 8) & 0xff;
    inst.src[2] = (word1 >> 16) & 0x1f;
    inst.slc = (word1 >> 22) & 1;
    inst.src[3] = (word1 >> 24) & 0xff;
}

} // namespace

bool Is64BitDst(const Inst& inst) {
    if (inst.enc == Enc::Sop1) {
        switch (inst.op) {
        case Op::SOP1_S_MOV_B64:
        case Op::SOP1_S_CMOV_B64:
        case Op::SOP1_S_NOT_B64:
        case Op::SOP1_S_WQM_B64:
        case Op::SOP1_S_AND_SAVEEXEC_B64:
        case Op::SOP1_S_OR_SAVEEXEC_B64:
        case Op::SOP1_S_XOR_SAVEEXEC_B64:
        case Op::SOP1_S_ANDN2_SAVEEXEC_B64:
        case Op::SOP1_S_ORN2_SAVEEXEC_B64:
        case Op::SOP1_S_NAND_SAVEEXEC_B64:
        case Op::SOP1_S_NOR_SAVEEXEC_B64:
        case Op::SOP1_S_XNOR_SAVEEXEC_B64:
            return true;
        default:
            return false;
        }
    }
    if (inst.enc == Enc::Sop2) {
        switch (inst.op) {
        case Op::SOP2_S_CSELECT_B64:
        case Op::SOP2_S_AND_B64:
        case Op::SOP2_S_OR_B64:
        case Op::SOP2_S_XOR_B64:
        case Op::SOP2_S_ANDN2_B64:
        case Op::SOP2_S_ORN2_B64:
        case Op::SOP2_S_NAND_B64:
        case Op::SOP2_S_NOR_B64:
        case Op::SOP2_S_XNOR_B64:
        case Op::SOP2_S_LSHL_B64:
        case Op::SOP2_S_LSHR_B64:
        case Op::SOP2_S_ASHR_I64:
            return true;
        default:
            return false;
        }
    }
    return false;
}

DecodeResult Decode(std::span<const u32> code) {
    DecodeResult result;
    result.insts.reserve(code.size());
    size_t at = 0;
    const auto second = [&](size_t index) { return index + 1 < code.size() ? code[index + 1] : 0u; };
    while (at < code.size()) {
        Inst inst{};
        const u32 word = code[at];
        const u32 next = second(at);
        inst.pc = u32(at * 4);
        u32 size = 1;
        if (word >> 25 == 0x3f) {
            inst.enc = Enc::Vop;
            inst.op = u16(Op::Vop1Base + ((word >> 9) & 0xff));
            inst.src[0] = word & 0x1ff;
            inst.dst = u16(SrcVgpr + ((word >> 17) & 0xff));
            if (inst.src[0] == SrcLiteral) {
                inst.literal = next;
                size = 2;
            }
        } else if (word >> 25 == 0x3e) {
            inst.enc = Enc::Vop;
            inst.op = u16((word >> 17) & 0xff);
            inst.src[0] = word & 0x1ff;
            inst.src[1] = u16(SrcVgpr + ((word >> 9) & 0xff));
            inst.sdst = SrcVcc;
            if (inst.src[0] == SrcLiteral) {
                inst.literal = next;
                size = 2;
            }
        } else if (word >> 31 == 0) {
            inst.enc = Enc::Vop;
            inst.op = u16(Op::Vop2Base + ((word >> 25) & 0x3f));
            inst.src[0] = word & 0x1ff;
            inst.src[1] = u16(SrcVgpr + ((word >> 9) & 0xff));
            inst.dst = u16(SrcVgpr + ((word >> 17) & 0xff));
            if (inst.op == Op::V_MADMK_F32 || inst.op == Op::V_MADAK_F32 ||
                inst.src[0] == SrcLiteral) {
                inst.literal = next;
                size = 2;
            }
            if (inst.op == Op::V_READLANE_B32) {
                inst.dst = u16((word >> 17) & 0xff);
                inst.src[1] = u16((word >> 9) & 0xff);
            }
            if (inst.op == Op::V_WRITELANE_B32) {
                inst.src[1] = u16((word >> 9) & 0xff);
            }
            if (IsVop3b(inst.op)) {
                inst.sdst = SrcVcc;
            }
            if (inst.op == Op::V_CNDMASK_B32 || inst.op == Op::V_ADDC_U32 ||
                inst.op == Op::V_SUBB_U32 || inst.op == Op::V_SUBBREV_U32) {
                inst.src[2] = SrcVcc;
            }
        } else if (word >> 23 == 0x17d) {
            inst.enc = Enc::Sop1;
            inst.op = u16((word >> 8) & 0xff);
            inst.src[0] = word & 0xff;
            inst.dst = u16((word >> 16) & 0x7f);
            if (inst.src[0] == SrcLiteral) {
                inst.literal = next;
                size = 2;
            }
        } else if (word >> 23 == 0x17e) {
            inst.enc = Enc::Sopc;
            inst.op = u16((word >> 16) & 0x7f);
            inst.src[0] = word & 0xff;
            inst.src[1] = (word >> 8) & 0xff;
            if (inst.src[0] == SrcLiteral || inst.src[1] == SrcLiteral) {
                inst.literal = next;
                size = 2;
            }
        } else if (word >> 23 == 0x17f) {
            inst.enc = Enc::Sopp;
            inst.op = u16((word >> 16) & 0x7f);
            inst.simm = s32(short(word & 0xffff));
        } else if (word >> 28 == 0xb) {
            inst.enc = Enc::Sopk;
            inst.op = u16((word >> 23) & 0x1f);
            inst.dst = u16((word >> 16) & 0x7f);
            inst.simm = s32(short(word & 0xffff));
        } else if (word >> 30 == 2) {
            inst.enc = Enc::Sop2;
            inst.op = u16((word >> 23) & 0x7f);
            inst.src[0] = word & 0xff;
            inst.src[1] = (word >> 8) & 0xff;
            inst.dst = u16((word >> 16) & 0x7f);
            if (inst.src[0] == SrcLiteral || inst.src[1] == SrcLiteral) {
                inst.literal = next;
                size = 2;
            }
        } else if (word >> 27 == 0x18) {
            inst.enc = Enc::Smrd;
            inst.op = u16((word >> 22) & 0x1f);
            inst.dst = u16((word >> 15) & 0x7f);
            inst.src[0] = u16(((word >> 9) & 0x3f) * 2);
            inst.imm = (word >> 8) & 1;
            inst.offset = word & 0xff;
            if (!inst.imm && inst.offset == SrcLiteral) {
                inst.literal = next;
                size = 2;
            }
        } else if (word >> 26 == 0x34) {
            inst.enc = Enc::Vop;
            inst.e64 = true;
            inst.op = u16((word >> 17) & 0x1ff);
            const u16 op = inst.op;
            inst.dst = u16(SrcVgpr + (word & 0xff));
            if (op < Op::Vop2Base) {
                inst.sdst = u16(word & 0xff);
                inst.dst = 0;
                inst.abs = (word >> 8) & 7;
                inst.clamp = (word >> 11) & 1;
            } else if (IsVop3b(op)) {
                inst.sdst = u16((word >> 8) & 0x7f);
            } else {
                inst.abs = (word >> 8) & 7;
                inst.clamp = (word >> 11) & 1;
            }
            if (op == Op::V_READLANE_B32) {
                inst.dst = u16(word & 0xff);
            }
            inst.src[0] = next & 0x1ff;
            inst.src[1] = (next >> 9) & 0x1ff;
            inst.src[2] = (next >> 18) & 0x1ff;
            inst.omod = (next >> 27) & 3;
            inst.neg = (next >> 29) & 7;
            size = 2;
        } else if (word >> 26 == 0x32) {
            inst.enc = Enc::Vintrp;
            inst.op = u16((word >> 16) & 3);
            inst.src[0] = u16(word & 0xff);
            inst.chan = (word >> 8) & 3;
            inst.attr = (word >> 10) & 0x3f;
            inst.dst = u16(SrcVgpr + ((word >> 18) & 0xff));
        } else if (word >> 26 == 0x36) {
            inst.enc = Enc::Ds;
            inst.op = u16((word >> 18) & 0xff);
            inst.offset = word & 0xff;
            inst.offset1 = (word >> 8) & 0xff;
            inst.gds = (word >> 17) & 1;
            inst.src[0] = u16(SrcVgpr + (next & 0xff));
            inst.src[1] = u16(SrcVgpr + ((next >> 8) & 0xff));
            inst.src[2] = u16(SrcVgpr + ((next >> 16) & 0xff));
            inst.dst = u16(SrcVgpr + ((next >> 24) & 0xff));
            size = 2;
        } else if (word >> 26 == 0x38) {
            inst.enc = Enc::Mubuf;
            inst.op = u16((word >> 18) & 0x7f);
            Mubuf(inst, word, next);
            size = 2;
        } else if (word >> 26 == 0x3a) {
            inst.enc = Enc::Mtbuf;
            inst.op = u16((word >> 16) & 7);
            inst.dfmt = (word >> 19) & 0xf;
            inst.nfmt = (word >> 23) & 7;
            Mubuf(inst, word, next);
            size = 2;
        } else if (word >> 26 == 0x3c) {
            inst.enc = Enc::Mimg;
            inst.op = u16((word >> 18) & 0x7f);
            inst.dmask = (word >> 8) & 0xf;
            inst.unorm = (word >> 12) & 1;
            inst.glc = (word >> 13) & 1;
            inst.da = (word >> 14) & 1;
            inst.r128 = (word >> 15) & 1;
            inst.slc = (word >> 25) & 1;
            inst.src[0] = next & 0xff;
            inst.src[1] = (next >> 8) & 0xff;
            inst.src[2] = (next >> 16) & 0x1f;
            inst.src[3] = (next >> 21) & 0x1f;
            size = 2;
        } else if (word >> 26 == 0x3e) {
            inst.enc = Enc::Exp;
            inst.en = word & 0xf;
            inst.target = (word >> 4) & 0x3f;
            inst.compr = (word >> 10) & 1;
            inst.done = (word >> 11) & 1;
            inst.vm = (word >> 12) & 1;
            inst.src[0] = next & 0xff;
            inst.src[1] = (next >> 8) & 0xff;
            inst.src[2] = (next >> 16) & 0xff;
            inst.src[3] = (next >> 24) & 0xff;
            size = 2;
        } else {
            result.bad_at = u32(at);
            return result;
        }
        at += size;
        inst.next = u32(at * 4);
        result.insts.push_back(inst);
    }
    return result;
}

} // namespace Lifter
