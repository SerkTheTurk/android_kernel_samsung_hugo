/* This file is subject to the terms and conditions of the GNU General
 * Public License. See the file "COPYING" in the main directory of this
 * archive for more details.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */


#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/timer.h>
#include <linux/switch.h>
#include <plat/gpio.h>
#include <linux/i2c/twl4030-madc.h>
#include <linux/i2c/twl.h>
#include <linux/delay.h>
#include <linux/wakelock.h>
#include <linux/workqueue.h>

#include <mach/hardware.h>
#include <plat/mux.h>

#include "./switch_omap_gpio.h"
//#define CONFIG_DEBUG_SEC_HEADSET

#ifdef CONFIG_DEBUG_SEC_HEADSET
#define SEC_HEADSET_DBG(fmt, arg...) printk(KERN_ERR "[JACKKEY] %s" fmt "\r\n", __func__,## arg)
#else
#define SEC_HEADSET_DBG(fmt, arg...) do {} while(0)
#endif

#define KEYCODE_SENDEND 226
#define KEYCODE_VOL_UP 115
#define KEYCODE_VOL_DW 114

#define SEND_END_CHECK_COUNT	2
#define SEND_END_CHECK_TIME get_jiffies_64() + (HZ/100)	// 1000ms / 11 = 90m -> 1000ms/100 = 10ms
#define WAKE_LOCK_TIME (HZ*1)// 1000ms  = 1sec

struct switch_dev switch_sendend = {
        .name = "send_end",
};

#ifdef CONFIG_EXTRA_DOCK_SPEAKER
extern int g_dock;

struct switch_dev switch_dock_detection = {
		.name = "dock",
		.state = -1,
};

#define EARKEY_ADC_CHECK_COUNT	7
u16 adc_array[EARKEY_ADC_CHECK_COUNT] = {0};

static struct delayed_work home_dock_pend_work;

static void home_dock_pend_work_handler(struct work_struct* work)
{
	if (g_dock == 1)	/* only if home dock is attached */
	{
		if (switch_dock_detection.state != -1)
		{
			switch_set_state(&switch_dock_detection, 1);	/* send delayed dock event to UI */
		}
		else
		{
			schedule_delayed_work(&home_dock_pend_work, HZ);
		}
	}
}
#endif

static int __devinit ear_key_driver_probe(struct platform_device *plat_dev);
static irqreturn_t earkey_press_handler(int irq_num, void * dev);

#ifdef CONFIG_EXTRA_DOCK_SPEAKER
void set_dock_state(int value)
{
	if (switch_dock_detection.state != -1)
	{
		SEC_HEADSET_DBG(KERN_INFO "\tset_dock_state : 0X%x\n", value);
		switch_set_state(&switch_dock_detection, value);
		printk("%s : set_dock_state 0x%x\n", __func__, value);
	}
	else
	{
		/* delay sending dock event to UI until switch driver is loaded */
		if (g_dock == 1)
		{
			INIT_DELAYED_WORK(&home_dock_pend_work, home_dock_pend_work_handler);
			schedule_delayed_work(&home_dock_pend_work, HZ);
		}
		SEC_HEADSET_DBG(KERN_INFO "\t%s : Skip this because switch_dev driver not yet loaded!\n", __func__);
	}
}
#endif

int get_adc_data( int ch )
{
	int ret = 0;
	struct twl4030_madc_request req;

	#ifdef USE_ADC_SEL_GPIO
	gpio_direction_output(EAR_ADC_SEL_GPIO , 0);
	#endif
    
	req.channels = ( 1 << ch );
	req.do_avg = 0;
	req.method = TWL4030_MADC_SW1;
	req.active = 0;
	req.func_cb = NULL;
	twl4030_madc_conversion( &req );

	ret = req.rbuf[ch];
	SEC_HEADSET_DBG("\tear key adc value is : %d\n", ret);

	#ifdef USE_ADC_SEL_GPIO
	gpio_direction_output(EAR_ADC_SEL_GPIO , 1);
	#endif

	return ret;
}
EXPORT_SYMBOL(get_adc_data);


