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


/*
 * arg0: int [file descriptor]
 *
 * Duplicate the file descriptor arg0, must use the smallest unused file descriptor.
 * Return a new file descriptor of the duplicated file, -1 otherwise
 *
 * dup is generally used by the shell to configure stdin/stdout between
 * two programs connected by a pipe (lab 2).  For example, "ls | more"
 * creates two programs, ls and more, where the stdout of ls is sent
 * as the stdin of more.  The parent (shell) first creates a pipe 
 * creating two new open file descriptors, and then create the two children. 
 * Child processes inherit file descriptors, so each child process can 
 * use dup to install each end of the pipe as stdin or stdout, and then
 * close the pipe.
 *
 * Error conditions:
 * arg0 is not an open file descriptor
 * there is no available file descriptor
 */
int sys_dup(void) {
  int fd;

  // Check for valid arguments
  if (argint(0, &fd) < 0) {
    cprintf("sys_dup error: could not validate arg0\n");
    return -1;
  }

  if (fd < 0 || fd >= NOFILE) {
    cprintf("sys_dup error: fd is out of bounds\n");
  }

  struct desc desc = myproc()->file_array[fd];
  struct file* file = desc.fileptr;

  // Check that fd is a valid open file descriptor
  if (desc.available == DESC_AVAIL) {
    cprintf("sys_dup error: file descriptor %d is not available.\n", fd);
    return -1;
  }

  // Sanity check: check that file is not available
  if (file->available == FILE_AVAIL) {
    cprintf("sys_dup error: file struct should not be available\n", fd);
    return -1;
  }

  int dup_fd = -1;
  // Iterate through each desc struct in current proc struct, and find
  // one that is available.
  for (int i = 0; i < NOFILE; i++) {
    // If file descriptor is available, then allocate to current process
    if (myproc()->file_array[i].available == DESC_AVAIL) {
      dup_fd = i;
      myproc()->file_array[i].available = DESC_NOT_AVAIL;
      break;
    }
  }

  // If process has too many files, then return error 
  if (dup_fd == -1) {
    cprintf("sys_dup error: too many open files\n");
    return -1;
  }

  myproc()->file_array[dup_fd].fileptr = file;
  myproc()->file_array[dup_fd].fileptr->ref_count++;

  return dup_fd;
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
    cprintf("sys_read error: arguments were invalid.\n");
    return -1;
  }

  // Check that size is positive
  if (size < 0) {
    cprintf("sys_read error: size was negative.\n");
    return -1;
  }

  struct desc desc = myproc()->file_array[fd];
  struct file* file = desc.fileptr;

  // Check that fd is a valid open file descriptor
  if (desc.available == DESC_AVAIL) {
    cprintf("sys_read error: file descriptor %d is not available.\n", fd);
    return -1;
  }

  // Check that access mode is allowing reading
  int access_mode = file->access_mode;
  if (access_mode != O_RDONLY && access_mode != O_RDWR) {
    cprintf("sys_read error: attempted to set write access mode.\n");

    return -1;
  }

  // Read bytes into buffer
  int bytes_read = concurrent_readi(file->inodep, buffer, file->offset, size);

  // Update offset
  file->offset += bytes_read;
  
  return bytes_read;
}


/*
 * arg0: int [file descriptor]
 * arg1: char * [buffer of bytes to write to the given fd]
 * arg2: int [number of bytes to write]
 *
 * Write up to arg2 bytes from arg1 to the current position of the corresponding file of
 * the file descriptor. The current position of the file is updated by the number of bytes written.
 *
 * Return the number of bytes written, or -1 if there was an error.
 *
 * If the full write cannot be completed, write as many as possible 
 * before returning with that number of bytes.
 *
 * If writing to a pipe and the other end of the pipe is closed,
 * return -1.
 *
 * Error conditions:
 * arg0 is not a file descriptor open for write
 * some address between [arg1,arg1+arg2-1] is invalid
 * arg2 is not positive
 *
 * note that for lab1, the file system does not support writing past 
 * the end of the file. Normally this would extend the size of the file
 * allowing the write to complete, to the maximum extent possible 
 * provided there is space on the disk.
 */
int sys_write(void) {
  int fd;
  char* buffer;
  int size;

  // Parse arguments 
  if (argint(0, &fd) < 0 || argint(2, &size) < 0 || argptr(1, &buffer, size) < 0) {
    cprintf("sys_write error: invalid arguments.\n");
    return -1;
  }
  
  // Check that size is positive
  if (size < 0) {
    cprintf("sys_write error: size was negative.\n");
    return -1;
  }

  // Check that fd is open
  if (fd < 0 || myproc()->file_array[fd].available == DESC_AVAIL) {
    cprintf("sys_write error: fd %d was not valid.\n", fd);
    return -1;
  }

  struct desc current_desc = myproc()->file_array[fd];
  struct file* file = current_desc.fileptr;

  // Check that access mode is allowing writing
  int access_mode = file->access_mode;
  if (access_mode != O_WRONLY && access_mode != O_RDWR) {
    cprintf("sys_write error: no write access mode.\n");
    return -1;
  }

  // Write to file
  int bytes_written = concurrent_writei(file->inodep, buffer, file->offset,size);
  if (bytes_written < 0) {
    cprintf("Error: could not write bytes to file.\n");
    return -1;
  }

  file->offset += bytes_written;
  return bytes_written;
}


/*
 * arg0: int [file descriptor]
 *
 * Close the given file descriptor
 * Return 0 on successful close, -1 otherwise
 *
 * Error conditions:
 * arg0 is not an open file descriptor
 */
int sys_close(void) {
  int fd;

  // Check arguments
  if (argint(0, &fd) < 0) {
    cprintf("sys_close error: could not parse arg0.\n");
    return -1;
  }

  // Check that fd is currently open file descriptor
  if (fd < 0 || myproc()->file_array[fd].available == DESC_AVAIL) {
    cprintf("sys_close error: fd %d is not currently open. \n", fd);
    return -1;
  }

  struct file* file = myproc()->file_array[fd].fileptr;

  // Decrement reference count of global file struct, if ref count is zero,
  // then we can deallocate it.
  file->ref_count--;
  if (file->ref_count == 0) {
    file->available = FILE_AVAIL;
  }

  // Deallocate file descriptor in fd array
  myproc()->file_array[fd].available = DESC_AVAIL;
  return 0;
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

  // Open the inode with given filePath
  struct inode* ip = namei(file_path);
  if (ip == NULL) {
    return -1; // File path was not found
  }

  struct stat si;
  concurrent_stati(ip, &si);

  // Check that non-console files can only be read at this time
  if (ip->type != T_DEV && (mode == O_CREATE || mode ==  O_RDWR || mode == O_WRONLY)) {
    cprintf("sys_open error: attempted to write on non console file.\n");
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
    cprintf("sys_open error: too many open files\n");
    return -1;
  }

  // Set the file struct pointer
  for (int i = 0; i < NFILE; i++) {
    if (global_files[i].available == FILE_AVAIL) {
      global_files[i].available = FILE_NOT_AVAIL;
      myproc()->file_array[fd].fileptr = &global_files[i];
      break;
    }
  }
  
  myproc()->file_array[fd].fileptr->access_mode = mode; // Set access mode
  myproc()->file_array[fd].fileptr->inodep = ip; // Set inode pointer and increase reference count
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
