/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "var_handles.h"

#include "art_field-inl.h"
#include "art_method.h"
#include "common_throws.h"
#include "dex/dex_instruction.h"
#include "handle.h"
#include "method_handles-inl.h"
#include "mirror/method_type-inl.h"
#include "mirror/var_handle.h"

namespace art HIDDEN {

namespace {

const char* PFCutVarHandleAccessModeName(mirror::VarHandle::AccessMode access_mode) {
  switch (access_mode) {
    case mirror::VarHandle::AccessMode::kGet: return "get";
    case mirror::VarHandle::AccessMode::kSet: return "set";
    case mirror::VarHandle::AccessMode::kGetVolatile: return "getVolatile";
    case mirror::VarHandle::AccessMode::kSetVolatile: return "setVolatile";
    case mirror::VarHandle::AccessMode::kGetAcquire: return "getAcquire";
    case mirror::VarHandle::AccessMode::kSetRelease: return "setRelease";
    case mirror::VarHandle::AccessMode::kGetOpaque: return "getOpaque";
    case mirror::VarHandle::AccessMode::kSetOpaque: return "setOpaque";
    case mirror::VarHandle::AccessMode::kCompareAndSet: return "compareAndSet";
    case mirror::VarHandle::AccessMode::kCompareAndExchange: return "compareAndExchange";
    case mirror::VarHandle::AccessMode::kCompareAndExchangeAcquire:
      return "compareAndExchangeAcquire";
    case mirror::VarHandle::AccessMode::kCompareAndExchangeRelease:
      return "compareAndExchangeRelease";
    case mirror::VarHandle::AccessMode::kWeakCompareAndSetPlain: return "weakCompareAndSetPlain";
    case mirror::VarHandle::AccessMode::kWeakCompareAndSet: return "weakCompareAndSet";
    case mirror::VarHandle::AccessMode::kWeakCompareAndSetAcquire:
      return "weakCompareAndSetAcquire";
    case mirror::VarHandle::AccessMode::kWeakCompareAndSetRelease:
      return "weakCompareAndSetRelease";
    case mirror::VarHandle::AccessMode::kGetAndSet: return "getAndSet";
    case mirror::VarHandle::AccessMode::kGetAndSetAcquire: return "getAndSetAcquire";
    case mirror::VarHandle::AccessMode::kGetAndSetRelease: return "getAndSetRelease";
    case mirror::VarHandle::AccessMode::kGetAndAdd: return "getAndAdd";
    case mirror::VarHandle::AccessMode::kGetAndAddAcquire: return "getAndAddAcquire";
    case mirror::VarHandle::AccessMode::kGetAndAddRelease: return "getAndAddRelease";
    case mirror::VarHandle::AccessMode::kGetAndBitwiseOr: return "getAndBitwiseOr";
    case mirror::VarHandle::AccessMode::kGetAndBitwiseOrRelease:
      return "getAndBitwiseOrRelease";
    case mirror::VarHandle::AccessMode::kGetAndBitwiseOrAcquire:
      return "getAndBitwiseOrAcquire";
    case mirror::VarHandle::AccessMode::kGetAndBitwiseAnd: return "getAndBitwiseAnd";
    case mirror::VarHandle::AccessMode::kGetAndBitwiseAndRelease:
      return "getAndBitwiseAndRelease";
    case mirror::VarHandle::AccessMode::kGetAndBitwiseAndAcquire:
      return "getAndBitwiseAndAcquire";
    case mirror::VarHandle::AccessMode::kGetAndBitwiseXor: return "getAndBitwiseXor";
    case mirror::VarHandle::AccessMode::kGetAndBitwiseXorRelease:
      return "getAndBitwiseXorRelease";
    case mirror::VarHandle::AccessMode::kGetAndBitwiseXorAcquire:
      return "getAndBitwiseXorAcquire";
  }
  return "<unknown>";
}

bool PFCutIsZeroMaskFieldVarHandleModeSupported(Handle<mirror::VarHandle> var_handle,
                                                mirror::VarHandle::AccessMode access_mode)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (var_handle.IsNull() ||
      var_handle->GetField32(mirror::VarHandle::AccessModesBitMaskOffset()) != 0) {
    return false;
  }

