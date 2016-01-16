#include <linux/rfkill.h>
#include <linux/module.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Johannes Berg <johannes@sipsolutions.net>");

static struct rfkill *rfk;

static void test_poll(struct rfkill *rfkill, void *data)
{
	printk(KERN_DEBUG "poll test rfkill\n");
}

static void test_query(struct rfkill *rfkill, void *data)
{
	printk(KERN_DEBUG "query test rfkill\n");
}

static int test_set_block(void *data, bool blocked)
{
	printk(KERN_DEBUG "set test rfkill (%s)\n",
		blocked ? "blocked" : "active");
	return 0;
}

static struct rfkill_ops ops = {
	.poll = test_poll,
	.query = test_query,
	.set_block = test_set_block,
};

int mod_init(void)
{
	int err;

	rfk = rfkill_alloc("fake", NULL, RFKILL_TYPE_WLAN, &ops, NULL);
	if (!rfk)
		return -ENOMEM;
	err = rfkill_register(rfk);
	if (err)
		rfkill_destroy(rfk);
	return err;
}
module_init(mod_init);

void mod_exit(void)
{
	rfkill_unregister(rfk);
	rfkill_destroy(rfk);
}
module_exit(mod_exit);
