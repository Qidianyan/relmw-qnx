#pragma once
#include <stdint.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct _ham_entity ham_entity_t;
typedef struct _ham_condition ham_condition_t;
typedef struct _ham_action ham_action_t;
enum { CONDDEATH = 1, CONDABNORMALDEATH, CONDDETACH, CONDATTACH, CONDHBEATMISSEDLOW, CONDHBEATMISSEDHIGH, CONDRESTART, CONDANY };
#define HREARMAFTERRESTART 0x00000001u
#define HCONDINDEPENDENT 0x00000002u
#define HCONDNOWAIT 0x00000004u
#define HENTITYKEEPONDEATH 0x00000008u
#define HACTIONBREAKONFAIL 0x00000010u
#define HACTIONKEEPONFAIL 0x00000020u
int ham_connect(unsigned flags);
int ham_disconnect(unsigned flags);
ham_entity_t* ham_attach(const char* ename, int reserved, pid_t pid, const char* line, unsigned flags);
ham_entity_t* ham_attach_self(const char* ename, uint64_t hp, int hpdl, int hpdh, unsigned flags);
int ham_detach_self(ham_entity_t* ehdl, unsigned flags);
int ham_heartbeat(void);
ham_condition_t* ham_condition(ham_entity_t* ehdl, int type, const char* cname, unsigned flags);
ham_action_t* ham_action_restart(ham_condition_t* chdl, const char* aname, const char* path, unsigned flags);
ham_action_t* ham_action_execute(ham_condition_t* chdl, const char* aname, const char* path, unsigned flags);
ham_action_t* ham_action_log(ham_condition_t* chdl, const char* aname, const char* msg, unsigned attachprefix, int verbosity, unsigned flags);
#ifdef __cplusplus
}
#endif
