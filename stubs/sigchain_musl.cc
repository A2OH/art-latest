// sigchain_musl.cc - Minimal signal chain for musl/OHOS static builds.
// The real sigchain.cc dlopen's libc to intercept signal handlers, which doesn't
// work in static builds. This provides pass-through installation of ART's special
// fault handlers PLUS a re-assert thread.
//
// 2026-07-09: the previous version did a one-shot sigaction() for ART's fault
// handler. But OHOS libdfx installs its OWN SIGSEGV/SIGBUS handler LATER (during VM
// preload / board-lib load), overwriting ART's — so ART's NullPointerHandler never
// received the fault and every framework null deref became a fatal exit(1) (the ~18s
// parent death). Since we can't intercept every sigaction() call in a static build,
// we instead RE-ASSERT ART's handler on a background thread (~20ms), keeping it on
// top of whatever libdfx installs. ART's handler recovers implicit null checks (and
// falls through to the default for real faults).

#include <signal.h>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <pthread.h>
#include <unistd.h>
#include <cerrno>

// Must match aosp-art-15/sigchainlib/sigchain.h — note sc_sigaction returns bool.
struct SigchainAction {
  bool (*sc_sigaction)(int, siginfo_t*, void*);
  sigset_t sc_mask;
  uint64_t sc_flags;
};

