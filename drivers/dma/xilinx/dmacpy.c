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
#include <linux/wait.h>
#include <linux/delay.h>

#define DMA_MAJOR					200
#define DMA_MINOR 					0
#define DMA_NAME					"dmaCpy"
#define MAX_DMA_CHANNELS			4
#define MAX_SG_TRANSFER_SIZE		0x20000

enum
{
	DMA_FLAG_SRC_PHY_ADDR		= (1<<0),
	DMA_FLAG_DST_PHY_ADDR		= (1<<1),
	DMA_FLAG_SRC_ADDR_COHERENT	= (1<<2),
	DMA_FLAG_DST_ADDR_COHERENT	= (1<<3),
};

typedef struct
{
	volatile struct dma_chan *pChan;
	volatile int use;
}dmaCh_t;

typedef struct
{
	void *pSrcAddr;
	void *pDstAddr;
	uint32_t size;
	uint32_t flag;
}dma_ctrl_t;

static volatile dmaCh_t g_dmaCh[MAX_DMA_CHANNELS] = {0};
static spinlock_t dmalock;
static struct cdev *dma_cdev = NULL;
static struct class *dma_class = NULL;  
static volatile int gDmaChanRqtFlag = 0;

static DECLARE_WAIT_QUEUE_HEAD(dmacpy_queue);

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
	
//	printk(KERN_ERR "dmaCpy:no idle dma channel!\n");
	spin_unlock(&dmalock);
	return NULL;
}

static void release_dma_channel(dmaCh_t *pDmaCh)
{
	if(pDmaCh)
	{
		pDmaCh->use = 0;
		wake_up(&dmacpy_queue);
	}		
}

static void dma_complete_func(void *completion)
{	
	if(completion)
	{
		complete(completion);
	}
}

static long dma_transfer_coherent_to_coherent(dma_addr_t srcPhyAddr, dma_addr_t dstPhyAddr, size_t size)
{
	struct dma_device *dma_dev;
	dma_cookie_t cookie;
	enum dma_ctrl_flags flags;
	struct dma_async_tx_descriptor *tx1 = NULL;
	dmaCh_t *pDmaChan = NULL;
	volatile struct completion comp1;

	wait_event(dmacpy_queue, pDmaChan=get_idle_dma_channel());

	dma_dev = pDmaChan->pChan->device;
	flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
	tx1 = dma_dev->device_prep_dma_memcpy(pDmaChan->pChan, dstPhyAddr, srcPhyAddr, size, flags);
	if (!tx1) {
		printk(KERN_ERR "dmaCpy:Failed to prepare DMA memcpy\n");
		goto ERROR;
	}
	
	init_completion(&comp1);
	tx1->callback = dma_complete_func;
	tx1->callback_param = &comp1;
	cookie = tx1->tx_submit(tx1);
	if (dma_submit_error(cookie)) {
		printk(KERN_ERR "dmaCpy:Failed to do DMA tx_submit\n");
		goto ERROR;
	}

	dma_async_issue_pending(pDmaChan->pChan);
	wait_for_completion(&comp1);
	release_dma_channel(pDmaChan);
	return 0;
	
ERROR:
	release_dma_channel(pDmaChan);
	return -1;
}