static struct timer_list send_end_key_event_timer;
static unsigned int send_end_key_timer_token;
static unsigned int send_end_irq_token;
static unsigned int earkey_stats = 0;
static unsigned int check_adc;
static int adcCompensation = 0;

struct input_dev *ip_dev = NULL;

struct wake_lock earkey_wakelock;

extern int earjack_min_max_adc;

void set_adc_compensation(int adcComp)
{
	adcCompensation = adcComp;
}
EXPORT_SYMBOL(set_adc_compensation);

#if 0
static void release_sysfs_event(struct work_struct *work)
{
	int adc_value = get_adc_data(2);

	SEC_HEADSET_DBG("\tadc data is %d", adc_value);
	SEC_HEADSET_DBG("\tear key state is %d", earkey_stats);
		
	if(earkey_stats)
	{
		if(earjack_min_max_adc > 700)
		{
			if((adc_value > 2) && ( 65 > adc_value))
			 {
				wake_lock( &earkey_wakelock);
	        	        switch_set_state(&switch_sendend, 1);
		       		input_report_key(ip_dev,KEYCODE_SENDEND,1);
		                input_sync(ip_dev);
				SEC_HEADSET_DBG("\tSEND/END key is working\n");
			 }
			else if((adc_value > 70 ) && ( 160 > adc_value))
                	 {       
                        	 wake_lock( &earkey_wakelock);
	                         switch_set_state(&switch_sendend, 1);
				 input_event(ip_dev, 1, KEYCODE_VOL_UP, earkey_stats);
                	         input_sync(ip_dev);
                        	 SEC_HEADSET_DBG("\tVOLUME UP key is working\n");                 
               	 	}
	       	         else if ((adc_value > 170 ) && ( 290 > adc_value))
			 {
                	         wake_lock( &earkey_wakelock);
                        	 switch_set_state(&switch_sendend, 1);
	                         input_event(ip_dev, 1, KEYCODE_VOL_DW, earkey_stats);
        	                 input_sync(ip_dev);
	        	         SEC_HEADSET_DBG("\tVOLUME DOWN key is working\n");
			 }		
			else
			{
				SEC_HEADSET_DBG("\tadc value is too low or high. adc value is %d\n",adc_value);
			}
		}

		else 
		{
			if((adc_value > 2 ) && ( 63 > adc_value))
			{
				wake_lock( &earkey_wakelock);
				switch_set_state(&switch_sendend, 1);
				input_report_key(ip_dev,KEYCODE_SENDEND,1);
				input_sync(ip_dev);
				SEC_HEADSET_DBG("\tSEND/END key is working\n");
			} 	
			else if((adc_value > 65 ) && ( 140 > adc_value))
			{
				wake_lock( &earkey_wakelock);
				switch_set_state(&switch_sendend, 1);
				input_event(ip_dev, 1, KEYCODE_VOL_UP, earkey_stats);
				input_sync(ip_dev);
				SEC_HEADSET_DBG("\tVOLUME UP key is working\n");
	
			}
			else if ((adc_value > 142 ) && ( 290 > adc_value))
			{
				wake_lock( &earkey_wakelock);
				switch_set_state(&switch_sendend, 1);
				input_event(ip_dev, 1, KEYCODE_VOL_DW, earkey_stats);
				input_sync(ip_dev);
				SEC_HEADSET_DBG("\tVOLUME DOWN key is working\n");
			}	
			else
			{
				SEC_HEADSET_DBG("\tadc value is too low or high. adc value is %d\n",adc_value);
			}
		}
	}
	else
	{	
		switch_set_state(&switch_sendend, 0);
		wake_unlock( &earkey_wakelock);
		wake_lock_timeout(&earkey_wakelock, WAKE_LOCK_TIME);
	}
}
static DECLARE_DELAYED_WORK(release_sysfs_event_work, release_sysfs_event);
#else
enum	{	SEND_END_LOW_1K=1, 	SEND_END_HIGH_1K=74,
			VOL_UP_LOW_1K=74, 	VOL_UP_HIGH_1K=151,
			VOL_DN_LOW_1K=152, 	VOL_DN_HIGH_1K=320};
