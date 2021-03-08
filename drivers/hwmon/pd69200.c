// SPDX-License-Identifier: GPL-2.0-only
/*
 * Microchip PD69200 HWMON driver
 *
 * Copyright 2021 Sartura Ltd.
 *
 * Author: Robert Marko <robert.marko@sartura.hr>
 */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/hwmon-sysfs.h>
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#define PD69200_MESSAGE_LENGTH		15
#define PD69200_MESSAGE_ECHO		1
#define PD69200_MESSAGE_CHECKSUM_HIGH	13
#define PD69200_MESSAGE_CHECKSUM_LOW	14

#define PD69200_KEY_COMMAND	0x0
#define PD69200_KEY_PROGRAM	0x1
#define PD69200_KEY_REQUEST	0x2
#define PD69200_KEY_TELEMETRY	0x3
#define PD69200_KEY_TEST	0x4
#define PD69200_KEY_REPORT	0x52

#define PD69200_SUBJECT_GLOBAL 	0x7
#define PD69200_SUBJECT_CHANNEL 0x5

#define PD69200_CHANNEL_SUBJECT_MASK	GENMASK(7, 4)
#define PD69200_CHANNEL_COMMAND_MASK	GENMASK(3, 0)

enum pd69200_power_type {
	PD69200_POWER_CONSUMPTION = 0,
	PD69200_POWER_CALCULATED,
	PD69200_POWER_AVAILABLE,
	PD69200_POWER_LIMIT,
};

struct pd69200_data {
	struct device *dev;
	struct i2c_client *client;
	struct dentry *debugfs_dir;
};

static u16 pd69200_checksum(u8 *buf)
{
	u16 checksum;
	int i;

	for (i = 0; i < 13; i++)
		checksum += buf[i];

	return checksum;
}

/**
 * pd69200_send - issue a single I2C message to PD69200
 * @data: Device structure
 * @buf: Data that will be written to the PD69200
 *
 * Returns pseudo-random number echo value, or else negative errno.
 */
static int pd69200_send(struct pd69200_data *data, u8 *buf)
{
	u16 checksum;
	int ret;
	u8 echo;

	/*
	 * Pseudo-random 8 bit value to synchronise sent and received
	 * messages. PD69200 will use the same number when replying.
	 */
	prandom_bytes(&echo, 1);
	buf[PD69200_MESSAGE_ECHO] = echo;

	/*
	 * 16 bit checksum  is used for integrity validation.
	 * It is simply a arithemitc sum of first 13 message bytes.
	 */
	checksum = pd69200_checksum(buf);
	buf[PD69200_MESSAGE_CHECKSUM_HIGH] = (checksum >> 8) & 0xff;
	buf[PD69200_MESSAGE_CHECKSUM_LOW] = checksum & 0xff;

	ret = i2c_master_send(data->client, buf, PD69200_MESSAGE_LENGTH);
	if (ret != PD69200_MESSAGE_LENGTH) {
		ret = ret < 0 ? ret : -EIO;
		return ret;
	}

	/*
	 * Requires 300ms at least before attempting to read
	 */
	usleep_range(35000, 35000 + 1000);

	return echo;
}

/**
 * pd69200_receive - issue a single I2C message to PD69200
 * @data: Device structure
 * @buf: Buffer to store received data
 * @echo: pseudo-random number echo value used to send the message
 *
 * Returns zero, or else negative errno.
 */
static int pd69200_receive(struct pd69200_data *data, u8 *buf, u8 echo)
{
	u16 checksum;
	int ret;

	ret = i2c_master_recv(data->client, buf, PD69200_MESSAGE_LENGTH);
	if (ret != PD69200_MESSAGE_LENGTH) {
		ret = ret < 0 ? ret : -EIO;
		return ret;
	}

	if (buf[PD69200_MESSAGE_ECHO] != echo)
		return -EINVAL;

	checksum = (buf[13] && 0xff) | ((buf[14] && 0xff) << 8);
	if (checksum != pd69200_checksum(buf))
		return -EINVAL;

	return 0;
}

