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

#include "asm_writing/icinfo.h"

#include <cstring>
#include <memory>

#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/Memory.h"

#include "asm_writing/assembler.h"
#include "asm_writing/mc_writer.h"
#include "codegen/patchpoints.h"
#include "codegen/unwinding.h"
#include "core/common.h"
#include "core/options.h"
#include "core/types.h"
#include "runtime/types.h"

namespace pyston {

using namespace pyston::assembler;

#define MAX_RETRY_BACKOFF 1024

int64_t ICInvalidator::version() {
    return cur_version;
}

ICInvalidator::~ICInvalidator() {
    for (ICSlotInfo* slot : dependents) {
        slot->invalidators.erase(std::find(slot->invalidators.begin(), slot->invalidators.end(), this));
    }
}

void ICInvalidator::addDependent(ICSlotInfo* entry_info) {
    auto p = dependents.insert(entry_info);
    bool was_inserted = p.second;
    if (was_inserted)
        entry_info->invalidators.push_back(this);
}

void ICInvalidator::invalidateAll() {
    cur_version++;
    for (ICSlotInfo* slot : dependents) {
        bool found_self = false;
        for (auto invalidator : slot->invalidators) {
            if (invalidator == this) {
                assert(!found_self);
                found_self = true;
            } else {
                assert(invalidator->dependents.count(slot));
                invalidator->dependents.erase(slot);
            }
        }
        assert(found_self);

        slot->invalidators.clear();
        slot->clear();
    }
    dependents.clear();
}

void ICSlotInfo::clear() {
    ic->clear(this);
    decref_infos.clear();
}

std::unique_ptr<ICSlotRewrite> ICSlotRewrite::create(ICInfo* ic, const char* debug_name) {
    if (ic->currently_rewriting)
        return NULL;

    auto ic_entry = ic->pickEntryForRewrite(debug_name);
    if (!ic_entry)
        return NULL;

    return std::unique_ptr<ICSlotRewrite>(new ICSlotRewrite(ic, debug_name, ic_entry));
}

ICSlotRewrite::ICSlotRewrite(ICInfo* ic, const char* debug_name, ICSlotInfo* ic_entry)
    : ic(ic),
      debug_name(debug_name),
      buf((uint8_t*)malloc(ic_entry->size)),
      assembler(buf, ic_entry->size),
      ic_entry(ic_entry) {
    ic->currently_rewriting = true;
    assembler.nop();
    if (VERBOSITY() >= 4)
        printf("starting %s icentry\n", debug_name);
}

ICSlotRewrite::~ICSlotRewrite() {
    free(buf);
    ic->currently_rewriting = false;
}

void ICSlotRewrite::abort() {
    if (assembler.hasFailed() && ic->percentBackedoff() > 50 && ic->slots.size() > 1) {
        for (int i = 0; i < ic->slots.size() - 1; ++i) {
            if (!ic->slots[i].num_inside && !ic->slots[i + 1].num_inside) {
                ic->slots[i].clear();
                ic->slots[i + 1].clear();
                ic->slots[i].size = ic->slots[i].size + ic->slots[i + 1].size;
                ic->slots[i + 1].size = 0;
                ic->next_slot_to_try = i;
                break;
            }
        }
    }
    ic->retry_backoff = std::min(MAX_RETRY_BACKOFF, 2 * ic->retry_backoff);
    ic->retry_in = ic->retry_backoff;
}

ICSlotInfo* ICSlotRewrite::prepareEntry() {
    if (this->ic_entry)
        return this->ic_entry;
    assert(0);
    /*
    this->ic_entry = ic->pickEntryForRewrite(debug_name);
    if (!this->ic_entry)
        return NULL;
    */
    return NULL;
}

uint8_t* ICSlotRewrite::getSlotStart() {
    assert(ic_entry != NULL);
    return ic_entry->start_addr;
}

void ICSlotRewrite::commit(CommitHook* hook, std::vector<void*> gc_references,
                           std::vector<std::pair<uint64_t, std::vector<Location>>> decref_infos,
                           std::vector<std::tuple<int, int, int>> jumps_to_patch) {
    bool still_valid = true;
    for (int i = 0; i < dependencies.size(); i++) {
        int orig_version = dependencies[i].second;
        ICInvalidator* invalidator = dependencies[i].first;
        if (orig_version != invalidator->version()) {
            still_valid = false;
            break;
        }
    }
    if (!still_valid) {
        if (VERBOSITY() >= 3)
            printf("not committing %s icentry since a dependency got updated before commit\n", debug_name);
        for (auto p : gc_references)
            Py_DECREF(p);
        return;
    }

    uint8_t* slot_start = getSlotStart();
    uint8_t* continue_point = (uint8_t*)ic->continue_addr;

    bool should_fill_with_nops = true;
    bool do_commit = hook->finishAssembly(continue_point - slot_start, should_fill_with_nops);

    if (!do_commit) {
        for (auto p : gc_references)
            Py_DECREF(p);
        return;
    }

    assert(!assembler.hasFailed());
    int real_size = assembler.bytesWritten();
    if (should_fill_with_nops)
        assembler.fillWithNops();
    int old_size = ic_entry->size;
    assert(real_size <= old_size);
    if (assembler.getSize() != old_size)
        printf("2 %d %d \n", assembler.getSize(), old_size);
    assert(assembler.getSize() == old_size);
    if (should_fill_with_nops && old_size != assembler.bytesWritten())
        printf("%d %d \n", old_size, assembler.bytesWritten());
    assert(!should_fill_with_nops || old_size == assembler.bytesWritten());

    for (int i = 0; i < dependencies.size(); i++) {
        ICInvalidator* invalidator = dependencies[i].first;
        invalidator->addDependent(ic_entry);
    }

    ic->next_slot_to_try++;

    Assembler new_asm(assembler.getStartAddr(), assembler.getSize());
    for (auto&& jump : jumps_to_patch) {
        new_asm.setCurInstPointer(assembler.getStartAddr() + std::get<0>(jump));
        new_asm.jmp_cond(assembler::JumpDestination::fromStart(real_size), (assembler::ConditionCode)std::get<2>(jump));
        while (new_asm.bytesWritten() < std::get<1>(jump))
            new_asm.nop();
    }

    // if (VERBOSITY()) printf("Commiting to %p-%p\n", start, start + ic->slot_size);
    memcpy(slot_start, buf, old_size);
    int new_slot_size = ic_entry->size - real_size;
    bool should_create_new_slot = new_slot_size > 30 && &ic->slots.back() == ic_entry && ic->slots.size() <= 8;
    if (should_create_new_slot)
        ic_entry->size = real_size;

    for (auto p : ic_entry->gc_references) {
        Py_DECREF(p);
    }
    ic_entry->gc_references = std::move(gc_references);

    ic->times_rewritten++;

    if (ic->times_rewritten == IC_MEGAMORPHIC_THRESHOLD) {
        static StatCounter megamorphic_ics("megamorphic_ics");
        megamorphic_ics.log();
    }

    // deregister old decref infos
    ic_entry->decref_infos.clear();

    // register new decref info
    for (auto&& decref_info : decref_infos) {
        // add decref locations which are always to decref inside this IC
        auto&& merged_locations = decref_info.second;
        merged_locations.insert(merged_locations.end(), ic->ic_global_decref_locations.begin(),
                                ic->ic_global_decref_locations.end());
        if (merged_locations.empty())
            continue;

        ic_entry->decref_infos.emplace_back(decref_info.first, std::move(merged_locations));
    }

    llvm::sys::Memory::InvalidateInstructionCache(slot_start, old_size);

    if (should_create_new_slot) {
        // printf("adding new slot: %d\n", new_slot_size);
        ic->slots.emplace_back(ic, (uint8_t*)ic_entry->start_addr + real_size, new_slot_size);
    }
}

void ICSlotRewrite::addDependenceOn(ICInvalidator& invalidator) {
    dependencies.push_back(std::make_pair(&invalidator, invalidator.version()));
}

int ICSlotRewrite::getSlotSize() {
    return ic_entry->size;
}

int ICSlotRewrite::getScratchRspOffset() {
    assert(ic->stack_info.scratch_size);
    return ic->stack_info.scratch_rsp_offset;
}

int ICSlotRewrite::getScratchSize() {
    return ic->stack_info.scratch_size;
}

TypeRecorder* ICSlotRewrite::getTypeRecorder() {
    return ic->type_recorder;
}

assembler::GenericRegister ICSlotRewrite::returnRegister() {
    return ic->return_register;
}



std::unique_ptr<ICSlotRewrite> ICInfo::startRewrite(const char* debug_name) {
    return ICSlotRewrite::create(this, debug_name);
}

ICSlotInfo* ICInfo::pickEntryForRewrite(const char* debug_name) {
    int num_slots = slots.size();
    for (int _i = 0; _i < num_slots; _i++) {
        int i = (_i + next_slot_to_try) % num_slots;

        ICSlotInfo& sinfo = slots[i];
        assert(sinfo.num_inside >= 0);
        if (sinfo.num_inside)
            continue;
        if (!sinfo.size)
            continue;

        if (VERBOSITY() >= 4) {
            printf("picking %s icentry to in-use slot %d at %p\n", debug_name, i, start_addr);
        }

        next_slot_to_try = i;
        return &sinfo;
    }
    if (VERBOSITY() >= 4)
        printf("not committing %s icentry since there are no available slots\n", debug_name);
    return NULL;
}

static llvm::DenseMap<void*, ICInfo*> ics_by_return_addr;

ICInfo::ICInfo(void* start_addr, void* slowpath_rtn_addr, void* continue_addr, StackInfo stack_info, int num_slots,
               int slot_size, llvm::CallingConv::ID calling_conv, LiveOutSet _live_outs,
               assembler::GenericRegister return_register, TypeRecorder* type_recorder,
               std::vector<Location> ic_global_decref_locations)
    : next_slot_to_try(0),
      stack_info(stack_info),
      num_slots(num_slots),
      calling_conv(calling_conv),
      live_outs(std::move(_live_outs)),
      return_register(return_register),
      type_recorder(type_recorder),
      retry_in(0),
      retry_backoff(1),
      times_rewritten(0),
      ic_global_decref_locations(std::move(ic_global_decref_locations)),
      start_addr(start_addr),
      slowpath_rtn_addr(slowpath_rtn_addr),
      continue_addr(continue_addr) {
    // slots.reserve(10);
    slots.emplace_back(this, (uint8_t*)start_addr, num_slots * slot_size);
    if (slowpath_rtn_addr && !this->ic_global_decref_locations.empty())
        slowpath_decref_info = DecrefInfo((uint64_t)slowpath_rtn_addr, this->ic_global_decref_locations);
}

ICInfo::~ICInfo() {
    for (auto& slot : slots) {
        for (auto invalidator : slot.invalidators) {
            assert(invalidator->dependents.count(&slot));
            invalidator->dependents.erase(&slot);
        }
    }
}

DecrefInfo::DecrefInfo(uint64_t ip, std::vector<Location> locations) : ip(ip) {
    addDecrefInfoEntry(ip, std::move(locations));
}

void DecrefInfo::reset() {
    if (ip) {
        removeDecrefInfoEntry(ip);
        ip = 0;
    }
}

std::unique_ptr<ICInfo> registerCompiledPatchpoint(uint8_t* start_addr, uint8_t* slowpath_start_addr,
                                                   uint8_t* continue_addr, uint8_t* slowpath_rtn_addr,
                                                   const ICSetupInfo* ic, StackInfo stack_info, LiveOutSet live_outs,
                                                   std::vector<Location> decref_info) {
    assert(slowpath_start_addr - start_addr >= ic->num_slots * ic->slot_size);
    assert(slowpath_rtn_addr > slowpath_start_addr);
    assert(slowpath_rtn_addr <= start_addr + ic->totalSize());

    assembler::GenericRegister return_register;
    assert(ic->getCallingConvention() == llvm::CallingConv::C
           || ic->getCallingConvention() == llvm::CallingConv::PreserveAll);

    if (ic->hasReturnValue()) {
        static const int DWARF_RAX = 0;
        // It's possible that the return value doesn't get used, in which case
        // we can avoid copying back into RAX at the end
        live_outs.clear(DWARF_RAX);

        // TODO we only need to do this if 0 was in live_outs, since if it wasn't, that indicates
        // the return value won't be used and we can optimize based on that.
        return_register = assembler::RAX;
    }

    // we can let the user just slide down the nop section, but instead
    // emit jumps to the end.
    // Not sure if this is worth it or not?
    for (int i = 0; i < ic->num_slots; i++) {
        uint8_t* start = start_addr + i * ic->slot_size;
        // std::unique_ptr<MCWriter> writer(createMCWriter(start, ic->slot_size * (ic->num_slots - i), 0));
        // writer->emitNop();
        // writer->emitGuardFalse();

        Assembler writer(start, ic->slot_size);
        writer.nop();
        // writer.trap();
        // writer.jmp(JumpDestination::fromStart(ic->slot_size * (ic->num_slots - i)));
        writer.jmp(JumpDestination::fromStart(slowpath_start_addr - start));
    }

    ICInfo* icinfo
        = new ICInfo(start_addr, slowpath_rtn_addr, continue_addr, stack_info, ic->num_slots, ic->slot_size,
                     ic->getCallingConvention(), std::move(live_outs), return_register, ic->type_recorder, decref_info);

    assert(!ics_by_return_addr.count(slowpath_rtn_addr));
    ics_by_return_addr[slowpath_rtn_addr] = icinfo;

    registerGCTrackedICInfo(icinfo);

    return std::unique_ptr<ICInfo>(icinfo);
}

void deregisterCompiledPatchpoint(ICInfo* ic) {
    ic->clearAll();

    assert(ics_by_return_addr[ic->slowpath_rtn_addr] == ic);
    ics_by_return_addr.erase(ic->slowpath_rtn_addr);

    deregisterGCTrackedICInfo(ic);
}

ICInfo* getICInfo(void* rtn_addr) {
    // TODO: load this from the CF instead of tracking it separately
    auto&& it = ics_by_return_addr.find(rtn_addr);
    if (it == ics_by_return_addr.end())
        return NULL;
    return it->second;
}

void ICInfo::clear(ICSlotInfo* icentry) {
    assert(icentry);

    uint8_t* start = (uint8_t*)icentry->start_addr;

    if (VERBOSITY() >= 4)
        printf("clearing patchpoint %p, slot at %p\n", start_addr, start);

    Assembler writer(start, icentry->size);
    writer.nop();
    writer.jmp(JumpDestination::fromStart(icentry->size));
    assert(writer.bytesWritten() <= IC_INVALDITION_HEADER_SIZE);

    for (auto p : icentry->gc_references) {
        Py_DECREF(p);
    }
    icentry->gc_references.clear();

    // std::unique_ptr<MCWriter> writer(createMCWriter(start, getSlotSize(), 0));
    // writer->emitNop();
    // writer->emitGuardFalse();

    // writer->endWithSlowpath();
    llvm::sys::Memory::InvalidateInstructionCache(start, icentry->size);
}

bool ICInfo::shouldAttempt() {
    if (currently_rewriting)
        return false;

    if (retry_in) {
        retry_in--;
        return false;
    }
    // Note(kmod): in some pathological deeply-recursive cases, it's important that we set the
    // retry counter even if we attempt it again.  We could probably handle this by setting
    // the backoff to 0 on commit, and then setting the retry to the backoff here.

    return !isMegamorphic() && ENABLE_ICS;
}

bool ICInfo::isMegamorphic() {
    return times_rewritten >= IC_MEGAMORPHIC_THRESHOLD;
}

static llvm::DenseMap<AST*, ICInfo*> ics_by_ast_node;

ICInfo* ICInfo::getICInfoForNode(AST* node) {
    auto&& it = ics_by_ast_node.find(node);
    if (it != ics_by_ast_node.end())
        return it->second;
    return NULL;
}
void ICInfo::associateNodeWithICInfo(AST* node) {
    ics_by_ast_node[node] = this;
}
void ICInfo::appendDecrefInfosTo(std::vector<DecrefInfo>& dest_decref_infos) {
    if (slowpath_decref_info.ip)
        dest_decref_infos.emplace_back(std::move(slowpath_decref_info));
    for (auto&& slot : slots) {
        for (DecrefInfo& decref_info : slot.decref_infos) {
            dest_decref_infos.emplace_back(std::move(decref_info));
            assert(decref_info.ip == 0 && "this can only happen if we copied instead of moved the value");
        }
        slot.decref_infos.clear();
    }
}

void clearAllICs() {
    for (auto&& p : ics_by_return_addr) {
        p.second->clearAll();
    }
}
}
