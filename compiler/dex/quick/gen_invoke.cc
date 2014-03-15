/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "dex/compiler_ir.h"
#include "dex/frontend.h"
#include "dex/quick/dex_file_method_inliner.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"
#include "dex_file-inl.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "invoke_type.h"
#include "mirror/array.h"
#include "mirror/string.h"
#include "mir_to_lir-inl.h"
#include "x86/codegen_x86.h"

namespace art {

/*
 * This source files contains "gen" codegen routines that should
 * be applicable to most targets.  Only mid-level support utilities
 * and "op" calls may be used here.
 */

void Mir2Lir::AddIntrinsicLaunchpad(CallInfo* info, LIR* branch, LIR* resume) {
  class IntrinsicLaunchpadPath : public Mir2Lir::LIRSlowPath {
   public:
    IntrinsicLaunchpadPath(Mir2Lir* m2l, CallInfo* info, LIR* branch, LIR* resume = nullptr)
        : LIRSlowPath(m2l, info->offset, branch, resume), info_(info) {
    }

    void Compile() {
      m2l_->ResetRegPool();
      m2l_->ResetDefTracking();
      LIR* label = GenerateTargetLabel();
      label->opcode = kPseudoIntrinsicRetry;
      // NOTE: GenInvokeNoInline() handles MarkSafepointPC.
      m2l_->GenInvokeNoInline(info_);
      if (cont_ != nullptr) {
        m2l_->OpUnconditionalBranch(cont_);
      }
    }

   private:
    CallInfo* const info_;
  };

  AddSlowPath(new (arena_) IntrinsicLaunchpadPath(this, info, branch, resume));
}

/*
 * To save scheduling time, helper calls are broken into two parts: generation of
 * the helper target address, and the actuall call to the helper.  Because x86
 * has a memory call operation, part 1 is a NOP for x86.  For other targets,
 * load arguments between the two parts.
 */
int Mir2Lir::CallHelperSetup(ThreadOffset helper_offset) {
  return (cu_->instruction_set == kX86) ? 0 : LoadHelper(helper_offset);
}

/* NOTE: if r_tgt is a temp, it will be freed following use */
LIR* Mir2Lir::CallHelper(int r_tgt, ThreadOffset helper_offset, bool safepoint_pc) {
  LIR* call_inst;
  if (cu_->instruction_set == kX86) {
    call_inst = OpThreadMem(kOpBlx, helper_offset);
  } else {
    call_inst = OpReg(kOpBlx, r_tgt);
    FreeTemp(r_tgt);
  }
  if (safepoint_pc) {
    MarkSafepointPC(call_inst);
  }
  return call_inst;
}

void Mir2Lir::CallRuntimeHelperImm(ThreadOffset helper_offset, int arg0, bool safepoint_pc) {
  int r_tgt = CallHelperSetup(helper_offset);
  LoadConstant(TargetReg(kArg0), arg0);
  ClobberCallerSave();
  CallHelper(r_tgt, helper_offset, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperReg(ThreadOffset helper_offset, int arg0, bool safepoint_pc) {
  int r_tgt = CallHelperSetup(helper_offset);
  OpRegCopy(TargetReg(kArg0), arg0);
  ClobberCallerSave();
  CallHelper(r_tgt, helper_offset, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperRegLocation(ThreadOffset helper_offset, RegLocation arg0,
                                           bool safepoint_pc) {
  int r_tgt = CallHelperSetup(helper_offset);
  if (arg0.wide) {
    LoadValueDirectWideFixed(arg0, arg0.fp ? TargetReg(kFArg0) : TargetReg(kArg0),
        arg0.fp ? TargetReg(kFArg1) : TargetReg(kArg1));
  } else {
    LoadValueDirectFixed(arg0, arg0.fp ? TargetReg(kFArg0) : TargetReg(kArg0));
  }
  ClobberCallerSave();
  CallHelper(r_tgt, helper_offset, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperImmImm(ThreadOffset helper_offset, int arg0, int arg1,
                                      bool safepoint_pc) {
  int r_tgt = CallHelperSetup(helper_offset);
  LoadConstant(TargetReg(kArg0), arg0);
  LoadConstant(TargetReg(kArg1), arg1);
  ClobberCallerSave();
  CallHelper(r_tgt, helper_offset, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperImmRegLocation(ThreadOffset helper_offset, int arg0,
                                              RegLocation arg1, bool safepoint_pc) {
  int r_tgt = CallHelperSetup(helper_offset);
  if (arg1.wide == 0) {
    LoadValueDirectFixed(arg1, TargetReg(kArg1));
  } else {
    LoadValueDirectWideFixed(arg1, TargetReg(kArg1), TargetReg(kArg2));
  }
  LoadConstant(TargetReg(kArg0), arg0);
  ClobberCallerSave();
  CallHelper(r_tgt, helper_offset, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperRegLocationImm(ThreadOffset helper_offset, RegLocation arg0, int arg1,
                                              bool safepoint_pc) {
  int r_tgt = CallHelperSetup(helper_offset);
  LoadValueDirectFixed(arg0, TargetReg(kArg0));
  LoadConstant(TargetReg(kArg1), arg1);
  ClobberCallerSave();
  CallHelper(r_tgt, helper_offset, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperImmReg(ThreadOffset helper_offset, int arg0, int arg1,
                                      bool safepoint_pc) {
  int r_tgt = CallHelperSetup(helper_offset);
  OpRegCopy(TargetReg(kArg1), arg1);
  LoadConstant(TargetReg(kArg0), arg0);
  ClobberCallerSave();
  CallHelper(r_tgt, helper_offset, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperRegImm(ThreadOffset helper_offset, int arg0, int arg1,
                                      bool safepoint_pc) {
  int r_tgt = CallHelperSetup(helper_offset);
  OpRegCopy(TargetReg(kArg0), arg0);
  LoadConstant(TargetReg(kArg1), arg1);
  ClobberCallerSave();
  CallHelper(r_tgt, helper_offset, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperImmMethod(ThreadOffset helper_offset, int arg0, bool safepoint_pc) {
  int r_tgt = CallHelperSetup(helper_offset);
  LoadCurrMethodDirect(TargetReg(kArg1));
  LoadConstant(TargetReg(kArg0), arg0);
  ClobberCallerSave();
  CallHelper(r_tgt, helper_offset, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperRegMethod(ThreadOffset helper_offset, int arg0, bool safepoint_pc) {
  int r_tgt = CallHelperSetup(helper_offset);
  DCHECK_NE(TargetReg(kArg1), arg0);
  if (TargetReg(kArg0) != arg0) {
    OpRegCopy(TargetReg(kArg0), arg0);
  }
  LoadCurrMethodDirect(TargetReg(kArg1));
  ClobberCallerSave();
  CallHelper(r_tgt, helper_offset, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperRegMethodRegLocation(ThreadOffset helper_offset, int arg0,
                                                    RegLocation arg2, bool safepoint_pc) {
  int r_tgt = CallHelperSetup(helper_offset);
  DCHECK_NE(TargetReg(kArg1), arg0);
  if (TargetReg(kArg0) != arg0) {
    OpRegCopy(TargetReg(kArg0), arg0);
  }
  LoadCurrMethodDirect(TargetReg(kArg1));
  LoadValueDirectFixed(arg2, TargetReg(kArg2));
  ClobberCallerSave();
  CallHelper(r_tgt, helper_offset, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperRegLocationRegLocation(ThreadOffset helper_offset, RegLocation arg0,
                                                      RegLocation arg1, bool safepoint_pc) {
  int r_tgt = CallHelperSetup(helper_offset);
  if (arg0.wide == 0) {
    LoadValueDirectFixed(arg0, arg0.fp ? TargetReg(kFArg0) : TargetReg(kArg0));
    if (arg1.wide == 0) {
      if (cu_->instruction_set == kMips) {
        LoadValueDirectFixed(arg1, arg1.fp ? TargetReg(kFArg2) : TargetReg(kArg1));
      } else {
        LoadValueDirectFixed(arg1, TargetReg(kArg1));
      }
    } else {
      if (cu_->instruction_set == kMips) {
        LoadValueDirectWideFixed(arg1, arg1.fp ? TargetReg(kFArg2) : TargetReg(kArg1), arg1.fp ? TargetReg(kFArg3) : TargetReg(kArg2));
      } else {
        LoadValueDirectWideFixed(arg1, TargetReg(kArg1), TargetReg(kArg2));
      }
    }
  } else {
    LoadValueDirectWideFixed(arg0, arg0.fp ? TargetReg(kFArg0) : TargetReg(kArg0), arg0.fp ? TargetReg(kFArg1) : TargetReg(kArg1));
    if (arg1.wide == 0) {
      LoadValueDirectFixed(arg1, arg1.fp ? TargetReg(kFArg2) : TargetReg(kArg2));
    } else {
      LoadValueDirectWideFixed(arg1, arg1.fp ? TargetReg(kFArg2) : TargetReg(kArg2), arg1.fp ? TargetReg(kFArg3) : TargetReg(kArg3));
    }
  }
  ClobberCallerSave();
  CallHelper(r_tgt, helper_offset, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperRegReg(ThreadOffset helper_offset, int arg0, int arg1,
                                      bool safepoint_pc) {
  int r_tgt = CallHelperSetup(helper_offset);
  DCHECK_NE(TargetReg(kArg0), arg1);  // check copy into arg0 won't clobber arg1
  OpRegCopy(TargetReg(kArg0), arg0);
  OpRegCopy(TargetReg(kArg1), arg1);
  ClobberCallerSave();
  CallHelper(r_tgt, helper_offset, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperRegRegImm(ThreadOffset helper_offset, int arg0, int arg1,
                                         int arg2, bool safepoint_pc) {
  int r_tgt = CallHelperSetup(helper_offset);
  DCHECK_NE(TargetReg(kArg0), arg1);  // check copy into arg0 won't clobber arg1
  OpRegCopy(TargetReg(kArg0), arg0);
  OpRegCopy(TargetReg(kArg1), arg1);
  LoadConstant(TargetReg(kArg2), arg2);
  ClobberCallerSave();
  CallHelper(r_tgt, helper_offset, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperImmMethodRegLocation(ThreadOffset helper_offset,
                                                    int arg0, RegLocation arg2, bool safepoint_pc) {
  int r_tgt = CallHelperSetup(helper_offset);
  LoadValueDirectFixed(arg2, TargetReg(kArg2));
  LoadCurrMethodDirect(TargetReg(kArg1));
  LoadConstant(TargetReg(kArg0), arg0);
  ClobberCallerSave();
  CallHelper(r_tgt, helper_offset, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperImmMethodImm(ThreadOffset helper_offset, int arg0,
                                            int arg2, bool safepoint_pc) {
  int r_tgt = CallHelperSetup(helper_offset);
  LoadCurrMethodDirect(TargetReg(kArg1));
  LoadConstant(TargetReg(kArg2), arg2);
  LoadConstant(TargetReg(kArg0), arg0);
  ClobberCallerSave();
  CallHelper(r_tgt, helper_offset, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperImmRegLocationRegLocation(ThreadOffset helper_offset,
                                                         int arg0, RegLocation arg1,
                                                         RegLocation arg2, bool safepoint_pc) {
  int r_tgt = CallHelperSetup(helper_offset);
  DCHECK_EQ(arg1.wide, 0U);
  LoadValueDirectFixed(arg1, TargetReg(kArg1));
  if (arg2.wide == 0) {
    LoadValueDirectFixed(arg2, TargetReg(kArg2));
  } else {
    LoadValueDirectWideFixed(arg2, TargetReg(kArg2), TargetReg(kArg3));
  }
  LoadConstant(TargetReg(kArg0), arg0);
  ClobberCallerSave();
  CallHelper(r_tgt, helper_offset, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperRegLocationRegLocationRegLocation(ThreadOffset helper_offset,
                                                                 RegLocation arg0, RegLocation arg1,
                                                                 RegLocation arg2,
                                                                 bool safepoint_pc) {
  int r_tgt = CallHelperSetup(helper_offset);
  DCHECK_EQ(arg0.wide, 0U);
  LoadValueDirectFixed(arg0, TargetReg(kArg0));
  DCHECK_EQ(arg1.wide, 0U);
  LoadValueDirectFixed(arg1, TargetReg(kArg1));
  DCHECK_EQ(arg1.wide, 0U);
  LoadValueDirectFixed(arg2, TargetReg(kArg2));
  ClobberCallerSave();
  CallHelper(r_tgt, helper_offset, safepoint_pc);
}

/*
 * If there are any ins passed in registers that have not been promoted
 * to a callee-save register, flush them to the frame.  Perform intial
 * assignment of promoted arguments.
 *
 * ArgLocs is an array of location records describing the incoming arguments
 * with one location record per word of argument.
 */
void Mir2Lir::FlushIns(RegLocation* ArgLocs, RegLocation rl_method) {
  /*
   * Dummy up a RegLocation for the incoming Method*
   * It will attempt to keep kArg0 live (or copy it to home location
   * if promoted).
   */
  RegLocation rl_src = rl_method;
  rl_src.location = kLocPhysReg;
  rl_src.reg = RegStorage(RegStorage::k32BitSolo, TargetReg(kArg0));
  rl_src.home = false;
  MarkLive(rl_src.reg.GetReg(), rl_src.s_reg_low);
  StoreValue(rl_method, rl_src);
  // If Method* has been promoted, explicitly flush
  if (rl_method.location == kLocPhysReg) {
    StoreWordDisp(TargetReg(kSp), 0, TargetReg(kArg0));
  }

  if (cu_->num_ins == 0) {
    return;
  }

  int start_vreg = cu_->num_dalvik_registers - cu_->num_ins;
  /*
   * Copy incoming arguments to their proper home locations.
   * NOTE: an older version of dx had an issue in which
   * it would reuse static method argument registers.
   * This could result in the same Dalvik virtual register
   * being promoted to both core and fp regs. To account for this,
   * we only copy to the corresponding promoted physical register
   * if it matches the type of the SSA name for the incoming
   * argument.  It is also possible that long and double arguments
   * end up half-promoted.  In those cases, we must flush the promoted
   * half to memory as well.
   */
  for (int i = 0; i < cu_->num_ins; i++) {
    PromotionMap* v_map = &promotion_map_[start_vreg + i];
    int reg = GetArgMappingToPhysicalReg(i);

    if (reg != INVALID_REG) {
      // If arriving in register
      bool need_flush = true;
      RegLocation* t_loc = &ArgLocs[i];
      if ((v_map->core_location == kLocPhysReg) && !t_loc->fp) {
        OpRegCopy(v_map->core_reg, reg);
        need_flush = false;
      } else if ((v_map->fp_location == kLocPhysReg) && t_loc->fp) {
        OpRegCopy(v_map->FpReg, reg);
        need_flush = false;
      } else {
        need_flush = true;
      }

      // For wide args, force flush if not fully promoted
      if (t_loc->wide) {
        PromotionMap* p_map = v_map + (t_loc->high_word ? -1 : +1);
        // Is only half promoted?
        need_flush |= (p_map->core_location != v_map->core_location) ||
            (p_map->fp_location != v_map->fp_location);
        if ((cu_->instruction_set == kThumb2) && t_loc->fp && !need_flush) {
          /*
           * In Arm, a double is represented as a pair of consecutive single float
           * registers starting at an even number.  It's possible that both Dalvik vRegs
           * representing the incoming double were independently promoted as singles - but
           * not in a form usable as a double.  If so, we need to flush - even though the
           * incoming arg appears fully in register.  At this point in the code, both
           * halves of the double are promoted.  Make sure they are in a usable form.
           */
          int lowreg_index = start_vreg + i + (t_loc->high_word ? -1 : 0);
          int low_reg = promotion_map_[lowreg_index].FpReg;
          int high_reg = promotion_map_[lowreg_index + 1].FpReg;
          if (((low_reg & 0x1) != 0) || (high_reg != (low_reg + 1))) {
            need_flush = true;
          }
        }
      }
      if (need_flush) {
        StoreBaseDisp(TargetReg(kSp), SRegOffset(start_vreg + i), reg, kWord);
      }
    } else {
      // If arriving in frame & promoted
      if (v_map->core_location == kLocPhysReg) {
        LoadWordDisp(TargetReg(kSp), SRegOffset(start_vreg + i),
                     v_map->core_reg);
      }
      if (v_map->fp_location == kLocPhysReg) {
        LoadWordDisp(TargetReg(kSp), SRegOffset(start_vreg + i),
                     v_map->FpReg);
      }
    }
  }
}

/*
 * Bit of a hack here - in the absence of a real scheduling pass,
 * emit the next instruction in static & direct invoke sequences.
 */
static int NextSDCallInsn(CompilationUnit* cu, CallInfo* info,
                          int state, const MethodReference& target_method,
                          uint32_t unused,
                          uintptr_t direct_code, uintptr_t direct_method,
                          InvokeType type) {
  Mir2Lir* cg = static_cast<Mir2Lir*>(cu->cg.get());
  if (direct_code != 0 && direct_method != 0) {
    switch (state) {
    case 0:  // Get the current Method* [sets kArg0]
      if (direct_code != static_cast<unsigned int>(-1)) {
        if (cu->instruction_set != kX86) {
          cg->LoadConstant(cg->TargetReg(kInvokeTgt), direct_code);
        }
      } else if (cu->instruction_set != kX86) {
        cg->LoadCodeAddress(target_method, type, kInvokeTgt);
      }
      if (direct_method != static_cast<unsigned int>(-1)) {
        cg->LoadConstant(cg->TargetReg(kArg0), direct_method);
      } else {
        cg->LoadMethodAddress(target_method, type, kArg0);
      }
      break;
    default:
      return -1;
    }
  } else {
    switch (state) {
    case 0:  // Get the current Method* [sets kArg0]
      // TUNING: we can save a reg copy if Method* has been promoted.
      cg->LoadCurrMethodDirect(cg->TargetReg(kArg0));
      break;
    case 1:  // Get method->dex_cache_resolved_methods_
      cg->LoadWordDisp(cg->TargetReg(kArg0),
        mirror::ArtMethod::DexCacheResolvedMethodsOffset().Int32Value(), cg->TargetReg(kArg0));
      // Set up direct code if known.
      if (direct_code != 0) {
        if (direct_code != static_cast<unsigned int>(-1)) {
          cg->LoadConstant(cg->TargetReg(kInvokeTgt), direct_code);
        } else if (cu->instruction_set != kX86) {
          CHECK_LT(target_method.dex_method_index, target_method.dex_file->NumMethodIds());
          cg->LoadCodeAddress(target_method, type, kInvokeTgt);
        }
      }
      break;
    case 2:  // Grab target method*
      CHECK_EQ(cu->dex_file, target_method.dex_file);
      cg->LoadWordDisp(cg->TargetReg(kArg0),
                       mirror::Array::DataOffset(sizeof(mirror::Object*)).Int32Value() +
                           (target_method.dex_method_index * 4),
                       cg-> TargetReg(kArg0));
      break;
    case 3:  // Grab the code from the method*
      if (cu->instruction_set != kX86) {
        if (direct_code == 0) {
          cg->LoadWordDisp(cg->TargetReg(kArg0),
                           mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset().Int32Value(),
                           cg->TargetReg(kInvokeTgt));
        }
        break;
      }
      // Intentional fallthrough for x86
    default:
      return -1;
    }
  }
  return state + 1;
}

/*
 * Bit of a hack here - in the absence of a real scheduling pass,
 * emit the next instruction in a virtual invoke sequence.
 * We can use kLr as a temp prior to target address loading
 * Note also that we'll load the first argument ("this") into
 * kArg1 here rather than the standard LoadArgRegs.
 */
static int NextVCallInsn(CompilationUnit* cu, CallInfo* info,
                         int state, const MethodReference& target_method,
                         uint32_t method_idx, uintptr_t unused, uintptr_t unused2,
                         InvokeType unused3) {
  Mir2Lir* cg = static_cast<Mir2Lir*>(cu->cg.get());
  /*
   * This is the fast path in which the target virtual method is
   * fully resolved at compile time.
   */
  switch (state) {
    case 0: {  // Get "this" [set kArg1]
      RegLocation  rl_arg = info->args[0];
      cg->LoadValueDirectFixed(rl_arg, cg->TargetReg(kArg1));
      break;
    }
    case 1:  // Is "this" null? [use kArg1]
      cg->GenNullCheck(cg->TargetReg(kArg1), info->opt_flags);
      // get this->klass_ [use kArg1, set kInvokeTgt]
      cg->LoadWordDisp(cg->TargetReg(kArg1), mirror::Object::ClassOffset().Int32Value(),
                       cg->TargetReg(kInvokeTgt));
      cg->MarkPossibleNullPointerException(info->opt_flags);
      break;
    case 2:  // Get this->klass_->vtable [usr kInvokeTgt, set kInvokeTgt]
      cg->LoadWordDisp(cg->TargetReg(kInvokeTgt), mirror::Class::VTableOffset().Int32Value(),
                       cg->TargetReg(kInvokeTgt));
      break;
    case 3:  // Get target method [use kInvokeTgt, set kArg0]
      cg->LoadWordDisp(cg->TargetReg(kInvokeTgt), (method_idx * 4) +
                       mirror::Array::DataOffset(sizeof(mirror::Object*)).Int32Value(),
                       cg->TargetReg(kArg0));
      break;
    case 4:  // Get the compiled code address [uses kArg0, sets kInvokeTgt]
      if (cu->instruction_set != kX86) {
        cg->LoadWordDisp(cg->TargetReg(kArg0),
                         mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset().Int32Value(),
                         cg->TargetReg(kInvokeTgt));
        break;
      }
      // Intentional fallthrough for X86
    default:
      return -1;
  }
  return state + 1;
}

/*
 * Emit the next instruction in an invoke interface sequence. This will do a lookup in the
 * class's IMT, calling either the actual method or art_quick_imt_conflict_trampoline if
 * more than one interface method map to the same index. Note also that we'll load the first
 * argument ("this") into kArg1 here rather than the standard LoadArgRegs.
 */
static int NextInterfaceCallInsn(CompilationUnit* cu, CallInfo* info, int state,
                                 const MethodReference& target_method,
                                 uint32_t method_idx, uintptr_t unused,
                                 uintptr_t direct_method, InvokeType unused2) {
  Mir2Lir* cg = static_cast<Mir2Lir*>(cu->cg.get());

  switch (state) {
    case 0:  // Set target method index in case of conflict [set kHiddenArg, kHiddenFpArg (x86)]
      CHECK_LT(target_method.dex_method_index, target_method.dex_file->NumMethodIds());
      cg->LoadConstant(cg->TargetReg(kHiddenArg), target_method.dex_method_index);
      if (cu->instruction_set == kX86) {
        cg->OpRegCopy(cg->TargetReg(kHiddenFpArg), cg->TargetReg(kHiddenArg));
      }
      break;
    case 1: {  // Get "this" [set kArg1]
      RegLocation  rl_arg = info->args[0];
      cg->LoadValueDirectFixed(rl_arg, cg->TargetReg(kArg1));
      break;
    }
    case 2:  // Is "this" null? [use kArg1]
      cg->GenNullCheck(cg->TargetReg(kArg1), info->opt_flags);
      // Get this->klass_ [use kArg1, set kInvokeTgt]
      cg->LoadWordDisp(cg->TargetReg(kArg1), mirror::Object::ClassOffset().Int32Value(),
                       cg->TargetReg(kInvokeTgt));
      cg->MarkPossibleNullPointerException(info->opt_flags);
      break;
    case 3:  // Get this->klass_->imtable [use kInvokeTgt, set kInvokeTgt]
      cg->LoadWordDisp(cg->TargetReg(kInvokeTgt), mirror::Class::ImTableOffset().Int32Value(),
                       cg->TargetReg(kInvokeTgt));
      break;
    case 4:  // Get target method [use kInvokeTgt, set kArg0]
      cg->LoadWordDisp(cg->TargetReg(kInvokeTgt), ((method_idx % ClassLinker::kImtSize) * 4) +
                       mirror::Array::DataOffset(sizeof(mirror::Object*)).Int32Value(),
                       cg->TargetReg(kArg0));
      break;
    case 5:  // Get the compiled code address [use kArg0, set kInvokeTgt]
      if (cu->instruction_set != kX86) {
        cg->LoadWordDisp(cg->TargetReg(kArg0),
                         mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset().Int32Value(),
                         cg->TargetReg(kInvokeTgt));
        break;
      }
      // Intentional fallthrough for X86
    default:
      return -1;
  }
  return state + 1;
}

static int NextInvokeInsnSP(CompilationUnit* cu, CallInfo* info, ThreadOffset trampoline,
                            int state, const MethodReference& target_method,
                            uint32_t method_idx) {
  Mir2Lir* cg = static_cast<Mir2Lir*>(cu->cg.get());
  /*
   * This handles the case in which the base method is not fully
   * resolved at compile time, we bail to a runtime helper.
   */
  if (state == 0) {
    if (cu->instruction_set != kX86) {
      // Load trampoline target
      cg->LoadWordDisp(cg->TargetReg(kSelf), trampoline.Int32Value(), cg->TargetReg(kInvokeTgt));
    }
    // Load kArg0 with method index
    CHECK_EQ(cu->dex_file, target_method.dex_file);
    cg->LoadConstant(cg->TargetReg(kArg0), target_method.dex_method_index);
    return 1;
  }
  return -1;
}

static int NextStaticCallInsnSP(CompilationUnit* cu, CallInfo* info,
                                int state,
                                const MethodReference& target_method,
                                uint32_t unused, uintptr_t unused2,
                                uintptr_t unused3, InvokeType unused4) {
  ThreadOffset trampoline = QUICK_ENTRYPOINT_OFFSET(pInvokeStaticTrampolineWithAccessCheck);
  return NextInvokeInsnSP(cu, info, trampoline, state, target_method, 0);
}

static int NextDirectCallInsnSP(CompilationUnit* cu, CallInfo* info, int state,
                                const MethodReference& target_method,
                                uint32_t unused, uintptr_t unused2,
                                uintptr_t unused3, InvokeType unused4) {
  ThreadOffset trampoline = QUICK_ENTRYPOINT_OFFSET(pInvokeDirectTrampolineWithAccessCheck);
  return NextInvokeInsnSP(cu, info, trampoline, state, target_method, 0);
}

static int NextSuperCallInsnSP(CompilationUnit* cu, CallInfo* info, int state,
                               const MethodReference& target_method,
                               uint32_t unused, uintptr_t unused2,
                               uintptr_t unused3, InvokeType unused4) {
  ThreadOffset trampoline = QUICK_ENTRYPOINT_OFFSET(pInvokeSuperTrampolineWithAccessCheck);
  return NextInvokeInsnSP(cu, info, trampoline, state, target_method, 0);
}

static int NextVCallInsnSP(CompilationUnit* cu, CallInfo* info, int state,
                           const MethodReference& target_method,
                           uint32_t unused, uintptr_t unused2,
                           uintptr_t unused3, InvokeType unused4) {
  ThreadOffset trampoline = QUICK_ENTRYPOINT_OFFSET(pInvokeVirtualTrampolineWithAccessCheck);
  return NextInvokeInsnSP(cu, info, trampoline, state, target_method, 0);
}

static int NextInterfaceCallInsnWithAccessCheck(CompilationUnit* cu,
                                                CallInfo* info, int state,
                                                const MethodReference& target_method,
                                                uint32_t unused, uintptr_t unused2,
                                                uintptr_t unused3, InvokeType unused4) {
  ThreadOffset trampoline = QUICK_ENTRYPOINT_OFFSET(pInvokeInterfaceTrampolineWithAccessCheck);
  return NextInvokeInsnSP(cu, info, trampoline, state, target_method, 0);
}

int Mir2Lir::LoadArgRegs(CallInfo* info, int call_state,
                         NextCallInsn next_call_insn,
                         const MethodReference& target_method,
                         uint32_t vtable_idx, uintptr_t direct_code,
                         uintptr_t direct_method, InvokeType type, bool skip_this) {
  int last_arg_reg = TargetReg(kArg3);
  int next_reg = TargetReg(kArg1);
  int next_arg = 0;
  if (skip_this) {
    next_reg++;
    next_arg++;
  }
  for (; (next_reg <= last_arg_reg) && (next_arg < info->num_arg_words); next_reg++) {
    RegLocation rl_arg = info->args[next_arg++];
    rl_arg = UpdateRawLoc(rl_arg);
    if (rl_arg.wide && (next_reg <= TargetReg(kArg2))) {
      LoadValueDirectWideFixed(rl_arg, next_reg, next_reg + 1);
      next_reg++;
      next_arg++;
    } else {
      if (rl_arg.wide) {
        rl_arg.wide = false;
        rl_arg.is_const = false;
      }
      LoadValueDirectFixed(rl_arg, next_reg);
    }
    call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                                direct_code, direct_method, type);
  }
  return call_state;
}

/*
 * Load up to 5 arguments, the first three of which will be in
 * kArg1 .. kArg3.  On entry kArg0 contains the current method pointer,
 * and as part of the load sequence, it must be replaced with
 * the target method pointer.  Note, this may also be called
 * for "range" variants if the number of arguments is 5 or fewer.
 */
int Mir2Lir::GenDalvikArgsNoRange(CallInfo* info,
                                  int call_state, LIR** pcrLabel, NextCallInsn next_call_insn,
                                  const MethodReference& target_method,
                                  uint32_t vtable_idx, uintptr_t direct_code,
                                  uintptr_t direct_method, InvokeType type, bool skip_this) {
  RegLocation rl_arg;

  /* If no arguments, just return */
  if (info->num_arg_words == 0)
    return call_state;

  call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                              direct_code, direct_method, type);

  DCHECK_LE(info->num_arg_words, 5);
  if (info->num_arg_words > 3) {
    int32_t next_use = 3;
    // Detect special case of wide arg spanning arg3/arg4
    RegLocation rl_use0 = info->args[0];
    RegLocation rl_use1 = info->args[1];
    RegLocation rl_use2 = info->args[2];
    if (((!rl_use0.wide && !rl_use1.wide) || rl_use0.wide) &&
      rl_use2.wide) {
      int reg = -1;
      // Wide spans, we need the 2nd half of uses[2].
      rl_arg = UpdateLocWide(rl_use2);
      if (rl_arg.location == kLocPhysReg) {
        reg = rl_arg.reg.GetHighReg();
      } else {
        // kArg2 & rArg3 can safely be used here
        reg = TargetReg(kArg3);
        LoadWordDisp(TargetReg(kSp), SRegOffset(rl_arg.s_reg_low) + 4, reg);
        call_state = next_call_insn(cu_, info, call_state, target_method,
                                    vtable_idx, direct_code, direct_method, type);
      }
      StoreBaseDisp(TargetReg(kSp), (next_use + 1) * 4, reg, kWord);
      call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                                  direct_code, direct_method, type);
      next_use++;
    }
    // Loop through the rest
    while (next_use < info->num_arg_words) {
      int low_reg;
      int high_reg = -1;
      rl_arg = info->args[next_use];
      rl_arg = UpdateRawLoc(rl_arg);
      if (rl_arg.location == kLocPhysReg) {
        low_reg = rl_arg.reg.GetReg();
        if (rl_arg.wide) {
          high_reg = rl_arg.reg.GetHighReg();
        }
      } else {
        low_reg = TargetReg(kArg2);
        if (rl_arg.wide) {
          high_reg = TargetReg(kArg3);
          LoadValueDirectWideFixed(rl_arg, low_reg, high_reg);
        } else {
          LoadValueDirectFixed(rl_arg, low_reg);
        }
        call_state = next_call_insn(cu_, info, call_state, target_method,
                                    vtable_idx, direct_code, direct_method, type);
      }
      int outs_offset = (next_use + 1) * 4;
      if (rl_arg.wide) {
        StoreBaseDispWide(TargetReg(kSp), outs_offset, low_reg, high_reg);
        next_use += 2;
      } else {
        StoreWordDisp(TargetReg(kSp), outs_offset, low_reg);
        next_use++;
      }
      call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                               direct_code, direct_method, type);
    }
  }

  call_state = LoadArgRegs(info, call_state, next_call_insn,
                           target_method, vtable_idx, direct_code, direct_method,
                           type, skip_this);

  if (pcrLabel) {
    *pcrLabel = GenNullCheck(TargetReg(kArg1), info->opt_flags);
  }
  return call_state;
}

/*
 * May have 0+ arguments (also used for jumbo).  Note that
 * source virtual registers may be in physical registers, so may
 * need to be flushed to home location before copying.  This
 * applies to arg3 and above (see below).
 *
 * Two general strategies:
 *    If < 20 arguments
 *       Pass args 3-18 using vldm/vstm block copy
 *       Pass arg0, arg1 & arg2 in kArg1-kArg3
 *    If 20+ arguments
 *       Pass args arg19+ using memcpy block copy
 *       Pass arg0, arg1 & arg2 in kArg1-kArg3
 *
 */
int Mir2Lir::GenDalvikArgsRange(CallInfo* info, int call_state,
                                LIR** pcrLabel, NextCallInsn next_call_insn,
                                const MethodReference& target_method,
                                uint32_t vtable_idx, uintptr_t direct_code, uintptr_t direct_method,
                                InvokeType type, bool skip_this) {
  // If we can treat it as non-range (Jumbo ops will use range form)
  if (info->num_arg_words <= 5)
    return GenDalvikArgsNoRange(info, call_state, pcrLabel,
                                next_call_insn, target_method, vtable_idx,
                                direct_code, direct_method, type, skip_this);
  /*
   * First load the non-register arguments.  Both forms expect all
   * of the source arguments to be in their home frame location, so
   * scan the s_reg names and flush any that have been promoted to
   * frame backing storage.
   */
  // Scan the rest of the args - if in phys_reg flush to memory
  for (int next_arg = 0; next_arg < info->num_arg_words;) {
    RegLocation loc = info->args[next_arg];
    if (loc.wide) {
      loc = UpdateLocWide(loc);
      if ((next_arg >= 2) && (loc.location == kLocPhysReg)) {
        StoreBaseDispWide(TargetReg(kSp), SRegOffset(loc.s_reg_low),
                          loc.reg.GetReg(), loc.reg.GetHighReg());
      }
      next_arg += 2;
    } else {
      loc = UpdateLoc(loc);
      if ((next_arg >= 3) && (loc.location == kLocPhysReg)) {
        StoreBaseDisp(TargetReg(kSp), SRegOffset(loc.s_reg_low),
                      loc.reg.GetReg(), kWord);
      }
      next_arg++;
    }
  }

  // Logic below assumes that Method pointer is at offset zero from SP.
  DCHECK_EQ(VRegOffset(static_cast<int>(kVRegMethodPtrBaseReg)), 0);

  // The first 3 arguments are passed via registers.
  // TODO: For 64-bit, instead of hardcoding 4 for Method* size, we should either
  // get size of uintptr_t or size of object reference according to model being used.
  int outs_offset = 4 /* Method* */ + (3 * sizeof(uint32_t));
  int start_offset = SRegOffset(info->args[3].s_reg_low);
  int regs_left_to_pass_via_stack = info->num_arg_words - 3;
  DCHECK_GT(regs_left_to_pass_via_stack, 0);

  if (cu_->instruction_set == kThumb2 && regs_left_to_pass_via_stack <= 16) {
    // Use vldm/vstm pair using kArg3 as a temp
    call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                             direct_code, direct_method, type);
    OpRegRegImm(kOpAdd, TargetReg(kArg3), TargetReg(kSp), start_offset);
    LIR* ld = OpVldm(TargetReg(kArg3), regs_left_to_pass_via_stack);
    // TUNING: loosen barrier
    ld->u.m.def_mask = ENCODE_ALL;
    SetMemRefType(ld, true /* is_load */, kDalvikReg);
    call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                             direct_code, direct_method, type);
    OpRegRegImm(kOpAdd, TargetReg(kArg3), TargetReg(kSp), 4 /* Method* */ + (3 * 4));
    call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                             direct_code, direct_method, type);
    LIR* st = OpVstm(TargetReg(kArg3), regs_left_to_pass_via_stack);
    SetMemRefType(st, false /* is_load */, kDalvikReg);
    st->u.m.def_mask = ENCODE_ALL;
    call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                             direct_code, direct_method, type);
  } else if (cu_->instruction_set == kX86) {
    int current_src_offset = start_offset;
    int current_dest_offset = outs_offset;

    while (regs_left_to_pass_via_stack > 0) {
      // This is based on the knowledge that the stack itself is 16-byte aligned.
      bool src_is_16b_aligned = (current_src_offset & 0xF) == 0;
      bool dest_is_16b_aligned = (current_dest_offset & 0xF) == 0;
      size_t bytes_to_move;

      /*
       * The amount to move defaults to 32-bit. If there are 4 registers left to move, then do a
       * a 128-bit move because we won't get the chance to try to aligned. If there are more than
       * 4 registers left to move, consider doing a 128-bit only if either src or dest are aligned.
       * We do this because we could potentially do a smaller move to align.
       */
      if (regs_left_to_pass_via_stack == 4 ||
          (regs_left_to_pass_via_stack > 4 && (src_is_16b_aligned || dest_is_16b_aligned))) {
        // Moving 128-bits via xmm register.
        bytes_to_move = sizeof(uint32_t) * 4;

        // Allocate a free xmm temp. Since we are working through the calling sequence,
        // we expect to have an xmm temporary available.
        int temp = AllocTempDouble();
        CHECK_GT(temp, 0);

        LIR* ld1 = nullptr;
        LIR* ld2 = nullptr;
        LIR* st1 = nullptr;
        LIR* st2 = nullptr;

        /*
         * The logic is similar for both loads and stores. If we have 16-byte alignment,
         * do an aligned move. If we have 8-byte alignment, then do the move in two
         * parts. This approach prevents possible cache line splits. Finally, fall back
         * to doing an unaligned move. In most cases we likely won't split the cache
         * line but we cannot prove it and thus take a conservative approach.
         */
        bool src_is_8b_aligned = (current_src_offset & 0x7) == 0;
        bool dest_is_8b_aligned = (current_dest_offset & 0x7) == 0;

        if (src_is_16b_aligned) {
          ld1 = OpMovRegMem(temp, TargetReg(kSp), current_src_offset, kMovA128FP);
        } else if (src_is_8b_aligned) {
          ld1 = OpMovRegMem(temp, TargetReg(kSp), current_src_offset, kMovLo128FP);
          ld2 = OpMovRegMem(temp, TargetReg(kSp), current_src_offset + (bytes_to_move >> 1), kMovHi128FP);
        } else {
          ld1 = OpMovRegMem(temp, TargetReg(kSp), current_src_offset, kMovU128FP);
        }

        if (dest_is_16b_aligned) {
          st1 = OpMovMemReg(TargetReg(kSp), current_dest_offset, temp, kMovA128FP);
        } else if (dest_is_8b_aligned) {
          st1 = OpMovMemReg(TargetReg(kSp), current_dest_offset, temp, kMovLo128FP);
          st2 = OpMovMemReg(TargetReg(kSp), current_dest_offset + (bytes_to_move >> 1), temp, kMovHi128FP);
        } else {
          st1 = OpMovMemReg(TargetReg(kSp), current_dest_offset, temp, kMovU128FP);
        }

        // TODO If we could keep track of aliasing information for memory accesses that are wider
        // than 64-bit, we wouldn't need to set up a barrier.
        if (ld1 != nullptr) {
          if (ld2 != nullptr) {
            // For 64-bit load we can actually set up the aliasing information.
            AnnotateDalvikRegAccess(ld1, current_src_offset >> 2, true, true);
            AnnotateDalvikRegAccess(ld2, (current_src_offset + (bytes_to_move >> 1)) >> 2, true, true);
          } else {
            // Set barrier for 128-bit load.
            SetMemRefType(ld1, true /* is_load */, kDalvikReg);
            ld1->u.m.def_mask = ENCODE_ALL;
          }
        }
        if (st1 != nullptr) {
          if (st2 != nullptr) {
            // For 64-bit store we can actually set up the aliasing information.
            AnnotateDalvikRegAccess(st1, current_dest_offset >> 2, false, true);
            AnnotateDalvikRegAccess(st2, (current_dest_offset + (bytes_to_move >> 1)) >> 2, false, true);
          } else {
            // Set barrier for 128-bit store.
            SetMemRefType(st1, false /* is_load */, kDalvikReg);
            st1->u.m.def_mask = ENCODE_ALL;
          }
        }

        // Free the temporary used for the data movement.
        FreeTemp(temp);
      } else {
        // Moving 32-bits via general purpose register.
        bytes_to_move = sizeof(uint32_t);

        // Instead of allocating a new temp, simply reuse one of the registers being used
        // for argument passing.
        int temp = TargetReg(kArg3);

        // Now load the argument VR and store to the outs.
        LoadWordDisp(TargetReg(kSp), current_src_offset, temp);
        StoreWordDisp(TargetReg(kSp), current_dest_offset, temp);
      }

      current_src_offset += bytes_to_move;
      current_dest_offset += bytes_to_move;
      regs_left_to_pass_via_stack -= (bytes_to_move >> 2);
    }
  } else {
    // Generate memcpy
    OpRegRegImm(kOpAdd, TargetReg(kArg0), TargetReg(kSp), outs_offset);
    OpRegRegImm(kOpAdd, TargetReg(kArg1), TargetReg(kSp), start_offset);
    CallRuntimeHelperRegRegImm(QUICK_ENTRYPOINT_OFFSET(pMemcpy), TargetReg(kArg0),
                               TargetReg(kArg1), (info->num_arg_words - 3) * 4, false);
  }

  call_state = LoadArgRegs(info, call_state, next_call_insn,
                           target_method, vtable_idx, direct_code, direct_method,
                           type, skip_this);

  call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                           direct_code, direct_method, type);
  if (pcrLabel) {
    *pcrLabel = GenNullCheck(TargetReg(kArg1), info->opt_flags);
  }
  return call_state;
}

RegLocation Mir2Lir::InlineTarget(CallInfo* info) {
  RegLocation res;
  if (info->result.location == kLocInvalid) {
    res = GetReturn(false);
  } else {
    res = info->result;
  }
  return res;
}

RegLocation Mir2Lir::InlineTargetWide(CallInfo* info) {
  RegLocation res;
  if (info->result.location == kLocInvalid) {
    res = GetReturnWide(false);
  } else {
    res = info->result;
  }
  return res;
}

bool Mir2Lir::GenInlinedCharAt(CallInfo* info) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  // Location of reference to data array
  int value_offset = mirror::String::ValueOffset().Int32Value();
  // Location of count
  int count_offset = mirror::String::CountOffset().Int32Value();
  // Starting offset within data array
  int offset_offset = mirror::String::OffsetOffset().Int32Value();
  // Start of char data with array_
  int data_offset = mirror::Array::DataOffset(sizeof(uint16_t)).Int32Value();

  RegLocation rl_obj = info->args[0];
  RegLocation rl_idx = info->args[1];
  rl_obj = LoadValue(rl_obj, kCoreReg);
  // X86 wants to avoid putting a constant index into a register.
  if (!(cu_->instruction_set == kX86 && rl_idx.is_const)) {
    rl_idx = LoadValue(rl_idx, kCoreReg);
  }
  int reg_max;
  GenNullCheck(rl_obj.reg.GetReg(), info->opt_flags);
  bool range_check = (!(info->opt_flags & MIR_IGNORE_RANGE_CHECK));
  LIR* range_check_branch = nullptr;
  int reg_off = INVALID_REG;
  int reg_ptr = INVALID_REG;
  if (cu_->instruction_set != kX86) {
    reg_off = AllocTemp();
    reg_ptr = AllocTemp();
    if (range_check) {
      reg_max = AllocTemp();
      LoadWordDisp(rl_obj.reg.GetReg(), count_offset, reg_max);
      MarkPossibleNullPointerException(info->opt_flags);
    }
    LoadWordDisp(rl_obj.reg.GetReg(), offset_offset, reg_off);
    MarkPossibleNullPointerException(info->opt_flags);
    LoadWordDisp(rl_obj.reg.GetReg(), value_offset, reg_ptr);
    if (range_check) {
      // Set up a launch pad to allow retry in case of bounds violation */
      OpRegReg(kOpCmp, rl_idx.reg.GetReg(), reg_max);
      FreeTemp(reg_max);
      range_check_branch = OpCondBranch(kCondUge, nullptr);
    }
    OpRegImm(kOpAdd, reg_ptr, data_offset);
  } else {
    if (range_check) {
      // On x86, we can compare to memory directly
      // Set up a launch pad to allow retry in case of bounds violation */
      if (rl_idx.is_const) {
        range_check_branch = OpCmpMemImmBranch(
            kCondUlt, INVALID_REG, rl_obj.reg.GetReg(), count_offset,
            mir_graph_->ConstantValue(rl_idx.orig_sreg), nullptr);
      } else {
        OpRegMem(kOpCmp, rl_idx.reg.GetReg(), rl_obj.reg.GetReg(), count_offset);
        range_check_branch = OpCondBranch(kCondUge, nullptr);
      }
    }
    reg_off = AllocTemp();
    reg_ptr = AllocTemp();
    LoadWordDisp(rl_obj.reg.GetReg(), offset_offset, reg_off);
    LoadWordDisp(rl_obj.reg.GetReg(), value_offset, reg_ptr);
  }
  if (rl_idx.is_const) {
    OpRegImm(kOpAdd, reg_off, mir_graph_->ConstantValue(rl_idx.orig_sreg));
  } else {
    OpRegReg(kOpAdd, reg_off, rl_idx.reg.GetReg());
  }
  FreeTemp(rl_obj.reg.GetReg());
  if (rl_idx.location == kLocPhysReg) {
    FreeTemp(rl_idx.reg.GetReg());
  }
  RegLocation rl_dest = InlineTarget(info);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  if (cu_->instruction_set != kX86) {
    LoadBaseIndexed(reg_ptr, reg_off, rl_result.reg.GetReg(), 1, kUnsignedHalf);
  } else {
    LoadBaseIndexedDisp(reg_ptr, reg_off, 1, data_offset, rl_result.reg.GetReg(),
                        INVALID_REG, kUnsignedHalf, INVALID_SREG);
  }
  FreeTemp(reg_off);
  FreeTemp(reg_ptr);
  StoreValue(rl_dest, rl_result);
  if (range_check) {
    DCHECK(range_check_branch != nullptr);
    info->opt_flags |= MIR_IGNORE_NULL_CHECK;  // Record that we've already null checked.
    AddIntrinsicLaunchpad(info, range_check_branch);
  }
  return true;
}

// Generates an inlined String.is_empty or String.length.
bool Mir2Lir::GenInlinedStringIsEmptyOrLength(CallInfo* info, bool is_empty) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  // dst = src.length();
  RegLocation rl_obj = info->args[0];
  rl_obj = LoadValue(rl_obj, kCoreReg);
  RegLocation rl_dest = InlineTarget(info);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  GenNullCheck(rl_obj.reg.GetReg(), info->opt_flags);
  LoadWordDisp(rl_obj.reg.GetReg(), mirror::String::CountOffset().Int32Value(),
               rl_result.reg.GetReg());
  MarkPossibleNullPointerException(info->opt_flags);
  if (is_empty) {
    // dst = (dst == 0);
    if (cu_->instruction_set == kThumb2) {
      int t_reg = AllocTemp();
      OpRegReg(kOpNeg, t_reg, rl_result.reg.GetReg());
      OpRegRegReg(kOpAdc, rl_result.reg.GetReg(), rl_result.reg.GetReg(), t_reg);
    } else {
      DCHECK_EQ(cu_->instruction_set, kX86);
      OpRegImm(kOpSub, rl_result.reg.GetReg(), 1);
      OpRegImm(kOpLsr, rl_result.reg.GetReg(), 31);
    }
  }
  StoreValue(rl_dest, rl_result);
  return true;
}

bool Mir2Lir::GenInlinedReverseBytes(CallInfo* info, OpSize size) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  RegLocation rl_src_i = info->args[0];
  RegLocation rl_dest = (size == kLong) ? InlineTargetWide(info) : InlineTarget(info);  // result reg
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  if (size == kLong) {
    RegLocation rl_i = LoadValueWide(rl_src_i, kCoreReg);
    int r_i_low = rl_i.reg.GetReg();
    if (rl_i.reg.GetReg() == rl_result.reg.GetReg()) {
      // First REV shall clobber rl_result.reg.GetReg(), save the value in a temp for the second REV.
      r_i_low = AllocTemp();
      OpRegCopy(r_i_low, rl_i.reg.GetReg());
    }
    OpRegReg(kOpRev, rl_result.reg.GetReg(), rl_i.reg.GetHighReg());
    OpRegReg(kOpRev, rl_result.reg.GetHighReg(), r_i_low);
    if (rl_i.reg.GetReg() == rl_result.reg.GetReg()) {
      FreeTemp(r_i_low);
    }
    StoreValueWide(rl_dest, rl_result);
  } else {
    DCHECK(size == kWord || size == kSignedHalf);
    OpKind op = (size == kWord) ? kOpRev : kOpRevsh;
    RegLocation rl_i = LoadValue(rl_src_i, kCoreReg);
    OpRegReg(op, rl_result.reg.GetReg(), rl_i.reg.GetReg());
    StoreValue(rl_dest, rl_result);
  }
  return true;
}

bool Mir2Lir::GenInlinedAbsInt(CallInfo* info) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  RegLocation rl_src = info->args[0];
  rl_src = LoadValue(rl_src, kCoreReg);
  RegLocation rl_dest = InlineTarget(info);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  int sign_reg = AllocTemp();
  // abs(x) = y<=x>>31, (x+y)^y.
  OpRegRegImm(kOpAsr, sign_reg, rl_src.reg.GetReg(), 31);
  OpRegRegReg(kOpAdd, rl_result.reg.GetReg(), rl_src.reg.GetReg(), sign_reg);
  OpRegReg(kOpXor, rl_result.reg.GetReg(), sign_reg);
  StoreValue(rl_dest, rl_result);
  return true;
}

bool Mir2Lir::GenInlinedAbsLong(CallInfo* info) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  if (cu_->instruction_set == kThumb2) {
    RegLocation rl_src = info->args[0];
    rl_src = LoadValueWide(rl_src, kCoreReg);
    RegLocation rl_dest = InlineTargetWide(info);
    RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
    int sign_reg = AllocTemp();
    // abs(x) = y<=x>>31, (x+y)^y.
    OpRegRegImm(kOpAsr, sign_reg, rl_src.reg.GetHighReg(), 31);
    OpRegRegReg(kOpAdd, rl_result.reg.GetReg(), rl_src.reg.GetReg(), sign_reg);
    OpRegRegReg(kOpAdc, rl_result.reg.GetHighReg(), rl_src.reg.GetHighReg(), sign_reg);
    OpRegReg(kOpXor, rl_result.reg.GetReg(), sign_reg);
    OpRegReg(kOpXor, rl_result.reg.GetHighReg(), sign_reg);
    StoreValueWide(rl_dest, rl_result);
    return true;
  } else {
    DCHECK_EQ(cu_->instruction_set, kX86);
    // Reuse source registers to avoid running out of temps
    RegLocation rl_src = info->args[0];
    rl_src = LoadValueWide(rl_src, kCoreReg);
    RegLocation rl_dest = InlineTargetWide(info);
    RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
    OpRegCopyWide(rl_result.reg.GetReg(), rl_result.reg.GetHighReg(), rl_src.reg.GetReg(), rl_src.reg.GetHighReg());
    FreeTemp(rl_src.reg.GetReg());
    FreeTemp(rl_src.reg.GetHighReg());
    int sign_reg = AllocTemp();
    // abs(x) = y<=x>>31, (x+y)^y.
    OpRegRegImm(kOpAsr, sign_reg, rl_result.reg.GetHighReg(), 31);
    OpRegReg(kOpAdd, rl_result.reg.GetReg(), sign_reg);
    OpRegReg(kOpAdc, rl_result.reg.GetHighReg(), sign_reg);
    OpRegReg(kOpXor, rl_result.reg.GetReg(), sign_reg);
    OpRegReg(kOpXor, rl_result.reg.GetHighReg(), sign_reg);
    StoreValueWide(rl_dest, rl_result);
    return true;
  }
}

bool Mir2Lir::GenInlinedAbsFloat(CallInfo* info) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  RegLocation rl_src = info->args[0];
  rl_src = LoadValue(rl_src, kCoreReg);
  RegLocation rl_dest = InlineTarget(info);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  int signMask = AllocTemp();
  LoadConstant(signMask, 0x7fffffff);
  OpRegRegReg(kOpAnd, rl_result.reg.GetReg(), rl_src.reg.GetReg(), signMask);
  FreeTemp(signMask);
  StoreValue(rl_dest, rl_result);
  return true;
}

bool Mir2Lir::GenInlinedAbsDouble(CallInfo* info) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  RegLocation rl_src = info->args[0];
  rl_src = LoadValueWide(rl_src, kCoreReg);
  RegLocation rl_dest = InlineTargetWide(info);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  OpRegCopyWide(rl_result.reg.GetReg(), rl_result.reg.GetHighReg(), rl_src.reg.GetReg(), rl_src.reg.GetHighReg());
  FreeTemp(rl_src.reg.GetReg());
  FreeTemp(rl_src.reg.GetHighReg());
  int signMask = AllocTemp();
  LoadConstant(signMask, 0x7fffffff);
  OpRegReg(kOpAnd, rl_result.reg.GetHighReg(), signMask);
  FreeTemp(signMask);
  StoreValueWide(rl_dest, rl_result);
  return true;
}

bool Mir2Lir::GenInlinedFloatCvt(CallInfo* info) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  RegLocation rl_src = info->args[0];
  RegLocation rl_dest = InlineTarget(info);
  StoreValue(rl_dest, rl_src);
  return true;
}

bool Mir2Lir::GenInlinedDoubleCvt(CallInfo* info) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  RegLocation rl_src = info->args[0];
  RegLocation rl_dest = InlineTargetWide(info);
  StoreValueWide(rl_dest, rl_src);
  return true;
}

/*
 * Fast String.indexOf(I) & (II).  Tests for simple case of char <= 0xFFFF,
 * otherwise bails to standard library code.
 */
bool Mir2Lir::GenInlinedIndexOf(CallInfo* info, bool zero_based) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  RegLocation rl_obj = info->args[0];
  RegLocation rl_char = info->args[1];
  if (rl_char.is_const && (mir_graph_->ConstantValue(rl_char) & ~0xFFFF) != 0) {
    // Code point beyond 0xFFFF. Punt to the real String.indexOf().
    return false;
  }

  ClobberCallerSave();
  LockCallTemps();  // Using fixed registers
  int reg_ptr = TargetReg(kArg0);
  int reg_char = TargetReg(kArg1);
  int reg_start = TargetReg(kArg2);

  LoadValueDirectFixed(rl_obj, reg_ptr);
  LoadValueDirectFixed(rl_char, reg_char);
  if (zero_based) {
    LoadConstant(reg_start, 0);
  } else {
    RegLocation rl_start = info->args[2];     // 3rd arg only present in III flavor of IndexOf.
    LoadValueDirectFixed(rl_start, reg_start);
  }
  int r_tgt = LoadHelper(QUICK_ENTRYPOINT_OFFSET(pIndexOf));
  GenNullCheck(reg_ptr, info->opt_flags);
  LIR* high_code_point_branch =
      rl_char.is_const ? nullptr : OpCmpImmBranch(kCondGt, reg_char, 0xFFFF, nullptr);
  // NOTE: not a safepoint
  OpReg(kOpBlx, r_tgt);
  if (!rl_char.is_const) {
    // Add the slow path for code points beyond 0xFFFF.
    DCHECK(high_code_point_branch != nullptr);
    LIR* resume_tgt = NewLIR0(kPseudoTargetLabel);
    info->opt_flags |= MIR_IGNORE_NULL_CHECK;  // Record that we've null checked.
    AddIntrinsicLaunchpad(info, high_code_point_branch, resume_tgt);
  } else {
    DCHECK_EQ(mir_graph_->ConstantValue(rl_char) & ~0xFFFF, 0);
    DCHECK(high_code_point_branch == nullptr);
  }
  RegLocation rl_return = GetReturn(false);
  RegLocation rl_dest = InlineTarget(info);
  StoreValue(rl_dest, rl_return);
  return true;
}

/* Fast string.compareTo(Ljava/lang/string;)I. */
bool Mir2Lir::GenInlinedStringCompareTo(CallInfo* info) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  ClobberCallerSave();
  LockCallTemps();  // Using fixed registers
  int reg_this = TargetReg(kArg0);
  int reg_cmp = TargetReg(kArg1);

  RegLocation rl_this = info->args[0];
  RegLocation rl_cmp = info->args[1];
  LoadValueDirectFixed(rl_this, reg_this);
  LoadValueDirectFixed(rl_cmp, reg_cmp);
  int r_tgt = (cu_->instruction_set != kX86) ?
      LoadHelper(QUICK_ENTRYPOINT_OFFSET(pStringCompareTo)) : 0;
  GenNullCheck(reg_this, info->opt_flags);
  info->opt_flags |= MIR_IGNORE_NULL_CHECK;  // Record that we've null checked.
  // TUNING: check if rl_cmp.s_reg_low is already null checked
  LIR* cmp_null_check_branch = OpCmpImmBranch(kCondEq, reg_cmp, 0, nullptr);
  AddIntrinsicLaunchpad(info, cmp_null_check_branch);
  // NOTE: not a safepoint
  if (cu_->instruction_set != kX86) {
    OpReg(kOpBlx, r_tgt);
  } else {
    OpThreadMem(kOpBlx, QUICK_ENTRYPOINT_OFFSET(pStringCompareTo));
  }
  RegLocation rl_return = GetReturn(false);
  RegLocation rl_dest = InlineTarget(info);
  StoreValue(rl_dest, rl_return);
  return true;
}

bool Mir2Lir::GenInlinedCurrentThread(CallInfo* info) {
  RegLocation rl_dest = InlineTarget(info);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  ThreadOffset offset = Thread::PeerOffset();
  if (cu_->instruction_set == kThumb2 || cu_->instruction_set == kMips) {
    LoadWordDisp(TargetReg(kSelf), offset.Int32Value(), rl_result.reg.GetReg());
  } else {
    CHECK(cu_->instruction_set == kX86);
    reinterpret_cast<X86Mir2Lir*>(this)->OpRegThreadMem(kOpMov, rl_result.reg.GetReg(), offset);
  }
  StoreValue(rl_dest, rl_result);
  return true;
}

bool Mir2Lir::GenInlinedUnsafeGet(CallInfo* info,
                                  bool is_long, bool is_volatile) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  // Unused - RegLocation rl_src_unsafe = info->args[0];
  RegLocation rl_src_obj = info->args[1];  // Object
  RegLocation rl_src_offset = info->args[2];  // long low
  rl_src_offset.wide = 0;  // ignore high half in info->args[3]
  RegLocation rl_dest = is_long ? InlineTargetWide(info) : InlineTarget(info);  // result reg
  if (is_volatile) {
    GenMemBarrier(kLoadLoad);
  }
  RegLocation rl_object = LoadValue(rl_src_obj, kCoreReg);
  RegLocation rl_offset = LoadValue(rl_src_offset, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  if (is_long) {
    OpRegReg(kOpAdd, rl_object.reg.GetReg(), rl_offset.reg.GetReg());
    LoadBaseDispWide(rl_object.reg.GetReg(), 0, rl_result.reg.GetReg(), rl_result.reg.GetHighReg(), INVALID_SREG);
    StoreValueWide(rl_dest, rl_result);
  } else {
    LoadBaseIndexed(rl_object.reg.GetReg(), rl_offset.reg.GetReg(), rl_result.reg.GetReg(), 0, kWord);
    StoreValue(rl_dest, rl_result);
  }
  return true;
}

bool Mir2Lir::GenInlinedUnsafePut(CallInfo* info, bool is_long,
                                  bool is_object, bool is_volatile, bool is_ordered) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  // Unused - RegLocation rl_src_unsafe = info->args[0];
  RegLocation rl_src_obj = info->args[1];  // Object
  RegLocation rl_src_offset = info->args[2];  // long low
  rl_src_offset.wide = 0;  // ignore high half in info->args[3]
  RegLocation rl_src_value = info->args[4];  // value to store
  if (is_volatile || is_ordered) {
    GenMemBarrier(kStoreStore);
  }
  RegLocation rl_object = LoadValue(rl_src_obj, kCoreReg);
  RegLocation rl_offset = LoadValue(rl_src_offset, kCoreReg);
  RegLocation rl_value;
  if (is_long) {
    rl_value = LoadValueWide(rl_src_value, kCoreReg);
    OpRegReg(kOpAdd, rl_object.reg.GetReg(), rl_offset.reg.GetReg());
    StoreBaseDispWide(rl_object.reg.GetReg(), 0, rl_value.reg.GetReg(), rl_value.reg.GetHighReg());
  } else {
    rl_value = LoadValue(rl_src_value, kCoreReg);
    StoreBaseIndexed(rl_object.reg.GetReg(), rl_offset.reg.GetReg(), rl_value.reg.GetReg(), 0, kWord);
  }

  // Free up the temp early, to ensure x86 doesn't run out of temporaries in MarkGCCard.
  FreeTemp(rl_offset.reg.GetReg());
  if (is_volatile) {
    GenMemBarrier(kStoreLoad);
  }
  if (is_object) {
    MarkGCCard(rl_value.reg.GetReg(), rl_object.reg.GetReg());
  }
  return true;
}

void Mir2Lir::GenInvoke(CallInfo* info) {
  DCHECK(cu_->compiler_driver->GetMethodInlinerMap() != nullptr);
  if (cu_->compiler_driver->GetMethodInlinerMap()->GetMethodInliner(cu_->dex_file)
      ->GenIntrinsic(this, info)) {
    return;
  }
  GenInvokeNoInline(info);
}

void Mir2Lir::GenInvokeNoInline(CallInfo* info) {
  int call_state = 0;
  LIR* null_ck;
  LIR** p_null_ck = NULL;
  NextCallInsn next_call_insn;
  FlushAllRegs();  /* Everything to home location */
  // Explicit register usage
  LockCallTemps();

  const MirMethodLoweringInfo& method_info = mir_graph_->GetMethodLoweringInfo(info->mir);
  cu_->compiler_driver->ProcessedInvoke(method_info.GetInvokeType(), method_info.StatsFlags());
  InvokeType original_type = static_cast<InvokeType>(method_info.GetInvokeType());
  info->type = static_cast<InvokeType>(method_info.GetSharpType());
  bool fast_path = method_info.FastPath();
  bool skip_this;
  if (info->type == kInterface) {
    next_call_insn = fast_path ? NextInterfaceCallInsn : NextInterfaceCallInsnWithAccessCheck;
    skip_this = fast_path;
  } else if (info->type == kDirect) {
    if (fast_path) {
      p_null_ck = &null_ck;
    }
    next_call_insn = fast_path ? NextSDCallInsn : NextDirectCallInsnSP;
    skip_this = false;
  } else if (info->type == kStatic) {
    next_call_insn = fast_path ? NextSDCallInsn : NextStaticCallInsnSP;
    skip_this = false;
  } else if (info->type == kSuper) {
    DCHECK(!fast_path);  // Fast path is a direct call.
    next_call_insn = NextSuperCallInsnSP;
    skip_this = false;
  } else {
    DCHECK_EQ(info->type, kVirtual);
    next_call_insn = fast_path ? NextVCallInsn : NextVCallInsnSP;
    skip_this = fast_path;
  }
  MethodReference target_method = method_info.GetTargetMethod();
  if (!info->is_range) {
    call_state = GenDalvikArgsNoRange(info, call_state, p_null_ck,
                                      next_call_insn, target_method, method_info.VTableIndex(),
                                      method_info.DirectCode(), method_info.DirectMethod(),
                                      original_type, skip_this);
  } else {
    call_state = GenDalvikArgsRange(info, call_state, p_null_ck,
                                    next_call_insn, target_method, method_info.VTableIndex(),
                                    method_info.DirectCode(), method_info.DirectMethod(),
                                    original_type, skip_this);
  }
  // Finish up any of the call sequence not interleaved in arg loading
  while (call_state >= 0) {
    call_state = next_call_insn(cu_, info, call_state, target_method, method_info.VTableIndex(),
                                method_info.DirectCode(), method_info.DirectMethod(), original_type);
  }
  LIR* call_inst;
  if (cu_->instruction_set != kX86) {
    call_inst = OpReg(kOpBlx, TargetReg(kInvokeTgt));
  } else {
    if (fast_path) {
      if (method_info.DirectCode() == static_cast<uintptr_t>(-1)) {
        // We can have the linker fixup a call relative.
        call_inst =
          reinterpret_cast<X86Mir2Lir*>(this)->CallWithLinkerFixup(target_method, info->type);
      } else {
        call_inst = OpMem(kOpBlx, TargetReg(kArg0),
                          mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset().Int32Value());
      }
    } else {
      ThreadOffset trampoline(-1);
      switch (info->type) {
      case kInterface:
        trampoline = QUICK_ENTRYPOINT_OFFSET(pInvokeInterfaceTrampolineWithAccessCheck);
        break;
      case kDirect:
        trampoline = QUICK_ENTRYPOINT_OFFSET(pInvokeDirectTrampolineWithAccessCheck);
        break;
      case kStatic:
        trampoline = QUICK_ENTRYPOINT_OFFSET(pInvokeStaticTrampolineWithAccessCheck);
        break;
      case kSuper:
        trampoline = QUICK_ENTRYPOINT_OFFSET(pInvokeSuperTrampolineWithAccessCheck);
        break;
      case kVirtual:
        trampoline = QUICK_ENTRYPOINT_OFFSET(pInvokeVirtualTrampolineWithAccessCheck);
        break;
      default:
        LOG(FATAL) << "Unexpected invoke type";
      }
      call_inst = OpThreadMem(kOpBlx, trampoline);
    }
  }
  MarkSafepointPC(call_inst);

  ClobberCallerSave();
  if (info->result.location != kLocInvalid) {
    // We have a following MOVE_RESULT - do it now.
    if (info->result.wide) {
      RegLocation ret_loc = GetReturnWide(info->result.fp);
      StoreValueWide(info->result, ret_loc);
    } else {
      RegLocation ret_loc = GetReturn(info->result.fp);
      StoreValue(info->result, ret_loc);
    }
  }
}

}  // namespace art
