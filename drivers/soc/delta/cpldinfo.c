// SPDX-License-Identifier: GPL-2.0-only
/*
 * Delta Networks CPLD info driver
 *
 * Copyright (C) 2021 Sartura Ltd.
 *
 * Author: Robert Marko <robert.marko@sartura.hr>
 */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define HARDWARE_VERSION_REG	0x00
#define BOARD_ID_REG		0x01
#define CPLD_CODE_VERSION_REG	0x02
#define PSU_DEVICE_STATUS_REG	0x0a

#define PSU1_PRESENTN		BIT(0)
#define PSU2_PRESENTN		BIT(1)
#define PSU1_PG			BIT(2)
#define PSU2_PG			BIT(3)
#define PSU1_ALERT		BIT(4)
#define PSU2_ALERT		BIT(5)

struct board_id {
	unsigned int id;
	const char *name;
};

struct cpldinfo_data {
	struct regmap *regmap;
	struct board_id board_id_data;
	struct dentry *debugfs_dir;
};

static const struct board_id board_id[] = {
	{ 0xa, "TN48M-DN" },
	{ 0xb, "TN48M-P-DN" },
	{ 0xc, "TN4810M-DN" },
	{ 0xd, "TN48M2" },
	{ 0xe, "TX4810-DN" },
};

static int cpldinfo_hw_version(struct cpldinfo_data *data)
{
	unsigned int regval;
	int ret;

	ret = regmap_read(data->regmap,
			  HARDWARE_VERSION_REG,
			  &regval);
	if (ret < 0)
		return ret;

	return 0;
}

static int cpldinfo_board_id(struct cpldinfo_data *data)
{
	unsigned int regval;
	int ret, i;

	ret = regmap_read(data->regmap,
			  BOARD_ID_REG,
			  &regval);
	if (ret < 0)
		return ret;

	for (i = 0; i < ARRAY_SIZE(board_id); i++) {
		if (board_id[i].id == regval) {
			data->board_id_data.id = board_id[i].id;
			data->board_id_data.name = board_id[i].name;
			break;
		}
	}

	return 0;
}

