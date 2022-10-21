#pragma once

#include <extent.h>
#include <sleeplock.h>
#include <param.h>

#define DESC_AVAIL 0
#define FILE_AVAIL 0
#define DESC_NOT_AVAIL 1
#define FILE_NOT_AVAIL 1

// in-memory copy of an inode
struct inode {
  uint dev;  // Device number
  uint inum; // Inode number
  int ref;   // Reference count
  int valid; // Flag for if node is valid
  struct sleeplock lock;

  short type; // copy of disk inode
  short devid;
  uint size;
  struct extent data;
};

// table mapping device ID (devid) to device functions
struct devsw {
  int (*read)(struct inode *, char *, int);
  int (*write)(struct inode *, char *, int);
};

extern struct devsw devsw[];

// Device ids
enum {
  CONSOLE = 1,
};


// Data structure representing open file
struct file {
  struct inode* inodep;
  int32_t offset;
  int32_t access_mode;
  int32_t ref_count;
  int32_t available;
};

// Data structure representing a file descriptor for a process
struct desc {
  struct file* fileptr; // Stores pointer to file struct
  int32_t available; // Stores whether this current position is available for use
};

// Data structure storing all file structs and a lock
struct files {
  struct file files[NFILE];
  struct sleeplock lock;
};

extern struct files global_files;