extern "C" {

static struct sigaction g_art_sa[NSIG];
static bool g_art_have[NSIG];
static bool g_reassert_started = false;

// ─────────────────────────────────────────────────────────────────────────────
// WESTLAKE §631 (2026-08-15): HONOUR ART's bool.
//
// The previous version installed ART's fault handler DIRECTLY as the kernel handler and
// cast away its return value, with the comment "the kernel ignores the return of an
// SA_SIGINFO handler, so casting to the void-returning type is safe in practice".
// That is wrong. In AOSP sigchain (sigchainlib/sigchain.cc, SignalChain::Handler) the
// bool is the CLAIM-OR-CHAIN decision:
//     if (handler.sc_sigaction(signo, siginfo, ucontext_raw)) return;   // claimed
//     ... otherwise fall through to the next special handler, then to the USER handler,
//     ... and if that is SIG_DFL: restore SIG_DFL and return so the instruction refaults
//         into the default action ("the pre-crash state is restored, the crash happens
//         again, and the next handler gets a chance").
//
// ART's FaultManager returns false for any PC that is not in generated managed code —
// e.g. a genuine SIGSEGV inside libart's own C++. With the bool discarded, nothing
// chained: the handler simply returned to the faulting context and the instruction
// re-executed FOREVER. That is the "refault spin" signature (§436 family), and it is what
// made Toutiao livelock at ~40 unlogged SIGSEGV/s instead of producing a crash.
//
// This trampoline reproduces upstream's decision sequence with the one adaptation this
// port needs: we cannot intercept sigaction() in a static musl build, so "the user's
// handler" is whatever handler we DISPLACED (in practice OHOS libdfx's crash reporter),
// captured through the oldact out-parameter every time we install.
// ─────────────────────────────────────────────────────────────────────────────
// ART registers more than one special handler per signal (fault manager, and on some
// configurations a second one), so keep a small array exactly like upstream's
// SigchainAction special_handlers_[2].
static constexpr int kMaxSpecial = 2;
static SigchainAction g_special[NSIG][kMaxSpecial];
static int g_special_count[NSIG];
static struct sigaction g_prev_sa[NSIG];        // handler we displaced (libdfx's, usually)
static bool g_prev_have[NSIG];
// Set once we have handed a signal back to SIG_DFL. Re-entry (a re-assert that raced us
// back on top) must NOT run the special handlers again — it must hand off again and return.
static volatile sig_atomic_t g_default_handoff[NSIG];
// Upstream's GetHandlingSignal(): if a special handler itself faults, do not recurse into
// the special handlers a second time — go straight to the chain.
static volatile sig_atomic_t g_in_special[NSIG];

static void sigchain_handoff_to_default(int sig) {
  g_default_handoff[sig] = 1;
  g_art_have[sig] = false;              // stop the 2 ms re-assert thread touching it
  struct sigaction dfl;
  memset(&dfl, 0, sizeof(dfl));
  dfl.sa_handler = SIG_DFL;
  sigaction(sig, &dfl, nullptr);
  // Return; the faulting instruction re-executes and the default action terminates the
  // process with a real, tombstone-able crash. Upstream does exactly this, and it keeps
  // si_code == SEGV_MAPERR and the original faulting PC — which re-raise() would destroy.
}

static void sigchain_trampoline(int sig, siginfo_t* info, void* uc) {
  const int saved_errno = errno;
  if (sig <= 0 || sig >= NSIG) { errno = saved_errno; return; }

  if (g_default_handoff[sig]) {          // a raced re-assert put us back on top
    sigchain_handoff_to_default(sig);
    errno = saved_errno;
    return;
  }

  if (!g_in_special[sig]) {
    for (int i = 0; i < g_special_count[sig]; ++i) {
      SigchainAction* h = &g_special[sig][i];
      if (h->sc_sigaction == nullptr) break;
      sigset_t previous_mask;
      pthread_sigmask(SIG_SETMASK, &h->sc_mask, &previous_mask);
      const bool noreturn = (h->sc_flags & 0x1UL) != 0;   // SIGCHAIN_ALLOW_NORETURN
      const sig_atomic_t was = g_in_special[sig];
      if (!noreturn) g_in_special[sig] = 1;
      const bool claimed = h->sc_sigaction(sig, info, uc);
      if (!noreturn) g_in_special[sig] = was;
      if (claimed) {                     // ART recovered it (implicit null / suspend check)
        errno = saved_errno;
        return;
      }
      pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
    }
  }

  // Not claimed → chain to the handler we displaced (OHOS libdfx's crash reporter).
  if (g_prev_have[sig]) {
    const struct sigaction* p = &g_prev_sa[sig];
    if (p->sa_flags & SA_SIGINFO) {
      if (p->sa_sigaction != nullptr) {
        errno = saved_errno;
        p->sa_sigaction(sig, info, uc);
        errno = saved_errno;
        return;
      }
    } else if (p->sa_handler == SIG_IGN) {
      errno = saved_errno;
      return;
    } else if (p->sa_handler != SIG_DFL && p->sa_handler != nullptr) {
      errno = saved_errno;
      p->sa_handler(sig);
      errno = saved_errno;
      return;
    }
  }

  sigchain_handoff_to_default(sig);
  errno = saved_errno;
  // async-signal-safe note: no stdio anywhere in this function, on purpose.
}

// Install ART's (trampolined) action for signal s and remember whatever we displaced,
// but NEVER record our own trampoline as the "previous" handler — the re-assert thread
// calls this every 2 ms, so without that guard we would immediately overwrite libdfx's
// entry with ourselves and chain into infinite recursion.
static void sigchain_install_capture(int s) {
  if (s <= 0 || s >= NSIG) return;
  if (g_default_handoff[s]) return;      // we have already handed this signal to SIG_DFL
  struct sigaction old;
  memset(&old, 0, sizeof(old));
  if (sigaction(s, &g_art_sa[s], &old) != 0) return;
  const bool is_ours = (old.sa_flags & SA_SIGINFO) &&
                       old.sa_sigaction == sigchain_trampoline;
  if (!is_ours) {
    g_prev_sa[s] = old;
    g_prev_have[s] = true;
  }
}
// 2026-07-11: Runtime::PreZygoteFork() requires a single-threaded process
// (WaitUntilSingleThreaded polls /proc/self/stat and LOG(FATAL)s if num_threads != 1).
// This re-assert thread (a raw pthread) keeps num_threads == 2, so the zygote prefork
// aborts. SigchainStopReassert() (called from PreZygoteFork) signals it to exit before
// the check; atfork_parent restarts it in the surviving zygote.
static volatile bool g_reassert_stop = false;

void SigchainStartReassert();  // fwd decl (defined below)

static void* sigchain_reassert_thread(void*) {
  // Keep ART's fault handlers on top of anything libdfx installs later. Tight loop
  // (2 ms) to minimize the race window where libdfx's handler is on top when a
  // GC-suspend-check SEGV fires (each GC re-arms the trigger; missing one = exit).
  for (;;) {
    if (g_reassert_stop) break;  // stop for zygote prefork (single-thread requirement)
    for (int s = 1; s < NSIG; ++s) {
      if (g_art_have[s]) {
        sigchain_install_capture(s);   // §631: also learns libdfx's displaced handler
      }
    }
    usleep(2000);  // 2 ms
  }
  g_reassert_started = false;  // mark exited so it can be restarted post-fork
  return nullptr;
}

// Called from Runtime::PreZygoteFork() BEFORE its single-threaded check. Signals the
// re-assert thread to exit and waits for it to actually leave (so the kernel thread
// count drops to 1). No-op if the thread isn't running.
void SigchainStopReassert() {
  if (!g_reassert_started) return;
  g_reassert_stop = true;
  for (int i = 0; i < 250 && g_reassert_started; ++i) {
    usleep(2000);  // wait up to ~500 ms for the thread to observe the flag and exit
  }
  g_reassert_stop = false;  // reset so a future thread runs normally
}

static void sigchain_reinstall_all() {
  for (int s = 1; s < NSIG; ++s) {
    if (g_art_have[s]) sigchain_install_capture(s);
  }
}

// 2026-07-09: the re-assert thread does NOT survive fork(), and appspawn-x's spawn
// path forks a child. Register pthread_atfork handlers so ART's fault handlers stay
// installed on top of libdfx across fork: the parent re-installs immediately (closing
// the window during zygotePostForkCommon), and the child re-installs AND restarts its
// own re-assert thread (so the forked app process is protected too).
static void atfork_parent() {
  // PreZygoteFork stopped the re-assert thread to fork single-threaded. Re-install
  // ART's handlers immediately. Do NOT restart the re-assert thread here: atfork_parent
  // also runs when a *child* forks a grandchild (child_main.cpp:299), which would start
  // the thread in the child BEFORE its SELinux setcon → multi-threaded → setcon EPERM
  // (HapDomainSetcontext ret=-7). The child restarts its own thread after setcon
  // (child_main.cpp:193); the parent zygote is idle between spawns and relies on this
  // one-shot re-install plus the per-fork SigchainStopReassert/reinstall cycle.
  sigchain_reinstall_all();
}
static void atfork_child() {
  // Only RE-INSTALL the handler (single-threaded-safe). Do NOT start a thread here:
  // the child must stay single-threaded until its SELinux setcon completes, else the
  // kernel rejects setcon (EPERM). The child restarts its re-assert thread later, via
  // SigchainStartReassert() called from child_main after postForkChild/setcon.
  sigchain_reinstall_all();
  g_reassert_started = false;
}

// Called from appspawn-x child_main AFTER postForkChild/setcon (child is now allowed
// to be multi-threaded) to restart the re-assert thread in the forked app process.
void SigchainStartReassert() {
  sigchain_reinstall_all();
  if (!g_reassert_started) {
    g_reassert_started = true;
    pthread_t t;
    if (pthread_create(&t, nullptr, sigchain_reassert_thread, nullptr) == 0) {
      pthread_detach(t);
      char m[] = "[SIGCHAIN] child re-assert thread started\n";
      ssize_t w = write(2, m, sizeof(m) - 1); (void)w;
    }
  }
}

void EnsureFrontOfChain(int signal) {
  // Re-assert ART's handler for this signal now (best-effort front-of-chain).
  if (signal > 0 && signal < NSIG && g_art_have[signal]) {
    sigchain_install_capture(signal);
  }
}

void AddSpecialSignalHandlerFn(int signal, void* sa) {
  if (!sa || signal <= 0 || signal >= NSIG) return;
  SigchainAction* sca = reinterpret_cast<SigchainAction*>(sa);

  // §631: route through sigchain_trampoline so sc_sigaction's bool is honoured.
  if (g_special_count[signal] < kMaxSpecial) {
    g_special[signal][g_special_count[signal]] = *sca;
    g_special_count[signal]++;
  }

  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_sigaction = sigchain_trampoline;
  act.sa_mask = sca->sc_mask;
  act.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;

  g_art_sa[signal] = act;
  g_art_have[signal] = true;
  sigchain_install_capture(signal);

  fprintf(stderr, "[SIGCHAIN] installed+tracking ART handler for signal %d\n", signal);
  fflush(stderr);

  if (!g_reassert_started) {
    g_reassert_started = true;
    pthread_atfork(nullptr, atfork_parent, atfork_child);
    pthread_t t;
    if (pthread_create(&t, nullptr, sigchain_reassert_thread, nullptr) == 0) {
      pthread_detach(t);
      fprintf(stderr, "[SIGCHAIN] re-assert thread started + atfork registered\n");
      fflush(stderr);
    }
  }
}

void RemoveSpecialSignalHandlerFn(int signal, bool (*fn)(int, siginfo_t*, void*)) {
  if (signal > 0 && signal < NSIG) {
    // §631: drop just this handler; only stop re-asserting when none are left.
    for (int i = 0; i < g_special_count[signal]; ++i) {
      if (g_special[signal][i].sc_sigaction == fn) {
        for (int j = i; j + 1 < g_special_count[signal]; ++j) {
          g_special[signal][j] = g_special[signal][j + 1];
        }
        g_special_count[signal]--;
        break;
      }
    }
    if (g_special_count[signal] == 0) g_art_have[signal] = false;
  }
  (void)fn;
}

void InitializeSignalChain() {
  // Nothing to initialize in static builds.
}

void SkipAddSignalHandler(bool skip) {
  (void)skip;
}

}  // extern "C"
