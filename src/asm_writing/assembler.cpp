/*
** This file has been pre-processed with DynASM.
** http://luajit.org/dynasm.html
** DynASM version 1.4.0, DynASM x64 version 1.4.0
** DO NOT EDIT! The original file is in "assembler.dasc".
*/

#line 1 "assembler.dasc"
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
#define DASM_CHECKS 1
#include "dasm_proto.h"
#include "dasm_x86.h"
}
//|.arch x64
#if DASM_VERSION != 10400
#error "Version mismatch between DynASM and included encoding engine"
#endif
#line 28 "assembler.dasc"

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
#line 70 "assembler.dasc"
    dasm_init(&d, DASM_MAXSECTION);
    //|.actionlist bf_actions
static const unsigned char bf_actions[467] = {
  235,255,144,255,241,144,255,64,184,240,42,237,255,72,199,192,240,35,237,255,
  72,199,128,253,240,3,233,237,255,72,137,192,240,131,240,35,255,72,137,128,
  253,240,131,240,3,233,255,72,139,128,253,240,131,240,3,233,255,64,139,128,
  253,240,131,240,19,233,255,64,138,128,253,240,131,240,3,233,255,64,15,182,
  128,253,240,132,240,20,233,255,64,15,190,128,253,240,132,240,20,233,255,64,
  15,183,128,253,240,132,240,20,233,255,64,15,191,128,253,240,132,240,20,233,
  255,72,15,182,128,253,240,132,240,4,233,255,72,15,190,128,253,240,132,240,
  4,233,255,72,15,183,128,253,240,132,240,4,233,255,72,15,191,128,253,240,132,
  240,4,233,255,72,49,192,240,131,240,35,255,252,242,64,15,16,192,240,132,240,
  52,255,252,242,64,15,17,128,253,240,132,240,20,233,255,252,242,64,15,16,128,
  253,240,132,240,20,233,255,252,243,64,15,16,128,253,240,132,240,20,233,255,
  252,243,64,15,90,192,240,132,240,52,255,64,80,240,42,255,64,88,240,42,255,
  72,129,192,240,35,239,255,72,129,232,240,35,239,255,72,129,128,253,240,3,
  233,239,255,64,252,255,128,253,240,11,233,255,64,252,255,136,253,240,11,233,
  255,252,255,4,37,237,255,252,255,12,37,237,255,72,252,255,128,253,240,3,233,
  255,72,252,255,136,253,240,3,233,255,72,252,255,4,37,237,255,72,252,255,12,
  37,237,255,252,255,20,37,237,255,64,252,255,208,240,43,255,64,252,255,144,
  253,240,11,233,255,195,255,72,57,192,240,131,240,35,255,72,129,252,248,240,
  35,239,255,64,129,252,248,240,43,239,255,72,129,184,253,240,3,233,239,255,
  64,129,184,253,240,11,233,239,255,72,59,128,253,240,131,240,3,233,255,72,
  141,128,253,240,131,240,3,233,255,72,133,192,240,131,240,35,255,252,233,243,
  255,64,252,255,160,253,240,11,233,255,15,133,243,255,15,132,243,255,64,252,
  255,224,240,43,255,64,15,148,208,240,36,255,64,15,149,208,240,36,255,201,
  255
};

#line 72 "assembler.dasc"
    unsigned npc = 10;
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
    assert(d);
    int ret = dasm_link(&d, &size);
    assert(ret == DASM_S_OK);
    return size;
}

void Assembler::updateAddr() const {
    addr = startAddr() + estSize();
}

