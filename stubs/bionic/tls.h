/* Bionic TLS stub for ART compilation.
 * ART accesses thread-local storage via bionic's TLS slot mechanism.
 * We stub it to use __thread variables instead. */
#ifndef BIONIC_TLS_H
#define BIONIC_TLS_H

/* TLS slot indices used by ART */
#define TLS_SLOT_ART_THREAD_SELF 7

/* On real bionic, __get_tls() returns the TLS area.
 * We provide a stub that works for static binaries. */
static inline void** __get_tls(void) {
#if defined(__aarch64__)
    void** tls;
    __asm__("mrs %0, tpidr_el0" : "=r"(tls));
    return tls;
#else
    return (void**)0;
#endif
}

#endif
