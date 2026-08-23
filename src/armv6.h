// ARMv6-M Thumb-1 instruction encoders/decoders used by the JIT.

#ifndef JIT_ARMV6M_ARMV6_H_
#define JIT_ARMV6M_ARMV6_H_

#include <cstdint>
#include <cstddef>
#include <cassert>

struct ArmV6M
{
    struct LoReg
    {
        uint16_t idx;

        inline LoReg() = default;
        inline LoReg(const LoReg&) = default;

        explicit inline LoReg(uint16_t idx): idx(idx)
        {
            assert(idx < 8); // GCOV_EXCL_LINE
        }
    };

    struct AnyReg
    {
        uint16_t idx;

        inline AnyReg() = default;
        inline AnyReg(const AnyReg&) = default;

        inline AnyReg(const LoReg& lo): idx(lo.idx)
        {
        }

        explicit inline AnyReg(uint16_t idx): idx(idx)
        {
            assert(idx < 16); // GCOV_EXCL_LINE
        }
    };

    template<size_t n>
    struct Imm
    {
        uint16_t v;

        inline Imm() = default;
        inline Imm(const Imm&) = default;

        inline Imm(uint16_t v): v(v)
        {
            assert(v < (1 << n)); // GCOV_EXCL_LINE
        }
    };

    template<size_t a, size_t n>
    struct Uoff
    {
        uint16_t v;

        inline Uoff() = default;
        inline Uoff(const Uoff&) = default;

        Uoff(uint16_t v): v(v >> a)
        {
            assert((v & ~(-1 << a)) == 0); // GCOV_EXCL_LINE
            assert(v < (1 << (n + a)));    // GCOV_EXCL_LINE
        }
    };

    template<size_t a, size_t n>
    struct Ioff
    {
        static constexpr auto minValue = -(1 << (n + a - 1));
        static constexpr auto maxValue = (1 << (n + a - 1)) - 1;

        uint16_t v;

        static inline bool isInRange(int16_t v)
        {
            return minValue <= v && v <= maxValue;
        }

        inline Ioff() = default;
        inline Ioff(const Ioff&) = default;

        Ioff(int16_t v): v((v >> a) & ~(-1 << n))
        {
            assert((v & ~(-1 << a)) == 0); // GCOV_EXCL_LINE
            assert(isInRange(v >> a));     // GCOV_EXCL_LINE
        }
    };

    struct LoRegs
    {
        uint16_t flags;

        inline LoRegs& add(LoReg r)
        {
            flags |= 1 << r.idx;
            return *this;
        }

        inline LoRegs add(LoReg r) const
        {
            LoRegs ret = *this;
            ret.add(r);
            return ret;
        }
    };

    enum class Reg2Op: uint16_t
    {
        //            opcode  m   dn
        AND   = 0b0100000000'000'000,
        EOR   = 0b0100000001'000'000,
        LSL   = 0b0100000010'000'000,
        LSR   = 0b0100000011'000'000,
        ASR   = 0b0100000100'000'000,
        ADC   = 0b0100000101'000'000,
        SBC   = 0b0100000110'000'000,
        ROR   = 0b0100000111'000'000,
        TST   = 0b0100001000'000'000,
        RSB   = 0b0100001001'000'000,
        CMP   = 0b0100001010'000'000,
        CMN   = 0b0100001011'000'000,
        ORR   = 0b0100001100'000'000,
        MUL   = 0b0100001101'000'000,
        BIC   = 0b0100001110'000'000,
        MVN   = 0b0100001111'000'000,
        SXH   = 0b1011001000'000'000,
        SXB   = 0b1011001001'000'000,
        UXH   = 0b1011001010'000'000,
        UXB   = 0b1011001011'000'000,
        REV   = 0b1011101000'000'000,
        REV16 = 0b1011101001'000'000,
        REVSH = 0b1011101011'000'000,
    };

