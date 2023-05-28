#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "revofs.h"

/* Mount a revofs partition */
struct dentry *revofs_mount(struct file_system_type *fs_type,
                              int flags,
                              const char *dev_name,
                              void *data)
{
    struct dentry *dentry =
        mount_bdev(fs_type, flags, dev_name, data, revofs_fill_super);
    if (IS_ERR(dentry))
        pr_err("'%s' mount failure\n", dev_name);
    else
        pr_info("'%s' mount success\n", dev_name);

    return dentry;
}

/* Unmount a revofs partition */
void revofs_kill_sb(struct super_block *sb)
{
    kill_block_super(sb);

    pr_info("unmounted disk\n");
}

static struct file_system_type revofs_file_system_type = {
    .owner = THIS_MODULE,
    .name = "revofs",
    .mount = revofs_mount,
    .kill_sb = revofs_kill_sb,
    .fs_flags = FS_REQUIRES_DEV,
    .next = NULL,
};

static int __init revofs_init(void)
{
    int ret = revofs_init_inode_cache();
    if (ret) {
        pr_err("inode cache creation failed\n");
        goto end;
    }

    ret = register_filesystem(&revofs_file_system_type);
    if (ret) {
        pr_err("register_filesystem() failed\n");
        goto end;
    }

    pr_info("module loaded\n");
end:
    return ret;
}

static void __exit revofs_exit(void)
{
    int ret = unregister_filesystem(&revofs_file_system_type);
    if (ret)
        pr_err("unregister_filesystem() failed\n");

    revofs_destroy_inode_cache();

    pr_info("module unloaded\n");
}

module_init(revofs_init);
module_exit(revofs_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("a revo file system");
