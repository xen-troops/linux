/*
 * Copyright (c) 2015, Linaro Limited
 * Copyright (c) 2016, EPAM Systems
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef SHM_POOL_H
#define SHM_POOL_H
#include "../tee_private.h"

struct tee_shm_pool *
optee_shm_get_pool(struct tee_shm_pool_mem_info *dmabuf_info);

void optee_shm_pool_free(struct tee_shm_pool *pool);

#endif
