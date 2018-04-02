typedef struct opts_s {
	short mode;
	char *pakfile;
	char *destination;
	char **paths;
	int path_count;
} opts_t;

int parseopts(int argc, char *argv[], opts_t *opts);
void setmodetype(opts_t *opts, short mode);
