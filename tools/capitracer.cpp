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

#include <cstdio>
#include <iostream>
#include <unordered_map>

#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/MCJIT.h"
#include "llvm/ExecutionEngine/ObjectCache.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/SystemUtils.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Cloning.h"

using namespace llvm;

static cl::opt<std::string>
InputFilename(cl::Positional, cl::desc("<input bitcode file>"),
        cl::init("-"), cl::value_desc("filename"));

static cl::opt<std::string>
OutputFilename("o", cl::desc("Specify output filename"),
        cl::value_desc("filename"), cl::init("-"));

static cl::opt<bool>
Force("f", cl::desc("Enable binary output on terminals"));


void printLambda(std::string code, std::string vars, std::string type = "MUTATION") {
    outs() << "rewrite_args->rewriter->addAction([&](){ auto a = rewrite_args->rewriter->assembler;\n" << code << " }, " << vars << ", ActionType::" << type << ");";
}

std::string getValue(llvm::raw_ostream& o, llvm::DenseMap<Value*, std::string>& known_values, Value* v) {
    if (!known_values.count(v)) {
        if (auto I = dyn_cast_or_null<ConstantInt>(v)) {
            return "r->loadConstant(" + std::to_string(I->getSExtValue()) + ")";
        }

        o << "unknown value: " << v << "\n";
        return std::string("");
    }
    return known_values[v];
}


void visitBB(int level, llvm::raw_ostream& o, BasicBlock* bb, DataLayout& DL, bool& no_guards_allowed, bool& ok, llvm::DenseMap<Value*, std::string>& known_values) {
    for (llvm::Instruction& II : *bb) {
        llvm::Instruction* I = &II;

        o.indent(level) <<"// " << *I << "\n";


        if (isa<DbgInfoIntrinsic>(I)) {
            o.indent(level) << "//    ignored debug info\n";
            continue;
        } else if (auto GEP = dyn_cast_or_null<GetElementPtrInst>(I)) {
            o.indent(level) << "//    skipping\n";
            continue;
        }
        else if (auto BC = dyn_cast_or_null<BitCastInst>(I)) {
            o.indent(level) << "//    skipping\n";
            continue;
        }
        else if (auto L = dyn_cast_or_null<LoadInst>(I)) {
            if (DL.getTypeSizeInBits(L->getType()) != 64) {
                o.indent(level) << "unknown return type size: " << DL.getTypeSizeInBits(L->getType()) << " " << L->getType() << "\n";
                continue;
            }

            auto PT = L->getPointerOperand();
            auto PT_no_casts = PT->stripPointerCasts();
            if (auto GEP = dyn_cast_or_null<GetElementPtrInst>(PT_no_casts)) {
                std::string v = getValue(o, known_values, GEP->getPointerOperand());
                if (v.empty())
                    continue;

                //if (GEP->getType()->getElementType() != llvm::Type::getInt64Ty(c)) {
                if (DL.getPointerTypeSizeInBits(GEP->getType()) != 64) {
                    o.indent(level) << "unknown return type: " << DL.getPointerTypeSizeInBits(GEP->getType()) << " " << GEP->getType() << "\n";
                    continue;
                }

                APInt offset(DL.getPointerSizeInBits(GEP->getPointerAddressSpace()), 0);
                if (!GEP->accumulateConstantOffset(DL, offset)) {
                    o.indent(level) << "non const gep\n";
                    continue;
                }

                auto new_var = "v" + std::to_string(known_values.size() + 1);
                o.indent(level) << "auto " << new_var << " = " << v << "->getAttr(" << offset.getSExtValue() << "" << ");\n";

                known_values[L] = new_var;
            } else {
                o.indent(level)  << "unhandled\n";
                //assert(0);
                ok = false;
                continue;
            }
        } else if (auto S = dyn_cast_or_null<StoreInst>(I)) {
            if (DL.getTypeSizeInBits(S->getPointerOperand()->getType()) != 64) {
                o.indent(level) << "unknown return type size: " << DL.getTypeSizeInBits(S->getPointerOperand()->getType()) << " " << S->getPointerOperand() << "\n";
                continue;
            }

            auto PT = S->getPointerOperand();
            auto PT_no_casts = PT->stripPointerCasts();
            if (auto GEP = dyn_cast_or_null<GetElementPtrInst>(PT_no_casts)) {
                APInt offset(DL.getPointerSizeInBits(GEP->getPointerAddressSpace()), 0);
                if (!GEP->accumulateConstantOffset(DL, offset)) {
                    o.indent(level) << "non const gep\n";
                    continue;
                }

                std::string v = getValue(o, known_values, S->getValueOperand());
                if (v.empty())
                    continue;

                o.indent(level) << "d->setAttr(" << offset.getSExtValue() << ", " << v << ");\n";
            } else {
                o.indent(level) << "unhandled\n";
                //assert(0);
                ok = false;
                continue;
            }

            no_guards_allowed = true;

        } else if (auto R = dyn_cast_or_null<ReturnInst>(I)) {
            if (!R->getReturnValue())
                continue;

            std::string v = getValue(o, known_values, R->getReturnValue());
            if (v.empty())
                continue;
            o.indent(level) << "rewrite_args->out_rtn = " << v << ";\n";
        } else if (auto A = dyn_cast_or_null<AddOperator>(I)) {
            std::string lhs = getValue(o, known_values, A->getOperand(0));
            if (lhs.empty())
                continue;
            std::string rhs = getValue(o, known_values, A->getOperand(1));
            if (rhs.empty())
                continue;
            auto new_var = "v" + std::to_string(known_values.size() + 1);
            o.indent(level) << new_var << " = " << lhs << "->add(" << rhs << ");\n";
            known_values[A] = new_var;
        } else if (auto C = dyn_cast_or_null<CallInst>(I)) {
            auto F = C->getCalledFunction();
            if (!F)
                continue;

            if (F->getName() != "boxInt") {
                o.indent(level) << "unknown func " << F->getName() << "\n";
                continue;
            }

            std::string op = getValue(o, known_values, C->getOperand(0));
            if (op.empty())
                continue;

            auto new_var = "v" + std::to_string(known_values.size() + 1);
            o.indent(level) << new_var << " = r->call(true, (void*)" << F->getName() << ", { " << op << " });\n";
            known_values[C] = new_var;

        } else if (auto B = dyn_cast_or_null<BranchInst>(I)) {
            if (!B->isConditional())
                continue;

            auto btrue = B->getSuccessor(0);
            o.indent(level) << "{ // " << btrue->getName() << "\n";
            visitBB(level + 4, o, btrue, DL, no_guards_allowed, ok, known_values);
            o.indent(level) << "}\n";


            auto bfalse = B->getSuccessor(1);
            o.indent(level) << "{ // " << bfalse->getName() << "\n";
            visitBB(level + 4, o, bfalse, DL, no_guards_allowed, ok, known_values);
            o.indent(level) << "}\n";

        } else {
            ok = false;
            o.indent(level) << "UNSUPPORTED inst!\n";
        }
    }
}

