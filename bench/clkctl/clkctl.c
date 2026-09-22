// clkctl.c — minimal debugfs clock control for A733 bring-up testing.
// Exposes /sys/kernel/debug/clkctl/<name>: read = current rate, write = clk_set_rate().
// Used to sweep GPU / DSU clock rates at runtime instead of rebooting per frequency.
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

static struct dentry *clkctl_dir;

struct clkctl_entry {
	const char *file;
	const char *node_path;
	const char *clk_name;
	int index;			/* >=0: of_clk_get(np, index); else by name */
	struct clk *clk;
};

static struct clkctl_entry entries[] = {
	{ "gpu_clk",    "/soc@3000000/gpu@1800000", "clk",         -1 },
	{ "gpu_parent", "/soc@3000000/gpu@1800000", "clk_parent",  -1 },
	{ "dsu",        "/dsufreq@0",               NULL,           0 },
};

static ssize_t rate_read(struct file *f, char __user *ubuf, size_t len, loff_t *off)
{
	struct clkctl_entry *e = f->private_data;
	char buf[32];
	int n;

	if (!e->clk)
		return -ENODEV;
	n = scnprintf(buf, sizeof(buf), "%lu\n", clk_get_rate(e->clk));
	return simple_read_from_buffer(ubuf, len, off, buf, n);
}

static ssize_t rate_write(struct file *f, const char __user *ubuf, size_t len, loff_t *off)
{
	struct clkctl_entry *e = f->private_data;
	char buf[32];
	unsigned long rate;
	int ret;

	if (!e->clk)
		return -ENODEV;
	if (len == 0 || len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';
	if (kstrtoul(buf, 10, &rate))
		return -EINVAL;

	ret = clk_set_rate(e->clk, rate);
	if (ret)
		return ret;
	pr_info("clkctl: %s set to %lu (actual %lu)\n", e->file, rate, clk_get_rate(e->clk));
	return len;
}

static const struct file_operations rate_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = rate_read,
	.write = rate_write,
	.llseek = default_llseek,
};

static int __init clkctl_init(void)
{
	int i;

	clkctl_dir = debugfs_create_dir("clkctl", NULL);
	if (!clkctl_dir)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(entries); i++) {
		struct clkctl_entry *e = &entries[i];
		struct device_node *np = of_find_node_by_path(e->node_path);

		if (!np) {
			pr_warn("clkctl: node %s not found\n", e->node_path);
			continue;
		}
		if (e->index >= 0)
			e->clk = of_clk_get(np, e->index);
		else
			e->clk = of_clk_get_by_name(np, e->clk_name);
		of_node_put(np);

		if (IS_ERR(e->clk)) {
			pr_warn("clkctl: clock %s not available (%ld)\n", e->file, PTR_ERR(e->clk));
			e->clk = NULL;
			continue;
		}
		debugfs_create_file(e->file, 0600, clkctl_dir, e, &rate_fops);
		pr_info("clkctl: %s -> %lu Hz\n", e->file, clk_get_rate(e->clk));
	}
	return 0;
}

static void __exit clkctl_exit(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(entries); i++)
		if (entries[i].clk)
			clk_put(entries[i].clk);
	debugfs_remove_recursive(clkctl_dir);
}

module_init(clkctl_init);
module_exit(clkctl_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("debugfs clock rate control for A733 bring-up testing");
