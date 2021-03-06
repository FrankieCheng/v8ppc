// Copyright 2012 the V8 project authors. All rights reserved.
//
// Copyright IBM Corp. 2012, 2013. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "v8.h"

#include "ppc/lithium-codegen-ppc.h"
#include "ppc/lithium-gap-resolver-ppc.h"
#include "code-stubs.h"
#include "stub-cache.h"

namespace v8 {
namespace internal {


class SafepointGenerator : public CallWrapper {
 public:
  SafepointGenerator(LCodeGen* codegen,
                     LPointerMap* pointers,
                     Safepoint::DeoptMode mode)
      : codegen_(codegen),
        pointers_(pointers),
        deopt_mode_(mode) { }
  virtual ~SafepointGenerator() { }

  virtual void BeforeCall(int call_size) const { }

  virtual void AfterCall() const {
    codegen_->RecordSafepoint(pointers_, deopt_mode_);
  }

 private:
  LCodeGen* codegen_;
  LPointerMap* pointers_;
  Safepoint::DeoptMode deopt_mode_;
};


#define __ masm()->

bool LCodeGen::GenerateCode() {
  HPhase phase("Z_Code generation", chunk());
  ASSERT(is_unused());
  status_ = GENERATING;

  CodeStub::GenerateFPStubs();

  // Open a frame scope to indicate that there is a frame on the stack.  The
  // NONE indicates that the scope shouldn't actually generate code to set up
  // the frame (that is done in GeneratePrologue).
  FrameScope frame_scope(masm_, StackFrame::NONE);

  return GeneratePrologue() &&
      GenerateBody() &&
      GenerateDeferredCode() &&
#if 0
      GenerateDeoptJumpTable() &&  // not used on PPC
#endif
      GenerateSafepointTable();
}


void LCodeGen::FinishCode(Handle<Code> code) {
  ASSERT(is_done());
  code->set_stack_slots(GetStackSlotCount());
  code->set_safepoint_table_offset(safepoints_.GetCodeOffset());
  PopulateDeoptimizationData(code);
}


void LCodeGen::Abort(const char* reason) {
  info()->set_bailout_reason(reason);
  status_ = ABORTED;
}


void LCodeGen::Comment(const char* format, ...) {
  if (!FLAG_code_comments) return;
  char buffer[4 * KB];
  StringBuilder builder(buffer, ARRAY_SIZE(buffer));
  va_list arguments;
  va_start(arguments, format);
  builder.AddFormattedList(format, arguments);
  va_end(arguments);

  // Copy the string before recording it in the assembler to avoid
  // issues when the stack allocated buffer goes out of scope.
  size_t length = builder.position();
  Vector<char> copy = Vector<char>::New(length + 1);
  memcpy(copy.start(), builder.Finalize(), copy.length());
  masm()->RecordComment(copy.start());
}


bool LCodeGen::GeneratePrologue() {
  ASSERT(is_generating());

  ProfileEntryHookStub::MaybeCallEntryHook(masm_);

#ifdef DEBUG
  if (strlen(FLAG_stop_at) > 0 &&
      info_->function()->name()->IsEqualTo(CStrVector(FLAG_stop_at))) {
    __ stop("stop_at");
  }
#endif

  // r4: Callee's JS function.
  // cp: Callee's context.
  // fp: Caller's frame pointer.
  // lr: Caller's pc.

  // Strict mode functions and builtins need to replace the receiver
  // with undefined when called as functions (without an explicit
  // receiver object). r8 is zero for method calls and non-zero for
  // function calls.
  if (!info_->is_classic_mode() || info_->is_native()) {
    Label ok;
    __ cmpi(r8, Operand::Zero());
    __ beq(&ok);
    int receiver_offset = scope()->num_parameters() * kPointerSize;
    __ LoadRoot(r5, Heap::kUndefinedValueRootIndex);
    __ stw(r5, MemOperand(sp, receiver_offset));
    __ bind(&ok);
  }

  __ mflr(r0);
  __ Push(r0, fp, cp, r4);
  __ addi(fp, sp, Operand(2 * kPointerSize));  // Adjust FP to point to saved FP

  // Reserve space for the stack slots needed by the code.
  int slots = GetStackSlotCount();
  if (slots > 0) {
    if (FLAG_debug_code) {
      __ mov(r3, Operand(slots));
      __ mov(r5, Operand(kSlotsZapValue));
      __ mtctr(r3);
      Label loop;
      __ bind(&loop);
      __ push(r5);
      __ bdnz(&loop);
    } else {
      __ addi(sp, sp, Operand(-slots * kPointerSize));
    }
  }

  // Possibly allocate a local context.
  int heap_slots = scope()->num_heap_slots() - Context::MIN_CONTEXT_SLOTS;
  if (heap_slots > 0) {
    Comment(";;; Allocate local context");
    // Argument to NewContext is the function, which is in r4.
    __ push(r4);
    if (heap_slots <= FastNewContextStub::kMaximumSlots) {
      FastNewContextStub stub(heap_slots);
      __ CallStub(&stub);
    } else {
      __ CallRuntime(Runtime::kNewFunctionContext, 1);
    }
    RecordSafepoint(Safepoint::kNoLazyDeopt);
    // Context is returned in both r3 and cp.  It replaces the context
    // passed to us.  It's saved in the stack and kept live in cp.
    __ stw(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
    // Copy any necessary parameters into the context.
    int num_parameters = scope()->num_parameters();
    for (int i = 0; i < num_parameters; i++) {
      Variable* var = scope()->parameter(i);
      if (var->IsContextSlot()) {
        int parameter_offset = StandardFrameConstants::kCallerSPOffset +
            (num_parameters - 1 - i) * kPointerSize;
        // Load parameter from stack.
        __ lwz(r3, MemOperand(fp, parameter_offset));
        // Store it in the context.
        MemOperand target = ContextOperand(cp, var->index());
        __ stw(r3, target);
        // Update the write barrier. This clobbers r6 and r3.
        __ RecordWriteContextSlot(
            cp, target.offset(), r3, r6, kLRHasBeenSaved, kSaveFPRegs);
      }
    }
    Comment(";;; End allocate local context");
  }

  // Trace the call.
  if (FLAG_trace) {
    __ CallRuntime(Runtime::kTraceEnter, 0);
  }
  return !is_aborted();
}


bool LCodeGen::GenerateBody() {
  ASSERT(is_generating());
  bool emit_instructions = true;
  for (current_instruction_ = 0;
       !is_aborted() && current_instruction_ < instructions_->length();
       current_instruction_++) {
    LInstruction* instr = instructions_->at(current_instruction_);
    if (instr->IsLabel()) {
      LLabel* label = LLabel::cast(instr);
      emit_instructions = !label->HasReplacement();
    }

    if (emit_instructions) {
      Comment(";;; @%d: %s.", current_instruction_, instr->Mnemonic());
      instr->CompileToNative(this);
    }
  }
  EnsureSpaceForLazyDeopt();
  return !is_aborted();
}


bool LCodeGen::GenerateDeferredCode() {
  ASSERT(is_generating());
  if (deferred_.length() > 0) {
    for (int i = 0; !is_aborted() && i < deferred_.length(); i++) {
      LDeferredCode* code = deferred_[i];
      __ bind(code->entry());
      Comment(";;; Deferred code @%d: %s.",
              code->instruction_index(),
              code->instr()->Mnemonic());
      code->Generate();
      __ b(code->exit());
    }
  }

  // Deferred code is the last part of the instruction sequence. Mark
  // the generated code as done unless we bailed out.
  if (!is_aborted()) status_ = DONE;
  return !is_aborted();
}

// Currently unused on PPC
bool LCodeGen::GenerateDeoptJumpTable() {
  Abort("Unimplemented: GenerateDeoptJumpTable");
  return false;
}


bool LCodeGen::GenerateSafepointTable() {
  ASSERT(is_done());
  safepoints_.Emit(masm(), GetStackSlotCount());
  return !is_aborted();
}


Register LCodeGen::ToRegister(int index) const {
  return Register::FromAllocationIndex(index);
}


DoubleRegister LCodeGen::ToDoubleRegister(int index) const {
  return DoubleRegister::FromAllocationIndex(index);
}


Register LCodeGen::ToRegister(LOperand* op) const {
  ASSERT(op->IsRegister());
  return ToRegister(op->index());
}


Register LCodeGen::EmitLoadRegister(LOperand* op, Register scratch) {
  if (op->IsRegister()) {
    return ToRegister(op->index());
  } else if (op->IsConstantOperand()) {
    LConstantOperand* const_op = LConstantOperand::cast(op);
    HConstant* constant = chunk_->LookupConstant(const_op);
    Handle<Object> literal = constant->handle();
    Representation r = chunk_->LookupLiteralRepresentation(const_op);
    if (r.IsInteger32()) {
      ASSERT(literal->IsNumber());
      __ mov(scratch, Operand(static_cast<int32_t>(literal->Number())));
    } else if (r.IsDouble()) {
      Abort("EmitLoadRegister: Unsupported double immediate.");
    } else {
      ASSERT(r.IsTagged());
      if (literal->IsSmi()) {
        __ mov(scratch, Operand(literal));
      } else {
       __ LoadHeapObject(scratch, Handle<HeapObject>::cast(literal));
      }
    }
    return scratch;
  } else if (op->IsStackSlot() || op->IsArgument()) {
    __ lwz(scratch, ToMemOperand(op));
    return scratch;
  }
  UNREACHABLE();
  return scratch;
}


DoubleRegister LCodeGen::ToDoubleRegister(LOperand* op) const {
  ASSERT(op->IsDoubleRegister());
  return ToDoubleRegister(op->index());
}

  /*
DoubleRegister LCodeGen::EmitLoadDoubleRegister(LOperand* op,
                                                SwVfpRegister flt_scratch,
                                                DoubleRegister dbl_scratch) {
#ifdef PENGUIN_CLEANUP
  if (op->IsDoubleRegister()) {
    return ToDoubleRegister(op->index());
  } else if (op->IsConstantOperand()) {
    LConstantOperand* const_op = LConstantOperand::cast(op);
    HConstant* constant = chunk_->LookupConstant(const_op);
    Handle<Object> literal = constant->handle();
    Representation r = chunk_->LookupLiteralRepresentation(const_op);
    if (r.IsInteger32()) {
      ASSERT(literal->IsNumber());
      __ mov(ip, Operand(static_cast<int32_t>(literal->Number())));
      __ vmov(flt_scratch, ip);
      __ vcvt_f64_s32(dbl_scratch, flt_scratch);
      return dbl_scratch;
    } else if (r.IsDouble()) {
      Abort("unsupported double immediate");
    } else if (r.IsTagged()) {
      Abort("unsupported tagged immediate");
    }
  } else if (op->IsStackSlot() || op->IsArgument()) {
    // TODO(regis): Why is lfd not taking a MemOperand?
    // __ lfd(dbl_scratch, ToMemOperand(op));
    MemOperand mem_op = ToMemOperand(op);
    __ lfd(dbl_scratch, MemOperand(mem_op.rn(), mem_op.offset()));
    return dbl_scratch;
  }
  UNREACHABLE();
#else
  PPCPORT_UNIMPLEMENTED();
  __ fake_asm(fLITHIUM91);
#endif
  return dbl_scratch;
}
  */


Handle<Object> LCodeGen::ToHandle(LConstantOperand* op) const {
  HConstant* constant = chunk_->LookupConstant(op);
  ASSERT(chunk_->LookupLiteralRepresentation(op).IsTagged());
  return constant->handle();
}


bool LCodeGen::IsInteger32(LConstantOperand* op) const {
  return chunk_->LookupLiteralRepresentation(op).IsInteger32();
}


int LCodeGen::ToInteger32(LConstantOperand* op) const {
  HConstant* constant = chunk_->LookupConstant(op);
  ASSERT(chunk_->LookupLiteralRepresentation(op).IsInteger32());
  ASSERT(constant->HasInteger32Value());
  return constant->Integer32Value();
}


double LCodeGen::ToDouble(LConstantOperand* op) const {
  HConstant* constant = chunk_->LookupConstant(op);
  ASSERT(constant->HasDoubleValue());
  return constant->DoubleValue();
}


Operand LCodeGen::ToOperand(LOperand* op) {
  if (op->IsConstantOperand()) {
    LConstantOperand* const_op = LConstantOperand::cast(op);
    HConstant* constant = chunk()->LookupConstant(const_op);
    Representation r = chunk_->LookupLiteralRepresentation(const_op);
    if (r.IsInteger32()) {
      ASSERT(constant->HasInteger32Value());
      return Operand(constant->Integer32Value());
    } else if (r.IsDouble()) {
      Abort("ToOperand Unsupported double immediate.");
    }
    ASSERT(r.IsTagged());
    return Operand(constant->handle());
  } else if (op->IsRegister()) {
    return Operand(ToRegister(op));
  } else if (op->IsDoubleRegister()) {
    Abort("ToOperand IsDoubleRegister unimplemented");
    return Operand::Zero();
  }
  // Stack slots not implemented, use ToMemOperand instead.
  UNREACHABLE();
  return Operand::Zero();
}


MemOperand LCodeGen::ToMemOperand(LOperand* op) const {
  ASSERT(!op->IsRegister());
  ASSERT(!op->IsDoubleRegister());
  ASSERT(op->IsStackSlot() || op->IsDoubleStackSlot());
  int index = op->index();
  if (index >= 0) {
    // Local or spill slot. Skip the frame pointer, function, and
    // context in the fixed part of the frame.
    return MemOperand(fp, -(index + 3) * kPointerSize);
  } else {
    // Incoming parameter. Skip the return address.
    return MemOperand(fp, -(index - 1) * kPointerSize);
  }
}


MemOperand LCodeGen::ToHighMemOperand(LOperand* op) const {
  ASSERT(op->IsDoubleStackSlot());
  int index = op->index();
  if (index >= 0) {
    // Local or spill slot. Skip the frame pointer, function, context,
    // and the first word of the double in the fixed part of the frame.
    return MemOperand(fp, -(index + 3) * kPointerSize + kPointerSize);
  } else {
    // Incoming parameter. Skip the return address and the first word of
    // the double.
    return MemOperand(fp, -(index - 1) * kPointerSize + kPointerSize);
  }
}


void LCodeGen::WriteTranslation(LEnvironment* environment,
                                Translation* translation,
                                int* arguments_index,
                                int* arguments_count) {
  if (environment == NULL) return;

  // The translation includes one command per value in the environment.
  int translation_size = environment->values()->length();
  // The output frame height does not include the parameters.
  int height = translation_size - environment->parameter_count();

  // Function parameters are arguments to the outermost environment. The
  // arguments index points to the first element of a sequence of tagged
  // values on the stack that represent the arguments. This needs to be
  // kept in sync with the LArgumentsElements implementation.
  *arguments_index = -environment->parameter_count();
  *arguments_count = environment->parameter_count();

  WriteTranslation(environment->outer(),
                   translation,
                   arguments_index,
                   arguments_count);
  int closure_id = *info()->closure() != *environment->closure()
      ? DefineDeoptimizationLiteral(environment->closure())
      : Translation::kSelfLiteralId;

  switch (environment->frame_type()) {
    case JS_FUNCTION:
      translation->BeginJSFrame(environment->ast_id(), closure_id, height);
      break;
    case JS_CONSTRUCT:
      translation->BeginConstructStubFrame(closure_id, translation_size);
      break;
    case JS_GETTER:
      ASSERT(translation_size == 1);
      ASSERT(height == 0);
      translation->BeginGetterStubFrame(closure_id);
      break;
    case JS_SETTER:
      ASSERT(translation_size == 2);
      ASSERT(height == 0);
      translation->BeginSetterStubFrame(closure_id);
      break;
    case ARGUMENTS_ADAPTOR:
      translation->BeginArgumentsAdaptorFrame(closure_id, translation_size);
      break;
  }

  // Inlined frames which push their arguments cause the index to be
  // bumped and a new stack area to be used for materialization.
  if (environment->entry() != NULL &&
      environment->entry()->arguments_pushed()) {
    *arguments_index = *arguments_index < 0
        ? GetStackSlotCount()
        : *arguments_index + *arguments_count;
    *arguments_count = environment->entry()->arguments_count() + 1;
  }

  for (int i = 0; i < translation_size; ++i) {
    LOperand* value = environment->values()->at(i);
    // spilled_registers_ and spilled_double_registers_ are either
    // both NULL or both set.
    if (environment->spilled_registers() != NULL && value != NULL) {
      if (value->IsRegister() &&
          environment->spilled_registers()[value->index()] != NULL) {
        translation->MarkDuplicate();
        AddToTranslation(translation,
                         environment->spilled_registers()[value->index()],
                         environment->HasTaggedValueAt(i),
                         environment->HasUint32ValueAt(i),
                         *arguments_index,
                         *arguments_count);
      } else if (
          value->IsDoubleRegister() &&
          environment->spilled_double_registers()[value->index()] != NULL) {
        translation->MarkDuplicate();
        AddToTranslation(
            translation,
            environment->spilled_double_registers()[value->index()],
            false,
            false,
            *arguments_index,
            *arguments_count);
      }
    }

    AddToTranslation(translation,
                     value,
                     environment->HasTaggedValueAt(i),
                     environment->HasUint32ValueAt(i),
                     *arguments_index,
                     *arguments_count);
  }
}


void LCodeGen::AddToTranslation(Translation* translation,
                                LOperand* op,
                                bool is_tagged,
                                bool is_uint32,
                                int arguments_index,
                                int arguments_count) {
  if (op == NULL) {
    // TODO(twuerthinger): Introduce marker operands to indicate that this value
    // is not present and must be reconstructed from the deoptimizer. Currently
    // this is only used for the arguments object.
    translation->StoreArgumentsObject(arguments_index, arguments_count);
  } else if (op->IsStackSlot()) {
    if (is_tagged) {
      translation->StoreStackSlot(op->index());
    } else if (is_uint32) {
      translation->StoreUint32StackSlot(op->index());
    } else {
      translation->StoreInt32StackSlot(op->index());
    }
  } else if (op->IsDoubleStackSlot()) {
    translation->StoreDoubleStackSlot(op->index());
  } else if (op->IsArgument()) {
    ASSERT(is_tagged);
    int src_index = GetStackSlotCount() + op->index();
    translation->StoreStackSlot(src_index);
  } else if (op->IsRegister()) {
    Register reg = ToRegister(op);
    if (is_tagged) {
      translation->StoreRegister(reg);
    } else if (is_uint32) {
      translation->StoreUint32Register(reg);
    } else {
      translation->StoreInt32Register(reg);
    }
  } else if (op->IsDoubleRegister()) {
    DoubleRegister reg = ToDoubleRegister(op);
    translation->StoreDoubleRegister(reg);
  } else if (op->IsConstantOperand()) {
    HConstant* constant = chunk()->LookupConstant(LConstantOperand::cast(op));
    int src_index = DefineDeoptimizationLiteral(constant->handle());
    translation->StoreLiteral(src_index);
  } else {
    UNREACHABLE();
  }
}


void LCodeGen::CallCode(Handle<Code> code,
                        RelocInfo::Mode mode,
                        LInstruction* instr) {
  CallCodeGeneric(code, mode, instr, RECORD_SIMPLE_SAFEPOINT);
}


void LCodeGen::CallCodeGeneric(Handle<Code> code,
                               RelocInfo::Mode mode,
                               LInstruction* instr,
                               SafepointMode safepoint_mode) {
  ASSERT(instr != NULL);
  LPointerMap* pointers = instr->pointer_map();
  RecordPosition(pointers->position());
  __ Call(code, mode);
  RecordSafepointWithLazyDeopt(instr, safepoint_mode);

  // Signal that we don't inline smi code before these stubs in the
  // optimizing code generator.
  if (code->kind() == Code::BINARY_OP_IC ||
      code->kind() == Code::COMPARE_IC) {
    __ nop();
  }
}


void LCodeGen::CallRuntime(const Runtime::Function* function,
                           int num_arguments,
                           LInstruction* instr) {
  ASSERT(instr != NULL);
  LPointerMap* pointers = instr->pointer_map();
  ASSERT(pointers != NULL);
  RecordPosition(pointers->position());

  __ CallRuntime(function, num_arguments);
  RecordSafepointWithLazyDeopt(instr, RECORD_SIMPLE_SAFEPOINT);
}


void LCodeGen::CallRuntimeFromDeferred(Runtime::FunctionId id,
                                       int argc,
                                       LInstruction* instr) {
  __ CallRuntimeSaveDoubles(id);
  RecordSafepointWithRegisters(
      instr->pointer_map(), argc, Safepoint::kNoLazyDeopt);
}


void LCodeGen::RegisterEnvironmentForDeoptimization(LEnvironment* environment,
                                                    Safepoint::DeoptMode mode) {
  if (!environment->HasBeenRegistered()) {
    // Physical stack frame layout:
    // -x ............. -4  0 ..................................... y
    // [incoming arguments] [spill slots] [pushed outgoing arguments]

    // Layout of the environment:
    // 0 ..................................................... size-1
    // [parameters] [locals] [expression stack including arguments]

    // Layout of the translation:
    // 0 ........................................................ size - 1 + 4
    // [expression stack including arguments] [locals] [4 words] [parameters]
    // |>------------  translation_size ------------<|

    int frame_count = 0;
    int jsframe_count = 0;
    int args_index = 0;
    int args_count = 0;
    for (LEnvironment* e = environment; e != NULL; e = e->outer()) {
      ++frame_count;
      if (e->frame_type() == JS_FUNCTION) {
        ++jsframe_count;
      }
    }
    Translation translation(&translations_, frame_count, jsframe_count, zone());
    WriteTranslation(environment, &translation, &args_index, &args_count);
    int deoptimization_index = deoptimizations_.length();
    int pc_offset = masm()->pc_offset();
    environment->Register(deoptimization_index,
                          translation.index(),
                          (mode == Safepoint::kLazyDeopt) ? pc_offset : -1);
    deoptimizations_.Add(environment, zone());
  }
}


void LCodeGen::DeoptimizeIf(Condition cond, LEnvironment* environment,
                            CRegister cr) {
  RegisterEnvironmentForDeoptimization(environment, Safepoint::kNoLazyDeopt);
  ASSERT(environment->HasBeenRegistered());
  int id = environment->deoptimization_index();
  Address entry = Deoptimizer::GetDeoptimizationEntry(id, Deoptimizer::EAGER);
  if (entry == NULL) {
    Abort("bailout was not prepared");
    return;
  }

  ASSERT(FLAG_deopt_every_n_times < 2);  // Other values not supported on ARM.

  if (FLAG_deopt_every_n_times == 1 &&
      info_->shared_info()->opt_count() == id) {
    __ Jump(entry, RelocInfo::RUNTIME_ENTRY);
    return;
  }

  if (FLAG_trap_on_deopt) __ stop("trap_on_deopt", cond, kDefaultStopCode, cr);

  __ Jump(entry, RelocInfo::RUNTIME_ENTRY, cond, cr);
}


void LCodeGen::PopulateDeoptimizationData(Handle<Code> code) {
  int length = deoptimizations_.length();
  if (length == 0) return;
  Handle<DeoptimizationInputData> data =
      factory()->NewDeoptimizationInputData(length, TENURED);

  Handle<ByteArray> translations = translations_.CreateByteArray();
  data->SetTranslationByteArray(*translations);
  data->SetInlinedFunctionCount(Smi::FromInt(inlined_function_count_));

  Handle<FixedArray> literals =
      factory()->NewFixedArray(deoptimization_literals_.length(), TENURED);
  for (int i = 0; i < deoptimization_literals_.length(); i++) {
    literals->set(i, *deoptimization_literals_[i]);
  }
  data->SetLiteralArray(*literals);

  data->SetOsrAstId(Smi::FromInt(info_->osr_ast_id().ToInt()));
  data->SetOsrPcOffset(Smi::FromInt(osr_pc_offset_));

  // Populate the deoptimization entries.
  for (int i = 0; i < length; i++) {
    LEnvironment* env = deoptimizations_[i];
    data->SetAstId(i, env->ast_id());
    data->SetTranslationIndex(i, Smi::FromInt(env->translation_index()));
    data->SetArgumentsStackHeight(i,
                                  Smi::FromInt(env->arguments_stack_height()));
    data->SetPc(i, Smi::FromInt(env->pc_offset()));
  }
  code->set_deoptimization_data(*data);
}


int LCodeGen::DefineDeoptimizationLiteral(Handle<Object> literal) {
  int result = deoptimization_literals_.length();
  for (int i = 0; i < deoptimization_literals_.length(); ++i) {
    if (deoptimization_literals_[i].is_identical_to(literal)) return i;
  }
  deoptimization_literals_.Add(literal, zone());
  return result;
}


void LCodeGen::PopulateDeoptimizationLiteralsWithInlinedFunctions() {
  ASSERT(deoptimization_literals_.length() == 0);

  const ZoneList<Handle<JSFunction> >* inlined_closures =
      chunk()->inlined_closures();

  for (int i = 0, length = inlined_closures->length();
       i < length;
       i++) {
    DefineDeoptimizationLiteral(inlined_closures->at(i));
  }

  inlined_function_count_ = deoptimization_literals_.length();
}


void LCodeGen::RecordSafepointWithLazyDeopt(
    LInstruction* instr, SafepointMode safepoint_mode) {
  if (safepoint_mode == RECORD_SIMPLE_SAFEPOINT) {
    RecordSafepoint(instr->pointer_map(), Safepoint::kLazyDeopt);
  } else {
    ASSERT(safepoint_mode == RECORD_SAFEPOINT_WITH_REGISTERS_AND_NO_ARGUMENTS);
    RecordSafepointWithRegisters(
        instr->pointer_map(), 0, Safepoint::kLazyDeopt);
  }
}


void LCodeGen::RecordSafepoint(
    LPointerMap* pointers,
    Safepoint::Kind kind,
    int arguments,
    Safepoint::DeoptMode deopt_mode) {
  ASSERT(expected_safepoint_kind_ == kind);

  const ZoneList<LOperand*>* operands = pointers->GetNormalizedOperands();
  Safepoint safepoint = safepoints_.DefineSafepoint(masm(),
      kind, arguments, deopt_mode);
  for (int i = 0; i < operands->length(); i++) {
    LOperand* pointer = operands->at(i);
    if (pointer->IsStackSlot()) {
      safepoint.DefinePointerSlot(pointer->index(), zone());
    } else if (pointer->IsRegister() && (kind & Safepoint::kWithRegisters)) {
      safepoint.DefinePointerRegister(ToRegister(pointer), zone());
    }
  }
  if (kind & Safepoint::kWithRegisters) {
    // Register cp always contains a pointer to the context.
    safepoint.DefinePointerRegister(cp, zone());
  }
}


void LCodeGen::RecordSafepoint(LPointerMap* pointers,
                               Safepoint::DeoptMode deopt_mode) {
  RecordSafepoint(pointers, Safepoint::kSimple, 0, deopt_mode);
}


void LCodeGen::RecordSafepoint(Safepoint::DeoptMode deopt_mode) {
  LPointerMap empty_pointers(RelocInfo::kNoPosition, zone());
  RecordSafepoint(&empty_pointers, deopt_mode);
}


void LCodeGen::RecordSafepointWithRegisters(LPointerMap* pointers,
                                            int arguments,
                                            Safepoint::DeoptMode deopt_mode) {
  RecordSafepoint(
      pointers, Safepoint::kWithRegisters, arguments, deopt_mode);
}


void LCodeGen::RecordSafepointWithRegistersAndDoubles(
    LPointerMap* pointers,
    int arguments,
    Safepoint::DeoptMode deopt_mode) {
  RecordSafepoint(
      pointers, Safepoint::kWithRegistersAndDoubles, arguments, deopt_mode);
}


void LCodeGen::RecordPosition(int position) {
  if (position == RelocInfo::kNoPosition) return;
  masm()->positions_recorder()->RecordPosition(position);
}


void LCodeGen::DoLabel(LLabel* label) {
  if (label->is_loop_header()) {
    Comment(";;; B%d - LOOP entry", label->block_id());
  } else {
    Comment(";;; B%d", label->block_id());
  }
  __ bind(label->label());
  current_block_ = label->block_id();
  DoGap(label);
}


void LCodeGen::DoParallelMove(LParallelMove* move) {
  resolver_.Resolve(move);
}


void LCodeGen::DoGap(LGap* gap) {
  for (int i = LGap::FIRST_INNER_POSITION;
       i <= LGap::LAST_INNER_POSITION;
       i++) {
    LGap::InnerPosition inner_pos = static_cast<LGap::InnerPosition>(i);
    LParallelMove* move = gap->GetParallelMove(inner_pos);
    if (move != NULL) DoParallelMove(move);
  }
}


void LCodeGen::DoInstructionGap(LInstructionGap* instr) {
  DoGap(instr);
}


void LCodeGen::DoParameter(LParameter* instr) {
  // Nothing to do.
}


void LCodeGen::DoCallStub(LCallStub* instr) {
  ASSERT(ToRegister(instr->result()).is(r3));
  switch (instr->hydrogen()->major_key()) {
    case CodeStub::RegExpConstructResult: {
      RegExpConstructResultStub stub;
      CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
      break;
    }
    case CodeStub::RegExpExec: {
      RegExpExecStub stub;
      CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
      break;
    }
    case CodeStub::SubString: {
      SubStringStub stub;
      CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
      break;
    }
    case CodeStub::NumberToString: {
      NumberToStringStub stub;
      CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
      break;
    }
    case CodeStub::StringAdd: {
      StringAddStub stub(NO_STRING_ADD_FLAGS);
      CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
      break;
    }
    case CodeStub::StringCompare: {
      StringCompareStub stub;
      CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
      break;
    }
    case CodeStub::TranscendentalCache: {
      __ lwz(r3, MemOperand(sp, 0));
      TranscendentalCacheStub stub(instr->transcendental_type(),
                                   TranscendentalCacheStub::TAGGED);
      CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
      break;
    }
    default:
      UNREACHABLE();
  }
}


void LCodeGen::DoUnknownOSRValue(LUnknownOSRValue* instr) {
  // Nothing to do.
}


void LCodeGen::DoModI(LModI* instr) {
  Register dividend = ToRegister(instr->left());
  Register result = ToRegister(instr->result());
  Register scratch = scratch0();
  Label done;

  if (instr->hydrogen()->HasPowerOf2Divisor()) {
    int32_t divisor =
        HConstant::cast(instr->hydrogen()->right())->Integer32Value();

    if (divisor < 0) divisor = -divisor;

    Label positive_dividend;
    __ cmpi(dividend, Operand::Zero());
    __ bge(&positive_dividend);
    __ neg(result, dividend);
    __ mov(scratch, Operand(divisor - 1));
    __ and_(result, result, scratch, SetRC);
    if (instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero)) {
      DeoptimizeIf(eq, instr->environment(), cr0);
    }
    __ neg(result, result);
    __ b(&done);
    __ bind(&positive_dividend);
    __ mov(scratch, Operand(divisor - 1));
    __ and_(result, dividend, scratch);
  } else {
    // div runs in the background while we check for special cases.
    Register divisor = ToRegister(instr->right());
    __ divw(scratch, dividend, divisor);

    // Check for x % 0.
    if (instr->hydrogen()->CheckFlag(HValue::kCanBeDivByZero)) {
        __ cmpi(divisor, Operand::Zero());
        DeoptimizeIf(eq, instr->environment());
    }

    __ mullw(scratch, divisor, scratch);
    __ sub(result, dividend, scratch, LeaveOE, SetRC);

    if (instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero)) {
      __ cmpi(dividend, Operand::Zero());
      __ bge(&done);
      DeoptimizeIf(eq, instr->environment(), cr0);
    }
  }