enum	{	SEND_END_LOW_25K=1, 	SEND_END_HIGH_25K=79,
			VOL_UP_LOW_25K=79, 	VOL_UP_HIGH_25K=183,
			VOL_DN_LOW_25K=184, 	VOL_DN_HIGH_25K=399};
#define ADC_BOUNDARY	700

static void release_sysfs_event(struct work_struct *work)
{
	u16 adc_value, i, j, adc_temp;

	for (i=0; i<EARKEY_ADC_CHECK_COUNT; i++)
	{
		adc_array[i]=get_adc_data(2);
		msleep(5);
	}

//#define EARKEY_LOG
#ifdef EARKEY_LOG
	printk ("\n1st ==> ");
	for (i=0; i<EARKEY_ADC_CHECK_COUNT; i++)
		printk ("\t%d", adc_array[i]);
#endif

	for (i=0; i<EARKEY_ADC_CHECK_COUNT; i++)
	{
		for (j=i+1; j<EARKEY_ADC_CHECK_COUNT; j++)
		{
			if (adc_array[i] > adc_array[j])
			{
				adc_temp = adc_array[i];
				adc_array[i] = adc_array[j];
				adc_array[j] = adc_temp;
			}
		}
	}

#ifdef EARKEY_LOG
	printk ("\n2ndt ==> ");
	for (i=0; i<EARKEY_ADC_CHECK_COUNT; i++)
		printk ("\t%d", adc_array[i]);
#endif

#if 1
	if (earjack_min_max_adc > ADC_BOUNDARY)
	{
		if (adc_array[6] > VOL_DN_HIGH_25K)
			return;
		if (adc_array[6] - adc_array[0] > VOL_DN_HIGH_25K - SEND_END_HIGH_25K)
			return;
		if (adc_array[4] - adc_array[2] > 5)
			return;
	}
	else
	{
		if (adc_array[6] > VOL_DN_HIGH_1K)
			return;
		if (adc_array[6] - adc_array[0] > VOL_DN_HIGH_1K - SEND_END_HIGH_1K)
			return;
		if (adc_array[4] - adc_array[2] > 5)
			return;
	}
#endif

	adc_value = adc_array[3] + adcCompensation;
	
	if (HEADSET_4POLE_WITH_MIC != get_headset_status())
		return;

	SEC_HEADSET_DBG("\tear key state is %d", earkey_stats);
	if (!earkey_stats)
	{	
		switch_set_state(&switch_sendend, 0);
		wake_unlock( &earkey_wakelock);
		wake_lock_timeout(&earkey_wakelock, WAKE_LOCK_TIME);
		return;
	}

	SEC_HEADSET_DBG("\t!!!!!!!!!!!!!!!!!!!!adc data is %d\n", adc_value);
	SEC_HEADSET_DBG("\t@@@@@@@@@@@@adcCompensation : %d\n", adcCompensation);

	wake_lock( &earkey_wakelock);
	switch_set_state(&switch_sendend, 1);
	if (earjack_min_max_adc > ADC_BOUNDARY)
	{
		if((adc_value > SEND_END_LOW_25K) && (SEND_END_HIGH_25K > adc_value))
		{
			input_report_key(ip_dev,KEYCODE_SENDEND, earkey_stats);
			SEC_HEADSET_DBG("\tSEND/END key is working\n");
		}
		else if((adc_value > VOL_UP_LOW_25K) && (VOL_UP_HIGH_25K > adc_value))
		{
			input_report_key(ip_dev, KEYCODE_VOL_UP, earkey_stats);
                    	SEC_HEADSET_DBG("\tVOLUME UP key is working\n");                 
		}
		else if ((adc_value > VOL_DN_LOW_25K) && (VOL_DN_HIGH_25K > adc_value))
		{
			input_report_key(ip_dev, KEYCODE_VOL_DW, earkey_stats);
			SEC_HEADSET_DBG("\tVOLUME DOWN key is working\n");
		}		
		else
		{
			SEC_HEADSET_DBG("\tadc value is too low or high. adc value is %d\n",adc_value);
		}
	}
	else 
	{
		if((adc_value > SEND_END_LOW_1K) && (SEND_END_HIGH_1K > adc_value))
		{
			input_report_key(ip_dev,KEYCODE_SENDEND, earkey_stats);
			SEC_HEADSET_DBG("\tSEND/END key is working\n");
		} 	
		else if((adc_value > VOL_UP_LOW_1K) && (VOL_UP_HIGH_1K > adc_value))
		{
			input_report_key(ip_dev, KEYCODE_VOL_UP, earkey_stats);
			SEC_HEADSET_DBG("\tVOLUME UP key is working\n");

		}
		else if ((adc_value > VOL_DN_LOW_1K) && (VOL_DN_HIGH_1K > adc_value))
		{
			input_report_key(ip_dev, KEYCODE_VOL_DW, earkey_stats);
			SEC_HEADSET_DBG("\tVOLUME DOWN key is working\n");
		}	
		else
		{
			SEC_HEADSET_DBG("\tadc value is too low or high. adc value is %d\n",adc_value);
		}
	}
	input_sync(ip_dev);
}
static DECLARE_DELAYED_WORK(release_sysfs_event_work, release_sysfs_event);
#endif


