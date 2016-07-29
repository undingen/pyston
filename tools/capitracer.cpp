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


class Snippet {
public:
    std::string r, cpp;

    Snippet(std::string rewriter="", std::string cpp="") : r(rewriter), cpp(cpp) {}

    bool empty() const { return r.empty() && cpp.empty(); }
};

Snippet getValue(llvm::raw_ostream& o, llvm::DenseMap<Value*, Snippet>& known_values, Value* v) {
    if (!known_values.count(v)) {
        if (auto I = dyn_cast_or_null<ConstantInt>(v)) {
            return Snippet("r->loadConst(" + std::to_string(I->getSExtValue()) + ")", std::to_string(I->getSExtValue()));
        }

        o << "unknown value: " << *v << "\n";
        return Snippet(std::string(""), std::string(""));
    }
    return known_values[v];
}

Snippet createNewVar(llvm::DenseMap<Value*, Snippet>& known_values, Value *v) {
    auto new_var = std::to_string(known_values.size() + 1);
    assert(!known_values.count(v));
    auto rtn = Snippet("r" + new_var, "v" + new_var);;
    known_values[v] = rtn;
    return rtn;
}

std::unordered_map<PHINode*, Snippet> phis;
Snippet getDestVar(llvm::DenseMap<Value*, Snippet>& known_values, Value *v) {
    auto prepend = Snippet(std::string(""), std::string(""));
    for (auto&& phi : phis) {
        for (auto&& vv : phi.first->incoming_values()) {
            if (vv == v) {
                prepend = phi.second;
            }
        }
    }
    auto var = createNewVar(known_values, v);
    std::string pre = "auto ";
    if (prepend.empty())
        return Snippet(pre + var.r, pre + var.cpp);
    return Snippet(pre + var.r + " = " + prepend.r, pre + var.cpp + " = " + prepend.cpp);

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
    else if (pred == CmpInst::ICMP_SLE)
        return "assembler::ConditionCode::COND_NOT_GREATER";
    else if (pred == CmpInst::ICMP_SGE)
        return "assembler::ConditionCode::COND_NOT_LESS";
    else if (pred == CmpInst::ICMP_ULT)
        return "assembler::ConditionCode::COND_NOT_BELOW";


    printf("%d\n", (int)pred);
    assert(0);
    return "";
}
std::string getPredicateCpp(CmpInst::Predicate pred, std::string lhs, std::string rhs) {
    if (pred == CmpInst::ICMP_EQ)
        return lhs + " == " + rhs;
    else if (pred == CmpInst::ICMP_NE)
        return lhs + "!=";
    else if (pred == CmpInst::ICMP_SLT)
        return lhs + "<";
    else if (pred == CmpInst::ICMP_SGT)
        return lhs + ">";
    else if (pred == CmpInst::ICMP_SLE)
        return lhs + "<=";
    else if (pred == CmpInst::ICMP_SGE)
        return lhs + ">=";
    else if (pred == CmpInst::ICMP_ULT)
        return "(uint64_t)" + lhs + "< (uint64_t)" + rhs;
    printf("%d\n", (int)pred);
    assert(0);
    return "";
}
Snippet getOpcodeStr(BinaryOperator::BinaryOps op) {
    if (op == BinaryOperator::BinaryOps::Add)
        return Snippet("add", "+");
    else if (op == BinaryOperator::BinaryOps::And)
        return Snippet("and_", "&");
    assert(0);
    return Snippet(std::string(), std::string());
}

std::string getTypeStr(Type* t, LLVMContext& c) {
    if (t == Type::getInt64Ty(c))
        return "uint64_t";
    if (t == Type::getInt32Ty(c))
        return "uint32_t";
    if (t == Type::getVoidTy(c))
        return "void";
    assert(t->isPointerTy());
    return t->getPointerElementType()->isPointerTy() ? "Box**" : "Box*";
}


