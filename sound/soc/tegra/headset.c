/*
 *  Headset device detection driver.
 *
 * Copyright (C) 2011 ASUSTek Corporation.
 *
 * Authors:
 *  Jason Cheng <jason4_cheng@asus.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/types.h>
#include <linux/input.h>
#include <linux/mutex.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/hrtimer.h>
#include <linux/timer.h>
#include <linux/switch.h>
#include <linux/input.h>
#include <linux/slab.h>
#include <linux/wakelock.h>
#include <asm/gpio.h>
#include <asm/uaccess.h>
#include <asm/string.h>
#include <sound/soc.h>
#include "../gpio-names.h"
#include "../codecs/rt5639.h"
#include <mach/pinmux.h>
#include "../board.h"
#include "../../../arch/arm/mach-tegra/include/mach/gpio-tegra.h"
#include <asm/mach-types.h>
MODULE_DESCRIPTION("Headset detection driver");
MODULE_LICENSE("GPL");


/*----------------------------------------------------------------------------
** FUNCTION DECLARATION
**----------------------------------------------------------------------------*/
static int   __init     	headset_init(void);
static void __exit    headset_exit(void);
static irqreturn_t   	detect_irq_handler(int irq, void *dev_id);
static void 		detection_work(struct work_struct *work);
static int               	jack_config_gpio(void);
static irqreturn_t   	lineout_irq_handler(int irq, void *dev_id);
static void 		lineout_work_queue(struct work_struct *work);
static int               	lineout_config_gpio(void);
static void 		detection_work(struct work_struct *work);
static int               	btn_config_gpio(void);
int 			hs_micbias_power(int on);
/*----------------------------------------------------------------------------
** GLOBAL VARIABLES
**----------------------------------------------------------------------------*/
#define JACK_GPIO		(TEGRA_GPIO_PR7)
#define LINEOUT_GPIO	(TEGRA_GPIO_PX5)
#define HOOK_GPIO		(TEGRA_GPIO_PW3)
#define ON	(1)
#define OFF	(0)
#define RETRYMAX (10)
#define DB_TIME_HAYDN (600)

enum{
	NO_DEVICE = 0,
	HEADSET_WITH_MIC = 1,
	HEADSET_WITHOUT_MIC = 2,
};

struct headset_data {
	struct switch_dev sdev;
	struct input_dev *input;
	unsigned int irq;
	struct hrtimer timer;
	struct wake_lock hs_det;
	ktime_t debouncing_time;
};

static struct headset_data *hs_data;
bool headset_alive = false;
EXPORT_SYMBOL(headset_alive);
bool lineout_alive;
EXPORT_SYMBOL(lineout_alive);

static struct workqueue_struct *g_detection_work_queue;
static DECLARE_WORK(g_detection_work, detection_work);

extern struct snd_soc_codec *rt5639_audio_codec;
extern bool is_rt5639_suspended;
struct work_struct headset_work;
struct work_struct lineout_work;

static ssize_t headset_name_show(struct switch_dev *sdev, char *buf)
{
	switch (switch_get_state(&hs_data->sdev)){
	case NO_DEVICE:{
		return sprintf(buf, "%s\n", "No Device");
		}
	case HEADSET_WITH_MIC:{
		return sprintf(buf, "%s\n", "HEADSET");
		}
	case HEADSET_WITHOUT_MIC:{
		return sprintf(buf, "%s\n", "HEADPHONE");
		}
	}
	return -EINVAL;
}

static ssize_t headset_state_show(struct switch_dev *sdev, char *buf)
{
	switch (switch_get_state(&hs_data->sdev)){
	case NO_DEVICE:
		return sprintf(buf, "%d\n", 0);
	case HEADSET_WITH_MIC:
		return sprintf(buf, "%d\n", 1);
	case HEADSET_WITHOUT_MIC:
		return sprintf(buf, "%d\n", 2);
	}
	return -EINVAL;
}

