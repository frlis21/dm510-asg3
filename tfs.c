#include "tfs.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <math.h>
#include <search.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static struct tfs_info tfs_info;

// Node number from pointer.
#define NODENO(node) ((node)-tfs_info.nodes)
// Block number from pointer.
#define BLOCKNO(block) ((block)-tfs_info.data)
// Cast block data to an array of node offsets.
#define BLOCK_NODES(block) ((nodoff_t *)(tfs_info.data[block]))
// Cast block data to an array of block offsets.
#define BLOCK_POINTERS(block) ((blkoff_t *)(tfs_info.data[block]))
// Cast block data to the next member in the free block linked list.
#define NEXT_FREE_BLOCK(block) BLOCK_POINTERS(block)[0]

/**
 * Iterating through indirect levels is painful,
 * so the process is abstracted away with the help of this iterator-like thingy.
 */
struct block_cursor {
	struct tfs_node *node;
	blkoff_t i;
	int level;
	blkoff_t pos[ILEVELS];
	blkoff_t block[ILEVELS];
};

// Get what block an iterator is currently on.
#define CURRENT_BLOCK(cursor)                                                                                          \
	((cursor)->i < DIRECT_BLOCKS ? (cursor)->node->blocks[(cursor)->i]                                                 \
	                             : BLOCK_POINTERS((cursor)->block[(cursor)->level])[(cursor)->pos[(cursor)->level]])

// Iterator definition helper
#define DEFINE_BLOCK_CURSOR(var, nodeptr)                                                                              \
	struct block_cursor var = {                                                                                        \
	    .node = nodeptr,                                                                                               \
	    .level = -1,                                                                                                   \
	};

// Most significant bit
static int msb(unsigned int n) {
	// I'm pretty sure gcc -O>1 compiles this down to a single BSR instruction.
	// Godbolt says it does, in which case this happens to be fast enough.
	// In any case, this is faster than log2().
	unsigned r = 0;
	while (n >>= 1)
		r++;

	return r;
}

// msb(x) == floor(log2(x))
#define MAX_POINTERS_NBITS msb(BLOCK_MAX_POINTERS)
#define BLOCK_SIZE_NBITS msb(BLOCK_SIZE)
// floor-log base MAX_POINTERS
#define FLOG(x) (msb(x) / MAX_POINTERS_NBITS)
// This bit-twiddling is why BLOCK_SIZE must be a power of 2.
#define MAX_POINTERS_POW(e) (e == 0 ? 1 : BLOCK_MAX_POINTERS << (MAX_POINTERS_NBITS * (e - 1)))

/**
 * Set the position of a cursor for random access.
 */
static blkoff_t block_seek(struct block_cursor *cursor, blkoff_t pos) {
	blkoff_t nblocks = cursor->node->nblocks;
	cursor->i = pos;
	cursor->level = -1;

	if (pos > nblocks)
		return -1;
	if (pos < DIRECT_BLOCKS)
		return cursor->node->blocks[pos];

	// Start from 0
	pos -= DIRECT_BLOCKS;
	cursor->level = FLOG(pos + 1 - (pos + 1) / BLOCK_MAX_POINTERS);
	blkoff_t accum = BLOCK_MAX_POINTERS * (MAX_POINTERS_POW(cursor->level) - 1) / (BLOCK_MAX_POINTERS - 1);
	blkoff_t offset = pos - accum;

	cursor->block[0] = cursor->node->iblocks[cursor->level];

	for (int i = 0; i < cursor->level; i++) {
		blkcnt_t N = MAX_POINTERS_POW(cursor->level - i);
		cursor->pos[i] = offset / N;
		offset %= N;
		cursor->block[i + 1] = BLOCK_POINTERS((cursor)->block[i])[(cursor)->pos[i]];
	}

	cursor->pos[cursor->level] = offset;

	for (int i = cursor->level + 1; i < ILEVELS; i++)
		cursor->pos[i] = 0;

	return CURRENT_BLOCK(cursor);
}

/**
 * Iterate a cursor through a callback.
 *
 * Useful for inspecting all blocks we iterate through, including between levels.
 */
static blkoff_t iter_through(struct block_cursor *cursor,
                             blkoff_t (*callback)(struct block_cursor *cursor, int level)) {
	if (++cursor->i < DIRECT_BLOCKS)
		return callback(cursor, -1);

	int level = cursor->level;
	while (level >= 0 && ++cursor->pos[level] >= BLOCK_MAX_POINTERS)
		cursor->pos[level--] = 0;

	if (level == -1)
		// Done with this level, go to next level.
		cursor->level += 1;

	for (; level < cursor->level; level++)
		cursor->block[level + 1] = callback(cursor, level);

	return callback(cursor, cursor->level);
}

