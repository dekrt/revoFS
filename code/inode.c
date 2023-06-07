#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "bitmap.h"
#include "revofs.h"

static const struct inode_operations revofs_inode_ops;
static const struct inode_operations symlink_inode_ops;

/* Get inode ino from disk */
struct inode *revofs_iget(struct super_block *sb, unsigned long ino)
{
    struct inode *inode = NULL;
    struct revofs_inode *cinode = NULL;
    struct revofs_inode_info *ci = NULL;
    struct revofs_sb_info *sbi = REVOFS_SB(sb);
    struct buffer_head *bh = NULL;
    uint32_t inode_block = (ino / REVOFS_INODES_PER_BLOCK) + 1;
    uint32_t inode_shift = ino % REVOFS_INODES_PER_BLOCK;
    int ret;

    /* Fail if ino is out of range */
    if (ino >= sbi->nr_inodes)
        return ERR_PTR(-EINVAL);

    /* Get a locked inode from Linux */
    inode = iget_locked(sb, ino);
    if (!inode)
        return ERR_PTR(-ENOMEM);

    /* If inode is in cache, return it */
    if (!(inode->i_state & I_NEW))
        return inode;

    ci = REVOFS_INODE(inode);
    /* Read inode from disk and initialize */
    bh = sb_bread(sb, inode_block);
    if (!bh) {
        ret = -EIO;
        goto failed;
    }
    cinode = (struct revofs_inode *) bh->b_data;
    cinode += inode_shift;

    inode->i_ino = ino;
    inode->i_sb = sb;
    inode->i_op = &revofs_inode_ops;

    inode->i_mode = le32_to_cpu(cinode->i_mode);
    i_uid_write(inode, le32_to_cpu(cinode->i_uid));
    i_gid_write(inode, le32_to_cpu(cinode->i_gid));
    inode->i_size = le32_to_cpu(cinode->i_size);
    inode->i_ctime.tv_sec = (time64_t) le32_to_cpu(cinode->i_ctime);
    inode->i_ctime.tv_nsec = 0;
    inode->i_atime.tv_sec = (time64_t) le32_to_cpu(cinode->i_atime);
    inode->i_atime.tv_nsec = 0;
    inode->i_mtime.tv_sec = (time64_t) le32_to_cpu(cinode->i_mtime);
    inode->i_mtime.tv_nsec = 0;
    inode->i_blocks = le32_to_cpu(cinode->i_blocks);
    set_nlink(inode, le32_to_cpu(cinode->i_nlink));

    if (S_ISDIR(inode->i_mode)) {
        ci->ei_block = le32_to_cpu(cinode->ei_block);
        inode->i_fop = &revofs_dir_ops;
    } else if (S_ISREG(inode->i_mode)) {
        ci->ei_block = le32_to_cpu(cinode->ei_block);
        inode->i_fop = &revofs_file_ops;
        inode->i_mapping->a_ops = &revofs_aops;
    } else if (S_ISLNK(inode->i_mode)) {
        strncpy(ci->i_data, cinode->i_data, sizeof(ci->i_data));
        inode->i_link = ci->i_data;
        inode->i_op = &symlink_inode_ops;
    }

    brelse(bh);

    /* Unlock the inode to make it usable */
    unlock_new_inode(inode);

    return inode;

failed:
    brelse(bh);
    iget_failed(inode);
    return ERR_PTR(ret);
}

/*
 * Look for dentry in dir.
 * Fill dentry with NULL if not in dir, with the corresponding inode if found.
 * Returns NULL on success.
 */
