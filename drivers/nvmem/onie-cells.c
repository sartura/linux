// SPDX-License-Identifier: GPL-2.0+
/*
 * ONIE NVMEM cells provider
 *
 * Author: Vadym Kochan <vadym.kochan@plvision.eu>
 */

#define ONIE_NVMEM_DRVNAME	"onie-nvmem-cells"

#define pr_fmt(fmt) ONIE_NVMEM_DRVNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kref.h>
#include <linux/nvmem-consumer.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define ONIE_NVMEM_TLV_MAX_LEN	2048

#define ONIE_NVMEM_HDR_ID	"TlvInfo"

struct onie_nvmem_hdr {
	u8 id[8];
	u8 version;
	__be16 data_len;
} __packed;

struct onie_nvmem_tlv {
	u8 type;
	u8 len;
	u8 val[0];
} __packed;

struct onie_nvmem_attr {
	struct list_head head;
	const char *name;
	unsigned int offset;
	unsigned int len;
};

struct onie_nvmem {
	struct platform_device *pdev;
	struct notifier_block nvmem_nb;
	unsigned int attr_count;
	struct list_head attrs;
	struct kref refcnt;
	const char *nvmem_match;

	struct nvmem_cell_lookup *cell_lookup;
	struct nvmem_cell_table cell_tbl;
	struct nvmem_device *nvmem;
};

static bool onie_nvmem_hdr_is_valid(struct onie_nvmem_hdr *hdr)
{
	if (memcmp(hdr->id, ONIE_NVMEM_HDR_ID, sizeof(hdr->id)) != 0)
		return false;
	if (hdr->version != 0x1)
		return false;

	return true;
}

static void onie_nvmem_attrs_free(struct onie_nvmem *onie)
{
	struct onie_nvmem_attr *attr, *tmp;

	list_for_each_entry_safe(attr, tmp, &onie->attrs, head) {
		list_del(&attr->head);
		kfree(attr);
	}
}

static const char *onie_nvmem_attr_name(u8 type)
{
	switch (type) {
	case 0x21: return "product-name";
	case 0x22: return "part-number";
	case 0x23: return "serial-number";
	case 0x24: return "mac-address";
	case 0x25: return "manufacture-date";
	case 0x26: return "device-version";
	case 0x27: return "label-revision";
	case 0x28: return "platforn-name";
	case 0x29: return "onie-version";
	case 0x2A: return "num-macs";
	case 0x2B: return "manufacturer";
	case 0x2C: return "country-code";
	case 0x2D: return "vendor";
	case 0x2E: return "diag-version";
	case 0x2F: return "service-tag";
	case 0xFD: return "vendor-extension";
	case 0xFE: return "crc32";

	default: return "unknown";
	}
}

static int onie_nvmem_tlv_parse(struct onie_nvmem *onie, u8 *data, u16 len)
{
	unsigned int hlen = sizeof(struct onie_nvmem_hdr);
	unsigned int offset = 0;
	int err;

	while (offset < len) {
		struct onie_nvmem_attr *attr;
		struct onie_nvmem_tlv *tlv;

		tlv = (struct onie_nvmem_tlv *)(data + offset);

		if (offset + tlv->len >= len) {
			struct nvmem_device *nvmem = onie->nvmem;

			pr_err("%s: TLV len is too big(0x%x) at 0x%x\n",
			       nvmem_dev_name(nvmem), tlv->len, hlen + offset);

			/* return success in case something was parsed */
			return 0;
		}

		attr = kmalloc(sizeof(*attr), GFP_KERNEL);
		if (!attr) {
			err = -ENOMEM;
			goto err_attr_alloc;
		}

		attr->name = onie_nvmem_attr_name(tlv->type);
		/* skip 'type' and 'len' */
		attr->offset = hlen + offset + 2;
		attr->len = tlv->len;

		list_add(&attr->head, &onie->attrs);
		onie->attr_count++;

		offset += sizeof(*tlv) + tlv->len;
	}

	return 0;

err_attr_alloc:
	onie_nvmem_attrs_free(onie);

	return err;
}

static int onie_nvmem_decode(struct onie_nvmem *onie)
{
	struct nvmem_device *nvmem = onie->nvmem;
	struct onie_nvmem_hdr hdr;
	u8 *data;
	u16 len;
	int ret;

	ret = nvmem_device_read(nvmem, 0, sizeof(hdr), &hdr);
	if (ret < 0)
		return ret;

	if (!onie_nvmem_hdr_is_valid(&hdr)) {
		pr_err("%s: invalid ONIE TLV header\n", nvmem_dev_name(nvmem));
		ret = -EINVAL;
		goto err_invalid;
	}

	len = be16_to_cpu(hdr.data_len);

	if (len > ONIE_NVMEM_TLV_MAX_LEN)
		len = ONIE_NVMEM_TLV_MAX_LEN;

	data = kmalloc(len, GFP_KERNEL);
	if (!data) {
		ret = -ENOMEM;
		goto err_kmalloc;
	}

	ret = nvmem_device_read(nvmem, sizeof(hdr), len, data);
	if (ret < 0)
		goto err_data_read;

	ret = onie_nvmem_tlv_parse(onie, data, len);
	if (ret)
		goto err_info_parse;

	kfree(data);

	return 0;

err_info_parse:
err_data_read:
	kfree(data);
err_kmalloc:
err_invalid:
	return ret;
}

