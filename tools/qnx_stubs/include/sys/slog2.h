#pragma once
#include <stdarg.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define SLOG2_MAX_BUFFERS 4
#define SLOG2_SHUTDOWN 0
#define SLOG2_CRITICAL 1
#define SLOG2_ERROR 2
#define SLOG2_WARNING 3
#define SLOG2_NOTICE 4
#define SLOG2_INFO 5
#define SLOG2_DEBUG1 6
#define SLOG2_DEBUG2 7
#define SLOG2_DISCARD_NEWLINE 0x00000400u
typedef void* slog2_buffer_t;
typedef struct { const char* buffer_name; int num_pages; } slog2_buffer_config_t;
typedef struct {
  int num_buffers; const char* buffer_set_name; uint8_t verbosity_level;
  slog2_buffer_config_t buffer_config[SLOG2_MAX_BUFFERS]; uint32_t max_retries;
} slog2_buffer_set_config_t;
int slog2_register(slog2_buffer_set_config_t* config, slog2_buffer_t* handles, uint32_t flags);
int slog2f(slog2_buffer_t buffer, uint16_t code, uint8_t severity, const char* format, ...) __attribute__((format(printf, 4, 5)));
int vslog2f(slog2_buffer_t buffer, uint16_t code, uint8_t severity, const char* format, va_list arg);
int slog2c(slog2_buffer_t buffer, uint16_t code, uint8_t severity, const char* data);
#ifdef __cplusplus
}
#endif