void Assembler::assemble(uint8_t* buf, int size) const {
    if (failed || estSize()>size)
        failed = true;
    else {
        int ret = dasm_encode(&d, buf);
        printf("%d\n", ret);
        assert(ret == DASM_S_OK);
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
#line 131 "assembler.dasc"
}

void Assembler::nop() {
    //|nop
    dasm_put(Dst, 2);
#line 135 "assembler.dasc"
}

void Assembler::trap() {
    emitByte(0xcc);
}

void Assembler::mov(Immediate val, Register dest, bool force_64bit_load) {
    //force_64bit_load = force_64bit_load || !val.fitsInto32Bit();

    if (val.fitsInto32Bit()) {
        if (force_64bit_load) {
            //|.space 4, 0x90
            dasm_put(Dst, 4, 4);
#line 147 "assembler.dasc"
        }
        //|mov Rd(dest.regnum), val.val
        dasm_put(Dst, 7, (dest.regnum), val.val);
#line 149 "assembler.dasc"
    } else {
        //|mov Rq(dest.regnum), val.val
        dasm_put(Dst, 13, (dest.regnum), val.val);
#line 151 "assembler.dasc"
    }
}

void Assembler::movq(Immediate src, Indirect dest) {
    //|mov qword [Rq(dest.base.regnum)+dest.offset], src
    dasm_put(Dst, 20, (dest.base.regnum), dest.offset, src);
#line 156 "assembler.dasc"
}

void Assembler::mov(Register src, Register dest) {
    ASSERT(src != dest, "probably better to avoid calling this?");
    //|mov Rq(dest.regnum), Rq(src.regnum)
    dasm_put(Dst, 29, (src.regnum), (dest.regnum));
#line 161 "assembler.dasc"
}

void Assembler::mov(Register src, Indirect dest) {
    //|mov qword [Rq(dest.base.regnum)+dest.offset], Rq(src.regnum)
    dasm_put(Dst, 37, (src.regnum), (dest.base.regnum), dest.offset);
#line 165 "assembler.dasc"
}

void Assembler::mov(Indirect src, Register dest) {
    //|mov Rq(dest.regnum), qword [Rq(src.base.regnum)+src.offset]
    dasm_put(Dst, 47, (dest.regnum), (src.base.regnum), src.offset);
#line 169 "assembler.dasc"
}
void Assembler::movq(Indirect src, Register dest) {
    //|mov Rq(dest.regnum), qword [Rq(src.base.regnum)+src.offset]
    dasm_put(Dst, 47, (dest.regnum), (src.base.regnum), src.offset);
#line 172 "assembler.dasc"
}
void Assembler::movl(Indirect src, Register dest) {
    //|mov Rd(dest.regnum), dword [Rq(src.base.regnum)+src.offset]
    dasm_put(Dst, 57, (dest.regnum), (src.base.regnum), src.offset);
#line 175 "assembler.dasc"
}
void Assembler::movb(Indirect src, Register dest) {
    //|mov Rb(dest.regnum), byte [Rq(src.base.regnum)+src.offset]
    dasm_put(Dst, 67, (dest.regnum), (src.base.regnum), src.offset);
#line 178 "assembler.dasc"
}
void Assembler::movzbl(Indirect src, Register dest) {
    //|movzx Rd(dest.regnum), byte [Rq(src.base.regnum)+src.offset]
    dasm_put(Dst, 77, (dest.regnum), (src.base.regnum), src.offset);
#line 181 "assembler.dasc"
}
void Assembler::movsbl(Indirect src, Register dest) {
    //|movsx Rd(dest.regnum), byte [Rq(src.base.regnum)+src.offset]
    dasm_put(Dst, 88, (dest.regnum), (src.base.regnum), src.offset);
#line 184 "assembler.dasc"
}
void Assembler::movzwl(Indirect src, Register dest) {
    //|movzx Rd(dest.regnum), word [Rq(src.base.regnum)+src.offset]
    dasm_put(Dst, 99, (dest.regnum), (src.base.regnum), src.offset);
#line 187 "assembler.dasc"
}
void Assembler::movswl(Indirect src, Register dest) {
    //|movsx Rd(dest.regnum), word [Rq(src.base.regnum)+src.offset]
    dasm_put(Dst, 110, (dest.regnum), (src.base.regnum), src.offset);
#line 190 "assembler.dasc"
}
void Assembler::movzbq(Indirect src, Register dest) {
    //|movzx Rq(dest.regnum), byte [Rq(src.base.regnum)+src.offset]
    dasm_put(Dst, 121, (dest.regnum), (src.base.regnum), src.offset);
#line 193 "assembler.dasc"
}
void Assembler::movsbq(Indirect src, Register dest) {
    //|movsx Rq(dest.regnum), byte [Rq(src.base.regnum)+src.offset]
    dasm_put(Dst, 132, (dest.regnum), (src.base.regnum), src.offset);
#line 196 "assembler.dasc"
}
void Assembler::movzwq(Indirect src, Register dest) {
    //|movzx Rq(dest.regnum), word [Rq(src.base.regnum)+src.offset]
    dasm_put(Dst, 143, (dest.regnum), (src.base.regnum), src.offset);
#line 199 "assembler.dasc"
}
void Assembler::movswq(Indirect src, Register dest) {
    //|movsx Rq(dest.regnum), word [Rq(src.base.regnum)+src.offset]
    dasm_put(Dst, 154, (dest.regnum), (src.base.regnum), src.offset);
#line 202 "assembler.dasc"
}
void Assembler::movslq(Indirect src, Register dest) {
    assert(0);
    //|movsx Rq(dest.regnum), dword [Rq(src.base.regnum)+src.offset]
}

void Assembler::clear_reg(Register reg) {
    //|xor Rq(reg.regnum), Rq(reg.regnum)
    dasm_put(Dst, 165, (reg.regnum), (reg.regnum));
#line 210 "assembler.dasc"
}

void Assembler::movsd(XMMRegister src, XMMRegister dest) {
    //|movsd xmm(dest.regnum), xmm(src.regnum)
    dasm_put(Dst, 173, (dest.regnum), (src.regnum));
#line 214 "assembler.dasc"
}

void Assembler::movsd(XMMRegister src, Indirect dest) {
    //|movsd qword [Rq(dest.base.regnum)+dest.offset], xmm(src.regnum)
    dasm_put(Dst, 184, (src.regnum), (dest.base.regnum), dest.offset);
#line 218 "assembler.dasc"
}

void Assembler::movsd(Indirect src, XMMRegister dest) {
    //|movsd xmm(dest.regnum), qword [Rq(src.base.regnum)+src.offset]
    dasm_put(Dst, 197, (dest.regnum), (src.base.regnum), src.offset);
#line 222 "assembler.dasc"
}

void Assembler::movss(Indirect src, XMMRegister dest) {
    //|movss xmm(dest.regnum), dword [Rq(src.base.regnum)+src.offset]
    dasm_put(Dst, 210, (dest.regnum), (src.base.regnum), src.offset);
#line 226 "assembler.dasc"
}

void Assembler::cvtss2sd(XMMRegister src, XMMRegister dest) {
    //|cvtss2sd xmm(dest.regnum), xmm(src.regnum)
    dasm_put(Dst, 223, (dest.regnum), (src.regnum));
#line 230 "assembler.dasc"
}

void Assembler::push(Register reg) {
    // assert(0 && "This breaks unwinding, please don't use.");

    assert(reg != RSP); // this might work but most likely a bug

    //|push Rq(reg.regnum)
    dasm_put(Dst, 234, (reg.regnum));
#line 238 "assembler.dasc"
}

void Assembler::pop(Register reg) {
    // assert(0 && "This breaks unwinding, please don't use.");

    assert(reg != RSP); // this might work but most likely a bug

    //|pop Rq(reg.regnum)
    dasm_put(Dst, 239, (reg.regnum));
#line 246 "assembler.dasc"
}

void Assembler::add(Immediate imm, Register reg) {
    //|add Rq(reg.regnum), imm.val
    dasm_put(Dst, 244, (reg.regnum), imm.val);
#line 250 "assembler.dasc"
}

void Assembler::sub(Immediate imm, Register reg) {
    //|sub Rq(reg.regnum), imm.val
    dasm_put(Dst, 251, (reg.regnum), imm.val);
#line 254 "assembler.dasc"
}

void Assembler::add(Immediate imm, Indirect mem) {
    //|add qword [Rq(mem.base.regnum)+mem.offset], imm.val
    dasm_put(Dst, 258, (mem.base.regnum), mem.offset, imm.val);
#line 258 "assembler.dasc"
}

void Assembler::incl(Indirect mem) {
    //|inc dword [Rq(mem.base.regnum)+mem.offset]
    dasm_put(Dst, 267, (mem.base.regnum), mem.offset);
#line 262 "assembler.dasc"
}

void Assembler::decl(Indirect mem) {
    //|dec dword [Rq(mem.base.regnum)+mem.offset]
    dasm_put(Dst, 276, (mem.base.regnum), mem.offset);
#line 266 "assembler.dasc"
}

void Assembler::incl(Immediate imm) {
    //|inc dword [imm.val]
    dasm_put(Dst, 285, imm.val);
#line 270 "assembler.dasc"
}

void Assembler::decl(Immediate imm) {
    //|dec dword [imm.val]
    dasm_put(Dst, 291, imm.val);
#line 274 "assembler.dasc"
}

void Assembler::incq(Indirect mem) {
    //|inc qword [Rq(mem.base.regnum)+mem.offset]
    dasm_put(Dst, 297, (mem.base.regnum), mem.offset);
#line 278 "assembler.dasc"
}

void Assembler::decq(Indirect mem) {
    //|dec qword [Rq(mem.base.regnum)+mem.offset]
    dasm_put(Dst, 306, (mem.base.regnum), mem.offset);
#line 282 "assembler.dasc"
}

void Assembler::incq(Immediate imm) {
    //|inc qword [imm.val]
    dasm_put(Dst, 315, imm.val);
#line 286 "assembler.dasc"
}

void Assembler::decq(Immediate imm) {
    //|dec qword [imm.val]
    dasm_put(Dst, 322, imm.val);
#line 290 "assembler.dasc"
}

void Assembler::call(Immediate imm) {
    //|call qword [imm.val]
    dasm_put(Dst, 329, imm.val);
#line 294 "assembler.dasc"
}

void Assembler::callq(Register r) {
    //|call Rq(r.regnum)
    dasm_put(Dst, 335, (r.regnum));
#line 298 "assembler.dasc"
}

void Assembler::callq(Indirect mem) {
    //|call qword [Rq(mem.base.regnum)+mem.offset]
    dasm_put(Dst, 342, (mem.base.regnum), mem.offset);
#line 302 "assembler.dasc"
}

void Assembler::retq() {
    //|ret
    dasm_put(Dst, 351);
#line 306 "assembler.dasc"
}


void Assembler::cmp(Register reg1, Register reg2) {
    //|cmp Rq(reg2.regnum), Rq(reg1.regnum)
    dasm_put(Dst, 353, (reg1.regnum), (reg2.regnum));
#line 311 "assembler.dasc"
}

void Assembler::cmp(Register reg, Immediate imm, MovType type) {
    if (type == MovType::Q)
        //|cmp Rq(reg.regnum), imm.val
        dasm_put(Dst, 361, (reg.regnum), imm.val);
#line 316 "assembler.dasc"
    else if (type == MovType::L)
        //|cmp Rd(reg.regnum), imm.val
        dasm_put(Dst, 369, (reg.regnum), imm.val);
#line 318 "assembler.dasc"
    else
        assert(0);
}

void Assembler::cmp(Indirect mem, Immediate imm, MovType type) {
    if (type == MovType::Q)
        //|cmp qword [Rq(mem.base.regnum)+mem.offset], imm.val
        dasm_put(Dst, 377, (mem.base.regnum), mem.offset, imm.val);
#line 325 "assembler.dasc"
    else if (type == MovType::L)
        //|cmp dword [Rq(mem.base.regnum)+mem.offset], imm.val
        dasm_put(Dst, 386, (mem.base.regnum), mem.offset, imm.val);
#line 327 "assembler.dasc"
    else
        assert(0);
}

void Assembler::cmp(Indirect mem, Register reg) {
    //|cmp Rq(reg.regnum), qword [Rq(mem.base.regnum)+mem.offset]
    dasm_put(Dst, 395, (reg.regnum), (mem.base.regnum), mem.offset);
#line 333 "assembler.dasc"
}

void Assembler::lea(Indirect mem, Register reg) {
    //|lea Rq(reg.regnum), qword [Rq(mem.base.regnum)+mem.offset]
    dasm_put(Dst, 405, (reg.regnum), (mem.base.regnum), mem.offset);
#line 337 "assembler.dasc"
}

void Assembler::test(Register reg1, Register reg2) {
    //|test Rq(reg2.regnum), Rq(reg1.regnum)
    dasm_put(Dst, 415, (reg1.regnum), (reg2.regnum));
#line 341 "assembler.dasc"
}



void Assembler::jmp_cond(JumpDestination dest, ConditionCode condition) {
    if (condition == ConditionCode::COND_NOT_EQUAL)
        jne(dest);
    else if (condition == ConditionCode::COND_EQUAL)
        je(dest);
    else
        assert(0);
}

void Assembler::jmp(JumpDestination dest) {
    assert(dest.type == JumpDestination::FROM_START);
    //|jmp &(start_addr+dest.offset)
    dasm_put(Dst, 423, (ptrdiff_t)((start_addr+dest.offset)));
#line 357 "assembler.dasc"
}

void Assembler::jmp(Indirect dest) {
    //|jmp qword [Rq(dest.base.regnum)+dest.offset]
    dasm_put(Dst, 427, (dest.base.regnum), dest.offset);
#line 361 "assembler.dasc"
}

void Assembler::jne(JumpDestination dest) {
    //|jne &(start_addr+dest.offset)
    dasm_put(Dst, 436, (ptrdiff_t)((start_addr+dest.offset)));
#line 365 "assembler.dasc"
}

void Assembler::je(JumpDestination dest) {
    //|je &(start_addr+dest.offset)
    dasm_put(Dst, 440, (ptrdiff_t)((start_addr+dest.offset)));
#line 369 "assembler.dasc"
}

void Assembler::jmpq(Register dest) {
    //|jmp Rq(dest.regnum)
    dasm_put(Dst, 444, (dest.regnum));
#line 373 "assembler.dasc"
}

void Assembler::sete(Register reg) {
    //|sete Rb(reg.regnum)
    dasm_put(Dst, 451, (reg.regnum));
#line 377 "assembler.dasc"
}

void Assembler::setne(Register reg) {
    //|setne Rb(reg.regnum)
    dasm_put(Dst, 458, (reg.regnum));
#line 381 "assembler.dasc"
}

void Assembler::leave() {
    //|leave
    dasm_put(Dst, 465);
#line 385 "assembler.dasc"
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
    dasm_put(Dst, 4, end_addr - addr);
#line 445 "assembler.dasc"
}

void Assembler::fillWithNopsExcept(int bytes) {
    assert(end_addr - addr >= bytes);
    updateAddr();
    //|.space end_addr - addr - bytes, 0x90
    dasm_put(Dst, 4, end_addr - addr - bytes);
#line 451 "assembler.dasc"
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