    static constexpr inline uint16_t fmtReg2(Reg2Op op, uint16_t dn, uint16_t m)
    {
        return (uint16_t)op | (m << 3) | dn;
    }

    enum class Reg3Op: uint16_t
    {
        //         opcode  m   n  d/t
        ADDREG = 0b0001100'000'000'000,
        SUBREG = 0b0001101'000'000'000,
        ADDIMM = 0b0001110'000'000'000,
        SUBIMM = 0b0001111'000'000'000,
        STR    = 0b0101000'000'000'000,
        STRH   = 0b0101001'000'000'000,
        STRB   = 0b0101010'000'000'000,
        LDRSB  = 0b0101011'000'000'000,
        LDR    = 0b0101100'000'000'000,
        LDRH   = 0b0101101'000'000'000,
        LDRB   = 0b0101110'000'000'000,
        LDRSH  = 0b0101111'000'000'000,
    };

    static inline uint16_t fmtReg3(Reg3Op op, uint16_t dt, uint16_t n, uint16_t m)
    {
        return (uint16_t)op | (m << 6) | (n << 3) | dt;
    }

    enum class Imm5Op: uint16_t
    {
        //      opcode  imm5 m/n d/t
        LSL  = 0b00000'00000'000'000,
        LSR  = 0b00001'00000'000'000,
        ASR  = 0b00010'00000'000'000,
        STR  = 0b01100'00000'000'000,
        LDR  = 0b01101'00000'000'000,
        STRB = 0b01110'00000'000'000,
        LDRB = 0b01111'00000'000'000,
        STRH = 0b10000'00000'000'000,
        LDRH = 0b10001'00000'000'000,
    };

    static inline uint16_t fmtImm5(Imm5Op op, uint16_t dt, uint16_t mn, uint16_t imm5)
    {
        return (uint16_t)op | (imm5 << 6) | (mn << 3) | dt;
    }

    enum class Imm7Op: uint16_t
    {
        //            opcode   imm7
        INCRSP = 0b101100000'0000000,
        DECRSP = 0b101100001'0000000,
    };

    static inline uint16_t fmtImm7(Imm7Op op, uint16_t imm7)
    {
        return (uint16_t)op | imm7;
    }

    enum class Imm8Op: uint16_t
    {
        //       opcode d/n   imm8
        MOV   = 0b00100'000'00000000,
        CMP   = 0b00101'000'00000000,
        ADD   = 0b00110'000'00000000,
        SUB   = 0b00111'000'00000000,
        STRSP = 0b10010'000'00000000,
        LDRSP = 0b10011'000'00000000,
        LDR   = 0b01001'000'00000000,
        ADR   = 0b10100'000'00000000,
        ADDSP = 0b10101'000'00000000,
    };

    static inline uint16_t fmtImm8(Imm8Op op, uint16_t r, uint16_t imm8)
    {
        return (uint16_t) op | (r << 8) | imm8;
    }

    enum class NoArgOp: uint16_t
    {
        CPSIE = 0b1011'0110'0110'0010,
        CPSID = 0b1011'0110'0111'0010,
        NOP   = 0b1011'1111'0000'0000,
        YIELD = 0b1011'1111'0001'0000,
        WFE   = 0b1011'1111'0010'0000,
        WFI   = 0b1011'1111'0011'0000,
        SEV   = 0b1011'1111'0100'0000,
    };

    static inline uint16_t fmtNoArg(NoArgOp op)
    {
        return (uint16_t)op;
    }