static int pd69200_read_power(struct pd69200_data *data, int channel,
			      long *val)
{
	u16 power;
	u8 echo;
	int ret;

	u8 command[PD69200_MESSAGE_LENGTH];
	u8 buf[PD69200_MESSAGE_LENGTH];

	command[0] = PD69200_KEY_REQUEST;
	command[2] = PD69200_SUBJECT_GLOBAL;
	command[3] = 0xb;
	command[4] = 0x60;
	command[5] = 0x4e;
	command[6] = 0x4e;
	command[7] = 0x4e;
	command[8] = 0x4e;
	command[9] = 0x4e;
	command[10] = 0x4e;
	command[11] = 0x4e;
	command[12] = 0x4e;

	echo = pd69200_send(data, command);
	if (echo < 0)
		return echo;

	ret = pd69200_receive(data, buf, echo);
	if (!echo)
		return ret;

	switch (channel) {
	case PD69200_POWER_CONSUMPTION:
		power = buf[2] << 8 | buf[3];
		break;
	case PD69200_POWER_CALCULATED:
		power = buf[4] << 8 | buf[5];
		break;
	case PD69200_POWER_AVAILABLE:
		power = buf[6] << 8 | buf[7];
		break;
	case PD69200_POWER_LIMIT:
		power = buf[8] << 8 | buf[9];
		break;
	}

	*val = power * 1000000;

	return 0;
}

static int pd69200_read_voltage(struct pd69200_data *data, long *val)
{
	u16 voltage;
	u8 echo;
	int ret;

	u8 command[PD69200_MESSAGE_LENGTH];
	u8 buf[PD69200_MESSAGE_LENGTH];

	command[0] = PD69200_KEY_REQUEST;
	command[2] = PD69200_SUBJECT_GLOBAL;
	command[3] = 0xb;
	command[4] = 0x1a;
	command[5] = 0x4e;
	command[6] = 0x4e;
	command[7] = 0x4e;
	command[8] = 0x4e;
	command[9] = 0x4e;
	command[10] = 0x4e;
	command[11] = 0x4e;
	command[12] = 0x4e;

	echo = pd69200_send(data, command);
	if (echo < 0)
		return echo;

	ret = pd69200_receive(data, buf, echo);
	if (!echo)
		return ret;

	voltage = buf[2] << 8 | buf[3];

	*val = voltage * 100;

	return 0;
}

static int pd69200_read_current_ma(struct pd69200_data *data, long *val)
{
	u16 current_ma;
	u8 echo;
	int ret;

	u8 command[PD69200_MESSAGE_LENGTH];
	u8 buf[PD69200_MESSAGE_LENGTH];

	command[0] = PD69200_KEY_REQUEST;
	command[2] = PD69200_SUBJECT_GLOBAL;
	command[3] = 0xb;
	command[4] = 0x1a;
	command[5] = 0x4e;
	command[6] = 0x4e;
	command[7] = 0x4e;
	command[8] = 0x4e;
	command[9] = 0x4e;
	command[10] = 0x4e;
	command[11] = 0x4e;
	command[12] = 0x4e;

	echo = pd69200_send(data, command);
	if (echo < 0)
		return echo;

	ret = pd69200_receive(data, buf, echo);
	if (!echo)
		return ret;

	current_ma = buf[7] << 8 | buf[8];

	*val = current_ma * 100;

	return 0;
}

static int pd69200_port_disable(struct pd69200_data *data, int channel)
{
	u8 port_cmd = 0;
	u8 echo;

	u8 command[PD69200_MESSAGE_LENGTH];

	port_cmd &= ~PD69200_CHANNEL_SUBJECT_MASK;
	port_cmd |= FIELD_PREP(PD69200_CHANNEL_SUBJECT_MASK, 0x0);
	port_cmd &= ~PD69200_CHANNEL_COMMAND_MASK;
	port_cmd &= ~BIT(0);

	command[0] = PD69200_KEY_COMMAND;
	command[2] = PD69200_SUBJECT_CHANNEL;
	command[3] = 0x0c;
	command[4] = channel;
	command[5] = port_cmd;
	command[6] = 0x1;
	command[7] = 0x4e;
	command[8] = 0x4e;
	command[9] = 0x4e;
	command[10] = 0x4e;
	command[11] = 0x4e;
	command[12] = 0x4e;

	echo = pd69200_send(data, command);
	if (echo < 0)
		return echo;

	return 0;
}

static int pd69200_port_enable(struct pd69200_data *data, int channel)
{
	u8 port_cmd = 0;
	u8 echo;

	u8 command[PD69200_MESSAGE_LENGTH];

	port_cmd &= ~PD69200_CHANNEL_SUBJECT_MASK;
	port_cmd |= FIELD_PREP(PD69200_CHANNEL_SUBJECT_MASK, 0x0);
	port_cmd &= ~PD69200_CHANNEL_COMMAND_MASK;
	port_cmd |= BIT(0);

	command[0] = PD69200_KEY_COMMAND;
	command[2] = PD69200_SUBJECT_CHANNEL;
	command[3] = 0x0c;
	command[4] = channel;
	command[5] = port_cmd;
	command[6] = 0x1;
	command[7] = 0x4e;
	command[8] = 0x4e;
	command[9] = 0x4e;
	command[10] = 0x4e;
	command[11] = 0x4e;
	command[12] = 0x4e;

	echo = pd69200_send(data, command);
	if (echo < 0)
		return echo;

	return 0;
}

