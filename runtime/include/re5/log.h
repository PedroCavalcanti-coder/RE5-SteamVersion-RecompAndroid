/*
 * log.h - log do runtime.
 *
 * Nível pela variável de ambiente RE5_LOG (error, warn, info, debug, trace;
 * padrão info). Saída em stderr e, se RE5_LOG_FILE estiver definida, também
 * nesse arquivo. Cada linha leva o canal ("mem", "dispatch", "kernel32"...),
 * para filtrar com grep sem precisar de níveis por canal.
 */
#ifndef RE5_LOG_H
#define RE5_LOG_H

#include <stdint.h>
#include "re5/platform.h"

typedef enum {
    RE5_LOG_ERROR = 0,
    RE5_LOG_WARN  = 1,
    RE5_LOG_INFO  = 2,
    RE5_LOG_DEBUG = 3,
    RE5_LOG_TRACE = 4,
} re5_log_level;

void re5_log_init(void);
void re5_log_set_level(re5_log_level level);
int  re5_log_enabled(re5_log_level level);
void re5_log(re5_log_level level, const char *channel, const char *fmt, ...) RE5_PRINTF(3, 4);

/* Erro irrecuperável: registra, imprime a trilha de dispatch da thread e aborta. */
RE5_NORETURN void re5_fatal(const char *channel, const char *fmt, ...) RE5_PRINTF(2, 3);

#define RE5_ERROR(ch, ...) re5_log(RE5_LOG_ERROR, ch, __VA_ARGS__)
#define RE5_WARN(ch, ...)  re5_log(RE5_LOG_WARN,  ch, __VA_ARGS__)
#define RE5_INFO(ch, ...)  re5_log(RE5_LOG_INFO,  ch, __VA_ARGS__)
#define RE5_DEBUG(ch, ...) do { if (re5_log_enabled(RE5_LOG_DEBUG)) re5_log(RE5_LOG_DEBUG, ch, __VA_ARGS__); } while (0)
#define RE5_TRACE(ch, ...) do { if (re5_log_enabled(RE5_LOG_TRACE)) re5_log(RE5_LOG_TRACE, ch, __VA_ARGS__); } while (0)

#endif /* RE5_LOG_H */
