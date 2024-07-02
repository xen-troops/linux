/*************************************************************************/ /*
 CRC Wrapper (kernel module)
*/ /*************************************************************************/

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/clk.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>

#include "crc-wrapper.h"
#include "crc-drv.h"
#include "kcrc-drv.h"

#define DEVNAME "crc-wrapper"
#define CLASS_NAME "wcrc"
#define WCRC_DEVICES 10

/* Define global variable */
DEFINE_MUTEX(lock);

struct wcrc_device {
    // WCRC part
	void __iomem *base[WCRC_DEVICES];
    struct device *dev[WCRC_DEVICES];
	struct clk *clk;
    struct cdev cdev;
    dev_t devt;

	// DMA part
	//void __iomem *fifo_base;
    struct resource *res_fifo[WCRC_DEVICES];
	enum dma_data_direction dma_data_dir;
	// TX
	struct scatterlist *sg_tx;
	struct dma_chan *dma_tx;
	uint64_t *buf_tx;
	uint32_t len_tx;
	dma_addr_t tx_dma_addr;
	struct completion done_txdma;
	// RX
	struct scatterlist *sg_rx;
	struct dma_chan *dma_rx;
	uint64_t *buf_rx;
	uint32_t len_rx;
	dma_addr_t rx_dma_addr;
	struct completion done_rxdma;
};

static struct wcrc_device *wcrc = NULL;

static bool device_created = 0;
static int dev_chan = 0;

static dev_t wcrc_devt;
static struct class *wcrc_class = NULL;

static uint64_t *u_data = NULL;
static uint32_t u_data_len = 0;

static void rcar_wcrc_dma_tx_callback(void *data);
static void rcar_wcrc_dma_rx_callback(void *data);

//static int wcrc_wait_for_completion(struct wcrc_device *priv,
//					struct completion *x);

static u32 wcrc_read(void __iomem *base, unsigned int offset)
{
    return ioread32(base + offset);
}

static void wcrc_write(void __iomem *base, unsigned int offset, u32 data)
{
    iowrite32(data, base + offset);
}

