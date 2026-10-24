// Minimal runtime pieces for building without the MinGW CRT (the C library itself is msvcrt.dll).
// Not needed for a Visual Studio build.
#ifndef _MSC_VER
#include <stdarg.h>
#include <stddef.h>

extern "C" {
int __cdecl _vsnprintf(char*, size_t, const char*, va_list);
int __cdecl _vscprintf(const char*, va_list);

// C99-style vsnprintf on top of msvcrt: always terminates, returns the full length.
int __cdecl __ms_vsnprintf(char* buf, size_t n, const char* fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int full = _vscprintf(fmt, ap2);
    va_end(ap2);
    if (buf && n) {
        int r = _vsnprintf(buf, n, fmt, ap);
        if (r < 0 || (size_t)r >= n) buf[n - 1] = 0;
    }
    return full;
}

// Stack probe used by functions with large frames (same as libgcc's ___chkstk_ms).
__asm__(
    ".globl ___chkstk_ms\n"
    "___chkstk_ms:\n"
    "    pushq %rcx\n"
    "    pushq %rax\n"
    "    cmpq $0x1000, %rax\n"
    "    leaq 24(%rsp), %rcx\n"
    "    jb 2f\n"
    "1:  subq $0x1000, %rcx\n"
    "    orq $0, (%rcx)\n"
    "    subq $0x1000, %rax\n"
    "    cmpq $0x1000, %rax\n"
    "    ja 1b\n"
    "2:  subq %rax, %rcx\n"
    "    orq $0, (%rcx)\n"
    "    popq %rax\n"
    "    popq %rcx\n"
    "    ret\n");
}
#endif