std::list<BasicBlock*> bbs_to_visit;
std::unordered_set<BasicBlock*> blocks_visited;
llvm::DenseMap<BasicBlock*, std::string> bbs;
void visitBB(int level, llvm::raw_ostream& o, BasicBlock* bb, DataLayout& DL, bool& no_guards_allowed, bool& ok, llvm::DenseMap<Value*, Snippet>& known_values) {
    if (level > 8 && !bb->getSinglePredecessor()) {
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

    for (auto&& phi : phis) {
        for (auto I=phi.first->block_begin(), E=phi.first->block_end(); I!=E; ++I) {
            if (*I == bb) {
                auto vv = phi.first->getIncomingValueForBlock(bb);
                if (auto GV = dyn_cast_or_null<GlobalVariable>(vv)){
                    o.indent(level) << phi.second.r << " = r->loadConst((uint64_t)&" << GV->getName() << ");\n";
                    o.indent(level) << phi.second.cpp << " = &" << GV->getName() << ";\n";
                }
                break;
            }
        }
    }


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
            auto lhs = getValue(o, known_values, ICMP->getOperand(0));
            if (lhs.empty())
                continue;
            auto rhs = getValue(o, known_values, ICMP->getOperand(1));
            if (rhs.empty())
                continue;

            auto new_var = getDestVar(known_values, ICMP);
            o.indent(level) << new_var.r << " = " << lhs.r << "->cmp(" << rhs.r << ", " << getPredicate(ICMP->getPredicate()) << ");\n";
            o.indent(level) << new_var.cpp << " = " << getPredicateCpp(ICMP->getPredicate(), lhs.cpp, rhs.cpp) << ";\n";
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

                auto new_var = getDestVar(known_values, L);
                o.indent(level) << new_var.r << " = " << v.r << "->getAttr(" << offset.getSExtValue() << ");\n";
                std::string t = getTypeStr(L->getType(), bb->getContext());
                assert(offset.getSExtValue() % 8 == 0);
                o.indent(level) << new_var.cpp << " = " << "((" << t << "*)"  << v.cpp << ")[" << offset.getSExtValue() << "/8];\n";
            } else if (auto GV = dyn_cast_or_null<GlobalVariable>(PT_no_casts)) {

                auto new_var = getDestVar(known_values, L);
                o.indent(level) << new_var.r << " = r->loadConst((uint64_t)" << GV->getName() << ");\n";
                o.indent(level) << new_var.cpp << " = " << GV->getName() << ";\n";
            } else {
                if (!known_values.count(PT_no_casts)) {
                    o.indent(level)  << "unhandled\n";
                    //assert(0);
                    ok = false;
                    continue;
                }

                auto v = getValue(o, known_values, PT_no_casts);
                if (v.empty())
                    continue;

                auto new_var = getDestVar(known_values, L);
                o.indent(level) << new_var.r << " = " << v.r << "->getAttr(0);\n";
                std::string t = getTypeStr(L->getType(), bb->getContext());
                o.indent(level) << new_var.cpp << " = *((" << t << "*)"  << v.cpp << ");\n";
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
                if (d.empty())
                    continue;

                auto v = getValue(o, known_values, S->getValueOperand());
                if (v.empty())
                    continue;

                o.indent(level) << d.r << "->setAttr(" << offset.getSExtValue() << ", " << v.r << ");\n";
                std::string t = getTypeStr(S->getValueOperand()->getType(), bb->getContext());
                assert(offset.getSExtValue() % 8 == 0);
                o.indent(level) << "((" << t << "*)" << d.cpp << ")[" << offset.getSExtValue() << "/8] = " << v.cpp << ";\n";
            } else {
                if (!known_values.count(PT_no_casts)) {
                    o.indent(level) << "unhandled\n";
                    //assert(0);
                    ok = false;
                    continue;
                }

                auto d = getValue(o, known_values, PT_no_casts);
                if (d.empty())
                    continue;

                auto v = getValue(o, known_values, S->getValueOperand());
                if (v.empty())
                    continue;

                o.indent(level) << d.r << "->setAttr(0, " << v.r << ");\n";
                std::string t = getTypeStr(S->getValueOperand()->getType(), bb->getContext());
                o.indent(level) << "*((" << t << "*)" << d.cpp << ") = " << v.cpp << ";\n";
            }

            no_guards_allowed = true;

        } else if (auto R = dyn_cast_or_null<ReturnInst>(I)) {
            if (!R->getReturnValue())
                continue;

            auto v = getValue(o, known_values, R->getReturnValue());
            if (v.empty())
                continue;
            o.indent(level) << "rewrite_args->out_rtn = " << v.r << ";\n";
            o.indent(level) << "return " << v.cpp << ";\n";
            break;
        } else if (auto A = dyn_cast_or_null<BinaryOperator>(I)) {
            auto lhs = getValue(o, known_values, A->getOperand(0));
            if (lhs.empty())
                continue;
            auto rhs = getValue(o, known_values, A->getOperand(1));
            if (rhs.empty())
                continue;

            auto op = getOpcodeStr(A->getOpcode());

            auto new_var = getDestVar(known_values, A);
            o.indent(level) << new_var.r << " = " << lhs.r << "->" << op.r << "(" << rhs.r << ");\n";
            o.indent(level) << new_var.cpp << " = " << lhs.cpp << " " << op.cpp << " " << rhs.cpp << ";\n";
        } else if (auto C = dyn_cast_or_null<CallInst>(I)) {
            auto F = C->getCalledFunction();
            if (!F)
                continue;

            if (F->getName() != "boxInt" && F->getName() != "boxBool") {
                o.indent(level) << "unknown func " << F->getName() << "\n";
                continue;
            }

            auto op = getValue(o, known_values, C->getOperand(0));
            if (op.empty())
                continue;

            auto new_var = getDestVar(known_values, C);
            o.indent(level) << new_var.r << " = r->call(false, (void*)" << F->getName() << ", { " << op.r << " })->setType(RefType::OWNED);\n";
            o.indent(level) << new_var.cpp << " = " << F->getName() << "(" << op.cpp << ");\n";

        } else if (auto B = dyn_cast_or_null<BranchInst>(I)) {
            if (!B->isConditional()) {
                o.indent(level) << "{\n";
                visitBB(level + 4, o, B->getSuccessor(0), DL, no_guards_allowed, ok, known_values);
                o.indent(level) << "}\n";
                break;
            }

            auto cond = B->getCondition();
            auto new_var = getValue(o, known_values, cond);
            if (new_var.empty())
                continue;

            o.indent(level) << "if (" << new_var.cpp << ")\n";

            auto btrue = B->getSuccessor(0);
            o.indent(level) << "{\n";
            o.indent(level + 4) << new_var.r << "->addGuardNotEq(0);\n";
            visitBB(level + 4, o, btrue, DL, no_guards_allowed, ok, known_values);
            o.indent(level) << "} else {\n";


            auto bfalse = B->getSuccessor(1);
            o.indent(level + 4) << new_var.r << "->addGuard(0);\n";
            visitBB(level + 4, o, bfalse, DL, no_guards_allowed, ok, known_values);
            o.indent(level) << "}\n";
            break;
        } else if (auto U = dyn_cast_or_null<UnreachableInst>(I)) {
            o.indent(level) << "RELEASE_ASSERT(0, \"unreachable\");\n";
            break;
        } else if (auto PHI = dyn_cast_or_null<PHINode>(I)) {
            assert(phis.count(PHI));
            known_values[PHI] = phis[PHI];
        } else {
            ok = false;
            o.indent(level) << "RELEASE_ASSERT(0, \"UNSUPPORTED inst!\");\n";
        }
    }
}

