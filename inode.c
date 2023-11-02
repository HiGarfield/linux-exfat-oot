// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2012-2013 Samsung Electronics Co., Ltd.
 */

#include <linux/version.h>
#include <linux/init.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/time.h>
#include <linux/writeback.h>
#include <linux/uio.h>
#include <linux/random.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0)
#include <linux/iversion.h>
#endif
#include "exfat_raw.h"
#include "exfat_fs.h"

int __exfat_write_inode(struct inode *inode, int sync)
{
	unsigned long long on_disk_size;
	struct exfat_dentry *ep, *ep2;
	struct exfat_entry_set_cache es;
	struct super_block *sb = inode->i_sb;
	struct exfat_sb_info *sbi = EXFAT_SB(sb);
	struct exfat_inode_info *ei = EXFAT_I(inode);
	bool is_dir = (ei->type == TYPE_DIR) ? true : false;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 7, 0)
	struct timespec64 ts;
#endif

	if (inode->i_ino == EXFAT_ROOT_INO)
		return 0;

	/*
	 * If the inode is already unlinked, there is no need for updating it.
	 */
	if (ei->dir.dir == DIR_DELETED)
		return 0;

	if (is_dir && ei->dir.dir == sbi->root_dir && ei->entry == -1)
		return 0;

	exfat_set_volume_dirty(sb);

	/* get the directory entry of given file or directory */
	if (exfat_get_dentry_set(&es, sb, &(ei->dir), ei->entry, ES_ALL_ENTRIES))
		return -EIO;
	ep = exfat_get_dentry_cached(&es, ES_IDX_FILE);
	ep2 = exfat_get_dentry_cached(&es, ES_IDX_STREAM);

	ep->dentry.file.attr = cpu_to_le16(exfat_make_attr(inode));

	/* set FILE_INFO structure using the acquired struct exfat_dentry */
	exfat_set_entry_time(sbi, &ei->i_crtime,
			&ep->dentry.file.create_tz,
			&ep->dentry.file.create_time,
			&ep->dentry.file.create_date,
			&ep->dentry.file.create_time_cs);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 7, 0)
	exfat_set_entry_time(sbi, &ts,
			     &ep->dentry.file.modify_tz,
			     &ep->dentry.file.modify_time,
			     &ep->dentry.file.modify_date,
			     &ep->dentry.file.modify_time_cs);
	inode_set_mtime_to_ts(inode, ts);
	exfat_set_entry_time(sbi, &ts,
			     &ep->dentry.file.access_tz,
			     &ep->dentry.file.access_time,
			     &ep->dentry.file.access_date,
			     NULL);
	inode_set_atime_to_ts(inode, ts);
#else
	exfat_set_entry_time(sbi, &inode->i_mtime,
			&ep->dentry.file.modify_tz,
			&ep->dentry.file.modify_time,
			&ep->dentry.file.modify_date,
			&ep->dentry.file.modify_time_cs);
	exfat_set_entry_time(sbi, &inode->i_atime,
			&ep->dentry.file.access_tz,
			&ep->dentry.file.access_time,
			&ep->dentry.file.access_date,
			NULL);
#endif

	/* File size should be zero if there is no cluster allocated */
	on_disk_size = i_size_read(inode);

	if (ei->start_clu == EXFAT_EOF_CLUSTER)
		on_disk_size = 0;

	ep2->dentry.stream.valid_size = cpu_to_le64(on_disk_size);
	ep2->dentry.stream.size = ep2->dentry.stream.valid_size;
	if (on_disk_size) {
		ep2->dentry.stream.flags = ei->flags;
		ep2->dentry.stream.start_clu = cpu_to_le32(ei->start_clu);
	} else {
		ep2->dentry.stream.flags = ALLOC_FAT_CHAIN;
		ep2->dentry.stream.start_clu = EXFAT_FREE_CLUSTER;
	}

	exfat_update_dir_chksum_with_entry_set(&es);
	return exfat_put_dentry_set(&es, sync);
}

int exfat_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	int ret;

	mutex_lock(&EXFAT_SB(inode->i_sb)->s_lock);
	ret = __exfat_write_inode(inode, wbc->sync_mode == WB_SYNC_ALL);
	mutex_unlock(&EXFAT_SB(inode->i_sb)->s_lock);

	return ret;
}

