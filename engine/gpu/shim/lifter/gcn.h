// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace Lifter {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;

enum class Enc : u8 { Sop1, Sop2, Sopk, Sopc, Sopp, Smrd, Vop, Vintrp, Ds, Mubuf, Mtbuf, Mimg, Exp };

constexpr u32 SrcVcc = 106;
constexpr u32 SrcM0 = 124;
constexpr u32 SrcExec = 126;
constexpr u32 SrcZero = 128;
constexpr u32 SrcScc = 253;
constexpr u32 SrcLiteral = 255;
constexpr u32 SrcVgpr = 256;

struct Inst {
    Enc enc{};
    u16 op{};
    u32 pc{};
    u32 next{};
    u16 src[4]{};
    u16 dst{};
    u16 sdst{};
    u32 literal{};
    u8 abs{};
    u8 neg{};
    u8 clamp{};
    u8 omod{};
    bool e64{};
    s32 simm{};
    u32 offset{};
    u8 offset1{};
    bool offen{};
    bool idxen{};
    bool glc{};
    bool slc{};
    bool gds{};
    bool imm{};
    u8 dfmt{};
    u8 nfmt{};
    u8 dmask{};
    bool unorm{};
    bool da{};
    bool r128{};
    u8 chan{};
    u8 attr{};
    u8 en{};
    u8 target{};
    bool compr{};
    bool done{};
    bool vm{};
};