/**
 * Callback for simply getting the block a cursor is sitting on.
 */
static blkoff_t _next_block_callback(struct block_cursor *cursor, int level) {
	if (cursor->i >= cursor->node->nblocks)
		return END_BLOCKS;
	if (cursor->i < DIRECT_BLOCKS)
		return cursor->node->blocks[cursor->i];
	if (level == -1)
		return cursor->node->iblocks[cursor->level];

	return BLOCK_POINTERS((cursor)->block[level])[(cursor)->pos[level]];
}

/**
 * Sequential access.
 */
#define next_block(cursor) iter_through(cursor, _next_block_callback)

int tfs_open(const char *filename) {
	int fd = open(filename, O_RDWR);
	if (fd == -1)
		return -errno;

	// Memory map the file.
	// This will work for most x86-64 machines, but I'm not so sure about much else...
	tfs_info.filesize = lseek(fd, 0, SEEK_END);
	tfs_info.base = mmap(NULL, tfs_info.filesize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);

	if (tfs_info.base == MAP_FAILED)
		return -errno;

	return 0;
}

void tfs_format() {
	struct tfs_header *header = tfs_info.base;
	// Allocate the FAT (implicitly), blocks, and nodes.
	header->nblocks = tfs_info.filesize / (BLOCK_SIZE + (sizeof(struct tfs_node) / BLOCKS_PER_NODE));
	header->nnodes = header->nblocks / BLOCKS_PER_NODE;
	// Root takes 1 node.
	header->free_node_head = 1;
	header->free_block_head = 0;

	// Now (re)calculate pointers to FAT n' stuff.
	tfs_init();

	// Initialize root node:
	struct tfs_node *root = &tfs_info.nodes[0];
	root->mode = S_IFDIR | 644;
	root->name[0] = '\0'; // Root has no name.
	root->nblocks = 0;
	root->nlink = 0;
	clock_gettime(CLOCK_REALTIME, &root->atim);
	root->mtim = root->atim;

	// Initialize free blocks:
	for (int i = *tfs_info.free_block_head; i < tfs_info.nblocks - 1; i++)
		NEXT_FREE_BLOCK(i) = i + 1;
	NEXT_FREE_BLOCK(tfs_info.nblocks - 1) = END_BLOCKS;

	// Initialize free nodes:
	for (int i = *tfs_info.free_node_head; i < tfs_info.nnodes - 1; i++)
		tfs_info.nodes[i].next = i + 1;
	tfs_info.nodes[tfs_info.nnodes - 1].next = END_NODES;
}

/**
 * Walk the entire filesystem, adding all nodes to the hash table.
 */
static void init_htable(const char *path, struct tfs_node *node) {
	ENTRY entry;

	if (path) {
		entry.key = malloc(strlen(path) + 1 + strlen(node->name) + 1);
		sprintf(entry.key, "%s/%s", path, node->name);
	} else {
		// Special case for root
		entry.key = strdup("/");
	}

	entry.data = node;
	hsearch(entry, ENTER);

	fprintf(stderr, "found %s\n", entry.key);

	if (!(node->mode & S_IFDIR))
		return;

	// Recurse through directory:
	struct tfs_node **children = tfs_node_children(node);

	for (int i = 0; i < node->nlink; i++)
		init_htable(path ? entry.key : "", children[i]);

	free(children);
}

void tfs_init() {
	struct tfs_header *header = tfs_info.base;

	tfs_info.nblocks = header->nblocks;
	tfs_info.nnodes = header->nnodes;
	tfs_info.free_block_head = &header->free_block_head;
	tfs_info.free_node_head = &header->free_node_head;
	tfs_info.nodes = tfs_info.base + sizeof(struct tfs_header);
	tfs_info.data = (void *)tfs_info.nodes + sizeof(struct tfs_node) * tfs_info.nnodes;

	fprintf(stderr, "nblocks: %ld\n", tfs_info.nblocks);
	fprintf(stderr, "nnodes: %ld\n", tfs_info.nnodes);
	fprintf(stderr, "free_node_head: %ld\n", *tfs_info.free_node_head);
	fprintf(stderr, "free_block_head: %ld\n", *tfs_info.free_block_head);
}

