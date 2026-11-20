#pragma once

#if defined(__GNUC__)
#define AFMF_PRINTF(fmt_index, first_arg) __attribute__((format(printf, fmt_index, first_arg)))
#else
#define AFMF_PRINTF(fmt_index, first_arg)
#endif

enum afmf_log_level {
    AFMF_LOG_ERROR = 0,
    AFMF_LOG_WARN = 1,
    AFMF_LOG_INFO = 2,
    AFMF_LOG_DEBUG = 3,
};

/* Writes one line to stderr when `level` is enabled by AFMF_LOG. Safe from any thread. */
void afmf_log(enum afmf_log_level level, const char *fmt, ...) AFMF_PRINTF(2, 3);

#define AFMF_ERR(...) afmf_log(AFMF_LOG_ERROR, __VA_ARGS__)
#define AFMF_WARN(...) afmf_log(AFMF_LOG_WARN, __VA_ARGS__)
#define AFMF_INFO(...) afmf_log(AFMF_LOG_INFO, __VA_ARGS__)
#define AFMF_DEBUG(...) afmf_log(AFMF_LOG_DEBUG, __VA_ARGS__)
