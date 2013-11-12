/*
 * zswap.c - zswap driver file
 *
 * zswap is a backend for frontswap that takes pages that are in the
 * process of being swapped out and attempts to compress them and store
 * them in a RAM-based memory pool.  This results in a significant I/O
 * reduction on the real swap device and, in the case of a slow swap
 * device, can also improve workload performance.
 *
 * Copyright (C) 2012  Seth Jennings <sjenning@linux.vnet.ibm.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
*/

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/frontswap.h>
#include <linux/rbtree.h>
#include <linux/swap.h>
#include <linux/crypto.h>
#include <linux/mempool.h>
#include <linux/zsmalloc.h>

#include <linux/mm_types.h>
#include <linux/page-flags.h>
#include <linux/swapops.h>
#include <linux/writeback.h>
#include <linux/pagemap.h>

/* Debugging code for zswap kernel panic */
#include <linux/mm.h>

/*********************************
* statistics
**********************************/
/* Number of memory pages used by the compressed pool */
atomic_t zswap_pool_pages = ATOMIC_INIT(0);
/* The number of compressed pages currently stored in zswap */
atomic_t zswap_stored_pages = ATOMIC_INIT(0);

#ifdef CONFIG_ZSWAP_ENABLE_WRITEBACK
/* The number of outstanding pages awaiting writeback */
static atomic_t zswap_outstanding_writebacks = ATOMIC_INIT(0);
#endif

/*
 * The statistics below are not protected from concurrent access for
 * performance reasons so they may not be a 100% accurate.  However,
 * they do provide useful information on roughly how many times a
 * certain event is occurring.
*/
static u64 zswap_pool_limit_hit;
static u64 zswap_written_back_pages;
static u64 zswap_reject_compress_poor;
static u64 zswap_writeback_attempted;
static u64 zswap_reject_tmppage_fail;
static u64 zswap_reject_zsmalloc_fail;
static u64 zswap_reject_kmemcache_fail;
static u64 zswap_saved_by_writeback;
static u64 zswap_duplicate_entry;

/*********************************
* tunables
**********************************/
/* Enable/disable zswap (enabled by default, fixed at boot for now) */
static bool zswap_enabled = 1;
module_param_named(enabled, zswap_enabled, bool, 0);

/* Compressor to be used by zswap (fixed at boot for now) */
#ifdef CONFIG_CRYPTO_LZ4
#define ZSWAP_COMPRESSOR_DEFAULT "lz4"
#else
#define ZSWAP_COMPRESSOR_DEFAULT "lzo"
#endif
static char *zswap_compressor = ZSWAP_COMPRESSOR_DEFAULT;
module_param_named(compressor, zswap_compressor, charp, 0);

/* The maximum percentage of memory that the compressed pool can occupy */
static unsigned int zswap_max_pool_percent = 50;
module_param_named(max_pool_percent,
			zswap_max_pool_percent, uint, 0644);

/*
 * Maximum compression ratio, as as percentage, for an acceptable
 * compressed page. Any pages that do not compress by at least
 * this ratio will be rejected.
*/
static unsigned int zswap_max_compression_ratio = 80;
module_param_named(max_compression_ratio,
			zswap_max_compression_ratio, uint, 0644);

/*
 * Maximum number of outstanding writebacks allowed at any given time.
 * This is to prevent decompressing an unbounded number of compressed
 * pages into the swap cache all at once, and to help with writeback
 * congestion.
*/
#define ZSWAP_MAX_OUTSTANDING_FLUSHES 64

/*********************************
* compression functions
**********************************/
/* per-cpu compression transforms */
static struct crypto_comp * __percpu *zswap_comp_pcpu_tfms;

enum comp_op {
	ZSWAP_COMPOP_COMPRESS,
	ZSWAP_COMPOP_DECOMPRESS
};

static int zswap_comp_op(enum comp_op op, const u8 *src, unsigned int slen,
				u8 *dst, unsigned int *dlen)
{
	struct crypto_comp *tfm;
	int ret;

	tfm = *per_cpu_ptr(zswap_comp_pcpu_tfms, get_cpu());
	switch (op) {
	case ZSWAP_COMPOP_COMPRESS:
		ret = crypto_comp_compress(tfm, src, slen, dst, dlen);
		break;
	case ZSWAP_COMPOP_DECOMPRESS:
		ret = crypto_comp_decompress(tfm, src, slen, dst, dlen);
		break;
	default:
		ret = -EINVAL;
	}

	put_cpu();
	return ret;
}

