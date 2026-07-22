/*
 * leds-is31fl3236.c
 *
 * Copyright (c) 2016 Amazon.com, Inc. or its affiliates. All Rights Reserved
 *
 * The code contained herein is licensed under the GNU General Public
 * License Version 2. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

#include "leds-is31fl3236.h"

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/printk.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
#include <linux/sysfs.h>

#define REG_SW_SHUTDOWN 0x00
#define REG_PWM_BASE 0x01
#define REG_UPDATE 0x25
#define REG_CTRL_BASE 0x26
#define REG_G_CTRL 0x4A /* Global Control Register */
#define REG_RST 0x4F

#define LED_SW_ON 0x01
#define LED_CTRL_UPDATE 0x0
#define LED_CHAN_DISABLED 0x0
#define LED_CHAN_ENABLED 0x01

#define LED_CURRENT_1   0x00
#define LED_CURRENT_1_2 0x01
#define LED_CURRENT_1_3 0x02
#define LED_CURRENT_1_4 0x03
#define LED_CURRENT_DEFAULT LED_CURRENT_1_4

#define BOOT_ANIMATION_FRAME_DELAY 88

struct is31fl3236_data {
	struct mutex lock;
	struct mutex anim_lock;
	bool play_boot_animation;
	bool setup_device;
	bool shutting_down;
	int enable_gpio;
	uint8_t ch_offset;
	struct task_struct *boot_anim_task;
	bool boot_anim_running;
	bool boot_anim_started;
	bool boot_anim_exiting;
	int enabled;
	uint8_t *state;
	uint8_t led_current;
	struct i2c_client *client;
	struct notifier_block reboot_notifier;
};

static int is31fl3236_write_reg(struct i2c_client *client,
				uint32_t reg,
				uint8_t value)
{
	return i2c_smbus_write_byte_data(client, reg, value);
}

static int is31fl3236_update_led(struct i2c_client *client,
				 uint32_t led,
				 uint8_t value)
{
	return is31fl3236_write_reg(client, REG_PWM_BASE+led, value);
}

static int update_frame_locked(struct is31fl3236_data *pdata,
			       const uint8_t *buf)
{
	struct i2c_client *client = pdata->client;
	int ret;
	int i;

	for (i = 0; i < NUM_CHANNELS; i++) {
		ret = is31fl3236_update_led(client,
				(i+pdata->ch_offset)%NUM_CHANNELS,
				buf[i]);
		if (ret < 0)
			return ret;
	}
	ret = is31fl3236_write_reg(client, REG_UPDATE, 0x0);
	if (ret >= 0)
		memcpy(pdata->state, buf, NUM_CHANNELS);
	return ret;
}

static int update_frame(struct is31fl3236_data *pdata,
			const uint8_t *buf)
{
	int ret;

	mutex_lock(&pdata->lock);
	ret = update_frame_locked(pdata, buf);
	mutex_unlock(&pdata->lock);
	return ret;
}

static int boot_anim_thread(void *data)
{
	struct is31fl3236_data *pdata = (struct is31fl3236_data *)data;
	int i = 0;
	int ret = 0;
	int clear_ret;

	mutex_lock(&pdata->lock);
	pdata->boot_anim_started = true;
	mutex_unlock(&pdata->lock);

	while (!kthread_should_stop()) {
		mutex_lock(&pdata->lock);
		ret = update_frame_locked(pdata, &frames[i][0]);
		if (ret < 0)
			pdata->boot_anim_exiting = true;
		mutex_unlock(&pdata->lock);
		if (ret < 0) {
			pr_err("ISSI: boot animation frame update failed: %d\n",
			       ret);
			break;
		}
		msleep(BOOT_ANIMATION_FRAME_DELAY);
		i = (i + 1) % ARRAY_SIZE(frames);
	}
	mutex_lock(&pdata->lock);
	clear_ret = update_frame_locked(pdata, clear_frame);
	pdata->boot_anim_running = false;
	mutex_unlock(&pdata->lock);
	if (ret >= 0 && clear_ret < 0)
		ret = clear_ret;
	return ret;
}


