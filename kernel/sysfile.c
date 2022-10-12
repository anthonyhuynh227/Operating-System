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

/*
 * arg0: int [file descriptor]
 * arg1: char * [buffer to write read bytes to]
 * arg2: int [number of bytes to read]
 *
 * Read up to arg2 bytes from the current position of the corresponding file of the 
 * arg0 file descriptor, place those bytes into the arg1 buffer.
 * The current position of the open file is then updated with the number of bytes read.
 *
 * Return the number of bytes read, or -1 if there was an error.
 *
 * Fewer than arg2 bytes might be read due to these conditions:
 * If the current position + arg2 is beyond the end of the file.
 * If this is a pipe or console device and fewer than arg2 bytes are available 
 * If this is a pipe and the other end of the pipe has been closed.
 *
 * Error conditions:
 * arg0 is not a file descriptor open for read 
 * some address between [arg1, arg1+arg2) is invalid
 * arg2 is not positive
 */
int sys_read(void) {
  int fd;
  char* buffer;
  int size;

  // Parse arguments and check that buffer is in valid user memory
  if (argint(0, &fd) < 0 || argint(2, &size) < 0 || argptr(1, &buffer, size) < 0 ) {
    cprintf("Error: arguments were invalid.\n");
    return -1;
  }

  // Check that size is positive
  if (size < 0) {
    cprintf("Error: size was negative.\n");
    return -1;
  }

  struct desc desc = myproc()->file_array[fd];
  struct file* file = desc.fileptr;
  struct inode* inode = file->inodep;

  // Check that fd is a valid open file descriptor
  if (desc.available == DESC_AVAIL) {
    cprintf("Error: file descriptor %d is not available.\n", fd);
    return -1;
  }

  // Check that access mode is allowing reading
  int access_mode = file->access_mode;
  if (access_mode != O_RDONLY && access_mode != O_RDWR) {
    cprintf("Error: attempted to set write access mode.\n");

    return -1;
  }

  // Read bytes into buffer
  int bytes_read = concurrent_readi(inode, buffer, file->offset, size);
  cprintf("buffer holds: %s\n", buffer);
  // Update offset
  file->offset += bytes_read;
  
  return bytes_read;
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
  //cprintf("Filepath: %s\n", file_path);
  //cprintf("Mode: %d\n", mode);

  
  // Check that files can only be read at this time
  if (mode == O_CREATE || mode ==  O_RDWR || mode == O_WRONLY) {
    return -1;
  }

  int fd = -1;

  // Iterate through each desc struct in current proc struct, and find
  // one that is available.
  for (int i = 0; i < NOFILE; i++) {
    // If file descriptor is available, then allocate to current process
    if (myproc()->file_array[i].available == DESC_AVAIL) {
      fd = i;
      myproc()->file_array[i].available = DESC_NOT_AVAIL;
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

  //cprintf("IP RefCount: %d\n", ip->ref);
  //cprintf("IP devid: %d\n", ip->devid);

  // Set the file struct pointer
  for (int i = 0; i < NFILE; i++) {
    if (global_files[i].available == FILE_AVAIL) {
      global_files[i].available = FILE_NOT_AVAIL;
      myproc()->file_array[fd].fileptr = &global_files[i];
    }
  }
  
  // There exists a bug here!!!
  myproc()->file_array[fd].fileptr->access_mode = mode; // Set access mode
  myproc()->file_array[fd].fileptr->inodep = ip; // Set inode pointer
  myproc()->file_array[fd].fileptr->ref_count = 1; // Set reference count to 1
  myproc()->file_array[fd].fileptr->offset = 0; // Set offset at 0 to start
  
  cprintf("Returning fd: %d\n", fd);
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