static struct dentry *revofs_lookup(struct inode *dir,
                                    struct dentry *dentry,
                                    unsigned int flags)
{
    struct super_block *sb = dir->i_sb;
    struct revofs_inode_info *ci_dir = REVOFS_INODE(dir);
    struct inode *inode = NULL;
    struct buffer_head *bh = NULL, *bh2 = NULL;
    struct revofs_file_ei_block *eblock = NULL;
    struct revofs_dir_block *dblock = NULL;
    struct revofs_file *f = NULL;
    int ei, bi, fi;

    /* Check filename length */
    if (dentry->d_name.len > REVOFS_FILENAME_LEN)
        return ERR_PTR(-ENAMETOOLONG);

    /* Read the directory block on disk */
    bh = sb_bread(sb, ci_dir->ei_block);
    if (!bh)
        return ERR_PTR(-EIO);
    eblock = (struct revofs_file_ei_block *) bh->b_data;

    /* Search for the file in directory */
    for (ei = 0; ei < REVOFS_MAX_EXTENTS; ei++) {
        if (!eblock->extents[ei].ee_start)
            break;

        /* Iterate blocks in extent */
        for (bi = 0; bi < eblock->extents[ei].ee_len; bi++) {
            bh2 = sb_bread(sb, eblock->extents[ei].ee_start + bi);
            if (!bh2)
                return ERR_PTR(-EIO);
            dblock = (struct revofs_dir_block *) bh2->b_data;
            /* Search file in ei_block */
            for (fi = 0; fi < REVOFS_FILES_PER_BLOCK; fi++) {
                f = &dblock->files[fi];
                if (!f->inode) {
                    brelse(bh2);
                    goto search_end;
                }
                if (!strncmp(f->filename, dentry->d_name.name,
                             REVOFS_FILENAME_LEN)) {
                    inode = revofs_iget(sb, f->inode);
                    brelse(bh2);
                    goto search_end;
                }
            }
            brelse(bh2);
            bh2 = NULL;
        }
    }

search_end:
    brelse(bh);

    /* Update directory access time */
    dir->i_atime = current_time(dir);
    mark_inode_dirty(dir);

    /* Fill the dentry with the inode */
    d_add(dentry, inode);

    return NULL;
}

/* Create a new inode in dir */
static struct inode *revofs_new_inode(struct inode *dir, mode_t mode)
{
    struct inode *inode;
    struct revofs_inode_info *ci;
    struct super_block *sb;
    struct revofs_sb_info *sbi;
    uint32_t ino, bno;
    int ret;

    /* Check mode before doing anything to avoid undoing everything */
    if (!S_ISDIR(mode) && !S_ISREG(mode) && !S_ISLNK(mode)) {
        pr_err(
            "File type not supported (only directory, regular file and symlink "
            "supported)\n");
        return ERR_PTR(-EINVAL);
    }

    /* Check if inodes are available */
    sb = dir->i_sb;
    sbi = REVOFS_SB(sb);
    if (sbi->nr_free_inodes == 0 || sbi->nr_free_blocks == 0)
        return ERR_PTR(-ENOSPC);

    /* Get a new free inode */
    ino = get_free_inode(sbi);
    if (!ino)
        return ERR_PTR(-ENOSPC);

    inode = revofs_iget(sb, ino);
    if (IS_ERR(inode)) {
        ret = PTR_ERR(inode);
        goto put_ino;
    }

    if (S_ISLNK(mode)) {
#if USER_NS_REQUIRED()
        inode_init_owner(&init_user_ns, inode, dir, mode);
#else
        inode_init_owner(inode, dir, mode);
#endif
        set_nlink(inode, 1);
        inode->i_ctime = inode->i_atime = inode->i_mtime = current_time(inode);
        inode->i_op = &symlink_inode_ops;
        return inode;
    }

    ci = REVOFS_INODE(inode);

    /* Get a free block for this new inode's index */
    bno = get_free_blocks(sbi, 1);
    if (!bno) {
        ret = -ENOSPC;
        goto put_inode;
    }

    /* Initialize inode */
#if USER_NS_REQUIRED()
    inode_init_owner(&init_user_ns, inode, dir, mode);
#else
    inode_init_owner(inode, dir, mode);
#endif
    inode->i_blocks = 1;
    if (S_ISDIR(mode)) {
        ci->ei_block = bno;
        inode->i_size = REVOFS_BLOCK_SIZE;
        inode->i_fop = &revofs_dir_ops;
        set_nlink(inode, 2); /* . and .. */
    } else if (S_ISREG(mode)) {
        ci->ei_block = bno;
        inode->i_size = 0;
        inode->i_fop = &revofs_file_ops;
        inode->i_mapping->a_ops = &revofs_aops;
        set_nlink(inode, 1);
    }

    inode->i_ctime = inode->i_atime = inode->i_mtime = current_time(inode);