void exfat_sync_inode(struct inode *inode)
{
	lockdep_assert_held(&EXFAT_SB(inode->i_sb)->s_lock);
	__exfat_write_inode(inode, 1);
}

/*
 * Input: inode, (logical) clu_offset, target allocation area
 * Output: errcode, cluster number
 * *clu = (~0), if it's unable to allocate a new cluster
 */
static int exfat_map_cluster(struct inode *inode, unsigned int clu_offset,
		unsigned int *clu, int create)
{
	int ret;
	unsigned int last_clu;
	struct exfat_chain new_clu;
	struct super_block *sb = inode->i_sb;
	struct exfat_sb_info *sbi = EXFAT_SB(sb);
	struct exfat_inode_info *ei = EXFAT_I(inode);
	unsigned int local_clu_offset = clu_offset;
	unsigned int num_to_be_allocated = 0, num_clusters = 0;

	if (ei->i_size_ondisk > 0)
		num_clusters =
			EXFAT_B_TO_CLU_ROUND_UP(ei->i_size_ondisk, sbi);

	if (clu_offset >= num_clusters)
		num_to_be_allocated = clu_offset - num_clusters + 1;

	if (!create && (num_to_be_allocated > 0)) {
		*clu = EXFAT_EOF_CLUSTER;
		return 0;
	}

	*clu = last_clu = ei->start_clu;

	if (ei->flags == ALLOC_NO_FAT_CHAIN) {
		if (clu_offset > 0 && *clu != EXFAT_EOF_CLUSTER) {
			last_clu += clu_offset - 1;

			if (clu_offset == num_clusters)
				*clu = EXFAT_EOF_CLUSTER;
			else
				*clu += clu_offset;
		}
	} else if (ei->type == TYPE_FILE) {
		unsigned int fclus = 0;
		int err = exfat_get_cluster(inode, clu_offset,
				&fclus, clu, &last_clu, 1);
		if (err)
			return -EIO;

		clu_offset -= fclus;
	} else {
		/* hint information */
		if (clu_offset > 0 && ei->hint_bmap.off != EXFAT_EOF_CLUSTER &&
		    ei->hint_bmap.off > 0 && clu_offset >= ei->hint_bmap.off) {
			clu_offset -= ei->hint_bmap.off;
			/* hint_bmap.clu should be valid */
			WARN_ON(ei->hint_bmap.clu < 2);
			*clu = ei->hint_bmap.clu;
		}

		while (clu_offset > 0 && *clu != EXFAT_EOF_CLUSTER) {
			last_clu = *clu;
			if (exfat_get_next_cluster(sb, clu))
				return -EIO;
			clu_offset--;
		}
	}

	if (*clu == EXFAT_EOF_CLUSTER) {
		exfat_set_volume_dirty(sb);

		new_clu.dir = (last_clu == EXFAT_EOF_CLUSTER) ?
				EXFAT_EOF_CLUSTER : last_clu + 1;
		new_clu.size = 0;
		new_clu.flags = ei->flags;

		/* allocate a cluster */
		if (num_to_be_allocated < 1) {
			/* Broken FAT (i_sze > allocated FAT) */
			exfat_fs_error(sb, "broken FAT chain.");
			return -EIO;
		}

		ret = exfat_alloc_cluster(inode, num_to_be_allocated, &new_clu,
				inode_needs_sync(inode));
		if (ret)
			return ret;

		if (new_clu.dir == EXFAT_EOF_CLUSTER ||
		    new_clu.dir == EXFAT_FREE_CLUSTER) {
			exfat_fs_error(sb,
				"bogus cluster new allocated (last_clu : %u, new_clu : %u)",
				last_clu, new_clu.dir);
			return -EIO;
		}

		/* append to the FAT chain */
		if (last_clu == EXFAT_EOF_CLUSTER) {
			if (new_clu.flags == ALLOC_FAT_CHAIN)
				ei->flags = ALLOC_FAT_CHAIN;
			ei->start_clu = new_clu.dir;
		} else {
			if (new_clu.flags != ei->flags) {
				/* no-fat-chain bit is disabled,
				 * so fat-chain should be synced with
				 * alloc-bitmap
				 */
				exfat_chain_cont_cluster(sb, ei->start_clu,
					num_clusters);
				ei->flags = ALLOC_FAT_CHAIN;
			}
			if (new_clu.flags == ALLOC_FAT_CHAIN)
				if (exfat_ent_set(sb, last_clu, new_clu.dir))
					return -EIO;
		}

		num_clusters += num_to_be_allocated;
		*clu = new_clu.dir;

		inode->i_blocks += EXFAT_CLU_TO_B(num_to_be_allocated, sbi) >> 9;

		/*
		 * Move *clu pointer along FAT chains (hole care) because the
		 * caller of this function expect *clu to be the last cluster.
		 * This only works when num_to_be_allocated >= 2,
		 * *clu = (the first cluster of the allocated chain) =>
		 * (the last cluster of ...)
		 */
		if (ei->flags == ALLOC_NO_FAT_CHAIN) {
			*clu += num_to_be_allocated - 1;
		} else {
			while (num_to_be_allocated > 1) {
				if (exfat_get_next_cluster(sb, clu))
					return -EIO;
				num_to_be_allocated--;
			}
		}

	}

	/* hint information */
	ei->hint_bmap.off = local_clu_offset;
	ei->hint_bmap.clu = *clu;

	return 0;
}