    enum class BranchOp
    {
        EQ  = 0b1101'0000'00000000, // Equal                      Z == 1
        NE  = 0b1101'0001'00000000, // Not equal                  Z == 0
        HS  = 0b1101'0010'00000000, // Unsigned higher or same    C == 1
        LO  = 0b1101'0011'00000000, // Unsigned lower             C == 0
        MI  = 0b1101'0100'00000000, // Minus, negative            N == 1
        PL  = 0b1101'0101'00000000, // Plus, positive             N == 0
        VS  = 0b1101'0110'00000000, // Overflow                   V == 1
        VC  = 0b1101'0111'00000000, // No overflow                V == 0
        HI  = 0b1101'1000'00000000, // Unsigned higher            C == 1 && Z == 0
        LS  = 0b1101'1001'00000000, // Unsigned lower or same     C == 0 && Z == 1
        GE  = 0b1101'1010'00000000, // Signed greater or equal    N == V
        LT  = 0b1101'1011'00000000, // Signed less than           N != V
        GT  = 0b1101'1100'00000000, // Signed greater than        Z == 0 && N == V
        LE  = 0b1101'1101'00000000, // Signed less than or equal  Z == 1 || N != V
        UDF = 0b1101'1110'00000000, // Permanently undefined
        SVC = 0b1101'1111'00000000, // Supervisor call
    };

    static inline uint16_t fmtBranchSvc(BranchOp op, uint16_t imm8)
    {
        return (uint16_t)op | imm8;
    }

    enum class HiRegOp: uint16_t
    {
        ADD = 0b01000100'00000000,
        CMP = 0b01000101'00000000,
        MOV = 0b01000110'00000000,
        JMP = 0b01000111'00000000,
    };

    static inline uint16_t fmtHiReg(HiRegOp op, uint16_t dn, uint16_t m)
    {
        return (uint16_t)op | (((uint16_t)dn >> 3) << 7) | m << 3 | (dn & 0b0111);
    }

    static inline uint16_t fmtPushPop(bool pop, bool includeExtra, uint16_t regFlags)
    {
        return 0b1011'0'10'0'00000000 | ((uint16_t)pop << 11) | ((uint16_t)includeExtra << 8) | regFlags;
    }

    static inline uint16_t lsMia(bool load, uint16_t n, uint16_t regFlags)
    {
        return 0b11000'00000000000 | ((uint16_t)load << 11) | ((uint16_t)n << 8) | regFlags;
    }

    static inline uint16_t ands(LoReg dn, LoReg m)
    {
        return fmtReg2(Reg2Op::AND, dn.idx, m.idx);
    }

    static inline uint16_t eors(LoReg dn, LoReg m)
    {
        return fmtReg2(Reg2Op::EOR, dn.idx, m.idx);
    }

    static inline uint16_t lsls(LoReg dn, LoReg m)
    {
        return fmtReg2(Reg2Op::LSL, dn.idx, m.idx);
    }

    static inline uint16_t lsrs(LoReg dn, LoReg m)
    {
        return fmtReg2(Reg2Op::LSR, dn.idx, m.idx);
    }

    static inline uint16_t asrs(LoReg dn, LoReg m)
    {
        return fmtReg2(Reg2Op::ASR, dn.idx, m.idx);
    }

    static inline uint16_t adcs(LoReg dn, LoReg m)
    {
        return fmtReg2(Reg2Op::ADC, dn.idx, m.idx);
    }

    static inline uint16_t sbcs(LoReg dn, LoReg m)
    {
        return fmtReg2(Reg2Op::SBC, dn.idx, m.idx);
    }

    static inline uint16_t rors(LoReg dn, LoReg m)
    {
        return fmtReg2(Reg2Op::ROR, dn.idx, m.idx);
    }

    static inline uint16_t tst(LoReg n, LoReg m)
    {
        return fmtReg2(Reg2Op::TST, n.idx, m.idx);
    }

    static inline uint16_t negs(LoReg d, LoReg m)
    {
        return fmtReg2(Reg2Op::RSB, d.idx, m.idx);
    }

    static inline uint16_t cmp(LoReg n, LoReg m)
    {
        return fmtReg2(Reg2Op::CMP, n.idx, m.idx);
    }

    static inline uint16_t cmn(LoReg n, LoReg m)
    {
        return fmtReg2(Reg2Op::CMN, n.idx, m.idx);
    }

