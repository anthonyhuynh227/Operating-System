#pragma once

#include <extent.h>
#include <sleeplock.h>
#include <param.h>


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
  bool available;
};

struct desc {
  struct file* fileptr; // Stores pointer to file struct
  bool available; // Stores whether this current position is available for use
};

extern struct file global_files[];

// Initialize values for global file_structs
void init_files();