void ear_key_disable_irq(void)
{
	if(send_end_irq_token > 0)
	{
		SEC_HEADSET_DBG("\t[EAR_KEY]ear key disable\n");
		earkey_stats = 0;
		schedule_delayed_work(&release_sysfs_event_work, 0);
		//switch_set_state(&switch_sendend, 0);

		//can't use duplicately, sejong.
		//send_end_irq_token--;
		send_end_irq_token = 0;
	}
}
EXPORT_SYMBOL_GPL(ear_key_disable_irq);

void ear_key_enable_irq(void)
{
	if(send_end_irq_token == 0)
	{
		SEC_HEADSET_DBG("\t[EAR_KEY]ear key enable\n");

		//can't use duplicately, sejong.
		//send_end_irq_token++;
		send_end_irq_token = 1;
	}
}
EXPORT_SYMBOL_GPL(ear_key_enable_irq);


extern u16 wm1811_voip_earkey_check;
extern int codec_LDO_power_check;
static void send_end_key_event_timer_handler(unsigned long arg)
{
	int sendend_state = 0;

	if(codec_LDO_power_check)
	{
		SEC_HEADSET_DBG("\tcodec_LDO_power_check=%d", codec_LDO_power_check);
		codec_LDO_power_check = 0;
		return;
	}

#if 0
	// return initial step of the VoIP mode
	if (wm1811_voip_earkey_check)
	{
		SEC_HEADSET_DBG("VoIP Status=%d", wm1811_voip_earkey_check);
		wm1811_voip_earkey_check=0;
		return;
	}	
#endif

	sendend_state = gpio_get_value(EAR_KEY_GPIO) ^ EAR_KEY_INVERT_ENABLE;
	SEC_HEADSET_DBG("\tSendEnd state = %d\n", sendend_state);

	if((get_headset_status() == HEADSET_4POLE_WITH_MIC) && sendend_state)
	{
		if(send_end_key_timer_token < SEND_END_CHECK_COUNT)
		{	
			send_end_key_timer_token++;
			send_end_key_event_timer.expires = SEND_END_CHECK_TIME; 
			add_timer(&send_end_key_event_timer);
			SEC_HEADSET_DBG("\tSendEnd Timer Restart %d", send_end_key_timer_token);
		}
		else if((send_end_key_timer_token == SEND_END_CHECK_COUNT) && send_end_irq_token)
		{
			SEC_HEADSET_DBG("\tSEND/END is pressed\n");
			earkey_stats = 1;
			schedule_delayed_work(&release_sysfs_event_work, 0);
			send_end_key_timer_token = 0;
		}
		else
			SEC_HEADSET_DBG(KERN_ALERT "\t[Headset]wrong timer counter %d\n", send_end_key_timer_token);
	}
	else
	{		
		SEC_HEADSET_DBG(KERN_ALERT "\t[Headset]GPIO Error\n %d, %d", get_headset_status(), sendend_state);
	}
}

