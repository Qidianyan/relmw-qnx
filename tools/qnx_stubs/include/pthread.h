// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// QNX 8.0 cluster API (C Library Reference: pthread_cluster_set(), pthread_cluster_set_and_inherit(),
// pthread_cluster_get_runmask(); Programmer's Guide "Processor affinity, clusters, runmasks, and inherit masks").
#pragma once
#include_next <pthread.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int pthread_cluster_set(pthread_t tid, char const* name);
int pthread_cluster_set_and_inherit(pthread_t tid, char const* name, char const* inherit_name);
int pthread_cluster_get_runmask(char const* name, uint64_t* runmask);
#ifdef __cplusplus
}
#endif
