//
// File descriptors
//

#include <cdefs.h>
#include <defs.h>
#include <file.h>
#include <fs.h>
#include <param.h>
#include <sleeplock.h>
#include <spinlock.h>

// Global array of file structs
struct files global_files;

struct devsw devsw[NDEV];


void fileinit(void) {
  initsleeplock(&global_files.lock, "files lock");
}