static int pd69200_read_port_status(struct pd69200_data *data, int channel, long *val)
{
	u8 echo, status_buf;
	int ret;

	u8 command[PD69200_MESSAGE_LENGTH];
	u8 buf[PD69200_MESSAGE_LENGTH];

	command[0] = PD69200_KEY_REQUEST;
	command[2] = PD69200_SUBJECT_GLOBAL;
	command[3] = 0x0c;
	command[4] = 0x4e;
	command[5] = 0x4e;
	command[6] = 0x4e;
	command[7] = 0x4e;
	command[8] = 0x4e;
	command[9] = 0x4e;
	command[10] = 0x4e;
	command[11] = 0x4e;
	command[12] = 0x4e;

	echo = pd69200_send(data, command);
	if (echo < 0)
		return echo;

	ret = pd69200_receive(data, buf, echo);
	if (!echo)
		return ret;

	if (channel <= 7)
		status_buf = buf[2];
	else if (channel > 7 && channel <= 15)
		status_buf = buf[3];
	else if (channel > 15 && channel <= 23)
		status_buf = buf[4];
	else if (channel > 23 && channel <= 31)
		status_buf = buf[6];
	else if (channel > 31 && channel <= 39)
		status_buf = buf[7];
	else if (channel > 39 && channel <= 47)
		status_buf = buf[8];
	else
		return -EINVAL;

	if (BIT(channel / 8) & status_buf)
		*val = 1;
	else
		*val = 0;

	return 0;
}

static umode_t pd69200_is_visible(const void *data, enum hwmon_sensor_types type,
				  u32 attr, int channel)
{
	switch (type) {
	case hwmon_in:
		switch (attr) {
		case hwmon_in_input:
		case hwmon_in_label:
			return 0444;
		case hwmon_in_enable:
			return 0644;
		default:
			return 0;
		}
	case hwmon_curr:
		switch (attr) {
		case hwmon_curr_input:
		case hwmon_curr_label:
			return 0444;
		default:
			return 0;
		}
	case hwmon_power:
		switch (attr) {
		case hwmon_power_input:
		case hwmon_power_label:
			return 0444;
		default:
			return 0;
		}
	default:
		return 0;
	}
}

static const char * const pd69200_power_label[] = {
	"Power consumption",
	"Calculated power",
	"Available power",
	"Power limit",
};

static const char * const pd69200_port_label[] = {
	"Port1",
	"Port2",
	"Port3",
	"Port4",
	"Port5",
	"Port6",
	"Port7",
	"Port8",
	"Port9",
	"Port10",
	"Port11",
	"Port12",
	"Port13",
	"Port14",
	"Port15",
	"Port16",
	"Port17",
	"Port18",
	"Port19",
	"Port20",
	"Port21",
	"Port22",
	"Port23",
	"Port24",
	"Port25",
	"Port26",
	"Port27",
	"Port28",
	"Port29",
	"Port30",
	"Port31",
	"Port32",
	"Port33",
	"Port34",
	"Port35",
	"Port36",
	"Port37",
	"Port38",
	"Port39",
	"Port40",
	"Port41",
	"Port42",
	"Port43",
	"Port44",
	"Port45",
	"Port46",
	"Port47",
	"Port48",
	"Input",
};