static int __init zswap_comp_init(void)
{
	if (!crypto_has_comp(zswap_compressor, 0, 0)) {
		pr_info("%s compressor not available\n", zswap_compressor);
		/* fall back to default compressor */
		zswap_compressor = ZSWAP_COMPRESSOR_DEFAULT;
		if (!crypto_has_comp(zswap_compressor, 0, 0))
			/* can't even load the default compressor */
			return -ENODEV;
	}
	pr_info("using %s compressor\n", zswap_compressor);

	/* alloc percpu transforms */
	zswap_comp_pcpu_tfms = alloc_percpu(struct crypto_comp *);
	if (!zswap_comp_pcpu_tfms)
		return -ENOMEM;
	return 0;
}

static void zswap_comp_exit(void)
{
	/* free percpu transforms */
	if (zswap_comp_pcpu_tfms)
		free_percpu(zswap_comp_pcpu_tfms);
}

/*********************************
* data structures
**********************************/

/*
 * struct zswap_entry
 *
 * This structure contains the metadata for tracking a single compressed
 * page within zswap.
 *
 * rbnode - links the entry into red-black tree for the appropriate swap type
 * lru - links the entry into the lru list for the appropriate swap type
 * refcount - the number of outstanding reference to the entry. This is needed
 *            to protect against premature freeing of the entry by code
 *            concurent calls to load, invalidate, and writeback.  The lock
 *            for the zswap_tree structure that contains the entry must
 *            be held while changing the refcount.  Since the lock must
 *            be held, there is no reason to also make refcount atomic.
 * type - the swap type for the entry.  Used to map back to the zswap_tree
 *        structure that contains the entry.
 * offset - the swap offset for the entry.  Index into the red-black tree.
 * handle - zsmalloc allocation handle that stores the compressed page data
 * length - the length in bytes of the compressed page data.  Needed during
 *           decompression
 */
struct zswap_entry {
	struct rb_node rbnode;
	struct list_head lru;
	int refcount;
	pgoff_t offset;
	unsigned long handle;
	unsigned int length;
};

/*
 * The tree lock in the zswap_tree struct protects a few things:
 * - the rbtree
 * - the lru list
 * - the refcount field of each entry in the tree
 */
struct zswap_tree {
	struct rb_root rbroot;
	struct list_head lru;
	spinlock_t lock;
	struct zs_pool *pool;
	unsigned type;
};

static struct zswap_tree *zswap_trees[MAX_SWAPFILES];

/*********************************
* zswap entry functions
**********************************/
#define ZSWAP_KMEM_CACHE_NAME "zswap_entry_cache"
static struct kmem_cache *zswap_entry_cache;

static inline int zswap_entry_cache_create(void)
{
	zswap_entry_cache =
		kmem_cache_create(ZSWAP_KMEM_CACHE_NAME,
			sizeof(struct zswap_entry), 0, 0, NULL);
	return (zswap_entry_cache == NULL);
}

static inline void zswap_entry_cache_destory(void)
{
	kmem_cache_destroy(zswap_entry_cache);
}

static inline struct zswap_entry *zswap_entry_cache_alloc(gfp_t gfp)
{
	struct zswap_entry *entry;
	entry = kmem_cache_alloc(zswap_entry_cache, gfp);
	if (!entry)
		return NULL;
	INIT_LIST_HEAD(&entry->lru);
	entry->refcount = 1;
	return entry;
}

static inline void zswap_entry_cache_free(struct zswap_entry *entry)
{
	kmem_cache_free(zswap_entry_cache, entry);
}

static inline void zswap_entry_get(struct zswap_entry *entry)
{
	entry->refcount++;
}

static inline int zswap_entry_put(struct zswap_entry *entry)
{
	entry->refcount--;
	return entry->refcount;
}

/*********************************
* rbtree functions
**********************************/
static struct zswap_entry *zswap_rb_search(struct rb_root *root, pgoff_t offset)
{
	struct rb_node *node = root->rb_node;
	struct zswap_entry *entry;

	while (node) {
		entry = rb_entry(node, struct zswap_entry, rbnode);
		if (entry->offset > offset)
			node = node->rb_left;
		else if (entry->offset < offset)
			node = node->rb_right;
		else
			return entry;
	}
	return NULL;
}

/*
 * In the case that a entry with the same offset is found, it a pointer to
 * the existing entry is stored in dupentry and the function returns -EEXIST
*/
static int zswap_rb_insert(struct rb_root *root, struct zswap_entry *entry,
			struct zswap_entry **dupentry)
{
	struct rb_node **link = &root->rb_node, *parent = NULL;
	struct zswap_entry *myentry;