int tfs_load(const char *filename) {
	int ret = tfs_open(filename);
	if (ret)
		return ret;

	tfs_init();

	hcreate(tfs_info.nnodes); // Initialize hash table, see hsearch(3)
	init_htable(NULL, &tfs_info.nodes[0]);

	return ret;
}

struct tfs_node *get_node(const char *path) {
	ENTRY entry = {
	    .key = strdup(path),
	};
	ENTRY *result = hsearch(entry, FIND);
	free(entry.key);

	if (!result)
		return NULL;

	return result->data;
}

struct tfs_node *get_directory(const char *path) {
	char *pathc = strdup(path);
	char *parent_path = dirname(pathc);
	struct tfs_node *parent_node = get_node(parent_path);
	free(pathc);
	return parent_node;
}

/**
 * Set or update a node give a path.
 *
 * Especially useful for unsetting a node by passing NULL.
 */
static void set_node(const char *path, struct tfs_node *node) {
	ENTRY entry = {
	    .key = strdup(path),
	    .data = node,
	};
	ENTRY *result = hsearch(entry, FIND);

	if (!result) {
		hsearch(entry, ENTER);
		return;
	}

	result->data = node;
	free(entry.key);
}

/**
 * Iterator callback for freeing blocks we iterate through.
 *
 * WARNING extreme hacks, detailed in report.
 */
static blkoff_t free_block_buffer[ILEVELS + 1];
static blkoff_t _free_callback(struct block_cursor *cursor, int level) {
	return free_block_buffer[level + 1] = _next_block_callback(cursor, level);
}

/**
 * Iterator callback for allocating and setting blocks.
 *
 * Also kind of hacky...
 */
static blkoff_t _alloc_callback(struct block_cursor *cursor, int level) {
	blkoff_t block = *tfs_info.free_block_head;

	if (block == END_BLOCKS)
		return -1;

	*tfs_info.free_block_head = NEXT_FREE_BLOCK(block);

	if (cursor->i < DIRECT_BLOCKS)
		return cursor->node->blocks[cursor->i] = block;
	if (level == -1)
		return cursor->node->iblocks[cursor->level] = block;

	return BLOCK_POINTERS((cursor)->block[level])[(cursor)->pos[level]] = block;
}

int tfs_node_trim(struct tfs_node *node) {
	blkoff_t nrblocks = NODE_NRBLOCKS(node);
	blkoff_t dblocks = nrblocks - node->nblocks;

	DEFINE_BLOCK_CURSOR(cursor, node);

	if (dblocks < 0) {
		block_seek(&cursor, nrblocks - 1);
		while (dblocks && iter_through(&cursor, _free_callback) != END_BLOCKS) {
			dblocks += 1;

			// Clear out free block buffer:
			for (int i = 0; i <= cursor.level + 1; i++) {
				if (free_block_buffer[i] < 0)
					continue;
				NEXT_FREE_BLOCK(free_block_buffer[i]) = *tfs_info.free_block_head;
				*tfs_info.free_block_head = free_block_buffer[i];
				free_block_buffer[i] = -1;
			}
		}
		node->nblocks = nrblocks;
	} else {
		block_seek(&cursor, node->nblocks - 1);
		while (dblocks && iter_through(&cursor, _alloc_callback) != END_BLOCKS)
			dblocks -= 1;
		node->nblocks = nrblocks - dblocks;
	}

	// In case we couldn't allocate enough blocks, set sizes correctly.
	if (node->mode & S_IFDIR)
		node->nlink = MIN(node->nlink, node->nblocks * BLOCK_MAX_CHILDREN);
	else
		node->size = MIN(node->size, node->nblocks * BLOCK_SIZE);

	return dblocks > 0 ? -ENOSPC : 0;
}

int tfs_node_read(struct tfs_node *node, char *buf, size_t size, off_t offset) {
	DEFINE_BLOCK_CURSOR(cursor, node);
	size_t chunk, to_read = size;
	blkoff_t block = block_seek(&cursor, offset / BLOCK_SIZE);

	while (offset < NODE_SIZE(node) && (chunk = MIN(to_read, BLOCK_SIZE - (offset % BLOCK_SIZE)))) {
		memcpy(buf, &tfs_info.data[block][offset % BLOCK_SIZE], MIN(chunk, NODE_SIZE(node) - offset));
		block = next_block(&cursor);
		to_read -= chunk;
		offset += chunk;
		buf += chunk;
	}

	clock_gettime(CLOCK_REALTIME, &node->atim);

	return size - to_read;
}