static int pd69200_read_string(struct device *dev,
			       enum hwmon_sensor_types type,
			       u32 attr, int channel, const char **str)
{
	switch (type) {
	case hwmon_in:
	case hwmon_curr:
		*str = pd69200_port_label[channel];
		break;
	case hwmon_power:
		*str = pd69200_power_label[channel];
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int pd69200_write(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long val)
{
	struct pd69200_data *data = dev_get_drvdata(dev);
	int err;

	switch (type) {
	case hwmon_in:
		switch (attr) {
		case hwmon_in_enable:
			if (val == 0)
				err = pd69200_port_disable(data, channel);
			else if (val == 1)
				err = pd69200_port_enable(data, channel);
			else
				err = -EINVAL;
			break;
		default:
			return -EOPNOTSUPP;
		}
		break;
	default:
		return -EOPNOTSUPP;
	}

	return err;
}

static int pd69200_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
{
	struct pd69200_data *data = dev_get_drvdata(dev);
	int err;

	switch (type) {
	case hwmon_in:
		switch (attr) {
		case hwmon_in_input:
			err = pd69200_read_voltage(data, val);
			break;
		case hwmon_in_enable:
			err = pd69200_read_port_status(data, channel, val);
			break;
		default:
			return -EOPNOTSUPP;
		}
		break;
	case hwmon_curr:
		switch (attr) {
		case hwmon_curr_input:
			err = pd69200_read_current_ma(data, val);
			break;
		default:
			return -EOPNOTSUPP;
		}
		break;
	case hwmon_power:
		switch (attr) {
		case hwmon_power_input:
			err = pd69200_read_power(data, channel, val);
			break;
		default:
			return -EOPNOTSUPP;
		}
		break;
	default:
		return -EOPNOTSUPP;
	}

	return err;
}

static const struct hwmon_channel_info *pd69200_info[] = {
	HWMON_CHANNEL_INFO(chip,
			   HWMON_C_REGISTER_TZ),
	HWMON_CHANNEL_INFO(in,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL),
	HWMON_CHANNEL_INFO(curr,
			   HWMON_C_INPUT | HWMON_C_LABEL),
	HWMON_CHANNEL_INFO(power,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL),
	NULL
};

static const struct hwmon_ops pd69200_hwmon_ops = {
	.is_visible = pd69200_is_visible,
	.read = pd69200_read,
	.read_string = pd69200_read_string,
	.write = pd69200_write,
};

static const struct hwmon_chip_info pd69200_chip_info = {
	.ops = &pd69200_hwmon_ops,
	.info = pd69200_info,
};

static int pd69200_firmware_version_show(struct seq_file *s, void *data)
{
	struct pd69200_data *priv = s->private;
	u16 sw_version;
	u8 echo;
	int ret;

	u8 command[PD69200_MESSAGE_LENGTH];
	u8 buf[PD69200_MESSAGE_LENGTH];

	command[0] = PD69200_KEY_REQUEST;
	command[2] = PD69200_SUBJECT_GLOBAL;
	command[3] = 0x1e;
	command[4] = 0x21;
	command[5] = 0x4e;
	command[6] = 0x4e;
	command[7] = 0x4e;
	command[8] = 0x4e;
	command[9] = 0x4e;
	command[10] = 0x4e;
	command[11] = 0x4e;
	command[12] = 0x4e;

	echo = pd69200_send(priv, command);
	if (echo < 0)
		return echo;

	ret = pd69200_receive(priv, buf, echo);
	if (!echo)
		return ret;

	sw_version = buf[5] << 8 | buf[6];

	seq_printf(s, "%d.", sw_version/100);
	seq_printf(s, "%d", (sw_version/10)%10);
	seq_printf(s, "%d\n", sw_version%10);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(pd69200_firmware_version);

static void pd69200_init_debugfs(struct pd69200_data *data)
{
	data->debugfs_dir = debugfs_create_dir(data->client->name, NULL);

	debugfs_create_file("firmware_version",
			    0400,
			    data->debugfs_dir,
			    data,
			    &pd69200_firmware_version_fops);
}

static int pd69200_probe(struct i2c_client *client)
{
	struct pd69200_data *data;
	struct device *hwmon_dev;

	data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	data->dev = &client->dev;
	i2c_set_clientdata(client, data);

	hwmon_dev = devm_hwmon_device_register_with_info(&client->dev, client->name,
							 data, &pd69200_chip_info,
							 NULL);

	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);

	pd69200_init_debugfs(data);

	return 0;
}

static int pd69200_remove(struct i2c_client *client)
{
	struct pd69200_data *data = i2c_get_clientdata(client);

	debugfs_remove_recursive(data->debugfs_dir);

	return 0;
}

static const struct of_device_id pd69200_of_match[] = {
	{ .compatible = "microchip,pd69200"},
	{ }
};
MODULE_DEVICE_TABLE(of, pd69200_of_match);

static struct i2c_driver pd69200_driver = {
	.driver = {
		.name = "pd69200",
		.of_match_table = pd69200_of_match,
	},
	.probe_new	= pd69200_probe,
	.remove		= pd69200_remove,
};
module_i2c_driver(pd69200_driver);

MODULE_AUTHOR("Robert Marko <robert.marko@sartura.hr>");
MODULE_DESCRIPTION("Microchip PD69200 HWMON driver");
MODULE_LICENSE("GPL");