	while (*link) {
		parent = *link;
		myentry = rb_entry(parent, struct zswap_entry, rbnode);
		if (myentry->offset > entry->offset)
			link = &(*link)->rb_left;
		else if (myentry->offset < entry->offset)
			link = &(*link)->rb_right;
		else {
			*dupentry = myentry;
			return -EEXIST;
		}
	}
	rb_link_node(&entry->rbnode, parent, link);
	rb_insert_color(&entry->rbnode, root);
	return 0;
}

/*********************************
* per-cpu code
**********************************/
static DEFINE_PER_CPU(u8 *, zswap_dstmem);

static int __zswap_cpu_notifier(unsigned long action, unsigned long cpu)
{
	struct crypto_comp *tfm;
	u8 *dst;

	switch (action) {
	case CPU_UP_PREPARE:
		tfm = crypto_alloc_comp(zswap_compressor, 0, 0);
		if (IS_ERR(tfm)) {
			pr_err("can't allocate compressor transform\n");
			return NOTIFY_BAD;
		}
		*per_cpu_ptr(zswap_comp_pcpu_tfms, cpu) = tfm;
		dst = kmalloc(PAGE_SIZE * 2, GFP_KERNEL);
		if (!dst) {
			pr_err("can't allocate compressor buffer\n");
			crypto_free_comp(tfm);
			*per_cpu_ptr(zswap_comp_pcpu_tfms, cpu) = NULL;
			return NOTIFY_BAD;
		}
		per_cpu(zswap_dstmem, cpu) = dst;
		break;
	case CPU_DEAD:
	case CPU_UP_CANCELED:
		tfm = *per_cpu_ptr(zswap_comp_pcpu_tfms, cpu);
		if (tfm) {
			crypto_free_comp(tfm);
			*per_cpu_ptr(zswap_comp_pcpu_tfms, cpu) = NULL;
		}
		dst = per_cpu(zswap_dstmem, cpu);
		if (dst) {
			kfree(dst);
			per_cpu(zswap_dstmem, cpu) = NULL;
		}
		break;
	default:
		break;
	}
	return NOTIFY_OK;
}

static int zswap_cpu_notifier(struct notifier_block *nb,
				unsigned long action, void *pcpu)
{
	unsigned long cpu = (unsigned long)pcpu;
	return __zswap_cpu_notifier(action, cpu);
}

static struct notifier_block zswap_cpu_notifier_block = {
	.notifier_call = zswap_cpu_notifier
};

static int zswap_cpu_init(void)
{
	unsigned long cpu;

	cpu_notifier_register_begin();
	for_each_online_cpu(cpu)
		if (__zswap_cpu_notifier(CPU_UP_PREPARE, cpu) != NOTIFY_OK)
			goto cleanup;
	__register_cpu_notifier(&zswap_cpu_notifier_block);
	cpu_notifier_register_done();
	return 0;

cleanup:
	for_each_online_cpu(cpu)
		__zswap_cpu_notifier(CPU_UP_CANCELED, cpu);
	cpu_notifier_register_done();
	return -ENOMEM;
}

/*********************************
* zsmalloc callbacks
**********************************/
static mempool_t *zswap_page_pool;

static inline unsigned int zswap_max_pool_pages(void)
{
	return zswap_max_pool_percent * totalram_pages / 100;
}

static inline int zswap_page_pool_create(void)
{
	/* TODO: dynamically size mempool */
	zswap_page_pool = mempool_create_page_pool(256, 0);
	if (!zswap_page_pool)
		return -ENOMEM;
	return 0;
}

static inline void zswap_page_pool_destroy(void)
{
	mempool_destroy(zswap_page_pool);
}

static struct page *zswap_alloc_page(gfp_t flags)
{
	struct page *page;

	if (atomic_read(&zswap_pool_pages) >= zswap_max_pool_pages()) {
		zswap_pool_limit_hit++;
		return NULL;
	}
	page = mempool_alloc(zswap_page_pool, flags);
	if (page)
		atomic_inc(&zswap_pool_pages);
	return page;
}

static void zswap_free_page(struct page *page)
{
	if (!page)
		return;
	mempool_free(page, zswap_page_pool);
	atomic_dec(&zswap_pool_pages);
}

static struct zs_ops zswap_zs_ops = {
	.alloc = zswap_alloc_page,
	.free = zswap_free_page
};


/*********************************
* helpers
**********************************/

/*
 * Carries out the common pattern of freeing and entry's zsmalloc allocation,
 * freeing the entry itself, and decrementing the number of stored pages.
 */
static void zswap_free_entry(struct zswap_tree *tree, struct zswap_entry *entry)
{
	zs_free(tree->pool, entry->handle);
	zswap_entry_cache_free(entry);
	atomic_dec(&zswap_stored_pages);
}

