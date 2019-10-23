#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/debugfs.h>
#include <linux/mempolicy.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/cacheflush.h>
#include <linux/dma-mapping.h>
#include <linux/export.h>
#include <linux/gfp.h>
#include <linux/acpi.h>
#include <linux/bootmem.h>
#include <linux/cache.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/genalloc.h>
#include <linux/dma-mapping.h>
#include <linux/dma-contiguous.h>
#include <linux/cma.h>
#include <asm/io.h>
#include "cma_mem.h"

typedef struct cma_list_s
{
	unsigned long mem_base;
	unsigned long phy_base;
	unsigned int len;
	struct list_head list;
}cma_list_t;

static cma_list_t cma_list;
static struct cmamem_dev cmamem_dev;

static long cmamem_ioctl(struct file *file, unsigned int cmd, unsigned long arg) 
{
	unsigned long nr_pages;
	struct page *page;
	unsigned int pool_size_order;
	struct cmamem_info cma_info = {0};
	cma_list_t *pNode = NULL;
	int flag = 0;
	
	switch (cmd) {
	case CMEM_ALLOCATE:
		mutex_lock(&cmamem_dev.cmamem_lock);
		if (copy_from_user(&cma_info, (void __user *)arg, sizeof(struct cmamem_info))) {
            printk(KERN_ERR "CMEM_ALLOCATE: copy_from_user error\n");
            goto CMA_FAIL;
        }
        if(cma_info.version != CMA_MEM_VERSION) {
            printk(KERN_ERR "CMEM_ALLOCATE: kernel module version check fail, version % d\n", cma_info.version);
            goto CMA_FAIL;
        }

		pNode = kmalloc(sizeof(cma_list_t), GFP_KERNEL);
		if(NULL==pNode){
			printk(KERN_ERR "CMEM_ALLOCATE: kmalloc fail\n");
            goto CMA_FAIL;
		}

		nr_pages = cma_info.len	>> PAGE_SHIFT;
		pool_size_order = get_order(cma_info.len);
		page = dma_alloc_from_contiguous(NULL, nr_pages, pool_size_order, GFP_KERNEL);		

		if(!page) {
			printk(KERN_ERR "CMEM_ALLOCATE: dma_alloc_from_contiguous fail, len 0x%x\n", cma_info.len);
			goto CMA_FAIL;
		}

		cma_info.mem_base = (dma_addr_t)page_to_virt(page);
		cma_info.phy_base = (dma_addr_t)page_to_phys(page);
		pNode->len = cma_info.len;
		pNode->mem_base = cma_info.mem_base;
		pNode->phy_base = cma_info.phy_base;
		list_add(&(pNode->list), &(cma_list.list));
		if (copy_to_user((void __user *)arg, &cma_info, sizeof(struct cmamem_info))) {
			printk(KERN_ERR "CMEM_ALLOCATE: copy_to_user error\n");
			dma_release_from_contiguous(NULL, page, nr_pages);
			list_del(&(pNode->list));
			kfree(pNode);
			goto CMA_FAIL;
		}
#if 0
		list_for_each_entry(pNode, &(cma_list.list), list){
			printk(KERN_ERR "list <%lx><%d>\n", pNode->phy_base, pNode->len);
		}
#endif		
		mutex_unlock(&cmamem_dev.cmamem_lock);
		return 0;
	case CMEM_RELEASE:
		mutex_lock(&cmamem_dev.cmamem_lock);
		
		if (copy_from_user(&cma_info, (void __user *)arg, sizeof(struct cmamem_info))) {
			printk(KERN_ERR "CMEM_RELEASE: copy_from_user error\n");
			goto CMA_FAIL;
		}		
		if(cma_info.version != CMA_MEM_VERSION) {
            printk(KERN_ERR "CMEM_RELEASE: kernel module version check fail, version % d\n", cma_info.version);
            goto CMA_FAIL;
        }
		
		list_for_each_entry(pNode, &(cma_list.list), list){
			if(pNode->phy_base==cma_info.phy_base){
				flag = 1;
				break;
			}
		}

		if(0==flag){
			printk(KERN_ERR "CMEM_RELEASE: unknown CMA 0x%lx\n", cma_info.phy_base);
			goto CMA_FAIL;
		}

		page = phys_to_page(pNode->phy_base);
		nr_pages = cma_info.len	>> PAGE_SHIFT;
		dma_release_from_contiguous(NULL, page, nr_pages);
		list_del(&(pNode->list));
		kfree(pNode);
#if 0
		list_for_each_entry(pNode, &(cma_list.list), list){
			printk(KERN_ERR "list <%lx><%d>\n", pNode->phy_base, pNode->len);
		}
#endif
		mutex_unlock(&cmamem_dev.cmamem_lock);
		return 0;
	default:
		printk(KERN_INFO "cma mem not support command\n");
		return -EFAULT;
	}
CMA_FAIL:
    mutex_unlock(&cmamem_dev.cmamem_lock);
    return -EFAULT;
}

static int cmamem_mmap(struct file *filp, struct vm_area_struct *vma) {
	unsigned long start = vma->vm_start;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	unsigned long page, pos;
	cma_list_t *pNode = NULL;
	int flag = 0;

	list_for_each_entry(pNode, &(cma_list.list), list){
		if((offset==pNode->phy_base) && (size<=pNode->len)){
			flag = 1;
			break;
		}
	}

	if(0==flag){
		printk(KERN_ERR "cmamem_mmap: %s, memory have not be allocted or parameter overflow!\n", __func__);
		return -EINVAL;
	}

	pos = (unsigned long) pNode->phy_base;
	page = pos >> PAGE_SHIFT;
	if (remap_pfn_range(vma, start, page, size, PAGE_SHARED)){
		printk(KERN_ERR "cmamem_mmap: %s, mmap fail!\n", __func__);
		return -EAGAIN;
	}
		
	vma->vm_flags &= ~VM_IO;
	vma->vm_flags |= (VM_DONTEXPAND | VM_DONTDUMP);

	return 0;
}

static struct file_operations dev_fops = { 
	.owner = THIS_MODULE,
	.unlocked_ioctl = cmamem_ioctl, 
	.mmap = cmamem_mmap,
};

static int __init cmamem_init(void)
{
	printk(KERN_ERR "%s\n", __func__);
	mutex_init(&cmamem_dev.cmamem_lock);
	INIT_LIST_HEAD(&cma_list.list);
	cmamem_dev.dev.name = DEVICE_NAME;
	cmamem_dev.dev.minor = MISC_DYNAMIC_MINOR;
	cmamem_dev.dev.fops = &dev_fops;

	return misc_register(&cmamem_dev.dev);
}

static void __exit cmamem_exit(void)
{
	unsigned long nr_pages;
	struct page *page;
	cma_list_t *pNode = NULL;
	
	printk(KERN_ERR "%s\n", __func__);
	mutex_lock(&cmamem_dev.cmamem_lock);

	list_for_each_entry(pNode, &(cma_list.list), list){
		page = phys_to_page(pNode->mem_base);		
		nr_pages = pNode->len	>> PAGE_SHIFT;
		dma_release_from_contiguous(NULL, page, nr_pages);
	}	
	mutex_unlock(&cmamem_dev.cmamem_lock);
	misc_deregister(&cmamem_dev.dev);
}

module_init (cmamem_init);
module_exit (cmamem_exit);
MODULE_LICENSE("GPL");
