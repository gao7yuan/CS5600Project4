#ifndef FILESYS_DIRECTORY_H
#define FILESYS_DIRECTORY_H

#include "devices/block.h"
#include <stdbool.h>
#include <stddef.h>

/* Maximum length of a file name component.
   This is the traditional UNIX maximum length.
   After directories are implemented, this maximum length may be
   retained, but much longer full path names must be allowed. */
#define NAME_MAX 30

struct inode;

/* Opening and closing directories. */
bool dir_create(block_sector_t sector, size_t entry_cnt);
struct dir *dir_open(struct inode *);
struct dir *dir_open_root(void);
struct dir *dir_reopen(struct dir *);
void dir_close(struct dir *);
struct inode *dir_get_inode(struct dir *);

/* Reading and writing. */
bool dir_lookup(const struct dir *, const char *name, struct inode **);
bool dir_add(struct dir *, const char *name, block_sector_t);
bool dir_remove(struct dir *, const char *name);
bool dir_readdir(struct dir *, char name[NAME_MAX + 1]);

/**
 * Given a dir pointer, return true if the dir is the root dir
 * false if the dir is not the root dir
 * @param dir a pointer to the dir
 * @return a boolean
 */
bool dir_is_root(struct dir *dir);

/**
 * Given a dir pointer, return true if the dir has a parent, false otherwise
 * @param dir a pointer to the dir
 * @param inode the inode lists
 * @return a boolean
 */
bool dir_has_parent(struct dir *dir, struct inode **inode);

/**
 * Given an inode pointer, returns true if the directory which the inode points
 * to is not used by any process, false otherwise
 * @param inode an inode pointer
 * @return  a boolean
 */
bool dir_is_empty(struct inode *inode);
#endif /* filesys/directory.h */