#ifdef CONFIG_ZSWAP_ENABLE_WRITEBACK
/*********************************
* writeback code
**********************************/
static void zswap_end_swap_write(struct bio *bio, int err)
{
	end_swap_bio_write(bio, err);
	atomic_dec(&zswap_outstanding_writebacks);
	zswap_written_back_pages++;
}

/* return enum for zswap_get_swap_cache_page */
enum zswap_get_swap_ret {
	ZSWAP_SWAPCACHE_NEW,
	ZSWAP_SWAPCACHE_EXIST,
	ZSWAP_SWAPCACHE_NOMEM
};

/*
 * zswap_get_swap_cache_page
 *
 * This is an adaption of read_swap_cache_async()
 *
 * This function tries to find a page with the given swap entry
 * in the swapper_space address space (the swap cache).  If the page
 * is found, it is returned in retpage.  Otherwise, a page is allocated,
 * added to the swap cache, and returned in retpage.
 *
 * If success, the swap cache page is returned in retpage
 * Returns 0 if page was already in the swap cache, page is not locked
 * Returns 1 if the new page needs to be populated, page is locked
 * Returns <0 on error
 */
static int zswap_get_swap_cache_page(swp_entry_t entry,
				struct page **retpage)
{
	struct page *found_page, *new_page = NULL;
	struct address_space *swapper_space = swap_address_space(entry);
	int err;

	*retpage = NULL;
	do {
		/*
		 * First check the swap cache.  Since this is normally
		 * called after lookup_swap_cache() failed, re-calling
		 * that would confuse statistics.
		 */
		found_page = find_get_page(swapper_space, entry.val);
		if (found_page)
			break;

		/*
		 * Get a new page to read into from swap.
		 */
		if (!new_page) {
			new_page = alloc_page(GFP_KERNEL);
			if (!new_page)
				break; /* Out of memory */
		}

		/*
		 * call radix_tree_preload() while we can wait.
		 */
		err = radix_tree_preload(GFP_KERNEL);
		if (err)
			break;

		/*
		 * Swap entry may have been freed since our caller observed it.
		 */
		err = swapcache_prepare(entry);
		if (err == -EEXIST) { /* seems racy */
			radix_tree_preload_end();
			continue;
		}
		if (err) { /* swp entry is obsolete ? */
			radix_tree_preload_end();
			break;
		}

		/* May fail (-ENOMEM) if radix-tree node allocation failed. */
		__set_page_locked(new_page);
		SetPageSwapBacked(new_page);
		err = __add_to_swap_cache(new_page, entry);
		if (likely(!err)) {
			radix_tree_preload_end();
			lru_cache_add_anon(new_page);
			*retpage = new_page;
			return ZSWAP_SWAPCACHE_NEW;
		}
		radix_tree_preload_end();
		ClearPageSwapBacked(new_page);
		__clear_page_locked(new_page);
		/*
		 * add_to_swap_cache() doesn't return -EEXIST, so we can safely
		 * clear SWAP_HAS_CACHE flag.
		 */
		swapcache_free(entry, NULL);
	} while (err != -ENOMEM);

	if (new_page)
		page_cache_release(new_page);
	if (!found_page)
		return ZSWAP_SWAPCACHE_NOMEM;
	*retpage = found_page;
	return ZSWAP_SWAPCACHE_EXIST;
}

/*
 * Attempts to free and entry by adding a page to the swap cache,
 * decompressing the entry data into the page, and issuing a
 * bio write to write the page back to the swap device.
 *
 * This can be thought of as a "resumed writeback" of the page
 * to the swap device.  We are basically resuming the same swap
 * writeback path that was intercepted with the frontswap_store()
 * in the first place.  After the page has been decompressed into
 * the swap cache, the compressed version stored by zswap can be
 * freed.
 */
static int zswap_writeback_entry(struct zswap_tree *tree,
				struct zswap_entry *entry)
{
	unsigned long type = tree->type;
	struct page *page;
	swp_entry_t swpentry;
	u8 *src, *dst;
	unsigned int dlen;
	int ret;
	struct writeback_control wbc = {
		.sync_mode = WB_SYNC_NONE,
	};

	/* get/allocate page in the swap cache */
	swpentry = swp_entry(type, entry->offset);