/* DMA API */
static struct dma_chan *wcrc_request_dma_chan(struct device *dev,
					enum dma_transfer_direction dir,
					dma_addr_t port_addr, char *chan_name)
{
	struct dma_chan *chan;
	struct dma_slave_config cfg;
	int ret;

	chan = dma_request_chan(dev, chan_name);
	if (IS_ERR(chan)) {
		dev_dbg(dev, "request_channel failed for %s (%ld)\n",
			chan_name, PTR_ERR(chan));
		pr_info("dma_request_chan: %s FAILED\n", chan_name);
		return chan;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.direction = dir;
	if (dir == DMA_MEM_TO_DEV) {
		cfg.dst_addr = port_addr;
		//cfg.dst_addr_width = DMA_SLAVE_BUSWIDTH_16_BYTES;
		cfg.dst_addr_width = DMA_SLAVE_BUSWIDTH_8_BYTES;
	} else {
		cfg.src_addr = port_addr;
		//cfg.src_addr_width = DMA_SLAVE_BUSWIDTH_16_BYTES;
		cfg.src_addr_width = DMA_SLAVE_BUSWIDTH_8_BYTES;
	}

	ret = dmaengine_slave_config(chan, &cfg);
	if (ret) {
		dev_dbg(dev, "slave_config failed for %s (%d)\n",
			chan_name, ret);
		dma_release_channel(chan);
		pr_info("dmaengine_slave_config: FAILED\n");
		return ERR_PTR(ret);
	}

	dev_dbg(dev, "got DMA channel for %s\n", chan_name);
	return chan;
}

static struct dma_chan *wcrc_request_dma(struct wcrc_device *priv,
                    enum dma_transfer_direction dir,
                    dma_addr_t offs_port_addr, char *chan_name)
{
	struct device *dev = priv->dev[0];
	struct dma_chan *chan;

    if ((DMA_DEV_TO_MEM == dir) || (DMA_MEM_TO_DEV == dir))
    {
        chan = wcrc_request_dma_chan(dev, dir, (priv->res_fifo[0]->start + offs_port_addr), chan_name);
    }
    else
    {
        dev_dbg(dev, "%s: FAILED for dir=%d\n, chan %s", __func__, dir, chan_name);
        return ERR_PTR(-EPROBE_DEFER);
    }

    return chan;
}

static void rcar_wcrc_cleanup_dma(struct wcrc_device *priv) {

	if (priv->dma_data_dir == DMA_NONE)
		return;
	else if (priv->dma_data_dir == DMA_FROM_DEVICE)
		dmaengine_terminate_async(priv->dma_rx);
	else if (priv->dma_data_dir == DMA_TO_DEVICE)
		dmaengine_terminate_async(priv->dma_tx);
}

static bool rcar_wcrc_dma_tx(struct wcrc_device *priv, uint64_t *data)
{
	struct device *dev = priv->dev[0];
	struct dma_async_tx_descriptor *txdesc;
    struct dma_chan *chan;
    dma_cookie_t cookie;
	int num_desc;
	enum dma_data_direction data_dir;
	enum dma_transfer_direction trans_dir;
	uint64_t *buf;
	int len;
	//int ret;
	int i;

	/* DMA_MEM_TO_DEV */
	priv->buf_tx = data;
	//pr_info("priv->buf_tx points to block mem %zu bytes\n", ksize(priv->buf_tx));
	buf = priv->buf_tx;
	priv->len_tx = u_data_len;
	len = priv->len_tx;
    num_desc = 1;
	priv->dma_data_dir = DMA_TO_DEVICE;
	data_dir = priv->dma_data_dir;
    trans_dir = DMA_MEM_TO_DEV;
    chan = priv->dma_tx;

	priv->sg_tx = kmalloc_array(num_desc, sizeof(struct scatterlist), GFP_KERNEL);
	if (!priv->sg_tx) {
		pr_info("priv->sg_tx: kmalloc_array FAILED\n");
		return false;
	}

	sg_init_table(priv->sg_tx, num_desc);

	for (i = 0; i < num_desc; i++) {
		sg_dma_len(&priv->sg_tx[i]) = len/num_desc;
		sg_dma_address(&priv->sg_tx[i]) = dma_map_single(chan->device->dev,
												buf + i, 
												len, data_dir);
	}

	txdesc = dmaengine_prep_slave_sg(chan, priv->sg_tx, num_desc, 
									trans_dir, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);

	txdesc->callback = rcar_wcrc_dma_tx_callback;
	txdesc->callback_param = priv;

    cookie = dmaengine_submit(txdesc);
    if (dma_submit_error(cookie)) {
		//pr_info("dmaengine_submit_error\n");
		dev_dbg(dev, "%s: submit TX dma failed, using PIO\n", __func__);
		rcar_wcrc_cleanup_dma(priv);
		return false;
	}

	//reinit_completion(&priv->done_txdma);

    dma_async_issue_pending(chan);

	//ret = wcrc_wait_for_completion(priv, &priv->done_txdma);
	//if (ret) {
	//	//pr_info("wcrc_wait_for_completion: FAILED\n");
	//	dev_dbg(dev, "%s: wcrc_wait_for_completion: FAILED\n", __func__);
	//	return false;
	//}

	for (i=0; i<1000000; i++) {
		nop();
	}

	return true;
}

static bool rcar_wcrc_dma_rx(struct wcrc_device *priv)
{
	struct device *dev = priv->dev[0];
	struct dma_async_tx_descriptor *rxdesc;
    struct dma_chan *chan;
    dma_cookie_t cookie;
	int num_desc;
	enum dma_data_direction data_dir;
	enum dma_transfer_direction trans_dir;
	uint64_t *buf;
	int len;
	//int ret;
	int i;

	/* DMA_DEV_TO_MEM */
	priv->buf_rx = kzalloc(PORT_SIZE, GFP_KERNEL);
	//pr_info("priv->buf_rx points to block mem %zu bytes\n", ksize(priv->buf_rx));
	buf = priv->buf_rx;
	priv->len_rx = priv->len_tx;
	len = priv->len_rx;
    num_desc = 1;
	data_dir = DMA_FROM_DEVICE;
    trans_dir = DMA_DEV_TO_MEM;
    chan = priv->dma_rx;

	priv->sg_rx = kmalloc_array(num_desc, sizeof(struct scatterlist), GFP_KERNEL);
	if (!priv->sg_rx) {
		pr_info("priv->sg_rx: kmalloc_array FAILED\n");
		return false;
	}

	sg_init_table(priv->sg_rx, num_desc);

	for (i = 0; i < num_desc; i++) {
		sg_dma_len(&priv->sg_rx[i]) = len/num_desc;
		sg_dma_address(&priv->sg_rx[i]) = dma_map_single(chan->device->dev,
												buf + i, 
												len, data_dir);
	}

	rxdesc = dmaengine_prep_slave_sg(chan, priv->sg_rx, num_desc, 
									trans_dir, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);


	rxdesc->callback = rcar_wcrc_dma_rx_callback;
	rxdesc->callback_param = priv;

    cookie = dmaengine_submit(rxdesc);
    if (dma_submit_error(cookie)) {
		pr_info("dmaengine_submit_error\n");
		dev_dbg(dev, "%s: submit RX dma failed, using PIO\n", __func__);
		rcar_wcrc_cleanup_dma(priv);
		return false;
	}

	//reinit_completion(&priv->done_rxdma);
	
    dma_async_issue_pending(chan);

	//ret = wcrc_wait_for_completion(priv, &priv->done_rxdma);
	//if (ret) {
	//	//pr_info("wcrc_wait_for_completion: FAILED\n");
	//	dev_dbg(dev, "%s: wcrc_wait_for_completion: FAILED\n", __func__);
	//	return false;
	//}

	for (i=0; i<1000000; i++) {
		nop();
	}

	return true;

}

//static int wcrc_wait_for_completion(struct wcrc_device *priv,
//					struct completion *x)
//{
//	if (wait_for_completion_interruptible(x)) {
//		dev_dbg(priv->dev[0], "interrupted\n");
//		return -EINTR;
//	}
//	
//	return 0;	
//}

static void rcar_wcrc_dma_tx_callback(void *data)
{
	struct wcrc_device *priv = data;

	pr_info("tx-cb\n");
	//pr_info("tx %s %d\n", __func__, __LINE__);
	complete(&priv->done_txdma);

	/*
	struct dma_chan *chan = priv->dma_tx;
	// Use when device accesses the buffer through dma_map_single() 
	dma_sync_sg_for_device(chan->device->dev, &priv->sg_tx, 1, DMA_TO_DEVICE);	
	*/
}

static void rcar_wcrc_dma_rx_callback(void *data)
{
	struct wcrc_device *priv = data;
	struct dma_chan *chan = priv->dma_rx;

	pr_info("rx-cb\n");
	dma_sync_sg_for_cpu(chan->device->dev, priv->sg_rx, 1, DMA_FROM_DEVICE);	
	complete(&priv->done_rxdma);
}

static void rcar_wcrc_release_dma(struct wcrc_device *priv)
{
	//dma_unmap_single(priv->dma_tx->device->dev, priv->tx_dma_addr, priv->len_tx, DMA_TO_DEVICE);
	//dma_unmap_single(priv->dma_tx->device->dev, priv->tx_dma_addr, priv->len_tx, DMA_TO_DEVICE);
	dma_unmap_sg(priv->dma_rx->device->dev, priv->sg_tx, 1, DMA_FROM_DEVICE);
	dma_unmap_sg(priv->dma_rx->device->dev, priv->sg_rx, 1, DMA_FROM_DEVICE);

	kfree(priv->buf_tx);
	kfree(priv->buf_rx);

	if (!IS_ERR(priv->dma_tx)) {
		dma_release_channel(priv->dma_tx);
		priv->dma_tx = ERR_PTR(-EPROBE_DEFER);
	}

	if (!IS_ERR(priv->dma_rx)) {
		dma_release_channel(priv->dma_rx);
		priv->dma_rx = ERR_PTR(-EPROBE_DEFER);
	}
}

static int wcrc_independent_crc(struct wcrc_info *info)
{
    int ret;

    mutex_lock(&lock);

    if (info->crc_opt == 0) //INDEPENDENT_CRC mode
        ret = crc_calculate(info);
    else //INDEPENDENT_KCRC mode
        ret = kcrc_calculate(info);

    mutex_unlock(&lock);

    if (ret)
        pr_err("Calculation Aborted!, ERR: %d", ret);

    return 0;
}

static int wcrc_data_through_mode(struct wcrc_info *info)
{
    int ret = 0;
	unsigned int reg_val = 0;

    mutex_lock(&lock);

	//1. Set in_en=1, out_en=1 in WCRCm_XXXX_EN register.
	reg_val = wcrc_read(wcrc->base[info->wcrc_unit], WCRCm_CRCm_EN);

	if (1 == info->in_en)
	{
		reg_val |= CRCm_EN_IN_EN;
	}
	else
	{
		reg_val &= (~CRCm_EN_IN_EN);
	}
	
	if (1 == info->out_en)
	{
		reg_val |= CRCm_EN_OUT_EN;
	}
	else
	{
		reg_val &= (~CRCm_EN_OUT_EN);
	}

	wcrc_write(wcrc->base[info->wcrc_unit], WCRCm_CRCm_EN, reg_val);
	//pr_info("WCRCm_CRCm_EN = 0x%x\n", wcrc_read(wcrc->base[info->wcrc_unit], WCRCm_CRCm_EN));

	//2. Transfer input data to data port of FIFO by DMAC.
	wcrc->dma_tx = wcrc_request_dma(wcrc, DMA_MEM_TO_DEV, PORT_DATA(CRCm), "tx");
	(void)rcar_wcrc_dma_tx(wcrc, u_data);

	//3. Read out input data from result port of FIFO by DMAC.
	wcrc->dma_rx = wcrc_request_dma(wcrc, DMA_DEV_TO_MEM, PORT_DATA(CRCm), "rx_in");
	(void)rcar_wcrc_dma_rx(wcrc);

	//4. Set stop=1 in WCRCm_XXXX_STOP by command function.
	reg_val = wcrc_read(wcrc->base[info->wcrc_unit], WCRCm_CRCm_STOP);
	reg_val |= CRCm_STOP;
	wcrc_write(wcrc->base[info->wcrc_unit], WCRCm_CRCm_STOP, reg_val);

	//5. Clear stop_done in WCRCm_XXXX_STS.
	reg_val = wcrc_read(wcrc->base[info->wcrc_unit], WCRCm_CRCm_STS);
	reg_val |= CRCm_STS_STOP_DONE;
	wcrc_write(wcrc->base[info->wcrc_unit], WCRCm_CRCm_STS, reg_val);

    mutex_unlock(&lock);

    if (ret)
        pr_err("Calculation Aborted!, ERR: %d", ret);

    return ret;
}

static int wcrc_open(struct inode *inode, struct file *filep)
{
    struct wcrc_info *p_access;

    p_access = kzalloc(sizeof(*p_access), GFP_KERNEL);
    if (!p_access)
        return -ENOMEM;

    pr_debug("Device Open\n");
    filep->private_data = p_access;

	return 0;
}

static int wcrc_release(struct inode *inode, struct file *filep)
{
    struct wcrc_info *p_access;

    p_access = filep->private_data;
    kfree(p_access);
    pr_debug("Device Release\n");

	return 0;
}

static long dev_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
    struct wcrc_info u_features;
	struct wcrc_device *inst0 = wcrc;
    int ret = 0;

    switch (cmd) {
    case INDEPENDENT_CRC_MODE :
        ret = copy_from_user(&u_features, (struct wcrc_info *)arg, sizeof(u_features));
        if (ret){
            pr_err("Error taking data from user\n");
            return ret;
        }

        wcrc_independent_crc(&u_features);

        ret = copy_to_user((struct wcrc_info *)arg, &u_features, sizeof(u_features));
        if (ret) {
            pr_err("Error sending data to user\n");
            return ret;
        }

        break;

	case DATA_THROUGH_MODE:
		if (NULL != u_data) {
			ret = copy_from_user(&u_features, (struct wcrc_info *)arg, sizeof(u_features));
        	if (ret){
        	    pr_err("DATA_THROUGH_MODE: Error taking data from user\n");
        	    return ret;
        	}

        	wcrc_data_through_mode(&u_features);

			ret = copy_to_user((struct wcrc_device*)arg, &u_features, sizeof(u_features));
        	if (ret) {
        	    pr_err("DATA_THROUGH_MODE: Error sending data to user\n");
        	    return ret;
        	}

		}
		else {
			pr_info("DATA_THROUGH_MODE: missing user data, SET_INPUT_LEN and SET_INPUT_DATA\n");
			ret = 1;
		}

        break;

	case SET_INPUT_LEN:	
		ret = copy_from_user(&u_data_len, (uint32_t *)arg, sizeof(u_data_len));
		if (ret) {
			pr_err("SET_INPUT_LEN: Error taking data from user\n");
			return ret;
		}

		//pr_info("SET_INPUT_LEN: u_data_len=0x%x:\n", u_data_len);

		if (0 == u_data_len) {
			pr_info("SET_INPUT_LEN: ERROR (data length = 0)\n");
			ret = 2;
		}
		break;

	case SET_INPUT_DATA:
		if (0 != u_data_len) {
			u_data = kzalloc(u_data_len, GFP_KERNEL);
			//u_data = kzalloc(PORT_SIZE, GFP_KERNEL);
			if (NULL != u_data) {
				ret = copy_from_user(u_data, (uint64_t *)arg, u_data_len);
				if (ret) {
					pr_err("SET_INPUT_DATA: Error taking data from user\n");
					return ret;
				}
			}
			else {
				pr_info("SET_INPUT_DATA: Allocate memory FAILED\n");
				ret = 1;
			}
		}
		else {
			pr_info("SET_INPUT_DATA: data length = 0, SET_INPUT_LEN first\n");
		}
		break;

	case GET_OUTPUT:
		ret = copy_to_user((uint64_t *)arg, inst0->buf_rx, u_data_len);
		//ret = copy_to_user((uint32_t *)arg, u_data, u_data_len);
		if (ret) {
			if (NULL != inst0) {
				//Release DMA 
				rcar_wcrc_release_dma(inst0);
			}
			pr_err("GET_OUTPUT: Error sending data to user\n");
			return ret;
		}

		if (NULL != inst0) {
			//Release DMA 
			rcar_wcrc_release_dma(inst0);
		}

		// rcar_wcrc_release_dma(inst0) has called kfree(u_data) through inst0->buf_tx.
		// If call kfree(u_data) again, double free occurs (undefined behavior => kernel panic).
		u_data = NULL;

		break;

    default:
        return -EINVAL;
    }

    return ret;
}

