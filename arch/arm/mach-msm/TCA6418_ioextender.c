/* arch/arm/mach-msm/TCA6418_ioextender.c
 * Copyright (C) 2012 HTC Corporation.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
*/

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/workqueue.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/syscalls.h>
#include <mach/TCA6418_ioextender.h>
#include <mach/board_htc.h>
#include <asm/mach-types.h>
#include <linux/earlysuspend.h>
#include <linux/wakelock.h>
#include <linux/miscdevice.h>
#include <linux/irq.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include "proc_comm.h"

#define I2C_READ_RETRY_TIMES			10
#define I2C_WRITE_RETRY_TIMES			10
#define IOEXT_I2C_WRITE_BLOCK_SIZE		4

static struct i2c_client *private_ioext_client;

static int ioext_rw_delay;

static char *hex2string(uint8_t *data, int len)
{
	static char buf[IOEXT_I2C_WRITE_BLOCK_SIZE*4];
	int i;

	i = (sizeof(buf) - 1) / 4;
	if (len > i)
		len = i;

	for (i = 0; i < len; i++)
		sprintf(buf + i * 4, "[%02X]", data[i]);

	return buf;
}

static int i2c_read_ioextreg(struct i2c_client *client, uint8_t addr,
	uint8_t *data, int length)
{
	int retry;
	/*prepare the data which i2c_transfer needed*/
	struct ioext_i2c_client_data *cdata;
	struct i2c_msg msgs[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = 1,
			.buf = &addr,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = length,
			.buf = data,
		}
	};

	cdata = i2c_get_clientdata(client);
	mutex_lock(&cdata->ioext_i2c_rw_mutex);

	/*read data by i2c_transfer */
	for (retry = 0; retry <= I2C_READ_RETRY_TIMES; retry++) {
		if (i2c_transfer(client->adapter, msgs, 2) == 2)
			break;
		msleep(ioext_rw_delay);
	}

	mutex_unlock(&cdata->ioext_i2c_rw_mutex);
	dev_info(&client->dev, "R [%02X] = %s\n",
			addr, hex2string(data, length));

	if (retry > I2C_READ_RETRY_TIMES) {
		printk(KERN_INFO "%s(), [IOEXT_ERR] i2c_read_ioextreg retry over %d\n", __func__, I2C_READ_RETRY_TIMES);
		return -EIO;
	}

	return 0;
}

static int i2c_write_ioextreg(struct i2c_client *client, uint8_t addr,
	uint8_t *data, int length)
{
	int retry;
	uint8_t buf[6];
	int i;
	struct ioext_i2c_client_data *cdata;
	struct i2c_msg msg[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = length + 1,
			.buf = buf,
		}
	};

	dev_info(&client->dev, "W [%02X] = %s\n", addr, hex2string(data, length));

	/*setup client data*/
	cdata = i2c_get_clientdata(client);

	/*check data length*/
	if (length + 1 > IOEXT_I2C_WRITE_BLOCK_SIZE) {
		dev_err(&client->dev, "[IOEXT_ERR] i2c_write_ioextreg length too long\n");
		return -E2BIG;
	}

	/*setup data buffer which will be filled into register*/
	buf[0] = addr;
	for (i = 0; i < length; i++)
		buf[i+1] = data[i];

	mutex_lock(&cdata->ioext_i2c_rw_mutex);

	printk(KERN_INFO "%s(), i2c_transfer(addr:0x%x, len:%d)\n", __func__, msg->addr, msg->len);

	/*write data buffer into register*/
	for (retry = 0; retry <= I2C_WRITE_RETRY_TIMES; retry++) {
		if (i2c_transfer(client->adapter, msg, 1) == 1)
			break;
		msleep(ioext_rw_delay);
	}

	/*check if retry timeout*/
	if (retry > I2C_WRITE_RETRY_TIMES) {
		dev_err(&client->dev, "[IOEXT_ERR] i2c_write_ioextreg retry over %d\n",
			I2C_WRITE_RETRY_TIMES);
		mutex_unlock(&cdata->ioext_i2c_rw_mutex);
		return -EIO;
	}

	mutex_unlock(&cdata->ioext_i2c_rw_mutex);

	return 0;
}

int ioext_i2c_read(uint8_t addr, uint8_t *data, int length)
{
	struct i2c_client *client = private_ioext_client;

	/*printk(KERN_INFO "%s(), client = %u, client->addr:0x%x\n", __func__, (unsigned int) client, client->addr);*/

	if (!client)	{
		printk(KERN_ERR "[IOEXT_ERR] %s: dataset: client is empty\n", __func__);
		return -EIO;
	}

	/*printk(KERN_INFO "%s: i2c_read_ioextreg(client->addr:0x%x, addr:%x, len:%d)\n", __func__, client->addr, addr, length);*/

	if (i2c_read_ioextreg(client, addr, data, length) < 0)	{
		dev_err(&client->dev, "[IOEXT_ERR] %s: write ioext i2c fail\n", __func__);
		return -EIO;
	}

	return 0;
}