static int is31fl3236_start_boot_animation(struct is31fl3236_data *pdata)
{
	struct task_struct *task = NULL;
	int ret = 0;

	mutex_lock(&pdata->anim_lock);
	mutex_lock(&pdata->lock);
	if (pdata->shutting_down) {
		ret = -ESHUTDOWN;
		goto unlock;
	}
	if (pdata->boot_anim_task && pdata->boot_anim_running &&
	    !pdata->boot_anim_exiting)
		goto unlock;
	if (pdata->boot_anim_task) {
		task = pdata->boot_anim_task;
		pdata->boot_anim_task = NULL;
	}
	mutex_unlock(&pdata->lock);

	/* Join a naturally exited worker before publishing its replacement. */
	if (task) {
		ret = kthread_stop(task);
		put_task_struct(task);
		mutex_lock(&pdata->lock);
		pdata->boot_anim_running = false;
		pdata->boot_anim_started = false;
		pdata->boot_anim_exiting = false;
		mutex_unlock(&pdata->lock);
		if (ret < 0)
			pr_err("ISSI: previous boot animation failed: %d\n", ret);
	}

	task = kthread_create(boot_anim_thread, pdata,
			      "boot_animation_thread");
	if (IS_ERR(task)) {
		ret = PTR_ERR(task);
		goto unlock_anim;
	}

	/* Pin the task until a serialized start or stop path joins it. */
	get_task_struct(task);
	mutex_lock(&pdata->lock);
	pdata->boot_anim_task = task;
	pdata->boot_anim_running = true;
	pdata->boot_anim_started = false;
	pdata->boot_anim_exiting = false;
	mutex_unlock(&pdata->lock);
	wake_up_process(task);
	ret = 0;
	goto unlock_anim;

unlock:
	mutex_unlock(&pdata->lock);
unlock_anim:
	mutex_unlock(&pdata->anim_lock);
	return ret;
}

static int is31fl3236_stop_boot_animation(struct is31fl3236_data *pdata,
					  bool shutting_down)
{
	struct task_struct *task;
	bool started = false;
	int clear_ret;
	int ret;

	/* Do not permit a replacement worker until the old one has exited. */
	mutex_lock(&pdata->anim_lock);
	mutex_lock(&pdata->lock);
	if (shutting_down)
		pdata->shutting_down = true;
	task = pdata->boot_anim_task;
	if (task)
		pdata->boot_anim_exiting = true;
	mutex_unlock(&pdata->lock);

	ret = 0;
	if (task) {
		ret = kthread_stop(task);
		mutex_lock(&pdata->lock);
		started = pdata->boot_anim_started;
		if (ret == -EINTR && !started) {
			clear_ret = update_frame_locked(pdata, clear_frame);
			if (clear_ret < 0)
				ret = clear_ret;
			else
				ret = 0;
		}
		if (pdata->boot_anim_task == task)
			pdata->boot_anim_task = NULL;
		pdata->boot_anim_running = false;
		pdata->boot_anim_started = false;
		pdata->boot_anim_exiting = false;
		mutex_unlock(&pdata->lock);
		put_task_struct(task);
	}
	mutex_unlock(&pdata->anim_lock);
	return ret;
}

static ssize_t boot_animation_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf,
				    size_t len)
{
	struct is31fl3236_data *pdata = dev_get_drvdata(dev);
	uint8_t val;
	ssize_t ret;

	ret = kstrtou8(buf, 10, &val);
	if (ret)
		return ret;
	if (val > 1)
		return -EINVAL;

	if (val)
		ret = is31fl3236_start_boot_animation(pdata);
	else
		ret = is31fl3236_stop_boot_animation(pdata, false);

	return ret < 0 ? ret : len;
}

static ssize_t boot_animation_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct is31fl3236_data *pdata = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&pdata->lock);
	ret = sprintf(buf, "%d\n", pdata->boot_anim_running);
	mutex_unlock(&pdata->lock);

	return ret;
}
static DEVICE_ATTR_RW(boot_animation);

static ssize_t led_current_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf,
				 size_t len)
{
	struct is31fl3236_data *pdata = dev_get_drvdata(dev);
	struct i2c_client *client = pdata->client;
	uint8_t val;
	uint8_t reg_val;
	int i;
	ssize_t ret;

	ret = kstrtou8(buf, 10, &val);
	if (ret)
		return ret;
	if (val < LED_CURRENT_1 || val > LED_CURRENT_1_4) {
		return -EINVAL;
	}

	reg_val = (val << 1) | 0x1;
	mutex_lock(&pdata->lock);
	for (i = 0; i < NUM_CHANNELS; i++) {
		ret = is31fl3236_write_reg(client, REG_CTRL_BASE + i,
						  reg_val);
		if (ret < 0)
			goto unlock;
	}
	ret = is31fl3236_write_reg(client, REG_UPDATE, 0x0);
	if (ret >= 0)
		pdata->led_current = val;
unlock:
	mutex_unlock(&pdata->lock);
	return ret < 0 ? ret : len;
}

static ssize_t led_current_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct is31fl3236_data *pdata = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&pdata->lock);
	ret = sprintf(buf, "%d\n", pdata->led_current);
	mutex_unlock(&pdata->lock);

	return ret;
}
static DEVICE_ATTR_RW(led_current);