static int cpldinfo_board_name_show(struct seq_file *s, void *data)
{
	struct cpldinfo_data *priv = s->private;

	seq_printf(s, "%s\n", priv->board_id_data.name);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(cpldinfo_board_name);

static int cpldinfo_board_id_show(struct seq_file *s, void *data)
{
	struct cpldinfo_data *priv = s->private;

	seq_printf(s, "0x%x\n", priv->board_id_data.id);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(cpldinfo_board_id);

static int hardware_version_id_show(struct seq_file *s, void *data)
{
	struct cpldinfo_data *priv = s->private;
	unsigned int regval;
	int ret;

	ret = regmap_read(priv->regmap,
			  HARDWARE_VERSION_REG,
			  &regval);
	if (ret < 0)
		return ret;

	seq_printf(s, "0x%x\n", regval);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(hardware_version_id);

static int cpld_code_version_show(struct seq_file *s, void *data)
{
	struct cpldinfo_data *priv = s->private;
	unsigned int regval;
	int ret;

	ret = regmap_read(priv->regmap,
			  CPLD_CODE_VERSION_REG,
			  &regval);
	if (ret < 0)
		return ret;

	seq_printf(s, "%d\n", regval);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(cpld_code_version);

static int psu1_present_show(struct seq_file *s, void *data)
{
	struct cpldinfo_data *priv = s->private;
	unsigned int regval;
	int ret;

	ret = regmap_read(priv->regmap,
			  PSU_DEVICE_STATUS_REG,
			  &regval);
	if (ret < 0)
		return ret;

	seq_printf(s, "%d\n", !FIELD_GET(PSU1_PRESENTN, regval));

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(psu1_present);

static int psu2_present_show(struct seq_file *s, void *data)
{
	struct cpldinfo_data *priv = s->private;
	unsigned int regval;
	int ret;

	ret = regmap_read(priv->regmap,
			  PSU_DEVICE_STATUS_REG,
			  &regval);
	if (ret < 0)
		return ret;

	seq_printf(s, "%d\n", !FIELD_GET(PSU2_PRESENTN, regval));

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(psu2_present);

static int psu1_pg_show(struct seq_file *s, void *data)
{
	struct cpldinfo_data *priv = s->private;
	unsigned int regval;
	int ret;

	ret = regmap_read(priv->regmap,
			  PSU_DEVICE_STATUS_REG,
			  &regval);
	if (ret < 0)
		return ret;

	seq_printf(s, "%ld\n", FIELD_GET(PSU1_PG, regval));

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(psu1_pg);

static int psu2_pg_show(struct seq_file *s, void *data)
{
	struct cpldinfo_data *priv = s->private;
	unsigned int regval;
	int ret;

	ret = regmap_read(priv->regmap,
			  PSU_DEVICE_STATUS_REG,
			  &regval);
	if (ret < 0)
		return ret;

	seq_printf(s, "%ld\n", FIELD_GET(PSU2_PG, regval));

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(psu2_pg);

static int psu1_alert_show(struct seq_file *s, void *data)
{
	struct cpldinfo_data *priv = s->private;
	unsigned int regval;
	int ret;

	ret = regmap_read(priv->regmap,
			  PSU_DEVICE_STATUS_REG,
			  &regval);
	if (ret < 0)
		return ret;

	seq_printf(s, "%d\n", !FIELD_GET(PSU1_ALERT, regval));

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(psu1_alert);

static int psu2_alert_show(struct seq_file *s, void *data)
{
	struct cpldinfo_data *priv = s->private;
	unsigned int regval;
	int ret;

	ret = regmap_read(priv->regmap,
			  PSU_DEVICE_STATUS_REG,
			  &regval);
	if (ret < 0)
		return ret;

	seq_printf(s, "%d\n", !FIELD_GET(PSU2_ALERT, regval));

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(psu2_alert);

static void cpldinfo_debugfs_init(struct cpldinfo_data *data)
{
	data->debugfs_dir = debugfs_create_dir("delta_cpldinfo", NULL);

	debugfs_create_file("board_name",
			    0400,
			    data->debugfs_dir,
			    data,
			    &cpldinfo_board_name_fops);

	debugfs_create_file("board_id",
			    0400,
			    data->debugfs_dir,
			    data,
			    &cpldinfo_board_id_fops);

	debugfs_create_file("hardware_version_id",
			    0400,
			    data->debugfs_dir,
			    data,
			    &hardware_version_id_fops);

	debugfs_create_file("cpld_code_version",
			    0400,
			    data->debugfs_dir,
			    data,
			    &cpld_code_version_fops);

	debugfs_create_file("psu1_present",
			    0400,
			    data->debugfs_dir,
			    data,
			    &psu1_present_fops);

	debugfs_create_file("psu2_present",
			    0400,
			    data->debugfs_dir,
			    data,
			    &psu2_present_fops);

	debugfs_create_file("psu1_pg",
			    0400,
			    data->debugfs_dir,
			    data,
			    &psu1_pg_fops);

	debugfs_create_file("psu2_pg",
			    0400,
			    data->debugfs_dir,
			    data,
			    &psu2_pg_fops);

	debugfs_create_file("psu1_alert",
			    0400,
			    data->debugfs_dir,
			    data,
			    &psu1_alert_fops);

	debugfs_create_file("psu2_alert",
			    0400,
			    data->debugfs_dir,
			    data,
			    &psu2_alert_fops);
}

static int delta_cpldinfo_probe(struct platform_device *pdev)
{
	struct cpldinfo_data *data;
	struct regmap *regmap;
	int ret;

	regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!regmap)
		return -ENODEV;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->regmap = regmap;

	platform_set_drvdata(pdev, data);

	ret = cpldinfo_board_id(data);
	if (ret < 0)
		return ret;

	cpldinfo_hw_version(data);

	cpldinfo_debugfs_init(data);

	return 0;
}

static int delta_cpldinfo_remove(struct platform_device *pdev)
{
	struct cpldinfo_data *data = platform_get_drvdata(pdev);

	debugfs_remove_recursive(data->debugfs_dir);

	return 0;
}

static const struct of_device_id delta_cpldinfo_of_match[] = {
	{ .compatible = "delta,cpldinfo", },
	{ }
};
MODULE_DEVICE_TABLE(of, delta_cpldinfo_of_match);

static struct platform_driver delta_cpldinfo_driver = {
	.probe = delta_cpldinfo_probe,
	.remove = delta_cpldinfo_remove,
	.driver  = {
		.name = "delta-cpldinfo",
		.of_match_table = delta_cpldinfo_of_match,
	},
};

module_platform_driver(delta_cpldinfo_driver);

MODULE_AUTHOR("Robert Marko <robert.marko@sartura.hr>");
MODULE_DESCRIPTION("Delta Networks CPLD info driver");
MODULE_LICENSE("GPL v2");
