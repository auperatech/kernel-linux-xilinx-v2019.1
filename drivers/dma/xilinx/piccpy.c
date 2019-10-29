#include <linux/module.h>
#include <linux/kernel.h>   
#include <linux/moduleparam.h>
#include <asm/uaccess.h>
#include <asm/pgtable.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/cdev.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/mm.h>
#include <linux/kdev_t.h>
#include <linux/proc_fs.h>
#include <linux/ioctl.h>
#include <linux/slab.h>
#include <linux/mempool.h>
#include <linux/mm.h>
#include <linux/device.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <asm/types.h>
#include <asm/io.h>
#include <asm/dma-mapping.h>
#include <linux/delay.h>

#define DMA_MAJOR					201
#define DMA_MINOR 					0
#define DMA_NAME					"picCpy"
#define MAX_DMA_CHANNELS			3
#define MAX_SG_TRANSFER_SIZE		0x20000
#define MAX_CHUNKS					32

typedef struct
{
	volatile struct dma_chan *pChan;
	volatile int use;
}dmaCh_t;

typedef struct
{
	void *pSrcAddr;
	void *pDstAddr;
	uint32_t srcStride;
	uint32_t dstStride;
	uint32_t w;
	uint32_t h;
}piccpy_ctrl_t;

static volatile dmaCh_t g_dmaCh[MAX_DMA_CHANNELS] = {0};
static spinlock_t dmalock;
static struct cdev *dma_cdev = NULL;
static struct class *dma_class = NULL;  
static DECLARE_WAIT_QUEUE_HEAD(piccpy_queue);
static volatile int gDmaChanRqtFlag = 0;

static dmaCh_t *get_idle_dma_channel(void)
{
	int i;

	spin_lock(&dmalock);
	for(i=0;i<MAX_DMA_CHANNELS;i++)
	{
		if(0==g_dmaCh[i].use)
		{
			g_dmaCh[i].use = 1;
			spin_unlock(&dmalock);
			return &g_dmaCh[i];
		}
	}
	
//	printk(KERN_ERR "picCpy:no idle dma channel!\n");
	spin_unlock(&dmalock);
	return NULL;
}

static void release_dma_channel(dmaCh_t *pDmaCh)
{
	if(pDmaCh){
		pDmaCh->use = 0;
		wake_up(&piccpy_queue);
	}
}

static void dma_complete_func(void *completion)
{	
	if(completion)
	{
		complete(completion);
	}
}

static int device_open(struct inode *inode, struct file *file)
{
	dma_cap_mask_t mask;
	int i;

	spin_lock(&dmalock);
	if(!gDmaChanRqtFlag){
		dma_cap_zero(mask);
        	dma_cap_set(DMA_MEMCPY,mask);
		for(i=0;i<MAX_DMA_CHANNELS;i++)
		{
			g_dmaCh[i].use = 0;
			g_dmaCh[i].pChan = dma_request_channel(mask, 0, NULL);
			if(NULL==g_dmaCh[i].pChan)
			{
				printk(KERN_ERR "picCpy:dma_request_channel fail!\n");
				spin_unlock(&dmalock);
				return -1;
			}
		}
		gDmaChanRqtFlag = 1;
	}
	spin_unlock(&dmalock);
	return 0;
}