    static inline uint16_t orrs(LoReg dn, LoReg m)
    {
        return fmtReg2(Reg2Op::ORR, dn.idx, m.idx);
    }

    static inline uint16_t muls(LoReg dn, LoReg m)
    {
        return fmtReg2(Reg2Op::MUL, dn.idx, m.idx);
    }

    static inline uint16_t bics(LoReg dn, LoReg m)
    {
        return fmtReg2(Reg2Op::BIC, dn.idx, m.idx);
    }

    static inline uint16_t mvns(LoReg dn, LoReg m)
    {
        return fmtReg2(Reg2Op::MVN, dn.idx, m.idx);
    }

    static inline uint16_t sxth(LoReg d, LoReg m)
    {
        return fmtReg2(Reg2Op::SXH, d.idx, m.idx);
    }

    static inline uint16_t sxtb(LoReg d, LoReg m)
    {
        return fmtReg2(Reg2Op::SXB, d.idx, m.idx);
    }

    static inline uint16_t uxth(LoReg d, LoReg m)
    {
        return fmtReg2(Reg2Op::UXH, d.idx, m.idx);
    }

    static inline uint16_t uxtb(LoReg d, LoReg m)
    {
        return fmtReg2(Reg2Op::UXB, d.idx, m.idx);
    }

    static inline uint16_t rev(LoReg d, LoReg m)
    {
        return fmtReg2(Reg2Op::REV, d.idx, m.idx);
    }

    static inline uint16_t rev16(LoReg d, LoReg m)
    {
        return fmtReg2(Reg2Op::REV16, d.idx, m.idx);
    }

    static inline uint16_t revsh(LoReg d, LoReg m)
    {
        return fmtReg2(Reg2Op::REVSH, d.idx, m.idx);
    }

    static inline uint16_t adds(LoReg d, LoReg n, Imm<3> imm)
    {
        return fmtReg3(Reg3Op::ADDIMM, d.idx, n.idx, imm.v);
    }

    static inline uint16_t subs(LoReg d, LoReg n, Imm<3> imm)
    {
        return fmtReg3(Reg3Op::SUBIMM, d.idx, n.idx, imm.v);
    }

    static inline uint16_t adds(LoReg d, LoReg n, LoReg m)
    {
        return fmtReg3(Reg3Op::ADDREG, d.idx, n.idx, m.idx);
    }

    static inline uint16_t subs(LoReg d, LoReg n, LoReg m)
    {
        return fmtReg3(Reg3Op::SUBREG, d.idx, n.idx, m.idx);
    }

    static inline uint16_t str(LoReg t, LoReg n, LoReg m)
    {
        return fmtReg3(Reg3Op::STR, t.idx, n.idx, m.idx);
    }

    static inline uint16_t strh(LoReg t, LoReg n, LoReg m)
    {
        return fmtReg3(Reg3Op::STRH, t.idx, n.idx, m.idx);
    }

    static inline uint16_t strb(LoReg t, LoReg n, LoReg m)
    {
        return fmtReg3(Reg3Op::STRB, t.idx, n.idx, m.idx);
    }

    static inline uint16_t ldrsb(LoReg t, LoReg n, LoReg m)
    {
        return fmtReg3(Reg3Op::LDRSB, t.idx, n.idx, m.idx);
    }

    static inline uint16_t ldr(LoReg t, LoReg n, LoReg m)
    {
        return fmtReg3(Reg3Op::LDR, t.idx, n.idx, m.idx);
    }

    static inline uint16_t ldrh(LoReg t, LoReg n, LoReg m)
    {
        return fmtReg3(Reg3Op::LDRH, t.idx, n.idx, m.idx);
    }

    static inline uint16_t ldrb(LoReg t, LoReg n, LoReg m)
    {
        return fmtReg3(Reg3Op::LDRB, t.idx, n.idx, m.idx);
    }