static int exfat_map_new_buffer(struct exfat_inode_info *ei,
		struct buffer_head *bh, loff_t pos)
{
	if (buffer_delay(bh) && pos > ei->i_size_aligned)
		return -EIO;
	set_buffer_new(bh);

	/*
	 * Adjust i_size_aligned if i_size_ondisk is bigger than it.
	 */
	if (ei->i_size_ondisk > ei->i_size_aligned)
		ei->i_size_aligned = ei->i_size_ondisk;
	return 0;
}

static int exfat_get_block(struct inode *inode, sector_t iblock,
		struct buffer_head *bh_result, int create)
{
	struct exfat_inode_info *ei = EXFAT_I(inode);
	struct super_block *sb = inode->i_sb;
	struct exfat_sb_info *sbi = EXFAT_SB(sb);
	unsigned long max_blocks = bh_result->b_size >> inode->i_blkbits;
	int err = 0;
	unsigned long mapped_blocks = 0;
	unsigned int cluster, sec_offset;
	sector_t last_block;
	sector_t phys = 0;
	loff_t pos;

	mutex_lock(&sbi->s_lock);
	last_block = EXFAT_B_TO_BLK_ROUND_UP(i_size_read(inode), sb);
	if (iblock >= last_block && !create)
		goto done;

	/* Is this block already allocated? */
	err = exfat_map_cluster(inode, iblock >> sbi->sect_per_clus_bits,
			&cluster, create);
	if (err) {
		if (err != -ENOSPC)
			exfat_fs_error_ratelimit(sb,
				"failed to bmap (inode : %p iblock : %llu, err : %d)",
				inode, (unsigned long long)iblock, err);
		goto unlock_ret;
	}

	if (cluster == EXFAT_EOF_CLUSTER)
		goto done;

	/* sector offset in cluster */
	sec_offset = iblock & (sbi->sect_per_clus - 1);

	phys = exfat_cluster_to_sector(sbi, cluster) + sec_offset;
	mapped_blocks = sbi->sect_per_clus - sec_offset;
	max_blocks = min(mapped_blocks, max_blocks);

	/* Treat newly added block / cluster */
	if (iblock < last_block)
		create = 0;

	if (create || buffer_delay(bh_result)) {
		pos = EXFAT_BLK_TO_B((iblock + 1), sb);
		if (ei->i_size_ondisk < pos)
			ei->i_size_ondisk = pos;
	}

	if (create) {
		err = exfat_map_new_buffer(ei, bh_result, pos);
		if (err) {
			exfat_fs_error(sb,
					"requested for bmap out of range(pos : (%llu) > i_size_aligned(%llu)\n",
					pos, ei->i_size_aligned);
			goto unlock_ret;
		}
	}

	if (buffer_delay(bh_result))
		clear_buffer_delay(bh_result);
	map_bh(bh_result, sb, phys);
done:
	bh_result->b_size = EXFAT_BLK_TO_B(max_blocks, sb);
unlock_ret:
	mutex_unlock(&sbi->s_lock);
	return err;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)
