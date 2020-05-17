#ifndef YAZ_PROJECT4_CACHE_H
#define YAZ_PROJECT4_CACHE_H

#include "devices/block.h"
#include "devices/timer.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include <list.h>

#define MAX_CACHE_SECTORS 64 // cache is no greater than 64 sectors in size
#define WRITE_CACHE_FREQ TIMER_FREQ

struct cache *fs_cache;  /* Cache for the file system. */
struct block *fs_device; /* Device for the file system. */

/**
 * A cache of file blocks. Formed by a list of cache entries.
 */
struct cache {
  struct list cache_entry_list; /* A list of cache_entry. */
  uint32_t cache_size;          /* Size of cache in sectors. */
};

/**
 * A cache entry, one cache block corresponds to one sector on disk.
 */
struct cache_entry {
  struct list_elem elem;           /* Element in cache list. */
  uint8_t data[BLOCK_SECTOR_SIZE]; /* Data cached from disk. */
  block_sector_t sector;           /* Sector number on disk. */
  bool dirty;                      /* Whether data has been written. */
  bool accessed;                   /* Whether data has been accessed. */
  int open_cnt; /* Number of openers of this cache entry / sector. */
  struct lock cache_entry_lock; /* Lock for cache entry. */
};

/**
 * Initialize fs_cache for file system.
 */
void cache_init(void);

/**
 * Creates a cache for the file system and initialize its fields.
 * @return a cache with all fields initialized.
 */
struct cache *create_cache(void);

/**
 * Write cache to disk and free all memory allocated for fs_cache.
 */
void cache_done(void);

/**
 * Given a sector number on the disk, find corresponding entry in cache.
 * @param sector - sector number on the disk.
 * @return the cache entry that corresponds to this sector.
 */
struct cache_entry *find_cache_entry_by_sector(block_sector_t sector);

/**
 * Given a sector number on the disk and a dirty boolean indicating whether it's
 * been modified, find the cache entry on buffer cache. If not on cache, assign
 * a new cache entry for it.
 * @param sector - sector number on the disk.
 * @param dirty - whether data has been modified.
 * @return a cache entry on cache that corresponds to this disk data.
 */
struct cache_entry *get_cache_entry(block_sector_t sector, bool dirty);

/**
 * Given a sector number on the disk and a dirty boolean indicating whether it's
 * been modified, return a cache entry that is assigned to it.
 * @param sector - sector number on the disk.
 * @param dirty - whether data has been modified.
 * @return a cache entry that is assigned for this sector of data.
 */
struct cache_entry *assign_cache_entry(block_sector_t sector, bool dirty);

/**
 * Evict a cache entry according to clock algorithm.
 * If
 * @return a cache entry that has been evicted.
 */
struct cache_entry *evict_cache_entry(void);

/**
 * Write all content on cache to disk.
 */
void write_cache_to_disk(void);

/**
 * Given a pointer to a sector on disk, read content from disk to cache.
 * @param sector_ptr - a pointer to a sector on disk.
 */
void read_sector_to_cache(void *sector_ptr);

/**
 * Given a sector number that is currently being read, create a thread that
 * reads next sector to cache.
 * @param sector - sector number that is currently being read.
 */
void read_ahead(block_sector_t sector);

/**
 * Periodically write content from cache to disk.
 */
void periodic_write(void);

#endif // YAZ_PROJECT4_CACHE_H
