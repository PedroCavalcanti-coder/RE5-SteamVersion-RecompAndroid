/*
 * crash.c - diagnóstico de falhas do host causadas pelo guest (etapa 3.8).
 *
 * Um ponteiro ruim do guest vira um acesso inválido no host, dentro da reserva
 * de 4 GiB. Sem isto o processo some sem dizer nada ("abre e fecha"). Com isto
 * sai: o tipo da falha, o endereço traduzido para VA do guest, os registradores
 * do guest da thread que falhou e a trilha de dispatch dela - no stderr e no
 * RE5_LOG_FILE.
 */
#include <stdio.h>
#include <stdlib.h>

#include "re5/runtime.h"

static void report(const char *what, uintptr_t host_addr, int has_addr)
{
    if (has_addr && host_addr >= re5_guest_base && host_addr - re5_guest_base < ((uint64_t)1 << 32)) {
        uint32_t va = (uint32_t)(host_addr - re5_guest_base);
        re5_mem_region r;
        int mapped = re5_mem_query(va, &r) == 0;
        RE5_ERROR("crash", "%s no endereço do guest 0x%08X (%s%s)", what, va,
                  mapped ? "região " : "fora de qualquer região mapeada", mapped ? r.tag : "");
    } else if (has_addr) {
        RE5_ERROR("crash", "%s no endereço do host %p (fora do espaço do guest: bug do runtime/HLE)", what,
                  (void *)host_addr);
    } else {
        RE5_ERROR("crash", "%s", what);
    }
    re5_thread *t = re5_thread_current();
    if (t) {
        CPU *c = &t->cpu;
        RE5_ERROR("crash", "thread 0x%X: eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X ebp=%08X esp=%08X",
                  t->tid, c->eax, c->ebx, c->ecx, c->edx, c->esi, c->edi, c->ebp, c->esp);
        for (int i = 0; i < 8; i++) {
            uint32_t a = c->esp + 4u * (uint32_t)i;
            if (!re5_mem_is_mapped(a, 4)) break;
            RE5_ERROR("crash", "  [esp+%02X] = %08X", 4 * i, rd32(a));
        }
    }
    re5_dispatch_trail_print();
}

#if defined(RE5_HOST_WINDOWS)
#include <windows.h>

/* Função gerada que contém o endereço do host `pc`: a de maior endereço <= pc. A
 * tabela do dispatch é ordenada por VA do guest, não por endereço do host. */
static uint32_t guest_fn_at(uintptr_t pc)
{
    uintptr_t best = 0;
    uint32_t va = 0;
    for (uint32_t i = 0; i < re5_lifted_count; i++) {
        uintptr_t f = (uintptr_t)re5_lifted_table[i].fn;
        if (f <= pc && f > best) best = f, va = re5_lifted_table[i].va;
    }
    return pc - best < 0x100000u ? va : 0;
}

/* Estouro da pilha do host: o código gerado aninha uma chamada C por call (e por
 * jmp para outra função) do guest, então a pilha do host mostra o ciclo. Desenrola
 * a partir do contexto da falha e traduz cada quadro para a função do guest. */
static void report_host_stack(const CONTEXT *fault)
{
    CONTEXT ctx = *fault;
    RE5_ERROR("crash", "pilha do host (topo primeiro, função do guest de cada quadro):");
    uint32_t last = 0, reps = 0;
    for (int n = 0; n < 96 && ctx.Rip; n++) {
        uint32_t va = guest_fn_at((uintptr_t)ctx.Rip);
        if (va && va == last) {
            reps++;
        } else {
            if (reps) RE5_ERROR("crash", "    (mais %u vezes)", reps);
            reps = 0;
            last = va;
            if (va) RE5_ERROR("crash", "  #%02d L_%08X", n, va);
            else    RE5_ERROR("crash", "  #%02d host %p", n, (void *)(uintptr_t)ctx.Rip);
        }
        DWORD64 base = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
        if (f) {
            PVOID hd = NULL;
            DWORD64 est = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, f, &ctx, &hd, &est, NULL);
        } else {                       /* folha sem unwind info: o topo é o retorno */
            ctx.Rip = *(DWORD64 *)(uintptr_t)ctx.Rsp;
            ctx.Rsp += 8;
        }
    }
    if (reps) RE5_ERROR("crash", "    (mais %u vezes)", reps);
}

static LONG WINAPI on_unhandled(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    char what[256];
    int has_addr = 0;
    uintptr_t addr = 0;
    switch (er->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
        has_addr = er->NumberParameters >= 2;
        addr = has_addr ? (uintptr_t)er->ExceptionInformation[1] : 0;
        snprintf(what, sizeof what, "acesso inválido (%s)",
                 er->ExceptionInformation[0] == 0 ? "leitura" : er->ExceptionInformation[0] == 1 ? "escrita" : "execução");
        break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO: snprintf(what, sizeof what, "divisão inteira por zero"); break;
    case EXCEPTION_INT_OVERFLOW: snprintf(what, sizeof what, "estouro em divisão inteira"); break;
    case EXCEPTION_STACK_OVERFLOW: snprintf(what, sizeof what, "estouro da pilha do host (recursão do guest?)"); break;
    case EXCEPTION_ILLEGAL_INSTRUCTION: snprintf(what, sizeof what, "instrução ilegal no host"); break;
    /* "\x20GCC": throw do C++ do MinGW sem catch, numa DLL do host. A DXVK faz
     * isso no Direct3DCreate9 quando nenhuma GPU tem o Vulkan que ela exige. */
    case 0x20474343u:
        snprintf(what, sizeof what, "exceção C++ sem tratamento numa DLL do host (DXVK sem GPU compatível? "
                 "veja re5_d3d9.log; RE5_D3D9_DLL=d3d9.dll usa o Direct3D do sistema)");
        break;
    default: snprintf(what, sizeof what, "exceção 0x%08lX no host", (unsigned long)er->ExceptionCode); break;
    }
    report(what, addr, has_addr);
    RE5_ERROR("crash", "código do host em %p", er->ExceptionAddress);
    if (er->ExceptionCode == EXCEPTION_STACK_OVERFLOW) report_host_stack(ep->ContextRecord);
    fflush(NULL);
    return EXCEPTION_EXECUTE_HANDLER;
}

void re5_crash_handler_install(void) { SetUnhandledExceptionFilter(on_unhandled); }

#else
#include <signal.h>
#include <string.h>

static void on_signal(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    const char *what = sig == SIGSEGV ? "acesso inválido (SIGSEGV)" : sig == SIGBUS ? "acesso inválido (SIGBUS)"
                     : sig == SIGFPE ? "erro aritmético (SIGFPE)" : "instrução ilegal (SIGILL)";
    report(what, (uintptr_t)si->si_addr, sig == SIGSEGV || sig == SIGBUS);
    fflush(NULL);
    signal(sig, SIG_DFL);
    raise(sig);
}

void re5_crash_handler_install(void)
{
    static char alt[64 * 1024];   /* estouro de pilha: o handler precisa de outra */
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt, .ss_flags = 0};
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
}
#endif