	/* try to allocate swap cache page */
	switch (zswap_get_swap_cache_page(swpentry, &page)) {

	case ZSWAP_SWAPCACHE_NOMEM: /* no memory */
		return -ENOMEM;
		break; /* not reached */

	case ZSWAP_SWAPCACHE_EXIST: /* page is unlocked */
		/* page is already in the swap cache, ignore for now */
		return -EEXIST;
		break; /* not reached */

	case ZSWAP_SWAPCACHE_NEW: /* page is locked */
		/* decompress */
		dlen = PAGE_SIZE;
		src = zs_map_object(tree->pool, entry->handle, ZS_MM_RO);
		dst = kmap_atomic(page);
		ret = zswap_comp_op(ZSWAP_COMPOP_DECOMPRESS, src, entry->length,
				dst, &dlen);
		kunmap_atomic(dst);
		zs_unmap_object(tree->pool, entry->handle);
		BUG_ON(ret);
		BUG_ON(dlen != PAGE_SIZE);

		/* page is up to date */
		SetPageUptodate(page);
	}

	/* move it to the tail of the inactive list after end_writeback */
	SetPageReclaim(page);

	/* start writeback */
	SetPageReclaim(page);
	if (!__swap_writepage(page, &wbc, zswap_end_swap_write))
		atomic_inc(&zswap_outstanding_writebacks);
	page_cache_release(page);

	return 0;
}

/*
 * Attempts to free nr of entries via writeback to the swap device.
 * The number of entries that were actually freed is returned.
 */
static int zswap_writeback_entries(struct zswap_tree *tree, int nr)
{
	struct zswap_entry *entry;
	int i, ret, refcount, freed_nr = 0;

	for (i = 0; i < nr; i++) {
		/*
		 * This limits is arbitrary for now until a better
		 * policy can be implemented. This is so we don't
		 * eat all of RAM decompressing pages for writeback.
		 */
		if (atomic_read(&zswap_outstanding_writebacks) >
				ZSWAP_MAX_OUTSTANDING_FLUSHES)
			break;

		spin_lock(&tree->lock);

		/* dequeue from lru */
		if (list_empty(&tree->lru)) {
			spin_unlock(&tree->lock);
			break;
		}
		entry = list_first_entry(&tree->lru,
				struct zswap_entry, lru);
		list_del_init(&entry->lru);

		/* so invalidate doesn't free the entry from under us */
		zswap_entry_get(entry);

		spin_unlock(&tree->lock);

		/* attempt writeback */
		ret = zswap_writeback_entry(tree, entry);

		spin_lock(&tree->lock);

		/* drop reference from above */
		refcount = zswap_entry_put(entry);

		if (!ret)
			/* drop the initial reference from entry creation */
			refcount = zswap_entry_put(entry);

		/*
		 * There are four possible values for refcount here:
		 * (1) refcount is 2, writeback failed and load is in progress;
		 *     do nothing, load will add us back to the LRU
		 * (2) refcount is 1, writeback failed; do not free entry,
		 *     add back to LRU
		 * (3) refcount is 0, (normal case) not invalidate yet;
		 *     remove from rbtree and free entry
		 * (4) refcount is -1, invalidate happened during writeback;
		 *     free entry
		 */
		if (refcount == 1)
			list_add(&entry->lru, &tree->lru);

		if (refcount == 0) {
			/* no invalidate yet, remove from rbtree */
			rb_erase(&entry->rbnode, &tree->rbroot);
		}
		spin_unlock(&tree->lock);
		if (refcount <= 0) {
			/* free the entry */
			zswap_free_entry(tree, entry);
			freed_nr++;
		}
	}
	return freed_nr;
}
#endif /* CONFIG_ZSWAP_ENABLE_WRITEBACK */

/*******************************************
* page pool for temporary compression result
********************************************/
#define ZSWAP_TMPPAGE_POOL_PAGES 16
static LIST_HEAD(zswap_tmppage_list);
static DEFINE_SPINLOCK(zswap_tmppage_lock);

static void zswap_tmppage_pool_destroy(void)
{
	struct page *page, *tmppage;

	spin_lock(&zswap_tmppage_lock);
	list_for_each_entry_safe(page, tmppage, &zswap_tmppage_list, lru) {
		list_del(&page->lru);
		__free_pages(page, 1);
	}
	spin_unlock(&zswap_tmppage_lock);
}

static int zswap_tmppage_pool_create(void)
{
	int i;
	struct page *page;

	for (i = 0; i < ZSWAP_TMPPAGE_POOL_PAGES; i++) {
		page = alloc_pages(GFP_KERNEL, 1);
		if (!page) {
			zswap_tmppage_pool_destroy();
			return -ENOMEM;
		}
		spin_lock(&zswap_tmppage_lock);
		list_add(&page->lru, &zswap_tmppage_list);
		spin_unlock(&zswap_tmppage_lock);
	}
	return 0;
}

