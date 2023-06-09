// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xk/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include <cdefs.h>
#include <defs.h>
#include <file.h>
#include <fs.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <sleeplock.h>
#include <spinlock.h>
#include <stat.h>

#include <buf.h>


// Logging API
static void log_begin_tx();
static void log_write(struct buf* buff);
static void log_commit();
static void log_recover(); 
static int raw_writei(struct inode *ip, char *src, uint off, uint n);
static int concurrent_raw_writei(struct inode *ip, char *src, uint off, uint n);


// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb;

// Read the super block.
void readsb(int dev, struct superblock *sb) {
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// mark [start, end] bit in bp->data to 1 if used is true, else 0
static void bmark(struct buf *bp, uint start, uint end, bool used)
{
  int m, bi;
  for (bi = start; bi <= end; bi++) {
    m = 1 << (bi % 8);
    if (used) {
      bp->data[bi/8] |= m;  // Mark block in use.
    } else {
      if((bp->data[bi/8] & m) == 0)
        panic("freeing free block");
      bp->data[bi/8] &= ~m; // Mark block as free.
    }
  }
  bp->flags |= B_DIRTY; // mark our update
  //New: write out the changes to disk as well
  log_write(bp);
}

// Blocks.

// Allocate n disk blocks, no promise on content of allocated disk blocks
// Returns the beginning block number of a consecutive chunk of n blocks
static uint balloc(uint dev, uint n)
{
  int b, bi, m;
  struct buf *bp;

  bp = 0;
  for (b = 0; b < sb.size; b += BPB) {
    bp = bread(dev, BBLOCK(b, sb)); // look through each bitmap sector

    uint sz = 0;
    uint i = 0;
    for (bi = 0; bi < BPB && b + bi < sb.size; bi++) {
      m = 1 << (bi % 8);
      if ((bp->data[bi/8] & m) == 0) {  // Is block free?
        sz++;
        if (sz == 1) // reset starting blk
          i = bi;
        if (sz == n) { // found n blks
          bmark(bp, i, bi, true); // mark data block as used
          brelse(bp);
          return b+i;
        }
      } else { // reset search
        sz = 0;
        i =0;
      }
    }
    brelse(bp);
  }
  panic("balloc: can't allocate contiguous blocks");
}

// Free n disk blocks starting from b
static void bfree(int dev, uint b, uint n)
{
  struct buf *bp;

  assertm(n >= 1, "freeing less than 1 block");
  assertm(BBLOCK(b, sb) == BBLOCK(b+n-1, sb), "returned blocks live in different bitmap sectors");

  bp = bread(dev, BBLOCK(b, sb));
  bmark(bp, b % BPB, (b+n-1) % BPB, false);
  brelse(bp);
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// range of blocks holding the file's content.
//
// The inodes themselves are contained in a file known as the
// inodefile. This allows the number of inodes to grow dynamically
// appending to the end of the inode file. The inodefile has an
// inum of 0 and starts at sb.startinode.
//
// The kernel keeps a cache of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The cached
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->flags.
//
// Since there is no writing to the file system there is no need
// for the callers to worry about coherence between the disk
// and the in memory copy, although that will become important
// if writing to the disk is introduced.
//
// Clients use iget() to populate an inode with valid information
// from the disk. idup() can be used to add an in memory reference
// to and inode. irelease() will decrement the in memory reference count
// and will free the inode if there are no more references to it,
// freeing up space in the cache for the inode to be used again.



struct {
  struct spinlock lock;
  struct inode inode[NINODE];
  struct inode inodefile;
} icache;

// Find the inode file on the disk and load it into memory
// should only be called once, but is idempotent.
static void init_inodefile(int dev) {
  struct buf *b;
  struct dinode di;

  b = bread(dev, sb.inodestart);
  memmove(&di, b->data, sizeof(struct dinode));

  icache.inodefile.inum = INODEFILEINO;
  icache.inodefile.dev = dev;
  icache.inodefile.type = di.type;
  icache.inodefile.valid = 1;
  icache.inodefile.ref = 1;

  icache.inodefile.devid = di.devid;
  icache.inodefile.size = di.size;
  icache.inodefile.extent_array[0] = di.extent_array[0];


  // New: we also need to update the num_extents and used fields
  icache.inodefile.num_extents = 1;
  icache.inodefile.used = DINODE_USED;

  brelse(b);
}

void iinit(int dev) {
  int i;

  initlock(&icache.lock, "icache");
  for (i = 0; i < NINODE; i++) {
    initsleeplock(&icache.inode[i].lock, "inode");
  }
  initsleeplock(&icache.inodefile.lock, "inodefile");

  readsb(dev, &sb);
  cprintf("sb: size %d nblocks %d bmap start %d inodestart %d\n", sb.size,
          sb.nblocks, sb.bmapstart, sb.inodestart);

  // New: can recover log() here
  log_recover();

  init_inodefile(dev);
}


// Reads the dinode with the passed inum from the inode file.
// Threadsafe, will acquire sleeplock on inodefile inode if not held.
static void read_dinode(uint inum, struct dinode *dip) {
  int holding_inodefile_lock = holdingsleep(&icache.inodefile.lock);
  if (!holding_inodefile_lock)
    locki(&icache.inodefile);

  readi(&icache.inodefile, (char *)dip, INODEOFF(inum), sizeof(*dip));

  if (!holding_inodefile_lock)
    unlocki(&icache.inodefile);

}


// Find the inode with number inum on device dev
// and return the in-memory copy. Does not read
// the inode from from disk.
static struct inode *iget(uint dev, uint inum) {
  struct inode *ip, *empty;

  acquire(&icache.lock);

  // Is the inode already cached?
  empty = 0;
  for (ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++) {
    if (ip->ref > 0 && ip->dev == dev && ip->inum == inum) {
      ip->ref++;
      release(&icache.lock);
      return ip;
    }
    if (empty == 0 && ip->ref == 0) // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode cache entry.
  if (empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->ref = 1;
  ip->valid = 0;
  ip->dev = dev;
  ip->inum = inum;

  release(&icache.lock);

  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode *idup(struct inode *ip) {
  acquire(&icache.lock);
  ip->ref++;
  release(&icache.lock);
  return ip;
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode cache entry can
// be recycled.
void irelease(struct inode *ip) {
  acquire(&icache.lock);
  // inode has no other references release
  if (ip->ref == 1)
    ip->type = 0;
  ip->ref--;
  release(&icache.lock);
}

// Lock the given inode.
// Reads the inode from disk if necessary.
void locki(struct inode *ip) {
  struct dinode dip;

  if(ip == 0 || ip->ref < 1)
    panic("locki");

  acquiresleep(&ip->lock);

  if (ip->valid == 0) {

    if (ip != &icache.inodefile)
      locki(&icache.inodefile);
    read_dinode(ip->inum, &dip);
    if (ip != &icache.inodefile)
      unlocki(&icache.inodefile);
    
    ip->type = dip.type;
    ip->devid = dip.devid;

    ip->size = dip.size;

    for (int i = 0; i < 30; i++) {
      ip->extent_array[i] = dip.extent_array[i];
    }

    // New: update the num_extents and used fields
    ip->num_extents = dip.num_extents;
    ip->used = dip.used;

    ip->valid = 1;

    if (ip->type == 0)
      panic("iget: no type");
  }
  
}

// Unlock the given inode.
void unlocki(struct inode *ip) {
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("unlocki");

  releasesleep(&ip->lock);

}

// threadsafe stati.
void concurrent_stati(struct inode *ip, struct stat *st) {
  locki(ip);
  stati(ip, st);
  unlocki(ip);
}

// Copy stat information from inode.
// Caller must hold ip->lock.
void stati(struct inode *ip, struct stat *st) {
  if (!holdingsleep(&ip->lock))
    panic("not holding lock");

  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->size = ip->size;
}

// threadsafe readi.
int concurrent_readi(struct inode *ip, char *dst, uint off, uint n) {
  int retval;

  locki(ip);
  retval = readi(ip, dst, off, n);
  unlocki(ip);

  return retval;
}

// Read data from inode.
// Returns number of bytes read.
// Caller must hold ip->lock.
int readi(struct inode *ip, char *dst, uint off, uint n) {
  if (!holdingsleep(&ip->lock))
    panic("not holding lock");

  if (ip->type == T_DEV) {
    if (ip->devid < 0 || ip->devid >= NDEV || !devsw[ip->devid].read)
      return -1;
    return devsw[ip->devid].read(ip, dst, n);
  }

  // Check parameters
  if (off > ip->size || off + n < off)
    return -1;

  // Read at most size bytes
  if (off + n > ip->size)
    n = ip->size - off;

  uint bytes_read = 0;
  // Iterate through extents and read as much
  // bytes as possible from the offset
  uint off_blk = off / BSIZE;
  uint file_blk_no = 0;
  for (int ext = 0; ext < ip->num_extents; ext++) {
    for (int blk = ip->extent_array[ext].startblkno; blk < ip->extent_array[ext].startblkno + ip->extent_array[ext].nblocks; blk++) {
      // Continue until you find the right offset block
      if (file_blk_no != off_blk) {
        file_blk_no++;
        continue;
      }
      uint bytes_to_read = min(BSIZE - (off % BSIZE), n);
      struct buf* blk_buff = bread(ip->dev, blk); // Read the blk block into buffer
      memmove(dst, (char*) &blk_buff->data + (off % BSIZE), bytes_to_read);
      brelse(blk_buff); // Release block

      // Update off and n
      n -= bytes_to_read;
      bytes_read += bytes_to_read;
      off += bytes_to_read;
      dst += bytes_to_read;
      // Break early if necessary
      if (n == 0) {
        break;
      }

      off_blk++;
      file_blk_no++;
    }
  }
  return bytes_read;
}

// threadsafe writei.
int concurrent_writei(struct inode *ip, char *src, uint off, uint n) {
  int retval;

  locki(ip);
  retval = writei(ip, src, off, n);
  unlocki(ip);

  return retval;
}

// Write data to inode.
// Returns number of bytes written.
// Caller must hold ip->lock.
int writei(struct inode *ip, char *src, uint off, uint n) {
  // writei is just a raw_writei wrapped around a transaction
  log_begin_tx();
  int bytes_written = raw_writei(ip, src, off, n);
  log_commit();

  return bytes_written;
}

// Directories

int namecmp(const char *s, const char *t) { return strncmp(s, t, DIRSIZ); }

struct inode *rootlookup(char *name) {
  return dirlookup(namei("/"), name, 0);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode *dirlookup(struct inode *dp, char *name, uint *poff) {
  uint off, inum;
  struct dirent de;

  if (dp->type != T_DIR)
    panic("dirlookup not DIR");

  for (off = 0; off < dp->size; off += sizeof(de)) {
    if (readi(dp, (char *)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if (de.inum == 0)
      continue;
    if (namecmp(name, de.name) == 0) {
      // entry matches path element
      if (poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }

  return 0;
}

// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char *skipelem(char *path, char *name) {
  char *s;
  int len;

  while (*path == '/')
    path++;
  if (*path == 0)
    return 0;
  s = path;
  while (*path != '/' && *path != 0)
    path++;
  len = path - s;
  if (len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while (*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
static struct inode *namex(char *path, int nameiparent, char *name) {
  struct inode *ip, *next;

  if (*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(namei("/"));

  while ((path = skipelem(path, name)) != 0) {
    locki(ip);
    if (ip->type != T_DIR) {
      unlocki(ip);
      goto notfound;
    }

    // Stop one level early.
    if (nameiparent && *path == '\0') {
      unlocki(ip);
      return ip;
    }

    if ((next = dirlookup(ip, name, 0)) == 0) {
      unlocki(ip);
      goto notfound;
    }

    unlocki(ip);
    irelease(ip);
    ip = next;
  }
  if (nameiparent)
    goto notfound;

  return ip;

notfound:
  irelease(ip);
  return 0;
}

struct inode *namei(char *path) {
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode *nameiparent(char *path, char *name) {
  return namex(path, 1, name);
}

// Create new inode, modify the root directory, and return a new 
// inode ptr to it.
struct inode* create_inode(char* name) {
  log_begin_tx();

  struct inode* inodefile_inode = &icache.inodefile;
  struct dinode din;
  struct inode* new_inode = NULL;

  // Need to find an empty inode in the inodefile
  for (int i = 0; i < inodefile_inode->size / sizeof(struct dinode); i++) {
    read_dinode(i, &din);
    if (din.used == DINODE_AVAIL) {
      // We found an empty inode, so we need to populate it with real values now
      struct dinode new_dinode;
      new_dinode.devid = icache.inodefile.devid;
      new_dinode.type = icache.inodefile.type;
      new_dinode.num_extents = 0;
      new_dinode.size = 0;
      new_dinode.used = DINODE_USED;
      concurrent_raw_writei(&icache.inodefile, (char*) &new_dinode, INODEOFF(i), sizeof(struct dinode));
      new_inode = iget(ROOTDEV, i);
      locki(new_inode);


      break;
    }
  }

  // In the case where the inodefile is full, we must append it 
  // to make more inodes
  if (new_inode == NULL) {
    // We need to append a dinode to the inodefile
    struct dinode new_dinode;
    new_dinode.devid = icache.inodefile.devid;
    new_dinode.type = icache.inodefile.type;
    new_dinode.num_extents = 0;
    new_dinode.size = 0;
    new_dinode.used = DINODE_USED;

    int num_inodes = icache.inodefile.size / sizeof(struct dinode);

    // We need to append the new dinode to the inodefile
    concurrent_raw_writei(&icache.inodefile, (char*) &new_dinode, icache.inodefile.size, sizeof(struct dinode));

    // Get the new inode
    new_inode = iget(ROOTDEV, num_inodes);
    locki(new_inode);
  }

  // Get the root inode
  struct inode* root_inode = iget(ROOTDEV, 1);

  // Create new dirent entry to root inode
  struct dirent new_entry;
  new_entry.inum = new_inode->inum;
  memmove(&new_entry.name, name, strlen(name) + 1);

  // Need to find an empty entry in the root directory
  bool found_entry = false;
  struct dirent de;
  for (int i = 0; i < root_inode->size / sizeof(struct dirent); i++) {
    if (concurrent_readi(root_inode, (char*)&de, i * sizeof(struct dirent), sizeof(struct dirent)) != sizeof(struct dirent)) {
      panic("could not read root directory");
    }

    // If we find an empty entry, then we can simply update the root directory in-place
    if (de.inum == 0) {
      concurrent_raw_writei(root_inode, (char*) &new_entry, i * sizeof(struct dirent), sizeof(struct dirent));
      found_entry = true;
      break;
    }
  }

  // If we didn't find an entry, then append new entry to root inode
  if (!found_entry) {
    concurrent_raw_writei(root_inode, (char*) &new_entry, root_inode->size, sizeof(struct dirent));
  }
 
  // Unlock and release
  unlocki(new_inode);
  irelease(root_inode);

  log_commit();

  return new_inode;
}

// Function for deleting an inode
void delete_inode(struct inode* ip) {
  // First, we should lock the root directory and the file to prevent access to the file
  struct inode* root_inode = iget(ROOTDEV, 1);
  locki(root_inode);
  locki(ip);

  // Need to find entry to delete in root directory
  uint off;
  struct dirent de;
  for (off = 0; off < root_inode->size; off += sizeof(de)) {
    if (readi(root_inode, (char *)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if (de.inum == 0)
      continue;
    if (de.inum == ip->inum) {
      // We need to erase this entry from the root directory, and push these changes to 
      // disk
      struct dirent erase_entry;
      erase_entry.inum = 0;
      writei(root_inode, (char*) &erase_entry, off, sizeof(struct dirent));
    }
  }

  // Update the inodefile to have an invalid dinode
  struct dinode new_dinode;
  new_dinode.devid = 1234;
  new_dinode.type = 1234;
  new_dinode.num_extents = 1234;
  new_dinode.size = 1234;
  new_dinode.used = DINODE_AVAIL;

  concurrent_writei(&icache.inodefile, (char*) &new_dinode, INODEOFF(ip->inum), sizeof(struct dinode));

  // We should also free the data blocks pointed to by the inode. We 
  // can just iterate through all extents and free each one.
  for (int i = 0 ; i < ip->num_extents; i++) {
    bfree(ROOTDEV, ip->extent_array[i].startblkno, ip->extent_array[i].nblocks);
  }  

  // Unlock and release
  unlocki(ip);
  unlocki(root_inode);
  irelease(ip);
  irelease(root_inode);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// API Functions for crash-safe transactions
////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Begin a transaction on the log
static void log_begin_tx() {
  // Read the header block
  struct buf* log_header_buff = bread(ROOTDEV, sb.logstart);

  // Reset the header to initial state
  struct logheader log_header;
  log_header.size = 0;
  log_header.valid_flag = TX_INVALID;
  memmove(&log_header_buff->data, &log_header, sizeof(struct logheader));

  // Write the header block to disk
  bwrite(log_header_buff);
  brelse(log_header_buff);
}

// Write a data block to the log
static void log_write(struct buf* buff) {
  // Read the header block
  struct buf* log_header_buff = bread(ROOTDEV, sb.logstart);

  struct logheader log_header;
  memmove(&log_header, &log_header_buff->data, sizeof(struct logheader));

  assert(log_header.valid_flag == TX_INVALID);  
  assert(log_header.size <= 28);

  // Write data block to log
  struct buf* data_blk = bread(ROOTDEV, sb.logstart + log_header.size + 1);
  memmove(&data_blk->data, &buff->data, BSIZE);
  bwrite(data_blk);

  // Update header and write it to disk
  log_header.disk_loc[log_header.size] = buff->blockno; // The disk location should be in buff
  log_header.size++;
  memmove(&log_header_buff->data, &log_header, sizeof(struct logheader));
  bwrite(log_header_buff);

  brelse(data_blk);
  brelse(log_header_buff);
}

// Completes the transaction and flushes it to disk
static void log_commit() {
  // Read the header block
  struct buf* log_header_buff = bread(ROOTDEV, sb.logstart);
  struct logheader log_header;

  memmove(&log_header, &log_header_buff->data, sizeof(struct logheader));

  assert(log_header.valid_flag == TX_INVALID);  
  assert(log_header.size <= 29);

  // Update flag to VALID and write header to disk
  log_header.valid_flag = TX_VALID;
  memmove(&log_header_buff->data, &log_header, sizeof(struct logheader));
  bwrite(log_header_buff);
  brelse(log_header_buff);
  
  // Reread header, then transfer blocks to right location on disk.
  log_header_buff = bread(ROOTDEV, sb.logstart);
  memmove(&log_header, &log_header_buff->data, sizeof(struct logheader));

  assert(log_header.valid_flag == TX_VALID);  
  assert(log_header.size <= 29);

  // Transfer blocks
  for (int i = 0; i < log_header.size; i++) {
    struct buf* data_buff = bread(ROOTDEV, log_header.disk_loc[i]); // The actual disk block
    struct buf* log_buff = bread(ROOTDEV, sb.logstart + i + 1); // The corresponding block in the log

    // Write to correct disk location
    memmove(&data_buff->data, &log_buff->data, BSIZE);
    bwrite(data_buff);

    brelse(data_buff);
    brelse(log_buff);
  }

  // Complete transaction by setting header flag to INVALID
  log_header.valid_flag = TX_INVALID;
  log_header.size = 0;

  memmove(&log_header_buff->data, &log_header, sizeof(struct logheader));
  bwrite(log_header_buff);

  brelse(log_header_buff);
}

// Read from the log and recover any transactions, if applicable
static void log_recover() {
  // Read the header block
  struct buf* log_header_buff = bread(ROOTDEV, sb.logstart);
  struct logheader log_header;

  memmove(&log_header, &log_header_buff->data, sizeof(struct logheader));

  // If flag is valid, then need to make the transaction.
  if (log_header.valid_flag == TX_VALID) {
    // Transfer blocks
    for (int i = 0; i < log_header.size; i++) {
      struct buf* data_buff = bread(ROOTDEV, log_header.disk_loc[i]); // The actual disk block
      struct buf* log_buff = bread(ROOTDEV, sb.logstart + i + 1); // The corresponding block in the log

      // Write to correct disk location
      memmove(&data_buff->data, &log_buff->data, BSIZE);
      bwrite(data_buff);

      brelse(data_buff);
      brelse(log_buff);
  }
    
  }

  // Complete transaction by setting header flag to INVALID
  log_header.valid_flag = TX_INVALID;
  log_header.size = 0;

  memmove(&log_header_buff->data, &log_header, sizeof(struct logheader));
  bwrite(log_header_buff);
  brelse(log_header_buff);
}


// threadsafe raw_writei.
static int concurrent_raw_writei(struct inode *ip, char *src, uint off, uint n) {
  int retval;
  locki(ip);
  retval = raw_writei(ip, src, off, n);
  unlocki(ip);
  return retval;
}

// Raw write, meaning we only use log_writes but do not actually commit anything.
// Precondition: You must have called log_start_tx() beforehand, and you should
// call log_commit() afterwards to actually commit the changes. 
static int raw_writei(struct inode *ip, char *src, uint off, uint n) {
  if (!holdingsleep(&ip->lock))
    panic("not holding lock");
  if (ip->type == T_DEV) {
    if (ip->devid < 0 || ip->devid >= NDEV || !devsw[ip->devid].write)
      return -1;
    return devsw[ip->devid].write(ip, src, n);
  }

  // Check that the parameters are valid
  if (n < 0 || off < 0) {
    cprintf("writei: invalid parameters");
  }


  uint bytes_written = 0;
  uint orig_off = off;
  uint old_size = ip->size;

  // Case 1: Offset is within extent array. Then we need to write as much
  // as possible until either reaching end of allocated array or we wrote everything
  uint off_blk = off / BSIZE;
  uint file_blk_no = 0;
  for (int ext = 0; ext < ip->num_extents; ext++) {
    for (int blk = ip->extent_array[ext].startblkno; blk < ip->extent_array[ext].startblkno + ip->extent_array[ext].nblocks; blk++) {
      // Continue until you find the right offset block
      if (file_blk_no != off_blk) {
        file_blk_no++;
        continue;
      }
      uint space_avail = BSIZE - (off % BSIZE);
      uint num_to_write = min(space_avail, n);
      struct buf* blk_buff = bread(ip->dev, blk); // Read the blk block into buffer

      memmove( (char*) &blk_buff->data + (off % BSIZE), src + bytes_written, num_to_write); // Write the data into buffer
      log_write(blk_buff); // Flush buffer to disk
      brelse(blk_buff); // Release block

      // Update off and n
      off += num_to_write;
      n -= num_to_write;
      bytes_written += num_to_write;

      // Break early if necessary
      if (n == 0) {
        break;
      }

      off_blk++;
      file_blk_no++;
    }
  }
  

  // Case 2: There is more data to write. We must allocate a new data extent
  // and fill it up with as many data blocks as needed.
  if (n > 0) {
    //cprintf("allocating new extent!\n");
    // First find padding/data blocks needed
    int blk_padd = off_blk - file_blk_no;
    int blk_data = n / BSIZE + 1;

    // Allocate necessary space
    uint blk_num = balloc(ip->dev, blk_padd + blk_data);

    // Update inode fields
    ip->num_extents++;
    if (ip->num_extents == 31) {
      panic("our file used up all its extents");
    }
    ip->extent_array[ip->num_extents - 1].startblkno = blk_num;
    ip->extent_array[ip->num_extents - 1].nblocks = blk_padd + blk_data;
    
    // Write rest of data to newly allocated extent.
    for (int blk = blk_num; blk < blk_num + blk_padd + blk_data; blk++) {
      // Continue until you find the right offset block
      if (file_blk_no != off_blk) {
        file_blk_no++;
        continue;
      }

      uint space_avail = BSIZE - (off % BSIZE);
      uint num_to_write = min(space_avail, n);
      struct buf* blk_buff = bread(ip->dev, blk); // Read the blk block into buffer
      memmove( (char*) &blk_buff->data + (off % BSIZE), src + bytes_written, num_to_write); // Write the data into buffer
      log_write(blk_buff); // Flush buffer to disk
      brelse(blk_buff); // Release block

      // Update off and n
      off += num_to_write;
      n -= num_to_write;
      bytes_written += num_to_write;


      // Break early if necessary
      if (n == 0) {
        break;
      }

      off_blk++;
      file_blk_no++;
    }
  }
  // Verify that n is now 0
  if (n != 0) {
    cprintf("writei: could not write all n bytes");
    return -1;
  }
  // Update the size of the inode, if necessary
  ip->size = max(ip->size, orig_off + bytes_written);

  // Stop recursion, if the size is exactly the same
  if (ip->size == old_size) {

    return bytes_written;
  }

  // Otherwise, we need to update the inode itself. We can do this recursively.
  int holding_inodefile_lock = holdingsleep(&icache.inodefile.lock);
  if (!holding_inodefile_lock)
    locki(&icache.inodefile);

  struct dinode din;
  din.type = ip->type;
  din.devid = ip->devid;
  din.size = ip->size;
  din.used = ip->used;
  din.num_extents = ip->num_extents;
  memmove(&din.extent_array, &ip->extent_array, sizeof(struct extent) * 30);

  int num_bytes = raw_writei(&icache.inodefile, (char*) &din, INODEOFF(ip->inum), sizeof(struct dinode));

  if (num_bytes != sizeof(struct dinode)) {
    cprintf("writei: had error when writing inode to disk");
  }
  // We do not need to update the inodefile's inode, because we are not updating the size
  // or starting a new extent.
  
  if (!holding_inodefile_lock)
    unlocki(&icache.inodefile);
  

  return bytes_written;
}