static int exfat_read_folio(struct file *file, struct folio *folio)
{
	return mpage_read_folio(folio, exfat_get_block);
}
#else
static int exfat_readpage(struct file *file, struct page *page)
{
	return mpage_readpage(page, exfat_get_block);
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
static void exfat_readahead(struct readahead_control *rac)
{
	mpage_readahead(rac, exfat_get_block);
}
#else
static int exfat_readpages(struct file *file, struct address_space *mapping,
		struct list_head *pages, unsigned int nr_pages)
{
	return mpage_readpages(mapping, pages, nr_pages, exfat_get_block);
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 2, 0)
static int exfat_writepage(struct page *page, struct writeback_control *wbc)
{
	return block_write_full_page(page, exfat_get_block, wbc);
}
#endif

static int exfat_writepages(struct address_space *mapping,
		struct writeback_control *wbc)
{
	return mpage_writepages(mapping, wbc, exfat_get_block);
}

static void exfat_write_failed(struct address_space *mapping, loff_t to)
{
	struct inode *inode = mapping->host;

	if (to > i_size_read(inode)) {
		truncate_pagecache(inode, i_size_read(inode));
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 7, 0)
		inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
#else
		inode->i_mtime = inode_set_ctime_current(inode);
#endif
#else
		inode->i_mtime = inode->i_ctime = current_time(inode);
#endif
#else
		inode->i_mtime = inode->i_ctime = CURRENT_TIME_SEC;
#endif
		exfat_truncate(inode);
	}
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)
static int exfat_write_begin(struct file *file, struct address_space *mapping,
		loff_t pos, unsigned int len,
		struct page **pagep, void **fsdata)
#else
static int exfat_write_begin(struct file *file, struct address_space *mapping,
		loff_t pos, unsigned int len, unsigned int flags,
		struct page **pagep, void **fsdata)
#endif
{
	int ret;

	*pagep = NULL;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)
	ret = cont_write_begin(file, mapping, pos, len, pagep, fsdata,
			       exfat_get_block,
			       &EXFAT_I(mapping->host)->i_size_ondisk);
#else
	ret = cont_write_begin(file, mapping, pos, len, flags, pagep, fsdata,
			       exfat_get_block,
			       &EXFAT_I(mapping->host)->i_size_ondisk);
#endif
	if (ret < 0)
		exfat_write_failed(mapping, pos+len);

	return ret;
}

static int exfat_write_end(struct file *file, struct address_space *mapping,
		loff_t pos, unsigned int len, unsigned int copied,
		struct page *pagep, void *fsdata)
{
	struct inode *inode = mapping->host;
	struct exfat_inode_info *ei = EXFAT_I(inode);
	int err;

	err = generic_write_end(file, mapping, pos, len, copied, pagep, fsdata);

	if (ei->i_size_aligned < i_size_read(inode)) {
		exfat_fs_error(inode->i_sb,
			"invalid size(size(%llu) > aligned(%llu)\n",
			i_size_read(inode), ei->i_size_aligned);
		return -EIO;
	}

	if (err < len)
		exfat_write_failed(mapping, pos+len);

	if (!(err < 0) && !(ei->attr & EXFAT_ATTR_ARCHIVE)) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 7, 0)
		inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
#else
		inode->i_mtime = inode_set_ctime_current(inode);
#endif
#else
		inode->i_mtime = inode->i_ctime = current_time(inode);
#endif
#else
		inode->i_mtime = inode->i_ctime = CURRENT_TIME_SEC;
#endif
		ei->attr |= EXFAT_ATTR_ARCHIVE;
		mark_inode_dirty(inode);
	}

	return err;
}


#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 7, 0)
static ssize_t exfat_direct_IO(struct kiocb *iocb, struct iov_iter *iter)
#else
static ssize_t exfat_direct_IO(struct kiocb *iocb, struct iov_iter *iter,
                             loff_t offset)
