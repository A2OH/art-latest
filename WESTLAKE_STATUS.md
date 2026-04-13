# Westlake ART Engine — Status & Architecture

## What It Is

Westlake is a custom ART (Android Runtime) v114 engine compiled from AOSP `android-15.0.0_r10`
(tag `aosp-art-15`). It runs as a shared library (`libwestlake_art.so`) inside an Android host
APK, creating a second ART runtime alongside the phone's native ART.

**Note on version numbering:** The AOSP source tag is Android 15, but the internal ART version
number in the source is 114 (`kRuntimeISA` / image version). Android 15's shipping ART on
devices is typically v118+. Our build from the AOSP android-15 source produces ART v114.

## Current Capability (2026-04-13)

### What Works
- ART v114 runtime boots with boot image (49,541 method entry points fixed)
- Switch interpreter executes Java bytecode (no JIT/AOT)
- 40+ InterpreterJni shorty patterns for native method dispatch
- 11/11 Android framework classes initialize (Build, Context, Activity, etc.)
- Real McDonald's app classes loaded and introspected:
  - `SplashActivity` (61 methods, 13-level class hierarchy)
  - `SplashPresenterImpl` (71 methods)
  - Kotlin/Hilt/Dagger annotations
- In-process execution alongside phone's native ART
- Host APK displays engine status on screen

### What Doesn't Work Yet
- **Activity instantiation** — constructor triggers class init cascades that overflow
- **Context creation** — `ContextImpl.createSystemContext()` needs system service natives
- **UI rendering through Westlake** — would need full View/Canvas/Window bridge
- **String.lastIndexOf(int)** — interpreter register corruption bug (workaround: avoid it)
- **Build.<clinit>** — SocProperties infinite recursion (workaround: clinit tolerance + field patching)

## Architecture

```
┌─────────────────────────────────────────────┐
│  Phone (OnePlus 6, Android 15/LineageOS)    │
│                                             │
│  ┌─────────────────────────────────────┐    │
│  │  Host APK (com.westlake.host)       │    │
│  │  Runs on phone's native ART v118    │    │
│  │                                     │    │
│  │  ┌───────────────────────────────┐  │    │
│  │  │  libwestlake_art.so (26MB)    │  │    │
│  │  │  ART v114 runtime             │  │    │
│  │  │  Separate thread, own heap    │  │    │
│  │  │                               │  │    │
│  │  │  Boot Image (5MB)             │  │    │
│  │  │  ├ core-oj-a15.jar            │  │    │
│  │  │  └ core-libart-a15.jar        │  │    │
│  │  │                               │  │    │
│  │  │  Boot Classpath               │  │    │
│  │  │  ├ framework.jar              │  │    │
│  │  │  ├ mcd_classes[1-33].dex      │  │    │
│  │  │  └ mcdloader.jar              │  │    │
│  │  │                               │  │    │
│  │  │  McdLoader.main() ──────────► │  │    │
│  │  │  ├ Class.forName(Splash...)   │  │    │
│  │  │  ├ getDeclaredMethods()       │  │    │
│  │  │  ├ getSuperclass() chain      │  │    │
│  │  │  └ return 0 (success)         │  │    │
│  │  └───────────────────────────────┘  │    │
│  │                                     │    │
│  │  After engine returns:              │    │
│  │  startActivity(MCD SplashActivity)  │    │
│  └─────────────────────────────────────┘    │
│                                             │
│  ┌─────────────────────────────────────┐    │
│  │  MCD App (com.mcdonalds.app)        │    │
│  │  Runs on phone's native ART v118    │    │
│  │  (launched via Intent, NOT Westlake) │    │
│  └─────────────────────────────────────┘    │
└─────────────────────────────────────────────┘
```

## Key Fixes Applied

### SIGBUS at 0xfffffffffffffb17 (Root Cause)
InterpreterJni was calling JNI dlsym lookup assembly stubs as C functions.
The stubs expect ARM64 register calling convention, not C ABI.
**Fix:** Check if native entry is dlsym stub before calling; resolve via
`FindCodeForNativeMethod` or throw `UnsatisfiedLinkError`.

### Boot Image Entry Points
- `VisitPackedArtMethods`: reset all 49,541 method entry points to interpreter bridges
- Also clear stale `data_` (JNI entry) for 913 native methods
- `ClassLinker::AddImageSpace`: override OAT header trampolines with runtime stubs

### Class Init Tolerance
All `<clinit>` failures tolerated — class marked as initialized with defaults.
Prevents cascading failures from missing system services.

### Bytecode Patches (dalvikvm.cc / westlake_jni.cc)
- `Throwable.printStackTrace()` → no-op (prevents infinite exception loops)
- `TextUtils.formatSimple()` → C implementation (prevents Build clinit StackOverflow)
- `String.lastIndexOf(int)` → native (interpreter register corruption workaround)

### Native Stubs (framework_native_stubs.c)
- `Log.isLoggable`, `Log.println_native`
- `SystemProperties.native_get` (with hardcoded property values)
- `SocProperties.soc_manufacturer/soc_model`
- `TelephonyProperties.baseband_version`

### Interpreter Additions
40+ InterpreterJni shorty patterns including: ZLI, IILL, ZLZ, ILI, JLJ, VLL, LLI, IZ, II

## Files

### Source Patches (patches/)
- `runtime/runtime.cc` — direct JNI_OnLoad calls, system property stubs, clinit tolerance
- `runtime/class_linker.cc` — OAT trampoline override, clinit tolerance, class init logging
- `runtime/gc/space/image_space.cc` — stale JNI entry clearing, entry point fixup
- `runtime/interpreter/interpreter.cc` — 40+ InterpreterJni patterns, native resolution, depth guard
- `runtime/interpreter/interpreter_common.cc` — depth guard, debug logging removal
- `runtime/art_method.cc` — force all methods through EnterInterpreterFromInvoke
- `runtime/instrumentation.cc` — CanUseAotCode=false
- `dalvikvm/dalvikvm.cc` — bytecode patches, SIGBUS handler, Build field patching

### Stubs (stubs/)
- `westlake_jni.cc` — in-process engine thread, 32MB stack, bytecode patches
- `framework_native_stubs.c` — Log, SystemProperties, SocProperties stubs
- `generic_jni_trampoline_handler.cc` — C++ handler for assembly JNI trampoline
- `link_stubs_arm64.cc` — OpenNativeLibrary stub, JNI_OnLoad dispatchers

### Build
- `Makefile.bionic-arm64` — cross-compile for ARM64 with Android NDK
- Targets: `link-runtime` (static dalvikvm), `link-dynamic` (PIE dalvikvm-dynamic),
  `link-shared` (libwestlake_art.so)

## Device
- OnePlus 6 (ONEPLUS A6003)
- Android 15 / LineageOS 22.2
- Kernel 4.9.337
- Rooted (su available)
- Device ID: cfb7c9e3
- Deploy dir: /data/local/tmp/westlake/
