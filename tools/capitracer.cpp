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
#include <unordered_set>
#include <list>

#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/MCJIT.h"
#include "llvm/ExecutionEngine/ObjectCache.h"
#include "llvm/IR/LegacyPassManager.h"
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

std::pair<std::string, std::string> getValue(llvm::raw_ostream& o, llvm::DenseMap<Value*, std::pair<std::string, std::string>>& known_values, Value* v) {
    if (!known_values.count(v)) {
        if (auto I = dyn_cast_or_null<ConstantInt>(v)) {
            return std::make_pair("r->loadConst(" + std::to_string(I->getSExtValue()) + ")", std::to_string(I->getSExtValue()));
        }

        o << "unknown value: " << *v << "\n";
        return std::make_pair(std::string(""), std::string(""));
    }
    return known_values[v];
}

std::pair<std::string, std::string> createNewVar(llvm::DenseMap<Value*, std::pair<std::string, std::string>>& known_values, Value *v) {
    auto new_var = std::to_string(known_values.size() + 1);
    assert(!known_values.count(v));
    auto rtn = std::make_pair("r" + new_var, "v" + new_var);;
    known_values[v] = rtn;
    return rtn;
}

std::string getPredicate(CmpInst::Predicate pred) {
    if (pred == CmpInst::ICMP_EQ)
        return "assembler::ConditionCode::COND_EQUAL";
    else if (pred == CmpInst::ICMP_NE)
        return "assembler::ConditionCode::COND_NOT_EQUAL";
    else if (pred == CmpInst::ICMP_SLT)
        return "assembler::ConditionCode::COND_LESS";
    else if (pred == CmpInst::ICMP_SGT)
        return "assembler::ConditionCode::COND_GREATER";
    assert(0);
    return "";
}
std::string getPredicateCpp(CmpInst::Predicate pred) {
    if (pred == CmpInst::ICMP_EQ)
        return "==";
    else if (pred == CmpInst::ICMP_NE)
        return "!=";
    else if (pred == CmpInst::ICMP_SLT)
        return "<";
    else if (pred == CmpInst::ICMP_SGT)
        return ">";
    assert(0);
    return "";
}
std::pair<std::string, std::string> getOpcodeStr(BinaryOperator::BinaryOps op) {
    if (op == BinaryOperator::BinaryOps::Add)
        return std::make_pair("add", "+");
    else if (op == BinaryOperator::BinaryOps::And)
        return std::make_pair("and_", "&");
    assert(0);
    return std::make_pair(std::string(), std::string());
}

