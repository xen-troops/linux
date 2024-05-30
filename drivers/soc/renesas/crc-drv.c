/*************************************************************************/ /*
 CRC Driver (kernel module)
*/ /*************************************************************************/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "crc-wrapper.h"
#include "crc-drv.h"

struct crc_device {
    struct device *dev;
    void __iomem *base;
};

#define CRC_BASE_DEVICES 11
static void __iomem *device_addresses[CRC_BASE_DEVICES];
static int device_channel = 0;

static u32 crc_read(void __iomem *base, unsigned int offset)
{
    return ioread32(base + offset);
};

static void crc_write(void __iomem *base, unsigned int offset, u32 data)
{
    iowrite32(data, base + offset);
};

int crc_calculate(struct wcrc_info *info)
{
    unsigned int crc_size;
	unsigned int poly_set;
    unsigned int initial_set;
    unsigned int crc_cmd;
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
        poly_set = DCRAmCTL_POL_32_ETHERNET;
        initial_set = DCRAmCOUT_32_ETHERNET;
        break;
    case POLY_16_CCITT_FALSE_CRC16 :
        poly_set = DCRAmCTL_POL_16_CCITT_FALSE_CRC16;
        initial_set = DCRAmCOUT_16_CCITT_FALSE_CRC16;
        break;
    case POLY_8_SAE_J1850 :
        poly_set = DCRAmCTL_POL_8_SAE_J1850;
        initial_set = DCRAmCOUT_8_SAE_J1850;
        break;
    case POLY_8_0x2F :
        poly_set = DCRAmCTL_POL_8_0x2F;
        initial_set = DCRAmCOUT_8_0x2F;
        break;
    case POLY_32_0xF4ACFB13 :
        poly_set = DCRAmCTL_POL_32_0xF4ACFB13;
        initial_set = DCRAmCOUT_32_0xF4ACFB13;
        break;
    case POLY_32_0x1EDC6F41 :
        poly_set = DCRAmCTL_POL_32_0x1EDC6F41;
        initial_set = DCRAmCOUT_32_0x1EDC6F41;
        break;
    case POLY_21_0x102899 :
        poly_set = DCRAmCTL_POL_21_0x102899;
        initial_set = DCRAmCOUT_21_0x102899;
        break;
    case POLY_17_0x1685B :
        poly_set = DCRAmCTL_POL_17_0x1685B;
        initial_set = DCRAmCOUT_17_0x1685B;
        break;
    case POLY_15_0x4599 :
        poly_set = DCRAmCTL_POL_15_0x4599;
        initial_set = DCRAmCOUT_15_0x4599;
        break;
    default:
        pr_err("ERROR: Polynomial mode NOT found\n");
        return -ENXIO;
    }

    /* Checking DCRAmCIN data size setting */
    if (info->d_in_sz == 8) {
        crc_size = DCRAmCTL_ISZ_8;
    } else if (info->d_in_sz == 16) {
        crc_size = DCRAmCTL_ISZ_16;
    } else { //default 32-bit
        crc_size = DCRAmCTL_ISZ_32;
    }

    /* Set DCRAmCTL registers. */
    reg = crc_size | poly_set;
    crc_write(device_addresses[info->crc_unit], DCRAmCTL, reg);

    /* Checking DCRAmCTL2 setting */
    crc_cmd = (info->out_exor_on ? DCRAmCTL2_xorvalmode : 0) |
        (info->out_bit_swap ? DCRAmCTL2_bitswapmode : 0) |
        (info->in_exor_on ? DCRAmCTL2_xorvalinmode : 0) |
        (info->in_bit_swap ? DCRAmCTL2_bitswapinmode : 0);

    switch (info->out_byte_swap) {
    case 01 :
        crc_cmd |= DCRAmCTL2_byteswapmode_01;
        break;
    case 10 :
        crc_cmd |= DCRAmCTL2_byteswapmode_10;
        break;
    case 11 :
        crc_cmd |= DCRAmCTL2_byteswapmode_11;
        break;
    default : //no swap
        crc_cmd |= DCRAmCTL2_byteswapmode_00;
        break;
    }

    switch (info->in_byte_swap) {
    case 01 :
        crc_cmd |= DCRAmCTL2_byteswapinmode_01;
        break;
    case 10 :
        crc_cmd |= DCRAmCTL2_byteswapinmode_10;
        break;
    case 11 :
        crc_cmd |= DCRAmCTL2_byteswapinmode_11;
        break;
    default : //no swap
        crc_cmd |= DCRAmCTL2_byteswapinmode_00;
        break;
    }

    /* Set DCRAmCTL2 registers. */
    crc_write(device_addresses[info->crc_unit], DCRAmCTL2, crc_cmd);

    /* Set initial value to DCRAmCOUT register. */
    crc_write(device_addresses[info->crc_unit], DCRAmCOUT, DCRAmCOUT_DEFAULT);

    /* Set polynomial initial value to DCRAmCOUT register. */
    crc_write(device_addresses[info->crc_unit], DCRAmCOUT, initial_set);

bypass_setting:
    /* Set input value to DCRAmCIN register. */
    crc_write(device_addresses[info->crc_unit], DCRAmCIN, info->data_input);

geting_output:
    /* Read out the operated data from DCRAmCOUT register. */
    if (!info->during_conti_cal)
        info->crc_data_out = crc_read(device_addresses[info->crc_unit], DCRAmCOUT);

    return 0;
};
EXPORT_SYMBOL(crc_calculate);

static const struct of_device_id crc_of_ids[] = {
    {
        .compatible = "renesas,crc-drv",
    }, {
        .compatible = "renesas,crc-r8a78000",
    }, {
        /* Terminator */
    },
};

static int crc_probe(struct platform_device *pdev)
{
    struct crc_device *crc;
    struct resource *res;

    crc = devm_kzalloc(&pdev->dev, sizeof(*crc), GFP_KERNEL);
    if (!crc) {
        dev_err(&pdev->dev, "cannot allocate device data\n");
        return -ENOMEM;
    }

    crc->dev = &pdev->dev;

    /* Map I/O memory */
    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    crc->base = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(crc->base))
        return PTR_ERR(crc->base);

    /* Address storing for later uses */
    device_addresses[device_channel] = crc->base;
    device_channel++;

    return 0;
};

static int crc_remove(struct platform_device *pdev)
{
    return 0;
}

static struct platform_driver crc_driver = {
    .driver = {
        .name = "crc-driver",
        .of_match_table = of_match_ptr(crc_of_ids),
        .owner = THIS_MODULE,
    },
    .probe = crc_probe,
    .remove = crc_remove,
};

static int __init crc_drv_init(void)
{
    struct device_node *np;
    int ret;

    np = of_find_matching_node(NULL, crc_of_ids);
    if (!np)
        return 0;

    of_node_put(np);

    ret = platform_driver_register(&crc_driver);
    if (ret < 0)
        return ret;

    return 0;
};
subsys_initcall(crc_drv_init);