    static inline uint16_t ldrsh(LoReg t, LoReg n, LoReg m)
    {
        return fmtReg3(Reg3Op::LDRSH, t.idx, n.idx, m.idx);
    }

    static inline uint16_t lsls(LoReg d, LoReg m, Imm<5> imm)
    {
        return fmtImm5(Imm5Op::LSL, d.idx, m.idx, imm.v);
    }

    static inline uint16_t lsrs(LoReg d, LoReg m, Imm<5> imm)
    {
        return fmtImm5(Imm5Op::LSR, d.idx, m.idx, imm.v);
    }

    static inline uint16_t asrs(LoReg d, LoReg m, Imm<5> imm)
    {
        return fmtImm5(Imm5Op::ASR, d.idx, m.idx, imm.v);
    }

    static inline uint16_t str(LoReg t, LoReg n, Uoff<2, 5> imm)
    {
        return fmtImm5(Imm5Op::STR, t.idx, n.idx, imm.v);
    }

    static inline uint16_t ldr(LoReg t, LoReg n, Uoff<2, 5> imm)
    {
        return fmtImm5(Imm5Op::LDR, t.idx, n.idx, imm.v);
    }

    static inline uint16_t strh(LoReg t, LoReg n, Uoff<1, 5> imm)
    {
        return fmtImm5(Imm5Op::STRH, t.idx, n.idx, imm.v);
    }

    static inline uint16_t ldrh(LoReg t, LoReg n, Uoff<1, 5> imm)
    {
        return fmtImm5(Imm5Op::LDRH, t.idx, n.idx, imm.v);
    }

    static inline uint16_t strb(LoReg t, LoReg n, Uoff<0, 5> imm)
    {
        return fmtImm5(Imm5Op::STRB, t.idx, n.idx, imm.v);
    }

    static inline uint16_t ldrb(LoReg t, LoReg n, Uoff<0, 5> imm)
    {
        return fmtImm5(Imm5Op::LDRB, t.idx, n.idx, imm.v);
    }

    static inline uint16_t incrSp(Uoff<2, 7> imm)
    {
        return fmtImm7(Imm7Op::INCRSP, imm.v);
    }

    static inline uint16_t decrSp(Uoff<2, 7> imm)
    {
        return fmtImm7(Imm7Op::DECRSP, imm.v);
    }

    static inline uint16_t movs(LoReg d, Imm<8> imm)
    {
        return fmtImm8(Imm8Op::MOV, d.idx, imm.v);
    }

    static inline uint16_t cmp(LoReg n, Imm<8> imm)
    {
        return fmtImm8(Imm8Op::CMP, n.idx, imm.v);
    }

    static inline uint16_t adds(LoReg dn, Imm<8> imm)
    {
        return fmtImm8(Imm8Op::ADD, dn.idx, imm.v);
    }

    static inline uint16_t subs(LoReg dn, Imm<8> imm)
    {
        return fmtImm8(Imm8Op::SUB, dn.idx, imm.v);
    }

    static inline uint16_t strSp(LoReg t, Uoff<2, 8> imm)
    {
        return fmtImm8(Imm8Op::STRSP, t.idx, imm.v);
    }

    static inline uint16_t ldrSp(LoReg t, Uoff<2, 8> imm)
    {
        return fmtImm8(Imm8Op::LDRSP, t.idx, imm.v);
    }

    static inline uint16_t ldrPc(LoReg t, Uoff<2, 8> imm)
    {
        return fmtImm8(Imm8Op::LDR, t.idx, imm.v);
    }

    static inline uint16_t addPc(LoReg d, Uoff<2, 8> imm)
    {
        return fmtImm8(Imm8Op::ADR, d.idx, imm.v);
    }

    static inline uint16_t addSp(LoReg d, Uoff<2, 8> imm)
    {
        return fmtImm8(Imm8Op::ADDSP, d.idx, imm.v);
    }