  __ bind(&done);
}


void LCodeGen::EmitSignedIntegerDivisionByConstant(
    Register result,
    Register dividend,
    int32_t divisor,
    Register remainder,
    Register scratch,
    LEnvironment* environment) {
  ASSERT(!AreAliased(dividend, scratch, ip));
  ASSERT(LChunkBuilder::HasMagicNumberForDivisor(divisor));

  Label skip;
  uint32_t divisor_abs = abs(divisor);

  int32_t power_of_2_factor =
    CompilerIntrinsics::CountTrailingZeros(divisor_abs);

  switch (divisor_abs) {
    case 0:
      DeoptimizeIf(al, environment);
      return;

    case 1:
      if (divisor > 0) {
        __ Move(result, dividend);
      } else {
        __ li(r0, Operand::Zero());  // clear xer
        __ mtxer(r0);
        __ neg(result, dividend, SetOE, SetRC);
        __ bnotoverflow(&skip, cr0);
        DeoptimizeIf(al, environment);
        __ bind(&skip);
      }
      // Compute the remainder.
      __ li(remainder, Operand::Zero());
      return;

    default:
      if (IsPowerOf2(divisor_abs)) {
        // Branch and condition free code for integer division by a power
        // of two.
        int32_t power = WhichPowerOf2(divisor_abs);
        if (power > 1) {
          __ srawi(scratch, dividend, power - 1);
        }
        __ srwi(scratch, scratch, Operand(32 - power));
        __ add(scratch, dividend, scratch);
        __ srawi(result, scratch, power);
        // Negate if necessary.
        // We don't need to check for overflow because the case '-1' is
        // handled separately.
        if (divisor < 0) {
          ASSERT(divisor != -1);
          __ neg(result, result);
        }
        // Compute the remainder.
        __ slwi(scratch, result, Operand(power));
        if (divisor > 0) {
          __ sub(remainder, dividend, scratch);
        } else {
          __ add(remainder, dividend, scratch);
        }
        return;
      } else {
        // Use magic numbers for a few specific divisors.
        // Details and proofs can be found in:
        // - Hacker's Delight, Henry S. Warren, Jr.
        // - The PowerPC Compiler Writer’s Guide
        // and probably many others.
        //
        // We handle
        //   <divisor with magic numbers> * <power of 2>
        // but not
        //   <divisor with magic numbers> * <other divisor with magic numbers>
        DivMagicNumbers magic_numbers =
          DivMagicNumberFor(divisor_abs >> power_of_2_factor);
        // Branch and condition free code for integer division by a power
        // of two.
        const int32_t M = magic_numbers.M;
        const int32_t s = magic_numbers.s + power_of_2_factor;

        __ mov(ip, Operand(M));
        __ mullw(ip, dividend, ip);
        __ mulhw(scratch, dividend, ip);
        if (M < 0) {
          __ add(scratch, scratch, dividend);
        }
        if (s > 0) {
          __ srawi(scratch, scratch, s);
        }
        __ srwi(result, dividend, Operand(31));
        __ add(result, scratch, result);
        if (divisor < 0) __ neg(result, result);
        // Compute the remainder.
        __ mov(ip, Operand(divisor));
        // This sequence could be replaced with 'mls' when
        // it gets implemented.
        __ mul(scratch, result, ip);
        __ sub(remainder, dividend, scratch);
      }
  }
}


void LCodeGen::DoDivI(LDivI* instr) {
  class DeferredDivI: public LDeferredCode {
   public:
    DeferredDivI(LCodeGen* codegen, LDivI* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() {
      codegen()->DoDeferredBinaryOpStub(instr_->pointer_map(),
                                        instr_->left(),
                                        instr_->right(),
                                        Token::DIV);
    }
    virtual LInstruction* instr() { return instr_; }
   private:
    LDivI* instr_;
  };

  const Register left = ToRegister(instr->left());
  const Register right = ToRegister(instr->right());
  const Register scratch = scratch0();
  const Register result = ToRegister(instr->result());

  // Check for x / 0.
  if (instr->hydrogen()->CheckFlag(HValue::kCanBeDivByZero)) {
    __ cmpi(right, Operand::Zero());
    DeoptimizeIf(eq, instr->environment());
  }

  // Check for (0 / -x) that will produce negative zero.
  if (instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero)) {
    Label left_not_zero;
    __ cmpi(left, Operand::Zero());
    __ bne(&left_not_zero);
    __ cmpi(right, Operand::Zero());
    DeoptimizeIf(lt, instr->environment());
    __ bind(&left_not_zero);
  }

  // Check for (-kMinInt / -1).
  if (instr->hydrogen()->CheckFlag(HValue::kCanOverflow)) {
    Label left_not_min_int;
    __ Cmpi(left, Operand(kMinInt), r0);
    __ bne(&left_not_min_int);
    __ cmpi(right, Operand(-1));
    DeoptimizeIf(eq, instr->environment());
    __ bind(&left_not_min_int);
  }

  Label done, deoptimize;
  Label byTwo, byFour, general;
  // Test for a few common cases first.

  // divide by 1
  __ cmpi(right, Operand(1));
  __ bne(&byTwo);
  __ mr(result, left);
  __ b(&done);

  // divide by 2
  __ bind(&byTwo);
  __ cmpi(right, Operand(2));
  __ bne(&byFour);
  __ andi(r0, left, Operand(1));
  __ bne(&general, cr0);
  __ srawi(result, left, 1);
  __ b(&done);

  // divide by 4
  __ bind(&byFour);
  __ cmpi(right, Operand(4));
  __ bne(&general);
  __ andi(r0, left, Operand(3));
  __ bne(&general, cr0);
  __ srawi(result, left, 2);
  __ b(&done);

  __ bind(&general);

  // Call the stub. The numbers in r3 and r4 have
  // to be tagged to Smis. If that is not possible, deoptimize.
  DeferredDivI* deferred = new(zone()) DeferredDivI(this, instr);

  __ SmiTagCheckOverflow(ip, left, scratch);
  __ BranchOnOverflow(&deoptimize);
  __ mr(left, ip);

  __ SmiTagCheckOverflow(ip, right, scratch);
  __ BranchOnOverflow(&deoptimize);
  __ mr(right, ip);

  __ b(deferred->entry());
  __ bind(deferred->exit());

  // If the result in r3 is a Smi, untag it, else deoptimize.
  __ JumpIfNotSmi(result, &deoptimize);
  __ SmiUntag(result);
  __ b(&done);

  __ bind(&deoptimize);
  DeoptimizeIf(al, instr->environment());
  __ bind(&done);
}


void LCodeGen::DoMathFloorOfDiv(LMathFloorOfDiv* instr) {
  const Register result = ToRegister(instr->result());
  const Register left = ToRegister(instr->left());
  const Register remainder = ToRegister(instr->temp());
  const Register scratch = scratch0();

  // We only optimize this for division by constants, because the standard
  // integer division routine is usually slower than transitionning to VFP.
  // This could be optimized on processors with SDIV available.
  ASSERT(instr->right()->IsConstantOperand());
  int32_t divisor = ToInteger32(LConstantOperand::cast(instr->right()));
  if (divisor < 0) {
    __ cmpi(left, Operand::Zero());
    DeoptimizeIf(eq, instr->environment());
  }
  EmitSignedIntegerDivisionByConstant(result,
                                      left,
                                      divisor,
                                      remainder,
                                      scratch,
                                      instr->environment());
  // We operated a truncating division. Correct the result if necessary.
  __ cmpi(remainder, Operand::Zero());
#ifdef PENGUIN_CLEANUP
  __ teq(remainder, Operand(divisor), ne);
  __ sub(result, result, Operand(1), LeaveCC, mi);
#else
  PPCPORT_UNIMPLEMENTED();
  __ fake_asm(fLITHIUM111);
#endif
}


void LCodeGen::DoDeferredBinaryOpStub(LPointerMap* pointer_map,
                                      LOperand* left_argument,
                                      LOperand* right_argument,
                                      Token::Value op) {
  Register left = ToRegister(left_argument);
  Register right = ToRegister(right_argument);

  PushSafepointRegistersScope scope(this, Safepoint::kWithRegistersAndDoubles);
  // Move left to r4 and right to r3 for the stub call.
  if (left.is(r4)) {
    __ Move(r3, right);
  } else if (left.is(r3) && right.is(r4)) {
    __ Swap(r3, r4, r5);
  } else if (left.is(r3)) {
    ASSERT(!right.is(r4));
    __ mr(r4, r3);
    __ mr(r3, right);
  } else {
    ASSERT(!left.is(r3) && !right.is(r3));
    __ mr(r3, right);
    __ mr(r4, left);
  }
  BinaryOpStub stub(op, OVERWRITE_LEFT);
  __ CallStub(&stub);
  RecordSafepointWithRegistersAndDoubles(pointer_map,
                                         0,
                                         Safepoint::kNoLazyDeopt);
  // Overwrite the stored value of r3 with the result of the stub.
  __ StoreToSafepointRegistersAndDoublesSlot(r3, r3);
}


void LCodeGen::DoMulI(LMulI* instr) {
  Register scratch = scratch0();
  Register result = ToRegister(instr->result());
  // Note that result may alias left.
  Register left = ToRegister(instr->left());
  LOperand* right_op = instr->right();

  bool can_overflow = instr->hydrogen()->CheckFlag(HValue::kCanOverflow);
  bool bailout_on_minus_zero =
    instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero);

  if (right_op->IsConstantOperand() && !can_overflow) {
    // Use optimized code for specific constants.
    int32_t constant = ToInteger32(LConstantOperand::cast(right_op));

    if (bailout_on_minus_zero && (constant < 0)) {
      // The case of a null constant will be handled separately.
      // If constant is negative and left is null, the result should be -0.
      __ cmpi(left, Operand::Zero());
      DeoptimizeIf(eq, instr->environment());
    }

    switch (constant) {
      case -1:
        __ neg(result, left);
        break;
      case 0:
        if (bailout_on_minus_zero) {
          // If left is strictly negative and the constant is null, the
          // result is -0. Deoptimize if required, otherwise return 0.
          __ cmpi(left, Operand::Zero());
          DeoptimizeIf(lt, instr->environment());
        }
        __ li(result, Operand::Zero());
        break;
      case 1:
        __ Move(result, left);
        break;
      default:
        // Multiplying by powers of two and powers of two plus or minus
        // one can be done faster with shifted operands.
        // For other constants we emit standard code.
        int32_t mask = constant >> 31;
        uint32_t constant_abs = (constant + mask) ^ mask;

        if (IsPowerOf2(constant_abs) ||
            IsPowerOf2(constant_abs - 1) ||
            IsPowerOf2(constant_abs + 1)) {
          if (IsPowerOf2(constant_abs)) {
            int32_t shift = WhichPowerOf2(constant_abs);
            __ slwi(result, left, Operand(shift));
          } else if (IsPowerOf2(constant_abs - 1)) {
            int32_t shift = WhichPowerOf2(constant_abs - 1);
            __ slwi(result, left, Operand(shift));
            __ add(result, result, left);
          } else if (IsPowerOf2(constant_abs + 1)) {
            int32_t shift = WhichPowerOf2(constant_abs + 1);
            __ slwi(result, left, Operand(shift));
            __ sub(result, result, left);
          }

          // Correct the sign of the result is the constant is negative.
          if (constant < 0)  __ neg(result, result);

        } else {
          // Generate standard code.
          __ mov(ip, Operand(constant));
          __ mul(result, left, ip);
        }
    }

  } else {
    Register right = EmitLoadRegister(right_op, scratch);
    if (bailout_on_minus_zero) {
      __ orx(ToRegister(instr->temp()), left, right);
    }

    if (can_overflow) {
      // scratch:result = left * right.
      __ mullw(result, left, right);
      __ mulhw(scratch, left, right);
      __ srawi(r0, result, 31);
      __ cmp(scratch, r0);
      DeoptimizeIf(ne, instr->environment());
    } else {
      __ mul(result, left, right);
    }

    if (bailout_on_minus_zero) {
      // Bail out if the result is supposed to be negative zero.
      Label done;
      __ cmpi(result, Operand::Zero());
      __ bne(&done);
      __ cmpi(ToRegister(instr->temp()), Operand::Zero());
      DeoptimizeIf(lt, instr->environment());
      __ bind(&done);
    }
  }
}


void LCodeGen::DoBitI(LBitI* instr) {
  LOperand* left_op = instr->left();
  LOperand* right_op = instr->right();
  ASSERT(left_op->IsRegister());
  Register left = ToRegister(left_op);
  Register result = ToRegister(instr->result());
  Operand right(no_reg);

  if (right_op->IsStackSlot() || right_op->IsArgument()) {
    right = Operand(EmitLoadRegister(right_op, ip));
  } else {
    ASSERT(right_op->IsRegister() || right_op->IsConstantOperand());
    right = ToOperand(right_op);
  }

  switch (instr->op()) {
    case Token::BIT_AND:
      __ And(result, left, right);
      break;
    case Token::BIT_OR:
      __ Or(result, left, right);
      break;
    case Token::BIT_XOR:
      __ Xor(result, left, right);
      break;
    default:
      UNREACHABLE();
      break;
  }
}


void LCodeGen::DoShiftI(LShiftI* instr) {
  // Both 'left' and 'right' are "used at start" (see LCodeGen::DoShift), so
  // result may alias either of them.
  LOperand* right_op = instr->right();
  Register left = ToRegister(instr->left());
  Register result = ToRegister(instr->result());
  Register scratch = scratch0();
  if (right_op->IsRegister()) {
    // Mask the right_op operand.
    __ andi(scratch, ToRegister(right_op), Operand(0x1F));
    switch (instr->op()) {
      case Token::SAR:
        __ sraw(result, left, scratch);
        break;
      case Token::SHR:
        if (instr->can_deopt()) {
          __ srw(result, left, scratch, SetRC);
          DeoptimizeIf(lt, instr->environment(), cr0);
        } else {
          __ srw(result, left, scratch);
        }
        break;
      case Token::SHL:
        __ slw(result, left, scratch);
        break;
      default:
        UNREACHABLE();
        break;
    }
  } else {
    // Mask the right_op operand.
    int value = ToInteger32(LConstantOperand::cast(right_op));
    uint8_t shift_count = static_cast<uint8_t>(value & 0x1F);
    switch (instr->op()) {
      case Token::SAR:
        if (shift_count != 0) {
          __ srawi(result, left, shift_count);
        } else {
          __ Move(result, left);
        }
        break;
      case Token::SHR:
        if (shift_count != 0) {
          __ srwi(result, left, Operand(shift_count));
        } else {
          if (instr->can_deopt()) {
            __ TestBit(left, 0, r0);  // test sign bit
            DeoptimizeIf(ne, instr->environment(), cr0);
          }
          __ Move(result, left);
        }
        break;
      case Token::SHL:
        if (shift_count != 0) {
          __ slwi(result, left, Operand(shift_count));
        } else {
          __ Move(result, left);
        }
        break;
      default:
        UNREACHABLE();
        break;
    }
  }
}


void LCodeGen::DoSubI(LSubI* instr) {
  LOperand* left = instr->left();
  LOperand* right = instr->right();
  LOperand* result = instr->result();
  bool can_overflow = instr->hydrogen()->CheckFlag(HValue::kCanOverflow);
  Register right_reg = EmitLoadRegister(right, ip);

  if (!can_overflow) {
    __ sub(ToRegister(result), ToRegister(left), right_reg);
  } else {
    __ SubAndCheckForOverflow(ToRegister(result),
                              ToRegister(left),
                              right_reg,
                              scratch0(), r0);
    // Doptimize on overflow
    DeoptimizeIf(lt, instr->environment(), cr0);
  }
}


void LCodeGen::DoConstantI(LConstantI* instr) {
  ASSERT(instr->result()->IsRegister());
  __ mov(ToRegister(instr->result()), Operand(instr->value()));
}

// TODO(penguin): put const to constant pool instead
// of storing double to stack
void LCodeGen::DoConstantD(LConstantD* instr) {
  ASSERT(instr->result()->IsDoubleRegister());
  DwVfpRegister result = ToDoubleRegister(instr->result());
  double v = instr->value();
  __ LoadDoubleLiteral(result, v, scratch0());
}

void LCodeGen::DoConstantT(LConstantT* instr) {
  Handle<Object> value = instr->value();
  if (value->IsSmi()) {
    __ mov(ToRegister(instr->result()), Operand(value));
  } else {
    __ LoadHeapObject(ToRegister(instr->result()),
                      Handle<HeapObject>::cast(value));
  }
}


void LCodeGen::DoJSArrayLength(LJSArrayLength* instr) {
  Register result = ToRegister(instr->result());
  Register array = ToRegister(instr->value());
  __ lwz(result, FieldMemOperand(array, JSArray::kLengthOffset));
}


void LCodeGen::DoFixedArrayBaseLength(LFixedArrayBaseLength* instr) {
  Register result = ToRegister(instr->result());
  Register array = ToRegister(instr->value());
  __ lwz(result, FieldMemOperand(array, FixedArrayBase::kLengthOffset));
}


void LCodeGen::DoMapEnumLength(LMapEnumLength* instr) {
  Register result = ToRegister(instr->result());
  Register map = ToRegister(instr->value());
  __ EnumLength(result, map);
}


void LCodeGen::DoElementsKind(LElementsKind* instr) {
  Register result = ToRegister(instr->result());
  Register input = ToRegister(instr->value());

  // Load map into |result|.
  __ lwz(result, FieldMemOperand(input, HeapObject::kMapOffset));
  // Load the map's "bit field 2" into |result|
  __ lbz(result, FieldMemOperand(result, Map::kBitField2Offset));
  // Retrieve elements_kind from bit field 2.
  __ ExtractBitMask(result, result, Map::kElementsKindMask);
}


void LCodeGen::DoValueOf(LValueOf* instr) {
  Register input = ToRegister(instr->value());
  Register result = ToRegister(instr->result());
  Register map = ToRegister(instr->temp());
  Label done, is_smi_or_object;

  // If the object is a smi return the object.
  __ JumpIfSmi(input, &is_smi_or_object);

  // If the object is not a value type, return the object.
  __ CompareObjectType(input, map, map, JS_VALUE_TYPE);
  __ bne(&is_smi_or_object);

  __ lwz(result, FieldMemOperand(input, JSValue::kValueOffset));
  __ b(&done);

  __ bind(&is_smi_or_object);
  __ Move(result, input);

  __ bind(&done);
}


void LCodeGen::DoDateField(LDateField* instr) {
  Register object = ToRegister(instr->date());
  Register result = ToRegister(instr->result());
  Register scratch = ToRegister(instr->temp());
  Smi* index = instr->index();
  Label runtime, done;
  ASSERT(object.is(result));
  ASSERT(object.is(r3));
  ASSERT(!scratch.is(scratch0()));
  ASSERT(!scratch.is(object));

  __ TestIfSmi(object, r0);
  DeoptimizeIf(eq, instr->environment(), cr0);
  __ CompareObjectType(object, scratch, scratch, JS_DATE_TYPE);
  DeoptimizeIf(ne, instr->environment());

  if (index->value() == 0) {
    __ lwz(result, FieldMemOperand(object, JSDate::kValueOffset));
  } else {
    if (index->value() < JSDate::kFirstUncachedField) {
      ExternalReference stamp = ExternalReference::date_cache_stamp(isolate());
      __ mov(scratch, Operand(stamp));
      __ lwz(scratch, MemOperand(scratch));
      __ lwz(scratch0(), FieldMemOperand(object, JSDate::kCacheStampOffset));
      __ cmp(scratch, scratch0());
      __ bne(&runtime);
      __ lwz(result, FieldMemOperand(object, JSDate::kValueOffset +
                                             kPointerSize * index->value()));
      __ b(&done);
    }
    __ bind(&runtime);
    __ PrepareCallCFunction(2, scratch);
    __ mov(r4, Operand(index));
    __ CallCFunction(ExternalReference::get_date_field_function(isolate()), 2);
    __ bind(&done);
  }
}


void LCodeGen::DoBitNotI(LBitNotI* instr) {
  Register input = ToRegister(instr->value());
  Register result = ToRegister(instr->result());
  __ notx(result, input);
}


void LCodeGen::DoThrow(LThrow* instr) {
  Register input_reg = EmitLoadRegister(instr->value(), ip);
  __ push(input_reg);
  CallRuntime(Runtime::kThrow, 1, instr);

  if (FLAG_debug_code) {
    __ stop("Unreachable code.");
  }
}


void LCodeGen::DoAddI(LAddI* instr) {
  LOperand* left = instr->left();
  LOperand* right = instr->right();
  LOperand* result = instr->result();
  bool can_overflow = instr->hydrogen()->CheckFlag(HValue::kCanOverflow);
  Register right_reg = EmitLoadRegister(right, ip);

  if (!can_overflow) {
    __ add(ToRegister(result), ToRegister(left), right_reg);
  } else {  // can_overflow.
    __ AddAndCheckForOverflow(ToRegister(result),
                              ToRegister(left),
                              right_reg,
                              scratch0(), r0);
    // Doptimize on overflow
    DeoptimizeIf(lt, instr->environment(), cr0);
  }
}


