/*
** This file has been pre-processed with DynASM.
** http://luajit.org/dynasm.html
** DynASM version 1.4.0, DynASM x64 version 1.4.0
** DO NOT EDIT! The original file is in "src/asm_writing/assembler.dasc".
*/

#line 1 "src/asm_writing/assembler.dasc"
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

#include "asm_writing/assembler.h"

#include <cstring>

#include "core/common.h"
#include "core/options.h"

extern "C" {
#include "dasm_proto.h"
#include "dasm_x86.h"
}
//|.arch x64
#if DASM_VERSION != 10400
#error "Version mismatch between DynASM and included encoding engine"
#endif
#line 27 "src/asm_writing/assembler.dasc"

#define Dst &d

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

Assembler::Assembler(uint8_t* start, int size)
: start_addr(start), end_addr(start + size), addr(start_addr), failed(false) {
    d = 0;
    //|.section code
#define DASM_SECTION_CODE	0
#define DASM_MAXSECTION		1
#line 69 "src/asm_writing/assembler.dasc"
    dasm_init(&d, DASM_MAXSECTION);
    //|.actionlist bf_actions
static const unsigned char bf_actions[470] = {
  235,255,144,255,205,3,255,241,144,255,64,184,240,42,237,255,72,199,192,240,
  35,237,255,72,199,128,253,240,3,233,237,255,72,137,192,240,131,240,35,255,
  72,137,128,253,240,131,240,3,233,255,72,139,128,253,240,131,240,3,233,255,
  64,139,128,253,240,131,240,19,233,255,64,138,128,253,240,131,240,3,233,255,
  64,15,182,128,253,240,132,240,20,233,255,64,15,190,128,253,240,132,240,20,
  233,255,64,15,183,128,253,240,132,240,20,233,255,64,15,191,128,253,240,132,
  240,20,233,255,72,15,182,128,253,240,132,240,4,233,255,72,15,190,128,253,
  240,132,240,4,233,255,72,15,183,128,253,240,132,240,4,233,255,72,15,191,128,
  253,240,132,240,4,233,255,72,49,192,240,131,240,35,255,252,242,64,15,16,192,
  240,132,240,52,255,252,242,64,15,17,128,253,240,132,240,20,233,255,252,242,
  64,15,16,128,253,240,132,240,20,233,255,252,243,64,15,16,128,253,240,132,
  240,20,233,255,252,243,64,15,90,192,240,132,240,52,255,64,80,240,42,255,64,
  88,240,42,255,72,129,192,240,35,239,255,72,129,232,240,35,239,255,72,129,
  128,253,240,3,233,239,255,64,252,255,128,253,240,11,233,255,64,252,255,136,
  253,240,11,233,255,252,255,4,37,237,255,252,255,12,37,237,255,72,252,255,
  128,253,240,3,233,255,72,252,255,136,253,240,3,233,255,72,252,255,4,37,237,
  255,72,252,255,12,37,237,255,252,255,20,37,237,255,64,252,255,208,240,43,
  255,64,252,255,144,253,240,11,233,255,195,255,72,57,192,240,131,240,35,255,
  72,129,252,248,240,35,239,255,64,129,252,248,240,43,239,255,72,129,184,253,
  240,3,233,239,255,64,129,184,253,240,11,233,239,255,72,59,128,253,240,131,
  240,3,233,255,72,141,128,253,240,131,240,3,233,255,72,133,192,240,131,240,
  35,255,252,233,245,255,64,252,255,160,253,240,11,233,255,15,133,245,255,15,
  132,245,255,64,252,255,224,240,43,255,64,15,148,208,240,36,255,64,15,149,
  208,240,36,255,201,255
};

#line 71 "src/asm_writing/assembler.dasc"
    unsigned npc = 8;
    dasm_setup(&d, bf_actions);
    dasm_growpc(&d, npc);
}

Assembler::~Assembler() {
    if (d) {
        assert(0);
        assemble(startAddr(), size());

    }
}

int Assembler::estSize() const {
    size_t size = 0;
    dasm_link(&d, &size);
    return size;
}

void Assembler::updateAddr() const {
    addr = startAddr() + estSize();
}

void Assembler::assemble(uint8_t* buf, int size) const {
    if (failed || estSize()>size)
        failed = true;
    else {
        dasm_encode(&d, buf);
        dasm_free(&d);
        d = 0;
    }
}

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
    //|.byte b
    dasm_put(Dst, 0, b);
