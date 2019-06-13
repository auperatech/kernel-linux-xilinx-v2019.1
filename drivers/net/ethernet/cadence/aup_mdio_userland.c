/*
 * Auperator MDIO Userland Procedure-Interface
 *
 * (C) 2017.09 <buddy.zhang@aliyun.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/stat.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/of_gpio.h>

#include <linux/err.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <asm/uaccess.h>
#include <linux/bitops.h>
#include <linux/phy.h>

#define DEV_NAME "mdio-userland"

#ifndef UINT64_MAX
#define UINT64_MAX		(u64)(~((u64)0))
#endif

struct mv88e6185_hw_stat {
        char string[ETH_GSTRING_LEN];
        int sizeof_stat;
        int reg;
        int type;
};

#define STATS_TYPE_PORT         BIT(0)
#define STATS_TYPE_BANK0        BIT(1)
#define STATS_TYPE_BANK1        BIT(2)

struct mii_bus *aup_mdio = NULL;

/* Ingood array */
static uint64_t ingood_array[32];

static struct mv88e6185_hw_stat mv88e6185_hw_stats[] = {
	{ "in_good_octets",		8, 0x00, STATS_TYPE_BANK0, },
	{ "in_bad_octets",		4, 0x02, STATS_TYPE_BANK0, },
	{ "in_unicast",			4, 0x04, STATS_TYPE_BANK0, },
	{ "in_broadcasts",		4, 0x06, STATS_TYPE_BANK0, },
	{ "in_multicasts",		4, 0x07, STATS_TYPE_BANK0, },
	{ "in_pause",			4, 0x16, STATS_TYPE_BANK0, },
	{ "in_undersize",		4, 0x18, STATS_TYPE_BANK0, },
	{ "in_fragments",		4, 0x19, STATS_TYPE_BANK0, },
	{ "in_oversize",		4, 0x1a, STATS_TYPE_BANK0, },
	{ "in_jabber",			4, 0x1b, STATS_TYPE_BANK0, },
	{ "in_rx_error",		4, 0x1c, STATS_TYPE_BANK0, },
	{ "in_fcs_error",		4, 0x1d, STATS_TYPE_BANK0, },
	{ "out_octets",			8, 0x0e, STATS_TYPE_BANK0, },
	{ "out_unicast",		4, 0x10, STATS_TYPE_BANK0, },
	{ "out_broadcasts",		4, 0x13, STATS_TYPE_BANK0, },
	{ "out_multicasts",		4, 0x12, STATS_TYPE_BANK0, },
	{ "out_pause",			4, 0x15, STATS_TYPE_BANK0, },
	{ "excessive",			4, 0x11, STATS_TYPE_BANK0, },
	{ "collisions",			4, 0x1e, STATS_TYPE_BANK0, },
	{ "deferred",			4, 0x05, STATS_TYPE_BANK0, },
	{ "single",			4, 0x14, STATS_TYPE_BANK0, },
	{ "multiple",			4, 0x17, STATS_TYPE_BANK0, },
	{ "out_fcs_error",		4, 0x03, STATS_TYPE_BANK0, },
	{ "late",			4, 0x1f, STATS_TYPE_BANK0, },
	{ "hist_64bytes",		4, 0x08, STATS_TYPE_BANK0, },
	{ "hist_65_127bytes",		4, 0x09, STATS_TYPE_BANK0, },
	{ "hist_128_255bytes",		4, 0x0a, STATS_TYPE_BANK0, },
	{ "hist_256_511bytes",		4, 0x0b, STATS_TYPE_BANK0, },
	{ "hist_512_1023bytes",		4, 0x0c, STATS_TYPE_BANK0, },
	{ "hist_1024_max_bytes",	4, 0x0d, STATS_TYPE_BANK0, },
	{ "sw_in_discards",		4, 0x10, STATS_TYPE_PORT, },
	{ "sw_in_filtered",		2, 0x12, STATS_TYPE_PORT, },
	{ "sw_out_filtered",		2, 0x13, STATS_TYPE_PORT, },
	{ "in_discards",		4, 0x00, STATS_TYPE_BANK1, },
	{ "in_filtered",		4, 0x01, STATS_TYPE_BANK1, },
	{ "in_accepted",		4, 0x02, STATS_TYPE_BANK1, },
	{ "in_bad_accepted",		4, 0x03, STATS_TYPE_BANK1, },
	{ "in_good_avb_class_a",	4, 0x04, STATS_TYPE_BANK1, },
	{ "in_good_avb_class_b",	4, 0x05, STATS_TYPE_BANK1, },
	{ "in_bad_avb_class_a",		4, 0x06, STATS_TYPE_BANK1, },
	{ "in_bad_avb_class_b",		4, 0x07, STATS_TYPE_BANK1, },
	{ "tcam_counter_0",		4, 0x08, STATS_TYPE_BANK1, },
	{ "tcam_counter_1",		4, 0x09, STATS_TYPE_BANK1, },
	{ "tcam_counter_2",		4, 0x0a, STATS_TYPE_BANK1, },
	{ "tcam_counter_3",		4, 0x0b, STATS_TYPE_BANK1, },
	{ "in_da_unknown",		4, 0x0e, STATS_TYPE_BANK1, },
	{ "in_management",		4, 0x0f, STATS_TYPE_BANK1, },
	{ "out_queue_0",		4, 0x10, STATS_TYPE_BANK1, },
	{ "out_queue_1",		4, 0x11, STATS_TYPE_BANK1, },
	{ "out_queue_2",		4, 0x12, STATS_TYPE_BANK1, },
	{ "out_queue_3",		4, 0x13, STATS_TYPE_BANK1, },
	{ "out_queue_4",		4, 0x14, STATS_TYPE_BANK1, },
	{ "out_queue_5",		4, 0x15, STATS_TYPE_BANK1, },
	{ "out_queue_6",		4, 0x16, STATS_TYPE_BANK1, },
	{ "out_queue_7",		4, 0x17, STATS_TYPE_BANK1, },
	{ "out_cut_through",		4, 0x18, STATS_TYPE_BANK1, },
	{ "out_octets_a",		4, 0x1a, STATS_TYPE_BANK1, },
	{ "out_octets_b",		4, 0x1b, STATS_TYPE_BANK1, },
	{ "out_management",		4, 0x1f, STATS_TYPE_BANK1, },
};