void LCodeGen::DoMathMinMax(LMathMinMax* instr) {
  LOperand* left = instr->left();
  LOperand* right = instr->right();
  HMathMinMax::Operation operation = instr->hydrogen()->operation();
  Condition cond = (operation == HMathMinMax::kMathMin) ? le : ge;
  if (instr->hydrogen()->representation().IsInteger32()) {
    Register left_reg = ToRegister(left);
    Register right_reg = EmitLoadRegister(right, ip);
    Register result_reg = ToRegister(instr->result());
    Label return_left, done;
    __ cmp(left_reg, right_reg);
    __ b(cond, &return_left);
    __ Move(result_reg, right_reg);
    __ b(&done);
    __ bind(&return_left);
    __ Move(result_reg, left_reg);
    __ bind(&done);
  } else {
    ASSERT(instr->hydrogen()->representation().IsDouble());
    DoubleRegister left_reg = ToDoubleRegister(left);
    DoubleRegister right_reg = ToDoubleRegister(right);
    DoubleRegister result_reg = ToDoubleRegister(instr->result());
    Label check_nan_left, check_zero, return_left, return_right, done;
    __ fcmpu(left_reg, right_reg);
    __ bunordered(&check_nan_left);
    __ beq(&check_zero);
    __ b(cond, &return_left);
    __ b(&return_right);

    __ bind(&check_zero);
    __ fcmpu(left_reg, kDoubleRegZero);
    __ bne(&return_left);  // left == right != 0.

    // At this point, both left and right are either 0 or -0.
    // N.B. The following works because +0 + -0 == +0
    if (operation == HMathMinMax::kMathMin) {
      // For min we want logical-or of sign bit: -(-L + -R)
      __ fneg(left_reg, left_reg);
      __ fsub(result_reg, left_reg, right_reg);
      __ fneg(result_reg, result_reg);
    } else {
      // For max we want logical-and of sign bit: (L + R)
      __ fadd(result_reg, left_reg, right_reg);
    }
    __ b(&done);

    __ bind(&check_nan_left);
    __ fcmpu(left_reg, left_reg);
    __ bunordered(&return_left);  // left == NaN.

    __ bind(&return_right);
    if (!right_reg.is(result_reg)) {
      __ fmr(result_reg, right_reg);
    }
    __ b(&done);

    __ bind(&return_left);
    if (!left_reg.is(result_reg)) {
      __ fmr(result_reg, left_reg);
    }
    __ bind(&done);
  }
}


void LCodeGen::DoArithmeticD(LArithmeticD* instr) {
  DoubleRegister left = ToDoubleRegister(instr->left());
  DoubleRegister right = ToDoubleRegister(instr->right());
  DoubleRegister result = ToDoubleRegister(instr->result());
  switch (instr->op()) {
    case Token::ADD:
      __ fadd(result, left, right);
      break;
    case Token::SUB:
      __ fsub(result, left, right);
      break;
    case Token::MUL:
      __ fmul(result, left, right);
      break;
    case Token::DIV:
      __ fdiv(result, left, right);
      break;
    case Token::MOD: {
      // Save r3-r6 on the stack.
      __ MultiPush(r3.bit() | r4.bit() | r5.bit() | r6.bit());

      __ PrepareCallCFunction(0, 2, scratch0());
      __ SetCallCDoubleArguments(left, right);
      __ CallCFunction(
          ExternalReference::double_fp_operation(Token::MOD, isolate()),
          0, 2);
      // Move the result in the double result register.
      __ GetCFunctionDoubleResult(result);

      // Restore r3-r6.
      __ MultiPop(r3.bit() | r4.bit() | r5.bit() | r6.bit());
      break;
    }
    default:
      UNREACHABLE();
      break;
  }
}


void LCodeGen::DoArithmeticT(LArithmeticT* instr) {
  ASSERT(ToRegister(instr->left()).is(r4));
  ASSERT(ToRegister(instr->right()).is(r3));
  ASSERT(ToRegister(instr->result()).is(r3));

  BinaryOpStub stub(instr->op(), NO_OVERWRITE);
  CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
  __ nop();  // Signals no inlined code.
}


int LCodeGen::GetNextEmittedBlock(int block) {
  for (int i = block + 1; i < graph()->blocks()->length(); ++i) {
    LLabel* label = chunk_->GetLabel(i);
    if (!label->HasReplacement()) return i;
  }
  return -1;
}


void LCodeGen::EmitBranch(int left_block, int right_block, Condition cond,
                          CRegister cr) {
  int next_block = GetNextEmittedBlock(current_block_);
  right_block = chunk_->LookupDestination(right_block);
  left_block = chunk_->LookupDestination(left_block);

  if (right_block == left_block) {
    EmitGoto(left_block);
  } else if (left_block == next_block) {
    __ b(NegateCondition(cond), chunk_->GetAssemblyLabel(right_block), cr);
  } else if (right_block == next_block) {
    __ b(cond, chunk_->GetAssemblyLabel(left_block), cr);
  } else {
    __ b(cond, chunk_->GetAssemblyLabel(left_block), cr);
    __ b(chunk_->GetAssemblyLabel(right_block));
  }
}


void LCodeGen::DoBranch(LBranch* instr) {
  int true_block = chunk_->LookupDestination(instr->true_block_id());
  int false_block = chunk_->LookupDestination(instr->false_block_id());

  Representation r = instr->hydrogen()->value()->representation();
  if (r.IsInteger32()) {
    Register reg = ToRegister(instr->value());
    __ cmpi(reg, Operand::Zero());
    EmitBranch(true_block, false_block, ne);
  } else if (r.IsDouble()) {
    DoubleRegister reg = ToDoubleRegister(instr->value());
    Register scratch = scratch0();

    // Test the double value. Zero and NaN are false.
    uint crBits = (1 << (31 - Assembler::encode_crbit(cr7, CR_EQ)) |
                   1 << (31 - Assembler::encode_crbit(cr7, CR_FU)));
    __ fcmpu(reg, kDoubleRegZero, cr7);
    __ mfcr(scratch);
    __ andi(scratch, scratch, Operand(crBits));
    EmitBranch(true_block, false_block, eq, cr0);
  } else {
    ASSERT(r.IsTagged());
    Register reg = ToRegister(instr->value());
    HType type = instr->hydrogen()->value()->type();
    if (type.IsBoolean()) {
      __ CompareRoot(reg, Heap::kTrueValueRootIndex);
      EmitBranch(true_block, false_block, eq);
    } else if (type.IsSmi()) {
      __ cmpi(reg, Operand::Zero());
      EmitBranch(true_block, false_block, ne);
    } else {
      Label* true_label = chunk_->GetAssemblyLabel(true_block);
      Label* false_label = chunk_->GetAssemblyLabel(false_block);

      ToBooleanStub::Types expected = instr->hydrogen()->expected_input_types();
      // Avoid deopts in the case where we've never executed this path before.
      if (expected.IsEmpty()) expected = ToBooleanStub::all_types();

      if (expected.Contains(ToBooleanStub::UNDEFINED)) {
        // undefined -> false.
        __ CompareRoot(reg, Heap::kUndefinedValueRootIndex);
        __ beq(false_label);
      }
      if (expected.Contains(ToBooleanStub::BOOLEAN)) {
        // Boolean -> its value.
        __ CompareRoot(reg, Heap::kTrueValueRootIndex);
        __ beq(true_label);
        __ CompareRoot(reg, Heap::kFalseValueRootIndex);
        __ beq(false_label);
      }
      if (expected.Contains(ToBooleanStub::NULL_TYPE)) {
        // 'null' -> false.
        __ CompareRoot(reg, Heap::kNullValueRootIndex);
        __ beq(false_label);
      }

      if (expected.Contains(ToBooleanStub::SMI)) {
        // Smis: 0 -> false, all other -> true.
        __ cmpi(reg, Operand::Zero());
        __ beq(false_label);
        __ JumpIfSmi(reg, true_label);
      } else if (expected.NeedsMap()) {
        // If we need a map later and have a Smi -> deopt.
        __ TestIfSmi(reg, r0);
        DeoptimizeIf(eq, instr->environment(), cr0);
      }

      const Register map = scratch0();
      if (expected.NeedsMap()) {
        __ lwz(map, FieldMemOperand(reg, HeapObject::kMapOffset));

        if (expected.CanBeUndetectable()) {
          // Undetectable -> false.
          __ lbz(ip, FieldMemOperand(map, Map::kBitFieldOffset));
          __ TestBit(ip, 31 - Map::kIsUndetectable, r0);
          __ bne(false_label, cr0);
        }
      }

      if (expected.Contains(ToBooleanStub::SPEC_OBJECT)) {
        // spec object -> true.
        __ CompareInstanceType(map, ip, FIRST_SPEC_OBJECT_TYPE);
        __ bge(true_label);
      }

      if (expected.Contains(ToBooleanStub::STRING)) {
        // String value -> false iff empty.
        Label not_string;
        __ CompareInstanceType(map, ip, FIRST_NONSTRING_TYPE);
        __ bge(&not_string);
        __ lwz(ip, FieldMemOperand(reg, String::kLengthOffset));
        __ cmpi(ip, Operand::Zero());
        __ bne(true_label);
        __ b(false_label);
        __ bind(&not_string);
      }

      if (expected.Contains(ToBooleanStub::HEAP_NUMBER)) {
        // heap number -> false iff +0, -0, or NaN.
        DoubleRegister dbl_scratch = double_scratch0();
        Label not_heap_number;
        __ CompareRoot(map, Heap::kHeapNumberMapRootIndex);
        __ bne(&not_heap_number);
        __ lfd(dbl_scratch, FieldMemOperand(reg, HeapNumber::kValueOffset));
        __ fcmpu(dbl_scratch, kDoubleRegZero);
        __ bunordered(false_label);  // NaN -> false.
        __ beq(false_label);  // +0, -0 -> false.
        __ b(true_label);
        __ bind(&not_heap_number);
      }

      // We've seen something for the first time -> deopt.
      DeoptimizeIf(al, instr->environment());
    }
  }
}


void LCodeGen::EmitGoto(int block) {
  block = chunk_->LookupDestination(block);
  int next_block = GetNextEmittedBlock(current_block_);
  if (block != next_block) {
    __ b(chunk_->GetAssemblyLabel(block));
  }
}


void LCodeGen::DoGoto(LGoto* instr) {
  EmitGoto(instr->block_id());
}


Condition LCodeGen::TokenToCondition(Token::Value op) {
  Condition cond = kNoCondition;
  switch (op) {
    case Token::EQ:
    case Token::EQ_STRICT:
      cond = eq;
      break;
    case Token::LT:
      cond =  lt;
      break;
    case Token::GT:
      cond = gt;
      break;
    case Token::LTE:
      cond = le;
      break;
    case Token::GTE:
      cond = ge;
      break;
    case Token::IN:
    case Token::INSTANCEOF:
    default:
      UNREACHABLE();
  }
  return cond;
}


void LCodeGen::DoCmpIDAndBranch(LCmpIDAndBranch* instr) {
  LOperand* left = instr->left();
  LOperand* right = instr->right();
  int false_block = chunk_->LookupDestination(instr->false_block_id());
  int true_block = chunk_->LookupDestination(instr->true_block_id());
  Condition cond = TokenToCondition(instr->op());

  if (left->IsConstantOperand() && right->IsConstantOperand()) {
    // We can statically evaluate the comparison.
    double left_val = ToDouble(LConstantOperand::cast(left));
    double right_val = ToDouble(LConstantOperand::cast(right));
    int next_block =
      EvalComparison(instr->op(), left_val, right_val) ? true_block
                                                       : false_block;
    EmitGoto(next_block);
  } else {
    if (instr->is_double()) {
      // Compare left and right operands as doubles and load the
      // resulting flags into the normal status register.
      __ fcmpu(ToDoubleRegister(left), ToDoubleRegister(right));
      // If a NaN is involved, i.e. the result is unordered,
      // jump to false block label.
      __ bunordered(chunk_->GetAssemblyLabel(false_block));
    } else {
      if (right->IsConstantOperand()) {
        __ Cmpi(ToRegister(left),
                Operand(ToInteger32(LConstantOperand::cast(right))), r0);
      } else if (left->IsConstantOperand()) {
        __ Cmpi(ToRegister(right),
                Operand(ToInteger32(LConstantOperand::cast(left))), r0);
        // We transposed the operands. Reverse the condition.
        cond = ReverseCondition(cond);
      } else {
        __ cmp(ToRegister(left), ToRegister(right));
      }
    }
    EmitBranch(true_block, false_block, cond);
  }
}


void LCodeGen::DoCmpObjectEqAndBranch(LCmpObjectEqAndBranch* instr) {
  Register left = ToRegister(instr->left());
  Register right = ToRegister(instr->right());
  int false_block = chunk_->LookupDestination(instr->false_block_id());
  int true_block = chunk_->LookupDestination(instr->true_block_id());

  __ cmp(left, right);
  EmitBranch(true_block, false_block, eq);
}


void LCodeGen::DoCmpConstantEqAndBranch(LCmpConstantEqAndBranch* instr) {
  Register left = ToRegister(instr->left());
  int true_block = chunk_->LookupDestination(instr->true_block_id());
  int false_block = chunk_->LookupDestination(instr->false_block_id());

  __ Cmpi(left, Operand(instr->hydrogen()->right()), r0);
  EmitBranch(true_block, false_block, eq);
}


void LCodeGen::DoIsNilAndBranch(LIsNilAndBranch* instr) {
  Register scratch = scratch0();
  Register reg = ToRegister(instr->value());
  int false_block = chunk_->LookupDestination(instr->false_block_id());

  // If the expression is known to be untagged or a smi, then it's definitely
  // not null, and it can't be a an undetectable object.
  if (instr->hydrogen()->representation().IsSpecialization() ||
      instr->hydrogen()->type().IsSmi()) {
    EmitGoto(false_block);
    return;
  }

  int true_block = chunk_->LookupDestination(instr->true_block_id());
  Heap::RootListIndex nil_value = instr->nil() == kNullValue ?
      Heap::kNullValueRootIndex :
      Heap::kUndefinedValueRootIndex;
  __ LoadRoot(ip, nil_value);
  __ cmp(reg, ip);
  if (instr->kind() == kStrictEquality) {
    EmitBranch(true_block, false_block, eq);
  } else {
    Heap::RootListIndex other_nil_value = instr->nil() == kNullValue ?
        Heap::kUndefinedValueRootIndex :
        Heap::kNullValueRootIndex;
    Label* true_label = chunk_->GetAssemblyLabel(true_block);
    Label* false_label = chunk_->GetAssemblyLabel(false_block);
    __ beq(true_label);
    __ LoadRoot(ip, other_nil_value);
    __ cmp(reg, ip);
    __ beq(true_label);
    __ JumpIfSmi(reg, false_label);
    // Check for undetectable objects by looking in the bit field in
    // the map. The object has already been smi checked.
    __ lwz(scratch, FieldMemOperand(reg, HeapObject::kMapOffset));
    __ lbz(scratch, FieldMemOperand(scratch, Map::kBitFieldOffset));
    __ TestBit(scratch, 31 - Map::kIsUndetectable, r0);
    EmitBranch(true_block, false_block, ne, cr0);
  }
}


Condition LCodeGen::EmitIsObject(Register input,
                                 Register temp1,
                                 Label* is_not_object,
                                 Label* is_object) {
  Register temp2 = scratch0();
  __ JumpIfSmi(input, is_not_object);

  __ LoadRoot(temp2, Heap::kNullValueRootIndex);
  __ cmp(input, temp2);
  __ beq(is_object);

  // Load map.
  __ lwz(temp1, FieldMemOperand(input, HeapObject::kMapOffset));
  // Undetectable objects behave like undefined.
  __ lbz(temp2, FieldMemOperand(temp1, Map::kBitFieldOffset));
  __ TestBit(temp2, 31 - Map::kIsUndetectable, r0);
  __ bne(is_not_object, cr0);

  // Load instance type and check that it is in object type range.
  __ lbz(temp2, FieldMemOperand(temp1, Map::kInstanceTypeOffset));
  __ cmpi(temp2, Operand(FIRST_NONCALLABLE_SPEC_OBJECT_TYPE));
  __ blt(is_not_object);
  __ cmpi(temp2, Operand(LAST_NONCALLABLE_SPEC_OBJECT_TYPE));
  return le;
}


void LCodeGen::DoIsObjectAndBranch(LIsObjectAndBranch* instr) {
  Register reg = ToRegister(instr->value());
  Register temp1 = ToRegister(instr->temp());

  int true_block = chunk_->LookupDestination(instr->true_block_id());
  int false_block = chunk_->LookupDestination(instr->false_block_id());
  Label* true_label = chunk_->GetAssemblyLabel(true_block);
  Label* false_label = chunk_->GetAssemblyLabel(false_block);

  Condition true_cond =
      EmitIsObject(reg, temp1, false_label, true_label);

  EmitBranch(true_block, false_block, true_cond);
}


Condition LCodeGen::EmitIsString(Register input,
                                 Register temp1,
                                 Label* is_not_string) {
  __ JumpIfSmi(input, is_not_string);
  __ CompareObjectType(input, temp1, temp1, FIRST_NONSTRING_TYPE);

  return lt;
}


void LCodeGen::DoIsStringAndBranch(LIsStringAndBranch* instr) {
  Register reg = ToRegister(instr->value());
  Register temp1 = ToRegister(instr->temp());

  int true_block = chunk_->LookupDestination(instr->true_block_id());
  int false_block = chunk_->LookupDestination(instr->false_block_id());
  Label* false_label = chunk_->GetAssemblyLabel(false_block);

  Condition true_cond =
      EmitIsString(reg, temp1, false_label);

  EmitBranch(true_block, false_block, true_cond);
}


void LCodeGen::DoIsSmiAndBranch(LIsSmiAndBranch* instr) {
  int true_block = chunk_->LookupDestination(instr->true_block_id());
  int false_block = chunk_->LookupDestination(instr->false_block_id());

  Register input_reg = EmitLoadRegister(instr->value(), ip);
  __ TestIfSmi(input_reg, r0);
  EmitBranch(true_block, false_block, eq, cr0);
}


void LCodeGen::DoIsUndetectableAndBranch(LIsUndetectableAndBranch* instr) {
  Register input = ToRegister(instr->value());
  Register temp = ToRegister(instr->temp());

  int true_block = chunk_->LookupDestination(instr->true_block_id());
  int false_block = chunk_->LookupDestination(instr->false_block_id());

  __ JumpIfSmi(input, chunk_->GetAssemblyLabel(false_block));
  __ lwz(temp, FieldMemOperand(input, HeapObject::kMapOffset));
  __ lbz(temp, FieldMemOperand(temp, Map::kBitFieldOffset));
  __ TestBit(temp, 31 - Map::kIsUndetectable, r0);
  EmitBranch(true_block, false_block, ne, cr0);
}


static Condition ComputeCompareCondition(Token::Value op) {
  switch (op) {
    case Token::EQ_STRICT:
    case Token::EQ:
      return eq;
    case Token::LT:
      return lt;
    case Token::GT:
      return gt;
    case Token::LTE:
      return le;
    case Token::GTE:
      return ge;
    default:
      UNREACHABLE();
      return kNoCondition;
  }
}


void LCodeGen::DoStringCompareAndBranch(LStringCompareAndBranch* instr) {
  Token::Value op = instr->op();
  int true_block = chunk_->LookupDestination(instr->true_block_id());
  int false_block = chunk_->LookupDestination(instr->false_block_id());

  Handle<Code> ic = CompareIC::GetUninitialized(op);
  CallCode(ic, RelocInfo::CODE_TARGET, instr);
  // This instruction also signals no smi code inlined
  __ cmpi(r3, Operand::Zero());

  Condition condition = ComputeCompareCondition(op);

  EmitBranch(true_block, false_block, condition);
}


static InstanceType TestType(HHasInstanceTypeAndBranch* instr) {
  InstanceType from = instr->from();
  InstanceType to = instr->to();
  if (from == FIRST_TYPE) return to;
  ASSERT(from == to || to == LAST_TYPE);
  return from;
}


static Condition BranchCondition(HHasInstanceTypeAndBranch* instr) {
  InstanceType from = instr->from();
  InstanceType to = instr->to();
  if (from == to) return eq;
  if (to == LAST_TYPE) return ge;
  if (from == FIRST_TYPE) return le;
  UNREACHABLE();
  return eq;
}


void LCodeGen::DoHasInstanceTypeAndBranch(LHasInstanceTypeAndBranch* instr) {
  Register scratch = scratch0();
  Register input = ToRegister(instr->value());

  int true_block = chunk_->LookupDestination(instr->true_block_id());
  int false_block = chunk_->LookupDestination(instr->false_block_id());

  Label* false_label = chunk_->GetAssemblyLabel(false_block);

  __ JumpIfSmi(input, false_label);

  __ CompareObjectType(input, scratch, scratch, TestType(instr->hydrogen()));
  EmitBranch(true_block, false_block, BranchCondition(instr->hydrogen()));
}


void LCodeGen::DoGetCachedArrayIndex(LGetCachedArrayIndex* instr) {
  Register input = ToRegister(instr->value());
  Register result = ToRegister(instr->result());

  __ AssertString(input);

  __ lwz(result, FieldMemOperand(input, String::kHashFieldOffset));
  __ IndexFromHash(result, result);
}


void LCodeGen::DoHasCachedArrayIndexAndBranch(
    LHasCachedArrayIndexAndBranch* instr) {
  Register input = ToRegister(instr->value());
  Register scratch = scratch0();

  int true_block = chunk_->LookupDestination(instr->true_block_id());
  int false_block = chunk_->LookupDestination(instr->false_block_id());

  __ lwz(scratch,
         FieldMemOperand(input, String::kHashFieldOffset));
  __ mov(r0, Operand(String::kContainsCachedArrayIndexMask));
  __ and_(r0, scratch, r0, SetRC);
  EmitBranch(true_block, false_block, eq, cr0);
}


// Branches to a label or falls through with the answer in flags.  Trashes
// the temp registers, but not the input.
void LCodeGen::EmitClassOfTest(Label* is_true,
                               Label* is_false,
                               Handle<String>class_name,
                               Register input,
                               Register temp,
                               Register temp2) {
  ASSERT(!input.is(temp));
  ASSERT(!input.is(temp2));
  ASSERT(!temp.is(temp2));

  __ JumpIfSmi(input, is_false);

  if (class_name->IsEqualTo(CStrVector("Function"))) {
    // Assuming the following assertions, we can use the same compares to test
    // for both being a function type and being in the object type range.
    STATIC_ASSERT(NUM_OF_CALLABLE_SPEC_OBJECT_TYPES == 2);
    STATIC_ASSERT(FIRST_NONCALLABLE_SPEC_OBJECT_TYPE ==
                  FIRST_SPEC_OBJECT_TYPE + 1);
    STATIC_ASSERT(LAST_NONCALLABLE_SPEC_OBJECT_TYPE ==
                  LAST_SPEC_OBJECT_TYPE - 1);
    STATIC_ASSERT(LAST_SPEC_OBJECT_TYPE == LAST_TYPE);
    __ CompareObjectType(input, temp, temp2, FIRST_SPEC_OBJECT_TYPE);
    __ blt(is_false);
    __ beq(is_true);
    __ cmpi(temp2, Operand(LAST_SPEC_OBJECT_TYPE));
    __ beq(is_true);
  } else {
    // Faster code path to avoid two compares: subtract lower bound from the
    // actual type and do a signed compare with the width of the type range.
    __ lwz(temp, FieldMemOperand(input, HeapObject::kMapOffset));
    __ lbz(temp2, FieldMemOperand(temp, Map::kInstanceTypeOffset));
    __ sub(temp2, temp2, Operand(FIRST_NONCALLABLE_SPEC_OBJECT_TYPE));
    __ cmpi(temp2, Operand(LAST_NONCALLABLE_SPEC_OBJECT_TYPE -
                          FIRST_NONCALLABLE_SPEC_OBJECT_TYPE));
    __ bgt(is_false);
  }

  // Now we are in the FIRST-LAST_NONCALLABLE_SPEC_OBJECT_TYPE range.
  // Check if the constructor in the map is a function.
  __ lwz(temp, FieldMemOperand(temp, Map::kConstructorOffset));

  // Objects with a non-function constructor have class 'Object'.
  __ CompareObjectType(temp, temp2, temp2, JS_FUNCTION_TYPE);
  if (class_name->IsEqualTo(CStrVector("Object"))) {
    __ bne(is_true);
  } else {
    __ bne(is_false);
  }

  // temp now contains the constructor function. Grab the
  // instance class name from there.
  __ lwz(temp, FieldMemOperand(temp, JSFunction::kSharedFunctionInfoOffset));
  __ lwz(temp, FieldMemOperand(temp,
                               SharedFunctionInfo::kInstanceClassNameOffset));
  // The class name we are testing against is a symbol because it's a literal.
  // The name in the constructor is a symbol because of the way the context is
  // booted.  This routine isn't expected to work for random API-created
  // classes and it doesn't have to because you can't access it with natives
  // syntax.  Since both sides are symbols it is sufficient to use an identity
  // comparison.
  __ Cmpi(temp, Operand(class_name), r0);
  // End with the answer in flags.
}


void LCodeGen::DoClassOfTestAndBranch(LClassOfTestAndBranch* instr) {
  Register input = ToRegister(instr->value());
  Register temp = scratch0();
  Register temp2 = ToRegister(instr->temp());
  Handle<String> class_name = instr->hydrogen()->class_name();

  int true_block = chunk_->LookupDestination(instr->true_block_id());
  int false_block = chunk_->LookupDestination(instr->false_block_id());

  Label* true_label = chunk_->GetAssemblyLabel(true_block);
  Label* false_label = chunk_->GetAssemblyLabel(false_block);

  EmitClassOfTest(true_label, false_label, class_name, input, temp, temp2);

  EmitBranch(true_block, false_block, eq);
}


void LCodeGen::DoCmpMapAndBranch(LCmpMapAndBranch* instr) {
  Register reg = ToRegister(instr->value());
  Register temp = ToRegister(instr->temp());
  int true_block = instr->true_block_id();
  int false_block = instr->false_block_id();

  __ lwz(temp, FieldMemOperand(reg, HeapObject::kMapOffset));
  __ Cmpi(temp, Operand(instr->map()), r0);
  EmitBranch(true_block, false_block, eq);
}


void LCodeGen::DoInstanceOf(LInstanceOf* instr) {
  ASSERT(ToRegister(instr->left()).is(r3));  // Object is in r3.
  ASSERT(ToRegister(instr->right()).is(r4));  // Function is in r4.

  InstanceofStub stub(InstanceofStub::kArgsInRegisters);
  Label equal, done;
  CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);

  __ cmpi(r3, Operand::Zero());
  __ beq(&equal);
  __ mov(r3, Operand(factory()->false_value()));
  __ b(&done);

  __ bind(&equal);
  __ mov(r3, Operand(factory()->true_value()));
  __ bind(&done);
}


