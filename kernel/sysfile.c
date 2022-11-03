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
#include <trap.h>

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
int sys_dup(void)
{
  acquiresleep(&global_files.lock);

  int fd;

  // Check for valid arguments
  if (argint(0, &fd) < 0)
  {
    cprintf("sys_dup error: could not validate arg0\n");
    releasesleep(&global_files.lock);
    return -1;
  }

  if (fd < 0 || fd >= NOFILE)
  {
    cprintf("sys_dup error: fd is out of bounds\n");
  }

  struct desc desc = myproc()->file_array[fd];
  struct file *file = desc.fileptr;

  // Check that fd is a valid open file descriptor
  if (fd < 0 || fd >= NOFILE || desc.available == DESC_AVAIL)
  {
    cprintf("sys_dup error: file descriptor %d is not available.\n", fd);
    releasesleep(&global_files.lock);
    return -1;
  }

  // Sanity check: check that file is not available
  if (file->available == FILE_AVAIL)
  {
    cprintf("sys_dup error: file struct should not be available\n", fd);
    releasesleep(&global_files.lock);
    return -1;
  }

  int dup_fd = -1;
  // Iterate through each desc struct in current proc struct, and find
  // one that is available.
  for (int i = 0; i < NOFILE; i++)
  {
    // If file descriptor is available, then allocate to current process
    if (myproc()->file_array[i].available == DESC_AVAIL)
    {
      dup_fd = i;
      myproc()->file_array[i].available = DESC_NOT_AVAIL;
      break;
    }
  }

  // If process has too many files, then return error
  if (dup_fd == -1)
  {
    cprintf("sys_dup error: too many open files\n");
    releasesleep(&global_files.lock);
    return -1;
  }

  myproc()->file_array[dup_fd].fileptr = file;
  myproc()->file_array[dup_fd].fileptr->ref_count++;
  releasesleep(&global_files.lock);
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
int sys_read(void)
{
  acquiresleep(&global_files.lock);

  int fd;
  char *buffer;
  int size;

  // Parse arguments and check that buffer is in valid user memory
  if (argint(0, &fd) < 0 || argint(2, &size) < 0 || argptr(1, &buffer, size) < 0)
  {
    cprintf("sys_read error: arguments were invalid.\n");
    releasesleep(&global_files.lock);
    return -1;
  }

  // Check that size is positive
  if (size < 0)
  {
    cprintf("sys_read error: size was negative.\n");
    releasesleep(&global_files.lock);
    return -1;
  }

  struct desc desc = myproc()->file_array[fd];
  struct file *file = desc.fileptr;

  // Check that fd is a valid open file descriptor
  if (fd < 0 || fd >= NOFILE || desc.available == DESC_AVAIL)
  {
    cprintf("sys_read error: file descriptor %d is not available.\n", fd);
    releasesleep(&global_files.lock);
    return -1;
  }

  // Check that access mode is allowing reading
  int access_mode = file->access_mode;
  if (access_mode != O_RDONLY && access_mode != O_RDWR)
  {
    cprintf("sys_read error: attempted to read in write access mode.\n");
    releasesleep(&global_files.lock);
    return -1;
  }

  // Case: if the file struct points to a pipe, then we need to read from a pipe
  struct pipe *pipe = file->pipeptr;
  if (file->file_type == PIPE)
  {
    releasesleep(&global_files.lock);
    acquire(&pipe->lock);

    // Wait while the pipe is full
    int data_read = 0;
    while (data_read != size)
    {
      // Wait while the pipe is empty by sleeping on the pipe address
      while (pipe->data_count == 0)
      {
        // Special case: if there are no fds left and and no data left, then simply return zero
        int write_ref = 0;
        for (int i = 0; i < NFILE; i++)
        {
          struct file curr_file = global_files.files[i];
          if (curr_file.available == FILE_NOT_AVAIL && curr_file.access_mode == O_WRONLY && curr_file.file_type == PIPE && curr_file.pipeptr == pipe)
          {
            write_ref += curr_file.ref_count;
          }
        }
        if (write_ref == 0 && pipe->data_count == 0)
        {
          release(&pipe->lock);
          return data_read;
        }

        // Sleep, but who will wake us up again?
        sleep(pipe, &pipe->lock);
      }

      // Read as many bytes as you can
      while (1)
      {
        buffer[data_read] = pipe->buffer[pipe->read_off];
        data_read++;
        pipe->read_off = (pipe->read_off + 1) % MAX_PIPE_SIZE;
        pipe->data_count--;
        if (data_read == size || pipe->data_count == 0)
        {
          break;
        }
      }

      // Signal that some bytes were read
      wakeup(pipe);
    }
    release(&pipe->lock);
    return data_read;
  }

  // Read bytes into buffer
  int bytes_read = concurrent_readi(file->inodep, buffer, file->offset, size);

  // Update offset
  file->offset += bytes_read;
  releasesleep(&global_files.lock);
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
int sys_write(void)
{
  acquiresleep(&global_files.lock);

  int fd;
  char *buffer;
  int size;

  // Parse arguments
  if (argint(0, &fd) < 0 || argint(2, &size) < 0 || argptr(1, &buffer, size) < 0)
  {
    cprintf("sys_write error: invalid arguments.\n");
    releasesleep(&global_files.lock);
    return -1;
  }

  // Check that size is positive
  if (size < 0)
  {
    cprintf("sys_write error: size was negative.\n");
    releasesleep(&global_files.lock);
    return -1;
  }

  // Check that fd is open
  if (fd < 0 || fd >= NOFILE || myproc()->file_array[fd].available == DESC_AVAIL)
  {
    cprintf("sys_write error: fd %d was not valid.\n", fd);
    releasesleep(&global_files.lock);
    return -1;
  }

  struct file *file = myproc()->file_array[fd].fileptr;

  // Check that access mode is allowing writing
  int access_mode = file->access_mode;
  if (access_mode != O_WRONLY && access_mode != O_RDWR)
  {
    cprintf("sys_write error: no write access mode.\n");
    releasesleep(&global_files.lock);
    return -1;
  }

  // Case: if the file struct points to a pipe, then we need to write to a pip
  struct pipe *pipe = file->pipeptr;
  if (file->file_type == PIPE)
  {
    releasesleep(&global_files.lock);
    acquire(&pipe->lock);

    // Special case: If there are no read fds to pipe, then return an error
    int read_ref = 0;
    for (int i = 0; i < NFILE; i++)
    {
      struct file curr_file = global_files.files[i];
      if (curr_file.available == FILE_NOT_AVAIL && curr_file.access_mode == O_RDONLY && curr_file.file_type == PIPE && curr_file.pipeptr == pipe)
      {
        read_ref += curr_file.ref_count;
      }
    }
    if (read_ref == 0)
    {
      release(&pipe->lock);
      return -1;
    }

    // Wait while the pipe is full
    int data_written = 0;
    while (data_written != size)
    {
      // Wait while the pipe is full by sleeping on the pipe address
      while (pipe->data_count == MAX_PIPE_SIZE)
      {
        // Special case: If there are no read fds to pipe, then return an error
        int read_ref = 0;
        for (int i = 0; i < NFILE; i++)
        {
          struct file curr_file = global_files.files[i];
          if (curr_file.available == FILE_NOT_AVAIL && curr_file.access_mode == O_RDONLY && curr_file.file_type == PIPE && curr_file.pipeptr == pipe)
          {
            read_ref += curr_file.ref_count;
          }
        }
        if (read_ref == 0)
        {
          release(&pipe->lock);
          return -1;
        }

        sleep(pipe, &pipe->lock);
      }

      // Write as many bytes as you can
      while (1)
      {
        pipe->buffer[pipe->write_off] = buffer[data_written];
        data_written++;
        pipe->write_off = (pipe->write_off + 1) % MAX_PIPE_SIZE;
        pipe->data_count++;
        if (data_written == size || pipe->data_count == MAX_PIPE_SIZE)
        {
          break;
        }
      }

      // Signal that some bytes were written
      wakeup(pipe);
    }
    release(&pipe->lock);
    return data_written;
  }

  // Write to file
  int bytes_written = concurrent_writei(file->inodep, buffer, file->offset, size);
  if (bytes_written < 0)
  {
    cprintf("Error: could not write bytes to file.\n");
    releasesleep(&global_files.lock);
    return -1;
  }

  file->offset += bytes_written;
  releasesleep(&global_files.lock);
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
int sys_close(void)
{
  acquiresleep(&global_files.lock);
  int fd;

  // Check arguments
  if (argint(0, &fd) < 0)
  {
    cprintf("sys_close error: could not parse arg0.\n");
    releasesleep(&global_files.lock);
    return -1;
  }

  // Check that fd is currently open file descriptor
  if (fd < 0 || fd >= NOFILE || myproc()->file_array[fd].available == DESC_AVAIL)
  {
    cprintf("sys_close error: fd %d is not currently open. \n", fd);
    releasesleep(&global_files.lock);
    return -1;
  }

  struct file *file = myproc()->file_array[fd].fileptr;

  // Decrement reference count of global file struct, if ref count is zero,
  // then we can deallocate it.
  file->ref_count--;
  if (file->ref_count == 0 && file->file_type == FILE)
  {
    irelease(file->inodep);
    file->available = FILE_AVAIL;
  }

  // Deallocate file descriptor in fd array
  myproc()->file_array[fd].available = DESC_AVAIL;

  // One additional step if the file struct was a PIPE, need to potentially deallocate kernel buffer
  if (file->file_type == PIPE)
  {
    struct pipe *pipe = file->pipeptr;
    int ref_count = 0;
    for (int i = 0; i < NFILE; i++)
    {
      struct file curr_file = global_files.files[i];
      if (curr_file.available == FILE_NOT_AVAIL && curr_file.file_type == PIPE && curr_file.pipeptr == pipe)
      {
        ref_count += curr_file.ref_count;
      }
    }
    if (ref_count == 0)
    {
      kfree((char *)pipe);
    }
  }

  releasesleep(&global_files.lock);
  return 0;
}

/*
 * arg0: int [file descriptor]
 * arg1: struct stat *
 *
 * Populate the struct stat pointer passed in to the function
 *
 * Return 0 on success, -1 otherwise
 *
 * Error conditions:
 * if arg0 is not a valid file descriptor
 * if any address within the range [arg1, arg1+sizeof(struct stat)] is invalid
 */
int sys_fstat(void)
{
  acquiresleep(&global_files.lock);

  int fd;
  struct stat *statp;

  if (argint(0, &fd) < 0 || argptr(1, (char **)&statp, sizeof(struct stat)) < 0)
  {
    cprintf("sys_fstat error: arguments not valid");
    releasesleep(&global_files.lock);
    return -1;
  }

  // Check that fd is valid
  if (fd < 0 || fd >= NOFILE || myproc()->file_array[fd].available == DESC_AVAIL)
  {
    cprintf("sys_fstat error: fd %d is not currently open. \n", fd);
    releasesleep(&global_files.lock);
    return -1;
  }

  concurrent_stati(myproc()->file_array[fd].fileptr->inodep, statp);
  releasesleep(&global_files.lock);
  return 0;
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
int sys_open(void)
{
  acquiresleep(&global_files.lock);

  // Fetch arguments
  char *file_path;
  int32_t mode;

  if (argstr(0, &file_path) < 0)
  {
    releasesleep(&global_files.lock);
    return -1;
  }
  if (argint(1, &mode) < 0)
  {
    releasesleep(&global_files.lock);
    return -1;
  }

  // Open the inode with given filePath
  struct inode *ip = namei(file_path);
  if (ip == NULL)
  {
    releasesleep(&global_files.lock);
    return -1; // File path was not found
  }

  struct stat si;
  concurrent_stati(ip, &si);

  // Check that non-console files can only be read at this time
  if (ip->type != T_DEV && (mode == O_CREATE || mode == O_RDWR || mode == O_WRONLY))
  {
    cprintf("sys_open error: attempted to write on non console file.\n");
    releasesleep(&global_files.lock);
    return -1;
  }

  int fd = -1;

  // Iterate through each desc struct in current proc struct, and find
  // one that is available.
  for (int i = 0; i < NOFILE; i++)
  {
    // If file descriptor is available, then allocate to current process
    if (myproc()->file_array[i].available == DESC_AVAIL)
    {
      fd = i;
      myproc()->file_array[i].available = DESC_NOT_AVAIL;
      break;
    }
  }

  // If process has too many files, then return error
  if (fd == -1)
  {
    cprintf("sys_open error: too many open descriptors\n");
    releasesleep(&global_files.lock);
    return -1;
  }

  // Set the file struct pointer
  bool found_open_file = false;
  for (int i = 0; i < NFILE; i++)
  {
    if (global_files.files[i].available == FILE_AVAIL)
    {
      global_files.files[i].available = FILE_NOT_AVAIL;
      myproc()->file_array[fd].fileptr = &global_files.files[i];
      found_open_file = true;
      break;
    }
  }

  if (!found_open_file)
  {
    cprintf("sys_open error: too many open files\n");
    releasesleep(&global_files.lock);
    return -1;
  }

  myproc()->file_array[fd].fileptr->access_mode = mode; // Set access mode
  myproc()->file_array[fd].fileptr->inodep = ip;        // Set inode pointer and increase reference count
  myproc()->file_array[fd].fileptr->ref_count = 1;      // Set reference count to 1
  myproc()->file_array[fd].fileptr->offset = 0;         // Set offset at 0 to start
  myproc()->file_array[fd].fileptr->file_type = FILE;   // Set offset at 0 to start

  releasesleep(&global_files.lock);
  return fd;
}

/*
 * arg0: char * [path to the executable file]
 * arg1: char * [] [array of strings for arguments]
 *
 * Given a pathname for an executable file, sys_exec() runs that file
 * in the context of the current process (e.g., with the same open file
 * descriptors). arg1 is an array of strings; arg1[0] is the name of the
 * file; arg1[1] is the first argument; arg1[n] is `\0' signalling the
 * end of the arguments.
 *
 * Does not return on success; returns -1 on error
 *
 * Errors:
 * arg0 points to an invalid or unmapped address
 * there is an invalid address before the end of the arg0 string
 * arg0 is not a valid executable file, or it cannot be opened
 * the kernel lacks space to execute the program
 * arg1 points to an invalid or unmapped address
 * there is an invalid address between arg1 and the first n st arg1[n] == `\0'
 * for any i < n, there is an invalid address between arg1[i] and the first `\0'
 */
int sys_exec(void)
{
 
  char *filePath;
  char **arguments;

  // Check arguments are valid
  if (argstr(0, &filePath) < 0)
  {
    cprintf("sys_exec error: arg0 point to an invalid or unmapped adress.\n");
    return -1;
  }

  if (argstr(1, (char **)&arguments) < 0)
  {
    cprintf("sys_exec error: arg1 point to an invalid or unmapped adress.\n");
    return -1;
  }

  int argc = 0;
  for (int i = 0; arguments[i] != NULL; i++)
  {
    char *dummyptr;
    if (fetchstr((uint64_t)arguments[i], &dummyptr) < 0)
    {
      cprintf("sys_exec error: string of arg1 point to an invalid or unmapped adress.\n");
      return -1;
    }
    argc++;
  }

  // Create new vspace
  struct vspace vs;
  uint64_t stack = SZ_2G;
  if (vspaceinit(&vs) < 0)
  {
    cprintf("sys_exec error: vspaceinit failed.\n");
    return -1;
  }
  uint64_t rip;
  if (vspaceloadcode(&vs, filePath, &rip) <= 0)
  {
    cprintf("sys_exec error: vspaceloadcode failed.\n");
    return -1;
  }

  if (vspaceinitstack(&vs, stack) < 0)
  {
    cprintf("sys_exec error: vspaceinitstack failed.\n");
    return -1;
  }

  uint64_t pointers_array[argc]; // Stores the pointers to the strings

  for (int i = argc - 1; i >= 0; i--)
  {
    vspacewritetova(&vs, stack - 8 * (strlen(arguments[i]) / 8 + 1), arguments[i], strlen(arguments[i]));
    pointers_array[i] = stack - 8 * (strlen(arguments[i]) / 8 + 1);
    stack -= 8 * (strlen(arguments[i]) / 8 + 1);
  }

  char nullptr = '\0';
  vspacewritetova(&vs, stack - 8, &nullptr, 1);
  stack -= 8;

  int64_t argv;

  // Write pointers to strings
  for (int i = argc - 1; i >= 0; i--)
  {
    vspacewritetova(&vs, stack - 8, (char *)&pointers_array[i], 4);
    stack -= 8;
    // Save the pointer to the first argument
    if (i == 0)
    {
      argv = stack;
    }
  }

  stack -= 8;

  struct proc *p = myproc();
  p->tf->rip = rip;
  p->tf->rsp = stack;
  p->tf->rdi = argc;
  p->tf->rsi = argv;

  // Install new vspace and return to run new process
  struct vspace temp = myproc()->vspace;
  myproc()->vspace = vs;

  
  vspaceinstall(myproc());
  vspacefree(&temp);
  return 0;
}

int sys_pipe(void) {

  acquiresleep(&global_files.lock);

  int *fd_array;
  if (argptr(0, (char **)&fd_array, sizeof(int) * 2) < 0)
  {
    cprintf("sys_pipe error: address within [arg0, arg0 + sizeof(int) * 2] is invalid\n");
    releasesleep(&global_files.lock);
    return -1;
  }

  // Check to see that there is at least two file descriptors available
  int fd_read = -1;
  int fd_write = -1;
  for (int i = 0; i < NOFILE; i++)
  {
    if (myproc()->file_array[i].available == DESC_AVAIL)
    {
      if (fd_read == -1)
      {
        fd_read = i;
        myproc()->file_array[i].available = DESC_NOT_AVAIL;
      }
      else
      {
        fd_write = i;
        myproc()->file_array[i].available = DESC_NOT_AVAIL;
        break;
      }
    }
  }

  // If there were not two file descriptors, return an error
  if (fd_read != -1 && fd_write == -1)
  {
    myproc()->file_array[fd_read].available = DESC_AVAIL;
    releasesleep(&global_files.lock);
    return -1;
  }

  struct pipe *pipe = (struct pipe *)kalloc();
  // Allocate space for kernel buffer
  if (pipe == 0)
  {
    cprintf("sys_pipe error: kernel does not have space to create pipe\n");
    releasesleep(&global_files.lock);
    return -1;
  }

  // Allocate two global file structs
  bool found_read_file = false;
  bool found_write_file = false;
  for (int i = 0; i < NFILE; i++)
  {
    if (global_files.files[i].available == FILE_AVAIL && !found_read_file)
    {
      global_files.files[i].available = FILE_NOT_AVAIL;
      myproc()->file_array[fd_read].fileptr = &global_files.files[i];

      // Set file struct parameters
      global_files.files[i].file_type = PIPE;
      global_files.files[i].ref_count = 1;
      global_files.files[i].pipeptr = pipe;
      global_files.files[i].access_mode = O_RDONLY;

      found_read_file = true;
    }
    else if (global_files.files[i].available == FILE_AVAIL)
    {
      global_files.files[i].available = FILE_NOT_AVAIL;
      myproc()->file_array[fd_write].fileptr = &global_files.files[i];

      // Set file struct parameters
      global_files.files[i].file_type = PIPE;
      global_files.files[i].ref_count = 1;
      global_files.files[i].pipeptr = pipe;
      global_files.files[i].access_mode = O_WRONLY;
      found_write_file = true;
      break;
    }
  }

  // Set pipe parameters
  pipe->data_count = 0;
  pipe->read_off = 0;
  pipe->write_off = 0;
  initlock(&pipe->lock, "pipe lock");

  // Error if not enough file structs available
  if (found_read_file && !found_write_file)
  {
    cprintf("sys_open error: too many open files\n");
    myproc()->file_array[fd_read].fileptr->available = FILE_NOT_AVAIL;
    releasesleep(&global_files.lock);
    return -1;
  }

  // Write file descriptors into array and return
  fd_array[0] = fd_read;
  fd_array[1] = fd_write;
  releasesleep(&global_files.lock);
  return 0;
}

int sys_unlink(void)
{
  // LAB 4
  return -1;
}
