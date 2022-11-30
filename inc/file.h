#pragma once

#include <extent.h>
#include <sleeplock.h>
#include <param.h>

#define DESC_AVAIL 0
#define FILE_AVAIL 0
#define DESC_NOT_AVAIL 1
#define FILE_NOT_AVAIL 1

#define MAX_PIPE_SIZE 4000

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
  short used; // Stores a flag whether the inode is in use or not
  short num_extents; // Stores the number of extents currently in use 
  struct extent extent_array[30]; // Added an array of extents to track data
  char padding[4];
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

// File values
enum {
  FILE = 1,
  PIPE = 2,
};

// Data structure representing open file
struct file {
  struct inode* inodep;
  int32_t offset;
  int32_t ref_count;
  int32_t available;
  int32_t access_mode;
  struct pipe* pipeptr;
  short file_type; // Will hold FILE, PIPE
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

struct pipe {
  int read_off;
  int write_off;
  struct spinlock lock;
  int data_count;
  char buffer[MAX_PIPE_SIZE];
};

void fileinit(void); // The function that initializes the files array's sleeplock
extern struct files global_files;