static ssize_t frame_show(struct device *dev,
			  struct device_attribute *attr,
			  char *buf)
{
	struct is31fl3236_data *pdata = dev_get_drvdata(dev);
	int len = 0;
	int i;

	mutex_lock(&pdata->lock);
	for (i = 0; i < NUM_CHANNELS; i++)
		len += scnprintf(buf + len, PAGE_SIZE - len, "%02x",
				 pdata->state[i]);
	len += scnprintf(buf + len, PAGE_SIZE - len, "\n");
	mutex_unlock(&pdata->lock);
	return len;
}

static ssize_t frame_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t len)
{
	struct is31fl3236_data *pdata = dev_get_drvdata(dev);
	uint8_t new_state[NUM_CHANNELS];
	char val[3];
	int count = 0;
	int ret, i;

	if (len != NUM_CHANNELS * 2 &&
	    !(len == NUM_CHANNELS * 2 + 1 &&
	      buf[NUM_CHANNELS * 2] == '\n'))
		return -EINVAL;

	val[2] = '\0';
	for (i = 0; i < NUM_CHANNELS * 2; i += 2) {
		val[0] = buf[i];
		val[1] = buf[i + 1];
		ret = kstrtou8(val, 16, &new_state[count]);
		if (ret)
			return ret;
		count++;
	}

	ret = update_frame(pdata, new_state);
	return ret < 0 ? ret : len;
}

static DEVICE_ATTR_RW(frame);

static struct attribute *is31fl3236_attrs[] = {
	&dev_attr_frame.attr,
	&dev_attr_led_current.attr,
	&dev_attr_boot_animation.attr,
	NULL,
};

static const struct attribute_group is31fl3236_attr_group = {
	.attrs = is31fl3236_attrs,
};

static void is31fl3236_parse_dt(struct is31fl3236_data *pdata,
				struct device_node *node)
{
	pdata->play_boot_animation = of_property_read_bool(node,
							"play-boot-animation");
	pdata->setup_device = of_property_read_bool(node, "setup-device");
	pdata->enable_gpio = of_get_named_gpio(node, "enable-gpio", 0);
	/*Read a channel offset, if one is available*/
	if (of_property_read_u8(node, "channel-offset", &pdata->ch_offset))
		pdata->ch_offset = 0;
	/*Offset should not exceed the number of channels available*/
	if (pdata->ch_offset >= NUM_CHANNELS) {
		pr_err("ISSI: Invalid channel offset=%d\n", pdata->ch_offset);
		pdata->ch_offset = 0;
	}
}

static int is31fl3236_power_on_device(struct i2c_client *client)
{
	struct is31fl3236_data *pdata = dev_get_drvdata(&client->dev);
	int i;
	int ret = 0;

	if (pdata->setup_device) {
		if (gpio_is_valid(pdata->enable_gpio)) {
			ret = devm_gpio_request_one(&client->dev,
						    pdata->enable_gpio,
						    GPIOF_DIR_OUT,
						    "is31fl3236_enable");
			if (ret < 0) {
				pr_err("ISSI: could not request enable gpio\n");
				return ret;
			}
			/*Device goes into shutdown if enable*/
			/*gpio is low so force it high always*/
			gpio_set_value(pdata->enable_gpio, 1);
		}

		ret = is31fl3236_write_reg(client, REG_RST, 0x0);
		if (ret < 0) {
			pr_err("ISSI: Could not reset registers\n");
			goto fail;
		}

		ret = is31fl3236_write_reg(client, REG_SW_SHUTDOWN, LED_SW_ON);
		if (ret < 0) {
			pr_err("ISSI: Could not start device\n");
			goto fail;
		}
	}
	pdata->enabled = LED_CHAN_ENABLED;

	for (i = 0; i < NUM_CHANNELS; i++) {
		ret = is31fl3236_write_reg(client, REG_CTRL_BASE + i,
						  (pdata->led_current << 1) |
						  LED_CHAN_ENABLED);
		if (ret < 0) {
			pr_err("ISSI: Could not enable led: %d\n", i);
			goto fail;
		}
	}
	ret = is31fl3236_write_reg(client, REG_UPDATE, 0x0);
	if (ret < 0)
		goto fail;

	return 0;
fail:
	pdata->enabled = LED_CHAN_DISABLED;
	if (pdata->setup_device && gpio_is_valid(pdata->enable_gpio)) {
		gpio_set_value(pdata->enable_gpio, 0);
		devm_gpio_free(&client->dev, pdata->enable_gpio);
	}
	return ret;
}