static void insert_headset(void)
{
	if(gpio_get_value(HOOK_GPIO)){
		printk("%s: headphone\n", __func__);
		switch_set_state(&hs_data->sdev, HEADSET_WITHOUT_MIC);
		hs_micbias_power(OFF);
		headset_alive = false;
	}else{
		printk("%s: headset\n", __func__);
		switch_set_state(&hs_data->sdev, HEADSET_WITH_MIC);
		hs_micbias_power(ON);
		headset_alive = true;
	}
	hs_data->debouncing_time = ktime_set(0, 100000000);  /* 100 ms */
}
static void remove_headset(void)
{
	printk("%s:++++++++++\n", __func__);
	switch_set_state(&hs_data->sdev, NO_DEVICE);
	hs_data->debouncing_time = ktime_set(0, 100000000);  /* 100 ms */
	headset_alive = false;
}

static void detection_work(struct work_struct *work)
{
	unsigned long irq_flags;
	int cable_in1;
	int mic_in = 0;
	int retry;

	wake_lock(&hs_data->hs_det);

	for (retry = RETRYMAX; retry != 0; retry--) {
		if (is_rt5639_suspended) {
			msleep(150);
		} else {
			if (0) {                          //machine_is_haydn()
				if (retry == RETRYMAX)
					/*delay for jack pin stable. */
					msleep(100);
				if (gpio_get_value(JACK_GPIO) != 0) {
					if (switch_get_state(&hs_data->sdev) == HEADSET_WITH_MIC ||
						switch_get_state(&hs_data->sdev) == HEADSET_WITHOUT_MIC) {
						remove_headset();
						goto closed_micbias;
					}
				} else {
					hs_micbias_power(ON);
				}
			} else {
				hs_micbias_power(ON);
			}
			break;
		}
	}

	/* Disable headset interrupt while detecting.*/
	local_irq_save(irq_flags);
	disable_irq(hs_data->irq);
	local_irq_restore(irq_flags);

	/* Delay 1000ms for pin stable. */
	if (0)    //machine_is_haydn()
		msleep(DB_TIME_HAYDN);
	else
		msleep(1000);

	/* Restore IRQs */
	local_irq_save(irq_flags);
	enable_irq(hs_data->irq);
	local_irq_restore(irq_flags);

	if (1) {    //machine_is_mozart()
		if (gpio_get_value(JACK_GPIO) != 0) {
		/* Headset not plugged in */
			if (switch_get_state(&hs_data->sdev) == HEADSET_WITH_MIC ||
				switch_get_state(&hs_data->sdev) == HEADSET_WITHOUT_MIC)
				remove_headset();
			goto closed_micbias;
		}
	}

	cable_in1 = gpio_get_value(JACK_GPIO);
	mic_in  = gpio_get_value(HOOK_GPIO);
	if (cable_in1 == 0) {
	    printk("HOOK_GPIO value: %d\n", mic_in);
		if(switch_get_state(&hs_data->sdev) == NO_DEVICE)
			insert_headset();
		else if ( mic_in == 1)
			goto closed_micbias;
	} else{
		printk("HEADSET: Jack-in GPIO is low, but not a headset \n");
		goto closed_micbias;
	}
	wake_unlock(&hs_data->hs_det);
	return;

closed_micbias:
	hs_micbias_power(OFF);
	wake_unlock(&hs_data->hs_det);
	return;
}

static enum hrtimer_restart detect_event_timer_func(struct hrtimer *data)
{
	queue_work(g_detection_work_queue, &g_detection_work);
	return HRTIMER_NORESTART;
}