#line 126 "src/asm_writing/assembler.dasc"
}

void Assembler::nop() {
    //|nop
    dasm_put(Dst, 2);
#line 130 "src/asm_writing/assembler.dasc"
}

void Assembler::trap() {
    //|int 3
    dasm_put(Dst, 4);
#line 134 "src/asm_writing/assembler.dasc"
}

void Assembler::mov(Immediate val, Register dest, bool force_64bit_load) {
    //force_64bit_load = force_64bit_load || !val.fitsInto32Bit();

    if (val.fitsInto32Bit()) {
        if (force_64bit_load) {
            //|.space 4, 0x90
            dasm_put(Dst, 7, 4);
#line 142 "src/asm_writing/assembler.dasc"
        }
        //|mov Rd(dest.regnum), val.val
        dasm_put(Dst, 10, (dest.regnum), val.val);
#line 144 "src/asm_writing/assembler.dasc"
        //|mov Rq(dest.regnum), val.val
    } else {
        //|mov Rq(dest.regnum), val.val
        dasm_put(Dst, 16, (dest.regnum), val.val);
#line 147 "src/asm_writing/assembler.dasc"
    }
}

void Assembler::movq(Immediate src, Indirect dest) {
    //|mov qword [Rq(dest.base.regnum)+dest.offset], src
    dasm_put(Dst, 23, (dest.base.regnum), dest.offset, src);
#line 152 "src/asm_writing/assembler.dasc"
}

void Assembler::mov(Register src, Register dest) {
    ASSERT(src != dest, "probably better to avoid calling this?");
    //|mov Rq(dest.regnum), Rq(src.regnum)
    dasm_put(Dst, 32, (src.regnum), (dest.regnum));
#line 157 "src/asm_writing/assembler.dasc"
}

void Assembler::mov(Register src, Indirect dest) {
    //|mov qword [Rq(dest.base.regnum)+dest.offset], Rq(src.regnum)
    dasm_put(Dst, 40, (src.regnum), (dest.base.regnum), dest.offset);
#line 161 "src/asm_writing/assembler.dasc"
}

void Assembler::mov(Indirect src, Register dest) {
    //|mov Rq(dest.regnum), qword [Rq(src.base.regnum)+src.offset]
    dasm_put(Dst, 50, (dest.regnum), (src.base.regnum), src.offset);
#line 165 "src/asm_writing/assembler.dasc"
}
void Assembler::movq(Indirect src, Register dest) {
    //|mov Rq(dest.regnum), qword [Rq(src.base.regnum)+src.offset]
    dasm_put(Dst, 50, (dest.regnum), (src.base.regnum), src.offset);
#line 168 "src/asm_writing/assembler.dasc"
}
void Assembler::movl(Indirect src, Register dest) {
    //|mov Rd(dest.regnum), dword [Rq(src.base.regnum)+src.offset]
    dasm_put(Dst, 60, (dest.regnum), (src.base.regnum), src.offset);
#line 171 "src/asm_writing/assembler.dasc"
}
void Assembler::movb(Indirect src, Register dest) {
    //|mov Rb(dest.regnum), byte [Rq(src.base.regnum)+src.offset]
    dasm_put(Dst, 70, (dest.regnum), (src.base.regnum), src.offset);
#line 174 "src/asm_writing/assembler.dasc"
}
void Assembler::movzbl(Indirect src, Register dest) {
    //|movzx Rd(dest.regnum), byte [Rq(src.base.regnum)+src.offset]
    dasm_put(Dst, 80, (dest.regnum), (src.base.regnum), src.offset);
#line 177 "src/asm_writing/assembler.dasc"
}
void Assembler::movsbl(Indirect src, Register dest) {
    //|movsx Rd(dest.regnum), byte [Rq(src.base.regnum)+src.offset]
    dasm_put(Dst, 91, (dest.regnum), (src.base.regnum), src.offset);
#line 180 "src/asm_writing/assembler.dasc"
}
void Assembler::movzwl(Indirect src, Register dest) {
    //|movzx Rd(dest.regnum), word [Rq(src.base.regnum)+src.offset]
    dasm_put(Dst, 102, (dest.regnum), (src.base.regnum), src.offset);
#line 183 "src/asm_writing/assembler.dasc"
}
void Assembler::movswl(Indirect src, Register dest) {
    //|movsx Rd(dest.regnum), word [Rq(src.base.regnum)+src.offset]
    dasm_put(Dst, 113, (dest.regnum), (src.base.regnum), src.offset);
#line 186 "src/asm_writing/assembler.dasc"
}
void Assembler::movzbq(Indirect src, Register dest) {
    //|movzx Rq(dest.regnum), byte [Rq(src.base.regnum)+src.offset]
    dasm_put(Dst, 124, (dest.regnum), (src.base.regnum), src.offset);
#line 189 "src/asm_writing/assembler.dasc"
}
void Assembler::movsbq(Indirect src, Register dest) {
    //|movsx Rq(dest.regnum), byte [Rq(src.base.regnum)+src.offset]
    dasm_put(Dst, 135, (dest.regnum), (src.base.regnum), src.offset);
#line 192 "src/asm_writing/assembler.dasc"
}
void Assembler::movzwq(Indirect src, Register dest) {
    //|movzx Rq(dest.regnum), word [Rq(src.base.regnum)+src.offset]
    dasm_put(Dst, 146, (dest.regnum), (src.base.regnum), src.offset);
#line 195 "src/asm_writing/assembler.dasc"
}
void Assembler::movswq(Indirect src, Register dest) {
    //|movsx Rq(dest.regnum), word [Rq(src.base.regnum)+src.offset]
    dasm_put(Dst, 157, (dest.regnum), (src.base.regnum), src.offset);
#line 198 "src/asm_writing/assembler.dasc"
}
void Assembler::movslq(Indirect src, Register dest) {
    assert(0);
    //|movsx Rq(dest.regnum), dword [Rq(src.base.regnum)+src.offset]
}