static void ear_switch_change(struct work_struct *ignored)
{
  	int state;

	SEC_HEADSET_DBG("\tsend_end_irq_token=%d", send_end_irq_token);
	
	if(!ip_dev){
    		dev_err(ip_dev->dev.parent,"Input Device not allocated\n");
		return;
  	}
  
  	state = gpio_get_value(EAR_KEY_GPIO) ^ EAR_KEY_INVERT_ENABLE;
  	if( state < 0 ){
 	   	dev_err(ip_dev->dev.parent,"Failed to read GPIO value\n");
		return;
  	}

	del_timer(&send_end_key_event_timer);
	send_end_key_timer_token = 0;	

	SEC_HEADSET_DBG("\tear key %d", state);
	if((get_headset_status() == HEADSET_4POLE_WITH_MIC) && send_end_irq_token)//  4 pole headset connected && send irq enable
	{
		SEC_HEADSET_DBG("\t state = %d\n", state);
		if(state)
		{
			//wake_lock(&headset_sendend_wake_lock);
			send_end_key_event_timer.expires = SEND_END_CHECK_TIME; // 10ms ??
			add_timer(&send_end_key_event_timer);
			SEC_HEADSET_DBG("\tSEND/END %s.\t timer start \n", "pressed");
		}
		else
		{
			SEC_HEADSET_DBG(KERN_ERR "SISO:sendend isr work queue\n");    			
		 	input_report_key(ip_dev,KEYCODE_SENDEND,0);
		 	input_report_key(ip_dev,KEYCODE_VOL_UP,0);
		 	input_report_key(ip_dev,KEYCODE_VOL_DW,0);
  			input_sync(ip_dev);
			SEC_HEADSET_DBG("\tSEND/END is released ");
			earkey_stats = 0;
			switch_set_state(&switch_sendend, 0);
			wake_unlock( &earkey_wakelock);
			wake_lock_timeout(&earkey_wakelock, WAKE_LOCK_TIME);
			//schedule_delayed_work(&release_sysfs_event_work, 10);
			//wake_unlock(&headset_sendend_wake_lock);			
		}

	}else{
		SEC_HEADSET_DBG("SEND/END Button is %s but headset disconnect or irq disable.\n", state?"pressed":"released");
	}

}
static DECLARE_WORK(ear_switch_work, ear_switch_change);

static irqreturn_t earkey_press_handler(int irq_num, void * dev)
{
	SEC_HEADSET_DBG("\tearkey isr");
	schedule_work(&ear_switch_work);
	return IRQ_HANDLED;
}

