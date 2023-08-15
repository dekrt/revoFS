/* bitmap.h - Header file for bitmap operations in the revoFS file system.
 * This file contains utility functions for managing in-memory bitmaps.
 */

#ifndef REVOFS_BITMAP_H
#define REVOFS_BITMAP_H

#include <linux/bitmap.h>
#include "revofs.h"

/* 
 * This function finds and returns the first set bit in a given in-memory bitmap.
 * It then clears (sets to 0) the following `len` consecutive bits.
 * It returns 0 if there aren't enough consecutive set bits.
 * The function assumes that the first bit is always set due to the superblock 
 * and the root inode, so 0 is used as an error return value.
 */
static inline uint32_t get_first_free_bits(unsigned long *freemap,
                                           unsigned long size,
                                           uint32_t len)
{
    uint32_t bit, prev = 0, count = 0;
    for_each_set_bit (bit, freemap, size) {
        if (prev != bit - 1)
            count = 0;
        prev = bit;
        if (++count == len) {
            bitmap_clear(freemap, bit - len + 1, len);
            return bit - len + 1;
        }
    }
    return 0;
}

/* 
 * This function returns an unused inode number from the bitmap and marks it as used.
 * It returns 0 if no free inode is found.
 */
static inline uint32_t get_free_inode(struct revofs_sb_info *sbi)
{
    uint32_t ret = get_first_free_bits(sbi->ifree_bitmap, sbi->nr_inodes, 1);
    if (ret)
        sbi->nr_free_inodes--;
    return ret;
}

/* 
 * This function returns the number of the first block in a range of `len` unused blocks 
 * from the bitmap and marks them as used.
 * It returns 0 if not enough free blocks are found.
 */
static inline uint32_t get_free_blocks(struct revofs_sb_info *sbi,
                                       uint32_t len)
{
    uint32_t ret = get_first_free_bits(sbi->bfree_bitmap, sbi->nr_blocks, len);
    if (ret)
        sbi->nr_free_blocks -= len;
    return ret;
}

/* 
 * This function sets the `len` bits starting from the i-th bit in the freemap to 1, marking them as free.
 * It returns 0 on success and -1 if the range is out of bounds.
 */
static inline int put_free_bits(unsigned long *freemap,
                                unsigned long size,
                                uint32_t i,
                                uint32_t len)
{
    if (i + len - 1 > size)
        return -1;

    bitmap_set(freemap, i, len);

    return 0;
}

/* 
 * This function marks a specified inode as unused in the bitmap.
 */
static inline void put_inode(struct revofs_sb_info *sbi, uint32_t ino)
{
    if (put_free_bits(sbi->ifree_bitmap, sbi->nr_inodes, ino, 1))
        return;

    sbi->nr_free_inodes++;
}

/* 
 * This function marks a range of blocks as unused in the bitmap.
 */
static inline void put_blocks(struct revofs_sb_info *sbi,
                              uint32_t bno,
                              uint32_t len)
{
    if (put_free_bits(sbi->bfree_bitmap, sbi->nr_blocks, bno, len))
        return;

    sbi->nr_free_blocks += len;
}

#endif /* REVOFS_BITMAP_H */
