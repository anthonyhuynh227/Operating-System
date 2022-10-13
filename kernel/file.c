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
struct file global_files[NFILE];

struct devsw devsw[NDEV];
