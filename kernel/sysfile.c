//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include <cdefs.h>
#include <defs.h>
#include <fcntl.h>
#include <file.h>
#include <fs.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <sleeplock.h>
#include <spinlock.h>
#include <stat.h>


int sys_dup(void) {
  // LAB1
  return -1;
}

int sys_read(void) {
  // LAB1
  return -1;
}

int sys_write(void) {
  // you have to change the code in this function.
  // Currently it supports printing one character to the screen.

  int n;
  char *p;

  if (argint(2, &n) < 0 || argptr(1, &p, n) < 0)
    return -1;
  uartputc((int)(*p));
  return 1;
}

int sys_close(void) {
  // LAB1
  return -1;
}

int sys_fstat(void) {
  // LAB1
  return -1;
}

/*
 * arg0: char * [path to the file]
 * arg1: int [mode for opening the file (see inc/fcntl.h)]
 *
 * Given a pathname for a file, sys_open() returns a file descriptor, a small,
 * nonnegative integer for use in subsequent system calls. The file descriptor
 * returned by a successful call will be the lowest-numbered file descriptor
 * not currently open for the process.
 *
 * Each open file maintains a current position, initially zero.
 *
 * returns -1 on error
 *
 * Errors:
 * arg0 points to an invalid or unmapped address 
 * there is an invalid address before the end of the string 
 * the file does not exist
 * there is no available file descriptor 
 * since the file system is read only, any write flags for non console files are invalid
 * O_CREATE is not permitted (for now)
 *
 * note that for lab1, the file system does not support file create
 */

int sys_open(void) {
  // Fetch arguments
  char* file_path;
  int32_t mode;

  if (argstr(0, &file_path) < 0) {
    return -1;
  }
  if (argint(1, &mode) < 0) {
    return -1;
  }

  
  // Check that files can only be read at this time
  if (mode == O_CREATE || mode ==  O_RDWR || mode ==O_WRONLY) {
    return -1;
  }

  int fd = -1;

  // Iterate through each desc struct in current proc struct, and find
  // one that is available.
  for (int i = 0; i < NOFILE; i++) {
    // If file descriptor is available, then allocate to current process
    if (myproc()->file_array[i].available) {
      fd = i;
      myproc()->file_array[i]. available = false;
      break;
    }
  }

  // If process has too many files, then return error 
  if (fd == -1) {
    return -1;
  }
  
  // Then, open the inode with given filePath
  struct inode* ip = namei(file_path);
  if (ip == NULL) {
    return -1; // File path was not found
  }

  // Set the file struct pointer
  for (int i = 0; i < NFILE; i++) {
    if (global_files[i].available) {
      global_files[i].available = false;
      myproc()->file_array[fd].fileptr = &global_files[i];
    }
  }
  
  myproc()->file_array[fd].fileptr->available = false; // File struct is no longer available
  myproc()->file_array[fd].fileptr->access_mode = mode; // Set access mode
  myproc()->file_array[fd].fileptr->inodep = ip; // Set inode pointer
  myproc()->file_array[fd].fileptr->ref_count = 1; // Set reference count to 1
  myproc()->file_array[fd].fileptr->offset = 0; // Set offset at 0 to start
  
  return fd;
}

int sys_exec(void) {
  // LAB2
  return -1;
}

int sys_pipe(void) {
  // LAB2
  return -1;
}

int sys_unlink(void) {
  // LAB 4
  return -1;
}