  ObjPtr<mirror::Class> vh_class = var_handle->GetClass();
  if (vh_class == nullptr ||
      (!vh_class->DescriptorEquals("Ljava/lang/invoke/FieldVarHandle;") &&
       !vh_class->DescriptorEquals("Ljava/lang/invoke/StaticFieldVarHandle;"))) {
    return false;
  }

  ArtField* field = ObjPtr<mirror::FieldVarHandle>::DownCast(var_handle.Get())->GetArtField();
  if (field == nullptr) {
    return false;
  }

  const bool is_final = field->IsFinal();
  const Primitive::Type primitive_type = var_handle->GetVarType()->GetPrimitiveType();
  switch (access_mode) {
    case mirror::VarHandle::AccessMode::kGet:
    case mirror::VarHandle::AccessMode::kGetVolatile:
    case mirror::VarHandle::AccessMode::kGetAcquire:
    case mirror::VarHandle::AccessMode::kGetOpaque:
      return true;

    case mirror::VarHandle::AccessMode::kSet:
    case mirror::VarHandle::AccessMode::kSetVolatile:
    case mirror::VarHandle::AccessMode::kSetRelease:
    case mirror::VarHandle::AccessMode::kSetOpaque:
    case mirror::VarHandle::AccessMode::kCompareAndSet:
    case mirror::VarHandle::AccessMode::kCompareAndExchange:
    case mirror::VarHandle::AccessMode::kCompareAndExchangeAcquire:
    case mirror::VarHandle::AccessMode::kCompareAndExchangeRelease:
    case mirror::VarHandle::AccessMode::kWeakCompareAndSetPlain:
    case mirror::VarHandle::AccessMode::kWeakCompareAndSet:
    case mirror::VarHandle::AccessMode::kWeakCompareAndSetAcquire:
    case mirror::VarHandle::AccessMode::kWeakCompareAndSetRelease:
    case mirror::VarHandle::AccessMode::kGetAndSet:
    case mirror::VarHandle::AccessMode::kGetAndSetAcquire:
    case mirror::VarHandle::AccessMode::kGetAndSetRelease:
      return !is_final;

    case mirror::VarHandle::AccessMode::kGetAndAdd:
    case mirror::VarHandle::AccessMode::kGetAndAddAcquire:
    case mirror::VarHandle::AccessMode::kGetAndAddRelease:
      return !is_final &&
             (primitive_type == Primitive::kPrimByte ||
              primitive_type == Primitive::kPrimShort ||
              primitive_type == Primitive::kPrimChar ||
              primitive_type == Primitive::kPrimInt ||
              primitive_type == Primitive::kPrimLong ||
              primitive_type == Primitive::kPrimFloat ||
              primitive_type == Primitive::kPrimDouble);

    case mirror::VarHandle::AccessMode::kGetAndBitwiseOr:
    case mirror::VarHandle::AccessMode::kGetAndBitwiseOrRelease:
    case mirror::VarHandle::AccessMode::kGetAndBitwiseOrAcquire:
    case mirror::VarHandle::AccessMode::kGetAndBitwiseAnd:
    case mirror::VarHandle::AccessMode::kGetAndBitwiseAndRelease:
    case mirror::VarHandle::AccessMode::kGetAndBitwiseAndAcquire:
    case mirror::VarHandle::AccessMode::kGetAndBitwiseXor:
    case mirror::VarHandle::AccessMode::kGetAndBitwiseXorRelease:
    case mirror::VarHandle::AccessMode::kGetAndBitwiseXorAcquire:
      return !is_final &&
             (primitive_type == Primitive::kPrimBoolean ||
              primitive_type == Primitive::kPrimByte ||
              primitive_type == Primitive::kPrimShort ||
              primitive_type == Primitive::kPrimChar ||
              primitive_type == Primitive::kPrimInt ||
              primitive_type == Primitive::kPrimLong);
  }
  return false;
}

template <typename CallSiteType, typename CalleeType>
class ThrowWrongMethodTypeFunctionImpl final : public ThrowWrongMethodTypeFunction {
 public:
  ThrowWrongMethodTypeFunctionImpl(CallSiteType callsite_type, CalleeType callee_type)
      : callsite_type_(callsite_type),
        callee_type_(callee_type) {}

