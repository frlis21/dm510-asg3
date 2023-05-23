#include "tfs.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]) {
	int ret = 0;

	if (argc < 2) {
		fprintf(stderr,
		        "usage: %s <file>\n"
		        "\n"
		        "Allocate space to a file using fallocate(1) first.\n",
		        argv[0]);
		return 1;
	}

	ret = tfs_open(argv[1]);
	if (ret)
		return ret;

	tfs_format();
	tfs_destroy();

	return 0;
}