static long dma_transfer_scatter_to_coherent(char *pSrcVirtAddr, dma_addr_t dstPhyAddr, size_t size)
{
	struct sg_table *src_sg;
	struct sg_table *dst_sg;
	int offset;
	unsigned int src_alloc_pages;
//	unsigned int dst_alloc_pages;
	unsigned long first, last;
	struct page **src_cache_pages;
//	struct page **dst_cache_pages;
	enum dma_data_direction direction = DMA_TO_DEVICE;
	volatile struct completion comp1;
	struct dma_async_tx_descriptor *txd = NULL;
	dma_cookie_t cookie;
	enum dma_ctrl_flags flags;
	int err = -EPERM;
	int ret;
	int i=0;
	dmaCh_t *pDmaChan = NULL;
	wait_event(dmacpy_queue, pDmaChan=get_idle_dma_channel());
	struct device *pDev = pDmaChan->pChan->device->dev;
	int timeout = 1000;
	
	//src_sg
	offset = offset_in_page(pSrcVirtAddr);
	first = ((unsigned long)pSrcVirtAddr & PAGE_MASK) >> PAGE_SHIFT;
	last = (((unsigned long)pSrcVirtAddr + size - 1) & PAGE_MASK) >> PAGE_SHIFT;
	src_alloc_pages = (last - first) + 1;
	src_cache_pages = devm_kzalloc(pDev, (src_alloc_pages * (sizeof(struct page *))), GFP_ATOMIC);
	if (!src_cache_pages) {
		printk(KERN_ERR "Unable to allocate memory for page table holder\n");
		err = -ENOMEM;
		goto OUT_SRC_CACHE_PAGES_ALLOC;
	}

	ret = get_user_pages_fast((unsigned long)pSrcVirtAddr, src_alloc_pages, !(direction), src_cache_pages);
	if (ret <= 0) {
		printk(KERN_ERR "Unable to pin user pages\n");
		err = -EPERM;
		goto OUT_SRC_PIN_PAGES;
	} else if (ret < src_alloc_pages) {
		printk(KERN_ERR "Only pinned few user pages %d\n", ret);
		err = -EPERM;
		for (i = 0; i < ret; i++)
			put_page(src_cache_pages[i]);
		goto OUT_SRC_PIN_PAGES;
	}

	src_sg = devm_kzalloc(pDev, sizeof(struct sg_table), GFP_ATOMIC);
	if (!src_sg) {
		err = -ENOMEM;
		goto OUT_SRC_SG_ALLOC;
	}

	ret = sg_alloc_table_from_pages(src_sg, src_cache_pages, src_alloc_pages, offset, size, GFP_ATOMIC);
	if (ret < 0) {
		printk(KERN_ERR "Unable to create src sg table\n");
		err = -EPERM;
		goto OUT_SRC_SG_TO_SGL;
	}

	ret = dma_map_sg(pDev, src_sg->sgl, src_sg->nents, direction);
	if (ret == 0) {
		printk(KERN_ERR "Unable to map buffer to src sg table\n");
		err = -EPERM;
		goto OUT_SRC_DMA_MAP_SG;
	}

	//dst_sg
	dst_sg = devm_kzalloc(pDev, sizeof(struct sg_table), GFP_ATOMIC);
	if (!dst_sg) {
		err = -ENOMEM;
		goto OUT_DST_SG_ALLOC;
	}

	ret = sg_alloc_table(dst_sg, 1, GFP_ATOMIC);
	if (ret < 0) {
		printk(KERN_ERR "Unable to create dst sg table\n");
		err = -EPERM;
		goto OUT_DST_SG_TO_SGL;
	}

	sg_dma_address(dst_sg->sgl) = dstPhyAddr;
	sg_dma_len(dst_sg->sgl) = size;

	//dma
	init_completion(&comp1);
	flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
	txd = pDmaChan->pChan->device->device_prep_dma_sg(pDmaChan->pChan, dst_sg->sgl,
						 dst_sg->nents,
						 src_sg->sgl,
						 src_sg->nents, flags);
	if (!txd) {
		printk(KERN_ERR "device_prep_dma_sg fail!\n");
		err = -EPERM;
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
	
	while((dmaengine_tx_status(pDmaChan->pChan, cookie, NULL)!=DMA_COMPLETE))
	{
		msleep(1);
		timeout--;
		if(0==timeout){
			err = -ETIME;
			printk(KERN_ERR "dmacpy timeout!\n");
			goto OUT_TX_SUBMIT;
		}
	}

	err = 0;

OUT_TX_SUBMIT:
OUT_PREP_DMA_SG:
	if (dst_sg)
		sg_free_table(dst_sg);
OUT_DST_SG_TO_SGL:
	if (dst_sg)
		devm_kfree(pDev, dst_sg);
	
OUT_DST_SG_ALLOC:
err_out_no_slave_sg_async_descriptor:
	dma_unmap_sg(pDev, src_sg->sgl, src_sg->nents, direction);
OUT_SRC_DMA_MAP_SG:
	sg_free_table(src_sg);
OUT_SRC_SG_TO_SGL:
	devm_kfree(pDev, src_sg);
OUT_SRC_SG_ALLOC:
	for (i = 0; i < src_alloc_pages; i++)
		put_page(src_cache_pages[i]);
OUT_SRC_PIN_PAGES:
	devm_kfree(pDev, src_cache_pages);

OUT_SRC_CACHE_PAGES_ALLOC:
	release_dma_channel(pDmaChan);
	
	return (long)err;	
}

static long dma_transfer_scatter_to_scatter(char *pSrcVirtAddr, char *pDstVirtAddr, size_t size)
{
	struct sg_table *src_sg;
	struct sg_table *dst_sg;
	int offset;
	unsigned int src_alloc_pages;
	unsigned int dst_alloc_pages;
	unsigned long first, last;
	struct page **src_cache_pages;
	struct page **dst_cache_pages;
	enum dma_data_direction direction = DMA_TO_DEVICE;
	volatile struct completion comp1;
	struct dma_async_tx_descriptor *txd = NULL;
	dma_cookie_t cookie;
	enum dma_ctrl_flags flags;
	int err = -EPERM;
	int ret;
	int i=0;
	dmaCh_t *pDmaChan = NULL;
	wait_event(dmacpy_queue, pDmaChan=get_idle_dma_channel());
	struct device *pDev = pDmaChan->pChan->device->dev;
	int timeout = 1000;
	
	//src_sg
	offset = offset_in_page(pSrcVirtAddr);
	first = ((unsigned long)pSrcVirtAddr & PAGE_MASK) >> PAGE_SHIFT;
	last = (((unsigned long)pSrcVirtAddr + size - 1) & PAGE_MASK) >> PAGE_SHIFT;
	src_alloc_pages = (last - first) + 1;
	src_cache_pages = devm_kzalloc(pDev, (src_alloc_pages * (sizeof(struct page *))), GFP_ATOMIC);
	if (!src_cache_pages) {
		printk(KERN_ERR "Unable to allocate memory for page table holder\n");
		err = -ENOMEM;
		goto OUT_SRC_CACHE_PAGES_ALLOC;
	}

	ret = get_user_pages_fast((unsigned long)pSrcVirtAddr, src_alloc_pages, !(direction), src_cache_pages);
	if (ret <= 0) {
		printk(KERN_ERR "Unable to pin user pages\n");
		err = -EPERM;
		goto OUT_SRC_PIN_PAGES;
	} else if (ret < src_alloc_pages) {
		printk(KERN_ERR "Only pinned few user pages %d\n", ret);
		err = -EPERM;
		for (i = 0; i < ret; i++)
			put_page(src_cache_pages[i]);
		goto OUT_SRC_PIN_PAGES;
	}

	src_sg = devm_kzalloc(pDev, sizeof(struct sg_table), GFP_ATOMIC);
	if (!src_sg) {
		err = -ENOMEM;
		goto OUT_SRC_SG_ALLOC;
	}

	ret = sg_alloc_table_from_pages(src_sg, src_cache_pages, src_alloc_pages, offset, size, GFP_ATOMIC);
	if (ret < 0) {
		printk(KERN_ERR "Unable to create src sg table\n");
		err = -EPERM;
		goto OUT_SRC_SG_TO_SGL;
	}

	ret = dma_map_sg(pDev, src_sg->sgl, src_sg->nents, direction);
	if (ret == 0) {
		printk(KERN_ERR "Unable to map buffer to src sg table\n");
		err = -EPERM;
		goto OUT_SRC_DMA_MAP_SG;
	}

	//dst_sg
	offset = offset_in_page(pDstVirtAddr);
	first = ((unsigned long)pDstVirtAddr & PAGE_MASK) >> PAGE_SHIFT;
	last = (((unsigned long)pDstVirtAddr + size - 1) & PAGE_MASK) >> PAGE_SHIFT;
	dst_alloc_pages = (last - first) + 1;
	dst_cache_pages = devm_kzalloc(pDev, (dst_alloc_pages * (sizeof(struct page *))), GFP_ATOMIC);
	if (!dst_cache_pages) {
		printk(KERN_ERR "Unable to allocate memory for page table holder\n");
		err = -ENOMEM;
		goto OUT_DST_CACHE_PAGES_ALLOC;
	}

	ret = get_user_pages_fast((unsigned long)pDstVirtAddr, dst_alloc_pages, !(direction), dst_cache_pages);
	if (ret <= 0) {
		printk(KERN_ERR "Unable to pin user pages\n");
		err = -EPERM;
		goto OUT_DST_PIN_PAGES;
	} else if (ret < dst_alloc_pages) {
		printk(KERN_ERR "Only pinned few user pages %d\n", ret);
		err = -EPERM;
		for (i = 0; i < ret; i++)
			put_page(dst_cache_pages[i]);
		goto OUT_DST_PIN_PAGES;
	}

	dst_sg = devm_kzalloc(pDev, sizeof(struct sg_table), GFP_ATOMIC);
	if (!dst_sg) {
		err = -ENOMEM;
		goto OUT_DST_SG_ALLOC;
	}

	ret = sg_alloc_table_from_pages(dst_sg, dst_cache_pages, dst_alloc_pages, offset, size, GFP_ATOMIC);
	if (ret < 0) {
		printk(KERN_ERR "Unable to create dst sg table\n");
		err = -EPERM;
		goto OUT_DST_SG_TO_SGL;
	}

	ret = dma_map_sg(pDev, dst_sg->sgl, dst_sg->nents, direction);
	if (ret == 0) {
		printk(KERN_ERR "Unable to map buffer to dst sg table\n");
		err = -EPERM;
		goto OUT_DST_DMA_MAP_SG;
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

#if 0
	if (dmaengine_tx_status(pDmaChan->pChan, cookie, NULL)!=DMA_COMPLETE){
		err = -EPERM;
		goto OUT_TX_SUBMIT;
	}
#else
	while((dmaengine_tx_status(pDmaChan->pChan, cookie, NULL)!=DMA_COMPLETE))
	{
		msleep(1);
		timeout--;
		if(0==timeout){
			err = -ETIME;
			printk(KERN_ERR "dmacpy timeout!\n");
			goto OUT_TX_SUBMIT;
		}
	}
#endif

	err = 0;

OUT_TX_SUBMIT:
OUT_PREP_DMA_SG:
	dma_unmap_sg(pDev, dst_sg->sgl, dst_sg->nents, direction);
OUT_DST_DMA_MAP_SG:
	sg_free_table(dst_sg);
OUT_DST_SG_TO_SGL:
	devm_kfree(pDev, dst_sg);
OUT_DST_SG_ALLOC:
	for (i = 0; i < dst_alloc_pages; i++)
		put_page(dst_cache_pages[i]);
OUT_DST_PIN_PAGES:
	devm_kfree(pDev, dst_cache_pages);
OUT_DST_CACHE_PAGES_ALLOC:

	dma_unmap_sg(pDev, src_sg->sgl, src_sg->nents, direction);
OUT_SRC_DMA_MAP_SG:
	sg_free_table(src_sg);
OUT_SRC_SG_TO_SGL:
	devm_kfree(pDev, src_sg);
OUT_SRC_SG_ALLOC:
	for (i = 0; i < src_alloc_pages; i++)
		put_page(src_cache_pages[i]);
OUT_SRC_PIN_PAGES:
	devm_kfree(pDev, src_cache_pages);
OUT_SRC_CACHE_PAGES_ALLOC:
	release_dma_channel(pDmaChan);
	
	return (long)err;	
}

static long dma_transfer_coherent_to_scatter(dma_addr_t srcPhyAddr, char *pDstVirtAddr, size_t size)
{
	struct sg_table *src_sg;
	struct sg_table *dst_sg;
	int offset;
//	unsigned int src_alloc_pages;
	unsigned int dst_alloc_pages;
	unsigned long first, last;
//	struct page **src_cache_pages;
	struct page **dst_cache_pages;
	enum dma_data_direction direction = DMA_TO_DEVICE;
	volatile struct completion comp1;
	struct dma_async_tx_descriptor *txd = NULL;
	dma_cookie_t cookie;
	enum dma_ctrl_flags flags;
	int err = -EPERM;
	int ret;
	int i=0;
	dmaCh_t *pDmaChan = NULL;
	wait_event(dmacpy_queue, pDmaChan=get_idle_dma_channel());
	struct device *pDev = pDmaChan->pChan->device->dev;
	int timeout = 1000;

	//src_sg
	src_sg = devm_kzalloc(pDev, sizeof(struct sg_table), GFP_ATOMIC);
	if (!src_sg) {
		err = -ENOMEM;
		goto OUT_SRC_SG_ALLOC;
	}

	ret = sg_alloc_table(src_sg, 1, GFP_ATOMIC);
	if (ret < 0) {
		printk(KERN_ERR "sg table for ep mem description failure\n");
		err = -EPERM;
		goto OUT_SRC_SG_TO_SGL;
	}

	sg_dma_address(src_sg->sgl) = srcPhyAddr;
	sg_dma_len(src_sg->sgl) = size;

	//dst_sg
	offset = offset_in_page(pDstVirtAddr);
	first = ((unsigned long)pDstVirtAddr & PAGE_MASK) >> PAGE_SHIFT;
	last = (((unsigned long)pDstVirtAddr + size - 1) & PAGE_MASK) >> PAGE_SHIFT;
	dst_alloc_pages = (last - first) + 1;
	dst_cache_pages = devm_kzalloc(pDev, (dst_alloc_pages * (sizeof(struct page *))), GFP_ATOMIC);
	if (!dst_cache_pages) {
		printk(KERN_ERR "Unable to allocate memory for page table holder\n");
		err = -ENOMEM;
		goto OUT_DST_CACHE_PAGES_ALLOC;
	}

	ret = get_user_pages_fast((unsigned long)pDstVirtAddr, dst_alloc_pages, !(direction), dst_cache_pages);
	if (ret <= 0) {
		printk(KERN_ERR "Unable to pin user pages\n");
		err = -EPERM;
		goto OUT_DST_PIN_PAGES;
	} else if (ret < dst_alloc_pages) {
		printk(KERN_ERR "Only pinned few user pages %d\n", ret);
		err = -EPERM;
		for (i = 0; i < ret; i++)
			put_page(dst_cache_pages[i]);
		goto OUT_DST_PIN_PAGES;
	}

	dst_sg = devm_kzalloc(pDev, sizeof(struct sg_table), GFP_ATOMIC);
	if (!dst_sg) {
		err = -ENOMEM;
		goto OUT_DST_SG_ALLOC;
	}

	ret = sg_alloc_table_from_pages(dst_sg, dst_cache_pages, dst_alloc_pages, offset, size, GFP_ATOMIC);
	if (ret < 0) {
		printk(KERN_ERR "Unable to create sg table\n");
		err = -EPERM;
		goto OUT_DST_SG_TO_SGL;
	}

	ret = dma_map_sg(pDev, dst_sg->sgl, dst_sg->nents, direction);
	if (ret == 0) {
		printk(KERN_ERR "Unable to map buffer to sg table\n");
		err = -EPERM;
		goto OUT_DST_DMA_MAP_SG;
	}
	

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

	while((dmaengine_tx_status(pDmaChan->pChan, cookie, NULL)!=DMA_COMPLETE))
	{
		msleep(1);
		timeout--;
		if(0==timeout){
			err = -ETIME;
			printk(KERN_ERR "dmacpy timeout!\n");
			goto OUT_TX_SUBMIT;
		}
	}

	err = 0;

OUT_TX_SUBMIT:
OUT_PREP_DMA_SG:
	dma_unmap_sg(pDev, dst_sg->sgl, dst_sg->nents, direction);
OUT_DST_DMA_MAP_SG:
	sg_free_table(dst_sg);
OUT_DST_SG_TO_SGL:
	devm_kfree(pDev, dst_sg);
OUT_DST_SG_ALLOC:
	for (i = 0; i < dst_alloc_pages; i++)
		put_page(dst_cache_pages[i]);
OUT_DST_PIN_PAGES:
	devm_kfree(pDev, dst_cache_pages);
OUT_DST_CACHE_PAGES_ALLOC:
	if (src_sg)
		sg_free_table(src_sg);
OUT_SRC_SG_TO_SGL:
	if (src_sg)
		devm_kfree(pDev, src_sg);
OUT_SRC_SG_ALLOC:	
	release_dma_channel(pDmaChan);
	
	return (long)err;	
}

static int device_open(struct inode *inode, struct file *file)
{
	dma_cap_mask_t mask;
	int i;

	spin_lock(&(dmalock));
	if(!gDmaChanRqtFlag){
		dma_cap_zero(mask);
		dma_cap_set(DMA_MEMCPY,mask);
		for(i=0;i<MAX_DMA_CHANNELS;i++)
		{
			g_dmaCh[i].use = 0;
			g_dmaCh[i].pChan = dma_request_channel(mask, 0, NULL);
			if(NULL==g_dmaCh[i].pChan)
			{
				printk(KERN_ERR "dmaCpy:dma_request_channel fail!\n");
				spin_unlock(&(dmalock));
				return -1;
			}
		}
		gDmaChanRqtFlag = 1;		
	}
	spin_unlock(&(dmalock));
	return 0;
}

static long device_ioctl(struct file *file,unsigned int num,unsigned long param)
{
	

	volatile struct completion comp1;
	dma_ctrl_t *pCtrlData = (dma_ctrl_t *)param;
	size_t size = (size_t)(pCtrlData->size);
	
	if((pCtrlData->flag & DMA_FLAG_SRC_ADDR_COHERENT) && (pCtrlData->flag & DMA_FLAG_DST_ADDR_COHERENT))	// CMA to CMA
	{
		dma_addr_t srcPhyAddr, dstPhyAddr;

		if(pCtrlData->flag & DMA_FLAG_SRC_PHY_ADDR)
		{
			srcPhyAddr = *(uint32_t *)(pCtrlData->pSrcAddr);
		}
		else
		{
			printk(KERN_ERR "dmaCpy: have not support src virtual address!\n");
			return -1;
		}

		if(pCtrlData->flag & DMA_FLAG_DST_PHY_ADDR)
		{
			dstPhyAddr = *(uint32_t *)(pCtrlData->pDstAddr);
		}
		else
		{
			printk(KERN_ERR "dmaCpy: have not support dst virtual address!\n");
			return -1;
		}

		return dma_transfer_coherent_to_coherent(srcPhyAddr, dstPhyAddr, size);
	}
	else if((!(pCtrlData->flag & DMA_FLAG_SRC_ADDR_COHERENT)) && (pCtrlData->flag & DMA_FLAG_DST_ADDR_COHERENT))
	{
		char *pSrcVirtAddr = NULL;
		dma_addr_t dstPhyAddr = 0;

		if(pCtrlData->flag & DMA_FLAG_SRC_PHY_ADDR)
		{
			printk(KERN_ERR "dmaCpy: have not support src phy address!\n");
			return -1;
		}
		else
		{
			pSrcVirtAddr = (char *)(pCtrlData->pSrcAddr);
		}

		if(pCtrlData->flag & DMA_FLAG_DST_PHY_ADDR)
		{
			dstPhyAddr = *(uint32_t *)(pCtrlData->pDstAddr);
		}
		else
		{
			printk(KERN_ERR "dmaCpy: have not support dst virt address!\n");
			return -1;
		}
			
		uint32_t remainSize = size;
		uint32_t sum = 0;
		uint32_t transferSize = 0;
		for(;;)
		{
			transferSize = (remainSize>=MAX_SG_TRANSFER_SIZE)?MAX_SG_TRANSFER_SIZE:remainSize;
			if(dma_transfer_scatter_to_coherent(pSrcVirtAddr + sum, dstPhyAddr + sum, transferSize)!=0){
				printk(KERN_ERR "dmaCpy: scatter to coherent fail!\n");
				return -1;
			}
			sum += transferSize;
			remainSize -= transferSize;
			if(!remainSize){
				return 0;
			}
		}
	}
	else if((!(pCtrlData->flag & DMA_FLAG_SRC_ADDR_COHERENT)) && (!(pCtrlData->flag & DMA_FLAG_DST_ADDR_COHERENT)))
	{
		char *pSrcVirtAddr = NULL;
		char *pDstVirtAddr = NULL;

		if(pCtrlData->flag & DMA_FLAG_SRC_PHY_ADDR)
		{
			printk(KERN_ERR "dmaCpy: have not support src phy address!\n");
			return -1;
		}
		else
		{
			pSrcVirtAddr = (char *)(pCtrlData->pSrcAddr);
		}

		if(pCtrlData->flag & DMA_FLAG_DST_PHY_ADDR)
		{
			printk(KERN_ERR "dmaCpy: have not support dst phy address!\n");
			return -1;
		}
		else
		{
			pDstVirtAddr = (char *)(pCtrlData->pDstAddr);
		}
			
		uint32_t remainSize = size;
		uint32_t sum = 0;
		uint32_t transferSize = 0;
		for(;;)
		{
			transferSize = (remainSize>=MAX_SG_TRANSFER_SIZE)?MAX_SG_TRANSFER_SIZE:remainSize;
			if(dma_transfer_scatter_to_scatter(pSrcVirtAddr + sum, pDstVirtAddr + sum, transferSize)!=0){
				printk(KERN_ERR "dmaCpy: scatter to scatter fail!\n");
				return -1;
			}
			sum += transferSize;
			remainSize -= transferSize;
			if(!remainSize){
				return 0;;
			}
		}
	}
	else if((pCtrlData->flag & DMA_FLAG_SRC_ADDR_COHERENT) && (!(pCtrlData->flag & DMA_FLAG_DST_ADDR_COHERENT)))
	{
		dma_addr_t srcPhyAddr = 0;
		char *pDstVirtAddr = NULL;

		if(pCtrlData->flag & DMA_FLAG_SRC_PHY_ADDR)
		{
			srcPhyAddr = *(uint32_t *)(pCtrlData->pSrcAddr);
		}
		else
		{
			printk(KERN_ERR "dmaCpy: have not support src phy address!\n");
			return -1;
		}

		if(pCtrlData->flag & DMA_FLAG_DST_PHY_ADDR)
		{
			printk(KERN_ERR "dmaCpy: have not support dst phy address!\n");
			return -1;
		}
		else
		{
			pDstVirtAddr = (char *)(pCtrlData->pDstAddr);
		}
			
		uint32_t remainSize = size;
		uint32_t sum = 0;
		uint32_t transferSize = 0;
		for(;;)
		{
			transferSize = (remainSize>=MAX_SG_TRANSFER_SIZE)?MAX_SG_TRANSFER_SIZE:remainSize;
			if(dma_transfer_coherent_to_scatter(srcPhyAddr + sum, pDstVirtAddr + sum, transferSize)!=0){
				printk(KERN_ERR "dmaCpy: coherent to scatter fail!\n");
				return -1;
			}
			sum += transferSize;
			remainSize -= transferSize;
			if(!remainSize){
				return 0;;
			}
		}
	}
	else
	{
		printk(KERN_ERR "dmaCpy: have not support other direction!\n");
	}	
	
	return -1;
}

static struct file_operations dmaCpy_fops =	
{
    .owner = THIS_MODULE,
    .unlocked_ioctl = device_ioctl,
    .open = device_open
};

static int __init dmaCpy_init (void)
{
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
			printk(KERN_ERR "dmaCpy:dma_request_channel fail! (## %d)\n", i);
			return -1;
		}
	}
	*/

	if(register_chrdev_region(MKDEV(DMA_MAJOR, DMA_MINOR), 1, DMA_NAME))
	{
	     printk (KERN_ERR "dmaCpy:alloc chrdev error.\n");
	     return -1;
	}

	dma_class = class_create(THIS_MODULE, DMA_NAME);  
    if(IS_ERR(dma_class)){  
        printk(KERN_ERR "dmaCpy:create class error\n");  
        return -1;  
    }  
  
    device_create(dma_class, NULL, MKDEV(DMA_MAJOR, DMA_MINOR), NULL, DMA_NAME);  

	dma_cdev=cdev_alloc();
	if(!dma_cdev)
	{
	    printk (KERN_ERR "dmaCpy:cdev alloc error.\n");
	     return -1;
	}
	dma_cdev->ops = &dmaCpy_fops;
	dma_cdev->owner = THIS_MODULE;

	if(cdev_add(dma_cdev,MKDEV(DMA_MAJOR, DMA_MINOR), 1))
	{
	    printk (KERN_ERR "dmaCpy:cdev add error.\n");
	     return -1;
	}

	printk(KERN_INFO "dmaCpy: driver loaded.\n");
  	return 0;
}

static void __exit dmaCpy_exit(void)
{
	printk (KERN_INFO "%s\n", __func__);
	cdev_del(dma_cdev);  
    device_destroy(dma_class, MKDEV(DMA_MAJOR, DMA_MINOR));  
    class_destroy(dma_class);  
    unregister_chrdev_region(MKDEV(DMA_MAJOR, DMA_MINOR), 1);  
}

late_initcall(dmaCpy_init);
module_exit(dmaCpy_exit);
MODULE_LICENSE("GPL");


