/*************************************************************************/ /*
 CRC Driver (kernel module)
*/ /*************************************************************************/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "crc-wrapper.h"
#include "kcrc-drv.h"

struct kcrc_device {
    struct device *dev;
    void __iomem *base;
};

#define KCRC_BASE_DEVICES 11
static void __iomem *device_addresses[KCRC_BASE_DEVICES];
static int device_channel = 0;

static u32 kcrc_read(void __iomem *base, unsigned int offset)
{
    return ioread32(base + offset);
};

static void kcrc_write(void __iomem *base, unsigned int offset, u32 data)
{
    iowrite32(data, base + offset);
};

int kcrc_calculate(struct wcrc_info *info)
{
    unsigned int poly_set;
    unsigned int p_size;
    unsigned int kcrc_cmd;
    unsigned int input_dw;
    u32 reg;

    /* Skiping data input to*/
    if (info->skip_data_in)
        goto geting_output;

    /* Calculating data larger than 4bytes*/
    if (info->conti_cal)
        goto bypass_setting;

    /* Checking the Polynomial mode */
    switch (info->poly_mode) {
    case POLY_32_ETHERNET :
        p_size = KCRCmCTL_PSIZE_32;
        poly_set = KCRCmPOLY_32_ETHERNET;
        break;
    case POLY_16_CCITT_FALSE_CRC16 :
        p_size = KCRCmCTL_PSIZE_16;
        poly_set = KCRCmPOLY_16_CCITT;
        break;
    case POLY_8_SAE_J1850 :
        p_size = KCRCmCTL_PSIZE_8;
        poly_set = KCRCmPOLY_8_SAE_J1850;
        break;
    case POLY_8_0x2F :
        p_size = KCRCmCTL_PSIZE_8;
        poly_set = KCRCmPOLY_8_0x2F;
        break;
    case POLY_32_0x1EDC6F41 :
        p_size = KCRCmCTL_PSIZE_32;
        poly_set = KCRCmPOLY_32_CRC32C;
        break;
    default:
        pr_err("ERROR: Polynomial mode NOT found\n");
        return -ENXIO;
    }

    /* Checking KCRC Calculate Mode 0/1/2 */
    kcrc_cmd = (info->kcrc_cmd0 ? KCRCmCTL_CMD0 : 0) |
            (info->kcrc_cmd1 ? KCRCmCTL_CMD1 : 0) |
            (info->kcrc_cmd2 ? KCRCmCTL_CMD2 : 0);

    /* Checking KCRCmDIN and KCRCmDOUT data size setting */
    if (info->d_in_sz == 8) {
        input_dw = KCRCmCTL_DW_8;
    } else if (info->d_in_sz == 16) {
        input_dw = KCRCmCTL_DW_16;
    } else { //default 32-bit
        input_dw = KCRCmCTL_DW_32;
    }

    /* Set KCRCmCTL registers. */
    reg = p_size | kcrc_cmd | input_dw;
    kcrc_write(device_addresses[info->kcrc_unit], KCRCmCTL, reg);

    /* Set KCRCmPOLY registers. */
    kcrc_write(device_addresses[info->kcrc_unit], KCRCmPOLY, poly_set);

    /* Set KCRCmXOR register. */
    kcrc_write(device_addresses[info->kcrc_unit], KCRCmXOR, KCRCmXOR_XOR);

    /* Set initial value to KCRCmDOUT register. */
    kcrc_write(device_addresses[info->kcrc_unit], KCRCmDOUT, KCRCmDOUT_INITIAL);

bypass_setting:
    /* Set input value to KCRCmDIN register. */
    kcrc_write(device_addresses[info->kcrc_unit], KCRCmDIN, info->data_input);

geting_output:
    /* Read out the operated data from KCRCmDOUT register. */
    if (!info->during_conti_cal)
        info->kcrc_data_out = kcrc_read(device_addresses[info->kcrc_unit], KCRCmDOUT);

    return 0;
};
EXPORT_SYMBOL(kcrc_calculate);

static const struct of_device_id kcrc_of_ids[] = {
    {
        .compatible = "renesas,kcrc-drv",
    }, {
        .compatible = "renesas,kcrc-r8a78000",
    }, {
        /* Terminator */
    },
};

static int kcrc_probe(struct platform_device *pdev)
{
    struct kcrc_device *kcrc;
    struct resource *res;

    kcrc = devm_kzalloc(&pdev->dev, sizeof(*kcrc), GFP_KERNEL);
    if (!kcrc) {
        dev_err(&pdev->dev, "cannot allocate device data\n");
        return -ENOMEM;
    }

    kcrc->dev = &pdev->dev;

    /* Map I/O memory */
    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    kcrc->base = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(kcrc->base))
        return PTR_ERR(kcrc->base);

    /* Address storing for later uses */
    device_addresses[device_channel] = kcrc->base;
    device_channel++;

    return 0;
};

static int kcrc_remove(struct platform_device *pdev)
{
    return 0;
}

static struct platform_driver kcrc_driver = {
    .driver = {
        .name = "kcrc-driver",
        .of_match_table = of_match_ptr(kcrc_of_ids),
        .owner = THIS_MODULE,
    },
    .probe = kcrc_probe,
    .remove = kcrc_remove,
};

static int __init kcrc_drv_init(void)
{
    struct device_node *np;
    int ret;

    np = of_find_matching_node(NULL, kcrc_of_ids);
    if (!np)
        return 0;

    of_node_put(np);

    ret = platform_driver_register(&kcrc_driver);
    if (ret < 0)
        return ret;

    return 0;
};
subsys_initcall(kcrc_drv_init);