static int mv88e6185_g1_stats_wait(void);

static void mv88e6185_g1_stats_read(int stat, u32 *val)
{
        u32 value;
        u16 reg;
	int err;

        *val = 0;

        err = aup_mdio->write(aup_mdio, 0x1B, 0x1d, 0x8000 | 0x4000 | stat);
        if (err)
                return;

        err = mv88e6185_g1_stats_wait();
        if (err)
                return;

        reg = aup_mdio->read(aup_mdio, 0x1B, 0x1e);

        value = reg << 16;

        reg = aup_mdio->read(aup_mdio, 0x1B, 0x1F);

        *val = value | reg;
}


static uint64_t mv88e6185_get_ethtool_stat(struct mv88e6185_hw_stat *s,
                      int port, u16 bank1_select, u16 histogram)
{
        u32 low;
        u32 high = 0;
        u16 reg = 0;
        u64 value;

        switch (s->type) {
        case STATS_TYPE_PORT:
                reg = aup_mdio->read(aup_mdio, port + 0x10, s->reg);

                low = reg;
                if (s->sizeof_stat == 4) {
                        reg = aup_mdio->read(aup_mdio, port + 0x10, s->reg + 1);
                        high = reg;
                }
                break;
        case STATS_TYPE_BANK1:
                reg = bank1_select;
                /* fall through */
        case STATS_TYPE_BANK0:
                reg |= s->reg | histogram;
                mv88e6185_g1_stats_read(reg, &low);
                if (s->sizeof_stat == 8)
                        mv88e6185_g1_stats_read(reg + 1, &high);
                break;
        default:
                return UINT64_MAX;
        }
        value = (((u64)high) << 16) | low;
        return value;
}


static void mv88e61xx_stats_get_stats(int port, uint64_t *data, int types,
                                u16 bank1_select, u16 histogram)
{
    struct mv88e6185_hw_stat *stat;
    int i, j;

    for (i = 0, j = 0; i < ARRAY_SIZE(mv88e6185_hw_stats); i++) {
        stat = &mv88e6185_hw_stats[i];
        if (stat->type & types) {
            data[j] = mv88e6185_get_ethtool_stat(stat, port, bank1_select, histogram);
            j++;
        }
    }
}

static void mv88e6185_stats_get_stats(int port, uint64_t *data)
{
        return mv88e61xx_stats_get_stats(port, data, STATS_TYPE_BANK0 | STATS_TYPE_PORT |
                                       STATS_TYPE_BANK1, 0, 0x0c00);
}