    return inode;

put_inode:
    iput(inode);
put_ino:
    put_inode(sbi, ino);

    return ERR_PTR(ret);
}

/*
 * Create a file or directory in this way:
 *   - check filename length and if the parent directory is not full
 *   - create the new inode (allocate inode and blocks)
 *   - cleanup index block of the new inode
 *   - add new file/directory in parent index
 */
#if USER_NS_REQUIRED()
static int revofs_create(struct user_namespace *ns,
                         struct inode *dir,
                         struct dentry *dentry,
                         umode_t mode,
                         bool excl)
#else
static int revofs_create(struct inode *dir,
                         struct dentry *dentry,
                         umode_t mode,
                         bool excl)
#endif
{
    struct super_block *sb;
    struct inode *inode;
    struct revofs_inode_info *ci_dir;
    struct revofs_file_ei_block *eblock;
    struct revofs_dir_block *dblock;
    char *fblock;
    struct buffer_head *bh, *bh2;
    int ret = 0, alloc = false, bno = 0;
    int ei = 0, bi = 0, fi = 0;

    /* permission_check */
    if (!uid_eq(current_fsuid(), dir->i_uid)) {
        /* current user is not the file owner, deny the permission */
        pr_err("Permission denied\n");
        return -1;
    }

    /* Check filename length */
    if (strlen(dentry->d_name.name) > REVOFS_FILENAME_LEN)
        return -ENAMETOOLONG;

    /* Read parent directory index */
    ci_dir = REVOFS_INODE(dir);
    sb = dir->i_sb;
    bh = sb_bread(sb, ci_dir->ei_block);
    if (!bh)
        return -EIO;

    eblock = (struct revofs_file_ei_block *) bh->b_data;
    /* Check if parent directory is full */
    if (eblock->nr_files == REVOFS_MAX_SUBFILES) {
        ret = -EMLINK;
        goto end;
    }

    /* Get a new free inode */
    inode = revofs_new_inode(dir, mode);
    if (IS_ERR(inode)) {
        ret = PTR_ERR(inode);
        goto end;
    }

    /*
     * Scrub ei_block for new file/directory to avoid previous data
     * messing with new file/directory.
     */
    bh2 = sb_bread(sb, REVOFS_INODE(inode)->ei_block);
    if (!bh2) {
        ret = -EIO;
        goto iput;
    }
    fblock = (char *) bh2->b_data;
    memset(fblock, 0, REVOFS_BLOCK_SIZE);
    mark_buffer_dirty(bh2);
    brelse(bh2);

    /* Find first free slot in parent index and register new inode */
    ei = eblock->nr_files / REVOFS_FILES_PER_EXT;
    bi = eblock->nr_files % REVOFS_FILES_PER_EXT / REVOFS_FILES_PER_BLOCK;
    fi = eblock->nr_files % REVOFS_FILES_PER_BLOCK;

    if (!eblock->extents[ei].ee_start) {
        bno = get_free_blocks(REVOFS_SB(sb), 8);
        if (!bno) {
            ret = -ENOSPC;
            goto iput;
        }
        eblock->extents[ei].ee_start = bno;
        eblock->extents[ei].ee_len = 8;
        eblock->extents[ei].ee_block = ei ? eblock->extents[ei - 1].ee_block +
                                                eblock->extents[ei - 1].ee_len
                                          : 0;
        alloc = true;
    }
    bh2 = sb_bread(sb, eblock->extents[ei].ee_start + bi);
    if (!bh2) {
        ret = -EIO;
        goto put_block;
    }
    dblock = (struct revofs_dir_block *) bh2->b_data;

    dblock->files[fi].inode = inode->i_ino;
    strncpy(dblock->files[fi].filename, dentry->d_name.name,
            REVOFS_FILENAME_LEN);

    eblock->nr_files++;
    mark_buffer_dirty(bh2);
    mark_buffer_dirty(bh);
    brelse(bh2);
    brelse(bh);

    /* Update stats and mark dir and new inode dirty */
    mark_inode_dirty(inode);
    dir->i_mtime = dir->i_atime = dir->i_ctime = current_time(dir);
    if (S_ISDIR(mode))
        inc_nlink(dir);
    mark_inode_dirty(dir);

    /* setup dentry */
    d_instantiate(dentry, inode);

    return 0;

put_block:
    if (alloc && eblock->extents[ei].ee_start) {
        put_blocks(REVOFS_SB(sb), eblock->extents[ei].ee_start,
                   eblock->extents[ei].ee_len);
        memset(&eblock->extents[ei], 0, sizeof(struct revofs_extent));
    }
iput:
    put_blocks(REVOFS_SB(sb), REVOFS_INODE(inode)->ei_block, 1);
    put_inode(REVOFS_SB(sb), inode->i_ino);
    iput(inode);
end:
    brelse(bh);
    return ret;
}