    static inline uint16_t add(AnyReg dn, AnyReg m)
    {
        return fmtHiReg(HiRegOp::ADD, dn.idx, m.idx);
    }

    static inline uint16_t mov(AnyReg dn, AnyReg m)
    {
        return fmtHiReg(HiRegOp::MOV, dn.idx, m.idx);
    }

    static inline uint16_t blx(AnyReg m)
    {
        return fmtHiReg(HiRegOp::JMP, 0b1000, m.idx);
    }

    static inline uint16_t bx(AnyReg m)
    {
        return fmtHiReg(HiRegOp::JMP, 0b0000, m.idx);
    }

    static inline uint16_t cmp(AnyReg n, AnyReg m)
    {
        assert((n.idx & 0b1000) || (m.idx & 0b1000)); // GCOV_EXCL_LINE
        return fmtHiReg(HiRegOp::CMP, n.idx, m.idx);
    }

    static inline uint16_t push(LoRegs l)
    {
        return fmtPushPop(false, false, l.flags);
    }

    static inline uint16_t pushWithLr(LoRegs l)
    {
        return fmtPushPop(false, true, l.flags);
    }

    static inline uint16_t pop(LoRegs l)
    {
        return fmtPushPop(true, false, l.flags);
    }

    static inline uint16_t popWithPc(LoRegs l)
    {
        return fmtPushPop(true, true, l.flags);
    }

    static inline uint16_t stmia(LoReg n, LoRegs l)
    {
        return lsMia(false, n.idx, l.flags);
    }

    static inline uint16_t ldmia(LoReg n, LoRegs l)
    {
        return lsMia(true, n.idx, l.flags);
    }

    static inline uint16_t beq(Ioff<1, 8> off)
    {
        return fmtBranchSvc(BranchOp::EQ, off.v);
    }

    static inline uint16_t bne(Ioff<1, 8> off)
    {
        return fmtBranchSvc(BranchOp::NE, off.v);
    }

    static inline uint16_t bhs(Ioff<1, 8> off)
    {
        return fmtBranchSvc(BranchOp::HS, off.v);
    }

    static inline uint16_t blo(Ioff<1, 8> off)
    {
        return fmtBranchSvc(BranchOp::LO, off.v);
    }

    static inline uint16_t bmi(Ioff<1, 8> off)
    {
        return fmtBranchSvc(BranchOp::MI, off.v);
    }

    static inline uint16_t bpl(Ioff<1, 8> off)
    {
        return fmtBranchSvc(BranchOp::PL, off.v);
    }

    static inline uint16_t bvs(Ioff<1, 8> off)
    {
        return fmtBranchSvc(BranchOp::VS, off.v);
    }

    static inline uint16_t bvc(Ioff<1, 8> off)
    {
        return fmtBranchSvc(BranchOp::VC, off.v);
    }

    static inline uint16_t bhi(Ioff<1, 8> off)
    {
        return fmtBranchSvc(BranchOp::HI, off.v);
    }

    static inline uint16_t bls(Ioff<1, 8> off)
    {
        return fmtBranchSvc(BranchOp::LS, off.v);
    }

    static inline uint16_t bge(Ioff<1, 8> off)
    {
        return fmtBranchSvc(BranchOp::GE, off.v);
    }

    static inline uint16_t blt(Ioff<1, 8> off)
    {
        return fmtBranchSvc(BranchOp::LT, off.v);
    }

    static inline uint16_t bgt(Ioff<1, 8> off)
    {
        return fmtBranchSvc(BranchOp::GT, off.v);
    }

    static inline uint16_t ble(Ioff<1, 8> off)
    {
        return fmtBranchSvc(BranchOp::LE, off.v);
    }

    static inline uint16_t udf(Imm<8> off)
    {
        return fmtBranchSvc(BranchOp::UDF, off.v);
    }