static int mv88e6185_wait(int addr, int reg, u16 mask)
{
    int i;

    for (i = 0; i < 16; i++) {
        u16 val;

        val = aup_mdio->read(aup_mdio, addr, reg);

        if (!(val & mask))
            return 0;

        mdelay(500);
    }

    printk("Timeout while waiting for switch\n");
    return -1;
}


static int mv88e6185_g1_stats_wait(void)
{
    return mv88e6185_wait(0x1B, 0x1d, 0x8000);
}

static int mv88e6185_g1_stats_snapshot(int port)
{
    /* Snapshot the hardware statistics counters for this port. */
    aup_mdio->write(aup_mdio, 0x1B, 0x1d, 0x8000 | 0x5000 | 0x0c00 | port);

    /* Wait for the snapshotting to complete. */
    return mv88e6185_g1_stats_wait();
}

/* Parse input string */
static int parse_input_string(const char *string, int *value, int *flag)
{
    int nr;
    char *tmp;
    char *buffer, *leg;
    int i = 0; /* Value[0]: DeviceAddress. Value[1]: Register. Value[2]: date in Byte. */

    /* Read/Write operation 
     *  CMD: <r/w>,<DevAddr>,<RegAddr>,[Value],
     *       r: Read special PHY/SERDES register
     *       w: Write data to special PHY/SERDES register.
     *       DevAddr: Device address 
     *       RegAddr: Register address
     *       Value:   value what to write.
     */

    buffer = (char *)kmalloc(strlen(string) + 1, GFP_KERNEL);
    leg = buffer;
    memset(buffer, 0, strlen(string) + 1);
    /* Copy original data */
    strcpy(buffer, string);

    while ((tmp = strstr(buffer, ","))) {
        int data;
        char dd[20];

        nr = tmp - buffer;
        tmp++;
        strncpy(dd, buffer, nr);
        dd[nr] = '\0';
        if (strncmp(dd, "r", 1) == 0) {
            *flag = 1;
        } else if (strncmp(dd, "w", 1) == 0) {
            *flag = 2;
        } else {
            sscanf(dd, "%d", &data);
            value[i++] = data;
        }
        buffer = tmp;
    }
    kfree(leg);
    return 0;
}

/* Obtain 88E6185 Switch Port Register */
static ssize_t Switch_PortReg_show(struct device *dev,
                    struct device_attribute *attr, char *buf)
{
    ssize_t size = 0;
    int i, j;

    /* Dump 88E6185 Switch Port Register */
    for (i = 0; i < 14; i++) {
        //if (i > 6 && i < 11)            continue;
        if (i < 10)
            printk("\nPort %d:\n", i);
        else {
            if (i == 11)
                printk("\nGlobal 1 (0x1B):\n");
            else if (i == 12)
                printk("\nGlobal 2 (0x1C):\n");
            else if (i == 13)
                printk("\nGlobal 3 (0x1D):\n");
            else
                printk("\nUnknown register!\n");
        }
        for (j = 0; j < 32; j++) {
            unsigned short reg;

            if (((j % 16) == 0) && (j != 0))
                printk("\n");
            reg = aup_mdio->read(aup_mdio, i, j);
            printk("%#04x ", reg);
        }
    }
    printk("\n");
    return size;
}

/* Read/Write 88E6185 Switch */
static ssize_t Switch_PortReg_store(struct device *dev,
            struct device_attribute *attr, const char *buf, size_t size)
{
    int flag = 0; /* 1: read 2: write */
    int value[10];

    parse_input_string(buf, value, &flag);
    /* Read data from Port-Register */
    if (flag == 1) {
        unsigned short reg = 0;

        reg = aup_mdio->read(aup_mdio, value[0], value[1]);
        printk("\r\nRead: Port - Dev[%#x] Reg[%#x] Value[%#04x]\n", 
                             value[0], value[1], reg & 0xFFFF);
    } else if (flag == 2) { /* Write data to Port-Register */

        printk("\r\nWrite: Port - Dev[%#x] Reg[%#x] value[%#04x]\n", 
                             value[0], value[1], value[2] & 0xFFFF);
        aup_mdio->write(aup_mdio, value[0], value[1], value[2]);
    } else {
        printk("Unknown operation from Port register\n");
    }
 
    return size;
}