static int revofs_remove_from_dir(struct inode *dir, struct dentry *dentry)
{
    struct super_block *sb = dir->i_sb;
    struct inode *inode = d_inode(dentry);
    struct buffer_head *bh = NULL, *bh2 = NULL, *bh_prev = NULL;
    struct revofs_file_ei_block *eblock = NULL;
    struct revofs_dir_block *dblock = NULL, *dblock_prev = NULL;
    int ei = 0, bi = 0, fi = 0;
    int ret = 0, found = false;

    /* Read parent directory index */
    bh = sb_bread(sb, REVOFS_INODE(dir)->ei_block);
    if (!bh)
        return -EIO;
    eblock = (struct revofs_file_ei_block *) bh->b_data;
    for (ei = 0; ei < REVOFS_MAX_EXTENTS; ei++) {
        if (!eblock->extents[ei].ee_start)
            break;

        for (bi = 0; bi < eblock->extents[ei].ee_len; bi++) {
            bh2 = sb_bread(sb, eblock->extents[ei].ee_start + bi);
            if (!bh2) {
                ret = -EIO;
                goto release_bh;
            }
            dblock = (struct revofs_dir_block *) bh2->b_data;
            if (!dblock->files[0].inode)
                break;

            if (found) {
                memmove(dblock_prev->files + REVOFS_FILES_PER_BLOCK - 1,
                        dblock->files, sizeof(struct revofs_file));
                brelse(bh_prev);

                memmove(
                    dblock->files, dblock->files + 1,
                    (REVOFS_FILES_PER_BLOCK - 1) * sizeof(struct revofs_file));
                memset(dblock->files + REVOFS_FILES_PER_BLOCK - 1, 0,
                       sizeof(struct revofs_file));
                mark_buffer_dirty(bh2);

                bh_prev = bh2;
                dblock_prev = dblock;
                continue;
            }
            /* Remove file from parent directory */
            for (fi = 0; fi < REVOFS_FILES_PER_BLOCK; fi++) {
                if (dblock->files[fi].inode == inode->i_ino) {
                    found = true;
                    if (fi != REVOFS_FILES_PER_BLOCK - 1) {
                        memmove(dblock->files + fi, dblock->files + fi + 1,
                                (REVOFS_FILES_PER_BLOCK - fi - 1) *
                                    sizeof(struct revofs_file));
                    }
                    memset(dblock->files + REVOFS_FILES_PER_BLOCK - 1, 0,
                           sizeof(struct revofs_file));
                    mark_buffer_dirty(bh2);
                    bh_prev = bh2;
                    dblock_prev = dblock;
                    break;
                }
            }
            if (!found)
                brelse(bh2);
        }
    }
    if (found) {
        if (bh_prev) {
            brelse(bh_prev);
        }
        eblock->nr_files--;
        mark_buffer_dirty(bh);
    }
release_bh:
    brelse(bh);
    return ret;
}
/*
 * Remove a link for a file including the reference in the parent directory.
 * If link count is 0, destroy file in this way:
 *   - remove the file from its parent directory.
 *   - cleanup blocks containing data
 *   - cleanup file index block
 *   - cleanup inode
 */
