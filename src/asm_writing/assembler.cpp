// Copyright (c) 2014-2016 Dropbox, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
|.arch x64

#include "asm_writing/assembler.h"

#include <cstring>

#include "core/common.h"
#include "core/options.h"

namespace pyston {
namespace assembler {

const char* regnames[] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
};

void Register::dump() const {
    printf("%s\n", regnames[regnum]);
}

const int dwarf_to_gp[] = {
    // http://www.x86-64.org/documentation/abi.pdf#page=57
    0,  // 0 -> rax
    2,  // 1 -> rdx
    1,  // 2 -> rcx
    3,  // 3 -> rbx
    6,  // 4 -> rsi
    7,  // 5 -> rdi
    5,  // 6 -> rbp
    4,  // 7 -> rsp
    8,  // 8 -> r8
    9,  // 9 -> r9
    10, // 10 -> r10
    11, // 11 -> r11
    12, // 12 -> r12
    13, // 13 -> r13
    14, // 14 -> r14
    15, // 15 -> r15

    // Others:
    // 16 -> ReturnAddress RA (??)
    // 17-32: xmm0-xmm15
};

Register Register::fromDwarf(int dwarf_regnum) {
    assert(dwarf_regnum >= 0 && dwarf_regnum <= 16);

    return Register(dwarf_to_gp[dwarf_regnum]);
}

GenericRegister GenericRegister::fromDwarf(int dwarf_regnum) {
    assert(dwarf_regnum >= 0);

    if (dwarf_regnum < 16) {
        return GenericRegister(Register(dwarf_to_gp[dwarf_regnum]));
    }

    if (17 <= dwarf_regnum && dwarf_regnum <= 32) {
        return GenericRegister(XMMRegister(dwarf_regnum - 17));
    }

    abort();
}

void Assembler::emitByte(uint8_t b) {
    if (addr >= end_addr) {
        failed = true;
        return;
    }

    assert(addr < end_addr);
    |.byte b
    ++addr;
}

void Assembler::nop() {
    |nop
}

void Assembler::trap() {
    |int 3
}

void Assembler::mov(Immediate val, Register dest, bool force_64bit_load) {
    force_64bit_load = force_64bit_load || !val.fitsInto32Bit();

    if (force_64bit_load) {
        |mov Rq(dest.regnum), val.val
    } else {
        |mov Rd(dest.regnum), val.val
    }
}

void Assembler::movq(Immediate src, Indirect dest) {
    |mov qword [Rq(dest.base.regnum)+dest.offset], src
}

void Assembler::mov(Register src, Register dest) {
    ASSERT(src != dest, "probably better to avoid calling this?");
    |mov Rq(dest.regnum), Rq(src.regnum)
}

void Assembler::mov(Register src, Indirect dest) {
    |mov qword [Rq(dest.base.regnum)+dest.offset], Rq(src.regnum)
}

void Assembler::mov(Indirect src, Register dest) {
    |mov Rq(dest.regnum), qword [Rq(src.base.regnum)+src.offset]
}
void Assembler::movq(Indirect src, Register dest) {
    |mov Rq(dest.regnum), qword [Rq(src.base.regnum)+src.offset]
}
void Assembler::movl(Indirect src, Register dest) {
    |mov Rd(dest.regnum), dword [Rq(src.base.regnum)+src.offset]
}
void Assembler::movb(Indirect src, Register dest) {
    |mov Rb(dest.regnum), byte [Rq(src.base.regnum)+src.offset]
}
void Assembler::movzbl(Indirect src, Register dest) {
    |movzx Rd(dest.regnum), byte [Rq(src.base.regnum)+src.offset]
}
void Assembler::movsbl(Indirect src, Register dest) {
    |movsx Rd(dest.regnum), byte [Rq(src.base.regnum)+src.offset]
}
void Assembler::movzwl(Indirect src, Register dest) {
    |movzx Rd(dest.regnum), word [Rq(src.base.regnum)+src.offset]
}
void Assembler::movswl(Indirect src, Register dest) {
    |movsx Rd(dest.regnum), word [Rq(src.base.regnum)+src.offset]
}
void Assembler::movzbq(Indirect src, Register dest) {
    |movzx Rq(dest.regnum), byte [Rq(src.base.regnum)+src.offset]
}
void Assembler::movsbq(Indirect src, Register dest) {
    |movsx Rq(dest.regnum), byte [Rq(src.base.regnum)+src.offset]
}
void Assembler::movzwq(Indirect src, Register dest) {
    |movzx Rq(dest.regnum), word [Rq(src.base.regnum)+src.offset]
}
void Assembler::movswq(Indirect src, Register dest) {
    |movsx Rq(dest.regnum), word [Rq(src.base.regnum)+src.offset]
}
void Assembler::movslq(Indirect src, Register dest) {
    assert(0);
    //|movsx Rq(dest.regnum), dword [Rq(src.base.regnum)+src.offset]
}

void Assembler::clear_reg(Register reg) {
    |xor Rq(reg.regnum), Rq(req.regnum)
}

void Assembler::movsd(XMMRegister src, XMMRegister dest) {
    |movsd xmm(dest.regnum), xmm(src.regnum)
}

void Assembler::movsd(XMMRegister src, Indirect dest) {
    |movsd qword [Rq(dest.base.regnum)+dest.offset], xmm(src.regnum)
}

void Assembler::movsd(Indirect src, XMMRegister dest) {
    |movsd xmm(src.regnum), qword [Rq(dest.base.regnum)+dest.offset]
}

void Assembler::movss(Indirect src, XMMRegister dest) {
    |movss xmm(src.regnum), dword [Rq(dest.base.regnum)+dest.offset]
}

void Assembler::cvtss2sd(XMMRegister src, XMMRegister dest) {
    |cvtss2sd xmm(dest.regnum), xmm(src.regnum)
}

void Assembler::push(Register reg) {
    // assert(0 && "This breaks unwinding, please don't use.");

    assert(reg != RSP); // this might work but most likely a bug

    |push Rq(reg.regnum)
}

void Assembler::pop(Register reg) {
    // assert(0 && "This breaks unwinding, please don't use.");

    assert(reg != RSP); // this might work but most likely a bug

    |pop Rq(reg.regnum)
}

void Assembler::add(Immediate imm, Register reg) {
    |add Rq(reg.regnum), imm.val
}

void Assembler::sub(Immediate imm, Register reg) {
    |sub Rq(reg.regnum), imm.val
}

void Assembler::add(Immediate imm, Indirect mem) {
    |add qword [Rq(mem.base.regnum)+mem.offset], imm.val
}

void Assembler::incl(Indirect mem) {
    |inc dword [Rq(mem.base.regnum)+mem.offset]
}

void Assembler::decl(Indirect mem) {
    |dec dword [Rq(mem.base.regnum)+mem.offset]
}

void Assembler::incl(Immediate imm) {
    |inc dword [imm.val]
}

void Assembler::decl(Immediate imm) {
    |dec dword [imm.val]
}

void Assembler::incq(Indirect mem) {
    |inc qword [Rq(mem.base.regnum)+mem.offset]
}

void Assembler::decq(Indirect mem) {
    |dec qword [Rq(mem.base.regnum)+mem.offset]
}

void Assembler::incq(Immediate imm) {
    |inc qword [imm.val]
}

void Assembler::decq(Immediate imm) {
    |dec qword [imm.val]
}

void Assembler::call(Immediate imm) {
    |call qword [imm.val]
}

void Assembler::callq(Register r) {
    |call Rq(r.regnum)
}

void Assembler::callq(Indirect mem) {
    |call qword [Rq(mem.base.regnum)+mem.offset]
}

void Assembler::retq() {
    |ret
}


void Assembler::cmp(Register reg1, Register reg2) {
    |cmp Rq(reg2.regnum), Rq(reg1.regnum)
}

void Assembler::cmp(Register reg, Immediate imm, MovType type) {
    if (type == MovType::Q)
        |cmp Rq(reg.regnum), imm.val
    else if (type == MovType::L)
        |cmp Rd(reg.regnum), imm.val
    else
        assert(0);
}

void Assembler::cmp(Indirect mem, Immediate imm, MovType type) {
    if (type == MovType::Q)
        |cmp qword [Rq(mem.base.regnum)+mem.offset], imm.val
    else if (type == MovType::L)
        |cmp dword [Rq(mem.base.regnum)+mem.offset], imm.val
    else
        assert(0);
}

void Assembler::cmp(Indirect mem, Register reg) {
    |cmp Rq(reg.regnum), qword [Rq(mem.base.regnum)+mem.offset]
}

void Assembler::lea(Indirect mem, Register reg) {
    |lea Rq(reg.regnum), qword [Rq(mem.base.regnum)+mem.offset]
}

void Assembler::test(Register reg1, Register reg2) {
    |test Rq(reg2.regnum), Rq(reg1.regnum)
}



void Assembler::jmp_cond(JumpDestination dest, ConditionCode condition) {

}

void Assembler::jmp(JumpDestination dest) {
    assert(dest.type == JumpDestination::FROM_START);
    int offset = dest.offset - (addr - start_addr) - 2;
    |jmp =>offset

}

void Assembler::jmp(Indirect dest) {
    |jmp qword [Rq(dest.base.regnum)+dest.offset]
}

void Assembler::jne(JumpDestination dest) {
    int offset = dest.offset - (addr - start_addr) - 2;
    |jne =>offset
}

void Assembler::je(JumpDestination dest) {
    int offset = dest.offset - (addr - start_addr) - 2;
    |je =>offset
}

void Assembler::jmpq(Register dest) {
    |jmp Rq(dest.regnum)
}

void Assembler::sete(Register reg) {
    |sete Rb(reg.regnum)
}

void Assembler::setne(Register reg) {
    |setne Rb(reg.regnum)
}

void Assembler::leave() {
    |leave
}

uint8_t* Assembler::emitCall(void* ptr, Register scratch) {
    // emit a 64bit movabs because some caller expect a fixed number of bytes.
    // until they are fixed use the largest encoding.
    mov(Immediate(ptr), scratch, true /* force_64bit_load */);
    callq(scratch);
    return addr;
}

void Assembler::emitBatchPush(int scratch_rbp_offset, int scratch_size, const std::vector<GenericRegister>& to_push) {
    int offset = 0;

    for (const GenericRegister& r : to_push) {
        Indirect next_slot(RBP, offset + scratch_rbp_offset);

        if (r.type == GenericRegister::GP) {
            Register gp = r.gp;
            assert(gp.regnum >= 0 && gp.regnum < 16);
            assert(scratch_size >= offset + 8);
            mov(gp, next_slot);
            offset += 8;
        } else if (r.type == GenericRegister::XMM) {
            XMMRegister reg = r.xmm;
            assert(scratch_size >= offset + 8);
            movsd(reg, next_slot);
            offset += 8;
        } else {
            RELEASE_ASSERT(0, "%d", r.type);
        }
    }
}

void Assembler::emitBatchPop(int scratch_rbp_offset, int scratch_size, const std::vector<GenericRegister>& to_push) {
    int offset = 0;

    for (const GenericRegister& r : to_push) {
        assert(scratch_size >= offset + 8);
        Indirect next_slot(RBP, offset + scratch_rbp_offset);

        if (r.type == GenericRegister::GP) {
            Register gp = r.gp;
            assert(gp.regnum >= 0 && gp.regnum < 16);
            movq(next_slot, gp);
            offset += 8;
        } else if (r.type == GenericRegister::XMM) {
            XMMRegister reg = r.xmm;
            movsd(next_slot, reg);
            offset += 8;
        } else {
            RELEASE_ASSERT(0, "%d", r.type);
        }
    }
}

void Assembler::fillWithNops() {
    assert(addr <= end_addr);
    |.space end_addr - addr, 0x90
    addr = end_addr;
}

void Assembler::fillWithNopsExcept(int bytes) {
    assert(end_addr - addr >= bytes);
     |.space end_addr - addr - bytes, 0x90
    addr = end_addr - bytes;
}

void Assembler::emitAnnotation(int num) {
    nop();
    cmp(RAX, Immediate(num));
    nop();
}

void Assembler::skipBytes(int num) {
    if (addr + num >= end_addr) {
        addr = end_addr;
        failed = true;
        return;
    }

    addr += num;
}

template <int MaxJumpSize>
ForwardJumpBase<MaxJumpSize>::ForwardJumpBase(Assembler& assembler, ConditionCode condition)
    : assembler(assembler), condition(condition), jmp_inst(assembler.curInstPointer()) {
    assembler.jmp_cond(JumpDestination::fromStart(assembler.bytesWritten() + MaxJumpSize), condition);
    jmp_end = assembler.curInstPointer();
}

template <int MaxJumpSize> ForwardJumpBase<MaxJumpSize>::~ForwardJumpBase() {
    uint8_t* new_pos = assembler.curInstPointer();
    int offset = new_pos - jmp_inst;
    RELEASE_ASSERT(offset < MaxJumpSize, "");
    assembler.setCurInstPointer(jmp_inst);
    assembler.jmp_cond(JumpDestination::fromStart(assembler.bytesWritten() + offset), condition);
    while (assembler.curInstPointer() < jmp_end)
        assembler.nop();
    assembler.setCurInstPointer(new_pos);
}
template class ForwardJumpBase<128>;
template class ForwardJumpBase<1048576>;
}
}
