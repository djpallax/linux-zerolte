// SPDX-License-Identifier: GPL-2.0-only
/*
 * Zerolte STM FTS pilot, based on Samsung2014-2015 fts_ts.c protocol and
 * Linux stmfts.c event layout. No firmware upgrade or NVM writes.
 * Supplies use the verified Samsung3.3V/1.85V values; no suspend claim.
 */
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/leds.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
struct s6_fts {
	struct i2c_client *client;
	struct input_dev *input, *keys;
	struct regulator *avdd, *vdd, *keyled;
	struct led_classdev keyled_class;
	bool keyled_on;
	u8 events[256], id[7];
	unsigned long irqs, contacts, errors;
	unsigned int consecutive_errors;
	unsigned long key_events;
};

static int s6_fts_read(struct s6_fts *d, u8 *command, u16 len, void *data,
		       u16 size)
{
	struct i2c_msg m[] = {
		{ .addr = d->client->addr, .len = len, .buf = command },
		{ .addr = d->client->addr,
		  .flags = I2C_M_RD,
		  .len = size,
		  .buf = data }
	};
	int r;

	r = i2c_transfer(d->client->adapter, m, 2);
	return r == 2 ? 0 : (r < 0 ? r : -EIO);
}

static int s6_fts_write(struct s6_fts *d, const u8 *command, int size)
{
	int r = i2c_master_send(d->client, command, size);
	return r == size ? 0 : (r < 0 ? r : -EIO);
}

static int s6_fts_cmd(struct s6_fts *d, u8 command)
{
	return s6_fts_write(d, &command, 1);
}

static irqreturn_t s6_fts_irq(int irq, void *arg)
{
	struct s6_fts *d = arg;
	u8 command = 0x86;
	int n, r;

	d->irqs++;
	r = s6_fts_read(d, &command, 1, d->events, sizeof(d->events));
	if (r) {
		d->errors++;
		if (++d->consecutive_errors >= 3) {
			disable_irq_nosync(irq);
			dev_err(&d->client->dev,
				"three I2C failures; touch IRQ disabled, USB/WLAN rescue retained\n");
		}
		return IRQ_HANDLED;
	}
	d->consecutive_errors = 0;
	for (n = 0; n < 32; n++) {
		u8 *e = d->events + n * 8;
		unsigned int type = e[0] & 15, slot = e[0] >> 4;

		if (!e[0])
			break;
		if (e[0] == 0x0e) {
			input_report_key(d->keys, KEY_APPSELECT, !!(e[2] & 1));
			input_report_key(d->keys, KEY_BACK, !!(e[2] & 2));
			input_sync(d->keys);
			d->key_events++;
			continue;
		}
		if (slot >= 10 || (type != 3 && type != 4 && type != 5))
			continue;
		input_mt_slot(d->input, slot);
		input_mt_report_slot_state(d->input, MT_TOOL_FINGER, type != 4);
		if (type != 4) {
			input_report_abs(d->input, ABS_MT_POSITION_X,
					 e[1] | ((e[2] & 15) << 8));
			input_report_abs(d->input, ABS_MT_POSITION_Y,
					 (e[2] >> 4) | (e[3] << 4));
			input_report_abs(d->input, ABS_MT_TOUCH_MAJOR, e[4]);
			d->contacts++;
		}
	}
	input_mt_sync_frame(d->input);
	input_mt_report_pointer_emulation(d->input, false);
	input_sync(d->input);
	return IRQ_HANDLED;
}

static ssize_t diagnostics_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct s6_fts *d = dev_get_drvdata(dev);

	return sysfs_emit(
		buf,
		"chip=%02x%02x irqs=%lu contact_events=%lu i2c_errors=%lu key_events=%lu supplies_uV=3300000,1850000\n",
		d->id[1], d->id[2], READ_ONCE(d->irqs), READ_ONCE(d->contacts),
		READ_ONCE(d->errors), READ_ONCE(d->key_events));
}

static DEVICE_ATTR_RO(diagnostics);
static void s6_fts_disable(void *arg)
{
	struct s6_fts *d = arg;
	const u8 cmd[] = { 0xb6, 0, 0x1c, 0 };

	s6_fts_write(d, cmd, 4);
}

static int s6_keyled_set(struct led_classdev *led,
			 enum led_brightness brightness)
{
	struct s6_fts *d = container_of(led, struct s6_fts, keyled_class);
	int r;

	if (!!brightness == d->keyled_on)
		return 0;
	r = brightness ? regulator_enable(d->keyled) :
			 regulator_disable(d->keyled);
	if (!r)
		d->keyled_on = !!brightness;
	return r;
}

static void s6_keyled_off(void *arg)
{
	struct s6_fts *d = arg;

	if (d->keyled_on)
		regulator_disable(d->keyled);
}

