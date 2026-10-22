/*
 * Dispatch, thunks e chamadas guest <-> host, com funções escritas no mesmo
 * formato que o lift32_cpu gera (call = push retorno + dispatch; ret N =
 * esp += 4+N). Cobre: cdecl, stdcall do guest, thunk stdcall do host (a
 * limpeza da pilha vem do registro), import não implementado e GetProcAddress.
 */
#include "check.h"
#include "re5/runtime.h"

#define VA_ADD_CDECL    0x00401000u   /* int add(a, b)                         */
#define VA_CALLS_HOST   0x00401100u   /* int __stdcall f(a, b) { return mul(a, b) + 1; } */
#define VA_MUL_THUNK_AT 0x00500000u   /* "IAT" onde o loader poria o thunk     */

static void L_00401000(CPU *c)
{
    c->eax = rd32(c->esp + 4u);
    c->eax = flags_add32(c, c->eax, rd32(c->esp + 8u));
    c->esp += 4; return;
}

static void L_00401100(CPU *c)
{
    push32(c, rd32(c->esp + 8u));                      /* push b (esp sobe 4 a cada push) */
    push32(c, rd32(c->esp + 8u));                      /* push a */
    { uint32_t _ct = rd32(VA_MUL_THUNK_AT); push32(c, 0x0040110Du); dispatch(c, _ct); }
    c->eax = flags_add32(c, c->eax, 1u);
    c->esp += 12; return;                              /* ret 8 */
}

const re5_func_entry re5_lifted_table[] = {
    {VA_ADD_CDECL, L_00401000},
    {VA_CALLS_HOST, L_00401100},
};
const uint32_t re5_lifted_count = 2;

static void host_mul(CPU *c) { c->eax = re5_arg(c, 0) * re5_arg(c, 1); }

int main(void)
{
    CHECK(re5_runtime_init() == 0);
    CHECK(re5_mem_map_fixed(0x00400000u, 0x200000u, RE5_PAGE_READWRITE, "imagem") == 0);
    CHECK(re5_process_init(0x00400000u) == 0);
    re5_thread *t = re5_thread_attach(1u << 20);
    CHECK(t != NULL);
    CHECK_EQ_U32(t->cpu.fs_base, t->teb);
    CHECK_EQ_U32(FS_RD32(&t->cpu, RE5_TEB_SELF), t->teb);           /* fs:[0x18] pelo caminho do cpu.h */
    CHECK_EQ_U32(FS_RD32(&t->cpu, RE5_TEB_PEB), re5_process_peb());

    CHECK(re5_lookup(VA_ADD_CDECL) == L_00401000);
    CHECK(re5_lookup(VA_ADD_CDECL + 1) == NULL);

    uint32_t esp0 = t->cpu.esp;
    uint32_t args[2] = {40, 2};
    CHECK_EQ_U32(re5_guest_call(t, VA_ADD_CDECL, 2, args), 42);
    CHECK_EQ_U32(t->cpu.esp, esp0);

    /* Guest stdcall chamando um thunk stdcall do host pela "IAT". */
    uint32_t mul = re5_thunk_new("teste!mul", host_mul, 8, NULL);
    CHECK(re5_is_thunk(mul));
    CHECK(re5_thunk_get(mul) != NULL && re5_thunk_get(mul + 1) == NULL);
    wr32(VA_MUL_THUNK_AT, mul);
    uint32_t args2[2] = {6, 7};
    CHECK_EQ_U32(re5_guest_call(t, VA_CALLS_HOST, 2, args2), 43);
    CHECK_EQ_U32(t->cpu.esp, esp0);

    /* Chamada direta de um thunk pelo host (o caminho de uma vtable COM). */
    CHECK_EQ_U32(re5_guest_call(t, mul, 2, args2), 42);
    CHECK_EQ_U32(t->cpu.esp, esp0);

    /* Registro de imports: implementado, não implementado, GetProcAddress. */
    uint32_t tick = re5_import_bind("KERNEL32.dll", "GetTickCount", 0);
    uint32_t nope = re5_import_bind("kernel32.dll", "FunçãoQueNãoExiste", 0);
    CHECK(re5_is_thunk(tick) && re5_is_thunk(nope) && tick != nope);
    CHECK_EQ_U32(re5_import_bind("kernel32", "GetTickCount", 0), tick);   /* mesma função, mesmo thunk */
    CHECK_EQ_U32(re5_import_lookup("kernel32.dll", "GetTickCount", 0), tick);
    CHECK_EQ_U32(re5_import_lookup("kernel32.dll", "FunçãoQueAindaNãoFoiLigada", 0), 0);
    re5_census cs = re5_import_census();
    CHECK_EQ_U32(cs.bound, 2);
    CHECK_EQ_U32(cs.implemented, 1);

    /* cpuid/rdtsc que o código gerado chama: Core 2 com SSE2 e sem SSE3. */
    CPU q = {0};
    recomp_cpuid(&q);
    CHECK_EQ_U32(q.ebx, 0x756E6547u);   /* "Genu" */
    q.eax = 1;
    recomp_cpuid(&q);
    CHECK((q.edx >> 26) & 1);
    CHECK_EQ_U32(q.ecx & 1, 0);
    q.eax = 0x80000002u;
    recomp_cpuid(&q);
    CHECK_EQ_U32(q.eax, 0x65746E49u);   /* "Inte" */
    recomp_rdtsc(&q);
    uint64_t t0 = (uint64_t)q.edx << 32 | q.eax;
    recomp_rdtsc(&q);
    CHECK(((uint64_t)q.edx << 32 | q.eax) >= t0);

    /* SetLastError/GetLastError do HLE passam pelo TEB. */
    uint32_t err = 1234;
    re5_guest_call(t, re5_import_bind("kernel32.dll", "SetLastError", 0), 1, &err);
    CHECK_EQ_U32(FS_RD32(&t->cpu, RE5_TEB_LAST_ERROR), 1234);
    CHECK_EQ_U32(re5_guest_call(t, re5_import_bind("kernel32.dll", "GetLastError", 0), 0, NULL), 1234);
    CHECK_EQ_U32(t->cpu.esp, esp0);

    CHECK_DONE();
}
