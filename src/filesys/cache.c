#include "cache.h"

void cache_init(void) {
  fs_cache = create_cache();
  if (!fs_cache) {
    PANIC("No memory for cache!\n");
  }
}

struct cache *create_cache(void) {
  struct cache *ret;
  ret = malloc(sizeof(struct cache));
  if (!ret) {
    return NULL;
  }
  list_init(&(ret->cache_entry_list));
  ret->cache_size = 0;
  return ret;
}

void cache_done(void) {
  write_cache_to_disk(); // write cache to disk
  // free memory for cache
  struct list_elem *cur;
  struct list_elem *next;
  struct cache_entry *entry;
  cur = list_begin(&(fs_cache->cache_entry_list));
  // free allocated memory for each cache entry
  while (cur != list_end(&(fs_cache->cache_entry_list))) {
    next = list_next(cur);
    entry = list_entry(cur, struct cache_entry, elem);
    list_remove(&(entry->elem)); // remove entry from cache entry list
    free(entry);                 // free entry
    cur = next;
  }
  free(fs_cache);
}

struct cache_entry *find_cache_entry_by_sector(block_sector_t sector) {
  struct cache_entry *ret;
  struct list_elem *e; // for traversing list
  for (e = list_begin(&(fs_cache->cache_entry_list));
       e != list_end(&(fs_cache->cache_entry_list)); e = list_next(e)) {
    ret = list_entry(e, struct cache_entry, elem); // extract cache entry
    lock_acquire(&(ret->cache_entry_lock)); // add lock to this cache entry
    if (ret->sector == sector) {
      // found the cache entry that corresponds to target sector
      return ret; // return without releasing lock b/c we need it for further
                  // use
    }
    lock_release(&(ret->cache_entry_lock)); // release lock for this cache entry
                                            // since we don't need it
  }
  // not found
  return NULL;
}

struct cache_entry *get_cache_entry(block_sector_t sector, bool dirty) {
  struct cache_entry *ret = find_cache_entry_by_sector(sector);
  // if it's already on cache
  if (ret) {
    ret->accessed = true;
    ret->dirty = ret->dirty || dirty;
    ret->open_cnt++;
    return ret; // returned with lock
  }
  // if not on cache, assign a cache entry
  ret = assign_cache_entry(sector, dirty); // it holds a lock
  if (!ret) {
    PANIC("No memory for cache!\n");
  }
  return ret; // returned with lock
}

struct cache_entry *assign_cache_entry(block_sector_t sector, bool dirty) {
  struct cache_entry *ret;
  // first find the pointer to cache entry
  if (fs_cache->cache_size < MAX_CACHE_SECTORS) {
    // if cache has not hit max limit, assign new space for new cache entry
    ret = malloc(sizeof(struct cache_entry));
    if (!ret) {
      PANIC("No memory for cache!\n");
      return NULL;
    }
    lock_init(
        &(ret->cache_entry_lock)); // initialize the lock for this cache entry
    lock_acquire(&(ret->cache_entry_lock)); // add lock to this cache entry b/c
                                            // we need to use it later
    list_push_back(&(fs_cache->cache_entry_list),
                   &(ret->elem)); // add new cache entry to cache list
  } else {
    // if cache has hit max limit, apply clock algorithm to evict an entry
    ret = evict_cache_entry(); // it holds a cache entry lock
  }
  // set values for attributes and read data from disk to cache entry
  block_read(fs_device, sector,
             ret->data); // read from sector on disk to cache entry
  ret->sector = sector;
  ret->accessed = true;
  ret->dirty = dirty;
  ret->open_cnt = 1;
  return ret; // returned with lock
}

struct cache_entry *evict_cache_entry(void) {
  struct cache_entry *ret;
  struct list_elem *e; // for traversing
  while (true) {
    for (e = list_begin(&(fs_cache->cache_entry_list));
         e != list_end(&(fs_cache->cache_entry_list)); e = list_next(e)) {
      ret = list_entry(e, struct cache_entry, elem);
      lock_acquire(&(
          ret->cache_entry_lock)); // lock this cache entry to check attributes
      if (ret->open_cnt <= 0) {
        // can only evict when this sector is not opened by any threads
        if (ret->accessed) {
          // if recently accessed, update accessed boolean to false
          ret->accessed = false;
        } else {
          // if not accessed, prepare to evict
          if (ret->dirty) {
            // if modified, write to disk before evict
            block_write(fs_device, ret->sector, &(ret->data));
          }
          return ret; // return w/o releasing lock b/c we still need it
        }
      }
      lock_release(&(ret->cache_entry_lock)); // release lock before we check
                                              // for the next cache entry
    }
  }
}

void write_cache_to_disk(void) {
  struct list_elem *cur;
  struct list_elem *next;
  struct cache_entry *cache_entry;
  cur = list_begin(&(fs_cache->cache_entry_list));
  while (cur != list_end(&(fs_cache->cache_entry_list))) {
    next = list_next(cur);
    cache_entry = list_entry(cur, struct cache_entry, elem);
    lock_acquire(&(cache_entry->cache_entry_lock));
    if (cache_entry->dirty) {
      block_write(fs_device, cache_entry->sector, &(cache_entry->data));
      cache_entry->dirty = false;
    }
    cache_entry->open_cnt--; // done accessing this cache block
    lock_release(&(cache_entry->cache_entry_lock));
    cur = next;
  }
}

void read_sector_to_cache(void *sector_ptr) {
  block_sector_t sector =
      *((block_sector_t *)sector_ptr); // sector to read from
  struct cache_entry *entry = get_cache_entry(
      sector, false); // check if already on cache, if yes it holds the lock
  if (entry) {
    // if successfully read next sector to cache
    entry->open_cnt--; // done accessing this cache block
    lock_release(&(entry->cache_entry_lock));
  }
}

void read_ahead(block_sector_t sector) {
  block_sector_t *sector_ptr = malloc(sizeof(block_sector_t));
  if (sector_ptr) {
    *sector_ptr = sector + 1; // read next sector
    thread_create("read_ahead_to_cache", PRI_MIN, read_sector_to_cache,
                  sector_ptr);
    free(sector_ptr);
  }
}

void periodic_write(void) {
  while (true) {
    timer_sleep(WRITE_CACHE_FREQ);
    write_cache_to_disk();
  }
}