static int s6_fts_probe(struct i2c_client *client)
{
	struct s6_fts *d;
	struct input_dev *input;
	int r, n;
	u8 cmd[] = { 0xb6, 0, 7 }, event[8];
	const u8 interrupt_off[] = { 0xb6, 0, 0x1c, 0 },
		 reset[] = { 0xb6, 0, 0x23, 1 },
		 interrupt_on[] = { 0xb6, 0, 0x1c, 0x41 };
	if (!of_machine_is_compatible("samsung,exynos7420-zerolte") ||
	    client->addr != 0x49)
		return -ENODEV;
	if (client->irq <= 0)
		return -EINVAL;
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;
	d = devm_kzalloc(&client->dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->client = client;
	i2c_set_clientdata(client, d);
	d->avdd = devm_regulator_get(&client->dev, "avdd");
	if (IS_ERR(d->avdd))
		return dev_err_probe(&client->dev, PTR_ERR(d->avdd),
				     "FTS AVDD\n");
	d->vdd = devm_regulator_get(&client->dev, "vdd");
	if (IS_ERR(d->vdd))
		return dev_err_probe(&client->dev, PTR_ERR(d->vdd),
				     "FTS IO supply\n");
	if (regulator_get_voltage(d->avdd) != 3300000 ||
	    regulator_get_voltage(d->vdd) != 1850000)
		return -EINVAL;
	/* Match Samsung: analog supply first, then I/O. Both stay always-on. */
	r = regulator_enable(d->avdd);
	if (r)
		return r;
	r = regulator_enable(d->vdd);
	if (r)
		return r;
	msleep(20);
	r = s6_fts_read(d, cmd, 3, d->id, 7);
	if (r)
		return dev_err_probe(&client->dev, r, "FTS chip-ID read\n");
	dev_dbg(&client->dev,
		"FTS ID %*ph; no firmware upgrade; verified3.3/1.85V supplies\n",
		7, d->id);
	if (d->id[1] != 0x39 || (d->id[2] != 0x80 && d->id[2] != 0x6c))
		return -ENODEV;
	if (d->id[2] == 0x6c && !d->id[4] && !d->id[5])
		return dev_err_probe(
			&client->dev, -ENODATA,
			"FTS calibration invalid; refusing NVM repair\n");
	r = s6_fts_write(d, interrupt_off, 4);
	if (r)
		return r;
	r = s6_fts_write(d, reset, 4);
	if (r)
		return r;
	msleep(20);
	for (n = 0; n < 50; n++) {
		u8 read_one = 0x85;

		r = s6_fts_read(d, &read_one, 1, event, 8);
		if (r)
			return r;
		if (event[0] == 0x10)
			break;
		if (event[0] == 0x0f)
			return dev_err_probe(&client->dev, -EIO,
					     "FTS reset error event\n");
		usleep_range(10000, 12000);
	}
	if (n == 50)
		return -ETIMEDOUT;
	r = s6_fts_cmd(d, 0x91);
	if (r)
		return r;
	msleep(20);
	r = s6_fts_cmd(d, 0x93);
	if (r)
		return r;
	r = s6_fts_cmd(d, 0x9b);
	if (r)
		return r;
	r = s6_fts_cmd(d, 0xa1);
	if (r)
		return r;
	input = devm_input_allocate_device(&client->dev);
	if (!input)
		return -ENOMEM;
	d->input = input;
	input->name = "Galaxy S6 Edge STM FTS";
	input->id.bustype = BUS_I2C;
	input_set_capability(input, EV_KEY, BTN_TOUCH);

	input_set_abs_params(input, ABS_X, 0, 4095, 0, 0);
	input_set_abs_params(input, ABS_Y, 0, 4095, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_X, 0, 4095, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, 4095, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	r = input_mt_init_slots(input, 10, INPUT_MT_DIRECT);
	if (r)
		return r;
	r = devm_request_threaded_irq(&client->dev, client->irq, NULL,
				      s6_fts_irq, IRQF_ONESHOT | IRQF_NO_AUTOEN,
				      "s6-fts-touch", d);
	if (r)
		return r;
	r = input_register_device(input);
	if (r)
		return r;
	d->keys = devm_input_allocate_device(&client->dev);
	if (!d->keys)
		return -ENOMEM;
	d->keys->name = "Galaxy S6 capacitive keys";
	d->keys->id.bustype = BUS_I2C;
	input_set_capability(d->keys, EV_KEY, KEY_APPSELECT);
	input_set_capability(d->keys, EV_KEY, KEY_BACK);
	r = input_register_device(d->keys);
	if (r)
		return r;

	if (of_find_property(client->dev.of_node, "keyled-supply", NULL)) {
		d->keyled = devm_regulator_get(&client->dev, "keyled");
		if (IS_ERR(d->keyled))
			return dev_err_probe(&client->dev, PTR_ERR(d->keyled),
					     "key LED supply\n");
		if (regulator_get_voltage(d->keyled) != 3300000)
			return -EINVAL;
		d->keyled_class.name = "s6::kbd_backlight";
		d->keyled_class.max_brightness = 1;
		d->keyled_class.brightness_set_blocking = s6_keyled_set;
		r = devm_add_action_or_reset(&client->dev, s6_keyled_off, d);
		if (r)
			return r;
		r = devm_led_classdev_register(&client->dev, &d->keyled_class);
		if (r)
			return r;
	}

	r = device_create_file(&client->dev, &dev_attr_diagnostics);
	if (r)
		return r;
	r = s6_fts_write(d, interrupt_on, 4);
	if (r)
		return r;
	r = devm_add_action_or_reset(&client->dev, s6_fts_disable, d);
	if (r)
		return r;
	enable_irq(client->irq);
	dev_dbg(&client->dev, "touch registered, ten slots4095x4095, IRQ%d\n",
		client->irq);
	return 0;
}

static const struct of_device_id s6_fts_match[] = {
	{ .compatible = "samsung,zerolte-fts" },
	{}
};

static struct i2c_driver s6_fts_driver = {
	.driver = { .name = "s6-fts-touch", .of_match_table = s6_fts_match },
	.probe = s6_fts_probe
};

module_i2c_driver(s6_fts_driver);
MODULE_LICENSE("GPL");