static int onie_nvmem_cells_parse(struct onie_nvmem *onie)
{
	struct platform_device *pdev = onie->pdev;
	struct nvmem_device *nvmem = onie->nvmem;
	struct device *dev = &pdev->dev;
	struct nvmem_cell_info *cells;
	struct onie_nvmem_attr *attr;
	unsigned int ncells = 0;
	int err;

	INIT_LIST_HEAD(&onie->attrs);
	onie->attr_count = 0;

	err = onie_nvmem_decode(onie);
	if (err)
		return err;

	if (!onie->attr_count) {
		pr_err("%s: has no ONIE attributes\n", nvmem_dev_name(nvmem));
		return -EINVAL;
	}

	cells = kmalloc_array(onie->attr_count, sizeof(*cells), GFP_KERNEL);
	if (!cells) {
		err = -ENOMEM;
		goto err_cells_alloc;
	}

	onie->cell_lookup = kmalloc_array(onie->attr_count,
					  sizeof(struct nvmem_cell_lookup),
					  GFP_KERNEL);
	if (!onie->cell_lookup) {
		err = -ENOMEM;
		goto err_lookup_alloc;
	}

	list_for_each_entry(attr, &onie->attrs, head) {
		struct nvmem_cell_lookup *lookup;
		struct nvmem_cell_info *cell;

		cell = &cells[ncells];

		lookup = &onie->cell_lookup[ncells];
		lookup->con_id = NULL;

		cell->offset = attr->offset;
		cell->name = attr->name;
		cell->bytes = attr->len;
		cell->bit_offset = 0;
		cell->nbits = 0;

		lookup->nvmem_name = nvmem_dev_name(onie->nvmem);
		lookup->dev_id = dev_name(dev);
		lookup->cell_name = cell->name;
		lookup->con_id = cell->name;

		ncells++;
	}

	onie->cell_tbl.nvmem_name = nvmem_dev_name(onie->nvmem);
	onie->cell_tbl.ncells = ncells;
	onie->cell_tbl.cells = cells;

	nvmem_add_cell_table(&onie->cell_tbl);
	nvmem_add_cell_lookups(onie->cell_lookup, ncells);

	onie_nvmem_attrs_free(onie);

	return 0;

err_lookup_alloc:
	kfree(onie->cell_tbl.cells);
err_cells_alloc:
	onie_nvmem_attrs_free(onie);

	return err;
}

static void onie_nvmem_release(struct kref *kref)
{
	kfree(container_of(kref, struct onie_nvmem, refcnt));
}

static void onie_nvmem_get(struct onie_nvmem *onie)
{
	kref_get(&onie->refcnt);
}

static void onie_nvmem_put(struct onie_nvmem *onie)
{
	kref_put(&onie->refcnt, onie_nvmem_release);
}

static int onie_nvmem_notify(struct notifier_block *nb,
			     unsigned long val, void *data)
{
	struct nvmem_device *nvmem = data;
	struct onie_nvmem *onie;
	int err;

	if (val != NVMEM_PRE_ADD && val != NVMEM_REMOVE)
		return NOTIFY_DONE;

	onie = container_of(nb, struct onie_nvmem, nvmem_nb);

	if (strcmp(onie->nvmem_match, nvmem_dev_name(nvmem)) != 0)
		return NOTIFY_DONE;

	switch (val) {
	case NVMEM_PRE_ADD:
		onie->nvmem = nvmem;

		err = onie_nvmem_cells_parse(onie);
		if (err)
			return NOTIFY_BAD;

		onie_nvmem_get(onie);
		break;

	case NVMEM_REMOVE:
		nvmem_del_cell_lookups(onie->cell_lookup, onie->attr_count);
		nvmem_del_cell_table(&onie->cell_tbl);

		kfree(onie->cell_tbl.cells);
		kfree(onie->cell_lookup);

		onie_nvmem_put(onie);
		break;

	default:
		return NOTIFY_DONE;
	}

	return NOTIFY_OK;
}

static int onie_nvmem_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct onie_nvmem *onie;
	const char *nvmem_match;
	int err;

	err = of_property_read_string(np, "nvmem-name", &nvmem_match);
	if (err) {
		dev_err(dev, "error while parsing 'nvmem-name' property\n");
		return err;
	}

	onie = kmalloc(sizeof(*onie), GFP_KERNEL);
	if (!onie)
		return -ENOMEM;

	kref_init(&onie->refcnt);

	onie->nvmem_nb.notifier_call = onie_nvmem_notify;
	onie->nvmem_match = nvmem_match;
	onie->pdev = pdev;

	dev_set_drvdata(dev, onie);

	return nvmem_register_notifier(&onie->nvmem_nb);
}

static int onie_nvmem_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct onie_nvmem *onie;

	onie = dev_get_drvdata(dev);

	nvmem_unregister_notifier(&onie->nvmem_nb);
	onie_nvmem_put(onie);

	return 0;
}

static const struct of_device_id onie_nvmem_match[] = {
	{
		.compatible = "onie,nvmem-cells",
	},
	{ }
};
MODULE_DEVICE_TABLE(of, onie_nvmem_match);

static struct platform_driver onie_nvmem_driver = {
	.probe = onie_nvmem_probe,
	.remove = onie_nvmem_remove,
	.driver = {
		.name = ONIE_NVMEM_DRVNAME,
		.of_match_table = onie_nvmem_match,
	},
};

static int __init onie_nvmem_init(void)
{
	return platform_driver_register(&onie_nvmem_driver);
}

static void __exit onie_nvmem_exit(void)
{
	platform_driver_unregister(&onie_nvmem_driver);
}

subsys_initcall(onie_nvmem_init);
module_exit(onie_nvmem_exit);

MODULE_AUTHOR("Vadym Kochan <vadym.kochan@plvision.eu>");
MODULE_DESCRIPTION("ONIE NVMEM cells driver");
MODULE_LICENSE("GPL");