EXPORT_SYMBOL(ioext_i2c_read);

int ioext_i2c_write(uint8_t addr, uint8_t *data, int length)
{
	struct i2c_client *client = private_ioext_client;
	/*printk(KERN_INFO "%s(), client = 0x%x, client->addr:0x%x\n", __func__, (unsigned int)client, client->addr);*/

	/*check if client is empty*/
	if (!client) {
		printk(KERN_ERR "[IOEXT_ERR] %s: dataset: client is empty\n", __func__);
		return -EIO;
	}

	/*write data into register*/
	if (i2c_write_ioextreg(client, addr, data, length) < 0)	{
		dev_err(&client->dev, "[IOEXT_ERR] %s: write ioext i2c fail\n", __func__);
		return -EIO;
	}

	return 0;
}

EXPORT_SYMBOL(ioext_i2c_write);

int ioext_gpio_set_value(uint8_t gpio, uint8_t value)
{
	uint8_t addr;
	uint8_t rdata = 0;
	uint8_t wdata = 0;
	/* TCA6418 GPIO MAP */
	unsigned char TCA6418_GPIO_BITMAP[18]={7,6,5,4,3,2,1,0,0,1,2,3,4,5,6,7,0,1};

	struct ioext_i2c_client_data *cdata;
	struct i2c_client *client = private_ioext_client;

	/*check if client is null*/
	if (!client)	{
		printk(KERN_ERR "[IOEXT_ERR] %s: dataset: client is empty\n", __func__);
		return -EIO;
	}

	/*get clint data and lock resource*/
	cdata = i2c_get_clientdata(client);
	mutex_lock(&cdata->ioext_set_gpio_mutex);

	/*setup register*/
	if ( (gpio >= 0) && (gpio <= 7) )
		addr = TCA6418E_Reg_GPIO_DAT_OUT1;
	else if ( (gpio >= 8) && (gpio <= 15) )
		addr = TCA6418E_Reg_GPIO_DAT_OUT2;
	else if ( (gpio >= 16) && (gpio <= 17) )
		addr = TCA6418E_Reg_GPIO_DAT_OUT3;
	else {
		printk(KERN_ERR "[IOEXT_ERR] %s: Pin not support!\n", __func__);
		mutex_unlock(&cdata->ioext_set_gpio_mutex);
		return -1;
	}

	/*read register value*/
	if (ioext_i2c_read(addr, &rdata, 1)) {
		printk(KERN_ERR "[IOEXT_ERR] %s: readdata error, addr:0x%x\n", __func__, addr);
		mutex_unlock(&cdata->ioext_set_gpio_mutex);
		return -1;
	}

	/*fill gpio value into register value*/
	if (value)
		wdata = rdata | (1 << TCA6418_GPIO_BITMAP[gpio]);  	
	else
		wdata = rdata & ~(1 << TCA6418_GPIO_BITMAP[gpio]);  

	/*write register value into register*/
	if ( ioext_i2c_write(addr, &wdata, 1) ) {
		printk(KERN_ERR "[IOEXT_ERR] %s: writedata failed, addr:0x%x, data:0x%x\n", __func__, addr, wdata);
		mutex_unlock(&cdata->ioext_set_gpio_mutex);
		return -1;
	} else
		printk(KERN_INFO "[IOEXT_INFO] %s: OK, addr:0x%x, data=0x%x\n", __func__, addr, wdata);

	mutex_unlock(&cdata->ioext_set_gpio_mutex);
    return 0;
}

EXPORT_SYMBOL(ioext_gpio_set_value);