/* Hardware reset 88e6185 then re-init*/
void aup_reset_6185_gpio_reinit(void)
{
    int gpio = 410;	//for gpio 72, base is 308, connect to 6185 hard reset pin
    unsigned short reg;


    gpio_direction_output(gpio, 1);
    gpio_set_value(gpio, 0);
    mdelay(1000);
    gpio_set_value(gpio, 1);
    mdelay(1000);

    /*port 6/7/8/9 enable forwarding*/
    aup_mdio->write(aup_mdio, 0x17, 0x4, 0x77);
    aup_mdio->write(aup_mdio, 0x18, 0x4, 0x77);
    aup_mdio->write(aup_mdio, 0x19, 0x4, 0x77);
    aup_mdio->write(aup_mdio, 0x16, 0x4, 0x77);
    /*1000basex uplink port 7/8 link auto nego*/
    reg = aup_mdio->read(aup_mdio, 0x18, 0x1);
    reg = (reg | 0x600) & 0xffff;	//PCS Inband Auto-Negotiation Enable, and Restart PCS Inband Auto-Negotiation
    aup_mdio->write(aup_mdio, 0x18, 0x1, reg);
    reg = aup_mdio->read(aup_mdio, 0x17, 0x1);
    reg = (reg | 0x600) & 0xffff;	//PCS Inband Auto-Negotiation Enable, and Restart PCS Inband Auto-Negotiation
    aup_mdio->write(aup_mdio, 0x17, 0x1, reg);
    /*cpu port 6/9 set to auto nego then fixed link*/
    aup_mdio->write(aup_mdio, 0x16, 0x1, 0x83E);
    aup_mdio->write(aup_mdio, 0x19, 0x1, 0x83E);
    mdelay(500);
    aup_mdio->write(aup_mdio, 0x16, 0x1, 0x603);
    aup_mdio->write(aup_mdio, 0x19, 0x1, 0x603);
    mdelay(500);
    aup_mdio->write(aup_mdio, 0x16, 0x1, 0x83E);
    aup_mdio->write(aup_mdio, 0x19, 0x1, 0x83E);
    mdelay(500);
    aup_mdio->write(aup_mdio, 0x16, 0x1, 0x3E);
    aup_mdio->write(aup_mdio, 0x19, 0x1, 0x3E);
    mdelay(500);
    /*soft reset*/
    aup_mdio->write(aup_mdio, 0x1B, 0x4, 0xC000);
    mdelay(500);
    printk("Hard-Reset-88e6185\n");
}

/* MV88E6185 Switch init */
static ssize_t Switch_init_store(struct device *dev,
            struct device_attribute *attr, const char *buf, size_t size)
{
    //not enable// aup_reset_6185_gpio_reinit();
    return size;
}

/* 88E6185 XXX init */
static ssize_t Switch_init_show(struct device *dev,
                    struct device_attribute *attr, char *buf)
{
    return 0;
}

static ssize_t MIB_store(struct device *dev,
            struct device_attribute *attr, const char *buf, size_t size)
{
    /* Clear Port#8 */
    aup_mdio->write(aup_mdio, 0x1B, 0x1d, 0x8000 | 0x2000 | 0x0c00 | 0x8);
    /* Wait for the snapshotting to complete. */
    mv88e6185_g1_stats_wait();

    /* Clear Port#9 */
    aup_mdio->write(aup_mdio, 0x1B, 0x1d, 0x8000 | 0x2000 | 0x0c00 | 0x9);
    /* Wait for the snapshotting to complete. */
    mv88e6185_g1_stats_wait();
    printk("Flush Port#8 and Port#9\n");

    return size;
}

/* 88E6185 XXX init */
static ssize_t MIB_show(struct device *dev,
                    struct device_attribute *attr, char *buf)
{
    int ports[] = {0x8, 0x9};
    int i, j;
    int ret;
    uint64_t *data;

    data = (uint64_t *)kmalloc(sizeof(uint64_t) * 128, GFP_KERNEL);
    if (!data) {
        printk("Unable to obtain free memory\n");
        return 0;
    }

    for (i = 0; i < 2; i++) {
        ret = mv88e6185_g1_stats_snapshot(ports[i]);
        if (ret < 0) {
            printk("Unable to set g1 snapshot\n");
            kfree(data);
            return 0;
        }
        /* Get ethtool stats */
        mv88e6185_stats_get_stats(ports[i], data);
        printk("\n*****Port#%d*******\n", ports[i]);
        /* Dump all status */
        for (j = 0; j < ARRAY_SIZE(mv88e6185_hw_stats); j++)
            printk("[%#2x] %s: %#x\n", j, mv88e6185_hw_stats[j].string, data[j]);
    }
    kfree(data); 

    return 0;
}

unsigned int ingood_port = 0x8;

