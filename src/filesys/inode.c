#include "filesys/inode.h"
#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include <debug.h>
#include <list.h>
#include <round.h>
#include <string.h>

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define NUM_DIRECT_BLOCKS 122 /* Number of direct blocks in a sector. */
#define NUM_INDIRECT_BLOCK_POINTERS                                            \
  128 /* Number of blocks in an indirect inode block sector. */

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk {
  block_sector_t direct_blocks[NUM_DIRECT_BLOCKS]; /* 122 direct pointers. */
  block_sector_t indirect_block;        /* 1 single indirect pointer. */
  block_sector_t double_indirect_block; /* 1 double indirect pointer. */

  bool is_dir;           /* True for directory, false for file */
  off_t length;          /* File size in bytes. */
  unsigned magic;        /* Magic number. */
  block_sector_t parent; /* The parent of this inode */
};

/* Indirect inode. */
struct indirect_inode_block_sector {
  block_sector_t blocks[NUM_INDIRECT_BLOCK_POINTERS]; /* 128 pointers. */
};

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t bytes_to_sectors(off_t size) {
  return DIV_ROUND_UP(size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode {
  struct list_elem elem;  /* Element in inode list. */
  block_sector_t sector;  /* Sector number of disk location. */
  int open_cnt;           /* Number of openers. */
  bool removed;           /* True if deleted, false otherwise. */
  int deny_write_cnt;     /* 0: writes ok, >0: deny writes. */
  struct inode_disk data; /* Inode content. */
  struct lock lock;       /* Inode lock. */
  off_t readable_length;  /* Readable file size. */
  block_sector_t parent;  /* Sector number of parent location*/
  bool is_dir; /* true: this inode is a dir, false: this inode is a file*/
};

/**
 * Read data from sector and copy to memory.
 * @param sector - the sector to read data.
 * @param dest - pointer already allocated where data will be copied to.
 */
static void cache_read_sector_and_copy_to_memory(block_sector_t sector,
                                                 void *dest) {
  struct cache_entry *entry = get_cache_entry(
      sector, false); // check if already on cache, if yes it holds the lock
  if (entry) {
    memcpy(dest, entry->data, BLOCK_SECTOR_SIZE);

    // if successfully read next sector to cache
    entry->open_cnt--; // done accessing this cache block
    lock_release(&(entry->cache_entry_lock));
  }
}

/**
 * Write data from memory to sector.
 * @param sector - the sector to write data to.
 * @param source - pointer containing data that will be written.
 */
static void cache_write_sector_and_copy_from_memory(block_sector_t sector,
                                                    const void *source) {
  struct cache_entry *entry = get_cache_entry(
      sector, false); // check if already on cache, if yes it holds the lock
  if (entry) {
    memcpy(entry->data, source, BLOCK_SECTOR_SIZE);

    // if successfully read next sector to cache
    entry->open_cnt--; // done accessing this cache block
    lock_release(&(entry->cache_entry_lock));
  }
}

/* Returns the block device sector that contains INDEX of INODE_DISK.
   Returns -1 if INODE_DISK does not contain INDEX. */
static block_sector_t index_to_sector(const struct inode_disk *inode_disk,
                                      off_t index) {
  block_sector_t sector_to_return;

  // Direct blocks
  off_t index_base = 0;
  off_t index_limit = NUM_DIRECT_BLOCKS;
  if (index < index_limit) {
    return inode_disk->direct_blocks[index];
  }

  // Indirect block
  index_base = index_limit;
  index_limit += NUM_INDIRECT_BLOCK_POINTERS;
  if (index < index_limit) {
    struct indirect_inode_block_sector *indirect_inode_disk =
        calloc(1, sizeof(struct indirect_inode_block_sector));
    cache_read_sector_and_copy_to_memory(inode_disk->indirect_block,
                                         indirect_inode_disk);
    sector_to_return = indirect_inode_disk->blocks[index - index_base];
    free(indirect_inode_disk);
    return sector_to_return;
  }

  // Double indirect block
  index_base = index_limit;
  index_limit += NUM_INDIRECT_BLOCK_POINTERS * NUM_INDIRECT_BLOCK_POINTERS;
  if (index < index_limit) {
    off_t index_first_level =
        (index - index_base) / NUM_INDIRECT_BLOCK_POINTERS;
    off_t index_second_level =
        (index - index_base) % NUM_INDIRECT_BLOCK_POINTERS;

    struct indirect_inode_block_sector *indirect_inode_disk =
        calloc(1, sizeof(struct indirect_inode_block_sector));
    cache_read_sector_and_copy_to_memory(inode_disk->double_indirect_block,
                                         indirect_inode_disk);
    cache_read_sector_and_copy_to_memory(
        indirect_inode_disk->blocks[index_first_level], indirect_inode_disk);
    sector_to_return = indirect_inode_disk->blocks[index_second_level];
    free(indirect_inode_disk);
    return sector_to_return;
  }

  return -1;
}

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t byte_to_sector(const struct inode *inode, off_t pos) {
  ASSERT(inode != NULL);

  block_sector_t ret;

  bool lock_held = lock_held_by_current_thread(&(inode->lock));
  if (!lock_held) {
    lock_acquire((struct lock *)&inode->lock);
  }

  if (0 <= pos && pos < inode->data.length) {
    off_t index = pos / BLOCK_SECTOR_SIZE;
    ret = index_to_sector(&(inode->data), index);
  } else {
    ret = -1;
  }

  if (!lock_held) {
    lock_release((struct lock *)&(inode->lock));
  }

  return ret;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/**
 * Helper method for inode_allocate() to support indirect inodes.
 * @param sectorp - pointer to sector for allocation.
 * @param num_sectors_remaining - number of sectors for allocation.
 * @param inode_level - 0 for direct, 1 for indirect, 2 for double indirect.
 * @return true if succesful, false otherwise.
 */
static bool inode_allocate_indirect(block_sector_t *sectorp,
                                    size_t num_sectors_remaining,
                                    int inode_level) {
  static char zeros[BLOCK_SECTOR_SIZE];

  ASSERT(inode_level <=
         2); // Only upto double indirect inodes, which is level 2

  if (inode_level == 0) {
    if (*sectorp == 0) {
      if (!free_map_allocate(1, sectorp)) {
        return false;
      }
      cache_write_sector_and_copy_from_memory(*sectorp, &zeros);
    }
    return true;
  }

  struct indirect_inode_block_sector indirect_inode_block;
  if (*sectorp == 0) {
    if (!free_map_allocate(1, sectorp)) {
      return false;
    }
    cache_write_sector_and_copy_from_memory(*sectorp, &zeros);
  }
  cache_read_sector_and_copy_to_memory(*sectorp, &indirect_inode_block);

  if (inode_level == 1) {
    size_t num_sectors = DIV_ROUND_UP(num_sectors_remaining, 1);
    size_t i = 0;
    for (i = 0; i < num_sectors; i++) {
      size_t num_subsectors =
          1 < num_sectors_remaining ? 1 : num_sectors_remaining;
      if (!inode_allocate_indirect(&(indirect_inode_block.blocks[i]),
                                   num_subsectors, inode_level - 1)) {
        return false;
      }
      num_sectors_remaining -= num_subsectors;
    }
  } else { // inode level 2
    size_t num_sectors =
        DIV_ROUND_UP(num_sectors_remaining, NUM_INDIRECT_BLOCK_POINTERS);
    size_t i = 0;
    for (i = 0; i < num_sectors; i++) {
      size_t num_subsectors =
          NUM_INDIRECT_BLOCK_POINTERS < num_sectors_remaining
              ? NUM_INDIRECT_BLOCK_POINTERS
              : num_sectors_remaining;
      if (!inode_allocate_indirect(&(indirect_inode_block.blocks[i]),
                                   num_subsectors, inode_level - 1)) {
        return false;
      }
      num_sectors_remaining -= num_subsectors;
    }
  }

  ASSERT(num_sectors_remaining == 0);
  cache_write_sector_and_copy_from_memory(*sectorp, &indirect_inode_block);
  return true;
}

/**
 * Allocate memory for disk inode.
 * @param disk_inode - the disk inode.
 * @return true if successful, false otherwise.
 */
static bool inode_allocate(struct inode_disk *disk_inode) {
  static char zeros[BLOCK_SECTOR_SIZE];

  if (disk_inode->length < 0) {
    return false;
  }

  size_t num_sectors_remaining = bytes_to_sectors(disk_inode->length);

  // Direct blocks
  size_t num_sectors = NUM_DIRECT_BLOCKS < num_sectors_remaining
                           ? NUM_DIRECT_BLOCKS
                           : num_sectors_remaining;
  size_t i = 0;
  for (i = 0; i < num_sectors; i++) {
    if (disk_inode->direct_blocks[i] == 0) {
      if (!free_map_allocate(1, &(disk_inode->direct_blocks[i]))) {
        return false;
      }
      cache_write_sector_and_copy_from_memory(disk_inode->direct_blocks[i],
                                              &zeros);
    }
  }
  num_sectors_remaining -= num_sectors;
  if (num_sectors_remaining == 0) {
    return true;
  }

  // Indirect block
  num_sectors = NUM_INDIRECT_BLOCK_POINTERS < num_sectors_remaining
                    ? NUM_INDIRECT_BLOCK_POINTERS
                    : num_sectors_remaining;
  if (!inode_allocate_indirect(&(disk_inode->indirect_block), num_sectors, 1)) {
    return false;
  }
  num_sectors_remaining -= num_sectors;
  if (num_sectors_remaining == 0) {
    return true;
  }

  // Double indirect block
  num_sectors = NUM_INDIRECT_BLOCK_POINTERS * NUM_INDIRECT_BLOCK_POINTERS <
                        num_sectors_remaining
                    ? NUM_INDIRECT_BLOCK_POINTERS * NUM_INDIRECT_BLOCK_POINTERS
                    : num_sectors_remaining;
  if (!inode_allocate_indirect(&(disk_inode->double_indirect_block),
                               num_sectors, 2)) {
    return false;
  }
  num_sectors_remaining -= num_sectors;
  if (num_sectors_remaining == 0) {
    return true;
  }

  ASSERT(num_sectors_remaining == 0);
  return false;
}

/**
 * Helper method for inode_release() to support indirect inodes.
 * @param sector - sector to be made available.
 * @param num_sectors_remaining - number of sectors to be made available.
 * @param inode_level - 0 for direct, 1 for indirect, 2 for double indirect.
 */
static void inode_release_indirect(block_sector_t sector,
                                   size_t num_sectors_remaining,
                                   int inode_level) {
  ASSERT(inode_level <=
         2); // Only upto double indirect inodes, which is level 2

  if (inode_level == 0) {
    free_map_release(sector, 1);
    return;
  }

  struct indirect_inode_block_sector indirect_inode_block;
  cache_read_sector_and_copy_to_memory(sector, &indirect_inode_block);

  if (inode_level == 1) {
    size_t num_sectors = DIV_ROUND_UP(num_sectors_remaining, 1);
    size_t i = 0;
    for (i = 0; i < num_sectors; i++) {
      size_t num_subsectors =
          1 < num_sectors_remaining ? 1 : num_sectors_remaining;
      inode_release_indirect(indirect_inode_block.blocks[i], num_subsectors,
                             inode_level - 1);
      num_sectors_remaining -= num_subsectors;
    }
  } else { // inode level 2
    size_t num_sectors =
        DIV_ROUND_UP(num_sectors_remaining, NUM_INDIRECT_BLOCK_POINTERS);
    size_t i = 0;
    for (i = 0; i < num_sectors; i++) {
      size_t num_subsectors =
          NUM_INDIRECT_BLOCK_POINTERS < num_sectors_remaining
              ? NUM_INDIRECT_BLOCK_POINTERS
              : num_sectors_remaining;
      inode_release_indirect(indirect_inode_block.blocks[i], num_subsectors,
                             inode_level - 1);
      num_sectors_remaining -= num_subsectors;
    }
  }

  ASSERT(num_sectors_remaining == 0);
  free_map_release(sector, 1);
}

/**
 * Free inode memory.
 * @param inode - the inode.
 * @return true if successful, false otherwise.
 */
static bool inode_release(struct inode *inode) {
  if (inode->data.length < 0) {
    return false;
  }

  size_t num_sectors_remaining = bytes_to_sectors(inode->data.length);

  // Direct blocks
  size_t num_sectors = NUM_DIRECT_BLOCKS < num_sectors_remaining
                           ? NUM_DIRECT_BLOCKS
                           : num_sectors_remaining;
  size_t i = 0;
  for (i = 0; i < num_sectors; i++) {
    free_map_release(inode->data.direct_blocks[i], 1);
  }
  num_sectors_remaining -= num_sectors;
  if (num_sectors_remaining == 0) {
    return true;
  }

  // Indirect block
  num_sectors = NUM_INDIRECT_BLOCK_POINTERS < num_sectors_remaining
                    ? NUM_INDIRECT_BLOCK_POINTERS
                    : num_sectors_remaining;
  inode_release_indirect(inode->data.indirect_block, num_sectors, 1);
  num_sectors_remaining -= num_sectors;
  if (num_sectors_remaining == 0) {
    return true;
  }

  // Double indirect block
  num_sectors = NUM_INDIRECT_BLOCK_POINTERS * NUM_INDIRECT_BLOCK_POINTERS <
                        num_sectors_remaining
                    ? NUM_INDIRECT_BLOCK_POINTERS * NUM_INDIRECT_BLOCK_POINTERS
                    : num_sectors_remaining;
  inode_release_indirect(inode->data.double_indirect_block, num_sectors, 2);
  num_sectors_remaining -= num_sectors;
  if (num_sectors_remaining == 0) {
    return true;
  }

  ASSERT(num_sectors_remaining == 0);
  return false;
}

/* Initializes the inode module. */
void inode_init(void) { list_init(&open_inodes); }

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool inode_create(block_sector_t sector, off_t length, bool is_dir) {
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT(length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT(sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc(1, sizeof *disk_inode);
  if (disk_inode != NULL) {
    disk_inode->length = length;
    disk_inode->magic = INODE_MAGIC;
    disk_inode->is_dir = is_dir;
    if (inode_allocate(disk_inode)) {
      cache_write_sector_and_copy_from_memory(sector, disk_inode);
      success = true;
    }
    free(disk_inode);
  }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *inode_open(block_sector_t sector) {
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin(&open_inodes); e != list_end(&open_inodes);
       e = list_next(e)) {
    inode = list_entry(e, struct inode, elem);
    if (inode->sector == sector) {
      inode_reopen(inode);
      return inode;
    }
  }

  /* Allocate memory. */
  inode = malloc(sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front(&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_init(&(inode->lock));
  cache_read_sector_and_copy_to_memory(inode->sector, &(inode->data));
  inode->readable_length = inode->data.length;
  inode->parent = inode->data.parent;
  inode->is_dir = inode->data.is_dir;
  return inode;
}

/* Reopens and returns INODE. */
struct inode *inode_reopen(struct inode *inode) {
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t inode_get_inumber(const struct inode *inode) {
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void inode_close(struct inode *inode) {
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  inode->open_cnt--;
  if (inode->open_cnt == 0) {
    /* Remove from inode list and release lock. */
    list_remove(&inode->elem);

    /* Deallocate blocks if removed. */
    if (inode->removed) {
      free_map_release(inode->sector, 1);
      inode_release(inode);
    }

    free(inode);
  }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void inode_remove(struct inode *inode) {
  ASSERT(inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t inode_read_at(struct inode *inode, void *buffer_, off_t size,
                    off_t offset) {
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;

  while (size > 0) {
    /* Disk sector to read, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector(
        inode, offset); // -1 if offset exceeds data length in inode
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left =
        inode_length(inode) -
        offset; // 0 or negative if offset exceeds data length in inode
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs; // always positive
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually copy out of this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0) // if offset exceeds inode data length, finish
      break;

    /* Read with buffer cache. */
    if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE) {
      cache_read_sector_and_copy_to_memory(sector_idx, buffer + bytes_read);
    } else {
      uint8_t *temp_buffer = malloc(BLOCK_SECTOR_SIZE);
      cache_read_sector_and_copy_to_memory(sector_idx, temp_buffer);
      memcpy(buffer + bytes_read, temp_buffer + sector_ofs, chunk_size);
      free(temp_buffer);
    }

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_read += chunk_size;
  }

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t inode_write_at(struct inode *inode, const void *buffer_, off_t size,
                     off_t offset) {
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;

  if (inode->deny_write_cnt)
    return 0;

  /* Extend file if needed. */
  bool lock_held = lock_held_by_current_thread(&(inode->lock));
  if (!lock_held) {
    lock_acquire(&(inode->lock));
  }
  bool extension_needed = offset + size > inode->readable_length;
  if (extension_needed) {
    inode->data.length = offset + size;
    if (!inode_allocate(&(inode->data))) {
      if (!lock_held) {
        lock_release(&(inode->lock));
      }
      return 0;
    }
    cache_write_sector_and_copy_from_memory(inode->sector, &(inode->data));
  } else {
    // No need to hold lock if not extending.
    if (!lock_held) {
      lock_release(&(inode->lock));
    }
  }

  while (size > 0) {
    /* Sector to write, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector(inode, offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;

    /* Number of bytes to actually write into this sector. */
    int chunk_size = size < sector_left ? size : sector_left;
    if (chunk_size <= 0)
      break;

    /* Write with buffer cache. */
    if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE) {
      cache_write_sector_and_copy_from_memory(sector_idx,
                                              buffer + bytes_written);
    } else {
      uint8_t *temp_buffer = malloc(BLOCK_SECTOR_SIZE);

      if (sector_ofs > 0 || chunk_size < sector_left) {
        // Read data in sector before or after chunk.
        cache_read_sector_and_copy_to_memory(sector_idx, temp_buffer);
      } else {
        // Fill with zeros.
        memset(temp_buffer, 0, BLOCK_SECTOR_SIZE);
      }

      memcpy(temp_buffer + sector_ofs, buffer + bytes_written, chunk_size);
      cache_write_sector_and_copy_from_memory(sector_idx, temp_buffer);
      free(temp_buffer);
    }

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_written += chunk_size;
  }

  if (extension_needed) {
    inode->readable_length = offset;
    if (!lock_held) {
      lock_release(&(inode->lock));
    }
  }

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void inode_deny_write(struct inode *inode) {
  inode->deny_write_cnt++;
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void inode_allow_write(struct inode *inode) {
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
  if (inode->deny_write_cnt > 0) {
    inode->deny_write_cnt--;
  }
}

/* Returns the length, in bytes, of INODE's data. */
off_t inode_length(const struct inode *inode) { return inode->readable_length; }

void inode_lock(const struct inode *inode) {
  lock_acquire(&((struct inode *)inode)->lock);
}

void inode_unlock(const struct inode *inode) {
  lock_release(&((struct inode *)inode)->lock);
}

bool inode_add_parent(block_sector_t parent_sector,
                      block_sector_t child_sector) {
  struct inode *inode = inode_open(child_sector);
  if (!inode) {
    return false;
  } else {
    inode->parent = parent_sector;
    inode->data.parent = parent_sector;
    inode_close(inode);
    return true;
  }
}

bool inode_is_dir(const struct inode *inode) { return inode->data.is_dir; }

int inode_get_open_cnt(const struct inode *inode) { return inode->open_cnt; }

block_sector_t inode_get_parent(const struct inode *inode) {
  return inode->parent;
}
