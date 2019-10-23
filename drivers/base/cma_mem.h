#ifndef _CMA_MEM_H_
#define _CMA_MEM_H_

#define CMA_MEM_VERSION		3
#define DEVICE_NAME		"cma_mem" 
#define CMEM_IOCTL_MAGIC 'm'
#define CMEM_ALLOCATE		_IOW(CMEM_IOCTL_MAGIC, 1, unsigned int)
#define CMEM_RELEASE		_IOW(CMEM_IOCTL_MAGIC, 2, unsigned int)

struct cmamem_info {
	unsigned int version;
        unsigned int len;
	unsigned int offset;
	unsigned long mem_base;
	unsigned long phy_base;	
};

struct cmamem_dev {
	struct miscdevice dev;
	struct mutex cmamem_lock;
	struct list_head info_list;
};

struct current_status {
	int status;
	dma_addr_t phy_base;
};

#endif
