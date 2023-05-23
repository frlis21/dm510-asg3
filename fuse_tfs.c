#define FUSE_USE_VERSION 29

#include <assert.h>
#include <errno.h>
#include <fuse.h>
#include <libgen.h>
#include <math.h>
#include <search.h> // hsearch
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tfs.h"

static int fuse_tfs_getattr(const char *path, struct stat *stbuf) {
	fprintf(stderr, "getattr %s\n", path);
	memset(stbuf, 0, sizeof(struct stat));

	struct tfs_node *node = get_node(path);
	if (!node)
		return -ENOENT;

	stbuf->st_mode = node->mode;
	stbuf->st_nlink = (node->mode & S_IFDIR) ? node->nlink + 1 : 1;
	stbuf->st_size = NODE_SIZE(node);
	stbuf->st_atim = node->atim;
	stbuf->st_mtim = node->mtim;

	return 0;
}

static int fuse_tfs_mknod(const char *path, mode_t mode, dev_t rdev) {
	fprintf(stderr, "mknod %s\n", path);
	return tfs_add_node(path, mode);
}

static int fuse_tfs_mkdir(const char *path, mode_t mode) {
	fprintf(stderr, "mkdir %s\n", path);
	// Bitwise OR with S_IFDIR because the documentation says to.
	return tfs_add_node(path, mode | S_IFDIR);
}

static int fuse_tfs_unlink(const char *path) {
	fprintf(stderr, "unlink %s\n", path);
	struct tfs_node *node = get_node(path);
	if (!node)
		return -ENOENT;
	if (node->mode & S_IFDIR)
		return -EISDIR;

	return tfs_remove_node(path);
}

static int fuse_tfs_rmdir(const char *path) {
	fprintf(stderr, "rmdir %s\n", path);
	struct tfs_node *node = get_node(path);
	if (!node)
		return -ENOENT;
	if (!(node->mode & S_IFDIR))
		return -ENOTDIR;
	if (node->size > 0)
		return -ENOTEMPTY;

	return tfs_remove_node(path);
}

static int fuse_tfs_truncate(const char *path, off_t size) {
	fprintf(stderr, "truncate %s\n", path);
	struct tfs_node *node = get_node(path);
	if (!node)
		return -ENOENT;
	if (node->mode & S_IFDIR)
		return -EISDIR;

	node->size = size;

	return tfs_node_trim(node);
}

static int fuse_tfs_open(const char *path, struct fuse_file_info *fi) {
	fprintf(stderr, "open %s\n", path);
	if (!get_node(path))
		return -ENOENT;

	return 0;
}

static int fuse_tfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
	fprintf(stderr, "read %s\n", path);
	struct tfs_node *node = get_node(path);
	if (!node)
		return -ENOENT;
	if (node->mode & S_IFDIR)
		return -EISDIR;

	return tfs_node_read(node, buf, size, offset);
}

static int fuse_tfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
	fprintf(stderr, "write %s\n", path);
	struct tfs_node *node = get_node(path);
	if (!node)
		return -ENOENT;
	if (node->mode & S_IFDIR)
		return -EISDIR;

	return tfs_node_write(node, buf, size, offset);
}

static int fuse_tfs_release(const char *path, struct fuse_file_info *fi) {
	fprintf(stderr, "release %s\n", path);
	return 0; // no-op
}

static int fuse_tfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
                            struct fuse_file_info *fi) {
	fprintf(stderr, "readdir %s\n", path);
	struct tfs_node *node = get_node(path);
	if (!node)
		return -ENOENT;
	if (node->mode & S_IFREG)
		return -ENOTDIR;

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);

	struct tfs_node **children = tfs_node_children(node);

	for (int i = 0; i < node->nlink; i++) {
		struct tfs_node *child = children[i];

		filler(buf, child->name, NULL, 0);
	}

	free(children);

	return 0;
}

static void fuse_tfs_destroy(void *data) {
	tfs_destroy();
	hdestroy();
}

static int fuse_tfs_utimens(const char *path, const struct timespec tv[2]) {
	fprintf(stderr, "utimens %s\n", path);
	struct tfs_node *node = get_node(path);
	if (!node)
		return -ENOENT;

	node->atim = tv[0];
	node->mtim = tv[1];

	return 0;
}

struct tfs_config {
	char *tfs_file_path;
};

enum {
	KEY_HELP,
};

// We intercept the help flag.
static struct fuse_opt tfs_opts[] = {FUSE_OPT_KEY("-h", KEY_HELP), FUSE_OPT_KEY("--help", KEY_HELP), FUSE_OPT_END};

static int tfs_opt_proc(void *data, const char *arg, int key, struct fuse_args *outargs) {
	struct tfs_config *config = data;

	switch (key) {
	case KEY_HELP:
		fprintf(stderr,
		        "usage: %s file mountpoint [fuse options]\n"
		        "\n"
		        "`file` must exist and must be initialized with `mktfs`."
		        "\n"
		        "See fuse(8) for more options.\n",
		        outargs->argv[0]);
		return -1;
	case FUSE_OPT_KEY_NONOPT:
		if (config->tfs_file_path)
			return 1;

		config->tfs_file_path = strdup(arg);
		return 0;
	}

	return 1;
}

static struct fuse_operations fuse_tfs_oper = {.getattr = fuse_tfs_getattr,
                                               .mknod = fuse_tfs_mknod,
                                               .mkdir = fuse_tfs_mkdir,
                                               .unlink = fuse_tfs_unlink,
                                               .rmdir = fuse_tfs_rmdir,
                                               .truncate = fuse_tfs_truncate,
                                               .open = fuse_tfs_open,
                                               .read = fuse_tfs_read,
                                               .write = fuse_tfs_write,
                                               .release = fuse_tfs_release,
                                               .readdir = fuse_tfs_readdir,
                                               .destroy = fuse_tfs_destroy,
                                               .utimens = fuse_tfs_utimens};

int main(int argc, char *argv[]) {
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct tfs_config config = {.tfs_file_path = NULL};

	if (fuse_opt_parse(&args, &config, tfs_opts, tfs_opt_proc) == -1)
		return 1;

	if (!config.tfs_file_path) {
		fprintf(stderr, "tfs: missing file to mount\n");
		return 1;
	}

	int ret = tfs_load(config.tfs_file_path);
	if (ret)
		return ret;

	return fuse_main(args.argc, args.argv, &fuse_tfs_oper, NULL);
}