void LCodeGen::DoInstanceOfKnownGlobal(LInstanceOfKnownGlobal* instr) {
  class DeferredInstanceOfKnownGlobal: public LDeferredCode {
   public:
    DeferredInstanceOfKnownGlobal(LCodeGen* codegen,
                                  LInstanceOfKnownGlobal* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() {
      codegen()->DoDeferredInstanceOfKnownGlobal(instr_, &map_check_);
    }
    virtual LInstruction* instr() { return instr_; }
    Label* map_check() { return &map_check_; }
   private:
    LInstanceOfKnownGlobal* instr_;
    Label map_check_;
  };

  DeferredInstanceOfKnownGlobal* deferred;
  deferred = new(zone()) DeferredInstanceOfKnownGlobal(this, instr);

  Label done, false_result;
  Register object = ToRegister(instr->value());
  Register temp = ToRegister(instr->temp());
  Register result = ToRegister(instr->result());

  ASSERT(object.is(r3));
  ASSERT(result.is(r3));

  // A Smi is not instance of anything.
  __ JumpIfSmi(object, &false_result);

  // This is the inlined call site instanceof cache. The two occurences of the
  // hole value will be patched to the last map/result pair generated by the
  // instanceof stub.
  Label cache_miss;
  Register map = temp;
  __ lwz(map, FieldMemOperand(object, HeapObject::kMapOffset));
  {
    __ bind(deferred->map_check());  // Label for calculating code patching.
    // We use Factory::the_hole_value() on purpose instead of loading from the
    // root array to force relocation to be able to later patch with
    // the cached map.
    Handle<JSGlobalPropertyCell> cell =
        factory()->NewJSGlobalPropertyCell(factory()->the_hole_value());
    __ mov(ip, Operand(Handle<Object>(cell)));
    __ lwz(ip, FieldMemOperand(ip, JSGlobalPropertyCell::kValueOffset));
    __ cmp(map, ip);
    __ bne(&cache_miss);
    // We use Factory::the_hole_value() on purpose instead of loading from the
    // root array to force relocation to be able to later patch
    // with true or false.
    __ mov(result, Operand(factory()->the_hole_value()));
  }
  __ b(&done);

  // The inlined call site cache did not match. Check null and string before
  // calling the deferred code.
  __ bind(&cache_miss);
  // Null is not instance of anything.
  __ LoadRoot(ip, Heap::kNullValueRootIndex);
  __ cmp(object, ip);
  __ beq(&false_result);

  // String values is not instance of anything.
  Condition is_string = masm_->IsObjectStringType(object, temp);
  __ b(is_string, &false_result);

  // Go to the deferred code.
  __ b(deferred->entry());

  __ bind(&false_result);
  __ LoadRoot(result, Heap::kFalseValueRootIndex);

  // Here result has either true or false. Deferred code also produces true or
  // false object.
  __ bind(deferred->exit());
  __ bind(&done);
}


void LCodeGen::DoDeferredInstanceOfKnownGlobal(LInstanceOfKnownGlobal* instr,
                                               Label* map_check) {
  Register result = ToRegister(instr->result());
  ASSERT(result.is(r3));

  InstanceofStub::Flags flags = InstanceofStub::kNoFlags;
  flags = static_cast<InstanceofStub::Flags>(
      flags | InstanceofStub::kArgsInRegisters);
  flags = static_cast<InstanceofStub::Flags>(
      flags | InstanceofStub::kCallSiteInlineCheck);
  flags = static_cast<InstanceofStub::Flags>(
      flags | InstanceofStub::kReturnTrueFalseObject);
  InstanceofStub stub(flags);

  PushSafepointRegistersScope scope(this, Safepoint::kWithRegisters);

  // Get the temp register reserved by the instruction. This needs to be r7 as
  // its slot of the pushing of safepoint registers is used to communicate the
  // offset to the location of the map check.
  Register temp = ToRegister(instr->temp());
  ASSERT(temp.is(r7));
  __ LoadHeapObject(InstanceofStub::right(), instr->function());
  static const int kAdditionalDelta = 7;
  int delta = masm_->InstructionsGeneratedSince(map_check) + kAdditionalDelta;
  Label before_push_delta;
  __ bind(&before_push_delta);
  __ mov(temp, Operand(delta * kPointerSize));
  // The mov above can generate one or two instructions. The delta was computed
  // for two instructions, so we need to pad here in case of one instruction.
  if (masm_->InstructionsGeneratedSince(&before_push_delta) != 2) {
    ASSERT_EQ(1, masm_->InstructionsGeneratedSince(&before_push_delta));
    __ nop();
  }
  __ StoreToSafepointRegisterSlot(temp, temp);
  CallCodeGeneric(stub.GetCode(),
                  RelocInfo::CODE_TARGET,
                  instr,
                  RECORD_SAFEPOINT_WITH_REGISTERS_AND_NO_ARGUMENTS);
  LEnvironment* env = instr->GetDeferredLazyDeoptimizationEnvironment();
  safepoints_.RecordLazyDeoptimizationIndex(env->deoptimization_index());
  // Put the result value into the result register slot and
  // restore all registers.
  __ StoreToSafepointRegisterSlot(result, result);
}


void LCodeGen::DoCmpT(LCmpT* instr) {
  Token::Value op = instr->op();

  Handle<Code> ic = CompareIC::GetUninitialized(op);
  CallCode(ic, RelocInfo::CODE_TARGET, instr);
  // This instruction also signals no smi code inlined
  __ cmpi(r3, Operand::Zero());

  Condition condition = ComputeCompareCondition(op);
  Label true_value, done;

  __ b(condition, &true_value);

  __ LoadRoot(ToRegister(instr->result()), Heap::kFalseValueRootIndex);
  __ b(&done);

  __ bind(&true_value);
  __ LoadRoot(ToRegister(instr->result()), Heap::kTrueValueRootIndex);

  __ bind(&done);
}


void LCodeGen::DoReturn(LReturn* instr) {
  if (FLAG_trace) {
    // Push the return value on the stack as the parameter.
    // Runtime::TraceExit returns its parameter in r3.
    __ push(r3);
    __ CallRuntime(Runtime::kTraceExit, 1);
  }
  int32_t sp_delta = (GetParameterCount() + 1) * kPointerSize;
  __ mr(sp, fp);
  __ Pop(r0, fp);
  __ mtlr(r0);
  __ addi(sp, sp, Operand(sp_delta));
  __ blr();
}


void LCodeGen::DoLoadGlobalCell(LLoadGlobalCell* instr) {
  Register result = ToRegister(instr->result());
  __ mov(ip, Operand(Handle<Object>(instr->hydrogen()->cell())));
  __ lwz(result, FieldMemOperand(ip, JSGlobalPropertyCell::kValueOffset));
  if (instr->hydrogen()->RequiresHoleCheck()) {
    __ LoadRoot(ip, Heap::kTheHoleValueRootIndex);
    __ cmp(result, ip);
    DeoptimizeIf(eq, instr->environment());
  }
}


void LCodeGen::DoLoadGlobalGeneric(LLoadGlobalGeneric* instr) {
  ASSERT(ToRegister(instr->global_object()).is(r3));
  ASSERT(ToRegister(instr->result()).is(r3));

  __ mov(r5, Operand(instr->name()));
  RelocInfo::Mode mode = instr->for_typeof() ? RelocInfo::CODE_TARGET
                                             : RelocInfo::CODE_TARGET_CONTEXT;
  Handle<Code> ic = isolate()->builtins()->LoadIC_Initialize();
  CallCode(ic, mode, instr);
}


void LCodeGen::DoStoreGlobalCell(LStoreGlobalCell* instr) {
  Register value = ToRegister(instr->value());
  Register cell = scratch0();

  // Load the cell.
  __ mov(cell, Operand(instr->hydrogen()->cell()));

  // If the cell we are storing to contains the hole it could have
  // been deleted from the property dictionary. In that case, we need
  // to update the property details in the property dictionary to mark
  // it as no longer deleted.
  if (instr->hydrogen()->RequiresHoleCheck()) {
    // We use a temp to check the payload (CompareRoot might clobber ip).
    Register payload = ToRegister(instr->temp());
    __ lwz(payload, FieldMemOperand(cell, JSGlobalPropertyCell::kValueOffset));
    __ CompareRoot(payload, Heap::kTheHoleValueRootIndex);
    DeoptimizeIf(eq, instr->environment());
  }

  // Store the value.
  __ stw(value, FieldMemOperand(cell, JSGlobalPropertyCell::kValueOffset));
  // Cells are always rescanned, so no write barrier here.
}


void LCodeGen::DoStoreGlobalGeneric(LStoreGlobalGeneric* instr) {
  ASSERT(ToRegister(instr->global_object()).is(r4));
  ASSERT(ToRegister(instr->value()).is(r3));

  __ mov(r5, Operand(instr->name()));
  Handle<Code> ic = (instr->strict_mode_flag() == kStrictMode)
      ? isolate()->builtins()->StoreIC_Initialize_Strict()
      : isolate()->builtins()->StoreIC_Initialize();
  CallCode(ic, RelocInfo::CODE_TARGET_CONTEXT, instr);
}


void LCodeGen::DoLoadContextSlot(LLoadContextSlot* instr) {
  Register context = ToRegister(instr->context());
  Register result = ToRegister(instr->result());
  __ lwz(result, ContextOperand(context, instr->slot_index()));
  if (instr->hydrogen()->RequiresHoleCheck()) {
    __ LoadRoot(ip, Heap::kTheHoleValueRootIndex);
    __ cmp(result, ip);
    if (instr->hydrogen()->DeoptimizesOnHole()) {
      DeoptimizeIf(eq, instr->environment());
    } else {
      Label skip;
      __ bne(&skip);
      __ mov(result, Operand(factory()->undefined_value()));
      __ bind(&skip);
    }
  }
}


void LCodeGen::DoStoreContextSlot(LStoreContextSlot* instr) {
  Register context = ToRegister(instr->context());
  Register value = ToRegister(instr->value());
  Register scratch = scratch0();
  MemOperand target = ContextOperand(context, instr->slot_index());

  Label skip_assignment;

  if (instr->hydrogen()->RequiresHoleCheck()) {
    __ lwz(scratch, target);
    __ LoadRoot(ip, Heap::kTheHoleValueRootIndex);
    __ cmp(scratch, ip);
    if (instr->hydrogen()->DeoptimizesOnHole()) {
      DeoptimizeIf(eq, instr->environment());
    } else {
      __ bne(&skip_assignment);
    }
  }

  __ stw(value, target);
  if (instr->hydrogen()->NeedsWriteBarrier()) {
    HType type = instr->hydrogen()->value()->type();
    SmiCheck check_needed =
        type.IsHeapObject() ? OMIT_SMI_CHECK : INLINE_SMI_CHECK;
    __ RecordWriteContextSlot(context,
                              target.offset(),
                              value,
                              scratch,
                              kLRHasBeenSaved,
                              kSaveFPRegs,
                              EMIT_REMEMBERED_SET,
                              check_needed);
  }

  __ bind(&skip_assignment);
}


void LCodeGen::DoLoadNamedField(LLoadNamedField* instr) {
  Register object = ToRegister(instr->object());
  Register result = ToRegister(instr->result());
  if (instr->hydrogen()->is_in_object()) {
    __ lwz(result, FieldMemOperand(object, instr->hydrogen()->offset()));
  } else {
    __ lwz(result, FieldMemOperand(object, JSObject::kPropertiesOffset));
    __ lwz(result, FieldMemOperand(result, instr->hydrogen()->offset()));
  }
}


void LCodeGen::EmitLoadFieldOrConstantFunction(Register result,
                                               Register object,
                                               Handle<Map> type,
                                               Handle<String> name,
                                               LEnvironment* env) {
  LookupResult lookup(isolate());
  type->LookupDescriptor(NULL, *name, &lookup);
  ASSERT(lookup.IsFound() || lookup.IsCacheable());
  if (lookup.IsField()) {
    int index = lookup.GetLocalFieldIndexFromMap(*type);
    int offset = index * kPointerSize;
    if (index < 0) {
      // Negative property indices are in-object properties, indexed
      // from the end of the fixed part of the object.
      __ lwz(result, FieldMemOperand(object, offset + type->instance_size()));
    } else {
      // Non-negative property indices are in the properties array.
      __ lwz(result, FieldMemOperand(object, JSObject::kPropertiesOffset));
      __ lwz(result, FieldMemOperand(result, offset + FixedArray::kHeaderSize));
    }
  } else if (lookup.IsConstantFunction()) {
    Handle<JSFunction> function(lookup.GetConstantFunctionFromMap(*type));
    __ LoadHeapObject(result, function);
  } else {
    // Negative lookup.
    // Check prototypes.
    Handle<HeapObject> current(HeapObject::cast((*type)->prototype()));
    Heap* heap = type->GetHeap();
    while (*current != heap->null_value()) {
      __ LoadHeapObject(result, current);
      __ lwz(result, FieldMemOperand(result, HeapObject::kMapOffset));
      __ Cmpi(result, Operand(Handle<Map>(current->map())), r0);
      DeoptimizeIf(ne, env);
      current =
          Handle<HeapObject>(HeapObject::cast(current->map()->prototype()));
    }
    __ LoadRoot(result, Heap::kUndefinedValueRootIndex);
  }
}


void LCodeGen::DoLoadNamedFieldPolymorphic(LLoadNamedFieldPolymorphic* instr) {
  Register object = ToRegister(instr->object());
  Register result = ToRegister(instr->result());
  Register object_map = scratch0();

  int map_count = instr->hydrogen()->types()->length();
  bool need_generic = instr->hydrogen()->need_generic();

  if (map_count == 0 && !need_generic) {
    DeoptimizeIf(al, instr->environment());
    return;
  }
  Handle<String> name = instr->hydrogen()->name();
  Label done;
  __ lwz(object_map, FieldMemOperand(object, HeapObject::kMapOffset));
  for (int i = 0; i < map_count; ++i) {
    bool last = (i == map_count - 1);
    Handle<Map> map = instr->hydrogen()->types()->at(i);
    Label check_passed;
    __ CompareMap(
        object_map, map, &check_passed, ALLOW_ELEMENT_TRANSITION_MAPS);
    if (last && !need_generic) {
      DeoptimizeIf(ne, instr->environment());
      __ bind(&check_passed);
      EmitLoadFieldOrConstantFunction(
          result, object, map, name, instr->environment());
    } else {
      Label next;
      __ bne(&next);
      __ bind(&check_passed);
      EmitLoadFieldOrConstantFunction(
          result, object, map, name, instr->environment());
      __ b(&done);
      __ bind(&next);
    }
  }
  if (need_generic) {
    __ mov(r5, Operand(name));
    Handle<Code> ic = isolate()->builtins()->LoadIC_Initialize();
    CallCode(ic, RelocInfo::CODE_TARGET, instr);
  }
  __ bind(&done);
}


void LCodeGen::DoLoadNamedGeneric(LLoadNamedGeneric* instr) {
  ASSERT(ToRegister(instr->object()).is(r3));
  ASSERT(ToRegister(instr->result()).is(r3));

  // Name is always in r5.
  __ mov(r5, Operand(instr->name()));
  Handle<Code> ic = isolate()->builtins()->LoadIC_Initialize();
  CallCode(ic, RelocInfo::CODE_TARGET, instr);
}


void LCodeGen::DoLoadFunctionPrototype(LLoadFunctionPrototype* instr) {
  Register scratch = scratch0();
  Register function = ToRegister(instr->function());
  Register result = ToRegister(instr->result());

  // Check that the function really is a function. Load map into the
  // result register.
  __ CompareObjectType(function, result, scratch, JS_FUNCTION_TYPE);
  DeoptimizeIf(ne, instr->environment());

  // Make sure that the function has an instance prototype.
  Label non_instance;
  __ lbz(scratch, FieldMemOperand(result, Map::kBitFieldOffset));
  __ TestBit(scratch, 31 - Map::kHasNonInstancePrototype, r0);
  __ bne(&non_instance, cr0);

  // Get the prototype or initial map from the function.
  __ lwz(result,
         FieldMemOperand(function, JSFunction::kPrototypeOrInitialMapOffset));

  // Check that the function has a prototype or an initial map.
  __ LoadRoot(ip, Heap::kTheHoleValueRootIndex);
  __ cmp(result, ip);
  DeoptimizeIf(eq, instr->environment());

  // If the function does not have an initial map, we're done.
  Label done;
  __ CompareObjectType(result, scratch, scratch, MAP_TYPE);
  __ bne(&done);

  // Get the prototype from the initial map.
  __ lwz(result, FieldMemOperand(result, Map::kPrototypeOffset));
  __ b(&done);

  // Non-instance prototype: Fetch prototype from constructor field
  // in initial map.
  __ bind(&non_instance);
  __ lwz(result, FieldMemOperand(result, Map::kConstructorOffset));

  // All done.
  __ bind(&done);
}


void LCodeGen::DoLoadElements(LLoadElements* instr) {
  Register result = ToRegister(instr->result());
  Register input = ToRegister(instr->object());
  Register scratch = scratch0();

  __ lwz(result, FieldMemOperand(input, JSObject::kElementsOffset));
  if (FLAG_debug_code) {
    Label done, fail;
    __ lwz(scratch, FieldMemOperand(result, HeapObject::kMapOffset));
    __ LoadRoot(ip, Heap::kFixedArrayMapRootIndex);
    __ cmp(scratch, ip);
    __ beq(&done);
    __ LoadRoot(ip, Heap::kFixedCOWArrayMapRootIndex);
    __ cmp(scratch, ip);
    __ beq(&done);
    // |scratch| still contains |input|'s map.
    __ lbz(scratch, FieldMemOperand(scratch, Map::kBitField2Offset));
    __ ExtractBitMask(scratch, scratch, Map::kElementsKindMask);
    __ cmpi(scratch, Operand(GetInitialFastElementsKind()));
    __ blt(&fail);
    __ cmpi(scratch, Operand(TERMINAL_FAST_ELEMENTS_KIND));
    __ ble(&done);
    __ cmpi(scratch, Operand(FIRST_EXTERNAL_ARRAY_ELEMENTS_KIND));
    __ blt(&fail);
    __ cmpi(scratch, Operand(LAST_EXTERNAL_ARRAY_ELEMENTS_KIND));
    __ ble(&done);
    __ bind(&fail);
    __ Abort("Check for fast or external elements failed.");
    __ bind(&done);
  }
}


void LCodeGen::DoLoadExternalArrayPointer(
    LLoadExternalArrayPointer* instr) {
  Register to_reg = ToRegister(instr->result());
  Register from_reg  = ToRegister(instr->object());
  __ lwz(to_reg, FieldMemOperand(from_reg,
                                 ExternalArray::kExternalPointerOffset));
}


void LCodeGen::DoAccessArgumentsAt(LAccessArgumentsAt* instr) {
  Register arguments = ToRegister(instr->arguments());
  Register length = ToRegister(instr->length());
  Register index = ToRegister(instr->index());
  Register result = ToRegister(instr->result());

  // There are two words between the frame pointer and the last argument.
  // Subtracting from length accounts for one of them add one more.
  __ sub(length, length, index);
  __ addi(length, length, Operand(1));
  __ slwi(r0, length, Operand(kPointerSizeLog2));
  __ lwzx(result, MemOperand(arguments, r0));
}


void LCodeGen::DoLoadKeyedFastElement(LLoadKeyedFastElement* instr) {
  Register elements = ToRegister(instr->elements());
  Register result = ToRegister(instr->result());
  Register scratch = scratch0();
  Register store_base = scratch;
  int offset = 0;

  if (instr->key()->IsConstantOperand()) {
    LConstantOperand* const_operand = LConstantOperand::cast(instr->key());
    offset = FixedArray::OffsetOfElementAt(ToInteger32(const_operand) +
                                           instr->additional_index());
    store_base = elements;
  } else {
    Register key = EmitLoadRegister(instr->key(), scratch0());
    // Even though the HLoadKeyedFastElement instruction forces the input
    // representation for the key to be an integer, the input gets replaced
    // during bound check elimination with the index argument to the bounds
    // check, which can be tagged, so that case must be handled here, too.
    if (instr->hydrogen()->key()->representation().IsTagged()) {
      __ slwi(r0, key, Operand(kPointerSizeLog2 - kSmiTagSize));
    } else {
      __ slwi(r0, key, Operand(kPointerSizeLog2));
    }
    __ add(scratch, elements, r0);
    offset = FixedArray::OffsetOfElementAt(instr->additional_index());
  }
  __ lwz(result, FieldMemOperand(store_base, offset));

  // Check for the hole value.
  if (instr->hydrogen()->RequiresHoleCheck()) {
    if (IsFastSmiElementsKind(instr->hydrogen()->elements_kind())) {
      __ TestIfSmi(result, r0);
      DeoptimizeIf(ne, instr->environment(), cr0);
    } else {
      __ LoadRoot(scratch, Heap::kTheHoleValueRootIndex);
      __ cmp(result, scratch);
      DeoptimizeIf(eq, instr->environment());
    }
  }
}


void LCodeGen::DoLoadKeyedFastDoubleElement(
    LLoadKeyedFastDoubleElement* instr) {
  Register elements = ToRegister(instr->elements());
  bool key_is_constant = instr->key()->IsConstantOperand();
  Register key = no_reg;
  DwVfpRegister result = ToDoubleRegister(instr->result());
  Register scratch = scratch0();

  int element_size_shift = ElementsKindToShiftSize(FAST_DOUBLE_ELEMENTS);
  int shift_size = (instr->hydrogen()->key()->representation().IsTagged())
      ? (element_size_shift - kSmiTagSize) : element_size_shift;
  int constant_key = 0;
  if (key_is_constant) {
    constant_key = ToInteger32(LConstantOperand::cast(instr->key()));
    if (constant_key & 0xF0000000) {
      Abort("array index constant value too big.");
    }
  } else {
    key = ToRegister(instr->key());
  }

  if (key_is_constant) {
    __ Add(elements, elements,
           (FixedDoubleArray::kHeaderSize - kHeapObjectTag) +
           ((constant_key + instr->additional_index()) << element_size_shift),
           r0);
  } else {
    __ slwi(r0, key, Operand(shift_size));
    __ add(elements, elements, r0);
    __ mov(r0, Operand((FixedDoubleArray::kHeaderSize - kHeapObjectTag) +
                       (instr->additional_index() << element_size_shift)));
    __ add(elements, elements, r0);
  }

  if (instr->hydrogen()->RequiresHoleCheck()) {
#if __FLOAT_WORD_ORDER == __LITTLE_ENDIAN
    __ lwz(scratch, MemOperand(elements, sizeof(kHoleNanLower32)));
#else
    __ lwz(scratch, MemOperand(elements));
#endif
    __ Cmpi(scratch, Operand(kHoleNanUpper32), r0);
    DeoptimizeIf(eq, instr->environment());
  }

  __ lfd(result, MemOperand(elements, 0));
}


MemOperand LCodeGen::PrepareKeyedOperand(Register key,
                                         Register base,
                                         bool key_is_constant,
                                         int constant_key,
                                         int element_size,
                                         int shift_size,
                                         int additional_index,
                                         int additional_offset) {
  Register scratch = scratch0();

  if (key_is_constant) {
    return MemOperand(base,
                      (constant_key << element_size) + additional_offset);
  }

  if (!(additional_index || shift_size)) {
      return MemOperand(base, key);
  }

  if (additional_index) {
    additional_index *= 1 << (element_size - shift_size);
    __ Add(scratch, key, additional_index, r0);
  }

  if (shift_size) {
    Register effective_key = (additional_index ? scratch : key);
    if (shift_size > 0) {
      __ slwi(scratch, effective_key, Operand(shift_size));
    } else {
      ASSERT_EQ(-1, shift_size);
      __ srwi(scratch, effective_key, Operand(1));
    }
  }

  return MemOperand(base, scratch);
}


void LCodeGen::DoLoadKeyedSpecializedArrayElement(
    LLoadKeyedSpecializedArrayElement* instr) {
  Register external_pointer = ToRegister(instr->external_pointer());
  Register key = no_reg;
  ElementsKind elements_kind = instr->elements_kind();
  bool key_is_constant = instr->key()->IsConstantOperand();
  int constant_key = 0;
  if (key_is_constant) {
    constant_key = ToInteger32(LConstantOperand::cast(instr->key()));
    if (constant_key & 0xF0000000) {
      Abort("array index constant value too big.");
    }
  } else {
    key = ToRegister(instr->key());
  }
  int element_size_shift = ElementsKindToShiftSize(elements_kind);
  int shift_size = (instr->hydrogen()->key()->representation().IsTagged())
      ? (element_size_shift - kSmiTagSize) : element_size_shift;
  int additional_offset = instr->additional_index() << element_size_shift;

  if (elements_kind == EXTERNAL_FLOAT_ELEMENTS ||
      elements_kind == EXTERNAL_DOUBLE_ELEMENTS) {
    DwVfpRegister result = ToDoubleRegister(instr->result());
    if (key_is_constant) {
      __ Add(scratch0(), external_pointer,
             constant_key << element_size_shift,
             r0);
    } else {
      __ slwi(r0, key, Operand(shift_size));
      __ add(scratch0(), external_pointer, r0);
    }
    if (elements_kind == EXTERNAL_FLOAT_ELEMENTS) {
      __ lfs(result, MemOperand(scratch0(), additional_offset));
    } else  {  // i.e. elements_kind == EXTERNAL_DOUBLE_ELEMENTS
      __ lfd(result, MemOperand(scratch0(), additional_offset));
    }
  } else {
    Register result = ToRegister(instr->result());
    MemOperand mem_operand = PrepareKeyedOperand(
        key, external_pointer, key_is_constant, constant_key,
        element_size_shift, shift_size,
        instr->additional_index(), additional_offset);
    switch (elements_kind) {
      case EXTERNAL_BYTE_ELEMENTS:
        if (key_is_constant) {
          __ lbz(result, mem_operand);
        } else {
          __ lbzx(result, mem_operand);
        }
        __ extsb(result, result);
        break;
      case EXTERNAL_PIXEL_ELEMENTS:
      case EXTERNAL_UNSIGNED_BYTE_ELEMENTS:
        if (key_is_constant) {
          __ lbz(result, mem_operand);
        } else {
          __ lbzx(result, mem_operand);
        }
        break;
      case EXTERNAL_SHORT_ELEMENTS:
        if (key_is_constant) {
          __ lhz(result, mem_operand);
        } else {
          __ lhzx(result, mem_operand);
        }
        __ extsh(result, result);
        break;
      case EXTERNAL_UNSIGNED_SHORT_ELEMENTS:
        if (key_is_constant) {
          __ lhz(result, mem_operand);
        } else {
          __ lhzx(result, mem_operand);
        }
        break;
      case EXTERNAL_INT_ELEMENTS:
        if (key_is_constant) {
          __ lwz(result, mem_operand);
        } else {
          __ lwzx(result, mem_operand);
        }
        break;
      case EXTERNAL_UNSIGNED_INT_ELEMENTS:
        if (key_is_constant) {
          __ lwz(result, mem_operand);
        } else {
          __ lwzx(result, mem_operand);
        }
        if (!instr->hydrogen()->CheckFlag(HInstruction::kUint32)) {
          __ lis(r0, Operand(SIGN_EXT_IMM16(0x8000)));
          __ cmpl(result, r0);
          DeoptimizeIf(ge, instr->environment());
        }
        break;
      case EXTERNAL_FLOAT_ELEMENTS:
      case EXTERNAL_DOUBLE_ELEMENTS:
      case FAST_HOLEY_DOUBLE_ELEMENTS:
      case FAST_HOLEY_ELEMENTS:
      case FAST_HOLEY_SMI_ELEMENTS:
      case FAST_DOUBLE_ELEMENTS:
      case FAST_ELEMENTS:
      case FAST_SMI_ELEMENTS:
      case DICTIONARY_ELEMENTS:
      case NON_STRICT_ARGUMENTS_ELEMENTS:
        UNREACHABLE();
        break;
    }
  }
}


void LCodeGen::DoLoadKeyedGeneric(LLoadKeyedGeneric* instr) {
  ASSERT(ToRegister(instr->object()).is(r4));
  ASSERT(ToRegister(instr->key()).is(r3));

  Handle<Code> ic = isolate()->builtins()->KeyedLoadIC_Initialize();
  CallCode(ic, RelocInfo::CODE_TARGET, instr);
}


void LCodeGen::DoArgumentsElements(LArgumentsElements* instr) {
  Register scratch = scratch0();
  Register result = ToRegister(instr->result());

  if (instr->hydrogen()->from_inlined()) {
    __ sub(result, sp, Operand(2 * kPointerSize));
  } else {
    // Check if the calling frame is an arguments adaptor frame.
    Label done, adapted;
    __ lwz(scratch, MemOperand(fp, StandardFrameConstants::kCallerFPOffset));
    __ lwz(result, MemOperand(scratch, StandardFrameConstants::kContextOffset));
    __ Cmpi(result, Operand(Smi::FromInt(StackFrame::ARGUMENTS_ADAPTOR)), r0);

    // Result is the frame pointer for the frame if not adapted and for the real
    // frame below the adaptor frame if adapted.
    __ beq(&adapted);
    __ mr(result, fp);
    __ b(&done);

    __ bind(&adapted);
    __ mr(result, scratch);
    __ bind(&done);
  }
}


void LCodeGen::DoArgumentsLength(LArgumentsLength* instr) {
  Register elem = ToRegister(instr->elements());
  Register result = ToRegister(instr->result());

  Label done;

  // If no arguments adaptor frame the number of arguments is fixed.
  __ cmp(fp, elem);
  __ mov(result, Operand(scope()->num_parameters()));
  __ beq(&done);

  // Arguments adaptor frame present. Get argument length from there.
  __ lwz(result, MemOperand(fp, StandardFrameConstants::kCallerFPOffset));
  __ lwz(result,
         MemOperand(result, ArgumentsAdaptorFrameConstants::kLengthOffset));
  __ SmiUntag(result);

  // Argument length is in result register.
  __ bind(&done);
}


void LCodeGen::DoWrapReceiver(LWrapReceiver* instr) {
  Register receiver = ToRegister(instr->receiver());
  Register function = ToRegister(instr->function());
  Register scratch = scratch0();

  // If the receiver is null or undefined, we have to pass the global
  // object as a receiver to normal functions. Values have to be
  // passed unchanged to builtins and strict-mode functions.
  Label global_object, receiver_ok;

  // Do not transform the receiver to object for strict mode
  // functions.
  __ lwz(scratch,
         FieldMemOperand(function, JSFunction::kSharedFunctionInfoOffset));
  __ lwz(scratch,
         FieldMemOperand(scratch, SharedFunctionInfo::kCompilerHintsOffset));
  __ TestBit(scratch,
             31 - (SharedFunctionInfo::kStrictModeFunction + kSmiTagSize), r0);
  __ bne(&receiver_ok, cr0);

  // Do not transform the receiver to object for builtins.
  __ TestBit(scratch, 31 - (SharedFunctionInfo::kNative + kSmiTagSize), r0);
  __ bne(&receiver_ok, cr0);

  // Normal function. Replace undefined or null with global receiver.
  __ LoadRoot(scratch, Heap::kNullValueRootIndex);
  __ cmp(receiver, scratch);
  __ beq(&global_object);
  __ LoadRoot(scratch, Heap::kUndefinedValueRootIndex);
  __ cmp(receiver, scratch);
  __ beq(&global_object);

  // Deoptimize if the receiver is not a JS object.
  __ TestIfSmi(receiver, r0);
  DeoptimizeIf(eq, instr->environment(), cr0);
  __ CompareObjectType(receiver, scratch, scratch, FIRST_SPEC_OBJECT_TYPE);
  DeoptimizeIf(lt, instr->environment());
  __ b(&receiver_ok);

  __ bind(&global_object);
  __ lwz(receiver, GlobalObjectOperand());
  __ lwz(receiver,
         FieldMemOperand(receiver, JSGlobalObject::kGlobalReceiverOffset));
  __ bind(&receiver_ok);
}


void LCodeGen::DoApplyArguments(LApplyArguments* instr) {
  Register receiver = ToRegister(instr->receiver());
  Register function = ToRegister(instr->function());
  Register length = ToRegister(instr->length());
  Register elements = ToRegister(instr->elements());
  Register scratch = scratch0();
  ASSERT(receiver.is(r3));  // Used for parameter count.
  ASSERT(function.is(r4));  // Required by InvokeFunction.
  ASSERT(ToRegister(instr->result()).is(r3));

  // Copy the arguments to this function possibly from the
  // adaptor frame below it.
  const uint32_t kArgumentsLimit = 1 * KB;
  __ cmpli(length, Operand(kArgumentsLimit));
  DeoptimizeIf(gt, instr->environment());

  // Push the receiver and use the register to keep the original
  // number of arguments.
  __ push(receiver);
  __ mr(receiver, length);
  // The arguments are at a one pointer size offset from elements.
  __ addi(elements, elements, Operand(1 * kPointerSize));

  // Loop through the arguments pushing them onto the execution
  // stack.
  Label invoke, loop;
  // length is a small non-negative integer, due to the test above.
  __ cmpi(length, Operand::Zero());
  __ beq(&invoke);
  __ mtctr(length);
  __ bind(&loop);
  __ slwi(r0, length, Operand(2));
  __ lwzx(scratch, MemOperand(elements, r0));
  __ push(scratch);
  __ addi(length, length, Operand(-1));
  __ bdnz(&loop);

  __ bind(&invoke);
  ASSERT(instr->HasPointerMap());
  LPointerMap* pointers = instr->pointer_map();
  RecordPosition(pointers->position());
  SafepointGenerator safepoint_generator(
      this, pointers, Safepoint::kLazyDeopt);
  // The number of arguments is stored in receiver which is r3, as expected
  // by InvokeFunction.
  ParameterCount actual(receiver);
  __ InvokeFunction(function, actual, CALL_FUNCTION,
                    safepoint_generator, CALL_AS_METHOD);
  __ lwz(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
}


void LCodeGen::DoPushArgument(LPushArgument* instr) {
  LOperand* argument = instr->value();
  if (argument->IsDoubleRegister() || argument->IsDoubleStackSlot()) {
    Abort("DoPushArgument not implemented for double type.");
  } else {
    Register argument_reg = EmitLoadRegister(argument, ip);
    __ push(argument_reg);
  }
}


void LCodeGen::DoDrop(LDrop* instr) {
  __ Drop(instr->count());
}


void LCodeGen::DoThisFunction(LThisFunction* instr) {
  Register result = ToRegister(instr->result());
  __ lwz(result, MemOperand(fp, JavaScriptFrameConstants::kFunctionOffset));
}


void LCodeGen::DoContext(LContext* instr) {
  Register result = ToRegister(instr->result());
  __ mr(result, cp);
}


void LCodeGen::DoOuterContext(LOuterContext* instr) {
  Register context = ToRegister(instr->context());
  Register result = ToRegister(instr->result());
  __ lwz(result,
         MemOperand(context, Context::SlotOffset(Context::PREVIOUS_INDEX)));
}


void LCodeGen::DoDeclareGlobals(LDeclareGlobals* instr) {
  __ push(cp);  // The context is the first argument.
  __ LoadHeapObject(scratch0(), instr->hydrogen()->pairs());
  __ push(scratch0());
  __ mov(scratch0(), Operand(Smi::FromInt(instr->hydrogen()->flags())));
  __ push(scratch0());
  CallRuntime(Runtime::kDeclareGlobals, 3, instr);
}


void LCodeGen::DoGlobalObject(LGlobalObject* instr) {
  Register result = ToRegister(instr->result());
  __ lwz(result, ContextOperand(cp, Context::GLOBAL_OBJECT_INDEX));
}


void LCodeGen::DoGlobalReceiver(LGlobalReceiver* instr) {
  Register global = ToRegister(instr->global_object());
  Register result = ToRegister(instr->result());
  __ lwz(result, FieldMemOperand(global, GlobalObject::kGlobalReceiverOffset));
}


void LCodeGen::CallKnownFunction(Handle<JSFunction> function,
                                 int arity,
                                 LInstruction* instr,
                                 CallKind call_kind,
                                 R4State r4_state) {
  bool can_invoke_directly = !function->NeedsArgumentsAdaption() ||
      function->shared()->formal_parameter_count() == arity;

  LPointerMap* pointers = instr->pointer_map();
  RecordPosition(pointers->position());

  if (can_invoke_directly) {
    if (r4_state == R4_UNINITIALIZED) {
      __ LoadHeapObject(r4, function);
    }

    // Change context.
    __ lwz(cp, FieldMemOperand(r4, JSFunction::kContextOffset));

    // Set r3 to arguments count if adaption is not needed. Assumes that r3
    // is available to write to at this point.
    if (!function->NeedsArgumentsAdaption()) {
      __ mov(r3, Operand(arity));
    }

    // Invoke function.
    __ SetCallKind(r8, call_kind);
    __ lwz(ip, FieldMemOperand(r4, JSFunction::kCodeEntryOffset));
    __ Call(ip);

    // Set up deoptimization.
    RecordSafepointWithLazyDeopt(instr, RECORD_SIMPLE_SAFEPOINT);
  } else {
    SafepointGenerator generator(this, pointers, Safepoint::kLazyDeopt);
    ParameterCount count(arity);
    __ InvokeFunction(function, count, CALL_FUNCTION, generator, call_kind);
  }

  // Restore context.
  __ lwz(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
}


void LCodeGen::DoCallConstantFunction(LCallConstantFunction* instr) {
  ASSERT(ToRegister(instr->result()).is(r3));
  CallKnownFunction(instr->function(),
                    instr->arity(),
                    instr,
                    CALL_AS_METHOD,
                    R4_UNINITIALIZED);
}


void LCodeGen::DoDeferredMathAbsTaggedHeapNumber(LUnaryMathOperation* instr) {
  Register input = ToRegister(instr->value());
  Register result = ToRegister(instr->result());
  Register scratch = scratch0();

  // Deoptimize if not a heap number.
  __ lwz(scratch, FieldMemOperand(input, HeapObject::kMapOffset));
  __ LoadRoot(ip, Heap::kHeapNumberMapRootIndex);
  __ cmp(scratch, ip);
  DeoptimizeIf(ne, instr->environment());

  Label done;
  Register exponent = scratch0();
  scratch = no_reg;
  __ lwz(exponent, FieldMemOperand(input, HeapNumber::kExponentOffset));
  // Check the sign of the argument. If the argument is positive, just
  // return it.
  __ TestBit(exponent, 0, r0);  // test sign bit
  // Move the input to the result if necessary.
  __ Move(result, input);
  __ beq(&done, cr0);

  // Input is negative. Reverse its sign.
  // Preserve the value of all registers.
  {
    PushSafepointRegistersScope scope(this, Safepoint::kWithRegisters);

    // Registers were saved at the safepoint, so we can use
    // many scratch registers.
    Register tmp1 = input.is(r4) ? r3 : r4;
    Register tmp2 = input.is(r5) ? r3 : r5;
    Register tmp3 = input.is(r6) ? r3 : r6;
    Register tmp4 = input.is(r7) ? r3 : r7;

    // exponent: floating point exponent value.

    Label allocated, slow;
    __ LoadRoot(tmp4, Heap::kHeapNumberMapRootIndex);
    __ AllocateHeapNumber(tmp1, tmp2, tmp3, tmp4, &slow);
    __ b(&allocated);

    // Slow case: Call the runtime system to do the number allocation.
    __ bind(&slow);

    CallRuntimeFromDeferred(Runtime::kAllocateHeapNumber, 0, instr);
    // Set the pointer to the new heap number in tmp.
    if (!tmp1.is(r3)) __ mr(tmp1, r3);
    // Restore input_reg after call to runtime.
    __ LoadFromSafepointRegisterSlot(input, input);
    __ lwz(exponent, FieldMemOperand(input, HeapNumber::kExponentOffset));

    __ bind(&allocated);
    // exponent: floating point exponent value.
    // tmp1: allocated heap number.
    STATIC_ASSERT(HeapNumber::kSignMask == 0x80000000u);
    __ ExtractBitRange(exponent, exponent, 1, 31);  // clear sign bit
    __ stw(exponent, FieldMemOperand(tmp1, HeapNumber::kExponentOffset));
    __ lwz(tmp2, FieldMemOperand(input, HeapNumber::kMantissaOffset));
    __ stw(tmp2, FieldMemOperand(tmp1, HeapNumber::kMantissaOffset));

    __ StoreToSafepointRegisterSlot(tmp1, result);
  }

  __ bind(&done);
}


void LCodeGen::EmitIntegerMathAbs(LUnaryMathOperation* instr) {
  Register input = ToRegister(instr->value());
  Register result = ToRegister(instr->result());
  Label done;
  __ cmpi(input, Operand::Zero());
  __ Move(result, input);
  __ bge(&done);
  __ li(r0, Operand::Zero());  // clear xer
  __ mtxer(r0);
  __ neg(result, result, SetOE, SetRC);
  // Deoptimize on overflow.
  __ bnotoverflow(&done, cr0);
  DeoptimizeIf(al, instr->environment());
  __ bind(&done);
}


void LCodeGen::DoMathAbs(LUnaryMathOperation* instr) {
  // Class for deferred case.
  class DeferredMathAbsTaggedHeapNumber: public LDeferredCode {
   public:
    DeferredMathAbsTaggedHeapNumber(LCodeGen* codegen,
                                    LUnaryMathOperation* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() {
      codegen()->DoDeferredMathAbsTaggedHeapNumber(instr_);
    }
    virtual LInstruction* instr() { return instr_; }
   private:
    LUnaryMathOperation* instr_;
  };

  Representation r = instr->hydrogen()->value()->representation();
  if (r.IsDouble()) {
    DwVfpRegister input = ToDoubleRegister(instr->value());
    DwVfpRegister result = ToDoubleRegister(instr->result());
    __ fabs(result, input);
  } else if (r.IsInteger32()) {
    EmitIntegerMathAbs(instr);
  } else {
    // Representation is tagged.
    DeferredMathAbsTaggedHeapNumber* deferred =
        new(zone()) DeferredMathAbsTaggedHeapNumber(this, instr);
    Register input = ToRegister(instr->value());
    // Smi check.
    __ JumpIfNotSmi(input, deferred->entry());
    // If smi, handle it directly.
    EmitIntegerMathAbs(instr);
    __ bind(deferred->exit());
  }
}


void LCodeGen::DoMathFloor(LUnaryMathOperation* instr) {
  DoubleRegister input = ToDoubleRegister(instr->value());
  Register result = ToRegister(instr->result());
  Register scratch = scratch0();

  __ EmitVFPTruncate(kRoundToMinusInf,
                     result,
                     input,
                     scratch,
                     double_scratch0());
  DeoptimizeIf(ne, instr->environment());

  if (instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero)) {
    // Test for -0.
    Label done;
    __ cmpi(result, Operand::Zero());
    __ bne(&done);
    // Move high word to scrach and test sign bit
    __ sub(sp, sp, Operand(8));
    __ stfd(input, MemOperand(sp));
#if __FLOAT_WORD_ORDER == __LITTLE_ENDIAN
    __ lwz(scratch, MemOperand(sp, 4));
#else
    __ lwz(scratch, MemOperand(sp, 0));
#endif
    __ addi(sp, sp, Operand(8));
    __ TestBit(scratch, 0, r0);  // test sign bit
    DeoptimizeIf(ne, instr->environment(), cr0);
    __ bind(&done);
  }
}


void LCodeGen::DoMathRound(LUnaryMathOperation* instr) {
  DoubleRegister input = ToDoubleRegister(instr->value());
  Register result = ToRegister(instr->result());
  DwVfpRegister double_scratch1 = ToDoubleRegister(instr->temp());
  Register scratch = scratch0();
  DoubleRepresentation pointFive(0.5);
  Label done, check_sign_on_zero, skip1, skip2;


  // Extract exponent bits.
  __ sub(sp, sp, Operand(8));
  __ stfd(input, MemOperand(sp));
#if __FLOAT_WORD_ORDER == __LITTLE_ENDIAN
  __ lwz(result, MemOperand(sp, 4));
#else
  __ lwz(result, MemOperand(sp, 0));
#endif
  __ addi(sp, sp, Operand(8));
  __ ExtractBitMask(scratch, result, HeapNumber::kExponentMask);

  // If the number is in ]-0.5, +0.5[, the result is +/- 0.
  __ cmpi(scratch, Operand(HeapNumber::kExponentBias - 2));
  __ bgt(&skip1);
  __ li(result, Operand::Zero());
  if (instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero)) {
    __ b(&check_sign_on_zero);
  } else {
    __ b(&done);
  }

  // The following conversion will not work with numbers
  // outside of ]-2^32, 2^32[.
  __ bind(&skip1);
  __ cmpi(scratch, Operand(HeapNumber::kExponentBias + 32));
  DeoptimizeIf(ge, instr->environment());

  __ sub(sp, sp, Operand(8));
#if __FLOAT_WORD_ORDER == __LITTLE_ENDIAN
  __ mov(scratch, Operand(pointFive.bits >> 32));
  __ stw(scratch, MemOperand(sp, 4));
  __ mov(scratch, Operand(pointFive.bits & 0xffffffff));
  __ stw(scratch, MemOperand(sp, 0));
#else
  __ mov(scratch, Operand(pointFive.bits >> 32));
  __ stw(scratch, MemOperand(sp, 0));
  __ mov(scratch, Operand(pointFive.bits & 0xffffffff));
  __ stw(scratch, MemOperand(sp, 4));
#endif
  __ lfd(double_scratch0(), MemOperand(sp, 0));

  __ fadd(double_scratch0(), input, double_scratch0());

  // Save the original sign for later comparison.
  STATIC_ASSERT(HeapNumber::kSignMask == 0x80000000u);
  __ clrrwi(scratch, result, Operand(31));

  // Check sign of the result: if the sign changed, the input
  // value was in ]0.5, 0[ and the result should be -0.
  __ stfd(double_scratch0(), MemOperand(sp, 0));
#if __FLOAT_WORD_ORDER == __LITTLE_ENDIAN
  __ lwz(result, MemOperand(sp, 4));
#else
  __ lwz(result, MemOperand(sp, 0));
#endif
  __ addi(sp, sp, Operand(8));
  __ xor_(result, result, scratch, SetRC);
  if (instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero)) {
    DeoptimizeIf(lt, instr->environment(), cr0);
  } else {
    __ bge(&skip2);
    __ li(result, Operand::Zero());
    __ b(&done);
    __ bind(&skip2);
  }

  __ EmitVFPTruncate(kRoundToMinusInf,
                     result,
                     double_scratch0(),
                     scratch,
                     double_scratch1);
  DeoptimizeIf(ne, instr->environment());

  if (instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero)) {
    // Test for -0.
    __ cmpi(result, Operand::Zero());
    __ bne(&done);
    __ bind(&check_sign_on_zero);
    // Move high word to scrach and test sign bit
    __ sub(sp, sp, Operand(8));
    __ stfd(input, MemOperand(sp));
#if __FLOAT_WORD_ORDER == __LITTLE_ENDIAN
    __ lwz(scratch, MemOperand(sp, 4));
#else
    __ lwz(scratch, MemOperand(sp, 0));
#endif
    __ addi(sp, sp, Operand(8));
    __ TestBit(scratch, 0, r0);  // test sign bit
    DeoptimizeIf(ne, instr->environment(), cr0);
  }
  __ bind(&done);
}


