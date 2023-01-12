/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */
/*
 * Copyright 2023 Ihor Usyk <ihor_usyk@epam.com>
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 */

#ifndef R_TAURUS_CLUSTER_PROTOCOL_H
#define R_TAURUS_CLUSTER_PROTOCOL_H

#define CLUSTER_SPEED                   0x1
#define CLUSTER_GEAR                    0x2
#define CLUSTER_RPM                     0x3
#define CLUSTER_TURN                    0x4
#define CLUSTER_DOOR_OPEN               0x5
#define CLUSTER_FOG_LIGHTS_BACK         0x6
#define CLUSTER_FOG_LIGHTS_FRONT        0x7
#define CLUSTER_HIGH_BEAMS_LIGHT        0x8
#define CLUSTER_HIGH_ENGINE_TEMPERATURE 0x9
#define CLUSTER_LOW_BATTERY             0xA
#define CLUSTER_LOW_BEAMS_LIGHTS        0xB
#define CLUSTER_LOW_FUEL                0xC
#define CLUSTER_LOW_OIL                 0xD
#define CLUSTER_LOW_TIRE_PRESSURE       0xE
#define CLUSTER_SEAT_BELT               0xF
#define CLUSTER_SIDE_LIGHTS             0x10
#define CLUSTER_BATTERY_ISSUE           0x11
#define CLUSTER_AUTO_LIGHTING_ON        0x12
#define CLUSTER_ACTIVE                  0xFF

struct taurus_cluster_data {
uint64_t   value;
uint16_t   ioctl_cmd;
};

#endif /* R_TAURUS_CLUSTER_PROTOCOL_H */
