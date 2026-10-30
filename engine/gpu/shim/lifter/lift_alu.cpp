// SPDX-License-Identifier: GPL-3.0-or-later
#include "lifter/lift_core.h"

namespace Lifter {

namespace {

constexpr u16 O(spv::Op op) {
    return u16(op);
}

} // namespace

void Lift_::LiftSop1(const Inst& inst) {
    switch (inst.op) {
    case Op::SOP1_S_MOV_B32:
        s.r[inst.dst] = SrcRaw(inst.src[0], inst);
        return;
    case Op::SOP1_S_MOV_B64: {
        if (inst.dst == ExecReg) {
            s.r[ExecReg] = Mask(inst.src[0], inst);
            return;
        }
        const u32 code = inst.src[0];
        if (code < 128 && code != ExecReg) {
            const u32 lo = s.r[code];
            const u32 hi = s.r[code + 1];
            s.r[inst.dst] = lo;
            s.r[inst.dst + 1] = hi;
            return;
        }
        if (code == ExecReg) {
            SetMask(inst.dst, Exec());
            return;
        }
        if (code == SrcZero) {
            SetMask(inst.dst, false_id);
            return;
        }
        if ((code >= 193 && code <= 208)) {
            SetMask(inst.dst, true_id);
            return;
        }
        const u32 lo = ToU(SrcRaw(code, inst));
        s.r[inst.dst] = lo;
        s.r[inst.dst + 1] = CU(0);
        return;
    }
    case Op::SOP1_S_NOT_B64: {
        const u32 value = Not(Mask(inst.src[0], inst));
        SetMask(inst.dst, value);
        s.r[RegScc] = value;
        return;
    }
    case Op::SOP1_S_WQM_B64:
        if (inst.dst != ExecReg || inst.src[0] != ExecReg) {
            Fail("s_wqm_b64 on another mask");
        }
        return;
    case Op::SOP1_S_SWAPPC_B64: {
        for (const VsAttribute& attr : in.attributes) {
            u32 parts[4];
            if (!var_param_in[attr.semantic]) {
                const u32 type = attr.cls == NumClass::Float ? t_v4f
                                 : attr.cls == NumClass::Uint ? t_v4u
                                                              : t_v4s;
                var_param_in[attr.semantic] = InputVar(type, attr.semantic, std::nullopt);
            }
            const u32 var = var_param_in[attr.semantic];
            for (u32 c = 0; c < 4; ++c) {
                u32 value;
                if (attr.cls == NumClass::Float) {
                    const u32 ptr = Emit(O(spv::Op::OpAccessChain), ptr_input_f32, Ty::None, {var, CU(c)});
                    value = Emit(O(spv::Op::OpLoad), t_f32, Ty::F32, {ptr});
                } else {
                    const u32 scalar = attr.cls == NumClass::Uint ? t_u32 : t_s32;
                    const u32 ptr = Emit(O(spv::Op::OpAccessChain), PtrType(u32(spv::StorageClass::Input), scalar), Ty::None, {var, CU(c)});
                    const u32 raw = Emit(O(spv::Op::OpLoad), scalar, Ty::None, {ptr});
                    value = Emit(O(spv::Op::OpBitcast), t_f32, Ty::F32, {raw});
                }
                parts[c] = ApplyConv(value, attr.conv);
            }
            for (u32 c = 0; c < 4; ++c) {
                const u8 sel = attr.swizzle[c];
                const u32 chosen = sel == 0 ? CF(0.f) : sel == 1 ? CF(1.f) : parts[(sel - 4) & 3];
                s.r[SrcVgpr + attr.vgpr + c] = chosen;
            }
        }
        return;
    }
    case Op::SOP1_S_AND_SAVEEXEC_B64:
    case Op::SOP1_S_OR_SAVEEXEC_B64:
    case Op::SOP1_S_XOR_SAVEEXEC_B64:
    case Op::SOP1_S_ANDN2_SAVEEXEC_B64:
    case Op::SOP1_S_ORN2_SAVEEXEC_B64:
    case Op::SOP1_S_NAND_SAVEEXEC_B64:
    case Op::SOP1_S_NOR_SAVEEXEC_B64:
    case Op::SOP1_S_XNOR_SAVEEXEC_B64: {
        const u32 old = Exec();
        const u32 src = Mask(inst.src[0], inst);
        u32 value;
        switch (inst.op) {
        case Op::SOP1_S_AND_SAVEEXEC_B64:
            value = And(src, old);
            break;
        case Op::SOP1_S_OR_SAVEEXEC_B64:
            value = Or(src, old);
            break;
        case Op::SOP1_S_XOR_SAVEEXEC_B64:
            value = Xor(src, old);
            break;
        case Op::SOP1_S_ANDN2_SAVEEXEC_B64:
            value = And(src, Not(old));
            break;
        case Op::SOP1_S_ORN2_SAVEEXEC_B64:
            value = Or(src, Not(old));
            break;
        case Op::SOP1_S_NAND_SAVEEXEC_B64:
            value = Not(And(src, old));
            break;
        case Op::SOP1_S_NOR_SAVEEXEC_B64:
            value = Not(Or(src, old));
            break;
        default:
            value = Not(Xor(src, old));
            break;
        }
        SetMask(inst.dst, old);
        s.r[ExecReg] = value;
        s.r[RegScc] = value;
        return;
    }
    default:
        Fail("sop1 " + std::to_string(inst.op));
    }
}

void Lift_::LiftSop2(const Inst& inst) {
    const auto a = [&] { return ToU(SrcRaw(inst.src[0], inst)); };
    const auto b = [&] { return ToU(SrcRaw(inst.src[1], inst)); };
    const auto nonzero = [&](u32 value) {
        if (m.IsConst(value)) {
            return CB(m.ConstBits(value) != 0);
        }
        return ICmp(O(spv::Op::OpINotEqual), value, CU(0));
    };
    const auto mask_op = [&](auto&& fn) {
        const bool for_good = NarrowsForGood(inst);
        const u32 value = fn(Mask(inst.src[0], inst), Mask(inst.src[1], inst));
        SetMask(inst.dst, value);
        s.r[RegScc] = value;
        if (for_good && !m.IsConst(value)) {
            kill_masks.push_back(value);
        }
    };
    const auto signs_overflow = [&](u32 x, u32 y, u32 r, bool sub) {
        const u32 sx = IOp(O(spv::Op::OpShiftRightLogical), x, CU(31));
        const u32 sy = IOp(O(spv::Op::OpShiftRightLogical), y, CU(31));
        const u32 sr = IOp(O(spv::Op::OpShiftRightLogical), r, CU(31));
        const u32 first = ICmp(sub ? O(spv::Op::OpINotEqual) : O(spv::Op::OpIEqual), sx, sy);
        const u32 second = ICmp(O(spv::Op::OpINotEqual), sx, sr);
        return And(first, second);
    };
    const auto shift_amount = [&](u32 value) {
        return IOp(O(spv::Op::OpBitwiseAnd), value, CU(31));
    };
    switch (inst.op) {
    case Op::SOP2_S_ADD_U32: {
        const u32 x = a();
        const u32 y = b();
        const u32 r = IOp(O(spv::Op::OpIAdd), x, y);
        s.r[inst.dst] = r;
        s.r[RegScc] = ICmp(O(spv::Op::OpULessThan), r, x);
        return;
    }
    case Op::SOP2_S_SUB_U32: {
        const u32 x = a();
        const u32 y = b();
        s.r[inst.dst] = IOp(O(spv::Op::OpISub), x, y);
        s.r[RegScc] = ICmp(O(spv::Op::OpUGreaterThan), y, x);
        return;
    }
    case Op::SOP2_S_ADD_I32: {
        const u32 x = a();
        const u32 y = b();
        const u32 r = IOp(O(spv::Op::OpIAdd), x, y);
        s.r[inst.dst] = r;
        s.r[RegScc] = signs_overflow(x, y, r, false);
        return;
    }
    case Op::SOP2_S_SUB_I32: {
        const u32 x = a();
        const u32 y = b();
        const u32 r = IOp(O(spv::Op::OpISub), x, y);
        s.r[inst.dst] = r;
        s.r[RegScc] = signs_overflow(x, y, r, true);
        return;
    }
    case Op::SOP2_S_ADDC_U32: {
        const u32 x = a();
        const u32 y = b();
        const u32 carry = BoolToU(s.r[RegScc] ? s.r[RegScc] : false_id);
        const u32 r1 = IOp(O(spv::Op::OpIAdd), x, y);
        const u32 r2 = IOp(O(spv::Op::OpIAdd), r1, carry);
        s.r[inst.dst] = r2;
        s.r[RegScc] = Or(ICmp(O(spv::Op::OpULessThan), r1, x), ICmp(O(spv::Op::OpULessThan), r2, r1));
        return;
    }
    case Op::SOP2_S_SUBB_U32: {
        const u32 x = a();
        const u32 y = b();
        const u32 borrow = BoolToU(s.r[RegScc] ? s.r[RegScc] : false_id);
        const u32 diff = IOp(O(spv::Op::OpISub), x, y);
        s.r[inst.dst] = IOp(O(spv::Op::OpISub), diff, borrow);
        s.r[RegScc] = Or(ICmp(O(spv::Op::OpUGreaterThan), y, x), ICmp(O(spv::Op::OpUGreaterThan), borrow, diff));
        return;
    }
    case Op::SOP2_S_MIN_I32:
    case Op::SOP2_S_MAX_I32:
    case Op::SOP2_S_MIN_U32:
    case Op::SOP2_S_MAX_U32: {
        const u32 x = a();
        const u32 y = b();
        const GLSLstd450 fn = inst.op == Op::SOP2_S_MIN_I32   ? GLSLstd450SMin
                              : inst.op == Op::SOP2_S_MAX_I32 ? GLSLstd450SMax
                              : inst.op == Op::SOP2_S_MIN_U32 ? GLSLstd450UMin
                                                              : GLSLstd450UMax;
        const u32 r = Ext(fn, t_u32, Ty::U32, {x, y});
        s.r[inst.dst] = r;
        s.r[RegScc] = ICmp(O(spv::Op::OpIEqual), r, x);
        return;
    }
    case Op::SOP2_S_CSELECT_B32:
        s.r[inst.dst] = Select(s.r[RegScc] ? s.r[RegScc] : false_id, a(), b());
        return;
    case Op::SOP2_S_CSELECT_B64: {
        const u32 cond = s.r[RegScc] ? s.r[RegScc] : false_id;
        SetMask(inst.dst, Select(cond, Mask(inst.src[0], inst), Mask(inst.src[1], inst)));
        return;
    }
    case Op::SOP2_S_AND_B32: {
        const u32 r = IOp(O(spv::Op::OpBitwiseAnd), a(), b());
        s.r[inst.dst] = r;
        s.r[RegScc] = nonzero(r);
        return;
    }
    case Op::SOP2_S_OR_B32: {
        const u32 r = IOp(O(spv::Op::OpBitwiseOr), a(), b());
        s.r[inst.dst] = r;
        s.r[RegScc] = nonzero(r);
        return;
    }
    case Op::SOP2_S_XOR_B32: {
        const u32 r = IOp(O(spv::Op::OpBitwiseXor), a(), b());
        s.r[inst.dst] = r;
        s.r[RegScc] = nonzero(r);
        return;
    }
    case Op::SOP2_S_AND_B64:
        return mask_op([&](u32 x, u32 y) { return And(x, y); });
    case Op::SOP2_S_OR_B64:
        return mask_op([&](u32 x, u32 y) { return Or(x, y); });
    case Op::SOP2_S_XOR_B64:
        return mask_op([&](u32 x, u32 y) { return Xor(x, y); });
    case Op::SOP2_S_ANDN2_B64:
        return mask_op([&](u32 x, u32 y) { return And(x, Not(y)); });
    case Op::SOP2_S_ORN2_B64:
        return mask_op([&](u32 x, u32 y) { return Or(x, Not(y)); });
    case Op::SOP2_S_NAND_B64:
        return mask_op([&](u32 x, u32 y) { return Not(And(x, y)); });
    case Op::SOP2_S_NOR_B64:
        return mask_op([&](u32 x, u32 y) { return Not(Or(x, y)); });
    case Op::SOP2_S_XNOR_B64:
        return mask_op([&](u32 x, u32 y) { return Not(Xor(x, y)); });
    case Op::SOP2_S_LSHL_B32: {
        const u32 r = IOp(O(spv::Op::OpShiftLeftLogical), a(), shift_amount(b()));
        s.r[inst.dst] = r;
        s.r[RegScc] = nonzero(r);
        return;
    }
    case Op::SOP2_S_LSHR_B32: {
        const u32 r = IOp(O(spv::Op::OpShiftRightLogical), a(), shift_amount(b()));
        s.r[inst.dst] = r;
        s.r[RegScc] = nonzero(r);
        return;
    }
    case Op::SOP2_S_ASHR_I32: {
        const u32 r = Emit(O(spv::Op::OpShiftRightArithmetic), t_u32, Ty::U32, {a(), shift_amount(b())});
        s.r[inst.dst] = r;
        s.r[RegScc] = nonzero(r);
        return;
    }
    case Op::SOP2_S_BFM_B32: {
        const u32 width = shift_amount(a());
        const u32 offset = shift_amount(b());
        const u32 ones = IOp(O(spv::Op::OpISub), IOp(O(spv::Op::OpShiftLeftLogical), CU(1), width), CU(1));
        s.r[inst.dst] = IOp(O(spv::Op::OpShiftLeftLogical), ones, offset);
        return;
    }
    case Op::SOP2_S_MUL_I32:
        s.r[inst.dst] = IOp(O(spv::Op::OpIMul), a(), b());
        return;
    case Op::SOP2_S_BFE_U32:
    case Op::SOP2_S_BFE_I32: {
        const u32 x = a();
        const u32 y = b();
        const u32 offset = IOp(O(spv::Op::OpBitwiseAnd), y, CU(31));
        const u32 count = BfeU(y, CU(16), CU(7));
        const u32 r = inst.op == Op::SOP2_S_BFE_I32 ? BfeS(x, offset, count) : BfeU(x, offset, count);
        s.r[inst.dst] = r;
        s.r[RegScc] = nonzero(r);
        return;
    }
    default:
        Fail("sop2 " + std::to_string(inst.op));
    }
}

void Lift_::LiftSopc(const Inst& inst) {
    const u32 x = ToU(SrcRaw(inst.src[0], inst));
    const u32 y = ToU(SrcRaw(inst.src[1], inst));
    u16 op;
    switch (inst.op) {
    case Op::SOPC_S_CMP_EQ_I32:
    case Op::SOPC_S_CMP_EQ_U32:
        op = O(spv::Op::OpIEqual);
        break;
    case Op::SOPC_S_CMP_LG_I32:
    case Op::SOPC_S_CMP_LG_U32:
        op = O(spv::Op::OpINotEqual);
        break;
    case Op::SOPC_S_CMP_GT_I32:
        op = O(spv::Op::OpSGreaterThan);
        break;
    case Op::SOPC_S_CMP_GE_I32:
        op = O(spv::Op::OpSGreaterThanEqual);
        break;
    case Op::SOPC_S_CMP_LT_I32:
        op = O(spv::Op::OpSLessThan);
        break;
    case Op::SOPC_S_CMP_LE_I32:
        op = O(spv::Op::OpSLessThanEqual);
        break;
    case Op::SOPC_S_CMP_GT_U32:
        op = O(spv::Op::OpUGreaterThan);
        break;
    case Op::SOPC_S_CMP_GE_U32:
        op = O(spv::Op::OpUGreaterThanEqual);
        break;
    case Op::SOPC_S_CMP_LT_U32:
        op = O(spv::Op::OpULessThan);
        break;
    case Op::SOPC_S_CMP_LE_U32:
        op = O(spv::Op::OpULessThanEqual);
        break;
    default:
        Fail("sopc " + std::to_string(inst.op));
    }
    if (m.IsConst(x) && m.IsConst(y) && op == O(spv::Op::OpIEqual)) {
        s.r[RegScc] = CB(m.ConstBits(x) == m.ConstBits(y));
        return;
    }
    s.r[RegScc] = ICmp(op, x, y);
}

void Lift_::LiftSopk(const Inst& inst) {
    if (inst.op >= 3 && inst.op <= 14) {
        static constexpr spv::Op compares[12] = {
            spv::Op::OpIEqual,     spv::Op::OpINotEqual,        spv::Op::OpSGreaterThan,
            spv::Op::OpSGreaterThanEqual, spv::Op::OpSLessThan, spv::Op::OpSLessThanEqual,
            spv::Op::OpIEqual,     spv::Op::OpINotEqual,        spv::Op::OpUGreaterThan,
            spv::Op::OpUGreaterThanEqual, spv::Op::OpULessThan, spv::Op::OpULessThanEqual};
        const u32 imm = inst.op >= 9 ? u32(inst.simm) & 0xffff : u32(inst.simm);
        s.r[RegScc] = ICmp(O(compares[inst.op - 3]), ToU(s.r[inst.dst]), CU(imm));
        return;
    }
    switch (inst.op) {
    case Op::SOPK_S_CMOVK_I32:
        s.r[inst.dst] = Select(s.r[RegScc] ? s.r[RegScc] : false_id, CU(u32(inst.simm)), ToU(s.r[inst.dst]));
        return;
    case Op::SOPK_S_MOVK_I32:
        s.r[inst.dst] = CU(u32(inst.simm));
        return;
    case Op::SOPK_S_ADDK_I32: {
        const u32 x = ToU(s.r[inst.dst]);
        const u32 y = CU(u32(inst.simm));
        const u32 r = IOp(O(spv::Op::OpIAdd), x, y);
        s.r[inst.dst] = r;
        const u32 sx = IOp(O(spv::Op::OpShiftRightLogical), x, CU(31));
        const u32 sy = IOp(O(spv::Op::OpShiftRightLogical), y, CU(31));
        const u32 sr = IOp(O(spv::Op::OpShiftRightLogical), r, CU(31));
        s.r[RegScc] = And(ICmp(O(spv::Op::OpIEqual), sx, sy), ICmp(O(spv::Op::OpINotEqual), sx, sr));
        return;
    }
    case Op::SOPK_S_MULK_I32:
        s.r[inst.dst] = IOp(O(spv::Op::OpIMul), ToU(s.r[inst.dst]), CU(u32(inst.simm)));
        return;
    default:
        Fail("sopk " + std::to_string(inst.op));
    }
}

void Lift_::LiftVopc(const Inst& inst) {
    const u16 op = inst.op;
    u32 result = 0;
    bool set_exec = false;
    const auto fcmp = [&](u32 kind) {
        const u32 x = SrcF(inst, 0);
        const u32 y = SrcF(inst, 1);
        switch (kind) {
        case 0:
            return false_id;
        case 1:
            return FCmp(O(spv::Op::OpFOrdLessThan), x, y);
        case 2:
            return FCmp(O(spv::Op::OpFOrdEqual), x, y);
        case 3:
            return FCmp(O(spv::Op::OpFOrdLessThanEqual), x, y);
        case 4:
            return FCmp(O(spv::Op::OpFOrdGreaterThan), x, y);
        case 5:
            return FCmp(O(spv::Op::OpFOrdNotEqual), x, y);
        case 6:
            return FCmp(O(spv::Op::OpFOrdGreaterThanEqual), x, y);
        case 7:
            return Not(Or(Emit(O(spv::Op::OpIsNan), t_bool, Ty::Bool, {x}),
                          Emit(O(spv::Op::OpIsNan), t_bool, Ty::Bool, {y})));
        case 8:
            return Or(Emit(O(spv::Op::OpIsNan), t_bool, Ty::Bool, {x}),
                      Emit(O(spv::Op::OpIsNan), t_bool, Ty::Bool, {y}));
        case 9:
            return FCmp(O(spv::Op::OpFUnordLessThan), x, y);
        case 10:
            return FCmp(O(spv::Op::OpFUnordEqual), x, y);
        case 11:
            return FCmp(O(spv::Op::OpFUnordLessThanEqual), x, y);
        case 12:
            return FCmp(O(spv::Op::OpFUnordGreaterThan), x, y);
        case 13:
            return FCmp(O(spv::Op::OpFUnordNotEqual), x, y);
        case 14:
            return FCmp(O(spv::Op::OpFUnordGreaterThanEqual), x, y);
        default:
            return true_id;
        }
    };
    const auto icmp = [&](u32 kind, bool sign) {
        const u32 x = SrcU(inst, 0);
        const u32 y = SrcU(inst, 1);
        switch (kind) {
        case 0:
            return false_id;
        case 1:
            return ICmp(sign ? O(spv::Op::OpSLessThan) : O(spv::Op::OpULessThan), x, y);
        case 2:
            return ICmp(O(spv::Op::OpIEqual), x, y);
        case 3:
            return ICmp(sign ? O(spv::Op::OpSLessThanEqual) : O(spv::Op::OpULessThanEqual), x, y);
        case 4:
            return ICmp(sign ? O(spv::Op::OpSGreaterThan) : O(spv::Op::OpUGreaterThan), x, y);
        case 5:
            return ICmp(O(spv::Op::OpINotEqual), x, y);
        case 6:
            return ICmp(sign ? O(spv::Op::OpSGreaterThanEqual) : O(spv::Op::OpUGreaterThanEqual), x, y);
        default:
            return true_id;
        }
    };
    if (op <= Op::V_CMP_TRU_F32) {
        result = fcmp(op);
    } else if (op >= Op::V_CMPX_F_F32 && op <= Op::V_CMPX_TRU_F32) {
        result = fcmp(op - Op::V_CMPX_F_F32);
        set_exec = true;
    } else if (op >= Op::V_CMP_F_I32 && op <= Op::V_CMP_T_I32) {
        result = icmp(op - Op::V_CMP_F_I32, true);
    } else if (op >= Op::V_CMPX_F_I32 && op <= Op::V_CMPX_T_I32) {
        result = icmp(op - Op::V_CMPX_F_I32, true);
        set_exec = true;
    } else if (op >= Op::V_CMP_F_U32 && op <= Op::V_CMP_T_U32) {
        result = icmp(op - Op::V_CMP_F_U32, false);
    } else if (op >= Op::V_CMPX_F_U32 && op <= Op::V_CMPX_T_U32) {
        result = icmp(op - Op::V_CMPX_F_U32, false);
        set_exec = true;
    } else {
        Fail("vopc " + std::to_string(op));
    }
    const u32 masked = And(set_exec ? Exec() : GuardCond(), result);
    if (set_exec) {
        s.r[ExecReg] = masked;
    }
    SetMask(inst.sdst, masked);
}

void Lift_::LiftVop(const Inst& inst) {
    const u16 op = inst.op;
    if (op < Op::Vop2Base) {
        return LiftVopc(inst);
    }
    const auto f = [&](u32 i) { return SrcF(inst, i); };
    const auto u = [&](u32 i) { return SrcU(inst, i); };
    const auto shamt = [&](u32 value) { return IOp(O(spv::Op::OpBitwiseAnd), value, CU(31)); };
    const auto carry_out = [&](u32 value) {
        if (inst.sdst != 0 || true) {
            SetMask(inst.sdst, And(Exec(), value));
        }
    };
    const auto carry_in = [&] { return BoolToU(Mask(inst.src[2], inst)); };
    const auto ext_f = [&](GLSLstd450 fn, u32 a) { return Ext(fn, t_f32, Ty::F32, {a}); };
    switch (op) {
    case Op::V_NOP:
        return;
    case Op::V_MOV_B32: {
        u32 value = SrcRaw(inst.src[0], inst);
        if (m.TyOf(value) == Ty::Bool) {
            value = BoolToU(value);
        }
        if (inst.abs || inst.neg || inst.omod || inst.clamp) {
            return SetDstF(inst, f(0));
        }
        return SetVgpr(inst.dst, value);
    }
    case Op::V_READFIRSTLANE_B32: {
        uses_broadcast_first = true;
        const u32 value = Emit(O(spv::Op::OpGroupNonUniformBroadcastFirst), t_u32, Ty::U32,
                               {CU(u32(spv::Scope::Subgroup)), u(0)});
        s.r[inst.dst] = value;
        return;
    }
    case Op::V_READLANE_B32: {
        const u32 lane = ToU(SrcRaw(inst.src[1], inst));
        if (!m.IsConst(lane) || inst.src[0] < SrcVgpr) {
            Fail("v_readlane with a computed lane");
        }
        s.r[inst.dst] = s.r[SlotFor(1, inst.src[0], m.ConstBits(lane))];
        return;
    }
    case Op::V_WRITELANE_B32: {
        const u32 lane = ToU(SrcRaw(inst.src[1], inst));
        if (!m.IsConst(lane)) {
            Fail("v_writelane with a computed lane");
        }
        s.r[SlotFor(1, inst.dst, m.ConstBits(lane))] = SrcRaw(inst.src[0], inst);
        return;
    }
    case Op::V_CNDMASK_B32: {
        const u32 cond = Mask(inst.src[2], inst);
        const u32 x = f(0);
        const u32 y = f(1);
        return SetDstF(inst, Select(cond, y, x));
    }
    case Op::V_ADD_F32:
        return SetDstF(inst, FAdd(f(0), f(1)));
    case Op::V_SUB_F32:
        return SetDstF(inst, FSub(f(0), f(1)));
    case Op::V_SUBREV_F32:
        return SetDstF(inst, FSub(f(1), f(0)));
    case Op::V_MUL_F32:
        return SetDstF(inst, FMul(f(0), f(1)));
    case Op::V_MUL_LEGACY_F32: {
        const u32 x = f(0);
        const u32 y = f(1);
        const u32 zero = CF(0.f);
        const u32 either = Or(FCmp(O(spv::Op::OpFOrdEqual), x, zero), FCmp(O(spv::Op::OpFOrdEqual), y, zero));
        return SetDstF(inst, Select(either, zero, FMul(x, y)));
    }
    case Op::V_MAC_F32:
    case Op::V_MAC_LEGACY_F32: {
        const u32 x = f(0);
        const u32 y = f(1);
        return SetDstF(inst, FAdd(FMul(x, y), VReadF(inst.dst - SrcVgpr)));
    }
    case Op::V_MADMK_F32: {
        const u32 x = f(0);
        const u32 y = f(1);
        return SetDstF(inst, FAdd(FMul(x, m.Const(Ty::F32, t_f32, inst.literal)), y));
    }
    case Op::V_MADAK_F32: {
        const u32 x = f(0);
        const u32 y = f(1);
        return SetDstF(inst, FFma(x, y, m.Const(Ty::F32, t_f32, inst.literal)));
    }
    case Op::V_MAD_F32:
    case Op::V_MAD_LEGACY_F32: {
        const u32 x = f(0);
        const u32 y = f(1);
        const u32 z = f(2);
        return SetDstF(inst, FAdd(FMul(x, y), z));
    }
    case Op::V_FMA_F32:
        return SetDstF(inst, FFma(f(0), f(1), f(2)));
    case Op::V_MIN_F32:
    case Op::V_MAX_F32: {
        const u32 x = f(0);
        const u32 y = f(1);
        return SetDstF(inst, Ext(op == Op::V_MIN_F32 ? GLSLstd450FMin : GLSLstd450FMax, t_f32, Ty::F32, {x, y}));
    }
    case Op::V_MIN_LEGACY_F32:
    case Op::V_MAX_LEGACY_F32: {
        const u32 x = f(0);
        const u32 y = f(1);
        const u32 value = Ext(op == Op::V_MIN_LEGACY_F32 ? GLSLstd450FMin : GLSLstd450FMax, t_f32, Ty::F32, {x, y});
        const u32 any_nan = Or(Emit(O(spv::Op::OpIsNan), t_bool, Ty::Bool, {x}),
                               Emit(O(spv::Op::OpIsNan), t_bool, Ty::Bool, {y}));
        return SetDstF(inst, Select(any_nan, y, value));
    }
    case Op::V_MIN_I32:
        return SetVgpr(inst.dst, Ext(GLSLstd450SMin, t_u32, Ty::U32, {u(0), u(1)}));
    case Op::V_MAX_I32:
        return SetVgpr(inst.dst, Ext(GLSLstd450SMax, t_u32, Ty::U32, {u(0), u(1)}));
    case Op::V_MIN_U32:
        return SetVgpr(inst.dst, Ext(GLSLstd450UMin, t_u32, Ty::U32, {u(0), u(1)}));
    case Op::V_MAX_U32:
        return SetVgpr(inst.dst, Ext(GLSLstd450UMax, t_u32, Ty::U32, {u(0), u(1)}));
    case Op::V_LSHR_B32:
        return SetVgpr(inst.dst, IOp(O(spv::Op::OpShiftRightLogical), u(0), shamt(u(1))));
    case Op::V_LSHRREV_B32:
        return SetVgpr(inst.dst, IOp(O(spv::Op::OpShiftRightLogical), u(1), shamt(u(0))));
    case Op::V_ASHR_I32:
        return SetVgpr(inst.dst, Emit(O(spv::Op::OpShiftRightArithmetic), t_u32, Ty::U32, {u(0), shamt(u(1))}));
    case Op::V_ASHRREV_I32:
        return SetVgpr(inst.dst, Emit(O(spv::Op::OpShiftRightArithmetic), t_u32, Ty::U32, {u(1), shamt(u(0))}));
    case Op::V_LSHL_B32:
        return SetVgpr(inst.dst, IOp(O(spv::Op::OpShiftLeftLogical), u(0), shamt(u(1))));
    case Op::V_LSHLREV_B32:
        return SetVgpr(inst.dst, IOp(O(spv::Op::OpShiftLeftLogical), u(1), shamt(u(0))));
    case Op::V_AND_B32:
        return SetVgpr(inst.dst, IOp(O(spv::Op::OpBitwiseAnd), u(0), u(1)));
    case Op::V_OR_B32:
        return SetVgpr(inst.dst, IOp(O(spv::Op::OpBitwiseOr), u(0), u(1)));
    case Op::V_XOR_B32:
        return SetVgpr(inst.dst, IOp(O(spv::Op::OpBitwiseXor), u(0), u(1)));
    case Op::V_NOT_B32:
        return SetVgpr(inst.dst, Emit(O(spv::Op::OpNot), t_u32, Ty::U32, {u(0)}));
    case Op::V_BFM_B32: {
        const u32 width = BfeU(u(0), CU(0), CU(5));
        const u32 offset = BfeU(u(1), CU(0), CU(5));
        const u32 ones = IOp(O(spv::Op::OpISub), IOp(O(spv::Op::OpShiftLeftLogical), CU(1), width), CU(1));
        return SetVgpr(inst.dst, IOp(O(spv::Op::OpShiftLeftLogical), ones, offset));
    }
    case Op::V_BCNT_U32_B32:
        return SetVgpr(inst.dst, IOp(O(spv::Op::OpIAdd), Emit(O(spv::Op::OpBitCount), t_u32, Ty::U32, {u(0)}), u(1)));
    case Op::V_MBCNT_LO_U32_B32:
    case Op::V_MBCNT_HI_U32_B32:
        if (inst.src[0] == 193) {
            if (op == Op::V_MBCNT_HI_U32_B32) {
                return SetVgpr(inst.dst, u(1));
            }
            uses_lane = true;
            return SetVgpr(inst.dst, IOp(O(spv::Op::OpIAdd), LoadBuiltinU32(var_lane_id, spv::BuiltIn::SubgroupLocalInvocationId), u(1)));
        }
        if (inst.src[0] != ExecReg && inst.src[0] != ExecReg + 1) {
            Fail("v_mbcnt of another mask");
        }
        return SetVgpr(inst.dst, u(1));
    case Op::V_MUL_I32_I24:
    case Op::V_MUL_U32_U24: {
        const bool sign = op == Op::V_MUL_I32_I24;
        const u32 x = sign ? BfeS(u(0), CU(0), CU(24)) : BfeU(u(0), CU(0), CU(24));
        const u32 y = sign ? BfeS(u(1), CU(0), CU(24)) : BfeU(u(1), CU(0), CU(24));
        return SetVgpr(inst.dst, IOp(O(spv::Op::OpIMul), x, y));
    }
    case Op::V_MAD_I32_I24:
    case Op::V_MAD_U32_U24: {
        const bool sign = op == Op::V_MAD_I32_I24;
        const u32 x = sign ? BfeS(u(0), CU(0), CU(24)) : BfeU(u(0), CU(0), CU(24));
        const u32 y = sign ? BfeS(u(1), CU(0), CU(24)) : BfeU(u(1), CU(0), CU(24));
        return SetVgpr(inst.dst, IOp(O(spv::Op::OpIAdd), IOp(O(spv::Op::OpIMul), x, y), u(2)));
    }
    case Op::V_ADD_I32: {
        const u32 x = u(0);
        const u32 y = u(1);
        const u32 r = IOp(O(spv::Op::OpIAdd), x, y);
        SetVgpr(inst.dst, r);
        carry_out(ICmp(O(spv::Op::OpULessThan), r, x));
        return;
    }
    case Op::V_SUB_I32: {
        const u32 x = u(0);
        const u32 y = u(1);
        SetVgpr(inst.dst, IOp(O(spv::Op::OpISub), x, y));
        carry_out(ICmp(O(spv::Op::OpUGreaterThan), y, x));
        return;
    }
    case Op::V_SUBREV_I32: {
        const u32 x = u(0);
        const u32 y = u(1);
        SetVgpr(inst.dst, IOp(O(spv::Op::OpISub), y, x));
        carry_out(ICmp(O(spv::Op::OpUGreaterThan), x, y));
        return;
    }
    case Op::V_ADDC_U32: {
        const u32 x = u(0);
        const u32 y = u(1);
        const u32 carry = carry_in();
        const u32 r1 = IOp(O(spv::Op::OpIAdd), x, y);
        const u32 r2 = IOp(O(spv::Op::OpIAdd), r1, carry);
        SetVgpr(inst.dst, r2);
        carry_out(Or(ICmp(O(spv::Op::OpULessThan), r1, x), ICmp(O(spv::Op::OpULessThan), r2, r1)));
        return;
    }
    case Op::V_SUBB_U32:
    case Op::V_SUBBREV_U32: {
        const bool rev = op == Op::V_SUBBREV_U32;
        const u32 x = rev ? u(1) : u(0);
        const u32 y = rev ? u(0) : u(1);
        const u32 carry = carry_in();
        const u32 diff = IOp(O(spv::Op::OpISub), x, y);
        SetVgpr(inst.dst, IOp(O(spv::Op::OpISub), diff, carry));
        carry_out(Or(ICmp(O(spv::Op::OpUGreaterThan), y, x), ICmp(O(spv::Op::OpUGreaterThan), carry, diff)));
        return;
    }
    case Op::V_LDEXP_F32:
        return SetDstF(inst, Ext(GLSLstd450Ldexp, t_f32, Ty::F32, {f(0), u(1)}));
    case Op::V_CVT_PKRTZ_F16_F32: {
        const u32 pair = Emit(O(spv::Op::OpCompositeConstruct), t_v2f, Ty::V2F, {f(0), f(1)});
        return SetVgpr(inst.dst, Ext(GLSLstd450PackHalf2x16, t_u32, Ty::U32, {pair}));
    }
    case Op::V_CUBEID_F32:
    case Op::V_CUBESC_F32:
    case Op::V_CUBETC_F32:
    case Op::V_CUBEMA_F32: {
        const u32 x = f(0);
        const u32 y = f(1);
        const u32 z = f(2);
        const u32 abs_x = FAbs(x);
        const u32 abs_y = FAbs(y);
        const u32 abs_z = FAbs(z);
        const u32 z_face = And(FCmp(O(spv::Op::OpFOrdGreaterThanEqual), abs_z, abs_x),
                               FCmp(O(spv::Op::OpFOrdGreaterThanEqual), abs_z, abs_y));
        const u32 y_face = FCmp(O(spv::Op::OpFOrdGreaterThanEqual), abs_y, abs_x);
        const auto pick = [&](u32 x_res, u32 y_res, u32 z_res) {
            return Select(z_face, z_res, Select(y_face, y_res, x_res));
        };
        const u32 zero = CF(0.f);
        if (op == Op::V_CUBEID_F32 && in.opt.native_cube) {
            m.Extension("SPV_AMD_gcn_shader");
            const u32 set = m.ExtInst("SPV_AMD_gcn_shader");
            const u32 vec = ComponentsF({x, y, z});
            const u32 id = m.NewId(Ty::F32);
            Node node;
            node.op = O(spv::Op::OpExtInst);
            node.type = t_f32;
            node.result = id;
            node.nargs = 3;
            node.first = u32(args.size());
            node.lits = 2;
            args.insert(args.end(), {set, 1, vec});
            const u32 index = u32(nodes.size());
            nodes.push_back(node);
            m.BindNode(id, index);
            out_blocks[cur].nodes.push_back(index);
            return SetDstF(inst, id);
        }
        if (op == Op::V_CUBEID_F32) {
            const u32 fx = Select(FCmp(O(spv::Op::OpFOrdLessThan), x, zero), CF(1.f), CF(0.f));
            const u32 fy = Select(FCmp(O(spv::Op::OpFOrdLessThan), y, zero), CF(3.f), CF(2.f));
            const u32 fz = Select(FCmp(O(spv::Op::OpFOrdLessThan), z, zero), CF(5.f), CF(4.f));
            return SetDstF(inst, pick(fx, fy, fz));
        }
        if (op == Op::V_CUBESC_F32) {
            const u32 sx = Select(FCmp(O(spv::Op::OpFOrdLessThan), x, zero), z, FNeg(z));
            const u32 sz = Select(FCmp(O(spv::Op::OpFOrdLessThan), z, zero), FNeg(x), x);
            return SetDstF(inst, pick(sx, x, sz));
        }
        if (op == Op::V_CUBETC_F32) {
            const u32 neg_y = FNeg(y);
            const u32 ty = Select(FCmp(O(spv::Op::OpFOrdLessThan), y, zero), FNeg(z), z);
            return SetDstF(inst, pick(neg_y, ty, neg_y));
        }
        const u32 two = CF(2.f);
        return SetDstF(inst, pick(FMul(x, two), FMul(y, two), FMul(z, two)));
    }
    case Op::V_BFE_U32:
    case Op::V_BFE_I32: {
        const u32 base = u(0);
        const u32 offset = shamt(u(1));
        const u32 count = shamt(u(2));
        return SetVgpr(inst.dst, op == Op::V_BFE_I32 ? BfeS(base, offset, count) : BfeU(base, offset, count));
    }
    case Op::V_BFI_B32: {
        const u32 sel = u(0);
        const u32 x = IOp(O(spv::Op::OpBitwiseAnd), sel, u(1));
        const u32 y = IOp(O(spv::Op::OpBitwiseAnd), Emit(O(spv::Op::OpNot), t_u32, Ty::U32, {sel}), u(2));
        return SetVgpr(inst.dst, IOp(O(spv::Op::OpBitwiseOr), x, y));
    }
    case Op::V_MIN3_F32:
    case Op::V_MAX3_F32: {
        const GLSLstd450 fn = op == Op::V_MIN3_F32 ? GLSLstd450FMin : GLSLstd450FMax;
        const u32 x = f(0);
        const u32 y = f(1);
        const u32 z = f(2);
        return SetDstF(inst, Ext(fn, t_f32, Ty::F32, {x, Ext(fn, t_f32, Ty::F32, {y, z})}));
    }
    case Op::V_MED3_F32: {
        const u32 x = f(0);
        const u32 y = f(1);
        const u32 z = f(2);
        const u32 mmx = Ext(GLSLstd450FMin, t_f32, Ty::F32, {Ext(GLSLstd450FMax, t_f32, Ty::F32, {x, y}), z});
        return SetDstF(inst, Ext(GLSLstd450FMax, t_f32, Ty::F32, {Ext(GLSLstd450FMin, t_f32, Ty::F32, {x, y}), mmx}));
    }
    case Op::V_MIN3_I32:
    case Op::V_MAX3_I32:
    case Op::V_MIN3_U32:
    case Op::V_MAX3_U32: {
        const GLSLstd450 fn = op == Op::V_MIN3_I32   ? GLSLstd450SMin
                              : op == Op::V_MAX3_I32 ? GLSLstd450SMax
                              : op == Op::V_MIN3_U32 ? GLSLstd450UMin
                                                     : GLSLstd450UMax;
        const u32 x = u(0);
        const u32 y = u(1);
        const u32 z = u(2);
        return SetVgpr(inst.dst, Ext(fn, t_u32, Ty::U32, {x, Ext(fn, t_u32, Ty::U32, {y, z})}));
    }
    case Op::V_LSHL_B64: {
        const u32 base = inst.src[0];
        const u32 lo = ToU(SrcRaw(base, inst));
        const u32 hi = base < 128 || base >= SrcVgpr ? ToU(s.r[base + 1]) : CU(0);
        const u32 amount = IOp(O(spv::Op::OpBitwiseAnd), u(1), CU(63));
        if (!t_u64) {
            t_u64 = m.AddType(O(spv::Op::OpTypeInt), {64, 0});
            m.Capability(u32(spv::Capability::Int64));
        }
        const u32 pair = Emit(O(spv::Op::OpCompositeConstruct), t_v2u, Ty::V2U, {lo, hi});
        const u32 wide = Emit(O(spv::Op::OpBitcast), t_u64, Ty::U64, {pair});
        const u32 shifted = Emit(O(spv::Op::OpShiftLeftLogical), t_u64, Ty::U64, {wide, amount});
        const u32 split = Emit(O(spv::Op::OpBitcast), t_v2u, Ty::V2U, {shifted});
        SetVgpr(inst.dst, Extract(t_u32, Ty::U32, split, 0));
        SetVgpr(inst.dst + 1, Extract(t_u32, Ty::U32, split, 1));
        return;
    }
    case Op::V_MUL_LO_U32:
    case Op::V_MUL_LO_I32:
        return SetVgpr(inst.dst, IOp(O(spv::Op::OpIMul), u(0), u(1)));
    case Op::V_MUL_HI_U32:
    case Op::V_MUL_HI_I32: {
        const u32 full = Emit(op == Op::V_MUL_HI_U32 ? O(spv::Op::OpUMulExtended) : O(spv::Op::OpSMulExtended),
                              m.AddType(O(spv::Op::OpTypeStruct), {t_u32, t_u32}), Ty::None, {u(0), u(1)});
        return SetVgpr(inst.dst, Extract(t_u32, Ty::U32, full, 1));
    }
    case Op::V_CVT_F32_I32:
        return SetDstF(inst, Emit(O(spv::Op::OpConvertSToF), t_f32, Ty::F32, {u(0)}));
    case Op::V_CVT_F32_U32:
        return SetDstF(inst, Emit(O(spv::Op::OpConvertUToF), t_f32, Ty::F32, {u(0)}));
    case Op::V_CVT_U32_F32:
        return SetVgpr(inst.dst, Emit(O(spv::Op::OpConvertFToU), t_u32, Ty::U32, {f(0)}));
    case Op::V_CVT_I32_F32:
        return SetVgpr(inst.dst, Emit(O(spv::Op::OpConvertFToS), t_u32, Ty::U32, {f(0)}));
    case Op::V_CVT_F32_F16: {
        const u32 pair = Ext(GLSLstd450UnpackHalf2x16, t_v2f, Ty::V2F, {u(0)});
        return SetDstF(inst, Extract(t_f32, Ty::F32, pair, 0));
    }
    case Op::V_FRACT_F32:
        return SetDstF(inst, ext_f(GLSLstd450Fract, f(0)));
    case Op::V_TRUNC_F32:
        return SetDstF(inst, ext_f(GLSLstd450Trunc, f(0)));
    case Op::V_CEIL_F32:
        return SetDstF(inst, ext_f(GLSLstd450Ceil, f(0)));
    case Op::V_RNDNE_F32:
        return SetDstF(inst, ext_f(GLSLstd450RoundEven, f(0)));
    case Op::V_FLOOR_F32:
        return SetDstF(inst, ext_f(GLSLstd450Floor, f(0)));
    case Op::V_EXP_F32:
        return SetDstF(inst, ext_f(GLSLstd450Exp2, f(0)));
    case Op::V_LOG_F32:
    case Op::V_LOG_CLAMP_F32:
        return SetDstF(inst, ext_f(GLSLstd450Log2, f(0)));
    case Op::V_RCP_F32:
    case Op::V_RCP_IFLAG_F32:
    case Op::V_RCP_CLAMP_F32:
        return SetDstF(inst, Emit(O(spv::Op::OpFDiv), t_f32, Ty::F32, {CF(1.f), f(0)}));
    case Op::V_RSQ_F32:
    case Op::V_RSQ_LEGACY_F32:
    case Op::V_RSQ_CLAMP_F32:
        return SetDstF(inst, ext_f(GLSLstd450InverseSqrt, f(0)));
    case Op::V_SQRT_F32:
        return SetDstF(inst, ext_f(GLSLstd450Sqrt, f(0)));
    case Op::V_SIN_F32:
        return SetDstF(inst, ext_f(GLSLstd450Sin, Emit(O(spv::Op::OpFMul), t_f32, Ty::F32, {CF(6.2831853f), f(0)})));
    case Op::V_COS_F32:
        return SetDstF(inst, ext_f(GLSLstd450Cos, Emit(O(spv::Op::OpFMul), t_f32, Ty::F32, {CF(6.2831853f), f(0)})));
    default:
        Fail("vop " + std::to_string(op));
    }
}

} // namespace Lifter