static const struct of_device_id wcrc_of_ids[] = {
    {
        .compatible = "renesas,crc-wrapper",
    }, {
        .compatible = "renesas,wcrc-r8a78000",
    }, {
        /* Terminator */
    },
};

static struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = wcrc_open,
	.release = wcrc_release,
	.unlocked_ioctl = dev_ioctl,
};

static int wcrc_probe(struct platform_device *pdev)
{
    struct device *dev;
    struct resource *res;
    int ret;

    if (dev_chan == 0) {
        wcrc = devm_kzalloc(&pdev->dev, sizeof(*wcrc), GFP_KERNEL);
        if (!wcrc) {
            dev_err(&pdev->dev, "cannot allocate device data\n");
            return -ENOMEM;
        }
    }

    wcrc->dev[dev_chan] = &pdev->dev;

    /* Map I/O memory */
    res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "wcrc");
    wcrc->base[dev_chan] = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(wcrc->base[dev_chan])) {
        dev_err(&pdev->dev, "Unable to map I/O for device\n");
	    return PTR_ERR(wcrc->base[dev_chan]);
    }

    wcrc->res_fifo[dev_chan] = platform_get_resource_byname(pdev, IORESOURCE_MEM, "fifo");
    if (!res || !wcrc->res_fifo[dev_chan]) {
        dev_err(&pdev->dev, "invalid resource\n");
        return -EINVAL;
    }

	/* Look up and obtains to a clock node: TBD */

	
	/* Enable peripheral clock for register access: TBD */

	/* Init DMA */
	wcrc->dma_rx = wcrc->dma_tx = ERR_PTR(-EPROBE_DEFER);
	//init_completion(&wcrc->done_txdma);
	//init_completion(&wcrc->done_rxdma);

    if (!device_created) {
        /* Creating WCRC device */
        wcrc->devt = MKDEV(MAJOR(wcrc_devt), 0);
        cdev_init(&wcrc->cdev, &fops);
        wcrc->cdev.owner = THIS_MODULE;
        ret = cdev_add(&wcrc->cdev, wcrc->devt, 1);
        if (ret < 0) {
            dev_err(&pdev->dev, "Unable to add char device\n");
            return ret;
        }

        dev = device_create(wcrc_class,
                            NULL,
                            wcrc->devt,
                            NULL,
                            DEVNAME);
        if (IS_ERR(dev)) {
            dev_err(&pdev->dev, "Unable to create device\n");
            cdev_del(&wcrc->cdev);
            return PTR_ERR(dev);
        }
        device_created = 1;
    }

    dev_chan++;

    platform_set_drvdata(pdev, wcrc);

    return 0;
}

