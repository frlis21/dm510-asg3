#ifndef TFS_H
#define TFS_H

#include <stdlib.h>
#include <sys/mman.h>

typedef off_t blkoff_t;
typedef off_t nodoff_t;

// Block size must be a power of 2 for bit twiddlings
#define BLOCK_SIZE 4096
#define BLOCKS_PER_NODE 4
#define DIRECT_BLOCKS 12
#define ILEVELS 3
#define NAME_LIMIT 64
#define BLOCK_MAX_CHILDREN (BLOCK_SIZE / sizeof(nodoff_t))
#define BLOCK_MAX_POINTERS (BLOCK_SIZE / sizeof(blkoff_t))
#define END_BLOCKS -1
#define END_NODES -1

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

// Absolute node size
#define NODE_SIZE(node) ((node)->mode & S_IFDIR ? (node)->nlink * sizeof(nodoff_t) : (node)->size)
// Number required blocks for data (not including indirect pointer blocks)
#define NODE_NRBLOCKS(node) ((BLOCK_SIZE + NODE_SIZE(node) - 1) / BLOCK_SIZE)

/**
 * TFS superblock:
 * Metadata needed to calculate everything.
 */
struct tfs_header {
	blkoff_t nblocks, free_block_head;
	nodoff_t nnodes, free_node_head;
};

/**
 * The TFS node, as it is represented in the image file.
 */
struct tfs_node {
	union {
		struct {
			mode_t mode;
			char name[NAME_LIMIT];
			// Direct blocks
			blkoff_t blocks[DIRECT_BLOCKS];
			// Indirect blocks
			blkoff_t iblocks[ILEVELS];
			// Number of allocated blocks
			fsblkcnt_t nblocks;
			// Number of links of directory, file size otherwise
			union {
				off_t size;
				nlink_t nlink;
			};
			struct timespec atim, mtim;
		};
		// Used for free node linked list.
		nodoff_t next;
	};
};

/**
 * Collection of pointers to useful places and other info nice to have.
 */
struct tfs_info {
	blkoff_t nblocks;
	nodoff_t nnodes;
	blkoff_t *free_block_head;
	nodoff_t *free_node_head;
	struct tfs_node *nodes;
	char (*data)[BLOCK_SIZE];
	/* no touchy */
	void *base;
	off_t filesize;
};

/**
 * Open a file as a TFS image.
 */
int tfs_open(const char *filename);

/**
 * Open and initialize a TFS image.
 */
int tfs_load(const char *filename);

/**
 * Format a TFS image.
 */
void tfs_format();

/**
 * Calculate pointers and other useful things.
 */
void tfs_init();

/**
 * Write back any "queued" changes.
 */
int tfs_destroy();

/**
 * (De)allocate necessary blocks for a node.
 *
 * Must be called after changing the size or nlink of a node.
 */
int tfs_node_trim(struct tfs_node *node);

/**
 * Get a node from the hash table given the path.
 */
struct tfs_node *get_node(const char *path);

/**
 * Get the directory of a node from the hash table given the path.
 */
struct tfs_node *get_directory(const char *path);

/**
 * Collect children of a node into one contiguous array.
 *
 * The array must be freed when you are done with it.
 */
struct tfs_node **tfs_node_children(struct tfs_node *node);

/**
 * Read node data.
 */
int tfs_node_read(struct tfs_node *node, char *buf, size_t size, off_t offset);

/**
 * Write node data.
 */
int tfs_node_write(struct tfs_node *node, const char *buf, size_t size, off_t offset);

/**
 * Add a node.
 */
int tfs_add_node(const char *path, mode_t mode);

/**
 * Remove a node.
 */
int tfs_remove_node(const char *path);

#endif // TFS_H
