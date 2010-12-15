/*
 * Copyright (C) 2009 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#define _XOPEN_SOURCE 500
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include "kerncompat.h"
#include "ctree.h"
#include "volumes.h"
#include "disk-io.h"
#include "print-tree.h"
#include "transaction.h"
#include "list.h"
#include "version.h"

/* we write the mirror info to stdout unless they are dumping the data
 * to stdout
 * */
static FILE *info_file;

struct extent_buffer *debug_read_block(struct btrfs_root *root, u64 bytenr,
				     u32 blocksize, int copy)
{
	int ret;
	int dev_nr;
	struct extent_buffer *eb;
	u64 length;
	struct btrfs_multi_bio *multi = NULL;
	struct btrfs_device *device;
	int num_copies;
	int mirror_num = 1;

	eb = btrfs_find_create_tree_block(root, bytenr, blocksize);
	if (!eb)
		return NULL;

	dev_nr = 0;
	length = blocksize;
	while (1) {
		ret = btrfs_map_block(&root->fs_info->mapping_tree, READ,
				      eb->start, &length, &multi, mirror_num);
		BUG_ON(ret);
		device = multi->stripes[0].dev;
		eb->fd = device->fd;
		device->total_ios++;
		eb->dev_bytenr = multi->stripes[0].physical;

		fprintf(info_file, "mirror %d logical %Lu physical %Lu "
			"device %s\n", mirror_num, bytenr, eb->dev_bytenr,
			device->name);
		kfree(multi);

		if (!copy || mirror_num == copy)
			ret = read_extent_from_disk(eb);

		num_copies = btrfs_num_copies(&root->fs_info->mapping_tree,
					      eb->start, eb->len);
		if (num_copies == 1)
			break;

		mirror_num++;
		if (mirror_num > num_copies)
			break;
	}
	return eb;
}

static void print_usage(void)
{
	fprintf(stderr, "usage: btrfs-map-logical [options] mount_point\n");
	fprintf(stderr, "\t-l Logical extent to map\n");
	fprintf(stderr, "\t-c Copy of the extent to read (usually 1 or 2)\n");
	fprintf(stderr, "\t-o Output file to hold the extent\n");
	fprintf(stderr, "\t-b Number of bytes to read\n");
	exit(1);
}

static struct option long_options[] = {
	/* { "byte-count", 1, NULL, 'b' }, */
	{ "logical", 1, NULL, 'l' },
	{ "copy", 1, NULL, 'c' },
	{ "output", 1, NULL, 'c' },
	{ "bytes", 1, NULL, 'b' },
	{ 0, 0, 0, 0}
};

int main(int ac, char **av)
{
	struct cache_tree root_cache;
	struct btrfs_root *root;
	struct extent_buffer *eb;
	char *dev;
	char *output_file = NULL;
	u64 logical = 0;
	int ret = 0;
	int option_index = 0;
	int copy = 0;
	u64 bytes = 0;
	int out_fd = 0;
	int err;

	while(1) {
		int c;
		c = getopt_long(ac, av, "l:c:o:b:", long_options,
				&option_index);
		if (c < 0)
			break;
		switch(c) {
			case 'l':
				logical = atoll(optarg);
				if (logical == 0) {
					fprintf(stderr,
						"invalid extent number\n");
					print_usage();
				}
				break;
			case 'c':
				copy = atoi(optarg);
				if (copy == 0) {
					fprintf(stderr,
						"invalid copy number\n");
					print_usage();
				}
				break;
			case 'b':
				bytes = atoll(optarg);
				if (bytes == 0) {
					fprintf(stderr,
						"invalid byte count\n");
					print_usage();
				}
				break;
			case 'o':
				output_file = strdup(optarg);
				break;
			default:
				print_usage();
		}
	}
	ac = ac - optind;
	if (ac == 0)
		print_usage();
	if (logical == 0)
		print_usage();
	if (copy < 0)
		print_usage();

	dev = av[optind];

	radix_tree_init();
	cache_tree_init(&root_cache);

	root = open_ctree(dev, 0, 0);
	if (!root) {
		fprintf(stderr, "Open ctree failed\n");
		exit(1);
	}

	if (output_file) {
		if (strcmp(output_file, "-") == 0) {
			out_fd = 1;
			info_file = stderr;
		} else {
			out_fd = open(output_file, O_RDWR | O_CREAT, 0600);
			if (out_fd < 0)
				goto close;
			err = ftruncate(out_fd, 0);
			if (err) {
				close(out_fd);
				goto close;
			}
			info_file = stdout;
		}
	}

	if (bytes == 0)
		bytes = root->sectorsize;

	bytes = (bytes + root->sectorsize - 1) / root->sectorsize;
	bytes *= root->sectorsize;

	while (bytes > 0) {
		eb = debug_read_block(root, logical, root->sectorsize, copy);
		if (eb && output_file) {
			err = write(out_fd, eb->data, eb->len);
			if (err < 0 || err != eb->len) {
				fprintf(stderr, "output file write failed\n");
				goto out_close_fd;
			}
		}
		free_extent_buffer(eb);
		logical += root->sectorsize;
		bytes -= root->sectorsize;
	}

out_close_fd:
	if (output_file && out_fd != 1)
		close(out_fd);
close:
	close_ctree(root);
	return ret;
}
