/*
 * var_store watch / stamp / seed helpers (included by var_store.c).
 */

/*
 * Stay and save by itself. The kernel is told there is a saver, and then this waits: for the
 * kernel to ask (the run is ending, and it waits for the answer before it goes), and, if seconds
 * is given, for that long to pass between saves of its own accord. A save that fails is said so
 * and the waiting goes on -- the one at the end of the run still gets its chance.
 */
static int
save_once(void)
{
	int ok;

	/*
	 * The device is opened for the save and closed again, not held: while a write is in flight
	 * this kernel's own transfer list is out of the controller, and it goes back in when the
	 * device is closed (bsd/dev/redwood_ufs.c, ufs_cclose). A watcher that kept the device open
	 * would leave iOS reading the flash through the loader for the rest of the run.
	 */
	fd = open(DEVICE, O_RDWR, 0);
	if (fd < 0) {
		printf("var_store: %s: %s\n", DEVICE, strerror(errno));
		return 0;
	}
	ok = claim() && save();
	release();
	close(fd);
	fd = -1;
	return ok;
}

static int
watch(int seconds)
{
	uint64_t last = 0;
	int asked = 0;

	fd = open(DEVICE, O_RDWR, 0);
	if (fd < 0) {
		printf("var_store: %s cannot be written (%s): nothing would be saved\n", DEVICE,
		    strerror(errno));
		return 0;
	}
	close(fd);
	fd = -1;
	if (sysctl_set_int("kern.redwood.var_save", VAR_SAVE_IDLE) != 0) {
		printf("var_store: this kernel does not keep the data volume (%s)\n", strerror(errno));
		return 0;
	}
	printf("var_store: watching: the volume is saved when the run ends%s\n",
	    seconds > 0 ? ", and in between" : "");
	{
		struct timeval tv = { 0, 0 };

		gettimeofday(&tv, NULL);
		last = (uint64_t)tv.tv_sec;
	}
	for (;;) {
		struct timeval tv = { 0, 0 };
		int state = VAR_SAVE_NONE;

		if (sysctl_int("kern.redwood.var_save", &state) == 0 && state == VAR_SAVE_WANTED) {
			asked = 1;
		}
		gettimeofday(&tv, NULL);
		if (asked || (seconds > 0 && (uint64_t)tv.tv_sec - last >= (uint64_t)seconds)) {
			int ok = save_once();

			if (!ok) {
				printf("var_store: this save came to nothing; the store keeps what it had\n");
			}
			last = (uint64_t)tv.tv_sec;
			if (asked) {
				/* answered either way: the kernel is waiting on this to go back to the loader */
				(void)sysctl_set_int("kern.redwood.var_save", VAR_SAVE_DONE);
				return ok;
			}
		}
		usleep(100000);
	}
}

/* A line saying when, for a run to leave behind and the next one to find. */
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

/* etc/var.seed of the image this program was started from (argv[0] ends in bin/var_store), or NULL */
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