namespace Op {
constexpr u16 SOP2_S_ADD_U32 = 0, SOP2_S_SUB_U32 = 1, SOP2_S_ADD_I32 = 2, SOP2_S_SUB_I32 = 3,
              SOP2_S_ADDC_U32 = 4, SOP2_S_SUBB_U32 = 5, SOP2_S_MIN_I32 = 6, SOP2_S_MIN_U32 = 7,
              SOP2_S_MAX_I32 = 8, SOP2_S_MAX_U32 = 9, SOP2_S_CSELECT_B32 = 10,
              SOP2_S_CSELECT_B64 = 11, SOP2_S_AND_B32 = 14, SOP2_S_AND_B64 = 15, SOP2_S_OR_B32 = 16,
              SOP2_S_OR_B64 = 17, SOP2_S_XOR_B32 = 18, SOP2_S_XOR_B64 = 19, SOP2_S_ANDN2_B32 = 20,
              SOP2_S_ANDN2_B64 = 21, SOP2_S_ORN2_B32 = 22, SOP2_S_ORN2_B64 = 23,
              SOP2_S_NAND_B32 = 24, SOP2_S_NAND_B64 = 25, SOP2_S_NOR_B32 = 26, SOP2_S_NOR_B64 = 27,
              SOP2_S_XNOR_B32 = 28, SOP2_S_XNOR_B64 = 29, SOP2_S_LSHL_B32 = 30,
              SOP2_S_LSHL_B64 = 31, SOP2_S_LSHR_B32 = 32, SOP2_S_LSHR_B64 = 33,
              SOP2_S_ASHR_I32 = 34, SOP2_S_ASHR_I64 = 35, SOP2_S_BFM_B32 = 36, SOP2_S_MUL_I32 = 38,
              SOP2_S_BFE_U32 = 39, SOP2_S_BFE_I32 = 40;
constexpr u16 SOP1_S_MOV_B32 = 3, SOP1_S_MOV_B64 = 4, SOP1_S_CMOV_B32 = 5, SOP1_S_CMOV_B64 = 6,
              SOP1_S_NOT_B32 = 7, SOP1_S_NOT_B64 = 8, SOP1_S_WQM_B32 = 9, SOP1_S_WQM_B64 = 10,
              SOP1_S_BREV_B32 = 11, SOP1_S_BCNT1_I32_B32 = 15, SOP1_S_BCNT1_I32_B64 = 16,
              SOP1_S_SWAPPC_B64 = 33, SOP1_S_AND_SAVEEXEC_B64 = 36, SOP1_S_OR_SAVEEXEC_B64 = 37,
              SOP1_S_XOR_SAVEEXEC_B64 = 38, SOP1_S_ANDN2_SAVEEXEC_B64 = 39,
              SOP1_S_ORN2_SAVEEXEC_B64 = 40, SOP1_S_NAND_SAVEEXEC_B64 = 41,
              SOP1_S_NOR_SAVEEXEC_B64 = 42, SOP1_S_XNOR_SAVEEXEC_B64 = 43;
constexpr u16 SOPK_S_MOVK_I32 = 0, SOPK_S_CMOVK_I32 = 2, SOPK_S_CMPK_EQ_I32 = 3,
              SOPK_S_ADDK_I32 = 15, SOPK_S_MULK_I32 = 16;
constexpr u16 SOPC_S_CMP_EQ_I32 = 0, SOPC_S_CMP_LG_I32 = 1, SOPC_S_CMP_GT_I32 = 2,
              SOPC_S_CMP_GE_I32 = 3, SOPC_S_CMP_LT_I32 = 4, SOPC_S_CMP_LE_I32 = 5,
              SOPC_S_CMP_EQ_U32 = 6, SOPC_S_CMP_LG_U32 = 7, SOPC_S_CMP_GT_U32 = 8,
              SOPC_S_CMP_GE_U32 = 9, SOPC_S_CMP_LT_U32 = 10, SOPC_S_CMP_LE_U32 = 11;
constexpr u16 SOPP_S_NOP = 0, SOPP_S_ENDPGM = 1, SOPP_S_BRANCH = 2, SOPP_S_CBRANCH_SCC0 = 4,
              SOPP_S_CBRANCH_SCC1 = 5, SOPP_S_CBRANCH_VCCZ = 6, SOPP_S_CBRANCH_VCCNZ = 7,
              SOPP_S_CBRANCH_EXECZ = 8, SOPP_S_CBRANCH_EXECNZ = 9, SOPP_S_BARRIER = 10,
              SOPP_S_WAITCNT = 12, SOPP_S_SENDMSG = 16;
constexpr u16 SMRD_S_LOAD_DWORD = 0, SMRD_S_LOAD_DWORDX2 = 1, SMRD_S_LOAD_DWORDX4 = 2,
              SMRD_S_LOAD_DWORDX8 = 3, SMRD_S_LOAD_DWORDX16 = 4, SMRD_S_BUFFER_LOAD_DWORD = 8,
              SMRD_S_BUFFER_LOAD_DWORDX2 = 9, SMRD_S_BUFFER_LOAD_DWORDX4 = 10,
              SMRD_S_BUFFER_LOAD_DWORDX8 = 11, SMRD_S_BUFFER_LOAD_DWORDX16 = 12;

constexpr u16 Vop2Base = 256, Vop1Base = 384;
constexpr u16 V_CMP_F_F32 = 0, V_CMP_LT_F32 = 1, V_CMP_EQ_F32 = 2, V_CMP_LE_F32 = 3,
              V_CMP_GT_F32 = 4, V_CMP_LG_F32 = 5, V_CMP_GE_F32 = 6, V_CMP_O_F32 = 7,
              V_CMP_U_F32 = 8, V_CMP_NGE_F32 = 9, V_CMP_NLG_F32 = 10, V_CMP_NGT_F32 = 11,
              V_CMP_NLE_F32 = 12, V_CMP_NEQ_F32 = 13, V_CMP_NLT_F32 = 14, V_CMP_TRU_F32 = 15;
constexpr u16 V_CMPX_F_F32 = 16, V_CMPX_TRU_F32 = 31;
constexpr u16 V_CMP_F_I32 = 128, V_CMP_LT_I32 = 129, V_CMP_EQ_I32 = 130, V_CMP_LE_I32 = 131,
              V_CMP_GT_I32 = 132, V_CMP_NE_I32 = 133, V_CMP_GE_I32 = 134, V_CMP_T_I32 = 135;
constexpr u16 V_CMPX_F_I32 = 144, V_CMPX_T_I32 = 151;
constexpr u16 V_CMP_F_U32 = 192, V_CMP_LT_U32 = 193, V_CMP_EQ_U32 = 194, V_CMP_LE_U32 = 195,
              V_CMP_GT_U32 = 196, V_CMP_NE_U32 = 197, V_CMP_GE_U32 = 198, V_CMP_T_U32 = 199;
constexpr u16 V_CMPX_F_U32 = 208, V_CMPX_T_U32 = 215;
constexpr u16 V_CNDMASK_B32 = 256, V_READLANE_B32 = 257, V_WRITELANE_B32 = 258, V_ADD_F32 = 259,
              V_SUB_F32 = 260, V_SUBREV_F32 = 261, V_MAC_LEGACY_F32 = 262, V_MUL_LEGACY_F32 = 263,
              V_MUL_F32 = 264, V_MUL_I32_I24 = 265, V_MUL_U32_U24 = 267, V_MIN_LEGACY_F32 = 269,
              V_MAX_LEGACY_F32 = 270, V_MIN_F32 = 271, V_MAX_F32 = 272, V_MIN_I32 = 273,
              V_MAX_I32 = 274, V_MIN_U32 = 275, V_MAX_U32 = 276, V_LSHR_B32 = 277,
              V_LSHRREV_B32 = 278, V_ASHR_I32 = 279, V_ASHRREV_I32 = 280, V_LSHL_B32 = 281,
              V_LSHLREV_B32 = 282, V_AND_B32 = 283, V_OR_B32 = 284, V_XOR_B32 = 285,
              V_BFM_B32 = 286, V_MAC_F32 = 287, V_MADMK_F32 = 288, V_MADAK_F32 = 289,
              V_BCNT_U32_B32 = 290, V_MBCNT_LO_U32_B32 = 291, V_MBCNT_HI_U32_B32 = 292,
              V_ADD_I32 = 293, V_SUB_I32 = 294, V_SUBREV_I32 = 295, V_ADDC_U32 = 296,
              V_SUBB_U32 = 297, V_SUBBREV_U32 = 298, V_LDEXP_F32 = 299, V_CVT_PKRTZ_F16_F32 = 303;
constexpr u16 V_MAD_LEGACY_F32 = 320, V_MAD_F32 = 321, V_MAD_I32_I24 = 322, V_MAD_U32_U24 = 323,
              V_CUBEID_F32 = 324, V_CUBESC_F32 = 325, V_CUBETC_F32 = 326, V_CUBEMA_F32 = 327,
              V_BFE_U32 = 328, V_BFE_I32 = 329, V_BFI_B32 = 330, V_FMA_F32 = 331, V_MIN3_F32 = 337,
              V_MIN3_I32 = 338, V_MIN3_U32 = 339, V_MAX3_F32 = 340, V_MAX3_I32 = 341,
              V_MAX3_U32 = 342, V_MED3_F32 = 343, V_MED3_I32 = 344, V_MED3_U32 = 345,
              V_LSHL_B64 = 353, V_LSHR_B64 = 354, V_ASHR_I64 = 355, V_MUL_LO_U32 = 361,
              V_MUL_HI_U32 = 362, V_MUL_LO_I32 = 363, V_MUL_HI_I32 = 364;
constexpr u16 V_NOP = 384, V_MOV_B32 = 385, V_READFIRSTLANE_B32 = 386, V_CVT_F32_I32 = 389,
              V_CVT_F32_U32 = 390, V_CVT_U32_F32 = 391, V_CVT_I32_F32 = 392, V_CVT_F32_F16 = 395,
              V_CVT_F16_F32 = 394, V_FRACT_F32 = 416, V_TRUNC_F32 = 417, V_CEIL_F32 = 418,
              V_RNDNE_F32 = 419, V_FLOOR_F32 = 420, V_EXP_F32 = 421, V_LOG_CLAMP_F32 = 422,
              V_LOG_F32 = 423, V_RCP_CLAMP_F32 = 424, V_RCP_LEGACY_F32 = 425, V_RCP_F32 = 426,
              V_RCP_IFLAG_F32 = 427, V_RSQ_CLAMP_F32 = 428, V_RSQ_LEGACY_F32 = 429, V_RSQ_F32 = 430,
              V_SQRT_F32 = 435, V_SIN_F32 = 437, V_COS_F32 = 438, V_NOT_B32 = 439;
constexpr u16 VINTRP_P1 = 0, VINTRP_P2 = 1, VINTRP_MOV = 2;
constexpr u16 DS_WRITE_B32 = 13, DS_WRITE2_B32 = 14, DS_SWIZZLE_B32 = 53, DS_READ_B32 = 54,
              DS_READ2_B32 = 55, DS_CONSUME = 61, DS_APPEND = 62;
constexpr u16 MUBUF_LOAD_FORMAT_X = 0, MUBUF_LOAD_FORMAT_XY = 1, MUBUF_LOAD_FORMAT_XYZ = 2,
              MUBUF_LOAD_FORMAT_XYZW = 3, MUBUF_STORE_FORMAT_X = 4, MUBUF_STORE_FORMAT_XY = 5,
              MUBUF_STORE_FORMAT_XYZ = 6, MUBUF_STORE_FORMAT_XYZW = 7, MUBUF_LOAD_DWORD = 12,
              MUBUF_LOAD_DWORDX2 = 13, MUBUF_LOAD_DWORDX4 = 14, MUBUF_LOAD_DWORDX3 = 15,
              MUBUF_STORE_DWORD = 28, MUBUF_STORE_DWORDX2 = 29, MUBUF_STORE_DWORDX4 = 30,
              MUBUF_STORE_DWORDX3 = 31, MUBUF_ATOMIC_ADD = 50;
constexpr u16 MIMG_LOAD = 0, MIMG_LOAD_MIP = 1, MIMG_STORE = 8, MIMG_STORE_MIP = 9,
              MIMG_GET_RESINFO = 14, MIMG_SAMPLE = 32, MIMG_SAMPLE_CL = 33, MIMG_SAMPLE_D = 34,
              MIMG_SAMPLE_D_CL = 35, MIMG_SAMPLE_L = 36, MIMG_SAMPLE_B = 37, MIMG_SAMPLE_B_CL = 38,
              MIMG_SAMPLE_LZ = 39, MIMG_SAMPLE_C = 40, MIMG_SAMPLE_C_CL = 41, MIMG_SAMPLE_C_D = 42,
              MIMG_SAMPLE_C_D_CL = 43, MIMG_SAMPLE_C_L = 44, MIMG_SAMPLE_C_B = 45,
              MIMG_SAMPLE_C_B_CL = 46, MIMG_SAMPLE_C_LZ = 47, MIMG_SAMPLE_O = 48,
              MIMG_SAMPLE_LZ_O = 55, MIMG_SAMPLE_C_LZ_O = 63, MIMG_GATHER4 = 64,
              MIMG_GATHER4_LZ = 71, MIMG_GATHER4_C_LZ = 79, MIMG_GATHER4_LZ_O = 87,
              MIMG_GATHER4_C_LZ_O = 95, MIMG_GET_LOD = 96;
} // namespace Op

struct DecodeResult {
    std::vector<Inst> insts;
    u32 bad_at = 0xffffffff;
};

DecodeResult Decode(std::span<const u32> code);
bool Is64BitDst(const Inst& inst);

} // namespace Lifter