void Assembler::clear_reg(Register reg) {
    //|xor Rq(reg.regnum), Rq(reg.regnum)
    dasm_put(Dst, 168, (reg.regnum), (reg.regnum));
#line 206 "src/asm_writing/assembler.dasc"
}

void Assembler::movsd(XMMRegister src, XMMRegister dest) {
    //|movsd xmm(dest.regnum), xmm(src.regnum)
    dasm_put(Dst, 176, (dest.regnum), (src.regnum));
#line 210 "src/asm_writing/assembler.dasc"
}

void Assembler::movsd(XMMRegister src, Indirect dest) {
    //|movsd qword [Rq(dest.base.regnum)+dest.offset], xmm(src.regnum)
    dasm_put(Dst, 187, (src.regnum), (dest.base.regnum), dest.offset);
#line 214 "src/asm_writing/assembler.dasc"
}

void Assembler::movsd(Indirect src, XMMRegister dest) {
    //|movsd xmm(dest.regnum), qword [Rq(src.base.regnum)+src.offset]
    dasm_put(Dst, 200, (dest.regnum), (src.base.regnum), src.offset);
#line 218 "src/asm_writing/assembler.dasc"
}

void Assembler::movss(Indirect src, XMMRegister dest) {
    //|movss xmm(dest.regnum), dword [Rq(src.base.regnum)+src.offset]
    dasm_put(Dst, 213, (dest.regnum), (src.base.regnum), src.offset);
#line 222 "src/asm_writing/assembler.dasc"
}

void Assembler::cvtss2sd(XMMRegister src, XMMRegister dest) {
    //|cvtss2sd xmm(dest.regnum), xmm(src.regnum)
    dasm_put(Dst, 226, (dest.regnum), (src.regnum));
#line 226 "src/asm_writing/assembler.dasc"
}

void Assembler::push(Register reg) {
    // assert(0 && "This breaks unwinding, please don't use.");

    assert(reg != RSP); // this might work but most likely a bug

    //|push Rq(reg.regnum)
    dasm_put(Dst, 237, (reg.regnum));
#line 234 "src/asm_writing/assembler.dasc"
}

void Assembler::pop(Register reg) {
    // assert(0 && "This breaks unwinding, please don't use.");

    assert(reg != RSP); // this might work but most likely a bug

    //|pop Rq(reg.regnum)
    dasm_put(Dst, 242, (reg.regnum));
#line 242 "src/asm_writing/assembler.dasc"
}