static int wcrc_remove(struct platform_device *pdev)
{
    struct wcrc_device *priv = platform_get_drvdata(pdev);

	rcar_wcrc_release_dma(priv);
    device_destroy(wcrc_class, MKDEV(MAJOR(wcrc_devt), 0));
    cdev_del(&priv->cdev);

    return 0;
}

static struct platform_driver wcrc_driver = {
    .driver = {
        .name = DEVNAME,
        .of_match_table = of_match_ptr(wcrc_of_ids),
        .owner = THIS_MODULE,
    },
    .probe = wcrc_probe,
    .remove = wcrc_remove,
};

static int __init wcrc_init(void)
{
    struct device_node *np;
    int ret;

    np = of_find_matching_node(NULL, wcrc_of_ids);
    if (!np)
        return 0;

    of_node_put(np);

    ret = alloc_chrdev_region(&wcrc_devt, 0, WCRC_DEVICES, DEVNAME);
	if (ret) {
		pr_err("wcrc: Failed to register device\n");
        return ret;
    }

    wcrc_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(wcrc_class)) {
        pr_err("wcrc: Failed to create class\n");
        ret = (PTR_ERR(wcrc_class));
        goto class_err;
    }

    ret = platform_driver_register(&wcrc_driver);
    if (ret < 0)
        goto drv_reg_err;

    return 0;

drv_reg_err:
    class_destroy(wcrc_class);

class_err:
    unregister_chrdev_region(wcrc_devt, WCRC_DEVICES);

    return ret;
}

static void __exit wcrc_exit(void)
{
    unregister_chrdev_region(wcrc_devt, WCRC_DEVICES);
    class_destroy(wcrc_class);
    platform_driver_unregister(&wcrc_driver);
}

module_init(wcrc_init);
module_exit(wcrc_exit);

MODULE_LICENSE("GPLv2");
MODULE_AUTHOR("Renesas Electronics Corporation");
MODULE_DESCRIPTION("R-Car Cyclic Redundancy Check Wrapper");

