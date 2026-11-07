/*
 * var_store stamp and seed helpers (included by var_store.c).
 */

static int
stamp(const char *where)
{
	struct timeval tv = { 0, 0 };
	char line[256];
	int out_fd = open(where, O_WRONLY | O_CREAT | O_APPEND, 0644);
	int n;

	if (out_fd < 0) {
		printf("var_store: %s: %s\n", where, strerror(errno));
		return 0;
	}
	gettimeofday(&tv, NULL);
	n = 0;
	{
		static const char head[] = "written by run of ";
		unsigned long long seconds = (unsigned long long)tv.tv_sec;
		char digits[24];
		int k = 0;

		memcpy(line, head, sizeof(head) - 1);
		n = (int)sizeof(head) - 1;
		do {
			digits[k++] = (char)('0' + seconds % 10);
			seconds /= 10;
		} while (seconds > 0);
		while (k-- > 0) {
			line[n++] = digits[k];
		}
		line[n++] = '\n';
	}
	if (write(out_fd, line, (size_t)n) != n) {
		printf("var_store: %s: %s\n", where, strerror(errno));
		close(out_fd);
		return 0;
	}
	close(out_fd);
	printf("var_store: %s: %d bytes added\n", where, n);
	return 1;
}

static const char *
seed(const char *self)
{
	static char at[MAX_PATH];
	size_t n = strlen(self), tail = strlen("bin/var_store");
	struct stat st;

	if (n < tail || n - tail + sizeof("etc/var.seed") > sizeof(at) || strcmp(self + n - tail, "bin/var_store") != 0) {
		return NULL;
	}
	memcpy(at, self, n - tail);
	strcpy(at + n - tail, "etc/var.seed");
	return stat(at, &st) == 0 ? at : NULL;
}
