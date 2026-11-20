#include "log.h"

#include "config.h"

#include <stdarg.h>
#include <stdio.h>

void afmf_log(enum afmf_log_level level, const char *fmt, ...)
{
    static const char *const names[] = {"error", "warn", "info", "debug"};

    if ((int)level > afmf_config_get()->log_level)
        return;

    va_list ap;
    va_start(ap, fmt);
    flockfile(stderr);
    (void)fprintf(stderr, "[AFMF %s] ", names[level]);
    (void)vfprintf(stderr, fmt, ap);
    (void)fputc('\n', stderr);
    funlockfile(stderr);
    va_end(ap);
}
