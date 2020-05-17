#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include "devices/block.h"
#include "filesys/off_t.h"
#include <stdbool.h>

struct bitmap;

void inode_init(void);
bool inode_create(block_sector_t, off_t, bool);
struct inode *inode_open(block_sector_t);
struct inode *inode_reopen(struct inode *);
block_sector_t inode_get_inumber(const struct inode *);
void inode_close(struct inode *);
void inode_remove(struct inode *);
off_t inode_read_at(struct inode *, void *, off_t size, off_t offset);
off_t inode_write_at(struct inode *, const void *, off_t size, off_t offset);
void inode_deny_write(struct inode *);
void inode_allow_write(struct inode *);
off_t inode_length(const struct inode *);

/**
 * Get the parent of a given inode
 * @param inode
 * @return
 */
block_sector_t inode_get_parent(const struct inode *inode);

/**
 * Given an inode pointer, acquire lock for the inode
 * @param inode an inode pointer
 */
void inode_lock(const struct inode *inode);

/**
 * Given an inode pointer, release the lock for the inode
 * @param inode an inode pointer
 */
void inode_unlock(const struct inode *inode);

/**
 * Given two block sectors, set the second block sector's parent sector to the
 * first block sector returns true if it succeeds, false if the it fails(when
 * the child sector is NULL)
 * @param parent_sector the parent sector
 * @param child_sector the child sector
 * @return a boolean
 */
bool inode_add_parent(block_sector_t parent_sector,
                      block_sector_t child_sector);

/**
 * Given an inode pointer, returns a boolean indicating if this inode is a
 * directory or not
 * @return an inode pointer
 */
bool inode_is_dir(const struct inode *);

/**
 * Get the number of processes which has opened this inode
 * @return
 */
int inode_get_open_cnt(const struct inode *);
#endif /* filesys/inode.h */