    static inline uint16_t svc(Imm<8> imm)
    {
        return fmtBranchSvc(BranchOp::SVC, imm.v);
    }

    static inline uint16_t b(Ioff<1, 11> imm)
    {
        return 0b11100'00000000000 | imm.v;
    }

    /** `off` is a signed byte offset from this instruction's own address plus
     *  4 (ARMv6-M's PC-relative convention, same as every other branch here),
     *  must be even. Returns the two halfwords in encoding order —
     *  S:I1:I2:imm10:imm11, a 24-bit signed word-pair count, ±16 MiB range. */
    static inline void bl(int32_t off, uint16_t &hw1, uint16_t &hw2)
    {
        assert((off & 1) == 0); // GCOV_EXCL_LINE
        int32_t raw = off >> 1;
        constexpr int32_t lim = 1 << 23;
        assert(raw >= -lim && raw < lim); // GCOV_EXCL_LINE

        uint16_t s = (raw >> 23) & 1;
        uint16_t i1 = (raw >> 22) & 1;
        uint16_t i2 = (raw >> 21) & 1;
        uint16_t imm10 = (raw >> 11) & 0x3ff;
        uint16_t imm11 = raw & 0x7ff;
        uint16_t j1 = (~(i1 ^ s)) & 1;
        uint16_t j2 = (~(i2 ^ s)) & 1;

        hw1 = 0b11110'0'0000000000 | (s << 10) | imm10;
        hw2 = 0b11'0'1'0'00000000000 | (j1 << 13) | (j2 << 11) | imm11;
    }

    static inline bool isBL(uint16_t hw1)
    {
        return (hw1 >> 11) == 0b11110;
    }

    static inline int32_t getBLOffset(uint16_t hw1, uint16_t hw2)
    {
        assert(isBL(hw1)); // GCOV_EXCL_LINE
        uint16_t s = (hw1 >> 10) & 1;
        uint16_t imm10 = hw1 & 0x3ff;
        uint16_t j1 = (hw2 >> 13) & 1;
        uint16_t j2 = (hw2 >> 11) & 1;
        uint16_t imm11 = hw2 & 0x7ff;
        uint16_t i1 = (~(j1 ^ s)) & 1;
        uint16_t i2 = (~(j2 ^ s)) & 1;
        int32_t raw = (s << 23) | (i1 << 22) | (i2 << 21) | (imm10 << 11) | imm11;
        // Sign-extend the 24-bit field, then re-scale.
        return ((raw << 8) >> 8) << 1;
    }

    static inline uint16_t bkpt(Imm<8> imm)
    {
        return 0b10111110'00000000 | imm.v;
    }

    static inline uint16_t cpsie()
    {
        return fmtNoArg(NoArgOp::CPSIE);
    }

    static inline uint16_t cpsid()
    {
        return fmtNoArg(NoArgOp::CPSID);
    }

    static inline uint16_t nop()
    {
        return fmtNoArg(NoArgOp::NOP);
    }

    static inline uint16_t yield()
    {
        return fmtNoArg(NoArgOp::YIELD);
    }

    static inline uint16_t wfe()
    {
        return fmtNoArg(NoArgOp::WFE);
    }

    static inline uint16_t wfi()
    {
        return fmtNoArg(NoArgOp::WFI);
    }

    static inline uint16_t sev()
    {
        return fmtNoArg(NoArgOp::SEV);
    }

    /** Sign-extend the low `bits` bits of `raw` to a full int32_t — the
     *  decode-side counterpart to `Ioff`'s own encode-side truncation, used
     *  to recover a branch's real signed byte delta from its raw, unscaled
     *  encoded field. */
    static inline int32_t signExtend(uint16_t raw, unsigned bits)
    {
        int32_t shift = 32 - (int32_t)bits;
        return (int32_t)((uint32_t)raw << shift) >> shift;
    }