static inline struct page *zswap_tmppage_alloc(void)
{
	struct page *page;

	spin_lock(&zswap_tmppage_lock);
	if (list_empty(&zswap_tmppage_list)) {
		spin_unlock(&zswap_tmppage_lock);
		return NULL;
	}
	page = list_first_entry(&zswap_tmppage_list, struct page, lru);
	list_del(&page->lru);
	spin_unlock(&zswap_tmppage_lock);
	return page;
}

static inline void zswap_tmppage_free(struct page *page)
{
	spin_lock(&zswap_tmppage_lock);
	list_add(&page->lru, &zswap_tmppage_list);
	spin_unlock(&zswap_tmppage_lock);
}

/*********************************
* frontswap hooks
**********************************/
/* attempts to compress and store an single page */
static int zswap_frontswap_store(unsigned type, pgoff_t offset,
				struct page *page)
{
	struct zswap_tree *tree = zswap_trees[type];
	struct zswap_entry *entry, *dupentry;
	int ret;
	unsigned int dlen = PAGE_SIZE;
	unsigned long handle;
	char *buf;
	u8 *src, *dst;
	struct page *tmppage;
	bool writeback_attempted = 0;
#ifdef CONFIG_ZSWAP_ENABLE_WRITEBACK
	u8 *tmpdst;
#endif

	if (!tree) {
		ret = -ENODEV;
		goto reject;
	}

	/* allocate entry */
	entry = zswap_entry_cache_alloc(GFP_KERNEL);
	if (!entry) {
		zswap_reject_kmemcache_fail++;
		ret = -ENOMEM;
		goto reject;
	}

	/* compress */
	dst = get_cpu_var(zswap_dstmem);
	src = kmap_atomic(page);
	ret = zswap_comp_op(ZSWAP_COMPOP_COMPRESS, src, PAGE_SIZE, dst, &dlen);
	kunmap_atomic(src);
	if (ret) {
		ret = -EINVAL;
		goto freepage;
	}
	if ((dlen * 100 / PAGE_SIZE) > zswap_max_compression_ratio) {
		zswap_reject_compress_poor++;
		ret = -E2BIG;
		goto freepage;
	}

	/* store */
	handle = zs_malloc(tree->pool, dlen);
	if (!handle) {
#ifdef CONFIG_ZSWAP_ENABLE_WRITEBACK
		zswap_writeback_attempted++;
		/*
		 * Copy compressed buffer out of per-cpu storage so
		 * we can re-enable preemption.
		*/
		tmppage = zswap_tmppage_alloc();
		if (!tmppage) {
			zswap_reject_tmppage_fail++;
			ret = -ENOMEM;
			goto freepage;
		}
		writeback_attempted = 1;
		tmpdst = page_address(tmppage);
		memcpy(tmpdst, dst, dlen);
		dst = tmpdst;
		put_cpu_var(zswap_dstmem);

		/* try to free up some space */
		/* TODO: replace with more targeted policy */
		zswap_writeback_entries(tree, 16);
		/* try again, allowing wait */
		handle = zs_malloc(tree->pool, dlen);
		if (!handle) {
			/* still no space, fail */
			zswap_reject_zsmalloc_fail++;
			ret = -ENOMEM;
			goto freepage;
		}
		zswap_saved_by_writeback++;
#else
		ret = -ENOMEM;
		goto freepage;
#endif
	}

	buf = zs_map_object(tree->pool, handle, ZS_MM_WO);
	memcpy(buf, dst, dlen);
	zs_unmap_object(tree->pool, handle);
	if (writeback_attempted)
		zswap_tmppage_free(tmppage);
	else
		put_cpu_var(zswap_dstmem);

	/* populate entry */
	entry->offset = offset;
	entry->handle = handle;
	entry->length = dlen;

	/* map */
	spin_lock(&tree->lock);
	do {
		ret = zswap_rb_insert(&tree->rbroot, entry, &dupentry);
		if (ret == -EEXIST) {
			zswap_duplicate_entry++;
			/* remove from rbtree and lru */
			rb_erase(&dupentry->rbnode, &tree->rbroot);
			if (!list_empty(&dupentry->lru))
				list_del_init(&dupentry->lru);
			if (!zswap_entry_put(dupentry)) {
				/* free */
				zswap_free_entry(tree, dupentry);
			}
		}
	} while (ret == -EEXIST);
	list_add_tail(&entry->lru, &tree->lru);
	spin_unlock(&tree->lock);

	/* update stats */
	atomic_inc(&zswap_stored_pages);