static long device_ioctl(struct file *file,unsigned int num,unsigned long param)
{
	struct dma_device *dma_dev;
	dma_cookie_t cookie;
	enum dma_ctrl_flags flags;
	struct dma_async_tx_descriptor *tx1 = NULL;
	volatile struct completion comp1;
	piccpy_ctrl_t *pCtrlData = (piccpy_ctrl_t *)param;
	uint32_t srcPhy = *(uint32_t *)(pCtrlData->pSrcAddr);
	uint32_t dstPhy = *(uint32_t *)(pCtrlData->pDstAddr);
	struct sg_table *src_sg;
	struct sg_table *dst_sg;
	dmaCh_t *pDmaChan = NULL;

	wait_event(piccpy_queue, pDmaChan=get_idle_dma_channel());

	struct device *pDev = pDmaChan->pChan->device->dev;
	struct scatterlist *sg;
	unsigned int chunks;
	struct dma_async_tx_descriptor *txd = NULL;
	int err = -EPERM;
	int i,j;
	int ret;
	int timeout = 1000;

	//src_sg
	src_sg = devm_kzalloc(pDev, sizeof(struct sg_table), GFP_ATOMIC);
	if (!src_sg) {
		printk(KERN_ERR "%s line:%d error!!!\n", __func__, __LINE__);
		err = -ENOMEM;
		goto OUT_SRC_SG_ALLOC;
	}

	ret = sg_alloc_table(src_sg, MAX_CHUNKS, GFP_ATOMIC);
	if(ret<0){
		printk(KERN_ERR "%s line:%d error!!!\n", __func__, __LINE__);
		err = -ENOMEM;
		goto OUT_SRC_SG_TO_SGL;
	}
	
	//dst_sg
	dst_sg = devm_kzalloc(pDev, sizeof(struct sg_table), GFP_ATOMIC);
	if (!dst_sg) {
		printk(KERN_ERR "%s line:%d error!!!\n", __func__, __LINE__);
		err = -ENOMEM;
		goto OUT_DST_SG_ALLOC;
	}

	if(pCtrlData->dstStride == pCtrlData->w)
	{
		ret = sg_alloc_table(dst_sg, 1, GFP_ATOMIC);
		if(ret<0){
			printk(KERN_ERR "%s line:%d error!!!\n", __func__, __LINE__);
			err = -ENOMEM;
			goto OUT_DST_SG_TO_SGL;
		}
	}
	else
	{
		ret = sg_alloc_table(dst_sg, MAX_CHUNKS, GFP_ATOMIC);
		if(ret<0){
			printk(KERN_ERR "%s line:%d error!!!\n", __func__, __LINE__);
			err = -ENOMEM;
			goto OUT_DST_SG_TO_SGL;
		}
	}

	int remain = pCtrlData->h;
	for(j=0;;j++)
	{
		chunks = remain>MAX_CHUNKS?MAX_CHUNKS:remain;

		src_sg->nents = chunks;
		src_sg->orig_nents = chunks;
		for_each_sg(src_sg->sgl, sg, chunks, i)
		{
			sg_dma_address(sg) = srcPhy + pCtrlData->srcStride * (j*MAX_CHUNKS + i);
			sg_dma_len(sg) = pCtrlData->w;
			if(i<chunks-1)
				sg->page_link = 0x0;
			else
				sg->page_link = 0x2;
		}

		if(pCtrlData->dstStride == pCtrlData->w)
		{
			sg_dma_address(dst_sg->sgl) = dstPhy + pCtrlData->w * j * MAX_CHUNKS;
			sg_dma_len(dst_sg->sgl) = pCtrlData->w * chunks;
		}
		else
		{
			dst_sg->nents = chunks;
			dst_sg->orig_nents = chunks;
			for_each_sg(dst_sg->sgl, sg, chunks, i)
			{
				sg_dma_address(sg) = dstPhy + pCtrlData->dstStride * (j*MAX_CHUNKS + i);
				sg_dma_len(sg) = pCtrlData->w;
				if(i<chunks-1)
					sg->page_link = 0x0;
				else
					sg->page_link = 0x2;
			}
		}

		//dma
		init_completion(&comp1);
		flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
		txd = pDmaChan->pChan->device->device_prep_dma_sg(pDmaChan->pChan, dst_sg->sgl, dst_sg->nents, src_sg->sgl, src_sg->nents, flags);
		if (!txd) {
			err = -EPERM;
			printk(KERN_ERR "device_prep_dma_sg fail!\n");
			goto OUT_PREP_DMA_SG;
		}

		txd->callback = dma_complete_func;
		txd->callback_param = &comp1;

		cookie = txd->tx_submit(txd);
		if (dma_submit_error(cookie)) {
			err = -EPERM;
			printk(KERN_ERR "Unable to submit transaction\n");
			goto OUT_TX_SUBMIT;
		}

		dma_async_issue_pending(pDmaChan->pChan);
		wait_for_completion_killable(&comp1);

		while(dmaengine_tx_status(pDmaChan->pChan, cookie, NULL)!=DMA_COMPLETE)
		{
			msleep(1);
			timeout--;
			if(0==timeout){
				err = -ETIME;
				printk(KERN_ERR "piccpy timeout!\n");
				goto OUT_TX_SUBMIT;
			}
		}
		
		remain -= chunks;
		if(remain<=0){
			break;
		}
	}
	err = 0;

OUT_TX_SUBMIT:
OUT_PREP_DMA_SG:
	sg_free_table(dst_sg);
OUT_DST_SG_TO_SGL:
	devm_kfree(pDev, dst_sg);
OUT_DST_SG_ALLOC:
	sg_free_table(src_sg);
OUT_SRC_SG_TO_SGL:
	devm_kfree(pDev, src_sg);
OUT_SRC_SG_ALLOC:
	release_dma_channel(pDmaChan);

	return err;
}