/**********************************************************
**  Function: Jack detection-in gpio configuration function
**  Parameter: none
**  Return value: if sucess, then returns 0
**
************************************************************/
static int jack_config_gpio()
{
	int ret;

	printk("HEADSET: Config Jack-in detection gpio\n");
	hs_micbias_power(ON);
	ret = gpio_request(JACK_GPIO, "h2w_detect");
	ret = gpio_direction_input(JACK_GPIO);

	hs_data->irq = gpio_to_irq(JACK_GPIO);
	ret = request_irq(hs_data->irq, detect_irq_handler,
			  IRQF_TRIGGER_FALLING|IRQF_TRIGGER_RISING, "h2w_detect", NULL);

	ret = irq_set_irq_wake(hs_data->irq, 1);
	if (0)    //machine_is_haydn
		msleep(DB_TIME_HAYDN);
	else
		msleep(1);
	if (gpio_get_value(JACK_GPIO) == 0){
		insert_headset();
	} else {
		hs_micbias_power(OFF);
		remove_headset();
	}

	return 0;
}

/**********************************************************
**  Function: Headset Hook Key Detection interrupt handler
**  Parameter: irq
**  Return value: IRQ_HANDLED
**  High: Hook button pressed
************************************************************/
static int btn_config_gpio()
{
	int ret;

	printk("HEADSET: Config Headset Button detection gpio\n");

	ret = gpio_request(HOOK_GPIO, "btn_INT");
	ret = gpio_direction_input(HOOK_GPIO);

	return 0;
}

static void lineout_work_queue(struct work_struct *work)
{
	msleep(300);

	if (gpio_get_value(LINEOUT_GPIO) == 0){
		printk("LINEOUT: LineOut inserted\n");
		lineout_alive = true;
	}else if(gpio_get_value(LINEOUT_GPIO)){
		printk("LINEOUT: LineOut removed\n");
		lineout_alive = false;
	}

}

/**********************************************************
**  Function: LineOut Detection configuration function
**  Parameter: none
**  Return value: IRQ_HANDLED
**
************************************************************/
static int lineout_config_gpio()
{
	int ret;

	printk("HEADSET: Config LineOut detection gpio\n");

	ret = gpio_request(LINEOUT_GPIO, "lineout_int");
	ret = gpio_direction_input(LINEOUT_GPIO);
	ret = request_irq(gpio_to_irq(LINEOUT_GPIO), &lineout_irq_handler, IRQF_TRIGGER_FALLING|IRQF_TRIGGER_RISING, "lineout_int", 0);

	if (gpio_get_value(LINEOUT_GPIO) == 0)
		lineout_alive = true;
	else
		lineout_alive = false;

	return 0;
}

/**********************************************************
**  Function: LineOut detection interrupt handler
**  Parameter: dedicated irq
**  Return value: if sucess, then returns IRQ_HANDLED
**
************************************************************/
static irqreturn_t lineout_irq_handler(int irq, void *dev_id)
{
	schedule_work(&lineout_work);
	return IRQ_HANDLED;
}

/**********************************************************
**  Function: Headset jack-in detection interrupt handler
**  Parameter: dedicated irq
**  Return value: if sucess, then returns IRQ_HANDLED
**
************************************************************/
static irqreturn_t detect_irq_handler(int irq, void *dev_id)
{
	int value1, value2;
	int retry_limit = 10;

	do {
		value1 = gpio_get_value(JACK_GPIO);
		irq_set_irq_type(hs_data->irq, value1 ?
				IRQF_TRIGGER_FALLING : IRQF_TRIGGER_RISING);
		value2 = gpio_get_value(JACK_GPIO);
	}while (value1 != value2 && retry_limit-- > 0);

	if ((switch_get_state(&hs_data->sdev) == NO_DEVICE) ^ value2){
		hrtimer_start(&hs_data->timer, hs_data->debouncing_time, HRTIMER_MODE_REL);
	}

	return IRQ_HANDLED;
}