	/* Debugging code for zswap kernel panic */
	{
	/* check whether page is file page */
		if (!PageAnon(page) && !PageSwapCache(page)) {
			struct address_space *mapping = page_file_mapping(page);
			printk(KERN_ALERT
				"BUG: file page is swapped out (mapping = %p)\n", mapping);
		}
	}

	return 0;

freepage:
	if (writeback_attempted)
		zswap_tmppage_free(tmppage);
	else
		put_cpu_var(zswap_dstmem);
	zswap_entry_cache_free(entry);
reject:
	return ret;
}

/*
 * returns 0 if the page was successfully decompressed
 * return -1 on entry not found or error
*/
static int zswap_frontswap_load(unsigned type, pgoff_t offset,
				struct page *page)
{
	struct zswap_tree *tree = zswap_trees[type];
	struct zswap_entry *entry;
	u8 *src, *dst;
	unsigned int dlen;
	int refcount;

	/* find */
	spin_lock(&tree->lock);
	entry = zswap_rb_search(&tree->rbroot, offset);
	if (!entry) {
		/* entry was written back */
		spin_unlock(&tree->lock);
		return -1;
	}
	zswap_entry_get(entry);

	/* remove from lru */
	if (!list_empty(&entry->lru))
		list_del_init(&entry->lru);
	spin_unlock(&tree->lock);

	/* decompress */
	dlen = PAGE_SIZE;
	src = zs_map_object(tree->pool, entry->handle, ZS_MM_RO);
	dst = kmap_atomic(page);
	zswap_comp_op(ZSWAP_COMPOP_DECOMPRESS, src, entry->length,
		dst, &dlen);
	kunmap_atomic(dst);
	zs_unmap_object(tree->pool, entry->handle);

	spin_lock(&tree->lock);
	refcount = zswap_entry_put(entry);
	if (likely(refcount)) {
		list_add_tail(&entry->lru, &tree->lru);
		spin_unlock(&tree->lock);
		return 0;
	}
	spin_unlock(&tree->lock);

	/*
	 * We don't have to unlink from the rbtree because
	 * zswap_writeback_entry() or zswap_frontswap_invalidate page()
	 * has already done this for us if we are the last reference.
	 */
	/* free */

	zswap_free_entry(tree, entry);

	return 0;
}

/* invalidates a single page */
static void zswap_frontswap_invalidate_page(unsigned type, pgoff_t offset)
{
	struct zswap_tree *tree = zswap_trees[type];
	struct zswap_entry *entry;
	int refcount;

	/* find */
	spin_lock(&tree->lock);
	entry = zswap_rb_search(&tree->rbroot, offset);
	if (!entry) {
		/* entry was written back */
		spin_unlock(&tree->lock);
		return;
	}

	/* remove from rbtree and lru */
	rb_erase(&entry->rbnode, &tree->rbroot);
	if (!list_empty(&entry->lru))
		list_del_init(&entry->lru);

	/* drop the initial reference from entry creation */
	refcount = zswap_entry_put(entry);

	spin_unlock(&tree->lock);

	if (refcount) {
		/* writeback in progress, writeback will free */
		return;
	}

	/* free */
	zswap_free_entry(tree, entry);
}

/* invalidates all pages for the given swap type */
static void zswap_frontswap_invalidate_area(unsigned type)
{
	struct zswap_tree *tree = zswap_trees[type];
	struct rb_node *node;
	struct zswap_entry *entry;

	if (!tree)
		return;

	/* walk the tree and free everything */
	spin_lock(&tree->lock);
	/*
	 * TODO: Even though this code should not be executed because
	 * the try_to_unuse() in swapoff should have emptied the tree,
	 * it is very wasteful to rebalance the tree after every
	 * removal when we are freeing the whole tree.
	 *
	 * If post-order traversal code is ever added to the rbtree
	 * implementation, it should be used here.
	 */
	while ((node = rb_first(&tree->rbroot))) {
		entry = rb_entry(node, struct zswap_entry, rbnode);
		rb_erase(&entry->rbnode, &tree->rbroot);
		zs_free(tree->pool, entry->handle);
		zswap_entry_cache_free(entry);
		atomic_dec(&zswap_stored_pages);
	}
	tree->rbroot = RB_ROOT;
	INIT_LIST_HEAD(&tree->lru);
	spin_unlock(&tree->lock);
}