void LCodeGen::DoMathSqrt(LUnaryMathOperation* instr) {
  DoubleRegister input = ToDoubleRegister(instr->value());
  DoubleRegister result = ToDoubleRegister(instr->result());
  __ fsqrt(result, input);
}


void LCodeGen::DoMathPowHalf(LUnaryMathOperation* instr) {
  DoubleRegister input = ToDoubleRegister(instr->value());
  DoubleRegister result = ToDoubleRegister(instr->result());
  DoubleRegister temp = ToDoubleRegister(instr->temp());
  DoubleRepresentation minusInf(-V8_INFINITY);

  // Note that according to ECMA-262 15.8.2.13:
  // Math.pow(-Infinity, 0.5) == Infinity
  // Math.sqrt(-Infinity) == NaN
  Label skip, done;

  __ sub(sp, sp, Operand(8));
#if __FLOAT_WORD_ORDER == __LITTLE_ENDIAN
  __ mov(scratch0(), Operand(minusInf.bits >> 32));
  __ stw(scratch0(), MemOperand(sp, 4));
  __ mov(scratch0(), Operand(minusInf.bits & 0xffffffff));
  __ stw(scratch0(), MemOperand(sp, 0));
#else
  __ mov(scratch0(), Operand(minusInf.bits >> 32));
  __ stw(scratch0(), MemOperand(sp, 0));
  __ mov(scratch0(), Operand(minusInf.bits & 0xffffffff));
  __ stw(scratch0(), MemOperand(sp, 4));
#endif
  __ lfd(temp, MemOperand(sp, 0));
  __ addi(sp, sp, Operand(8));

  __ fcmpu(input, temp);
  __ bne(&skip);
  __ fneg(result, temp);
  __ b(&done);

  // Add +0 to convert -0 to +0.
  __ bind(&skip);
  __ fadd(result, input, kDoubleRegZero);
  __ fsqrt(result, result);
  __ bind(&done);
}


void LCodeGen::DoPower(LPower* instr) {
  Representation exponent_type = instr->hydrogen()->right()->representation();
  // Having marked this as a call, we can use any registers.
  // Just make sure that the input/output registers are the expected ones.
  ASSERT(!instr->right()->IsDoubleRegister() ||
         ToDoubleRegister(instr->right()).is(d2));
  ASSERT(!instr->right()->IsRegister() ||
         ToRegister(instr->right()).is(r5));
  ASSERT(ToDoubleRegister(instr->left()).is(d1));
  ASSERT(ToDoubleRegister(instr->result()).is(d3));

  if (exponent_type.IsTagged()) {
    Label no_deopt;
    __ JumpIfSmi(r5, &no_deopt);
    __ lwz(r10, FieldMemOperand(r5, HeapObject::kMapOffset));
    __ LoadRoot(ip, Heap::kHeapNumberMapRootIndex);
    __ cmp(r10, ip);
    DeoptimizeIf(ne, instr->environment());
    __ bind(&no_deopt);
    MathPowStub stub(MathPowStub::TAGGED);
    __ CallStub(&stub);
  } else if (exponent_type.IsInteger32()) {
    MathPowStub stub(MathPowStub::INTEGER);
    __ CallStub(&stub);
  } else {
    ASSERT(exponent_type.IsDouble());
    MathPowStub stub(MathPowStub::DOUBLE);
    __ CallStub(&stub);
  }
}