static uint64_t ingood_data(void)
{
    int ret;
    
    /* Snapshot Port */
    ret = mv88e6185_g1_stats_snapshot(ingood_port);
    if (ret < 0) {
        printk("Unable to set g1 snapshot\n");
        return 0;
    }
    
    return mv88e6185_get_ethtool_stat(&mv88e6185_hw_stats[0], ingood_port, 0, 0x0C00);
}

static ssize_t ingood_store(struct device *dev,
            struct device_attribute *attr, const char *buf, size_t size)
{
    sscanf(buf, "%x", &ingood_port);
 
    ingood_port %= 10;		//for 88e6185 have only 10 ports

    return size;
}

/* 88E6185 XXX init */
static ssize_t ingood_show(struct device *dev,
                    struct device_attribute *attr, char *buf)
{
    uint64_t data0, data1;

    data0 = ingood_data();
    data1 = ingood_array[ingood_port];
    ingood_array[ingood_port] = data0;

    return sprintf(buf, "%llu", data0 - data1);

}

/* initia ingood array */
int ingood_init(void)
{
    int i;

    for (i = 0; i < 10; i++) {
        ingood_port = i;
        ingood_array[ingood_port] = ingood_data();
    }

    return 0;
}

/* phy_name store */
static ssize_t phy_name_store(struct device *dev,
            struct device_attribute *attr, const char *buf, size_t size)
{
    return size;
}

unsigned int aup_phy_name = 0;

/* phy_name show */
static ssize_t phy_name_show(struct device *dev,
                    struct device_attribute *attr, char *buf)
{
	char *name;

	if (aup_phy_name == 0x6185)
		name = "MV6185";
	else if (aup_phy_name == 0x1112)
		name = "MV1112";
	else if	(aup_phy_name == 0x1512)
		name = "MV1512";
	else
		name = "invalide PHY";

	return sprintf(buf, "%s", name);

}

static struct device_attribute Switch_init_attr = 
       __ATTR_RW(Switch_init);

static struct device_attribute Switch_PortReg_attr = 
       __ATTR_RW(Switch_PortReg);

static struct device_attribute MIB_attr = 
       __ATTR_RW(MIB);

static struct device_attribute ingood_attr = 
       __ATTR_RW(ingood);

static struct device_attribute phy_name_attr =
       __ATTR_RW(phy_name);

/* probe platform driver */
static int reset_probe(struct platform_device *pdev)
{
    int err;

    err = device_create_file(&pdev->dev, &Switch_init_attr);
    if (err) {
        printk("Unable to create device file for Switch init.\n");
        return -EINVAL;
    }
    err = device_create_file(&pdev->dev, &Switch_PortReg_attr);
    if (err) {
        printk("Unable to create device file for Switch Port Register.\n");
        return -EINVAL;
    }
    err = device_create_file(&pdev->dev, &MIB_attr);
    if (err) {
        printk("Unable to create device file for MIB.\n");
        return -EINVAL;
    }
    err = device_create_file(&pdev->dev, &ingood_attr);
    if (err) {
        printk("Unable to create device file for ingood.\n");
        return -EINVAL;
    }
    err = device_create_file(&pdev->dev, &phy_name_attr);
    if (err) {
        printk("Unable to create device file for phy_mode.\n");
        return -EINVAL;
    }
    return 0;
}

/* remove platform driver */
static int reset_remove(struct platform_device *pdev)
{
    device_remove_file(&pdev->dev, &phy_name_attr);
    device_remove_file(&pdev->dev, &ingood_attr);
    device_remove_file(&pdev->dev, &MIB_attr);
    device_remove_file(&pdev->dev, &Switch_init_attr);
    device_remove_file(&pdev->dev, &Switch_PortReg_attr);
    /* Remove hardware */
    return 0;
}

/* platform device information */
static struct platform_device reset_device = {
    .name = DEV_NAME,  /* Same as driver name */
    .id   = -1,
};

/* platform driver information */
static struct platform_driver reset_driver = {
    .probe  = reset_probe,
    .remove = reset_remove,
    .driver = {
        .name = DEV_NAME, /* Same as device name */
    }, 
};

/* exit .. */
static __exit void reset_plat_exit(void)
{
    platform_driver_unregister(&reset_driver);
    platform_device_unregister(&reset_device);
}

/* init entry .. */
static __init int reset_plat_init(void)
{
    int ret;


    /* register device */
    ret = platform_device_register(&reset_device);
    if (ret)
        return ret;    
    
    /* register driver */
    return platform_driver_register(&reset_driver);
}
device_initcall(reset_plat_init); /* last invoke */
