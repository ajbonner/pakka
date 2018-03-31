typedef struct opts_s {
	short mode;
	char *pakfile;
	char *destination;
	char **paths;
	int path_count;
} opts_t;

int parseopts(int, char **, opts_t *);
void setmodetype(opts_t *, short);