/* NOTE: this is called in atomic context from swapon and must not sleep */
static void zswap_frontswap_init(unsigned type)
{
	struct zswap_tree *tree;

	tree = kzalloc(sizeof(struct zswap_tree), GFP_ATOMIC);
	if (!tree)
		goto err;
	tree->pool = zs_create_pool(GFP_NOWAIT | __GFP_HIGHMEM);
	if (!tree->pool)
		goto freetree;
	tree->rbroot = RB_ROOT;
	INIT_LIST_HEAD(&tree->lru);
	spin_lock_init(&tree->lock);
	tree->type = type;
	zswap_trees[type] = tree;
	return;

freetree:
	kfree(tree);
err:
	pr_err("alloc failed, zswap disabled for swap type %d\n", type);
}

static struct frontswap_ops zswap_frontswap_ops = {
	.store = zswap_frontswap_store,
	.load = zswap_frontswap_load,
	.invalidate_page = zswap_frontswap_invalidate_page,
	.invalidate_area = zswap_frontswap_invalidate_area,
	.init = zswap_frontswap_init
};

/*********************************
* debugfs functions
**********************************/
#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>

static struct dentry *zswap_debugfs_root;

static int __init zswap_debugfs_init(void)
{
	if (!debugfs_initialized())
		return -ENODEV;

	zswap_debugfs_root = debugfs_create_dir("zswap", NULL);
	if (!zswap_debugfs_root)
		return -ENOMEM;

	debugfs_create_u64("saved_by_writeback", S_IRUGO,
			zswap_debugfs_root, &zswap_saved_by_writeback);
	debugfs_create_u64("pool_limit_hit", S_IRUGO,
			zswap_debugfs_root, &zswap_pool_limit_hit);
	debugfs_create_u64("reject_writeback_attempted", S_IRUGO,
			zswap_debugfs_root, &zswap_writeback_attempted);
	debugfs_create_u64("reject_tmppage_fail", S_IRUGO,
			zswap_debugfs_root, &zswap_reject_tmppage_fail);
	debugfs_create_u64("reject_zsmalloc_fail", S_IRUGO,
			zswap_debugfs_root, &zswap_reject_zsmalloc_fail);
	debugfs_create_u64("reject_kmemcache_fail", S_IRUGO,
			zswap_debugfs_root, &zswap_reject_kmemcache_fail);
	debugfs_create_u64("reject_compress_poor", S_IRUGO,
			zswap_debugfs_root, &zswap_reject_compress_poor);
	debugfs_create_u64("written_back_pages", S_IRUGO,
			zswap_debugfs_root, &zswap_written_back_pages);
	debugfs_create_u64("duplicate_entry", S_IRUGO,
			zswap_debugfs_root, &zswap_duplicate_entry);
	debugfs_create_atomic_t("pool_pages", S_IRUGO,
			zswap_debugfs_root, &zswap_pool_pages);
	debugfs_create_atomic_t("stored_pages", S_IRUGO,
			zswap_debugfs_root, &zswap_stored_pages);
#ifdef CONFIG_ZSWAP_ENABLE_WRITEBACK
	debugfs_create_atomic_t("outstanding_writebacks", S_IRUGO,
			zswap_debugfs_root, &zswap_outstanding_writebacks);
#endif
	return 0;
}

static void __exit zswap_debugfs_exit(void)
{
	debugfs_remove_recursive(zswap_debugfs_root);
}
#else
static inline int __init zswap_debugfs_init(void)
{
	return 0;
}

static inline void __exit zswap_debugfs_exit(void) { }
#endif

/*********************************
* module init and exit
**********************************/
static int __init init_zswap(void)
{
	if (!zswap_enabled)
		return 0;

	pr_info("loading zswap\n");
	if (zswap_entry_cache_create()) {
		pr_err("entry cache creation failed\n");
		goto error;
	}
	if (zswap_page_pool_create()) {
		pr_err("page pool initialization failed\n");
		goto pagepoolfail;
	}
	if (zswap_tmppage_pool_create()) {
		pr_err("workmem pool initialization failed\n");
		goto tmppoolfail;
	}
	if (zswap_comp_init()) {
		pr_err("compressor initialization failed\n");
		goto compfail;
	}
	if (zswap_cpu_init()) {
		pr_err("per-cpu initialization failed\n");
		goto pcpufail;
	}
	frontswap_register_ops(&zswap_frontswap_ops);
	if (zswap_debugfs_init())
		pr_warn("debugfs initialization failed\n");
	return 0;
pcpufail:
	zswap_comp_exit();
compfail:
	zswap_tmppage_pool_destroy();
tmppoolfail:
	zswap_page_pool_destroy();
pagepoolfail:
	zswap_entry_cache_destory();
error:
	return -ENOMEM;
}
/* must be late so crypto has time to come up */
late_initcall(init_zswap);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Seth Jennings <sjenning@linux.vnet.ibm.com>");
MODULE_DESCRIPTION("Compressed cache for swap pages");
