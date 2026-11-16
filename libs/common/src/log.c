#define CAMSTREAM_LOG_MODULE "common"
#include <camstream/common/log.h>

#include <stdarg.h>
#include <stdio.h>

void camstream_log_write(const char* module, camstream_log_level level, const char* format, ...)
{
    static const char* const names[] = {"ERROR", "WARN", "INFO", "DEBUG"};
    const char* name = (unsigned)level < sizeof(names) / sizeof(names[0]) ? names[level] : "UNKNOWN";
    va_list arguments;
    va_start(arguments, format);
    (void)fprintf(stderr, "camstream-%s %s: ", module, name);
    (void)vfprintf(stderr, format, arguments);
    (void)fputc('\n', stderr);
    va_end(arguments);
}