void LCodeGen::DoRandom(LRandom* instr) {
  class DeferredDoRandom: public LDeferredCode {
   public:
    DeferredDoRandom(LCodeGen* codegen, LRandom* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() { codegen()->DoDeferredRandom(instr_); }
    virtual LInstruction* instr() { return instr_; }
   private:
    LRandom* instr_;
  };

  DeferredDoRandom* deferred = new(zone()) DeferredDoRandom(this, instr);

  // Having marked this instruction as a call we can use any
  // registers.
  ASSERT(ToDoubleRegister(instr->result()).is(d7));
  ASSERT(ToRegister(instr->global_object()).is(r3));

  static const int kSeedSize = sizeof(uint32_t);
#ifndef V8_TARGET_ARCH_PPC64  // todo fix (currently fails on 64bit)
  STATIC_ASSERT(kPointerSize == kSeedSize);
#endif

  __ lwz(r3, FieldMemOperand(r3, GlobalObject::kNativeContextOffset));
  static const int kRandomSeedOffset =
      FixedArray::kHeaderSize + Context::RANDOM_SEED_INDEX * kPointerSize;
  __ lwz(r5, FieldMemOperand(r3, kRandomSeedOffset));
  // r5: FixedArray of the native context's random seeds

  // Load state[0].
  __ lwz(r4, FieldMemOperand(r5, ByteArray::kHeaderSize));
  __ cmpi(r4, Operand::Zero());
  __ beq(deferred->entry());
  // Load state[1].
  __ lwz(r3, FieldMemOperand(r5, ByteArray::kHeaderSize + kSeedSize));
  // r4: state[0].
  // r3: state[1].

  // state[0] = 18273 * (state[0] & 0xFFFF) + (state[0] >> 16)
  __ andi(r6, r4, Operand(0xFFFF));
  __ li(r7, Operand(18273));
  __ mul(r6, r6, r7);
  __ srwi(r4, r4, Operand(16));
  __ add(r4, r6, r4);
  // Save state[0].
  __ stw(r4, FieldMemOperand(r5, ByteArray::kHeaderSize));

  // state[1] = 36969 * (state[1] & 0xFFFF) + (state[1] >> 16)
  __ andi(r6, r3, Operand(0xFFFF));
  __ mov(r7, Operand(36969));
  __ mul(r6, r6, r7);
  __ srwi(r3, r3, Operand(16));
  __ add(r3, r6, r3);
  // Save state[1].
  __ stw(r3, FieldMemOperand(r5, ByteArray::kHeaderSize + kSeedSize));

  // Random bit pattern = (state[0] << 14) + (state[1] & 0x3FFFF)
  __ ExtractBitMask(r3, r3, 0x3FFFF);
  __ slwi(r0, r4, Operand(14));
  __ add(r3, r3, r0);

  __ bind(deferred->exit());

  // Allocate temp stack space to for double
  __ addi(sp, sp, Operand(-8));

  // 0x41300000 is the top half of 1.0 x 2^20 as a double.
  __ lis(r4, Operand(0x4130));

  // Move 0x41300000xxxxxxxx (x = random bits) to VFP.
#if __FLOAT_WORD_ORDER == __LITTLE_ENDIAN
  __ stw(r3, MemOperand(sp, 0));
  __ stw(r4, MemOperand(sp, 4));
#else
  __ stw(r4, MemOperand(sp, 0));
  __ stw(r3, MemOperand(sp, 4));
#endif
  __ lfd(d7, MemOperand(sp, 0));

  // Move 0x4130000000000000 to VFP.
  __ li(r3, Operand::Zero());
#if __FLOAT_WORD_ORDER == __LITTLE_ENDIAN
  __ stw(r3, MemOperand(sp, 0));
  __ stw(r4, MemOperand(sp, 4));
#else
  __ stw(r4, MemOperand(sp, 0));
  __ stw(r3, MemOperand(sp, 4));
#endif
  __ lfd(d8, MemOperand(sp, 0));

  __ addi(sp, sp, Operand(8));

  // Subtract and store the result in the heap number.
  __ fsub(d7, d7, d8);
}


void LCodeGen::DoDeferredRandom(LRandom* instr) {
  __ PrepareCallCFunction(1, scratch0());
  __ CallCFunction(ExternalReference::random_uint32_function(isolate()), 1);
  // Return value is in r3.
}


void LCodeGen::DoMathLog(LUnaryMathOperation* instr) {
  ASSERT(ToDoubleRegister(instr->result()).is(d2));
  TranscendentalCacheStub stub(TranscendentalCache::LOG,
                               TranscendentalCacheStub::UNTAGGED);
  CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
}


void LCodeGen::DoMathTan(LUnaryMathOperation* instr) {
  ASSERT(ToDoubleRegister(instr->result()).is(d2));
  TranscendentalCacheStub stub(TranscendentalCache::TAN,
                               TranscendentalCacheStub::UNTAGGED);
  CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
}


void LCodeGen::DoMathCos(LUnaryMathOperation* instr) {
  ASSERT(ToDoubleRegister(instr->result()).is(d2));
  TranscendentalCacheStub stub(TranscendentalCache::COS,
                               TranscendentalCacheStub::UNTAGGED);
  CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
}


void LCodeGen::DoMathSin(LUnaryMathOperation* instr) {
  ASSERT(ToDoubleRegister(instr->result()).is(d2));
  TranscendentalCacheStub stub(TranscendentalCache::SIN,
                               TranscendentalCacheStub::UNTAGGED);
  CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
}


void LCodeGen::DoUnaryMathOperation(LUnaryMathOperation* instr) {
  switch (instr->op()) {
    case kMathAbs:
      DoMathAbs(instr);
      break;
    case kMathFloor:
      DoMathFloor(instr);
      break;
    case kMathRound:
      DoMathRound(instr);
      break;
    case kMathSqrt:
      DoMathSqrt(instr);
      break;
    case kMathPowHalf:
      DoMathPowHalf(instr);
      break;
    case kMathCos:
      DoMathCos(instr);
      break;
    case kMathSin:
      DoMathSin(instr);
      break;
    case kMathTan:
      DoMathTan(instr);
      break;
    case kMathLog:
      DoMathLog(instr);
      break;
    default:
      Abort("Unimplemented type of LUnaryMathOperation.");
      UNREACHABLE();
  }
}


void LCodeGen::DoInvokeFunction(LInvokeFunction* instr) {
  ASSERT(ToRegister(instr->function()).is(r4));
  ASSERT(instr->HasPointerMap());

  if (instr->known_function().is_null()) {
    LPointerMap* pointers = instr->pointer_map();
    RecordPosition(pointers->position());
    SafepointGenerator generator(this, pointers, Safepoint::kLazyDeopt);
    ParameterCount count(instr->arity());
    __ InvokeFunction(r4, count, CALL_FUNCTION, generator, CALL_AS_METHOD);
    __ lwz(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
  } else {
    CallKnownFunction(instr->known_function(),
                      instr->arity(),
                      instr,
                      CALL_AS_METHOD,
                      R4_CONTAINS_TARGET);
  }
}


void LCodeGen::DoCallKeyed(LCallKeyed* instr) {
  ASSERT(ToRegister(instr->result()).is(r3));

  int arity = instr->arity();
  Handle<Code> ic =
      isolate()->stub_cache()->ComputeKeyedCallInitialize(arity);
  CallCode(ic, RelocInfo::CODE_TARGET, instr);
  __ lwz(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
}


void LCodeGen::DoCallNamed(LCallNamed* instr) {
  ASSERT(ToRegister(instr->result()).is(r3));

  int arity = instr->arity();
  RelocInfo::Mode mode = RelocInfo::CODE_TARGET;
  Handle<Code> ic =
      isolate()->stub_cache()->ComputeCallInitialize(arity, mode);
  __ mov(r5, Operand(instr->name()));
  CallCode(ic, mode, instr);
  // Restore context register.
  __ lwz(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
}


void LCodeGen::DoCallFunction(LCallFunction* instr) {
  ASSERT(ToRegister(instr->function()).is(r4));
  ASSERT(ToRegister(instr->result()).is(r3));

  int arity = instr->arity();
  CallFunctionStub stub(arity, NO_CALL_FUNCTION_FLAGS);
  CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
  __ lwz(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
}


void LCodeGen::DoCallGlobal(LCallGlobal* instr) {
  ASSERT(ToRegister(instr->result()).is(r3));

  int arity = instr->arity();
  RelocInfo::Mode mode = RelocInfo::CODE_TARGET_CONTEXT;
  Handle<Code> ic =
      isolate()->stub_cache()->ComputeCallInitialize(arity, mode);
  __ mov(r5, Operand(instr->name()));
  CallCode(ic, mode, instr);
  __ lwz(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
}


void LCodeGen::DoCallKnownGlobal(LCallKnownGlobal* instr) {
  ASSERT(ToRegister(instr->result()).is(r3));
  CallKnownFunction(instr->target(),
                    instr->arity(),
                    instr,
                    CALL_AS_FUNCTION,
                    R4_UNINITIALIZED);
}


void LCodeGen::DoCallNew(LCallNew* instr) {
  ASSERT(ToRegister(instr->constructor()).is(r4));
  ASSERT(ToRegister(instr->result()).is(r3));

  CallConstructStub stub(NO_CALL_FUNCTION_FLAGS);
  __ mov(r3, Operand(instr->arity()));
  CallCode(stub.GetCode(), RelocInfo::CONSTRUCT_CALL, instr);
}


void LCodeGen::DoCallRuntime(LCallRuntime* instr) {
  CallRuntime(instr->function(), instr->arity(), instr);
}


void LCodeGen::DoStoreNamedField(LStoreNamedField* instr) {
  Register object = ToRegister(instr->object());
  Register value = ToRegister(instr->value());
  Register scratch = scratch0();
  int offset = instr->offset();

  ASSERT(!object.is(value));

  if (!instr->transition().is_null()) {
    __ mov(scratch, Operand(instr->transition()));
    __ stw(scratch, FieldMemOperand(object, HeapObject::kMapOffset));
    if (instr->hydrogen()->NeedsWriteBarrierForMap()) {
      Register temp = ToRegister(instr->temp());
      // Update the write barrier for the map field.
      __ RecordWriteField(object,
                          HeapObject::kMapOffset,
                          scratch,
                          temp,
                          kLRHasBeenSaved,
                          kSaveFPRegs,
                          OMIT_REMEMBERED_SET,
                          OMIT_SMI_CHECK);
    }
  }

  // Do the store.
  HType type = instr->hydrogen()->value()->type();
  SmiCheck check_needed =
      type.IsHeapObject() ? OMIT_SMI_CHECK : INLINE_SMI_CHECK;
  if (instr->is_in_object()) {
    __ stw(value, FieldMemOperand(object, offset));
    if (instr->hydrogen()->NeedsWriteBarrier()) {
      // Update the write barrier for the object for in-object properties.
      __ RecordWriteField(object,
                          offset,
                          value,
                          scratch,
                          kLRHasBeenSaved,
                          kSaveFPRegs,
                          EMIT_REMEMBERED_SET,
                          check_needed);
    }
  } else {
    __ lwz(scratch, FieldMemOperand(object, JSObject::kPropertiesOffset));
    __ stw(value, FieldMemOperand(scratch, offset));
    if (instr->hydrogen()->NeedsWriteBarrier()) {
      // Update the write barrier for the properties array.
      // object is used as a scratch register.
      __ RecordWriteField(scratch,
                          offset,
                          value,
                          object,
                          kLRHasBeenSaved,
                          kSaveFPRegs,
                          EMIT_REMEMBERED_SET,
                          check_needed);
    }
  }
}


void LCodeGen::DoStoreNamedGeneric(LStoreNamedGeneric* instr) {
  ASSERT(ToRegister(instr->object()).is(r4));
  ASSERT(ToRegister(instr->value()).is(r3));

  // Name is always in r5.
  __ mov(r5, Operand(instr->name()));
  Handle<Code> ic = (instr->strict_mode_flag() == kStrictMode)
      ? isolate()->builtins()->StoreIC_Initialize_Strict()
      : isolate()->builtins()->StoreIC_Initialize();
  CallCode(ic, RelocInfo::CODE_TARGET, instr);
}


void LCodeGen::DeoptIfTaggedButNotSmi(LEnvironment* environment,
                                      HValue* value,
                                      LOperand* operand) {
  if (value->representation().IsTagged() && !value->type().IsSmi()) {
    if (operand->IsRegister()) {
      __ TestIfSmi(ToRegister(operand), r0);
    } else {
      __ mov(ip, ToOperand(operand));
      __ TestIfSmi(ip, r0);
    }
    DeoptimizeIf(ne, environment, cr0);
  }
}


void LCodeGen::DoBoundsCheck(LBoundsCheck* instr) {
  DeoptIfTaggedButNotSmi(instr->environment(),
                         instr->hydrogen()->length(),
                         instr->length());
  DeoptIfTaggedButNotSmi(instr->environment(),
                         instr->hydrogen()->index(),
                         instr->index());
  if (instr->index()->IsConstantOperand()) {
    int constant_index =
        ToInteger32(LConstantOperand::cast(instr->index()));
    if (instr->hydrogen()->length()->representation().IsTagged()) {
      __ mov(ip, Operand(Smi::FromInt(constant_index)));
    } else {
      __ mov(ip, Operand(constant_index));
    }
    __ cmpl(ip, ToRegister(instr->length()));
  } else {
    __ cmpl(ToRegister(instr->index()), ToRegister(instr->length()));
  }
  DeoptimizeIf(ge, instr->environment());
}


void LCodeGen::DoStoreKeyedFastElement(LStoreKeyedFastElement* instr) {
  Register value = ToRegister(instr->value());
  Register elements = ToRegister(instr->object());
  Register key = instr->key()->IsRegister() ? ToRegister(instr->key()) : no_reg;
  Register scratch = scratch0();
  Register store_base = scratch;
  int offset = 0;

  // Do the store.
  if (instr->key()->IsConstantOperand()) {
    ASSERT(!instr->hydrogen()->NeedsWriteBarrier());
    LConstantOperand* const_operand = LConstantOperand::cast(instr->key());
    offset = FixedArray::OffsetOfElementAt(ToInteger32(const_operand) +
                                           instr->additional_index());
    store_base = elements;
  } else {
    // Even though the HLoadKeyedFastElement instruction forces the input
    // representation for the key to be an integer, the input gets replaced
    // during bound check elimination with the index argument to the bounds
    // check, which can be tagged, so that case must be handled here, too.
    if (instr->hydrogen()->key()->representation().IsTagged()) {
      __ slwi(scratch, key, Operand(kPointerSizeLog2 - kSmiTagSize));
    } else {
      __ slwi(scratch, key, Operand(kPointerSizeLog2));
    }
    __ add(scratch, elements, scratch);
    offset = FixedArray::OffsetOfElementAt(instr->additional_index());
  }
  __ stw(value, FieldMemOperand(store_base, offset));

  if (instr->hydrogen()->NeedsWriteBarrier()) {
    HType type = instr->hydrogen()->value()->type();
    SmiCheck check_needed =
        type.IsHeapObject() ? OMIT_SMI_CHECK : INLINE_SMI_CHECK;
    // Compute address of modified element and store it into key register.
    __ Add(key, store_base, offset - kHeapObjectTag, r0);
    __ RecordWrite(elements,
                   key,
                   value,
                   kLRHasBeenSaved,
                   kSaveFPRegs,
                   EMIT_REMEMBERED_SET,
                   check_needed);
  }
}


void LCodeGen::DoStoreKeyedFastDoubleElement(
    LStoreKeyedFastDoubleElement* instr) {
  DwVfpRegister value = ToDoubleRegister(instr->value());
  Register elements = ToRegister(instr->elements());
  Register key = no_reg;
  Register scratch = scratch0();
  bool key_is_constant = instr->key()->IsConstantOperand();
  int constant_key = 0;
  Label no_canonicalization, done;

  // Calculate the effective address of the slot in the array to store the
  // double value.
  if (key_is_constant) {
    constant_key = ToInteger32(LConstantOperand::cast(instr->key()));
    if (constant_key & 0xF0000000) {
      Abort("array index constant value too big.");
    }
  } else {
    key = ToRegister(instr->key());
  }
  int element_size_shift = ElementsKindToShiftSize(FAST_DOUBLE_ELEMENTS);
  int shift_size = (instr->hydrogen()->key()->representation().IsTagged())
      ? (element_size_shift - kSmiTagSize) : element_size_shift;
  int dst_offset = instr->additional_index() << element_size_shift;
  if (key_is_constant) {
    __ Add(scratch, elements,
           (constant_key << element_size_shift) +
           FixedDoubleArray::kHeaderSize - kHeapObjectTag,
           r0);
  } else {
    __ slwi(scratch, key, Operand(shift_size));
    __ add(scratch, elements, scratch);
    __ addi(scratch, scratch,
            Operand(FixedDoubleArray::kHeaderSize - kHeapObjectTag));
  }

  if (instr->NeedsCanonicalization()) {
    // Check for NaN. All NaNs must be canonicalized.
    __ fcmpu(value, value);
    // Only load canonical NaN if the comparison above set unordered.
    __ bordered(&no_canonicalization);

    uint64_t nan_int64 = BitCast<uint64_t>(
        FixedDoubleArray::canonical_not_the_hole_nan_as_double());
    __ mov(r0, Operand(static_cast<uint32_t>(nan_int64)));
#if __FLOAT_WORD_ORDER == __LITTLE_ENDIAN
    __ stw(r0, MemOperand(scratch, dst_offset));
#else
    __ stw(r0, MemOperand(scratch, dst_offset + 4));
#endif
    __ mov(r0, Operand(static_cast<uint32_t>(nan_int64 >> 32)));
#if __FLOAT_WORD_ORDER == __LITTLE_ENDIAN
    __ stw(r0, MemOperand(scratch, dst_offset + 4));
#else
    __ stw(r0, MemOperand(scratch, dst_offset));
#endif
    __ b(&done);
  }

  __ bind(&no_canonicalization);
  __ stfd(value, MemOperand(scratch, dst_offset));
  __ bind(&done);
}


void LCodeGen::DoStoreKeyedSpecializedArrayElement(
    LStoreKeyedSpecializedArrayElement* instr) {
  Register external_pointer = ToRegister(instr->external_pointer());
  Register key = no_reg;
  ElementsKind elements_kind = instr->elements_kind();
  bool key_is_constant = instr->key()->IsConstantOperand();
  int constant_key = 0;
  if (key_is_constant) {
    constant_key = ToInteger32(LConstantOperand::cast(instr->key()));
    if (constant_key & 0xF0000000) {
      Abort("array index constant value too big.");
    }
  } else {
    key = ToRegister(instr->key());
  }
  int element_size_shift = ElementsKindToShiftSize(elements_kind);
  int shift_size = (instr->hydrogen()->key()->representation().IsTagged())
      ? (element_size_shift - kSmiTagSize) : element_size_shift;
  int additional_offset = instr->additional_index() << element_size_shift;

  if (elements_kind == EXTERNAL_FLOAT_ELEMENTS ||
      elements_kind == EXTERNAL_DOUBLE_ELEMENTS) {
    DwVfpRegister value(ToDoubleRegister(instr->value()));
    if (key_is_constant) {
      __ Add(scratch0(), external_pointer,
             constant_key << element_size_shift,
             r0);
    } else {
      __ slwi(r0, key, Operand(shift_size));
      __ add(scratch0(), external_pointer, r0);
    }
    if (elements_kind == EXTERNAL_FLOAT_ELEMENTS) {
      __ frsp(double_scratch0(), value);
      __ stfs(double_scratch0(), MemOperand(scratch0(), additional_offset));
    } else {  // i.e. elements_kind == EXTERNAL_DOUBLE_ELEMENTS
      __ stfd(value, MemOperand(scratch0(), additional_offset));
    }
  } else {
    Register value(ToRegister(instr->value()));
    MemOperand mem_operand = PrepareKeyedOperand(
        key, external_pointer, key_is_constant, constant_key,
        element_size_shift, shift_size,
        instr->additional_index(), additional_offset);
    switch (elements_kind) {
      case EXTERNAL_PIXEL_ELEMENTS:
      case EXTERNAL_BYTE_ELEMENTS:
      case EXTERNAL_UNSIGNED_BYTE_ELEMENTS:
        if (key_is_constant) {
          __ stb(value, mem_operand);
        } else {
          __ stbx(value, mem_operand);
        }
        break;
      case EXTERNAL_SHORT_ELEMENTS:
      case EXTERNAL_UNSIGNED_SHORT_ELEMENTS:
        if (key_is_constant) {
          __ sth(value, mem_operand);
        } else {
          __ sthx(value, mem_operand);
        }
        break;
      case EXTERNAL_INT_ELEMENTS:
      case EXTERNAL_UNSIGNED_INT_ELEMENTS:
        if (key_is_constant) {
          __ stw(value, mem_operand);
        } else {
          __ stwx(value, mem_operand);
        }
        break;
      case EXTERNAL_FLOAT_ELEMENTS:
      case EXTERNAL_DOUBLE_ELEMENTS:
      case FAST_DOUBLE_ELEMENTS:
      case FAST_ELEMENTS:
      case FAST_SMI_ELEMENTS:
      case FAST_HOLEY_DOUBLE_ELEMENTS:
      case FAST_HOLEY_ELEMENTS:
      case FAST_HOLEY_SMI_ELEMENTS:
      case DICTIONARY_ELEMENTS:
      case NON_STRICT_ARGUMENTS_ELEMENTS:
        UNREACHABLE();
        break;
    }
  }
}


void LCodeGen::DoStoreKeyedGeneric(LStoreKeyedGeneric* instr) {
  ASSERT(ToRegister(instr->object()).is(r5));
  ASSERT(ToRegister(instr->key()).is(r4));
  ASSERT(ToRegister(instr->value()).is(r3));

  Handle<Code> ic = (instr->strict_mode_flag() == kStrictMode)
      ? isolate()->builtins()->KeyedStoreIC_Initialize_Strict()
      : isolate()->builtins()->KeyedStoreIC_Initialize();
  CallCode(ic, RelocInfo::CODE_TARGET, instr);
}


void LCodeGen::DoTransitionElementsKind(LTransitionElementsKind* instr) {
  Register object_reg = ToRegister(instr->object());
  Register new_map_reg = ToRegister(instr->new_map_temp());
  Register scratch = scratch0();

  Handle<Map> from_map = instr->original_map();
  Handle<Map> to_map = instr->transitioned_map();
  ElementsKind from_kind = from_map->elements_kind();
  ElementsKind to_kind = to_map->elements_kind();

  Label not_applicable;
  __ lwz(scratch, FieldMemOperand(object_reg, HeapObject::kMapOffset));
  __ Cmpi(scratch, Operand(from_map), r0);
  __ bne(&not_applicable);
  __ mov(new_map_reg, Operand(to_map));

  if (IsSimpleMapChangeTransition(from_kind, to_kind)) {
    __ stw(new_map_reg, FieldMemOperand(object_reg, HeapObject::kMapOffset));
    // Write barrier.
    __ RecordWriteField(object_reg, HeapObject::kMapOffset, new_map_reg,
                        scratch, kLRHasBeenSaved, kDontSaveFPRegs);
  } else if (IsFastSmiElementsKind(from_kind) &&
             IsFastDoubleElementsKind(to_kind)) {
    Register fixed_object_reg = ToRegister(instr->temp());
    ASSERT(fixed_object_reg.is(r5));
    ASSERT(new_map_reg.is(r6));
    __ mr(fixed_object_reg, object_reg);
    CallCode(isolate()->builtins()->TransitionElementsSmiToDouble(),
             RelocInfo::CODE_TARGET, instr);
  } else if (IsFastDoubleElementsKind(from_kind) &&
             IsFastObjectElementsKind(to_kind)) {
    Register fixed_object_reg = ToRegister(instr->temp());
    ASSERT(fixed_object_reg.is(r5));
    ASSERT(new_map_reg.is(r6));
    __ mr(fixed_object_reg, object_reg);
    CallCode(isolate()->builtins()->TransitionElementsDoubleToObject(),
             RelocInfo::CODE_TARGET, instr);
  } else {
    UNREACHABLE();
  }
  __ bind(&not_applicable);
}


void LCodeGen::DoStringAdd(LStringAdd* instr) {
  __ push(ToRegister(instr->left()));
  __ push(ToRegister(instr->right()));
  StringAddStub stub(NO_STRING_CHECK_IN_STUB);
  CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
}


void LCodeGen::DoStringCharCodeAt(LStringCharCodeAt* instr) {
  class DeferredStringCharCodeAt: public LDeferredCode {
   public:
    DeferredStringCharCodeAt(LCodeGen* codegen, LStringCharCodeAt* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() { codegen()->DoDeferredStringCharCodeAt(instr_); }
    virtual LInstruction* instr() { return instr_; }
   private:
    LStringCharCodeAt* instr_;
  };

  DeferredStringCharCodeAt* deferred =
      new(zone()) DeferredStringCharCodeAt(this, instr);

  StringCharLoadGenerator::Generate(masm(),
                                    ToRegister(instr->string()),
                                    ToRegister(instr->index()),
                                    ToRegister(instr->result()),
                                    deferred->entry());
  __ bind(deferred->exit());
}


void LCodeGen::DoDeferredStringCharCodeAt(LStringCharCodeAt* instr) {
  Register string = ToRegister(instr->string());
  Register result = ToRegister(instr->result());
  Register scratch = scratch0();

  // TODO(3095996): Get rid of this. For now, we need to make the
  // result register contain a valid pointer because it is already
  // contained in the register pointer map.
  __ li(result, Operand::Zero());

  PushSafepointRegistersScope scope(this, Safepoint::kWithRegisters);
  __ push(string);
  // Push the index as a smi. This is safe because of the checks in
  // DoStringCharCodeAt above.
  if (instr->index()->IsConstantOperand()) {
    int const_index = ToInteger32(LConstantOperand::cast(instr->index()));
    __ mov(scratch, Operand(Smi::FromInt(const_index)));
    __ push(scratch);
  } else {
    Register index = ToRegister(instr->index());
    __ SmiTag(index);
    __ push(index);
  }
  CallRuntimeFromDeferred(Runtime::kStringCharCodeAt, 2, instr);
  __ AssertSmi(r3);
  __ SmiUntag(r3);
  __ StoreToSafepointRegisterSlot(r3, result);
}


void LCodeGen::DoStringCharFromCode(LStringCharFromCode* instr) {
  class DeferredStringCharFromCode: public LDeferredCode {
   public:
    DeferredStringCharFromCode(LCodeGen* codegen, LStringCharFromCode* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() { codegen()->DoDeferredStringCharFromCode(instr_); }
    virtual LInstruction* instr() { return instr_; }
   private:
    LStringCharFromCode* instr_;
  };

  DeferredStringCharFromCode* deferred =
      new(zone()) DeferredStringCharFromCode(this, instr);

  ASSERT(instr->hydrogen()->value()->representation().IsInteger32());
  Register char_code = ToRegister(instr->char_code());
  Register result = ToRegister(instr->result());
  ASSERT(!char_code.is(result));

  __ cmpli(char_code, Operand(String::kMaxAsciiCharCode));
  __ b(gt, deferred->entry());
  __ LoadRoot(result, Heap::kSingleCharacterStringCacheRootIndex);
  __ slwi(r0, char_code, Operand(kPointerSizeLog2));
  __ add(result, result, r0);
  __ lwz(result, FieldMemOperand(result, FixedArray::kHeaderSize));
  __ LoadRoot(ip, Heap::kUndefinedValueRootIndex);
  __ cmp(result, ip);
  __ beq(deferred->entry());
  __ bind(deferred->exit());
}


void LCodeGen::DoDeferredStringCharFromCode(LStringCharFromCode* instr) {
  Register char_code = ToRegister(instr->char_code());
  Register result = ToRegister(instr->result());

  // TODO(3095996): Get rid of this. For now, we need to make the
  // result register contain a valid pointer because it is already
  // contained in the register pointer map.
  __ li(result, Operand::Zero());

  PushSafepointRegistersScope scope(this, Safepoint::kWithRegisters);
  __ SmiTag(char_code);
  __ push(char_code);
  CallRuntimeFromDeferred(Runtime::kCharFromCode, 1, instr);
  __ StoreToSafepointRegisterSlot(r3, result);
}


void LCodeGen::DoStringLength(LStringLength* instr) {
  Register string = ToRegister(instr->string());
  Register result = ToRegister(instr->result());
  __ lwz(result, FieldMemOperand(string, String::kLengthOffset));
}


void LCodeGen::DoInteger32ToDouble(LInteger32ToDouble* instr) {
  LOperand* input = instr->value();
  ASSERT(input->IsRegister() || input->IsStackSlot());
  LOperand* output = instr->result();
  ASSERT(output->IsDoubleRegister());
  if (input->IsStackSlot()) {
    Register scratch = scratch0();
    __ lwz(scratch, ToMemOperand(input));
    FloatingPointHelper::ConvertIntToDouble(masm(), scratch,
       ToDoubleRegister(output));
  } else {
    FloatingPointHelper::ConvertIntToDouble(masm(), ToRegister(input),
       ToDoubleRegister(output));
  }
}


void LCodeGen::DoUint32ToDouble(LUint32ToDouble* instr) {
  LOperand* input = instr->value();
  LOperand* output = instr->result();
  FloatingPointHelper::ConvertUnsignedIntToDouble(masm(), ToRegister(input),
       ToDoubleRegister(output));
}


void LCodeGen::DoNumberTagI(LNumberTagI* instr) {
  class DeferredNumberTagI: public LDeferredCode {
   public:
    DeferredNumberTagI(LCodeGen* codegen, LNumberTagI* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() {
      codegen()->DoDeferredNumberTagI(instr_,
                                      instr_->value(),
                                      SIGNED_INT32);
    }
    virtual LInstruction* instr() { return instr_; }
   private:
    LNumberTagI* instr_;
  };

  Register src = ToRegister(instr->value());
  Register dst = ToRegister(instr->result());

  DeferredNumberTagI* deferred = new(zone()) DeferredNumberTagI(this, instr);
  __ SmiTagCheckOverflow(dst, src, r0);
  __ BranchOnOverflow(deferred->entry());
  __ bind(deferred->exit());
}


void LCodeGen::DoNumberTagU(LNumberTagU* instr) {
  class DeferredNumberTagU: public LDeferredCode {
   public:
    DeferredNumberTagU(LCodeGen* codegen, LNumberTagU* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() {
      codegen()->DoDeferredNumberTagI(instr_,
                                      instr_->value(),
                                      UNSIGNED_INT32);
    }
    virtual LInstruction* instr() { return instr_; }
   private:
    LNumberTagU* instr_;
  };

  LOperand* input = instr->value();
  ASSERT(input->IsRegister() && input->Equals(instr->result()));
  Register reg = ToRegister(input);

  DeferredNumberTagU* deferred = new(zone()) DeferredNumberTagU(this, instr);
  __ Cmpli(reg, Operand(Smi::kMaxValue), r0);
  __ b(gt, deferred->entry());
  __ SmiTag(reg, reg);
  __ bind(deferred->exit());
}


void LCodeGen::DoDeferredNumberTagI(LInstruction* instr,
                                    LOperand* value,
                                    IntegerSignedness signedness) {
  Label slow;
  Register src = ToRegister(value);
  Register dst = ToRegister(instr->result());
  DoubleRegister dbl_scratch = double_scratch0();

  // Preserve the value of all registers.
  PushSafepointRegistersScope scope(this, Safepoint::kWithRegisters);

  Label done;
  if (signedness == SIGNED_INT32) {
    // There was overflow, so bits 30 and 31 of the original integer
    // disagree. Try to allocate a heap number in new space and store
    // the value in there. If that fails, call the runtime system.
    if (dst.is(src)) {
      __ SmiUntag(src, dst);
      __ xoris(src, src, Operand(HeapNumber::kSignMask >> 16));
    }
    FloatingPointHelper::ConvertIntToDouble(masm(), src, dbl_scratch);
  } else {
    FloatingPointHelper::ConvertUnsignedIntToDouble(masm(), src, dbl_scratch);
  }

  if (FLAG_inline_new) {
    __ LoadRoot(r9, Heap::kHeapNumberMapRootIndex);
    __ AllocateHeapNumber(r8, r6, r7, r9, &slow);
    __ Move(dst, r8);
    __ b(&done);
  }

  // Slow case: Call the runtime system to do the number allocation.
  __ bind(&slow);

  // TODO(3095996): Put a valid pointer value in the stack slot where the result
  // register is stored, as this register is in the pointer map, but contains an
  // integer value.
  __ li(ip, Operand::Zero());
  __ StoreToSafepointRegisterSlot(ip, dst);
  CallRuntimeFromDeferred(Runtime::kAllocateHeapNumber, 0, instr);
  __ Move(dst, r3);

  // Done. Put the value in dbl_scratch into the value of the allocated heap
  // number.
  __ bind(&done);
  __ stfd(dbl_scratch, FieldMemOperand(dst, HeapNumber::kValueOffset));
  __ StoreToSafepointRegisterSlot(dst, dst);
}


void LCodeGen::DoNumberTagD(LNumberTagD* instr) {
  class DeferredNumberTagD: public LDeferredCode {
   public:
    DeferredNumberTagD(LCodeGen* codegen, LNumberTagD* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() { codegen()->DoDeferredNumberTagD(instr_); }
    virtual LInstruction* instr() { return instr_; }
   private:
    LNumberTagD* instr_;
  };

  DoubleRegister input_reg = ToDoubleRegister(instr->value());
  Register scratch = scratch0();
  Register reg = ToRegister(instr->result());
  Register temp1 = ToRegister(instr->temp());
  Register temp2 = ToRegister(instr->temp2());

  DeferredNumberTagD* deferred = new(zone()) DeferredNumberTagD(this, instr);
  if (FLAG_inline_new) {
    __ LoadRoot(scratch, Heap::kHeapNumberMapRootIndex);
    __ AllocateHeapNumber(reg, temp1, temp2, scratch, deferred->entry());
  } else {
    __ b(deferred->entry());
  }
  __ bind(deferred->exit());
  __ stfd(input_reg, FieldMemOperand(reg, HeapNumber::kValueOffset));
}


void LCodeGen::DoDeferredNumberTagD(LNumberTagD* instr) {
  // TODO(3095996): Get rid of this. For now, we need to make the
  // result register contain a valid pointer because it is already
  // contained in the register pointer map.
  Register reg = ToRegister(instr->result());
  __ li(reg, Operand::Zero());

  PushSafepointRegistersScope scope(this, Safepoint::kWithRegisters);
  CallRuntimeFromDeferred(Runtime::kAllocateHeapNumber, 0, instr);
  __ StoreToSafepointRegisterSlot(r3, reg);
}


void LCodeGen::DoSmiTag(LSmiTag* instr) {
  ASSERT(!instr->hydrogen_value()->CheckFlag(HValue::kCanOverflow));
  __ SmiTag(ToRegister(instr->result()), ToRegister(instr->value()));
}


void LCodeGen::DoSmiUntag(LSmiUntag* instr) {
  Register scratch = scratch0();
  Register input = ToRegister(instr->value());
  Register result = ToRegister(instr->result());
  if (instr->needs_check()) {
    STATIC_ASSERT(kHeapObjectTag == 1);
    // If the input is a HeapObject, value of scratch won't be zero.
    __ andi(scratch, input, Operand(kHeapObjectTag));
    __ SmiUntag(result, input);
    DeoptimizeIf(ne, instr->environment(), cr0);
  } else {
    __ SmiUntag(result, input);
  }
}


void LCodeGen::EmitNumberUntagD(Register input_reg,
                                DoubleRegister result_reg,
                                bool deoptimize_on_undefined,
                                bool deoptimize_on_minus_zero,
                                LEnvironment* env) {
  Register scratch = scratch0();
  ASSERT(!result_reg.is(double_scratch0()));

  Label load_smi, heap_number, done;

  // Smi check.
  __ UntagAndJumpIfSmi(scratch, input_reg, &load_smi);

  // Heap number map check.
  __ lwz(scratch, FieldMemOperand(input_reg, HeapObject::kMapOffset));
  __ LoadRoot(ip, Heap::kHeapNumberMapRootIndex);
  __ cmp(scratch, ip);
  if (deoptimize_on_undefined) {
    DeoptimizeIf(ne, env);
  } else {
    Label heap_number;
    __ beq(&heap_number);

    __ LoadRoot(ip, Heap::kUndefinedValueRootIndex);
    __ cmp(input_reg, ip);
    DeoptimizeIf(ne, env);

    // Convert undefined to NaN.
    __ LoadRoot(ip, Heap::kNanValueRootIndex);
    __ lfd(result_reg, FieldMemOperand(ip, HeapNumber::kValueOffset));
    __ b(&done);

    __ bind(&heap_number);
  }
  // Heap number to double register conversion.
  __ lfd(result_reg, FieldMemOperand(input_reg, HeapNumber::kValueOffset));
  if (deoptimize_on_minus_zero) {
    __ sub(sp, sp, Operand(8));
    __ stfd(result_reg, MemOperand(sp));
#if __FLOAT_WORD_ORDER == __LITTLE_ENDIAN
    __ lwz(ip, MemOperand(sp, 0));
    __ lwz(scratch, MemOperand(sp, 4));
#else
    __ lwz(ip, MemOperand(sp, 4));
    __ lwz(scratch, MemOperand(sp, 0));
#endif
    __ addi(sp, sp, Operand(8));

    __ cmpi(ip, Operand::Zero());
    __ bne(&done);
    __ Cmpi(scratch, Operand(HeapNumber::kSignMask), r0);
    DeoptimizeIf(eq, env);
  }
  __ b(&done);

  // Smi to double register conversion
  __ bind(&load_smi);
  // scratch: untagged value of input_reg
  FloatingPointHelper::ConvertIntToDouble(masm(), scratch, result_reg);
  __ bind(&done);
}


void LCodeGen::DoDeferredTaggedToI(LTaggedToI* instr) {
  Register input_reg = ToRegister(instr->value());
  Register scratch1 = scratch0();
  Register scratch2 = ToRegister(instr->temp());
  DwVfpRegister double_scratch = double_scratch0();
  DwVfpRegister double_scratch2 = ToDoubleRegister(instr->temp3());

  ASSERT(!scratch1.is(input_reg) && !scratch1.is(scratch2));
  ASSERT(!scratch2.is(input_reg) && !scratch2.is(scratch1));

  Label done;

  // Heap number map check.
  __ lwz(scratch1, FieldMemOperand(input_reg, HeapObject::kMapOffset));
  __ LoadRoot(ip, Heap::kHeapNumberMapRootIndex);
  __ cmp(scratch1, ip);

  if (instr->truncating()) {
    Register scratch3 = ToRegister(instr->temp2());
    ASSERT(!scratch3.is(input_reg) &&
           !scratch3.is(scratch1) &&
           !scratch3.is(scratch2));
    // Performs a truncating conversion of a floating point number as used by
    // the JS bitwise operations.
    Label heap_number;
    __ beq(&heap_number);
    // Check for undefined. Undefined is converted to zero for truncating
    // conversions.
    __ LoadRoot(ip, Heap::kUndefinedValueRootIndex);
    __ cmp(input_reg, ip);
    DeoptimizeIf(ne, instr->environment());
    __ li(input_reg, Operand::Zero());
    __ b(&done);

    __ bind(&heap_number);
    __ lfd(double_scratch2,
           FieldMemOperand(input_reg, HeapNumber::kValueOffset));

    __ EmitECMATruncate(input_reg,
                        double_scratch2,
                        double_scratch,
                        scratch1,
                        scratch2,
                        scratch3);

  } else {
    // Deoptimize if we don't have a heap number.
    DeoptimizeIf(ne, instr->environment());

    __ lfd(double_scratch,
           FieldMemOperand(input_reg, HeapNumber::kValueOffset));
    __ EmitVFPTruncate(kRoundToZero,
                       input_reg,
                       double_scratch,
                       scratch1,
                       double_scratch2,
                       kCheckForInexactConversion);
    DeoptimizeIf(ne, instr->environment());

    if (instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero)) {
      __ cmpi(input_reg, Operand::Zero());
      __ bne(&done);
#ifdef PENGUIN_CLEANUP
      __ vmov(scratch1, double_scratch.high());
#else
      PPCPORT_UNIMPLEMENTED();
      __ fake_asm(fLITHIUM91);
#endif
      __ TestBit(scratch1, 0, r0);  // test sign bit
      DeoptimizeIf(ne, instr->environment(), cr0);
    }
  }
  __ bind(&done);
}


void LCodeGen::DoTaggedToI(LTaggedToI* instr) {
  class DeferredTaggedToI: public LDeferredCode {
   public:
    DeferredTaggedToI(LCodeGen* codegen, LTaggedToI* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() { codegen()->DoDeferredTaggedToI(instr_); }
    virtual LInstruction* instr() { return instr_; }
   private:
    LTaggedToI* instr_;
  };

  LOperand* input = instr->value();
  ASSERT(input->IsRegister());
  ASSERT(input->Equals(instr->result()));

  Register input_reg = ToRegister(input);

  DeferredTaggedToI* deferred = new(zone()) DeferredTaggedToI(this, instr);

  // Branch to deferred code if the input is a HeapObject.
  __ JumpIfNotSmi(input_reg, deferred->entry());

  __ SmiUntag(input_reg);
  __ bind(deferred->exit());
}


void LCodeGen::DoNumberUntagD(LNumberUntagD* instr) {
  LOperand* input = instr->value();
  ASSERT(input->IsRegister());
  LOperand* result = instr->result();
  ASSERT(result->IsDoubleRegister());

  Register input_reg = ToRegister(input);
  DoubleRegister result_reg = ToDoubleRegister(result);

  EmitNumberUntagD(input_reg, result_reg,
                   instr->hydrogen()->deoptimize_on_undefined(),
                   instr->hydrogen()->deoptimize_on_minus_zero(),
                   instr->environment());
}


void LCodeGen::DoDoubleToI(LDoubleToI* instr) {
  Register result_reg = ToRegister(instr->result());
  Register scratch1 = scratch0();
  Register scratch2 = ToRegister(instr->temp());
  DwVfpRegister double_input = ToDoubleRegister(instr->value());

  Label done;

  if (instr->truncating()) {
    Register scratch3 = ToRegister(instr->temp2());
    DwVfpRegister double_scratch = double_scratch0();
    __ EmitECMATruncate(result_reg,
                        double_input,
                        double_scratch,
                        scratch1,
                        scratch2,
                        scratch3);
  } else {
    DwVfpRegister double_scratch = double_scratch0();
    __ EmitVFPTruncate(kRoundToMinusInf,
                       result_reg,
                       double_input,
                       scratch1,
                       double_scratch,
                       kCheckForInexactConversion);

    // Deoptimize if we had a vfp invalid exception,
    // including inexact operation.
    DeoptimizeIf(ne, instr->environment());
  }
    __ bind(&done);
}


void LCodeGen::DoCheckSmi(LCheckSmi* instr) {
  LOperand* input = instr->value();
  __ TestIfSmi(ToRegister(input), r0);
  DeoptimizeIf(ne, instr->environment(), cr0);
}


void LCodeGen::DoCheckNonSmi(LCheckNonSmi* instr) {
  LOperand* input = instr->value();
  __ TestIfSmi(ToRegister(input), r0);
  DeoptimizeIf(eq, instr->environment(), cr0);
}


void LCodeGen::DoCheckInstanceType(LCheckInstanceType* instr) {
  Register input = ToRegister(instr->value());
  Register scratch = scratch0();

  __ lwz(scratch, FieldMemOperand(input, HeapObject::kMapOffset));
  __ lbz(scratch, FieldMemOperand(scratch, Map::kInstanceTypeOffset));

  if (instr->hydrogen()->is_interval_check()) {
    InstanceType first;
    InstanceType last;
    instr->hydrogen()->GetCheckInterval(&first, &last);

    __ cmpli(scratch, Operand(first));

    // If there is only one type in the interval check for equality.
    if (first == last) {
      DeoptimizeIf(ne, instr->environment());
    } else {
      DeoptimizeIf(lt, instr->environment());
      // Omit check for the last type.
      if (last != LAST_TYPE) {
        __ cmpli(scratch, Operand(last));
        DeoptimizeIf(gt, instr->environment());
      }
    }
  } else {
    uint8_t mask;
    uint8_t tag;
    instr->hydrogen()->GetCheckMaskAndTag(&mask, &tag);

    if (IsPowerOf2(mask)) {
      ASSERT(tag == 0 || IsPowerOf2(tag));
      __ andi(r0, scratch, Operand(mask));
      DeoptimizeIf(tag == 0 ? ne : eq, instr->environment(), cr0);
    } else {
      __ andi(scratch, scratch, Operand(mask));
      __ cmpi(scratch, Operand(tag));
      DeoptimizeIf(ne, instr->environment());
    }
  }
}


void LCodeGen::DoCheckFunction(LCheckFunction* instr) {
  Register reg = ToRegister(instr->value());
  Handle<JSFunction> target = instr->hydrogen()->target();
  if (isolate()->heap()->InNewSpace(*target)) {
    Register reg = ToRegister(instr->value());
    Handle<JSGlobalPropertyCell> cell =
        isolate()->factory()->NewJSGlobalPropertyCell(target);
    __ mov(ip, Operand(Handle<Object>(cell)));
    __ lwz(ip, FieldMemOperand(ip, JSGlobalPropertyCell::kValueOffset));
    __ cmp(reg, ip);
  } else {
    __ Cmpi(reg, Operand(target), r0);
  }
  DeoptimizeIf(ne, instr->environment());
}


void LCodeGen::DoCheckMapCommon(Register reg,
                                Register scratch,
                                Handle<Map> map,
                                CompareMapMode mode,
                                LEnvironment* env) {
  Label success;
  __ CompareMap(reg, scratch, map, &success, mode);
  DeoptimizeIf(ne, env);
  __ bind(&success);
}


void LCodeGen::DoCheckMaps(LCheckMaps* instr) {
  Register scratch = scratch0();
  LOperand* input = instr->value();
  ASSERT(input->IsRegister());
  Register reg = ToRegister(input);

  Label success;
  SmallMapList* map_set = instr->hydrogen()->map_set();
  for (int i = 0; i < map_set->length() - 1; i++) {
    Handle<Map> map = map_set->at(i);
    __ CompareMap(reg, scratch, map, &success, REQUIRE_EXACT_MAP);
    __ beq(&success);
  }
  Handle<Map> map = map_set->last();
  DoCheckMapCommon(reg, scratch, map, REQUIRE_EXACT_MAP, instr->environment());
  __ bind(&success);
}


void LCodeGen::DoClampDToUint8(LClampDToUint8* instr) {
  DoubleRegister value_reg = ToDoubleRegister(instr->unclamped());
  Register result_reg = ToRegister(instr->result());
  DoubleRegister temp_reg = ToDoubleRegister(instr->temp());
  __ ClampDoubleToUint8(result_reg, value_reg, temp_reg, double_scratch0());
}


void LCodeGen::DoClampIToUint8(LClampIToUint8* instr) {
  Register unclamped_reg = ToRegister(instr->unclamped());
  Register result_reg = ToRegister(instr->result());
  __ ClampUint8(result_reg, unclamped_reg);
}


void LCodeGen::DoClampTToUint8(LClampTToUint8* instr) {
  Register scratch = scratch0();
  Register input_reg = ToRegister(instr->unclamped());
  Register result_reg = ToRegister(instr->result());
  DoubleRegister temp_reg1 = ToDoubleRegister(instr->temp1());
  DoubleRegister temp_reg2 = ToDoubleRegister(instr->temp2());
  Label is_smi, done, heap_number;

  // Both smi and heap number cases are handled.
  __ UntagAndJumpIfSmi(result_reg, input_reg, &is_smi);

  // Check for heap number
  __ lwz(scratch, FieldMemOperand(input_reg, HeapObject::kMapOffset));
  __ Cmpi(scratch, Operand(factory()->heap_number_map()), r0);
  __ beq(&heap_number);

  // Check for undefined. Undefined is converted to zero for clamping
  // conversions.
  __ Cmpi(input_reg, Operand(factory()->undefined_value()), r0);
  DeoptimizeIf(ne, instr->environment());
  __ li(result_reg, Operand::Zero());
  __ b(&done);

  // Heap number
  __ bind(&heap_number);
  __ lfd(double_scratch0(), FieldMemOperand(input_reg,
                                            HeapNumber::kValueOffset));
  __ ClampDoubleToUint8(result_reg, double_scratch0(), temp_reg1, temp_reg2);
  __ b(&done);

  // smi
  __ bind(&is_smi);
  __ ClampUint8(result_reg, result_reg);

  __ bind(&done);
}


void LCodeGen::DoCheckPrototypeMaps(LCheckPrototypeMaps* instr) {
  Register temp1 = ToRegister(instr->temp());
  Register temp2 = ToRegister(instr->temp2());

  Handle<JSObject> holder = instr->holder();
  Handle<JSObject> current_prototype = instr->prototype();

  // Load prototype object.
  __ LoadHeapObject(temp1, current_prototype);

  // Check prototype maps up to the holder.
  while (!current_prototype.is_identical_to(holder)) {
    DoCheckMapCommon(temp1, temp2,
                     Handle<Map>(current_prototype->map()),
                     ALLOW_ELEMENT_TRANSITION_MAPS, instr->environment());
    current_prototype =
        Handle<JSObject>(JSObject::cast(current_prototype->GetPrototype()));
    // Load next prototype object.
    __ LoadHeapObject(temp1, current_prototype);
  }

  // Check the holder map.
  DoCheckMapCommon(temp1, temp2,
                   Handle<Map>(current_prototype->map()),
                   ALLOW_ELEMENT_TRANSITION_MAPS, instr->environment());
  DeoptimizeIf(ne, instr->environment());
}


void LCodeGen::DoAllocateObject(LAllocateObject* instr) {
  class DeferredAllocateObject: public LDeferredCode {
   public:
    DeferredAllocateObject(LCodeGen* codegen, LAllocateObject* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() { codegen()->DoDeferredAllocateObject(instr_); }
    virtual LInstruction* instr() { return instr_; }
   private:
    LAllocateObject* instr_;
  };

  DeferredAllocateObject* deferred =
      new(zone()) DeferredAllocateObject(this, instr);

  Register result = ToRegister(instr->result());
  Register scratch = ToRegister(instr->temp());
  Register scratch2 = ToRegister(instr->temp2());
  Handle<JSFunction> constructor = instr->hydrogen()->constructor();
  Handle<Map> initial_map(constructor->initial_map());
  int instance_size = initial_map->instance_size();
  ASSERT(initial_map->pre_allocated_property_fields() +
         initial_map->unused_property_fields() -
         initial_map->inobject_properties() == 0);

  // Allocate memory for the object.  The initial map might change when
  // the constructor's prototype changes, but instance size and property
  // counts remain unchanged (if slack tracking finished).
  ASSERT(!constructor->shared()->IsInobjectSlackTrackingInProgress());
  __ AllocateInNewSpace(instance_size,
                        result,
                        scratch,
                        scratch2,
                        deferred->entry(),
                        TAG_OBJECT);

  __ bind(deferred->exit());
  if (FLAG_debug_code) {
    Label is_in_new_space;
    __ JumpIfInNewSpace(result, scratch, &is_in_new_space);
    __ Abort("Allocated object is not in new-space");
    __ bind(&is_in_new_space);
  }

  // Load the initial map.
  Register map = scratch;
  __ LoadHeapObject(map, constructor);
  __ lwz(map, FieldMemOperand(map, JSFunction::kPrototypeOrInitialMapOffset));

  // Initialize map and fields of the newly allocated object.
  ASSERT(initial_map->instance_type() == JS_OBJECT_TYPE);
  __ stw(map, FieldMemOperand(result, JSObject::kMapOffset));
  __ LoadRoot(scratch, Heap::kEmptyFixedArrayRootIndex);
  __ stw(scratch, FieldMemOperand(result, JSObject::kElementsOffset));
  __ stw(scratch, FieldMemOperand(result, JSObject::kPropertiesOffset));
  if (initial_map->inobject_properties() != 0) {
    __ LoadRoot(scratch, Heap::kUndefinedValueRootIndex);
    for (int i = 0; i < initial_map->inobject_properties(); i++) {
      int property_offset = JSObject::kHeaderSize + i * kPointerSize;
      __ stw(scratch, FieldMemOperand(result, property_offset));
    }
  }
}


void LCodeGen::DoDeferredAllocateObject(LAllocateObject* instr) {
  Register result = ToRegister(instr->result());
  Handle<JSFunction> constructor = instr->hydrogen()->constructor();
  Handle<Map> initial_map(constructor->initial_map());
  int instance_size = initial_map->instance_size();

  // TODO(3095996): Get rid of this. For now, we need to make the
  // result register contain a valid pointer because it is already
  // contained in the register pointer map.
  __ li(result, Operand::Zero());

  PushSafepointRegistersScope scope(this, Safepoint::kWithRegisters);
  __ mov(r3, Operand(Smi::FromInt(instance_size)));
  __ push(r3);
  CallRuntimeFromDeferred(Runtime::kAllocateInNewSpace, 1, instr);
  __ StoreToSafepointRegisterSlot(r3, result);
}


void LCodeGen::DoArrayLiteral(LArrayLiteral* instr) {
  Handle<FixedArray> literals(instr->environment()->closure()->literals());
  ElementsKind boilerplate_elements_kind =
      instr->hydrogen()->boilerplate_elements_kind();

  // Deopt if the array literal boilerplate ElementsKind is of a type different
  // than the expected one. The check isn't necessary if the boilerplate has
  // already been converted to TERMINAL_FAST_ELEMENTS_KIND.
  if (CanTransitionToMoreGeneralFastElementsKind(
          boilerplate_elements_kind, true)) {
    __ LoadHeapObject(r4, instr->hydrogen()->boilerplate_object());
    // Load map into r5.
    __ lwz(r5, FieldMemOperand(r4, HeapObject::kMapOffset));
    // Load the map's "bit field 2".
    __ lbz(r5, FieldMemOperand(r5, Map::kBitField2Offset));
    // Retrieve elements_kind from bit field 2.
    __ ExtractBitMask(r5, r5, Map::kElementsKindMask);
    __ Cmpi(r5, Operand(boilerplate_elements_kind), r0);
    DeoptimizeIf(ne, instr->environment());
  }

  // Set up the parameters to the stub/runtime call.
  __ LoadHeapObject(r6, literals);
  __ mov(r5, Operand(Smi::FromInt(instr->hydrogen()->literal_index())));
  // Boilerplate already exists, constant elements are never accessed.
  // Pass an empty fixed array.
  __ mov(r4, Operand(isolate()->factory()->empty_fixed_array()));
  __ Push(r6, r5, r4);

  // Pick the right runtime function or stub to call.
  int length = instr->hydrogen()->length();
  if (instr->hydrogen()->IsCopyOnWrite()) {
    ASSERT(instr->hydrogen()->depth() == 1);
    FastCloneShallowArrayStub::Mode mode =
        FastCloneShallowArrayStub::COPY_ON_WRITE_ELEMENTS;
    FastCloneShallowArrayStub stub(mode, length);
    CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
  } else if (instr->hydrogen()->depth() > 1) {
    CallRuntime(Runtime::kCreateArrayLiteral, 3, instr);
  } else if (length > FastCloneShallowArrayStub::kMaximumClonedLength) {
    CallRuntime(Runtime::kCreateArrayLiteralShallow, 3, instr);
  } else {
    FastCloneShallowArrayStub::Mode mode =
        boilerplate_elements_kind == FAST_DOUBLE_ELEMENTS
            ? FastCloneShallowArrayStub::CLONE_DOUBLE_ELEMENTS
            : FastCloneShallowArrayStub::CLONE_ELEMENTS;
    FastCloneShallowArrayStub stub(mode, length);
    CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
  }
}


void LCodeGen::EmitDeepCopy(Handle<JSObject> object,
                            Register result,
                            Register source,
                            int* offset) {
  ASSERT(!source.is(r5));
  ASSERT(!result.is(r5));

  // Only elements backing stores for non-COW arrays need to be copied.
  Handle<FixedArrayBase> elements(object->elements());
  bool has_elements = elements->length() > 0 &&
      elements->map() != isolate()->heap()->fixed_cow_array_map();

  // Increase the offset so that subsequent objects end up right after
  // this object and its backing store.
  int object_offset = *offset;
  int object_size = object->map()->instance_size();
  int elements_offset = *offset + object_size;
  int elements_size = has_elements ? elements->Size() : 0;
  *offset += object_size + elements_size;

  // Copy object header.
  ASSERT(object->properties()->length() == 0);
  int inobject_properties = object->map()->inobject_properties();
  int header_size = object_size - inobject_properties * kPointerSize;
  for (int i = 0; i < header_size; i += kPointerSize) {
    if (has_elements && i == JSObject::kElementsOffset) {
      __ Add(r5, result, elements_offset, r0);
    } else {
      __ lwz(r5, FieldMemOperand(source, i));
    }
    __ stw(r5, FieldMemOperand(result, object_offset + i));
  }

  // Copy in-object properties.
  for (int i = 0; i < inobject_properties; i++) {
    int total_offset = object_offset + object->GetInObjectPropertyOffset(i);
    Handle<Object> value = Handle<Object>(object->InObjectPropertyAt(i));
    if (value->IsJSObject()) {
      Handle<JSObject> value_object = Handle<JSObject>::cast(value);
      __ Add(r5, result, *offset, r0);
      __ stw(r5, FieldMemOperand(result, total_offset));
      __ LoadHeapObject(source, value_object);
      EmitDeepCopy(value_object, result, source, offset);
    } else if (value->IsHeapObject()) {
      __ LoadHeapObject(r5, Handle<HeapObject>::cast(value));
      __ stw(r5, FieldMemOperand(result, total_offset));
    } else {
      __ mov(r5, Operand(value));
      __ stw(r5, FieldMemOperand(result, total_offset));
    }
  }

  if (has_elements) {
    // Copy elements backing store header.
    __ LoadHeapObject(source, elements);
    for (int i = 0; i < FixedArray::kHeaderSize; i += kPointerSize) {
      __ lwz(r5, FieldMemOperand(source, i));
      __ stw(r5, FieldMemOperand(result, elements_offset + i));
    }

    // Copy elements backing store content.
    int elements_length = has_elements ? elements->length() : 0;
    if (elements->IsFixedDoubleArray()) {
      Handle<FixedDoubleArray> double_array =
          Handle<FixedDoubleArray>::cast(elements);
      for (int i = 0; i < elements_length; i++) {
        int64_t value = double_array->get_representation(i);
        // We only support little endian mode...
        int32_t value_low = static_cast<int32_t>(value & 0xFFFFFFFF);
        int32_t value_high = static_cast<int32_t>(value >> 32);
        int total_offset =
            elements_offset + FixedDoubleArray::OffsetOfElementAt(i);
#if __FLOAT_WORD_ORDER == __LITTLE_ENDIAN
        __ mov(r5, Operand(value_low));
        __ stw(r5, FieldMemOperand(result, total_offset));
        __ mov(r5, Operand(value_high));
        __ stw(r5, FieldMemOperand(result, total_offset + 4));
#else
        __ mov(r5, Operand(value_high));
        __ stw(r5, FieldMemOperand(result, total_offset));
        __ mov(r5, Operand(value_low));
        __ stw(r5, FieldMemOperand(result, total_offset + 4));
#endif
      }
    } else if (elements->IsFixedArray()) {
      Handle<FixedArray> fast_elements = Handle<FixedArray>::cast(elements);
      for (int i = 0; i < elements_length; i++) {
        int total_offset = elements_offset + FixedArray::OffsetOfElementAt(i);
        Handle<Object> value(fast_elements->get(i));
        if (value->IsJSObject()) {
          Handle<JSObject> value_object = Handle<JSObject>::cast(value);
          __ Add(r5, result, *offset, r0);
          __ stw(r5, FieldMemOperand(result, total_offset));
          __ LoadHeapObject(source, value_object);
          EmitDeepCopy(value_object, result, source, offset);
        } else if (value->IsHeapObject()) {
          __ LoadHeapObject(r5, Handle<HeapObject>::cast(value));
          __ stw(r5, FieldMemOperand(result, total_offset));
        } else {
          __ mov(r5, Operand(value));
          __ stw(r5, FieldMemOperand(result, total_offset));
        }
      }
    } else {
      UNREACHABLE();
    }
  }
}


void LCodeGen::DoFastLiteral(LFastLiteral* instr) {
  int size = instr->hydrogen()->total_size();
  ElementsKind boilerplate_elements_kind =
      instr->hydrogen()->boilerplate()->GetElementsKind();

  // Deopt if the array literal boilerplate ElementsKind is of a type different
  // than the expected one. The check isn't necessary if the boilerplate has
  // already been converted to TERMINAL_FAST_ELEMENTS_KIND.
  if (CanTransitionToMoreGeneralFastElementsKind(
          boilerplate_elements_kind, true)) {
    __ LoadHeapObject(r4, instr->hydrogen()->boilerplate());
    // Load map into r5.
    __ lwz(r5, FieldMemOperand(r4, HeapObject::kMapOffset));
    // Load the map's "bit field 2".
    __ lbz(r5, FieldMemOperand(r5, Map::kBitField2Offset));
    // Retrieve elements_kind from bit field 2.
    __ ExtractBitMask(r5, r5, Map::kElementsKindMask);
    __ Cmpi(r5, Operand(boilerplate_elements_kind), r0);
    DeoptimizeIf(ne, instr->environment());
  }

  // Allocate all objects that are part of the literal in one big
  // allocation. This avoids multiple limit checks.
  Label allocated, runtime_allocate;
  __ AllocateInNewSpace(size, r3, r5, r6, &runtime_allocate, TAG_OBJECT);
  __ b(&allocated);

  __ bind(&runtime_allocate);
  __ mov(r3, Operand(Smi::FromInt(size)));
  __ push(r3);
  CallRuntime(Runtime::kAllocateInNewSpace, 1, instr);

  __ bind(&allocated);
  int offset = 0;
  __ LoadHeapObject(r4, instr->hydrogen()->boilerplate());
  EmitDeepCopy(instr->hydrogen()->boilerplate(), r3, r4, &offset);
  ASSERT_EQ(size, offset);
}


void LCodeGen::DoObjectLiteral(LObjectLiteral* instr) {
  Handle<FixedArray> literals(instr->environment()->closure()->literals());
  Handle<FixedArray> constant_properties =
      instr->hydrogen()->constant_properties();

  // Set up the parameters to the stub/runtime call.
  __ LoadHeapObject(r7, literals);
  __ mov(r6, Operand(Smi::FromInt(instr->hydrogen()->literal_index())));
  __ mov(r5, Operand(constant_properties));
  int flags = instr->hydrogen()->fast_elements()
      ? ObjectLiteral::kFastElements
      : ObjectLiteral::kNoFlags;
  __ mov(r4, Operand(Smi::FromInt(flags)));
  __ Push(r7, r6, r5, r4);

  // Pick the right runtime function or stub to call.
  int properties_count = constant_properties->length() / 2;
  if (instr->hydrogen()->depth() > 1) {
    CallRuntime(Runtime::kCreateObjectLiteral, 4, instr);
  } else if (flags != ObjectLiteral::kFastElements ||
      properties_count > FastCloneShallowObjectStub::kMaximumClonedProperties) {
    CallRuntime(Runtime::kCreateObjectLiteralShallow, 4, instr);
  } else {
    FastCloneShallowObjectStub stub(properties_count);
    CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
  }
}


void LCodeGen::DoToFastProperties(LToFastProperties* instr) {
  ASSERT(ToRegister(instr->value()).is(r3));
  __ push(r3);
  CallRuntime(Runtime::kToFastProperties, 1, instr);
}


void LCodeGen::DoRegExpLiteral(LRegExpLiteral* instr) {
  Label materialized;
  // Registers will be used as follows:
  // r10 = literals array.
  // r4 = regexp literal.
  // r3 = regexp literal clone.
  // r5 and r7-r9 are used as temporaries.
  int literal_offset =
      FixedArray::OffsetOfElementAt(instr->hydrogen()->literal_index());
  __ LoadHeapObject(r10, instr->hydrogen()->literals());
  __ lwz(r4, FieldMemOperand(r10, literal_offset));
  __ LoadRoot(ip, Heap::kUndefinedValueRootIndex);
  __ cmp(r4, ip);
  __ bne(&materialized);

  // Create regexp literal using runtime function
  // Result will be in r3.
  __ mov(r9, Operand(Smi::FromInt(instr->hydrogen()->literal_index())));
  __ mov(r8, Operand(instr->hydrogen()->pattern()));
  __ mov(r7, Operand(instr->hydrogen()->flags()));
  __ Push(r10, r9, r8, r7);
  CallRuntime(Runtime::kMaterializeRegExpLiteral, 4, instr);
  __ mr(r4, r3);

  __ bind(&materialized);
  int size = JSRegExp::kSize + JSRegExp::kInObjectFieldCount * kPointerSize;
  Label allocated, runtime_allocate;

  __ AllocateInNewSpace(size, r3, r5, r6, &runtime_allocate, TAG_OBJECT);
  __ b(&allocated);

  __ bind(&runtime_allocate);
  __ mov(r3, Operand(Smi::FromInt(size)));
  __ Push(r4, r3);
  CallRuntime(Runtime::kAllocateInNewSpace, 1, instr);
  __ pop(r4);

  __ bind(&allocated);
  // Copy the content into the newly allocated memory.
  // (Unroll copy loop once for better throughput).
  for (int i = 0; i < size - kPointerSize; i += 2 * kPointerSize) {
    __ lwz(r6, FieldMemOperand(r4, i));
    __ lwz(r5, FieldMemOperand(r4, i + kPointerSize));
    __ stw(r6, FieldMemOperand(r3, i));
    __ stw(r5, FieldMemOperand(r3, i + kPointerSize));
  }
  if ((size % (2 * kPointerSize)) != 0) {
    __ lwz(r6, FieldMemOperand(r4, size - kPointerSize));
    __ stw(r6, FieldMemOperand(r3, size - kPointerSize));
  }
}


void LCodeGen::DoFunctionLiteral(LFunctionLiteral* instr) {
  // Use the fast case closure allocation code that allocates in new
  // space for nested functions that don't need literals cloning.
  Handle<SharedFunctionInfo> shared_info = instr->shared_info();
  bool pretenure = instr->hydrogen()->pretenure();
  if (!pretenure && shared_info->num_literals() == 0) {
    FastNewClosureStub stub(shared_info->language_mode());
    __ mov(r4, Operand(shared_info));
    __ push(r4);
    CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
  } else {
    __ mov(r5, Operand(shared_info));
    __ mov(r4, Operand(pretenure
                       ? factory()->true_value()
                       : factory()->false_value()));
    __ Push(cp, r5, r4);
    CallRuntime(Runtime::kNewClosure, 3, instr);
  }
}


void LCodeGen::DoTypeof(LTypeof* instr) {
  Register input = ToRegister(instr->value());
  __ push(input);
  CallRuntime(Runtime::kTypeof, 1, instr);
}


void LCodeGen::DoTypeofIsAndBranch(LTypeofIsAndBranch* instr) {
  Register input = ToRegister(instr->value());
  int true_block = chunk_->LookupDestination(instr->true_block_id());
  int false_block = chunk_->LookupDestination(instr->false_block_id());
  Label* true_label = chunk_->GetAssemblyLabel(true_block);
  Label* false_label = chunk_->GetAssemblyLabel(false_block);

  Condition final_branch_condition = EmitTypeofIs(true_label,
                                                  false_label,
                                                  input,
                                                  instr->type_literal());
  if (final_branch_condition != kNoCondition) {
    EmitBranch(true_block, false_block, final_branch_condition);
  }
}


Condition LCodeGen::EmitTypeofIs(Label* true_label,
                                 Label* false_label,
                                 Register input,
                                 Handle<String> type_name) {
  Condition final_branch_condition = kNoCondition;
  Register scratch = scratch0();
  if (type_name->Equals(heap()->number_symbol())) {
    __ JumpIfSmi(input, true_label);
    __ lwz(input, FieldMemOperand(input, HeapObject::kMapOffset));
    __ LoadRoot(ip, Heap::kHeapNumberMapRootIndex);
    __ cmp(input, ip);
    final_branch_condition = eq;

  } else if (type_name->Equals(heap()->string_symbol())) {
    __ JumpIfSmi(input, false_label);
    __ CompareObjectType(input, input, scratch, FIRST_NONSTRING_TYPE);
    __ bge(false_label);
    __ lbz(ip, FieldMemOperand(input, Map::kBitFieldOffset));
    __ ExtractBit(r0, ip, 31 - Map::kIsUndetectable);
    __ cmpi(r0, Operand::Zero());
    final_branch_condition = eq;

  } else if (type_name->Equals(heap()->boolean_symbol())) {
    __ CompareRoot(input, Heap::kTrueValueRootIndex);
    __ beq(true_label);
    __ CompareRoot(input, Heap::kFalseValueRootIndex);
    final_branch_condition = eq;

  } else if (FLAG_harmony_typeof && type_name->Equals(heap()->null_symbol())) {
    __ CompareRoot(input, Heap::kNullValueRootIndex);
    final_branch_condition = eq;

  } else if (type_name->Equals(heap()->undefined_symbol())) {
    __ CompareRoot(input, Heap::kUndefinedValueRootIndex);
    __ beq(true_label);
    __ JumpIfSmi(input, false_label);
    // Check for undetectable objects => true.
    __ lwz(input, FieldMemOperand(input, HeapObject::kMapOffset));
    __ lbz(ip, FieldMemOperand(input, Map::kBitFieldOffset));
    __ ExtractBit(r0, ip, 31 - Map::kIsUndetectable);
    __ cmpi(r0, Operand::Zero());
    final_branch_condition = ne;

  } else if (type_name->Equals(heap()->function_symbol())) {
    STATIC_ASSERT(NUM_OF_CALLABLE_SPEC_OBJECT_TYPES == 2);
    __ JumpIfSmi(input, false_label);
    __ CompareObjectType(input, scratch, input, JS_FUNCTION_TYPE);
    __ beq(true_label);
    __ cmpi(input, Operand(JS_FUNCTION_PROXY_TYPE));
    final_branch_condition = eq;

  } else if (type_name->Equals(heap()->object_symbol())) {
    __ JumpIfSmi(input, false_label);
    if (!FLAG_harmony_typeof) {
      __ CompareRoot(input, Heap::kNullValueRootIndex);
      __ beq(true_label);
    }
    __ CompareObjectType(input, input, scratch,
                         FIRST_NONCALLABLE_SPEC_OBJECT_TYPE);
    __ blt(false_label);
    __ CompareInstanceType(input, scratch, LAST_NONCALLABLE_SPEC_OBJECT_TYPE);
    __ bgt(false_label);
    // Check for undetectable objects => false.
    __ lbz(ip, FieldMemOperand(input, Map::kBitFieldOffset));
    __ ExtractBit(r0, ip, 31 - Map::kIsUndetectable);
    __ cmpi(r0, Operand::Zero());
    final_branch_condition = eq;

  } else {
    __ b(false_label);
  }

  return final_branch_condition;
}


void LCodeGen::DoIsConstructCallAndBranch(LIsConstructCallAndBranch* instr) {
  Register temp1 = ToRegister(instr->temp());
  int true_block = chunk_->LookupDestination(instr->true_block_id());
  int false_block = chunk_->LookupDestination(instr->false_block_id());

  EmitIsConstructCall(temp1, scratch0());
  EmitBranch(true_block, false_block, eq);
}


void LCodeGen::EmitIsConstructCall(Register temp1, Register temp2) {
  ASSERT(!temp1.is(temp2));
  // Get the frame pointer for the calling frame.
  __ lwz(temp1, MemOperand(fp, StandardFrameConstants::kCallerFPOffset));

  // Skip the arguments adaptor frame if it exists.
  Label check_frame_marker;
  __ lwz(temp2, MemOperand(temp1, StandardFrameConstants::kContextOffset));
  __ cmpi(temp2, Operand(Smi::FromInt(StackFrame::ARGUMENTS_ADAPTOR)));
  __ bne(&check_frame_marker);
  __ lwz(temp1, MemOperand(temp1, StandardFrameConstants::kCallerFPOffset));

  // Check the marker in the calling frame.
  __ bind(&check_frame_marker);
  __ lwz(temp1, MemOperand(temp1, StandardFrameConstants::kMarkerOffset));
  __ cmpi(temp1, Operand(Smi::FromInt(StackFrame::CONSTRUCT)));
}


void LCodeGen::EnsureSpaceForLazyDeopt() {
  // Ensure that we have enough space after the previous lazy-bailout
  // instruction for patching the code here.
  int current_pc = masm()->pc_offset();
  int patch_size = Deoptimizer::patch_size();
  if (current_pc < last_lazy_deopt_pc_ + patch_size) {
    int padding_size = last_lazy_deopt_pc_ + patch_size - current_pc;
    ASSERT_EQ(0, padding_size % Assembler::kInstrSize);
    while (padding_size > 0) {
      __ nop();
      padding_size -= Assembler::kInstrSize;
    }
  }
  last_lazy_deopt_pc_ = masm()->pc_offset();
}


void LCodeGen::DoLazyBailout(LLazyBailout* instr) {
  EnsureSpaceForLazyDeopt();
  ASSERT(instr->HasEnvironment());
  LEnvironment* env = instr->environment();
  RegisterEnvironmentForDeoptimization(env, Safepoint::kLazyDeopt);
  safepoints_.RecordLazyDeoptimizationIndex(env->deoptimization_index());
}


void LCodeGen::DoDeoptimize(LDeoptimize* instr) {
  DeoptimizeIf(al, instr->environment());
}


void LCodeGen::DoDeleteProperty(LDeleteProperty* instr) {
  Register object = ToRegister(instr->object());
  Register key = ToRegister(instr->key());
  Register strict = scratch0();
  __ mov(strict, Operand(Smi::FromInt(strict_mode_flag())));
  __ Push(object, key, strict);
  ASSERT(instr->HasPointerMap());
  LPointerMap* pointers = instr->pointer_map();
  RecordPosition(pointers->position());
  SafepointGenerator safepoint_generator(
      this, pointers, Safepoint::kLazyDeopt);
  __ InvokeBuiltin(Builtins::DELETE, CALL_FUNCTION, safepoint_generator);
}


void LCodeGen::DoIn(LIn* instr) {
  Register obj = ToRegister(instr->object());
  Register key = ToRegister(instr->key());
  __ Push(key, obj);
  ASSERT(instr->HasPointerMap());
  LPointerMap* pointers = instr->pointer_map();
  RecordPosition(pointers->position());
  SafepointGenerator safepoint_generator(this, pointers, Safepoint::kLazyDeopt);
  __ InvokeBuiltin(Builtins::IN, CALL_FUNCTION, safepoint_generator);
}


void LCodeGen::DoDeferredStackCheck(LStackCheck* instr) {
  PushSafepointRegistersScope scope(this, Safepoint::kWithRegisters);
  __ CallRuntimeSaveDoubles(Runtime::kStackGuard);
  RecordSafepointWithLazyDeopt(
      instr, RECORD_SAFEPOINT_WITH_REGISTERS_AND_NO_ARGUMENTS);
  ASSERT(instr->HasEnvironment());
  LEnvironment* env = instr->environment();
  safepoints_.RecordLazyDeoptimizationIndex(env->deoptimization_index());
}


void LCodeGen::DoStackCheck(LStackCheck* instr) {
  class DeferredStackCheck: public LDeferredCode {
   public:
    DeferredStackCheck(LCodeGen* codegen, LStackCheck* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() { codegen()->DoDeferredStackCheck(instr_); }
    virtual LInstruction* instr() { return instr_; }
   private:
    LStackCheck* instr_;
  };

  ASSERT(instr->HasEnvironment());
  LEnvironment* env = instr->environment();
  // There is no LLazyBailout instruction for stack-checks. We have to
  // prepare for lazy deoptimization explicitly here.
  if (instr->hydrogen()->is_function_entry()) {
    // Perform stack overflow check.
    Label done;
    __ LoadRoot(ip, Heap::kStackLimitRootIndex);
    __ cmpl(sp, ip);
    __ bge(&done);
    StackCheckStub stub;
    CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
    EnsureSpaceForLazyDeopt();
    __ bind(&done);
    RegisterEnvironmentForDeoptimization(env, Safepoint::kLazyDeopt);
    safepoints_.RecordLazyDeoptimizationIndex(env->deoptimization_index());
  } else {
    ASSERT(instr->hydrogen()->is_backwards_branch());
    // Perform stack overflow check if this goto needs it before jumping.
    DeferredStackCheck* deferred_stack_check =
        new(zone()) DeferredStackCheck(this, instr);
    __ LoadRoot(ip, Heap::kStackLimitRootIndex);
    __ cmpl(sp, ip);
    __ blt(deferred_stack_check->entry());
    EnsureSpaceForLazyDeopt();
    __ bind(instr->done_label());
    deferred_stack_check->SetExit(instr->done_label());
    RegisterEnvironmentForDeoptimization(env, Safepoint::kLazyDeopt);
    // Don't record a deoptimization index for the safepoint here.
    // This will be done explicitly when emitting call and the safepoint in
    // the deferred code.
  }
}


void LCodeGen::DoOsrEntry(LOsrEntry* instr) {
  // This is a pseudo-instruction that ensures that the environment here is
  // properly registered for deoptimization and records the assembler's PC
  // offset.
  LEnvironment* environment = instr->environment();
  environment->SetSpilledRegisters(instr->SpilledRegisterArray(),
                                   instr->SpilledDoubleRegisterArray());

  // If the environment were already registered, we would have no way of
  // backpatching it with the spill slot operands.
  ASSERT(!environment->HasBeenRegistered());
  RegisterEnvironmentForDeoptimization(environment, Safepoint::kNoLazyDeopt);
  ASSERT(osr_pc_offset_ == -1);
  osr_pc_offset_ = masm()->pc_offset();
}


void LCodeGen::DoForInPrepareMap(LForInPrepareMap* instr) {
  __ LoadRoot(ip, Heap::kUndefinedValueRootIndex);
  __ cmp(r3, ip);
  DeoptimizeIf(eq, instr->environment());

  Register null_value = r8;
  __ LoadRoot(null_value, Heap::kNullValueRootIndex);
  __ cmp(r3, null_value);
  DeoptimizeIf(eq, instr->environment());

  __ TestIfSmi(r3, r0);
  DeoptimizeIf(eq, instr->environment(), cr0);

  STATIC_ASSERT(FIRST_JS_PROXY_TYPE == FIRST_SPEC_OBJECT_TYPE);
  __ CompareObjectType(r3, r4, r4, LAST_JS_PROXY_TYPE);
  DeoptimizeIf(le, instr->environment());

  Label use_cache, call_runtime;
  __ CheckEnumCache(null_value, &call_runtime);

  __ lwz(r3, FieldMemOperand(r3, HeapObject::kMapOffset));
  __ b(&use_cache);

  // Get the set of properties to enumerate.
  __ bind(&call_runtime);
  __ push(r3);
  CallRuntime(Runtime::kGetPropertyNamesFast, 1, instr);

  __ lwz(r4, FieldMemOperand(r3, HeapObject::kMapOffset));
  __ LoadRoot(ip, Heap::kMetaMapRootIndex);
  __ cmp(r4, ip);
  DeoptimizeIf(ne, instr->environment());
  __ bind(&use_cache);
}


void LCodeGen::DoForInCacheArray(LForInCacheArray* instr) {
  Register map = ToRegister(instr->map());
  Register result = ToRegister(instr->result());
  Label load_cache, done;
  __ EnumLength(result, map);
  __ cmpi(result, Operand(Smi::FromInt(0)));
  __ bne(&load_cache);
  __ mov(result, Operand(isolate()->factory()->empty_fixed_array()));
  __ b(&done);

  __ bind(&load_cache);
  __ LoadInstanceDescriptors(map, result);
  __ lwz(result,
         FieldMemOperand(result, DescriptorArray::kEnumCacheOffset));
  __ lwz(result,
         FieldMemOperand(result, FixedArray::SizeFor(instr->idx())));
  __ cmpi(result, Operand::Zero());
  DeoptimizeIf(eq, instr->environment());

  __ bind(&done);
}


void LCodeGen::DoCheckMapValue(LCheckMapValue* instr) {
  Register object = ToRegister(instr->value());
  Register map = ToRegister(instr->map());
  __ lwz(scratch0(), FieldMemOperand(object, HeapObject::kMapOffset));
  __ cmp(map, scratch0());
  DeoptimizeIf(ne, instr->environment());
}


void LCodeGen::DoLoadFieldByIndex(LLoadFieldByIndex* instr) {
  Register object = ToRegister(instr->object());
  Register index = ToRegister(instr->index());
  Register result = ToRegister(instr->result());
  Register scratch = scratch0();

  Label out_of_object, done;
  __ cmpi(index, Operand::Zero());
  __ blt(&out_of_object);

  STATIC_ASSERT(kPointerSizeLog2 > kSmiTagSize);
  __ slwi(r0, index, Operand(kPointerSizeLog2 - kSmiTagSize));
  __ add(scratch, object, r0);
  __ lwz(result, FieldMemOperand(scratch, JSObject::kHeaderSize));

  __ b(&done);

  __ bind(&out_of_object);
  __ lwz(result, FieldMemOperand(object, JSObject::kPropertiesOffset));
  // Index is equal to negated out of object property index plus 1.
  __ slwi(r0, index, Operand(kPointerSizeLog2 - kSmiTagSize));
  __ sub(scratch, result, r0);
  __ lwz(result, FieldMemOperand(scratch,
                                 FixedArray::kHeaderSize - kPointerSize));
  __ bind(&done);
}


#undef __

} }  // namespace v8::internal