int ioext_gpio_get_value(uint8_t gpio)
{
	uint8_t addr;
	uint8_t rdata = 0;
	int get_value = 0;
	/* TCA6418 GPIO MAP */
	unsigned char TCA6418_GPIO_BITMAP[18]={7,6,5,4,3,2,1,0,0,1,2,3,4,5,6,7,0,1};

	struct ioext_i2c_client_data *cdata;
	struct i2c_client *client = private_ioext_client;
	if (!client)	{
		printk(KERN_ERR "[IOEXT_ERR] %s: dataset: client is empty\n", __func__);
		return -EIO;
	}

	cdata = i2c_get_clientdata(client);
	mutex_lock(&cdata->ioext_set_gpio_mutex);

	/*setup gpio number and register*/
	if ( (gpio >= 0) && (gpio <= 7) )
		addr = TCA6418E_Reg_GPIO_DAT_STAT1;
	else if ( (gpio >= 8) && (gpio <= 15) )
		addr = TCA6418E_Reg_GPIO_DAT_STAT2;
	else if ( (gpio >= 16) && (gpio <= 17) )
		addr = TCA6418E_Reg_GPIO_DAT_STAT3;
	else {
		printk(KERN_ERR "[IOEXT_ERR] %s: pin not support!\n", __func__);
		mutex_unlock(&cdata->ioext_set_gpio_mutex);
		return -1;
	}

	/*read register data*/
	if (ioext_i2c_read(addr, &rdata, 1)) {
		/*failed -> output register address*/
		printk(KERN_ERR "[IOEXT_ERR] %s: readdata error, addr:0x%x\n", __func__, addr);
		mutex_unlock(&cdata->ioext_set_gpio_mutex);
		return -1;
	} else {
		/*success*/
		get_value = (rdata >> TCA6418_GPIO_BITMAP[gpio]) & 0x1;
		/* printk(KERN_INFO "[IOEXT_INFO] %s: OK, addr:0x%x, rdata = %x, get_value:%d\n", __func__, addr, rdata, get_value); */
	}

	mutex_unlock(&cdata->ioext_set_gpio_mutex);
    return get_value;
}

EXPORT_SYMBOL(ioext_gpio_get_value);

int ioext_gpio_get_direction(uint8_t gpio)
{
	uint8_t addr;
	uint8_t rdata = 0;
	int get_direction = 0;
	/* TCA6418 GPIO MAP */
	unsigned char TCA6418_GPIO_BITMAP[18]={7,6,5,4,3,2,1,0,0,1,2,3,4,5,6,7,0,1};

	struct ioext_i2c_client_data *cdata;
	struct i2c_client *client = private_ioext_client;
	if (!client)	{
		printk(KERN_ERR "[IOEXT_ERR] %s: dataset: client is empty\n", __func__);
		return -EIO;
	}

	cdata = i2c_get_clientdata(client);
	mutex_lock(&cdata->ioext_set_gpio_mutex);

	/*setup gpio number and register*/
	if ( (gpio >= 0) && (gpio <= 7) )
		addr = TCA6418E_Reg_GPIO_DIR1;
	else if ( (gpio >= 8) && (gpio <= 15) )
		addr = TCA6418E_Reg_GPIO_DIR2;
	else if ( (gpio >= 16) && (gpio <= 17) )
		addr = TCA6418E_Reg_GPIO_DIR3;
	else {
		printk(KERN_ERR "[IOEXT_ERR] %s: pin not support!\n", __func__);
		mutex_unlock(&cdata->ioext_set_gpio_mutex);
		return -1;
	}

	/*read register data*/
	if (ioext_i2c_read(addr, &rdata, 1)) {
		/*failed -> output register address*/
		printk(KERN_ERR "[IOEXT_ERR] %s: readdata error, addr:0x%x\n", __func__, addr);
		mutex_unlock(&cdata->ioext_set_gpio_mutex);
		return -1;
	} else {
		/*success*/
		get_direction = (rdata >> TCA6418_GPIO_BITMAP[gpio]) & 0x1;
		/* printk(KERN_INFO "[IOEXT_INFO] %s: OK, addr:0x%x, rdata = %x, get_value:%d\n", __func__, addr, rdata, get_direction); */
	}

	mutex_unlock(&cdata->ioext_set_gpio_mutex);
    return get_direction;
}

EXPORT_SYMBOL(ioext_gpio_get_direction);

void ioext_print_gpio_status()
{
	int i = 0;
	int value;
	int direction;

	for (i = 0; i < TCA6418E_GPIO_NUM; i++)
	{
		value = ioext_gpio_get_value(i);
		direction = ioext_gpio_get_direction(i);
		printk(KERN_INFO "EXT_GPIO_%d: oe=%s, value=%d", i, direction ? "OU" : "IN", value);
	}

}

EXPORT_SYMBOL( ioext_print_gpio_status);

static int __devexit ioext_i2c_remove(struct i2c_client *client)
{
	struct ioext_i2c_platform_data *pdata;
	struct ioext_i2c_client_data *cdata;

	pdata = client->dev.platform_data;
	cdata = i2c_get_clientdata(client);

#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&cdata->early_suspend);
#endif

	if (client->irq)
		free_irq(client->irq, &client->dev);

	if (pdata->gpio_reset != 0)
		gpio_free(pdata->gpio_reset);

	kfree(cdata);

	return 0;
}

static int ioext_i2c_suspend(struct i2c_client *client,
	pm_message_t mesg)
{
	if (get_kernel_flag() & KERNEL_FLAG_PM_MONITOR) {
		ioext_print_gpio_status();
	}

	return 0;
}

static int ioext_i2c_resume(struct i2c_client *client)
{
	return 0;
}

