#ifndef _DEVCTS_H_
#define _DEVCTS_H_

#include <linux/ktime.h>
#include <uapi/linux/devcts.h>

typedef int (*devcts_get_time_fn_t)(ktime_t *, ktime_t *, void *);

int devcts_register_device(const char *name, devcts_get_time_fn_t fn, void *ctx);

void devcts_unregister_device(const char *name);


#endif
