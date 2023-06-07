#ifndef REVOFS_H
#define REVOFS_H

/* source: https://en.wikipedia.org/wiki/Hexspeak */
#define REVOFS_MAGIC 0x52455245

#define REVOFS_SB_BLOCK_NR 0

#define REVOFS_BLOCK_SIZE (1 << 12) /* 4 KiB */
#define REVOFS_MAX_EXTENTS \
    ((REVOFS_BLOCK_SIZE - sizeof(uint32_t)) / sizeof(struct revofs_extent))
#define REVOFS_MAX_BLOCKS_PER_EXTENT 8 /* It can be ~(uint32) 0 */
#define REVOFS_MAX_FILESIZE                                      \
    ((uint64_t) REVOFS_MAX_BLOCKS_PER_EXTENT *REVOFS_BLOCK_SIZE \
        *REVOFS_MAX_EXTENTS)

#define REVOFS_FILENAME_LEN 255

#define REVOFS_FILES_PER_BLOCK \
    (REVOFS_BLOCK_SIZE / sizeof(struct revofs_file))
#define REVOFS_FILES_PER_EXT \
    (REVOFS_FILES_PER_BLOCK *REVOFS_MAX_BLOCKS_PER_EXTENT)

#define REVOFS_MAX_SUBFILES \
    (REVOFS_FILES_PER_EXT *REVOFS_MAX_EXTENTS)

#include <linux/version.h>

#define USER_NS_REQUIRED() LINUX_VERSION_CODE >= KERNEL_VERSION(5,12,0)



struct revofs_inode {
    uint32_t i_mode;   /* File mode */
    uint32_t i_uid;    /* Owner id */
    uint32_t i_gid;    /* Group id */
    uint32_t i_size;   /* Size in bytes */
    uint32_t i_ctime;  /* Inode change time */
    uint32_t i_atime;  /* Access time */
    uint32_t i_mtime;  /* Modification time */
    uint32_t i_blocks; /* Block count */
    uint32_t i_nlink;  /* Hard links count */
    uint32_t ei_block;  /* Block with list of extents for this file */
    char i_data[32]; /* store symlink content */
};

#define REVOFS_INODES_PER_BLOCK \
    (REVOFS_BLOCK_SIZE / sizeof(struct revofs_inode))

struct revofs_sb_info {
    uint32_t magic; /* Magic number */

    uint32_t nr_blocks; /* Total number of blocks (incl sb & inodes) */
    uint32_t nr_inodes; /* Total number of inodes */

    uint32_t nr_istore_blocks; /* Number of inode store blocks */
    uint32_t nr_ifree_blocks;  /* Number of inode free bitmap blocks */
    uint32_t nr_bfree_blocks;  /* Number of block free bitmap blocks */

    uint32_t nr_free_inodes; /* Number of free inodes */
    uint32_t nr_free_blocks; /* Number of free blocks */

#ifdef __KERNEL__
    unsigned long *ifree_bitmap; /* In-memory free inodes bitmap */
    unsigned long *bfree_bitmap; /* In-memory free blocks bitmap */
#endif
};

#ifdef __KERNEL__

struct revofs_inode_info {
    uint32_t ei_block;  /* Block with list of extents for this file */
    char i_data[32];
    struct inode vfs_inode;
};

struct revofs_extent {
    uint32_t ee_block; /* first logical block extent covers */
    uint32_t ee_len;   /* number of blocks covered by extent */
    uint32_t ee_start; /* first physical block extent covers */
};

struct revofs_file_ei_block {
    uint32_t nr_files; /* Number of files in directory */
    struct revofs_extent extents[REVOFS_MAX_EXTENTS];
};

struct revofs_file {
    uint32_t inode;
    char filename[REVOFS_FILENAME_LEN];
};

struct revofs_dir_block {
    struct revofs_file files[REVOFS_FILES_PER_BLOCK];
};

/* superblock functions */
int revofs_fill_super(struct super_block *sb, void *data, int silent);

/* inode functions */
int revofs_init_inode_cache(void);
void revofs_destroy_inode_cache(void);
struct inode *revofs_iget(struct super_block *sb, unsigned long ino);

/* file functions */
extern const struct file_operations revofs_file_ops;
extern const struct file_operations revofs_dir_ops;
extern const struct address_space_operations revofs_aops;

/* extent functions */
extern uint32_t revofs_ext_search(struct revofs_file_ei_block *index,
                                    uint32_t iblock);

/* Getters for superbock and inode */
#define REVOFS_SB(sb) (sb->s_fs_info)
#define REVOFS_INODE(inode) \
    (container_of(inode, struct revofs_inode_info, vfs_inode))

#endif /* __KERNEL__ */

#endif /* REVOFS_H */