  ~ThrowWrongMethodTypeFunctionImpl() {}

  void operator()() const override REQUIRES_SHARED(Locks::mutator_lock_) {
    ThrowWrongMethodTypeException(mirror::MethodType::PrettyDescriptor(callee_type_),
                                  mirror::MethodType::PrettyDescriptor(callsite_type_));
  }

 private:
  CallSiteType callsite_type_;
  CalleeType callee_type_;
};

template <typename CallSiteType>
bool VarHandleInvokeAccessorWithConversions(Thread* self,
                                            ShadowFrame& shadow_frame,
                                            Handle<mirror::VarHandle> var_handle,
                                            CallSiteType callsite_type,
                                            mirror::VarHandle::AccessMode access_mode,
                                            const InstructionOperands* operands,
                                            JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  // Use a raw method handle for `accessor_type`, avoid allocating a managed `MethodType`.
  VariableSizedHandleScope accessor_type_hs(self);
  mirror::RawMethodType accessor_type(&accessor_type_hs);
  var_handle->GetMethodTypeForAccessMode(access_mode, accessor_type);
  using HandleScopeType = std::conditional_t<
      std::is_same_v<VariableSizedHandleScope*, CallSiteType>,
      Thread*,  // No handle scope needed, use `Thread*` that can be initialized from `self`.
      StackHandleScope<3>>;
  HandleScopeType hs(self);
  ThrowWrongMethodTypeFunctionImpl throw_wmt(callsite_type, accessor_type);
  auto from_types = mirror::MethodType::NewHandlePTypes(callsite_type, &hs);
  auto to_types = mirror::MethodType::NewHandlePTypes(accessor_type, &hs);
  const size_t num_vregs = mirror::MethodType::NumberOfVRegs(accessor_type);
  ShadowFrameAllocaUniquePtr accessor_frame =
      CREATE_SHADOW_FRAME(num_vregs, shadow_frame.GetMethod(), shadow_frame.GetDexPC());
  ShadowFrameGetter getter(shadow_frame, operands);
  static const uint32_t kFirstDestinationReg = 0;
  ShadowFrameSetter setter(accessor_frame.get(), kFirstDestinationReg);
  if (!PerformConversions(throw_wmt, from_types, to_types, &getter, &setter)) {
    DCHECK(self->IsExceptionPending());
    return false;
  }
  RangeInstructionOperands accessor_operands(kFirstDestinationReg,
                                             kFirstDestinationReg + num_vregs);
  if (!var_handle->Access(access_mode, accessor_frame.get(), &accessor_operands, result)) {
    DCHECK(self->IsExceptionPending());
    return false;
  }
  if (!ConvertReturnValue(throw_wmt,
                          mirror::MethodType::GetRType(accessor_type),
                          mirror::MethodType::GetRType(callsite_type),
                          result)) {
    DCHECK(self->IsExceptionPending());
    return false;
  }
  return true;
}

template <typename CallSiteType>
bool VarHandleInvokeAccessorImpl(Thread* self,
                                 ShadowFrame& shadow_frame,
                                 Handle<mirror::VarHandle> var_handle,
                                 CallSiteType callsite_type,
                                 const mirror::VarHandle::AccessMode access_mode,
                                 const InstructionOperands* const operands,
                                 JValue* result) REQUIRES_SHARED(Locks::mutator_lock_) {
  if (var_handle.IsNull()) {
    ThrowNullPointerExceptionFromDexPC();
    return false;
  }

  if (!var_handle->IsAccessModeSupported(access_mode) &&
      !PFCutIsZeroMaskFieldVarHandleModeSupported(var_handle, access_mode)) {
    static thread_local int pfc_vh_uoe_count = 0;
    if (pfc_vh_uoe_count < 40) {
      pfc_vh_uoe_count++;
      ArtMethod* caller = shadow_frame.GetMethod();
      ObjPtr<mirror::Class> vh_class = var_handle->GetClass();
      ObjPtr<mirror::Class> var_type = var_handle->GetVarType();
      ObjPtr<mirror::Class> coord0 = var_handle->GetCoordinateType0();
      const int32_t mask =
          var_handle->GetField32(mirror::VarHandle::AccessModesBitMaskOffset());
      fprintf(stderr,
              "[PFCUT-VARHANDLE] unsupported mode=%s(%u) mask=0x%x caller=%s "
              "vh=%s var=%s coord0=%s expected=%s\n",
              PFCutVarHandleAccessModeName(access_mode),
              static_cast<uint32_t>(access_mode),
              mask,
              caller != nullptr ? caller->PrettyMethod().c_str() : "<null>",
              vh_class != nullptr ? vh_class->PrettyDescriptor().c_str() : "<null>",
              var_type != nullptr ? var_type->PrettyDescriptor().c_str() : "<null>",
              coord0 != nullptr ? coord0->PrettyDescriptor().c_str() : "<null>",
              var_handle->PrettyDescriptorForAccessMode(access_mode).c_str());
      fflush(stderr);
    }
    ThrowUnsupportedOperationException();
    return false;
  }
  if (!var_handle->IsAccessModeSupported(access_mode)) {
    const bool pfc_vh_zero_mask_fallback_trace = false;
    static thread_local int pfc_vh_zero_mask_fallback_count = 0;
    if (UNLIKELY(pfc_vh_zero_mask_fallback_trace && pfc_vh_zero_mask_fallback_count < 40)) {
      pfc_vh_zero_mask_fallback_count++;
      ArtMethod* caller = shadow_frame.GetMethod();
      fprintf(stderr,
              "[PFCUT-VARHANDLE] zero-mask native field fallback mode=%s(%u) caller=%s\n",
              PFCutVarHandleAccessModeName(access_mode),
              static_cast<uint32_t>(access_mode),
              caller != nullptr ? caller->PrettyMethod().c_str() : "<null>");
      fflush(stderr);
    }
  }

  mirror::VarHandle::MatchKind match_kind =
      var_handle->GetMethodTypeMatchForAccessMode(access_mode, callsite_type);
  if (LIKELY(match_kind == mirror::VarHandle::MatchKind::kExact)) {
    return var_handle->Access(access_mode, &shadow_frame, operands, result);
  } else if (match_kind == mirror::VarHandle::MatchKind::kWithConversions) {
    return VarHandleInvokeAccessorWithConversions(self,
                                                  shadow_frame,
                                                  var_handle,
                                                  callsite_type,
                                                  access_mode,
                                                  operands,
                                                  result);
  } else {
    DCHECK_EQ(match_kind, mirror::VarHandle::MatchKind::kNone);
    ThrowWrongMethodTypeException(var_handle->PrettyDescriptorForAccessMode(access_mode),
                                  mirror::MethodType::PrettyDescriptor(callsite_type));
    return false;
  }
}

}  // namespace