    static inline bool isCondBranch(uint16_t isn)
    {
        // LE (0b1101) is the largest valid condition (inverse()'s assert has
        // the same ceiling); fields 0b1110/0b1111 are UDF/SVC, not real
        // conditions, so those two are excluded, not LE. Using `< 0b1101`
        // here instead of `<=` would misclassify LE-conditioned branches as
        // unconditional, routing patchBranch() into the wrong patch path.
        return ((isn >> 12) == 0b1101) && (((isn >> 8) & 0b1111) <= 0b1101);
    }

    static inline bool getCondBranchOffset(uint16_t isn, uint16_t &off)
    {
        if(isCondBranch(isn))
        {
            off = isn & 0xff;
            return true;
        }

        return false;
    }

    static inline uint16_t setCondBranchOffset(uint16_t isn, Ioff<1, 8> off)
    {
        assert(isCondBranch(isn)); // GCOV_EXCL_LINE
        return (isn & ~0xff) | off.v;
    }

    static inline bool getBranchOffset(uint16_t isn, uint16_t &off)
    {
        if(isn >> 11 == 0b11100)
        {
            off = isn & 0x07ff;
            return true;
        }

        return false;
    }

    static inline uint16_t setBranchOffset(uint16_t isn, Ioff<1, 11> off)
    {
        assert(isn >> 11 == 0b11100); // GCOV_EXCL_LINE
        return (isn & ~0x07ff) | off.v;
    }

    enum class Condition: uint16_t // TODO uint8_t ?
    {
        EQ = 0b0000,
        NE = 0b0001,
        HS = 0b0010,
        LO = 0b0011,
        MI = 0b0100,
        PL = 0b0101,
        VS = 0b0110,
        VC = 0b0111,
        HI = 0b1000,
        LS = 0b1001,
        GE = 0b1010,
        LT = 0b1011,
        GT = 0b1100,
        LE = 0b1101
    };

    static inline Condition inverse(Condition c)
    {
        assert(c <= Condition::LE); // GCOV_EXCL_LINE
        return Condition(((uint8_t)c) ^ 0b0001);
    }

    static inline Condition getBranchCondtion(uint16_t isn)
    {
        assert(isCondBranch(isn)); // GCOV_EXCL_LINE
        return Condition((isn >> 8) & 0b1111);
    }

    static inline uint16_t condBranch(Condition c, Ioff<1, 8> off)
    {
        return fmtBranchSvc((BranchOp)((uint16_t)BranchOp::EQ | ((uint16_t)c << 8)), off.v);
    }

    static inline bool isLiteralAccess(uint16_t isn)
    {
        const auto p5 = isn >> 11;
        return (p5 == 0b01001 || p5 == 0b10100);
    }

    static inline bool getLiteralOffset(uint16_t isn, uint16_t &off)
    {
        if(isLiteralAccess(isn))
        {
            off = isn & 0xff;
            return true;
        }

        return false;
    }

    static inline uint16_t setLiteralOffset(uint16_t isn, Uoff<2, 8> off)
    {
        assert(isLiteralAccess(isn)); // GCOV_EXCL_LINE
        return (isn & ~0xff) | off.v;
    }

    static inline bool isStackAccess(uint16_t isn)
    {
        const auto p5 = isn & (-1 << 11);
        return (p5 == (uint16_t)Imm8Op::LDRSP || p5 == (uint16_t)Imm8Op::STRSP || p5 == (uint16_t)Imm8Op::ADDSP);
    }

    static inline bool getStackOffset(uint16_t isn, uint16_t &off)
    {
        if(isStackAccess(isn))
        {
            off = isn & 0xff;
            return true;
        }

        return false;
    }

    static inline uint16_t setStackOffset(uint16_t isn, Uoff<2, 8> off)
    {
        assert(isStackAccess(isn)); // GCOV_EXCL_LINE
        return (isn & ~0xff) | off.v;
    }
};

#endif /* JIT_ARMV6M_ARMV6_H_ */