static int revofs_unlink(struct inode *dir, struct dentry *dentry)
{
    struct super_block *sb = dir->i_sb;
    struct revofs_sb_info *sbi = REVOFS_SB(sb);
    struct inode *inode = d_inode(dentry);
    struct buffer_head *bh = NULL, *bh2 = NULL;
    struct revofs_file_ei_block *file_block = NULL;
    int ei = 0, bi = 0;
    int ret = 0;

    uint32_t ino = inode->i_ino;
    uint32_t bno = 0;
    /* permission_check */
    if (!uid_eq(current_fsuid(), dir->i_uid)) {
        /* current user is not the file owner, deny the permission */
        pr_err("Permission denied\n");
        return -1;
    }
    ret = revofs_remove_from_dir(dir, dentry);
    if (ret != 0)
        return ret;

    if (S_ISLNK(inode->i_mode))
        goto clean_inode;

    /* Update inode stats */
    dir->i_mtime = dir->i_atime = dir->i_ctime = current_time(dir);
    if (S_ISDIR(inode->i_mode)) {
        drop_nlink(dir);
        drop_nlink(inode);
    }
    mark_inode_dirty(dir);

    if (inode->i_nlink > 1) {
        inode_dec_link_count(inode);
        return ret;
    }

    /*
     * Cleanup pointed blocks if unlinking a file. If we fail to read the
     * index block, cleanup inode anyway and lose this file's blocks
     * forever. If we fail to scrub a data block, don't fail (too late
     * anyway), just put the block and continue.
     */
    bno = REVOFS_INODE(inode)->ei_block;
    bh = sb_bread(sb, bno);
    if (!bh)
        goto clean_inode;
    file_block = (struct revofs_file_ei_block *) bh->b_data;
    if (S_ISDIR(inode->i_mode))
        goto scrub;
    for (ei = 0; ei < REVOFS_MAX_EXTENTS; ei++) {
        char *block;

        if (!file_block->extents[ei].ee_start)
            break;

        put_blocks(sbi, file_block->extents[ei].ee_start,
                   file_block->extents[ei].ee_len);

        /* Scrub the extent */
        for (bi = 0; bi < file_block->extents[ei].ee_len; bi++) {
            bh2 = sb_bread(sb, file_block->extents[ei].ee_start + bi);
            if (!bh2)
                continue;
            block = (char *) bh2->b_data;
            memset(block, 0, REVOFS_BLOCK_SIZE);
            mark_buffer_dirty(bh2);
            brelse(bh2);
        }
    }

scrub:
    /* Scrub index block */
    memset(file_block, 0, REVOFS_BLOCK_SIZE);
    mark_buffer_dirty(bh);
    brelse(bh);

clean_inode:
    /* Cleanup inode and mark dirty */
    inode->i_blocks = 0;
    REVOFS_INODE(inode)->ei_block = 0;
    inode->i_size = 0;
    i_uid_write(inode, 0);
    i_gid_write(inode, 0);
    inode->i_mode = 0;
    inode->i_ctime.tv_sec = inode->i_mtime.tv_sec = inode->i_atime.tv_sec = 0;
    drop_nlink(inode);
    mark_inode_dirty(inode);

    /* Free inode and index block from bitmap */
    put_blocks(sbi, bno, 1);
    put_inode(sbi, ino);

    return ret;
}

#if USER_NS_REQUIRED()
static int revofs_rename(struct user_namespace *ns,
                         struct inode *old_dir,
                         struct dentry *old_dentry,
                         struct inode *new_dir,
                         struct dentry *new_dentry,
                         unsigned int flags)
#else
static int revofs_rename(struct inode *old_dir,
                         struct dentry *old_dentry,
                         struct inode *new_dir,
                         struct dentry *new_dentry,
                         unsigned int flags)