void Assembler::add(Immediate imm, Register reg) {
    //|add Rq(reg.regnum), imm.val
    dasm_put(Dst, 247, (reg.regnum), imm.val);
#line 246 "src/asm_writing/assembler.dasc"
}

void Assembler::sub(Immediate imm, Register reg) {
    //|sub Rq(reg.regnum), imm.val
    dasm_put(Dst, 254, (reg.regnum), imm.val);
#line 250 "src/asm_writing/assembler.dasc"
}

void Assembler::add(Immediate imm, Indirect mem) {
    //|add qword [Rq(mem.base.regnum)+mem.offset], imm.val
    dasm_put(Dst, 261, (mem.base.regnum), mem.offset, imm.val);
#line 254 "src/asm_writing/assembler.dasc"
}

void Assembler::incl(Indirect mem) {
    //|inc dword [Rq(mem.base.regnum)+mem.offset]
    dasm_put(Dst, 270, (mem.base.regnum), mem.offset);
#line 258 "src/asm_writing/assembler.dasc"
}

void Assembler::decl(Indirect mem) {
    //|dec dword [Rq(mem.base.regnum)+mem.offset]
    dasm_put(Dst, 279, (mem.base.regnum), mem.offset);
#line 262 "src/asm_writing/assembler.dasc"
}

void Assembler::incl(Immediate imm) {
    //|inc dword [imm.val]
    dasm_put(Dst, 288, imm.val);
#line 266 "src/asm_writing/assembler.dasc"
}

void Assembler::decl(Immediate imm) {
    //|dec dword [imm.val]
    dasm_put(Dst, 294, imm.val);
#line 270 "src/asm_writing/assembler.dasc"
}

void Assembler::incq(Indirect mem) {
    //|inc qword [Rq(mem.base.regnum)+mem.offset]
    dasm_put(Dst, 300, (mem.base.regnum), mem.offset);
#line 274 "src/asm_writing/assembler.dasc"
}

void Assembler::decq(Indirect mem) {
    //|dec qword [Rq(mem.base.regnum)+mem.offset]
    dasm_put(Dst, 309, (mem.base.regnum), mem.offset);
#line 278 "src/asm_writing/assembler.dasc"
}

void Assembler::incq(Immediate imm) {
    //|inc qword [imm.val]
    dasm_put(Dst, 318, imm.val);
#line 282 "src/asm_writing/assembler.dasc"
}

void Assembler::decq(Immediate imm) {
    //|dec qword [imm.val]
    dasm_put(Dst, 325, imm.val);
#line 286 "src/asm_writing/assembler.dasc"
}

void Assembler::call(Immediate imm) {
    //|call qword [imm.val]
    dasm_put(Dst, 332, imm.val);
#line 290 "src/asm_writing/assembler.dasc"
}

void Assembler::callq(Register r) {
    //|call Rq(r.regnum)
    dasm_put(Dst, 338, (r.regnum));
#line 294 "src/asm_writing/assembler.dasc"
}

void Assembler::callq(Indirect mem) {
    //|call qword [Rq(mem.base.regnum)+mem.offset]
    dasm_put(Dst, 345, (mem.base.regnum), mem.offset);
#line 298 "src/asm_writing/assembler.dasc"
}

void Assembler::retq() {
    //|ret
    dasm_put(Dst, 354);
#line 302 "src/asm_writing/assembler.dasc"
}


void Assembler::cmp(Register reg1, Register reg2) {
    //|cmp Rq(reg2.regnum), Rq(reg1.regnum)
    dasm_put(Dst, 356, (reg1.regnum), (reg2.regnum));
#line 307 "src/asm_writing/assembler.dasc"
}

void Assembler::cmp(Register reg, Immediate imm, MovType type) {
    if (type == MovType::Q)
        //|cmp Rq(reg.regnum), imm.val
        dasm_put(Dst, 364, (reg.regnum), imm.val);
#line 312 "src/asm_writing/assembler.dasc"
    else if (type == MovType::L)
        //|cmp Rd(reg.regnum), imm.val
        dasm_put(Dst, 372, (reg.regnum), imm.val);
#line 314 "src/asm_writing/assembler.dasc"
    else
        assert(0);
}