int tfs_node_write(struct tfs_node *node, const char *buf, size_t size, off_t offset) {
	node->size = MAX(node->size, offset + size);
	int ret = tfs_node_trim(node);

	DEFINE_BLOCK_CURSOR(cursor, node);
	size_t chunk, to_write = size;
	blkoff_t block = block_seek(&cursor, offset / BLOCK_SIZE);

	while (offset < NODE_SIZE(node) && (chunk = MIN(to_write, BLOCK_SIZE - (offset % BLOCK_SIZE)))) {
		memcpy(&tfs_info.data[block][offset % BLOCK_SIZE], buf, chunk);
		block = next_block(&cursor);
		to_write -= chunk;
		offset += chunk;
		buf += chunk;
	}

	clock_gettime(CLOCK_REALTIME, &node->mtim);

	return ret < 0 ? ret : size - to_write;
}

struct tfs_node **tfs_node_children(struct tfs_node *node) {
	nodoff_t *children = malloc(NODE_SIZE(node));
	if (!children)
		return NULL;

	tfs_node_read(node, (void *)children, NODE_SIZE(node), 0);

	struct tfs_node **children_nodes = malloc(node->nlink * sizeof(struct tfs_node *));
	if (!children_nodes)
		return NULL;

	for (int i = 0; i < node->nlink; i++)
		children_nodes[i] = &tfs_info.nodes[children[i]];

	free(children);

	return children_nodes;
}

int tfs_add_node(const char *path, mode_t mode) {
	if (get_node(path))
		return -EEXIST;
	if (*tfs_info.free_node_head == END_NODES)
		return -ENOSPC;

	char *basename = strrchr(path, '/') + 1;
	if (strlen(basename) + 1 > NAME_LIMIT)
		return -ENAMETOOLONG;

	// Allocate node.
	nodoff_t nodei = *tfs_info.free_node_head;
	struct tfs_node *node = &tfs_info.nodes[nodei];
	*tfs_info.free_node_head = node->next;
	fprintf(stderr, "\tAllocated node %ld...\n", nodei);

	// Initialize node.
	strcpy(node->name, basename);
	node->mode = mode;
	if (node->mode & S_IFDIR)
		node->nlink = 0;
	else
		node->size = 0;
	node->nblocks = 0;
	clock_gettime(CLOCK_REALTIME, &node->atim);
	node->mtim = node->atim;

	// Add child to parent.
	struct tfs_node *parent_node = get_directory(path);
	parent_node->nlink += 1;
	tfs_node_trim(parent_node);
	DEFINE_BLOCK_CURSOR(cursor, parent_node);
	BLOCK_NODES(block_seek(&cursor, parent_node->nblocks - 1))
	[(parent_node->nlink - 1) % BLOCK_MAX_CHILDREN] = nodei;

	clock_gettime(CLOCK_REALTIME, &parent_node->mtim);

	// Update hash table.
	set_node(path, node);

	return 0;
}

int tfs_remove_node(const char *path) {
	struct tfs_node *node = get_node(path);
	struct tfs_node *parent_node = get_directory(path);

	// Don't rm -rf / -_-
	if (!parent_node)
		return -ENOTSUP;

	// Remove from parent.
	DEFINE_BLOCK_CURSOR(cursor, parent_node);
	nodoff_t last_child =
	    BLOCK_NODES(block_seek(&cursor, parent_node->nblocks - 1))[(parent_node->nlink - 1) % BLOCK_MAX_CHILDREN];

	for (blkoff_t block = block_seek(&cursor, 0); block != END_BLOCKS; block = next_block(&cursor)) {
		for (int i = 0; i < BLOCK_MAX_CHILDREN; i++) {
			if (BLOCK_NODES(block)[i] == NODENO(node)) {
				BLOCK_NODES(block)[i] = last_child;
				goto outer;
			}
		}
	}
outer:
	parent_node->nlink -= 1;
	tfs_node_trim(parent_node);
	clock_gettime(CLOCK_REALTIME, &parent_node->mtim);

	// Deallocate blocks.
	node->size = 0;
	tfs_node_trim(node);

	// Deallocate node.
	node->next = *tfs_info.free_node_head;
	*tfs_info.free_node_head = NODENO(node);

	// Remove from hash table.
	set_node(path, NULL);

	return 0;
}

int tfs_destroy() {
	// Write back changes to disk.
	return munmap(tfs_info.base, tfs_info.filesize);
}