#endif
{
    struct super_block *sb = old_dir->i_sb;
    struct revofs_inode_info *ci_new = REVOFS_INODE(new_dir);
    struct inode *src = d_inode(old_dentry);
    struct buffer_head *bh_new = NULL, *bh2 = NULL;
    struct revofs_file_ei_block *eblock_new = NULL;
    struct revofs_dir_block *dblock = NULL;
    int new_pos = -1, ret = 0;
    int ei = 0, bi = 0, fi = 0, bno = 0;
    /* fail with these unsupported flags */
    if (flags & (RENAME_EXCHANGE | RENAME_WHITEOUT))
        return -EINVAL;

    /* Check if filename is not too long */
    if (strlen(new_dentry->d_name.name) > REVOFS_FILENAME_LEN)
        return -ENAMETOOLONG;

    /* Fail if new_dentry exists or if new_dir is full */
    bh_new = sb_bread(sb, ci_new->ei_block);
    if (!bh_new)
        return -EIO;

    eblock_new = (struct revofs_file_ei_block *) bh_new->b_data;
    for (ei = 0; new_pos < 0 && ei < REVOFS_MAX_EXTENTS; ei++) {
        if (!eblock_new->extents[ei].ee_start)
            break;

        for (bi = 0; new_pos < 0 && bi < eblock_new->extents[ei].ee_len; bi++) {
            bh2 = sb_bread(sb, eblock_new->extents[ei].ee_start + bi);
            if (!bh2) {
                ret = -EIO;
                goto release_new;
            }

            dblock = (struct revofs_dir_block *) bh2->b_data;
            for (fi = 0; fi < REVOFS_FILES_PER_BLOCK; fi++) {
                if (new_dir == old_dir) {
                    if (!strncmp(dblock->files[fi].filename,
                                 old_dentry->d_name.name,
                                 REVOFS_FILENAME_LEN)) {
                        strncpy(dblock->files[fi].filename,
                                new_dentry->d_name.name, REVOFS_FILENAME_LEN);
                        mark_buffer_dirty(bh2);
                        brelse(bh2);
                        goto release_new;
                    }
                }
                if (!strncmp(dblock->files[fi].filename,
                             new_dentry->d_name.name, REVOFS_FILENAME_LEN)) {
                    brelse(bh2);
                    ret = -EEXIST;
                    goto release_new;
                }
                if (new_pos < 0 && !dblock->files[fi].inode) {
                    new_pos = fi;
                    break;
                }
            }
            if (new_pos < 0)
                brelse(bh2);
        }
    }

    /* If new directory is full, fail */
    if (new_pos < 0 && eblock_new->nr_files == REVOFS_FILES_PER_EXT) {
        ret = -EMLINK;
        goto release_new;
    }

    /* insert in new parent directory */
    /* Get new freeblocks for extent if needed*/
    if (new_pos < 0) {
        bno = get_free_blocks(REVOFS_SB(sb), 8);
        if (!bno) {
            ret = -ENOSPC;
            goto release_new;
        }
        eblock_new->extents[ei].ee_start = bno;
        eblock_new->extents[ei].ee_len = 8;
        eblock_new->extents[ei].ee_block =
            ei ? eblock_new->extents[ei - 1].ee_block +
                     eblock_new->extents[ei - 1].ee_len
               : 0;
        bh2 = sb_bread(sb, eblock_new->extents[ei].ee_start + 0);
        if (!bh2) {
            ret = -EIO;
            goto put_block;
        }
        dblock = (struct revofs_dir_block *) bh2->b_data;
        mark_buffer_dirty(bh_new);
        new_pos = 0;
    }
    dblock->files[new_pos].inode = src->i_ino;
    strncpy(dblock->files[new_pos].filename, new_dentry->d_name.name,
            REVOFS_FILENAME_LEN);
    mark_buffer_dirty(bh2);
    brelse(bh2);

    /* Update new parent inode metadata */
    new_dir->i_atime = new_dir->i_ctime = new_dir->i_mtime =
        current_time(new_dir);
    if (S_ISDIR(src->i_mode))
        inc_nlink(new_dir);
    mark_inode_dirty(new_dir);

    /* remove target from old parent directory */
    ret = revofs_remove_from_dir(old_dir, old_dentry);
    if (ret != 0)
        goto release_new;

    /* Update old parent inode metadata */
    old_dir->i_atime = old_dir->i_ctime = old_dir->i_mtime =
        current_time(old_dir);
    if (S_ISDIR(src->i_mode))
        drop_nlink(old_dir);
    mark_inode_dirty(old_dir);

    return ret;

put_block:
    if (eblock_new->extents[ei].ee_start) {
        put_blocks(REVOFS_SB(sb), eblock_new->extents[ei].ee_start,
                   eblock_new->extents[ei].ee_len);
        memset(&eblock_new->extents[ei], 0, sizeof(struct revofs_extent));
    }
release_new:
    brelse(bh_new);
    return ret;
}

