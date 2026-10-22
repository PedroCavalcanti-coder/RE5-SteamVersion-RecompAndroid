/*
 * fx.c - executável PE32 sintético para o teste de ponta a ponta do pipeline.
 *
 * Compilado por tests/e2e/build_fixture.py com clang --target=i686-pc-windows-msvc
 * e lld-link (base fixa 0x00400000, sem relocações, como o re5dx9.exe). Exercita
 * o que o jogo real exige do pipeline: cdecl, stdcall, convenção interna do
 * otimizador, switch com tabela de saltos, globais, imports do kernel32 (com e
 * sem argumentos) e acesso a fs: (TEB).
 *
 * entry() deve retornar 555. Se mudar este arquivo, recalcule e atualize
 * EXPECTED em test_mini_pe.c.
 */
__declspec(dllimport) unsigned long __stdcall GetTickCount(void);
__declspec(dllimport) void __stdcall Sleep(unsigned long ms);
__declspec(dllimport) void __stdcall SetLastError(unsigned long err);
__declspec(dllimport) unsigned long __stdcall GetLastError(void);

int g_counter = 5;

static int square(int x) { return x * x; }

int __cdecl sum_squares(int n)
{
    int s = 0;
    for (int i = 1; i <= n; i++) s += square(i);
    return s;
}

int __stdcall add3(int a, int b, int c) { return a + b + c; }

/* Casos com operações diferentes: o clang gera tabela de saltos, não de valores. */
int __cdecl classify(int k)
{
    switch (k) {
    case 0: return k + g_counter;
    case 1: return k * g_counter;
    case 2: return g_counter - k;
    case 3: return g_counter << k;
    case 4: return g_counter ^ k;
    case 5: return g_counter / (k - 3);
    default: return -1;
    }
}

static unsigned teb_self(void)
{
    unsigned v;
    __asm__ volatile("movl %%fs:0x18, %0" : "=r"(v));
    return v;
}

int entry(void)
{
    g_counter += sum_squares(4) + add3(1, 2, 3);       /* 5 + 30 + 6 = 41 */
    Sleep(0);
    SetLastError(1234);
    int r = g_counter;
    if (GetLastError() != 1234) r += 1000;
    if (teb_self() == 0) r += 2000;
    for (int k = 0; k < 6; k++) r += classify(k);      /* 41+41+39+328+45+20 = 514 */
    return r + (int)(GetTickCount() & 0);              /* 41 + 514 = 555 */
}