static void register_ioext_devices(struct platform_device *devices, int num)
{
	int i;
	printk(KERN_INFO "%s()+, num:%d, private_ioext_client = 0x%x, addr:0x%x\n", __func__, num, (unsigned int)private_ioext_client, private_ioext_client->addr);

	for (i = 0; i < num; i++) {
		platform_device_register(devices + i);
		dev_set_drvdata(&(devices + i)->dev, private_ioext_client);
	}

	printk(KERN_INFO "%s()-, num:%d, i:%d\n", __func__, num, i);
}

static int ioext_i2c_probe(struct i2c_client *client
	, const struct i2c_device_id *id)
{
	struct ioext_i2c_platform_data *pdata;
	struct ioext_i2c_client_data *cdata;
	uint8_t data[6];
	int ret;
	printk(KERN_INFO "%s()+\n", __func__);

	memset(data, 0, sizeof(data));

	cdata = kzalloc(sizeof(struct ioext_i2c_client_data), GFP_KERNEL);
	if (!cdata) {
		ret = -ENOMEM;
		dev_err(&client->dev, "[IOEXT_PROBE_ERR] failed on allocat cdata\n");
		goto err_cdata;
	}

	i2c_set_clientdata(client, cdata);

	mutex_init(&cdata->ioext_i2c_rw_mutex);
	mutex_init(&cdata->ioext_set_gpio_mutex);

	private_ioext_client = client;

	printk(KERN_INFO "%s(), private_ioext_client = 0x%x\n", __func__, (unsigned int)private_ioext_client);

	pdata = client->dev.platform_data;
	if (!pdata) {
		ret = -EBUSY;
		dev_err(&client->dev, "[IOEXT_PROBE_ERR] failed on get pdata\n");
		goto err_exit;
	}
	pdata->dev_id = (void *)&client->dev;

	ioext_rw_delay = 5;

	printk(KERN_INFO "%s(), i2c_read_ioextreg(ChipID)\n", __func__);

	ret = i2c_read_ioextreg(client, IOEXTENDER_I2C_RCMD_VERSION, data, 1);
	if ((ret != 0) || !data[0]) {
		ret = -ENODEV;
		dev_err(&client->dev, "[IOEXT_PROBE_ERR] failed on get ioext version\n");
		goto err_exit;
	}
	dev_info(&client->dev, "IOExtender version [%02X]\n",
			data[0]);

	printk(KERN_INFO "%s(), pdata->gpio_reset:%d\n", __func__, pdata->gpio_reset);


	if (pdata->gpio_reset != 0) {
		ret = gpio_request(pdata->gpio_reset, "ADP5585-IOExtender");
		if (ret < 0) {
			dev_err(&client->dev, "[IOEXT_PROBE_ERR] failed on request gpio reset\n");
			goto err_exit;
		}
	}


	cdata->version = data[0];
	atomic_set(&cdata->ioext_is_suspend, 0);

	msleep(5);
	printk(KERN_INFO "%s(), i2c_read_ioextreg(IOEXTENDER_I2C_GPO_DATA_OUT, 2)\n", __func__);

	ret = i2c_read_ioextreg(client, IOEXTENDER_I2C_GPO_DATA_OUT, data, 2);
	dev_info(&client->dev, "IOEXT_I2C_GPO_DATA_OUT data[0]=[%02X], data[1]=[%02X]\n", data[0], data[1]);

	register_ioext_devices(pdata->ioext_devices, pdata->num_devices);

	printk(KERN_INFO "%s()-, OK\n", __func__);

	return 0;

err_exit:

err_cdata:	
	printk(KERN_INFO "%s()-, FAIL!\n", __func__);
	return ret;

}

static const struct i2c_device_id ioext_i2c_id[] = {
	{ IOEXTENDER_I2C_NAME, 0 },
	{ }
};

static struct i2c_driver ioext_i2c_driver = {
	.driver = {
		   .name = IOEXTENDER_I2C_NAME,
		   },
	.id_table = ioext_i2c_id,
	.probe = ioext_i2c_probe,
	.suspend = ioext_i2c_suspend,
	.resume = ioext_i2c_resume,
	.remove = __devexit_p(ioext_i2c_remove),
};

static int __init IO_Extender_init(void)
{
	int ret;
	printk(KERN_INFO "%s()+\n", __func__);

	ret = i2c_add_driver(&ioext_i2c_driver);

	printk(KERN_INFO "%s()-\n", __func__);

	ioext_gpio_set_value(11,0);

	if (ret)
		return ret;
	return 0;
}

static void __exit IO_Extender_exit(void)
{
	i2c_del_driver(&ioext_i2c_driver);
}

module_init(IO_Extender_init);
module_exit(IO_Extender_exit);

MODULE_AUTHOR("Joner Lin <Joner_Lin@htc.com>");
MODULE_DESCRIPTION("IO Entender driver");
MODULE_LICENSE("GPL");