static int __devinit ear_key_driver_probe(struct platform_device *plat_dev)
{
	struct input_dev *ear_key=NULL;
	int ear_key_irq=-1, err=0;

	SEC_HEADSET_DBG("");

	ear_key_irq = platform_get_irq(plat_dev, 0);
	if(ear_key_irq <= 0 ){
		dev_err(&plat_dev->dev,"failed to map the ear key to an IRQ %d\n",ear_key_irq);
		err = -ENXIO;
		return err;
	}

	ear_key = input_allocate_device();
	if(!ear_key)
	{
		dev_err(&plat_dev->dev,"failed to allocate an input devd %d \n",ear_key_irq);
		err = -ENOMEM;
		return err;
	}

	err = request_irq(ear_key_irq, &earkey_press_handler ,IRQF_TRIGGER_FALLING|IRQF_TRIGGER_RISING,
																				"ear_key_driver",ear_key);
	if(err) {
		dev_err(&plat_dev->dev,"failed to request an IRQ handler for num %d\n",ear_key_irq);
		goto free_input_dev;
	}
	dev_dbg(&plat_dev->dev,"\n ear Key Drive:Assigned IRQ num %d SUCCESS \n",ear_key_irq);
	/* register the input device now */
	set_bit(EV_SYN,ear_key->evbit);
	set_bit(EV_KEY,ear_key->evbit);
	set_bit(KEYCODE_SENDEND, ear_key->keybit);
	set_bit(KEYCODE_VOL_UP, ear_key->keybit);
	set_bit(KEYCODE_VOL_DW, ear_key->keybit);

	ear_key->name = "sec_jack";
	ear_key->phys = "sec_jack/input0";
	ear_key->dev.parent = &plat_dev->dev;
	platform_set_drvdata(plat_dev, ear_key);

	err = input_register_device(ear_key);
	if (err) {
		dev_err(&plat_dev->dev, "ear key couldn't be registered: %d\n", err);
		goto release_irq_num;
	}
	
	err = switch_dev_register(&switch_sendend);
	if (err < 0) {
		SEC_HEADSET_DBG(KERN_ERR "\tSEC HEADSET: Failed to register switch sendend device\n");
		goto free_input_dev;
	}

#ifdef CONFIG_EXTRA_DOCK_SPEAKER
	err = switch_dev_register(&switch_dock_detection);
	if (err < 0) 
	{
		SEC_HEADSET_DBG(KERN_ERR "\tSEC DOCK: Failed to register switch device\n");
		goto free_input_dev;
	}
#endif


	init_timer(&send_end_key_event_timer);
	send_end_key_event_timer.function = send_end_key_event_timer_handler;
	ip_dev = ear_key;

	wake_lock_init( &earkey_wakelock, WAKE_LOCK_SUSPEND, "ear_key");

	 return 0;

	release_irq_num:
	free_irq(ear_key_irq,NULL); //pass devID as NULL as device registration failed 

	free_input_dev:
	input_free_device(ear_key);
	switch_dev_unregister(&switch_sendend);
	#ifdef CONFIG_EXTRA_DOCK_SPEAKER
	switch_dev_unregister(&switch_dock_detection);	
	#endif

	return err;
}

static int __devexit ear_key_driver_remove(struct platform_device *plat_dev)
{
	int ear_key_irq=0;
	SEC_HEADSET_DBG("");
	//struct input_dev *ip_dev= platform_get_drvdata(plat_dev);
	ear_key_irq = platform_get_irq(plat_dev,0);
	  
	free_irq(ear_key_irq,ip_dev);
	switch_dev_unregister(&switch_sendend);
#ifdef CONFIG_EXTRA_DOCK_SPEAKER
	cancel_delayed_work(&home_dock_pend_work);
	switch_dev_unregister(&switch_dock_detection);	
#endif
	input_unregister_device(ip_dev); 

	return 0;
}

struct platform_driver ear_key_driver_t = {
        .probe          = &ear_key_driver_probe,
        .remove         = __devexit_p(ear_key_driver_remove),
        .driver         = {
                .name   = "sec_jack", 
                .owner  = THIS_MODULE,
        },
};

static int __init ear_key_driver_init(void)
{
        return platform_driver_register(&ear_key_driver_t);
}
module_init(ear_key_driver_init);

static void __exit ear_key_driver_exit(void)
{
        platform_driver_unregister(&ear_key_driver_t);
}
module_exit(ear_key_driver_exit);

MODULE_ALIAS("platform:ear key driver");
MODULE_DESCRIPTION("board zeus ear key");
MODULE_LICENSE("GPL");