static struct file_operations picCpy_fops =	
{
    .owner = THIS_MODULE,
    .unlocked_ioctl = device_ioctl,
    .open = device_open
};

static int __init picCpy_init (void)
{
	dev_t fpga_dev;
	struct device *dev;
	dma_addr_t dm;
	dma_cap_mask_t mask;
	int i;

	spin_lock_init(&dmalock);

	/*
	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY,mask);
	for(i=0;i<MAX_DMA_CHANNELS;i++)
	{
		g_dmaCh[i].use = 0;
		g_dmaCh[i].pChan = dma_request_channel(mask, 0, NULL);
		if(NULL==g_dmaCh[i].pChan)
		{
			printk(KERN_ERR "picCpy:dma_request_channel fail!\n");
			return -1;
		}
	}
	*/

	if(register_chrdev_region(MKDEV(DMA_MAJOR, DMA_MINOR), 1, DMA_NAME))
	{
	     printk (KERN_ERR "picCpy:alloc chrdev error.\n");
	     return -1;
	}

	dma_class = class_create(THIS_MODULE, DMA_NAME);  
    if(IS_ERR(dma_class)){  
        printk(KERN_ERR "picCpy:create class error\n");  
        return -1;  
    }  
  
    device_create(dma_class, NULL, MKDEV(DMA_MAJOR, DMA_MINOR), NULL, DMA_NAME);  

	dma_cdev=cdev_alloc();
	if(!dma_cdev)
	{
	    printk (KERN_ERR "picCpy:cdev alloc error.\n");
	     return -1;
	}
	dma_cdev->ops = &picCpy_fops;
	dma_cdev->owner = THIS_MODULE;

	if(cdev_add(dma_cdev,MKDEV(DMA_MAJOR, DMA_MINOR), 1))
	{
	    printk (KERN_ERR "picCpy:cdev add error.\n");
	     return -1;
	}

	printk(KERN_INFO "picCpy: driver loaded.\n");
  	return 0;
}

static void __exit picCpy_exit(void)
{
	printk (KERN_INFO "%s\n", __func__);
	cdev_del(dma_cdev);  
    device_destroy(dma_class, MKDEV(DMA_MAJOR, DMA_MINOR));  
    class_destroy(dma_class);  
    unregister_chrdev_region(MKDEV(DMA_MAJOR, DMA_MINOR), 1);  
}

late_initcall(picCpy_init);
module_exit(picCpy_exit);
MODULE_LICENSE("GPL");