void Assembler::cmp(Indirect mem, Immediate imm, MovType type) {
    if (type == MovType::Q)
        //|cmp qword [Rq(mem.base.regnum)+mem.offset], imm.val
        dasm_put(Dst, 380, (mem.base.regnum), mem.offset, imm.val);
#line 321 "src/asm_writing/assembler.dasc"
    else if (type == MovType::L)
        //|cmp dword [Rq(mem.base.regnum)+mem.offset], imm.val
        dasm_put(Dst, 389, (mem.base.regnum), mem.offset, imm.val);
#line 323 "src/asm_writing/assembler.dasc"
    else
        assert(0);
}

void Assembler::cmp(Indirect mem, Register reg) {
    //|cmp Rq(reg.regnum), qword [Rq(mem.base.regnum)+mem.offset]
    dasm_put(Dst, 398, (reg.regnum), (mem.base.regnum), mem.offset);
#line 329 "src/asm_writing/assembler.dasc"
}

void Assembler::lea(Indirect mem, Register reg) {
    //|lea Rq(reg.regnum), qword [Rq(mem.base.regnum)+mem.offset]
    dasm_put(Dst, 408, (reg.regnum), (mem.base.regnum), mem.offset);
#line 333 "src/asm_writing/assembler.dasc"
}

void Assembler::test(Register reg1, Register reg2) {
    //|test Rq(reg2.regnum), Rq(reg1.regnum)
    dasm_put(Dst, 418, (reg1.regnum), (reg2.regnum));
#line 337 "src/asm_writing/assembler.dasc"
}



void Assembler::jmp_cond(JumpDestination dest, ConditionCode condition) {

}

void Assembler::jmp(JumpDestination dest) {
    assert(dest.type == JumpDestination::FROM_START);
    int offset = dest.offset - (addr - start_addr) - 2;
    //|jmp =>offset
    dasm_put(Dst, 426, offset);
#line 349 "src/asm_writing/assembler.dasc"

}

void Assembler::jmp(Indirect dest) {
    //|jmp qword [Rq(dest.base.regnum)+dest.offset]
    dasm_put(Dst, 430, (dest.base.regnum), dest.offset);
#line 354 "src/asm_writing/assembler.dasc"
}

void Assembler::jne(JumpDestination dest) {
    int offset = dest.offset - (addr - start_addr) - 2;
    //|jne =>offset
    dasm_put(Dst, 439, offset);
#line 359 "src/asm_writing/assembler.dasc"
}

void Assembler::je(JumpDestination dest) {
    int offset = dest.offset - (addr - start_addr) - 2;
    //|je =>offset
    dasm_put(Dst, 443, offset);
#line 364 "src/asm_writing/assembler.dasc"
}

void Assembler::jmpq(Register dest) {
    //|jmp Rq(dest.regnum)
    dasm_put(Dst, 447, (dest.regnum));
#line 368 "src/asm_writing/assembler.dasc"
}

void Assembler::sete(Register reg) {
    //|sete Rb(reg.regnum)
    dasm_put(Dst, 454, (reg.regnum));
#line 372 "src/asm_writing/assembler.dasc"
}

void Assembler::setne(Register reg) {
    //|setne Rb(reg.regnum)
    dasm_put(Dst, 461, (reg.regnum));
#line 376 "src/asm_writing/assembler.dasc"
}

void Assembler::leave() {
    //|leave
    dasm_put(Dst, 468);
#line 380 "src/asm_writing/assembler.dasc"
}

uint8_t* Assembler::emitCall(void* ptr, Register scratch) {
    // emit a 64bit movabs because some caller expect a fixed number of bytes.
    // until they are fixed use the largest encoding.
    mov(Immediate(ptr), scratch, true /* force_64bit_load */);
    callq(scratch);
    updateAddr();
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
    updateAddr();
    assert(addr <= end_addr);
    //|.space end_addr - addr, 0x90
    dasm_put(Dst, 7, end_addr - addr);
#line 440 "src/asm_writing/assembler.dasc"
}

void Assembler::fillWithNopsExcept(int bytes) {
    assert(end_addr - addr >= bytes);
    updateAddr();
    //|.space end_addr - addr - bytes, 0x90
    dasm_put(Dst, 7, end_addr - addr - bytes);
#line 446 "src/asm_writing/assembler.dasc"
}

void Assembler::emitAnnotation(int num) {
    nop();
    cmp(RAX, Immediate(num));
    nop();
}

void Assembler::skipBytes(int num) {
    assert(0);
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