bool visitFunc(llvm::raw_ostream& o, Function* f) {
    bool ok = true;

    LLVMContext &c = f->getContext();


    if (f->getName().find("xrangeIteratorNext") == -1)
       return false;

    //if (f->getName() != "str_length")
    //    return changed;

    f->print(o);


    o << "\n\nvoid rewriter_" << f->getName() << "(CallRewriteArgs* rewrite_args";

    llvm::DenseMap<Value*, std::string> known_values;
    int i = 0;
    for (auto&& arg : f->args()) {
        o << ", Box* a" + std::to_string(i++);
    }
    o << ") {\n";
    o.indent(4) << "auto r = rewrite_args->rewriter;\n";

    for (auto&& arg : f->args()) {
        auto name = "v" + std::to_string(known_values.size());;
        known_values[&arg] = name;
        if (known_values.size() == 1)
            o.indent(4) << "auto " << name << " = rewrite_args->obj;\n";
        else
            o.indent(4) << "auto " << name << " = rewrite_args->arg" << std::to_string(known_values.size()-1) << ";\n";
    }

    auto DL = *f->getDataLayout();
    bool no_guards_allowed = false;

    for (auto&& bb : *f) {
        visitBB(4, o, &bb, DL, no_guards_allowed, ok, known_values);
    }

    o << "} \n";


    return true;
}

int main(int argc, char **argv) {
    sys::PrintStackTraceOnErrorSignal();
    PrettyStackTraceProgram X(argc, argv);

    LLVMContext &Context = getGlobalContext();
    llvm_shutdown_obj Y;
    cl::ParseCommandLineOptions(argc, argv, "mcjit pre-cacher");

    SMDiagnostic Err;

#if LLVMREV < 216466
    std::unique_ptr<Module> M(ParseIRFile(InputFilename, Err, Context));
#else
    std::unique_ptr<Module> M(parseIRFile(InputFilename, Err, Context));
#endif

    if (M.get() == 0) {
        Err.print(argv[0], errs());
        return 1;
    }


    std::string dummy;
    raw_string_ostream dummy_ostream(dummy);
    std::vector<llvm::Function*> traced_funcs;
    for (Module::iterator I = M->begin(), E = M->end(); I != E; ++I) {
        if (visitFunc(dummy_ostream, I))
            traced_funcs.push_back(I);
    }
    for (auto&& F : traced_funcs) {
        visitFunc(outs(), F);
    }

    outs() << "llvm::DenseMap<void*, void*> capi_tracer;\n";
    outs() << "int __capi_tracer_init_func() {\n";
    for (auto&& F : traced_funcs) {
        outs() << "    capi_tracer[(void*)" << F->getName() << "] = " << "(void*)rewriter_" << F->getName() << ";\n";
    }
    outs() << "}\nstatic int __capi_tracer_init = __capi_tracer_init_func();\n";




    /*
    if (OutputFilename.empty())
        OutputFilename = "-";

#if LLVMREV < 216393
    std::string ErrorInfo;
    tool_output_file out(OutputFilename.c_str(), ErrorInfo, sys::fs::F_None);
    if (!ErrorInfo.empty()) {
        errs() << ErrorInfo << '\n';
        return 1;
    }
#else
    std::error_code EC;
    tool_output_file out(OutputFilename, EC, sys::fs::F_None);
    if (EC) {
        errs() << "error opening file for writing\n";
        return 1;
    }
#endif

    WriteBitcodeToFile(M.get(), out.os());

    out.keep();
    */

    return 0;
}