#endif
{
	struct address_space *mapping = iocb->ki_filp->f_mapping;
	struct inode *inode = mapping->host;
	loff_t size = iocb->ki_pos + iov_iter_count(iter);
	int rw = iov_iter_rw(iter);
	ssize_t ret;

	if (rw == WRITE) {
		/*
		 * FIXME: blockdev_direct_IO() doesn't use ->write_begin(),
		 * so we need to update the ->i_size_aligned to block boundary.
		 *
		 * But we must fill the remaining area or hole by nul for
		 * updating ->i_size_aligned
		 *
		 * Return 0, and fallback to normal buffered write.
		 */
		if (EXFAT_I(inode)->i_size_aligned < size)
			return 0;
	}

	/*
	 * Need to use the DIO_LOCKING for avoiding the race
	 * condition of exfat_get_block() and ->truncate().
	 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 7, 0)
	ret = blockdev_direct_IO(iocb, inode, iter, exfat_get_block);
#else
	ret = blockdev_direct_IO(iocb, inode, iter, offset, exfat_get_block);
#endif
	if (ret < 0 && (rw & WRITE))
		exfat_write_failed(mapping, size);
	return ret;
}

static sector_t exfat_aop_bmap(struct address_space *mapping, sector_t block)
{
	sector_t blocknr;

	/* exfat_get_cluster() assumes the requested blocknr isn't truncated. */
	down_read(&EXFAT_I(mapping->host)->truncate_lock);
	blocknr = generic_block_bmap(mapping, block, exfat_get_block);
	up_read(&EXFAT_I(mapping->host)->truncate_lock);
	return blocknr;
}

/*
 * exfat_block_truncate_page() zeroes out a mapping from file offset `from'
 * up to the end of the block which corresponds to `from'.
 * This is required during truncate to physically zeroout the tail end
 * of that block so it doesn't yield old data if the file is later grown.
 * Also, avoid causing failure from fsx for cases of "data past EOF"
 */
int exfat_block_truncate_page(struct inode *inode, loff_t from)
{
	return block_truncate_page(inode->i_mapping, from, exfat_get_block);
}

static const struct address_space_operations exfat_aops = {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0)
	.dirty_folio	= block_dirty_folio,
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
	.set_page_dirty	= __set_page_dirty_buffers,
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0)
	.invalidate_folio = block_invalidate_folio,
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)
	.read_folio	= exfat_read_folio,
#else
	.readpage	= exfat_readpage,
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
	.readahead	= exfat_readahead,
#else
	.readpages	= exfat_readpages,
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 2, 0)
	.writepage	= exfat_writepage,
#endif
	.writepages	= exfat_writepages,
	.write_begin	= exfat_write_begin,
	.write_end	= exfat_write_end,
	.direct_IO	= exfat_direct_IO,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 2, 0)
	.bmap		= exfat_aop_bmap
#else
	.bmap		= exfat_aop_bmap,
	.migrate_folio	= buffer_migrate_folio,
#endif
};

static inline unsigned long exfat_hash(loff_t i_pos)
{
	return hash_32(i_pos, EXFAT_HASH_BITS);
}

void exfat_hash_inode(struct inode *inode, loff_t i_pos)
{
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	struct hlist_head *head = sbi->inode_hashtable + exfat_hash(i_pos);

	spin_lock(&sbi->inode_hash_lock);
	EXFAT_I(inode)->i_pos = i_pos;
	hlist_add_head(&EXFAT_I(inode)->i_hash_fat, head);
	spin_unlock(&sbi->inode_hash_lock);
}

void exfat_unhash_inode(struct inode *inode)
{
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);

	spin_lock(&sbi->inode_hash_lock);
	hlist_del_init(&EXFAT_I(inode)->i_hash_fat);
	EXFAT_I(inode)->i_pos = 0;
	spin_unlock(&sbi->inode_hash_lock);
}

struct inode *exfat_iget(struct super_block *sb, loff_t i_pos)
{
	struct exfat_sb_info *sbi = EXFAT_SB(sb);
	struct exfat_inode_info *info;
	struct hlist_head *head = sbi->inode_hashtable + exfat_hash(i_pos);
	struct inode *inode = NULL;

	spin_lock(&sbi->inode_hash_lock);
	hlist_for_each_entry(info, head, i_hash_fat) {
		WARN_ON(info->vfs_inode.i_sb != sb);

		if (i_pos != info->i_pos)
			continue;
		inode = igrab(&info->vfs_inode);
		if (inode)
			break;
	}
	spin_unlock(&sbi->inode_hash_lock);
	return inode;
}

