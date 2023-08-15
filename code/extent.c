#include <linux/fs.h>
#include <linux/kernel.h>

#include "revofs.h"

/*
 * Search the extent which contain the target block.
 * Retrun the first unused file index if not found.
 * Return -1 if it is out of range.
 */
uint32_t revofs_ext_search(struct revofs_file_ei_block *index, uint32_t iblock)
{
    uint32_t i;
    for (i = 0; i < REVOFS_MAX_EXTENTS; i++) {
        uint32_t block = index->extents[i].ee_block;
        uint32_t len = index->extents[i].ee_len;
        if (index->extents[i].ee_start == 0 ||
            (iblock >= block && iblock < block + len))
            return i;
    }
    return -1;
}