std::list<BasicBlock*> bbs_to_visit;
std::unordered_set<BasicBlock*> blocks_visited;
llvm::DenseMap<BasicBlock*, std::string> bbs;
void visitBB(int level, llvm::raw_ostream& o, BasicBlock* bb, DataLayout& DL, bool& no_guards_allowed, bool& ok, llvm::DenseMap<Value*, std::pair<std::string, std::string>>& known_values) {
    if (level > 4 && !bb->getSinglePredecessor()) {
        bbs_to_visit.push_back(bb);
        if (!bbs.count(bb)) {
            std::string bb_name = "bb_" + std::to_string(bbs.size());
            bbs[bb] = bb_name;
        }
        o.indent(level + 4) << "goto " << bbs[bb] << ";\n";
        return;
    }
    if (blocks_visited.count(bb)) {
        o.indent(level + 4) << "goto " << bbs[bb] << ";\n";
        return;
    }
    if (!bbs.count(bb)) {
        std::string bb_name = "bb_" + std::to_string(bbs.size());
        bbs[bb] = bb_name;
    }
    blocks_visited.insert(bb);

    o.indent(level) << bbs[bb] << ":\n";


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
        else if (auto ICMP = dyn_cast_or_null<ICmpInst>(I)) {
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
                auto v = getValue(o, known_values, GEP->getPointerOperand());
                if (v.first.empty())
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

                auto new_var = createNewVar(known_values, L);
                o.indent(level) << "auto " << new_var.first << " = " << v.first << "->getAttr(" << offset.getSExtValue() << ");\n";
                std::string t = L->getType() == Type::getInt64Ty(bb->getContext()) ? "uint64_t" : "Box*";
                assert(offset.getSExtValue() % 8 == 0);
                o.indent(level) << "auto " << new_var.second << " = " << "((" << t << "*)"  << v.second << ")[" << offset.getSExtValue() << "/8];\n";
            } else if (auto GV = dyn_cast_or_null<GlobalVariable>(PT_no_casts)) {

                auto new_var = createNewVar(known_values, L);
                o.indent(level) << "auto " << new_var.first << " = r->loadConst((uint64_t)" << GV->getName() << ");\n";
                o.indent(level) << "auto " << new_var.second << " = " << GV->getName() << ";\n";
            } else {
                if (!known_values.count(PT_no_casts)) {
                    o.indent(level)  << "unhandled\n";
                    //assert(0);
                    ok = false;
                    continue;
                }

                auto v = getValue(o, known_values, PT_no_casts);
                if (v.first.empty())
                    continue;

                auto new_var = createNewVar(known_values, L);
                o.indent(level) << "auto " << new_var.first << " = " << v.first << "->getAttr(0);\n";
                std::string t = L->getType() == Type::getInt64Ty(bb->getContext()) ? "uint64_t" : "Box*";
                o.indent(level) << "auto " << new_var.second << " = *((" << t << "*)"  << v.second << ");\n";
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

                auto d = getValue(o, known_values, GEP->getPointerOperand());
                if (d.first.empty())
                    continue;

                auto v = getValue(o, known_values, S->getValueOperand());
                if (v.first.empty())
                    continue;

                o.indent(level) << d.first << "->setAttr(" << offset.getSExtValue() << ", " << v.first << ");\n";
                std::string t = S->getValueOperand()->getType() == Type::getInt64Ty(bb->getContext()) ? "uint64_t" : "Box*";
                assert(offset.getSExtValue() % 8 == 0);
                o.indent(level) << "((" << t << "*)" << d.second << ")[" << offset.getSExtValue() << "/8] = " << v.second << ";\n";
            } else {
                if (!known_values.count(PT_no_casts)) {
                    o.indent(level) << "unhandled\n";
                    //assert(0);
                    ok = false;
                    continue;
                }

                auto d = getValue(o, known_values, PT_no_casts);
                if (d.first.empty())
                    continue;

                auto v = getValue(o, known_values, S->getValueOperand());
                if (v.first.empty())
                    continue;

                o.indent(level) << d.first << "->setAttr(0, " << v.first << ");\n";
                std::string t = S->getValueOperand()->getType() == Type::getInt64Ty(bb->getContext()) ? "uint64_t" : "Box*";
                o.indent(level) << "*((" << t << "*)" << d.second << ") = " << v.second << ";\n";
            }

            no_guards_allowed = true;

        } else if (auto R = dyn_cast_or_null<ReturnInst>(I)) {
            if (!R->getReturnValue())
                continue;

            auto v = getValue(o, known_values, R->getReturnValue());
            if (v.first.empty())
                continue;
            o.indent(level) << "rewrite_args->out_rtn = " << v.first << ";\n";
            o.indent(level) << "return " << v.second << ";\n";
            break;
        } else if (auto A = dyn_cast_or_null<BinaryOperator>(I)) {
            auto lhs = getValue(o, known_values, A->getOperand(0));
            if (lhs.first.empty())
                continue;
            auto rhs = getValue(o, known_values, A->getOperand(1));
            if (rhs.first.empty())
                continue;

            auto op = getOpcodeStr(A->getOpcode());

            auto new_var = createNewVar(known_values, A);
            o.indent(level) << "auto " << new_var.first << " = " << lhs.first << "->" << op.first << "(" << rhs.first << ");\n";
            o.indent(level) << "auto " << new_var.second << " = " << lhs.second << " " << op.second << " " << rhs.second << ";\n";
        } else if (auto C = dyn_cast_or_null<CallInst>(I)) {
            auto F = C->getCalledFunction();
            if (!F)
                continue;

            if (F->getName() != "boxInt") {
                o.indent(level) << "unknown func " << F->getName() << "\n";
                continue;
            }

            auto op = getValue(o, known_values, C->getOperand(0));
            if (op.first.empty())
                continue;

            auto new_var = createNewVar(known_values, C);
            o.indent(level) << "auto " << new_var.first << " = r->call(false, (void*)" << F->getName() << ", { " << op.first << " })->setType(RefType::OWNED);\n";
            o.indent(level) << "auto " << new_var.second << " = " << F->getName() << "(" << op.second << ");\n";

        } else if (auto B = dyn_cast_or_null<BranchInst>(I)) {
            if (!B->isConditional()) {
                o.indent(level) << "{\n";
                visitBB(level + 4, o, B->getSuccessor(0), DL, no_guards_allowed, ok, known_values);
                o.indent(level) << "}\n";
                break;
            }

            auto cond = B->getCondition();
            auto ICMP = dyn_cast_or_null<ICmpInst>(cond);
            if (!ICMP) {
                o.indent(level) << "we only support icmp branches for now " << cond << "\n";
                continue;
            }

            auto lhs = getValue(o, known_values, ICMP->getOperand(0));
            if (lhs.first.empty())
                continue;
            auto rhs = getValue(o, known_values, ICMP->getOperand(1));
            if (rhs.first.empty())
                continue;

            auto new_var = createNewVar(known_values, B);
            o.indent(level) << "auto " << new_var.first << " = " << lhs.first << "->cmp(" << rhs.first << ", " << getPredicate(ICMP->getPredicate()) << ");\n";
            o.indent(level) << "auto " << new_var.second << " = " << lhs.second << " " << getPredicateCpp(ICMP->getPredicate()) << " " << rhs.second << ";\n";
            o.indent(level) << "if (" << new_var.second << ")\n";

            auto btrue = B->getSuccessor(0);
            o.indent(level) << "{\n";
            o.indent(level + 4) << new_var.first << "->addGuardNotEq(0);\n";
            visitBB(level + 4, o, btrue, DL, no_guards_allowed, ok, known_values);
            o.indent(level) << "} else {\n";


            auto bfalse = B->getSuccessor(1);
            o.indent(level + 4) << new_var.first << "->addGuard(0);\n";
            visitBB(level + 4, o, bfalse, DL, no_guards_allowed, ok, known_values);
            o.indent(level) << "}\n";
            break;
        } else if (auto U = dyn_cast_or_null<UnreachableInst>(I)) {
            o.indent(level) << "RELEASE_ASSERT(0, \"unreachable\");\n";
            break;
        } else {
            ok = false;
            o.indent(level) << "UNSUPPORTED inst!\n";
        }
    }
}