static int is31fl3236_power_off_device(struct is31fl3236_data *pdata)
{
	int ret;

	ret = is31fl3236_write_reg(pdata->client, REG_RST, 0x0);
	if (pdata->setup_device && gpio_is_valid(pdata->enable_gpio))
		gpio_set_value(pdata->enable_gpio, 0);
	pdata->enabled = LED_CHAN_DISABLED;
	return ret;
}

static int is31fl3236_reboot_callback(struct notifier_block *self,
				      unsigned long val,
				      void *data)
{
	struct is31fl3236_data *pdata =
		container_of(self, struct is31fl3236_data, reboot_notifier);
	int anim_ret;
	int ret;

	anim_ret = is31fl3236_stop_boot_animation(pdata, true);

	mutex_lock(&pdata->lock);
	ret = is31fl3236_power_off_device(pdata);
	mutex_unlock(&pdata->lock);
	if (ret < 0)
		pr_err("ISSI: Could not reset registers before reboot: %d\n",
		       ret);
	if (anim_ret < 0)
		pr_err("ISSI: Could not stop boot animation before reboot: %d\n",
		       anim_ret);
	return NOTIFY_DONE;
}

static int is31fl3236_probe(struct i2c_client *client,
			    const struct i2c_device_id *id)
{
	struct is31fl3236_data *pdata;
	int ret;

	pdata = devm_kzalloc(&client->dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	pdata->state = devm_kzalloc(&client->dev, NUM_CHANNELS, GFP_KERNEL);
	if (!pdata->state)
		return -ENOMEM;

	pdata->client = client;
	pdata->led_current = LED_CURRENT_DEFAULT;
	pdata->enabled = LED_CHAN_DISABLED;
	mutex_init(&pdata->lock);
	mutex_init(&pdata->anim_lock);
	pdata->reboot_notifier.notifier_call = is31fl3236_reboot_callback;
	is31fl3236_parse_dt(pdata, client->dev.of_node);
	i2c_set_clientdata(client, pdata);

	ret = is31fl3236_power_on_device(client);
	if (ret < 0) {
		pr_err("ISSI: Could not power on device: %d\n", ret);
		return ret;
	}

	ret = sysfs_create_group(&client->dev.kobj, &is31fl3236_attr_group);
	if (ret) {
		pr_err("ISSI: Could not create sysfs attribute group\n");
		goto fail_power;
	}

	ret = register_reboot_notifier(&pdata->reboot_notifier);
	if (ret) {
		pr_err("ISSI: Could not register reboot callback\n");
		goto fail_sysfs;
	}

	if (pdata->play_boot_animation) {
		ret = is31fl3236_start_boot_animation(pdata);
		if (ret < 0) {
			pr_err("ISSI: Could not start boot animation: %d\n", ret);
			goto fail_notifier;
		}
	}

	return 0;

fail_notifier:
	unregister_reboot_notifier(&pdata->reboot_notifier);
fail_sysfs:
	sysfs_remove_group(&client->dev.kobj, &is31fl3236_attr_group);
fail_power:
	mutex_lock(&pdata->lock);
	is31fl3236_power_off_device(pdata);
	mutex_unlock(&pdata->lock);
	return ret;
}

static int is31fl3236_remove(struct i2c_client *client)
{
	struct is31fl3236_data *pdata = i2c_get_clientdata(client);
	int anim_ret;
	int ret;

	unregister_reboot_notifier(&pdata->reboot_notifier);
	sysfs_remove_group(&client->dev.kobj, &is31fl3236_attr_group);

	anim_ret = is31fl3236_stop_boot_animation(pdata, true);

	mutex_lock(&pdata->lock);
	ret = is31fl3236_power_off_device(pdata);
	mutex_unlock(&pdata->lock);
	if (ret < 0)
		pr_err("ISSI: Could not reset registers during remove\n");
	return ret < 0 ? ret : anim_ret;
}

static struct i2c_device_id is31fl3236_i2c_match[] = {
	{"issi,is31fl3236", 0},
};
MODULE_DEVICE_TABLE(i2c, is31fl3236_i2c_match);

static struct of_device_id is31fl3236_of_match[] = {
	{ .compatible = "issi,is31fl3236"},
};
MODULE_DEVICE_TABLE(of, is31fl3236_of_match);

static struct i2c_driver is31fl3236_driver = {
	.driver = {
		.name = "is31fl3236",
		.of_match_table = of_match_ptr(is31fl3236_of_match),
	},
	.probe = is31fl3236_probe,
	.remove = is31fl3236_remove,
	.id_table = is31fl3236_i2c_match,
};

module_i2c_driver(is31fl3236_driver);

MODULE_AUTHOR("Amazon.com");
MODULE_DESCRIPTION("ISSI IS31FL3236 LED Driver");
MODULE_LICENSE("GPL");