static int codec_micbias_power(int on)
{
	if(on){
		//for ALC5642
		if(rt5639_audio_codec == NULL){
			printk("%s: No rt5639_audio_codec - set micbias on fail\n", __func__);
			return 0;
		}
		snd_soc_update_bits(rt5639_audio_codec, RT5639_PWR_ANLG1, RT5639_PWR_LDO2, RT5639_PWR_LDO2); /* Enable LDO2 */
		/* VREF1, VREF2, mainbias and mainbias bandgap should be turned on
		when use analog function by vendor's recommendation */
		if (0)  //machine_is_haydn
			snd_soc_update_bits(rt5639_audio_codec, RT5639_PWR_ANLG1,
					RT5639_PWR_MB|RT5639_PWR_BG|RT5639_PWR_VREF1|RT5639_PWR_VREF2,
					RT5639_PWR_MB|RT5639_PWR_BG|RT5639_PWR_VREF1|RT5639_PWR_VREF2);
		snd_soc_update_bits(rt5639_audio_codec, RT5639_PWR_ANLG2, RT5639_PWR_MB1, RT5639_PWR_MB1); /*Enable MicBias1 */
		//for ALC5642
	}else{
		//for ALC5642
		if(rt5639_audio_codec == NULL){
			printk("%s: No rt5639_audio_codec - set micbias off fail\n", __func__);
			return 0;
		}
		snd_soc_update_bits(rt5639_audio_codec, RT5639_PWR_ANLG2, RT5639_PWR_MB1, 0); /* Disable MicBias1 */
		snd_soc_update_bits(rt5639_audio_codec, RT5639_PWR_ANLG1, RT5639_PWR_LDO2, 0); /* Disable LDO2 */
	}
	return 0;
}


int hs_micbias_power(int on)
{
	static int nLastVregStatus = -1;

	if(on && nLastVregStatus!=ON){
		printk("HEADSET: Turn on micbias power\n");
		nLastVregStatus = ON;
		codec_micbias_power(ON);
	}
	else if(!on && nLastVregStatus!=OFF){
		printk("HEADSET: Turn off micbias power\n");
		nLastVregStatus = OFF;
		codec_micbias_power(OFF);
	}
	return 0;
}
EXPORT_SYMBOL(hs_micbias_power);

/**********************************************************
**  Function: Headset driver init function
**  Parameter: none
**  Return value: none
**
************************************************************/
static int __init headset_init(void)
{
	int ret;
	printk(KERN_INFO "%s+ #####\n", __func__);

	printk("HEADSET: Headset detection init\n");

	hs_data = kzalloc(sizeof(struct headset_data), GFP_KERNEL);
	if (!hs_data)
		return -ENOMEM;

	hs_data->debouncing_time = ktime_set(0, 100000000);  /* 100 ms */
	hs_data->sdev.name = "h2w";
	hs_data->sdev.print_name = headset_name_show;
	hs_data->sdev.print_state = headset_state_show;

	ret = switch_dev_register(&hs_data->sdev);
	if (ret < 0)
		goto err_switch_dev_register;

	g_detection_work_queue = create_workqueue("detection");
	wake_lock_init(&hs_data->hs_det, WAKE_LOCK_SUSPEND, "headset dection");

	hrtimer_init(&hs_data->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hs_data->timer.function = detect_event_timer_func;

	printk("HEADSET: Headset detection mode\n");
	lineout_config_gpio();
	btn_config_gpio();/*Config hook detection GPIO*/
	jack_config_gpio();/*Config jack detection GPIO*/
	INIT_WORK(&lineout_work, lineout_work_queue);

	printk(KERN_INFO "%s- #####\n", __func__);
	return 0;

err_switch_dev_register:
	printk(KERN_ERR "Headset: Failed to register driver\n");

	return ret;
}

/**********************************************************
**  Function: Headset driver exit function
**  Parameter: none
**  Return value: none
**
************************************************************/
static void __exit headset_exit(void)
{
	printk("HEADSET: Headset exit\n");
	if (switch_get_state(&hs_data->sdev))
		remove_headset();
	gpio_free(JACK_GPIO);
	gpio_free(HOOK_GPIO);
	gpio_free(LINEOUT_GPIO);

	free_irq(hs_data->irq, 0);
	destroy_workqueue(g_detection_work_queue);
	wake_lock_destroy(&hs_data->hs_det);
	switch_dev_unregister(&hs_data->sdev);
}

module_init(headset_init);
module_exit(headset_exit);