/* doesn't deal with root inode */
static int exfat_fill_inode(struct inode *inode, struct exfat_dir_entry *info)
{
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	struct exfat_inode_info *ei = EXFAT_I(inode);
	loff_t size = info->size;

	ei->dir = info->dir;
	ei->entry = info->entry;
	ei->attr = info->attr;
	ei->start_clu = info->start_clu;
	ei->flags = info->flags;
	ei->type = info->type;

	ei->version = 0;
	ei->hint_stat.eidx = 0;
	ei->hint_stat.clu = info->start_clu;
	ei->hint_femp.eidx = EXFAT_HINT_NONE;
	ei->hint_bmap.off = EXFAT_EOF_CLUSTER;
	ei->i_pos = 0;

	inode->i_uid = sbi->options.fs_uid;
	inode->i_gid = sbi->options.fs_gid;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0)
	inode_inc_iversion(inode);
#else
	inode->i_version++;
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
	inode->i_generation = get_random_u32();
#else
	inode->i_generation = prandom_u32();
#endif

	if (info->attr & EXFAT_ATTR_SUBDIR) { /* directory */
		inode->i_generation &= ~1;
		inode->i_mode = exfat_make_mode(sbi, info->attr, 0777);
		inode->i_op = &exfat_dir_inode_operations;
		inode->i_fop = &exfat_dir_operations;
		set_nlink(inode, info->num_subdirs);
	} else { /* regular file */
		inode->i_generation |= 1;
		inode->i_mode = exfat_make_mode(sbi, info->attr, 0777);
		inode->i_op = &exfat_file_inode_operations;
		inode->i_fop = &exfat_file_operations;
		inode->i_mapping->a_ops = &exfat_aops;
		inode->i_mapping->nrpages = 0;
	}

	i_size_write(inode, size);

	/* ondisk and aligned size should be aligned with block size */
	if (size & (inode->i_sb->s_blocksize - 1)) {
		size |= (inode->i_sb->s_blocksize - 1);
		size++;
	}

	ei->i_size_aligned = size;
	ei->i_size_ondisk = size;

	exfat_save_attr(inode, info->attr);

	inode->i_blocks = round_up(i_size_read(inode), sbi->cluster_size) >> 9;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 7, 0)
	inode_set_mtime_to_ts(inode, info->mtime);
#else
	inode->i_mtime = info->mtime;
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
	inode_set_ctime_to_ts(inode, info->mtime);
#else
	inode->i_ctime = info->mtime;
#endif
	ei->i_crtime = info->crtime;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 7, 0)
	inode_set_atime_to_ts(inode, info->atime);
#else
	inode->i_atime = info->atime;
#endif

	return 0;
}

struct inode *exfat_build_inode(struct super_block *sb,
		struct exfat_dir_entry *info, loff_t i_pos)
{
	struct inode *inode;
	int err;

	inode = exfat_iget(sb, i_pos);
	if (inode)
		goto out;
	inode = new_inode(sb);
	if (!inode) {
		inode = ERR_PTR(-ENOMEM);
		goto out;
	}
	inode->i_ino = iunique(sb, EXFAT_ROOT_INO);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0)
	inode_set_iversion(inode, 1);
#else
	inode->i_version = 1;
#endif
	err = exfat_fill_inode(inode, info);
	if (err) {
		iput(inode);
		inode = ERR_PTR(err);
		goto out;
	}
	exfat_hash_inode(inode, i_pos);
	insert_inode_hash(inode);
out:
	return inode;
}

void exfat_evict_inode(struct inode *inode)
{
	truncate_inode_pages(&inode->i_data, 0);

	if (!inode->i_nlink) {
		i_size_write(inode, 0);
		mutex_lock(&EXFAT_SB(inode->i_sb)->s_lock);
		__exfat_truncate(inode);
		mutex_unlock(&EXFAT_SB(inode->i_sb)->s_lock);
	}

	invalidate_inode_buffers(inode);
	clear_inode(inode);
	exfat_cache_inval_inode(inode);
	exfat_unhash_inode(inode);
}
