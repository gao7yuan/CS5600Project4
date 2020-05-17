#ifndef FILESYS_FILESYS_H
#define FILESYS_FILESYS_H

#include "filesys/off_t.h"
#include <stdbool.h>

/* Sectors of system file inodes. */
#define FREE_MAP_SECTOR 0 /* Free map file inode sector. */
#define ROOT_DIR_SECTOR 1 /* Root directory file inode sector. */

/* Block device that contains the file system. */
struct block *fs_device;

void filesys_init(bool format);
void filesys_done(void);
bool filesys_create(const char *name, off_t initial_size, bool is_dir);
struct file *filesys_open(const char *name);
bool filesys_remove(const char *name);

/**
 * Given a name, change directory to that given directory
 * @param name a path
 * @return true if operation succeeds, false otherwise
 */
bool filesys_chdir(const char *name);

/**
 * Given a path, find the directory which contains this path
 * @param path a path
 * @return a pointer to the directory
 */
struct dir *get_contain_directory(const char *path);

/**
 * Given a path, return the final file name parsed from this path
 * @param path a path
 * @return a pointer to the file name indicated by this path
 */
char *get_file_name(const char *path);
#endif /* filesys/filesys.h */