#if USER_NS_REQUIRED()
static int revofs_mkdir(struct user_namespace *ns,
                        struct inode *dir,
                        struct dentry *dentry,
                        umode_t mode)
{
    return revofs_create(ns, dir, dentry, mode | S_IFDIR, 0);
}
#else
static int revofs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
    /* permission_check */
    if (!uid_eq(current_fsuid(), dir->i_uid)) {
        /* current user is not the file owner, deny the permission */
        pr_err("Permission denied\n");
        return -1;
    }
    return revofs_create(dir, dentry, mode | S_IFDIR, 0);
}
#endif

static int revofs_rmdir(struct inode *dir, struct dentry *dentry)
{
    struct super_block *sb = dir->i_sb;
    struct inode *inode = d_inode(dentry);
    struct buffer_head *bh;
    struct revofs_file_ei_block *eblock;
    /* permission_check */
    if (!uid_eq(current_fsuid(), dir->i_uid)) {
        /* current user is not the file owner, deny the permission */
        pr_err("Permission denied\n");
        return -1;
    }
    /* If the directory is not empty, fail */
    if (inode->i_nlink > 2)
        return -ENOTEMPTY;
    bh = sb_bread(sb, REVOFS_INODE(inode)->ei_block);
    if (!bh)
        return -EIO;
    eblock = (struct revofs_file_ei_block *) bh->b_data;
    if (eblock->nr_files != 0) {
        brelse(bh);
        return -ENOTEMPTY;
    }
    brelse(bh);

    /* Remove directory with unlink */
    return revofs_unlink(dir, dentry);
}

static int revofs_link(struct dentry *old_dentry,
                       struct inode *dir,
                       struct dentry *dentry)
{
    struct inode *inode = d_inode(old_dentry);
    struct super_block *sb = inode->i_sb;
    struct revofs_inode_info *ci_dir = REVOFS_INODE(dir);
    struct revofs_file_ei_block *eblock = NULL;
    struct revofs_dir_block *dblock;
    struct buffer_head *bh = NULL, *bh2 = NULL;
    int ret = 0, alloc = false, bno = 0;
    int ei = 0, bi = 0, fi = 0;
    /* permission_check */
    if (!uid_eq(current_fsuid(), dir->i_uid)) {
        /* current user is not the file owner, deny the permission */
        pr_err("Permission denied\n");
        return -1;
    }
    bh = sb_bread(sb, ci_dir->ei_block);
    if (!bh)
        return -EIO;
    eblock = (struct revofs_file_ei_block *) bh->b_data;

    if (eblock->nr_files == REVOFS_MAX_SUBFILES) {
        ret = -EMLINK;
        printk(KERN_INFO "directory is full");
        goto end;
    }

    ei = eblock->nr_files / REVOFS_FILES_PER_EXT;
    bi = eblock->nr_files % REVOFS_FILES_PER_EXT / REVOFS_FILES_PER_BLOCK;
    fi = eblock->nr_files % REVOFS_FILES_PER_BLOCK;

    if (eblock->extents[ei].ee_start == 0) {
        bno = get_free_blocks(REVOFS_SB(sb), 8);
        if (!bno) {
            ret = -ENOSPC;
            goto end;
        }
        eblock->extents[ei].ee_start = bno;
        eblock->extents[ei].ee_len = 8;
        eblock->extents[ei].ee_block = ei ? eblock->extents[ei - 1].ee_block +
                                                eblock->extents[ei - 1].ee_len
                                          : 0;
        alloc = true;
    }
    bh2 = sb_bread(sb, eblock->extents[ei].ee_start + bi);
    if (!bh2) {
        ret = -EIO;
        goto put_block;
    }
    dblock = (struct revofs_dir_block *) bh2->b_data;

    dblock->files[fi].inode = inode->i_ino;
    strncpy(dblock->files[fi].filename, dentry->d_name.name,
            REVOFS_FILENAME_LEN);

    eblock->nr_files++;
    mark_buffer_dirty(bh2);
    mark_buffer_dirty(bh);
    brelse(bh2);
    brelse(bh);