bool visitFunc(llvm::raw_ostream& o, Function* f) {
    bool ok = true;

    LLVMContext &c = f->getContext();


    //if (f->getName().find("xrangeIteratorNext") == -1)
    //  return false;

    if (f->getName() != "str_length")
        return false;

    //if (f->getName() != "int_richcompare")
    //    return false;

    f->print(o);


    o << "\n\n";
    o << getTypeStr(f->getReturnType(), f->getContext()) << " " << f->getName() << "(CallRewriteArgs* rewrite_args";

    llvm::DenseMap<Value*, Snippet> known_values;
    bbs.clear();
    bbs_to_visit.clear();
    blocks_visited.clear();
    phis.clear();




    int i = 0;
    for (auto&& arg : f->args()) {
        o << ", " << getTypeStr(arg.getType(), f->getContext())  << " v" + std::to_string(i++);
    }
    o << ") {\n";
    o.indent(4) << "auto r = rewrite_args->rewriter;\n";

    for (auto&& arg : f->args()) {
        auto name = std::to_string(known_values.size());
        known_values[&arg] = Snippet("r" + name, "v" + name);
        if (known_values.size() == 1)
            o.indent(4) << "auto r" << name << " = rewrite_args->obj;\n";
        else
            o.indent(4) << "auto r" << name << " = rewrite_args->arg" << std::to_string(known_values.size()-1) << ";\n";
    }


    for (auto I = inst_begin(*f), E = inst_end(*f); I != E; ++I) {
        if (auto PHI = dyn_cast_or_null<PHINode>(&*I)) {
            auto name = "_phi_" + std::to_string(phis.size());
            auto name_pair = Snippet("r" + name, "v" + name);
            phis[PHI] = name_pair;
            o.indent(4) << "RewriterVar* " << name_pair.r << " = NULL;\n";

            std::string t = getTypeStr(PHI->getType(), f->getContext());
            o.indent(4) << t << " " << name_pair.cpp << " = NULL;\n";
        }
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
        o.indent(4) << "{\n";
        visitBB(8, o, bb, DL, no_guards_allowed, ok, known_values);
        o.indent(4) << "}\n";
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