bool visitFunc(llvm::raw_ostream& o, Function* f) {
    bool ok = true;

    LLVMContext &c = f->getContext();


    //if (f->getName().find("xrangeIteratorNext") == -1)
    //  return false;

    //if (f->getName() != "str_length")
    //    return false;

    if (f->getName() != "int_richcompare")
        return false;

    f->print(o);


    o << "\n\nBox* rewriter_" << f->getName() << "(CallRewriteArgs* rewrite_args";

    llvm::DenseMap<Value*, std::pair<std::string, std::string>> known_values;
    bbs.clear();
    bbs_to_visit.clear();
    blocks_visited.clear();
    int i = 0;
    for (auto&& arg : f->args()) {
        o << ", Box* v" + std::to_string(i++);
    }
    o << ") {\n";
    o.indent(4) << "auto r = rewrite_args->rewriter;\n";

    for (auto&& arg : f->args()) {
        auto name = std::to_string(known_values.size());
        known_values[&arg] = std::make_pair("r" + name, "v" + name);
        if (known_values.size() == 1)
            o.indent(4) << "auto r" << name << " = rewrite_args->obj;\n";
        else
            o.indent(4) << "auto r" << name << " = rewrite_args->arg" << std::to_string(known_values.size()-1) << ";\n";
    }

    auto DL = *f->getDataLayout();
    bool no_guards_allowed = false;

    //for (auto&& bb : *f) {
    //    visitBB(4, o, &bb, DL, no_guards_allowed, ok, known_values);
    //}
    bbs_to_visit.push_back(&f->getEntryBlock());
    while (!bbs_to_visit.empty()) {
        auto bb = bbs_to_visit.front();
        bbs_to_visit.pop_front();
        if (blocks_visited.count(bb))
            continue;
        o << "{\n";
        visitBB(4, o, bb, DL, no_guards_allowed, ok, known_values);
        o << "}\n";
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

    llvm::legacy::FunctionPassManager fpm(&*M);
    //fpm.add(createDemoteRegisterToMemoryPass());
    fpm.add(createLowerSwitchPass());
    fpm.doInitialization();


    std::string dummy;
    raw_string_ostream dummy_ostream(dummy);
    std::vector<llvm::Function*> traced_funcs;
    for (Module::iterator I = M->begin(), E = M->end(); I != E; ++I) {
        fpm.run(*I);
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
    outs() << "    return 42;\n";
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