    inode_inc_link_count(inode);
    d_instantiate(dentry, inode);
    return ret;

put_block:
    if (alloc && eblock->extents[ei].ee_start) {
        put_blocks(REVOFS_SB(sb), eblock->extents[ei].ee_start,
                   eblock->extents[ei].ee_len);
        memset(&eblock->extents[ei], 0, sizeof(struct revofs_extent));
    }
end:
    brelse(bh);
    return ret;
}

#if USER_NS_REQUIRED()
static int revofs_symlink(struct user_namespace *ns,
                          struct inode *dir,
                          struct dentry *dentry,
                          const char *symname)
#else
static int revofs_symlink(struct inode *dir,
                          struct dentry *dentry,
                          const char *symname)
#endif
{
    struct super_block *sb = dir->i_sb;
    unsigned int l = strlen(symname) + 1;
    struct inode *inode = revofs_new_inode(dir, S_IFLNK | S_IRWXUGO);
    struct revofs_inode_info *ci = REVOFS_INODE(inode);
    struct revofs_inode_info *ci_dir = REVOFS_INODE(dir);
    struct revofs_file_ei_block *eblock = NULL;
    struct revofs_dir_block *dblock = NULL;
    struct buffer_head *bh = NULL, *bh2 = NULL;
    int ret = 0, alloc = false, bno = 0;
    int ei = 0, bi = 0, fi = 0;

    /* Check if symlink content is not too long */
    if (l > sizeof(ci->i_data))
        return -ENAMETOOLONG;

    /* fill directory data block */
    bh = sb_bread(sb, ci_dir->ei_block);
    if (!bh)
        return -EIO;
    eblock = (struct revofs_file_ei_block *) bh->b_data;

    if (eblock->nr_files == REVOFS_MAX_SUBFILES) {
        ret = -EMLINK;
        printk(KERN_INFO "directory is full");
        goto end;
    }

    ei = eblock->nr_files / REVOFS_FILES_PER_EXT;
    bi = eblock->nr_files % REVOFS_FILES_PER_EXT / REVOFS_FILES_PER_BLOCK;
    fi = eblock->nr_files % REVOFS_FILES_PER_BLOCK;

    if (eblock->extents[ei].ee_start == 0) {
        bno = get_free_blocks(REVOFS_SB(sb), 8);
        if (!bno) {
            ret = -ENOSPC;
            goto end;
        }
        eblock->extents[ei].ee_start = bno;
        eblock->extents[ei].ee_len = 8;
        eblock->extents[ei].ee_block = ei ? eblock->extents[ei - 1].ee_block +
                                                eblock->extents[ei - 1].ee_len
                                          : 0;
        alloc = true;
    }
    bh2 = sb_bread(sb, eblock->extents[ei].ee_start + bi);
    if (!bh2) {
        ret = -EIO;
        goto put_block;
    }
    dblock = (struct revofs_dir_block *) bh2->b_data;

    dblock->files[fi].inode = inode->i_ino;
    strncpy(dblock->files[fi].filename, dentry->d_name.name,
            REVOFS_FILENAME_LEN);

    eblock->nr_files++;
    mark_buffer_dirty(bh2);
    mark_buffer_dirty(bh);
    brelse(bh2);
    brelse(bh);

    inode->i_link = (char *) ci->i_data;
    memcpy(inode->i_link, symname, l);
    inode->i_size = l - 1;
    mark_inode_dirty(inode);
    d_instantiate(dentry, inode);
    return 0;

put_block:
    if (alloc && eblock->extents[ei].ee_start) {
        put_blocks(REVOFS_SB(sb), eblock->extents[ei].ee_start,
                   eblock->extents[ei].ee_len);
        memset(&eblock->extents[ei], 0, sizeof(struct revofs_extent));
    }

end:
    brelse(bh);
    return ret;
}

static const char *revofs_get_link(struct dentry *dentry,
                                   struct inode *inode,
                                   struct delayed_call *done)
{
    return inode->i_link;
}

static const struct inode_operations revofs_inode_ops = {
    .lookup = revofs_lookup,
    .create = revofs_create,
    .unlink = revofs_unlink,
    .mkdir = revofs_mkdir,
    .rmdir = revofs_rmdir,
    .rename = revofs_rename,
    .link = revofs_link,
    .symlink = revofs_symlink,
};

static const struct inode_operations symlink_inode_ops = {
    .get_link = revofs_get_link,
};
