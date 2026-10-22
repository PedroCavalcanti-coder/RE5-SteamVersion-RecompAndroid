/*
 * platform.h - macros de compilador e de plataforma do runtime.
 *
 * O runtime compila com GCC, Clang (Linux, Android NDK) e MSVC/clang-cl
 * (Windows). Tudo que difere entre eles fica aqui, para o resto do código não
 * ter #ifdef de compilador.
 */
#ifndef RE5_PLATFORM_H
#define RE5_PLATFORM_H

#if defined(_MSC_VER) && !defined(__clang__)
#define RE5_NORETURN       __declspec(noreturn)
#define RE5_THREAD_LOCAL   __declspec(thread)
#define RE5_PRINTF(f, a)
#else
#define RE5_NORETURN       __attribute__((noreturn))
#define RE5_THREAD_LOCAL   _Thread_local
#define RE5_PRINTF(f, a)   __attribute__((format(printf, f, a)))
#endif

#if defined(_WIN32)
#define RE5_HOST_WINDOWS 1
#elif defined(__ANDROID__)
#define RE5_HOST_ANDROID 1
#define RE5_HOST_POSIX   1
#else
#define RE5_HOST_POSIX   1
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#define RE5_HOST_ARM64 1   /* ordenação de memória fraca: ver docs/ARQUITETURA.md, D5 */
#endif

#endif /* RE5_PLATFORM_H */
