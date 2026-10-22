/*
 * check.h - asserções mínimas para os testes em C (sem dependências).
 * Cada teste é um executável; CTest considera falha qualquer saída != 0.
 */
#ifndef RE5_TEST_CHECK_H
#define RE5_TEST_CHECK_H

#include <stdio.h>
#include <stdlib.h>

static int check_failures;

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "%s:%d: falhou: %s\n", __FILE__, __LINE__, #cond); check_failures++; } \
} while (0)

#define CHECK_EQ_U32(a, b) do { \
    unsigned long long _a = (unsigned long long)(a), _b = (unsigned long long)(b); \
    if (_a != _b) { fprintf(stderr, "%s:%d: falhou: %s == %s (0x%llX != 0x%llX)\n", \
                            __FILE__, __LINE__, #a, #b, _a, _b); check_failures++; } \
} while (0)

#define CHECK_DONE() do { \
    if (check_failures) { fprintf(stderr, "%d verificação(ões) falharam\n", check_failures); return 1; } \
    printf("ok\n"); return 0; \
} while (0)

#endif /* RE5_TEST_CHECK_H */
