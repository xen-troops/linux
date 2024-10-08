/* SPDX-License-Identifier: GPL-2.0 */

#ifndef SVC_I3C_H
#define SVC_I3C_H

int rcar_i3c_master_probe(struct platform_device *pdev);
int rcar_i3c_master_remove(struct platform_device *pdev);
u32 i3c_reg_read(void __iomem *base, u32 offset);
void i3c_reg_write(void __iomem *base, u32 offset, u32 val);
void i3c_reg_set_bit(void __iomem *base, u32 reg, u32 val);
void i3c_reg_clear_bit(void __iomem *base, u32 reg, u32 val);
void i3c_reg_update_bit(void __iomem *base, u32 reg, u32 mask, u32 val);

enum i3c_internal_state {
        I3C_INTERNAL_STATE_DISABLED,
        I3C_INTERNAL_STATE_MASTER_IDLE,
        I3C_INTERNAL_STATE_MASTER_ENTDAA,
        I3C_INTERNAL_STATE_MASTER_SETDASA,
        I3C_INTERNAL_STATE_MASTER_WRITE,
        I3C_INTERNAL_STATE_MASTER_READ,
        I3C_INTERNAL_STATE_MASTER_COMMAND_WRITE,
        I3C_INTERNAL_STATE_MASTER_COMMAND_READ,
        I3C_INTERNAL_STATE_SLAVE_IDLE,
        I3C_INTERNAL_STATE_SLAVE_IBI,
};

enum i3c_event {
        I3C_COMMAND_ADDRESS_ASSIGNMENT,
        I3C_WRITE,
        I3C_READ,
        I3C_COMMAND_WRITE,
        I3C_COMMAND_READ,
        I3C_IBI_WRITE,
};

#endif
