// Host-only NOR model, no device access or non-synthetic credentials.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
void model_reset(unsigned sectors);
void model_remount(void);
void model_snapshot(void);
void model_restore(void);
unsigned model_writes(void);
unsigned model_reads(void);
unsigned model_flash_reads(void);
unsigned model_erases(void);
void model_inventory(const char *label);
unsigned model_events(void);
int model_ready(void);
void model_fail(unsigned event,int phase);
void model_no_fault(void);
void model_read_error(uint16_t id,int error);
void model_busy(int busy);
void model_counter(uint16_t id);
ssize_t model_put(uint16_t id,const void *data,size_t size);
int model_delete(uint16_t id);
ssize_t model_get(uint16_t id,void *out,size_t capacity);
void model_copy_flash(void *out);
extern int model_thread;
#ifdef __cplusplus
}
#endif