bool VarHandleInvokeAccessor(Thread* self,
                             ShadowFrame& shadow_frame,
                             Handle<mirror::VarHandle> var_handle,
                             Handle<mirror::MethodType> callsite_type,
                             const mirror::VarHandle::AccessMode access_mode,
                             const InstructionOperands* const operands,
                             JValue* result) {
  return VarHandleInvokeAccessorImpl(
      self, shadow_frame, var_handle, callsite_type, access_mode, operands, result);
}

bool VarHandleInvokeAccessor(Thread* self,
                             ShadowFrame& shadow_frame,
                             Handle<mirror::VarHandle> var_handle,
                             mirror::RawMethodType callsite_type,
                             const mirror::VarHandle::AccessMode access_mode,
                             const InstructionOperands* const operands,
                             JValue* result) {
  return VarHandleInvokeAccessorImpl(
      self, shadow_frame, var_handle, callsite_type, access_mode, operands, result);
}

bool VarHandleInvokeAccessor(Thread* self,
                             ShadowFrame& shadow_frame,
                             Handle<mirror::VarHandle> var_handle,
                             ArtMethod* caller_method,
                             const dex::ProtoIndex callsite_type_id,
                             const mirror::VarHandle::AccessMode access_mode,
                             const InstructionOperands* const operands,
                             JValue* result) {
  StackHandleScope<3> hs(self);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  Handle<mirror::DexCache> dex_cache = hs.NewHandle(caller_method->GetDexCache());
  Handle<mirror::ClassLoader> class_loader = hs.NewHandle(caller_method->GetClassLoader());

  // If the `ThreadLocalRandom` class is not yet initialized, do the `VarHandle` operation
  // without creating a managed `MethodType` object. This avoids a circular initialization
  // issue when `ThreadLocalRandom.<clinit>` indirectly calls `AtomicLong.compareAndSet()`
  // (implemented with a `VarHandle`) and the `MethodType` caching circles back to the
  // `ThreadLocalRandom` with uninitialized `seeder` and throws NPE.
  //
  // Do a quick test for "visibly initialized" without a read barrier and, if that fails,
  // do a thorough test for "initialized" (including load acquire) with the read barrier.
  ArtField* field = WellKnownClasses::java_util_concurrent_ThreadLocalRandom_seeder;
  if (LIKELY(field->GetDeclaringClass<kWithoutReadBarrier>()->IsVisiblyInitialized()) ||
      field->GetDeclaringClass()->IsInitialized()) {
    Handle<mirror::MethodType> callsite_type(hs.NewHandle(
        class_linker->ResolveMethodType(self, callsite_type_id, dex_cache, class_loader)));

    if (LIKELY(callsite_type != nullptr)) {
      return VarHandleInvokeAccessor(self,
                                     shadow_frame,
                                     var_handle,
                                     callsite_type,
                                     access_mode,
                                     operands,
                                     result);
    }
    // This implies we couldn't resolve one or more types in this VarHandle,
    // or we could not allocate the `MethodType` object.
    CHECK(self->IsExceptionPending());
    if (self->GetException()->GetClass() != WellKnownClasses::java_lang_OutOfMemoryError.Get()) {
      return false;
    }
    // Clear the OOME and retry without creating an actual `MethodType` object.
    // This prevents unexpected OOME for trivial `VarHandle` operations.
    // It also prevents odd situations where a `VarHandle` operation succeeds but the same
    // operation fails later because the `MethodType` object was evicted from the `DexCache`
    // and we suddenly run out of memory to allocate a new one.
    //
    // We have previously seen OOMEs in the run-test `183-rmw-stress-test` with
    // `--optimizng --no-image` (boot class path methods run in interpreter without JIT)
    // but it probably happened on the first execution of a trivial `VarHandle` operation
    // and not due to the `DexCache` eviction mentioned above.
    self->ClearException();
  }

  VariableSizedHandleScope callsite_type_hs(self);
  mirror::RawMethodType callsite_type(&callsite_type_hs);
  if (!class_linker->ResolveMethodType(self,
                                       callsite_type_id,
                                       dex_cache,
                                       class_loader,
                                       callsite_type)) {
    CHECK(self->IsExceptionPending());
    return false;
  }
  return VarHandleInvokeAccessor(self,
                                 shadow_frame,
                                 var_handle,
                                 callsite_type,
                                 access_mode,
                                 operands,
                                 result);
}

}  // namespace art
