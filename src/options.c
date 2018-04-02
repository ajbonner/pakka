#include "common.h"

int parseopts(int argc, char *argv[], opts_t *opts) {
	int c;

	if (argc < 2) {
		usage();
	}

	while ((c = getopt(argc, argv, "lxcadhf:C:")) != -1) {
		switch (c) {
		case 'l':
			setmodetype(opts, PAK_LIST);
			break;
		case 'x':
			setmodetype(opts, PAK_EXTRACT);
			break;
		case 'c':
			setmodetype(opts, PAK_CREATE);
			break;
		case 'a':
			setmodetype(opts, PAK_ADD);
			break;
		case 'd':
			setmodetype(opts, PAK_REMOVE);
			break;
		case 'f':
			opts->pakfile = optarg;
			break;
		case 'C':
			opts->destination = optarg;
			break;
		case 'h':
			help();
			break;
		}
	}

	if (optind < argc) {
		opts->paths = (char **)malloc(sizeof(char *) * (argc - optind));
		while (optind < argc) {
			opts->paths[opts->path_count++] = argv[optind++];
		}
	}

	if (!opts->mode) {
		fprintf(stderr, "You must specify one -lxcad option\n");
		usage();
	}

	if (!opts->pakfile) {
		fprintf(stderr, "You must specify a pakfile name with -f\n");
		usage();
	}

	return 0;
}

void setmodetype(opts_t *opt, short mode) {
	if (!opt->mode) {
		opt->mode = mode;
	} else {
		fprintf(stderr, "You may not specify more than one -lxcad option\n");
		usage();
	}
}
