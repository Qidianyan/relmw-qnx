#pragma once
#include_next <time.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void nsec2timespec(struct timespec* timespec_p, uint64_t nsec);
#ifdef __cplusplus
}
#endif
