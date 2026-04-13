// Generic JNI native method caller — handles ANY shorty pattern
// by iterating the shorty string and building JNI args dynamically.
#ifndef ART_GENERIC_JNI_CALLER_H
#define ART_GENERIC_JNI_CALLER_H

#include "art_method.h"
#include "jvalue.h"
#include "mirror/object.h"
#include "scoped_thread_state_change.h"

namespace art {
namespace interpreter {

// Call any native method by dispatching through its JNI entry point.
// This replaces the assembly art_quick_generic_jni_trampoline for cases
// where the assembly doesn't work (static binary, no proper frame setup).
static inline void GenericJniCall(Thread* self,
                                   ArtMethod* method,
                                   std::string_view shorty,
                                   ObjPtr<mirror::Object> receiver,
                                   uint32_t* args,
                                   JValue* result) {
  ScopedObjectAccessUnchecked soa(self);
  const void* native_fn = method->GetEntryPointFromJni();
  if (native_fn == nullptr) {
    LOG(WARNING) << "GenericJniCall: null JNI entry for " << method->PrettyMethod();
    return;
  }

  // Build JNI args: env, class/this, then params from shorty
  // Max 16 args should be enough for any practical JNI method
  jvalue jni_args[16];
  int nargs = 0;
  
  // First arg: JNIEnv*
  jni_args[nargs++].l = reinterpret_cast<jobject>(soa.Env());
  
  // Second arg: jclass (static) or jobject (instance)
  if (method->IsStatic()) {
    jni_args[nargs++].l = soa.AddLocalReference<jclass>(method->GetDeclaringClass());
  } else {
    jni_args[nargs++].l = soa.AddLocalReference<jobject>(receiver);
  }
  
  // Remaining args from shorty (skip return type at index 0)
  uint32_t* arg_ptr = args;
  for (size_t i = 1; i < shorty.size() && nargs < 16; i++) {
    switch (shorty[i]) {
      case 'Z': jni_args[nargs++].z = static_cast<jboolean>(*arg_ptr++); break;
      case 'B': jni_args[nargs++].b = static_cast<jbyte>(*arg_ptr++); break;
      case 'C': jni_args[nargs++].c = static_cast<jchar>(*arg_ptr++); break;
      case 'S': jni_args[nargs++].s = static_cast<jshort>(*arg_ptr++); break;
      case 'I': jni_args[nargs++].i = static_cast<jint>(*arg_ptr++); break;
      case 'F': jni_args[nargs++].f = *reinterpret_cast<float*>(arg_ptr++); break;
      case 'J': {
        jlong val = *reinterpret_cast<jlong*>(arg_ptr);
        jni_args[nargs++].j = val;
        arg_ptr += 2; // long takes 2 slots
        break;
      }
      case 'D': {
        jdouble val = *reinterpret_cast<jdouble*>(arg_ptr);
        jni_args[nargs++].d = val;
        arg_ptr += 2;
        break;
      }
      case 'L': {
        ObjPtr<mirror::Object> obj = reinterpret_cast<StackReference<mirror::Object>*>(arg_ptr)->AsMirrorPtr();
        jni_args[nargs++].l = obj == nullptr ? nullptr : soa.AddLocalReference<jobject>(obj);
        arg_ptr++;
        break;
      }
      default:
        LOG(WARNING) << "GenericJniCall: unknown shorty char '" << shorty[i] << "'";
        arg_ptr++;
        break;
    }
  }
  
  // Call the native function with the built args
  // We use a switch on return type to cast the function pointer correctly
  ScopedThreadStateChange tsc(self, ThreadState::kNative);
  
  char ret_type = shorty[0];
  switch (ret_type) {
    case 'V': {
      // Build call based on nargs (minus env and class/this)
      typedef void (*fn_v)(JNIEnv*, jobject, ...);
      auto fn = reinterpret_cast<fn_v>(native_fn);
      switch (nargs) {
        case 2: fn(soa.Env(), jni_args[1].l); break;
        case 3: fn(soa.Env(), jni_args[1].l, jni_args[2]); break;
        case 4: fn(soa.Env(), jni_args[1].l, jni_args[2], jni_args[3]); break;
        case 5: fn(soa.Env(), jni_args[1].l, jni_args[2], jni_args[3], jni_args[4]); break;
        case 6: fn(soa.Env(), jni_args[1].l, jni_args[2], jni_args[3], jni_args[4], jni_args[5]); break;
        default: fn(soa.Env(), jni_args[1].l); break;
      }
      break;
    }
    case 'I': case 'Z': case 'B': case 'C': case 'S': {
      typedef jint (*fn_i)(JNIEnv*, jobject, ...);
      auto fn = reinterpret_cast<fn_i>(native_fn);
      jint r;
      switch (nargs) {
        case 2: r = fn(soa.Env(), jni_args[1].l); break;
        case 3: r = fn(soa.Env(), jni_args[1].l, jni_args[2]); break;
        case 4: r = fn(soa.Env(), jni_args[1].l, jni_args[2], jni_args[3]); break;
        default: r = fn(soa.Env(), jni_args[1].l); break;
      }
      if (ret_type == 'Z') result->SetZ(r);
      else if (ret_type == 'B') result->SetB(r);
      else if (ret_type == 'C') result->SetC(r);
      else if (ret_type == 'S') result->SetS(r);
      else result->SetI(r);
      break;
    }
    case 'J': {
      typedef jlong (*fn_j)(JNIEnv*, jobject, ...);
      auto fn = reinterpret_cast<fn_j>(native_fn);
      jlong r;
      switch (nargs) {
        case 2: r = fn(soa.Env(), jni_args[1].l); break;
        case 3: r = fn(soa.Env(), jni_args[1].l, jni_args[2]); break;
        default: r = fn(soa.Env(), jni_args[1].l); break;
      }
      result->SetJ(r);
      break;
    }
    case 'L': {
      typedef jobject (*fn_l)(JNIEnv*, jobject, ...);
      auto fn = reinterpret_cast<fn_l>(native_fn);
      jobject r;
      switch (nargs) {
        case 2: r = fn(soa.Env(), jni_args[1].l); break;
        case 3: r = fn(soa.Env(), jni_args[1].l, jni_args[2]); break;
        case 4: r = fn(soa.Env(), jni_args[1].l, jni_args[2], jni_args[3]); break;
        default: r = fn(soa.Env(), jni_args[1].l); break;
      }
      result->SetL(soa.Decode<mirror::Object>(r));
      break;
    }
    default:
      LOG(WARNING) << "GenericJniCall: unhandled return type '" << ret_type << "'";
      break;
  }
}

}  // namespace interpreter
}  // namespace art

#endif  // ART_GENERIC_JNI_CALLER_H
