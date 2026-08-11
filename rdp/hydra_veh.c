/*
 * hydra_veh.c — name the caller of the null function pointer.
 *
 * PROBLEM 2 is "0xC0000005 at address 0, on a channel thread, no frames of
 * ours".  Address 0 means the CPU executed `call [something]` where something
 * was NULL.  The `call` already pushed its return address, so at the moment of
 * the fault [RSP] points at the instruction AFTER the call, inside whichever
 * libfreerdp3 function made it.  That single number identifies the missing
 * callback with no hypothesis at all.
 *
 * A vectored handler is used rather than SetUnhandledExceptionFilter because
 * it runs first-chance, before anything can swallow or rewrite the context,
 * and because it returns CONTINUE_SEARCH so the existing disconnect-on-crash
 * filter (PROBLEM 5 mitigation) still runs afterwards.
 *
 * Wire-up: call hydra_install_veh() as the first statement in main(), before
 * any FreeRDP call.
 *
 *   MSVC   : cl /c hydra_veh.c
 *   MinGW  : gcc -c hydra_veh.c
 *
 * x86-64 only (reads Rsp/Rip). If you ever build ARM64, the field names change.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#ifndef HYDRA_VEH_LOG
#define HYDRA_VEH_LOG "C:\\ProgramData\\Hydra\\logs\\gfx_crash.txt"
#endif

/* How many stack slots above RSP to resolve. The return address is [rsp+0];
 * the rest catch tail-call and thunk cases. */
#define HYDRA_VEH_SLOTS 24

static int hydra_readable(const void *p, SIZE_T n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi)))            return 0;
    if (mbi.State != MEM_COMMIT)                        return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))     return 0;
    return ((const BYTE *)p + n) <= ((const BYTE *)mbi.BaseAddress + mbi.RegionSize);
}

/* Resolve an address to module+RVA. Returns 1 if it landed in a module. */
static int hydra_describe(FILE *f, const char *tag, ULONG_PTR v)
{
    HMODULE mod = NULL;
    char    path[MAX_PATH];
    const char *base;

    if (v < 0x10000) return 0;

    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)v, &mod) || !mod)
        return 0;

    if (!GetModuleFileNameA(mod, path, MAX_PATH)) return 0;

    base = strrchr(path, '\\');
    base = base ? base + 1 : path;

    fprintf(f, "  %-12s %016llX   %s + 0x%llX\n",
            tag,
            (unsigned long long)v,
            base,
            (unsigned long long)(v - (ULONG_PTR)mod));
    return 1;
}

static LONG CALLBACK hydra_veh(PEXCEPTION_POINTERS ep)
{
    static volatile LONG once = 0;
    EXCEPTION_RECORD *er;
    CONTEXT          *cx;
    ULONG_PTR        *sp;
    SYSTEMTIME        st;
    FILE             *f;
    int               i;

    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;

    er = ep->ExceptionRecord;
    cx = ep->ContextRecord;

    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

    /* Only the first one. A second pass through here during unwind would
     * append noise on top of the interesting record. */
    if (InterlockedCompareExchange(&once, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    f = fopen(HYDRA_VEH_LOG, "a");
    if (!f) return EXCEPTION_CONTINUE_SEARCH;

    GetLocalTime(&st);
    fprintf(f, "\n================================================================\n");
    fprintf(f, "ACCESS VIOLATION  %04u-%02u-%02u %02u:%02u:%02u.%03u  tid=%lu\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds, GetCurrentThreadId());
    fprintf(f, "  fault addr  %016llX   (%s)\n",
            (unsigned long long)(ULONG_PTR)er->ExceptionAddress,
            ((ULONG_PTR)er->ExceptionAddress < 0x10000)
                ? "NULL FUNCTION POINTER -- read [rsp+00] below"
                : "code address");
    if (er->NumberParameters >= 2)
        fprintf(f, "  operation   %llu (0=read 1=write 8=exec)  at %016llX\n",
                (unsigned long long)er->ExceptionInformation[0],
                (unsigned long long)er->ExceptionInformation[1]);

#if defined(_M_X64) || defined(__x86_64__)
    fprintf(f, "  rip=%016llX rsp=%016llX rbp=%016llX\n",
            (unsigned long long)cx->Rip, (unsigned long long)cx->Rsp,
            (unsigned long long)cx->Rbp);
    fprintf(f, "  rax=%016llX rcx=%016llX rdx=%016llX rbx=%016llX\n",
            (unsigned long long)cx->Rax, (unsigned long long)cx->Rcx,
            (unsigned long long)cx->Rdx, (unsigned long long)cx->Rbx);
    fprintf(f, "  rsi=%016llX rdi=%016llX r8 =%016llX r9 =%016llX\n",
            (unsigned long long)cx->Rsi, (unsigned long long)cx->Rdi,
            (unsigned long long)cx->R8,  (unsigned long long)cx->R9);
    fprintf(f, "  r10=%016llX r11=%016llX r12=%016llX r13=%016llX\n",
            (unsigned long long)cx->R10, (unsigned long long)cx->R11,
            (unsigned long long)cx->R12, (unsigned long long)cx->R13);

    fprintf(f, "\n  -- stack words that resolve to a module ---------------------\n");
    fprintf(f, "  THE FIRST ONE IS THE CALLER. Subtract 5 from its RVA to land\n"
               "  on the call instruction, then disassemble there.\n\n");

    sp = (ULONG_PTR *)cx->Rsp;
    for (i = 0; i < HYDRA_VEH_SLOTS; i++) {
        char tag[16];
        if (!hydra_readable(&sp[i], sizeof(ULONG_PTR))) break;
        sprintf(tag, "[rsp+%02X]", (unsigned)(i * 8));
        hydra_describe(f, tag, sp[i]);
    }

    /* rcx is the first integer argument in the MS x64 ABI. For an indirect
     * call through a context struct it usually still holds that struct, so
     * this tells you WHICH object had the null slot. */
    fprintf(f, "\n  -- rcx as a candidate object -------------------------------\n");
    if (hydra_readable((void *)cx->Rcx, 8 * 48)) {
        ULONG_PTR *o = (ULONG_PTR *)cx->Rcx;
        for (i = 0; i < 48; i++)
            fprintf(f, "  +0x%03X  %016llX%s\n", (unsigned)(i * 8),
                    (unsigned long long)o[i], o[i] ? "" : "   <-- NULL");
    } else {
        fprintf(f, "  rcx not readable as an object\n");
    }
#else
    fprintf(f, "  (non-x64 build: register dump omitted)\n");
    (void)sp; (void)i;
#endif

    fprintf(f, "\n  -- loaded modules ------------------------------------------\n");
    fflush(f);
    fclose(f);

    return EXCEPTION_CONTINUE_SEARCH;   /* let the existing filter still run */
}

void hydra_install_veh(void)
{
    AddVectoredExceptionHandler(1 /* first */, hydra_veh);
}
