// Copyright (c) 2014-2015 Dropbox, Inc.
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

#ifndef PYSTON_CODEGEN_BCINSTRUCTIONS_H
#define PYSTON_CODEGEN_BCINSTRUCTIONS_H

#include <cstring>

namespace pyston {

typedef uint32_t ConstPoolIndex;
typedef uint32_t VRegIndex;

enum class BCOp : unsigned char {
    LoadConst = 1, // reg, const index
    Return,
    ReturnNone,
    Store,
    Print,
    SetAttrParent,
    GetGlobalParent,
    CreateFunction,
    RuntimeCall,
    BinOp,
};

struct __attribute__((packed)) Instruction {
    BCOp op;

    Instruction(BCOp op) : op(op) {}

    int sizeInBytes() const { return sizeof(Instruction); }
};

struct __attribute__((packed)) InstructionR : Instruction {
    uint16_t reg;

    char _padding;

    InstructionR(BCOp op, uint16_t reg) : Instruction(op), reg(reg), _padding(0) {}

    int sizeInBytes() const { return sizeof(InstructionR); }
};

struct __attribute__((packed)) InstructionRR : Instruction {
    uint16_t reg_dst;
    uint16_t reg_src;

    char _padding[3];

    InstructionRR(BCOp op, uint16_t reg_dst, uint16_t reg_src) : Instruction(op), reg_dst(reg_dst), reg_src(reg_src) {
        memset(_padding, 0, sizeof(_padding));
    }

    int sizeInBytes() const { return sizeof(InstructionRR); }
};

struct __attribute__((packed)) InstructionRRR : Instruction {
    uint16_t reg_dst;
    uint16_t reg_src1, reg_src2;

    char _padding;

    InstructionRRR(BCOp op, uint16_t reg_dst, uint16_t reg_src1, uint16_t reg_src2)
        : Instruction(op), reg_dst(reg_dst), reg_src1(reg_src1), reg_src2(reg_src2), _padding(0) {}

    int sizeInBytes() const { return sizeof(InstructionRRR); }
};

struct __attribute__((packed)) InstructionO8RRR : Instruction {
    uint8_t other;
    uint16_t reg_dst;
    uint16_t reg_src1, reg_src2;

    InstructionO8RRR(BCOp op, uint8_t other, uint16_t reg_dst, uint16_t reg_src1, uint16_t reg_src2)
        : Instruction(op), other(other), reg_dst(reg_dst), reg_src1(reg_src1), reg_src2(reg_src2) {}

    int sizeInBytes() const { return sizeof(InstructionO8RRR); }
};

struct __attribute__((packed)) InstructionRC : Instruction {
    uint16_t reg_dst;
    ConstPoolIndex const_pool_index;

    char _padding;

    InstructionRC(BCOp op, uint16_t reg_dst, ConstPoolIndex const_pool_index)
        : Instruction(op), reg_dst(reg_dst), const_pool_index(const_pool_index), _padding(0) {}

    int sizeInBytes() const { return sizeof(InstructionRC); }
};

struct __attribute__((packed)) InstructionV : Instruction {
    uint8_t num_args;
    uint16_t reg[0];

    InstructionV(BCOp op, uint8_t num_args) : Instruction(op), num_args(num_args) {}

    int sizeInBytes() const { return sizeof(InstructionV) + num_args * sizeof(uint16_t); }
};

static_assert(sizeof(InstructionR) == 4, "something is wrong");
static_assert(sizeof(InstructionRR) == 8, "something is wrong");
static_assert(sizeof(InstructionRRR) == 8, "something is wrong");
static_assert(sizeof(InstructionO8RRR) == 8, "something is wrong");
static_assert(sizeof(InstructionRC) == 8, "something is wrong");
static_assert(sizeof(InstructionV) == 2, "something is wrong");
}

#endif
