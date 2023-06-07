#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <net/sock.h>
#include <linux/netlink.h>

#include "revofs.h"



/* message type*/
#define NETLINK_MSG_FOR_SCHIPS    30 
// no more than 32

// The port number
#define USER_PORT        123
// message length limit
#define MSG_LEN         125
#define MAX_PLOAD       MSG_LEN

struct sock *nlsk = NULL;
extern struct net init_net;

int send_usrmsg(char *pbuf, uint16_t len);
static void netlink_rcv_msg(struct sk_buff *skb);

struct netlink_kernel_cfg cfg = { 
        .input  = netlink_rcv_msg, 
        /* set recv callback */
}; 

int send_usrmsg(char *pbuf, uint16_t len)
{
    struct sk_buff *nl_skb;
    struct nlmsghdr *nlh;
    int ret_nl;

  
    /* Create sk_buff space */
    nl_skb = nlmsg_new(len, GFP_ATOMIC);
    if(!nl_skb)
    {
        printk("netlink alloc failure\n");
        return -1;
    }

    /* Set netlink message header */
    nlh = nlmsg_put(nl_skb, 0, 0, NETLINK_MSG_FOR_SCHIPS, len, 0);
    if(nlh == NULL)
    {
        printk("nlmsg_put failaure \n");
        nlmsg_free(nl_skb);
        return -1;
    }

/*  /*copy data to send */
    memcpy(nlmsg_data(nlh), pbuf, len);
    ret_nl= netlink_unicast(nlsk, nl_skb, USER_PORT, MSG_DONTWAIT);
   
    return ret_nl;
}

static void netlink_rcv_msg(struct sk_buff *skb)
{
    struct nlmsghdr *nlh = NULL;
    char *umsg = NULL;
    char *kmsg = "hello RevoFS";

    if(skb->len >= nlmsg_total_size(0))
    {
        nlh = nlmsg_hdr(skb);
        umsg = NLMSG_DATA(nlh);
        if(umsg)
        {
            printk("kernel recv from user: %s\n", umsg);
            send_usrmsg(kmsg, strlen(kmsg));
        }
    }
}


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
     int ret;
    nlsk = (struct sock *)netlink_kernel_create(&init_net, NETLINK_MSG_FOR_SCHIPS, &cfg);
    if(nlsk == NULL)
    {   
        printk("netlink_kernel_create error !\n");
        return -1; 
    }   
    printk("test_netlink_init\n");

    ret=revofs_init_inode_cache();
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
    int ret;
    if (nlsk){
        netlink_kernel_release(nlsk); /* release ..*/
        nlsk = NULL;
    }   
    printk("test_netlink_exit!\n");

    
    ret= unregister_filesystem(&revofs_file_system_type);
    if (ret)
        pr_err("unregister_filesystem() failed\n");

    revofs_destroy_inode_cache();

    pr_info("module unloaded\n");
}

module_init(revofs_init);
module_exit(revofs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Storage Revolution Team");
MODULE_DESCRIPTION("a simple file system by Storage Revolution Team");
