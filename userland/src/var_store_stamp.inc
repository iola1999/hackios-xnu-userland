/*
 * var_store watch / stamp / seed helpers (included by var_store.c).
 */

#define NOTIFY_STATUS_OK 0
#define NOTIFY_TRIGGER_VAR "/private/var/run/com.redwood.var_save"
#define NOTIFY_TRIGGER_TMP "/tmp/com.redwood.var_save"

static int
notify_register_check(const char *name, int *out_token)
{
	(void)name;
	if (out_token != NULL) {
		*out_token = 1;
	}
	return NOTIFY_STATUS_OK;
}

static int
notify_check(int token, int *check)
{
	struct stat st;
	int state = VAR_SAVE_NONE;

	(void)token;
	if (check == NULL) {
		return NOTIFY_STATUS_OK;
	}
	*check = 0;

	if (stat(NOTIFY_TRIGGER_VAR, &st) == 0) {
		unlink(NOTIFY_TRIGGER_VAR);
		*check = 1;
		return NOTIFY_STATUS_OK;
	}
	if (stat(NOTIFY_TRIGGER_TMP, &st) == 0) {
		unlink(NOTIFY_TRIGGER_TMP);
		*check = 1;
		return NOTIFY_STATUS_OK;
	}

	if (sysctl_int("kern.redwood.var_save", &state) == 0 && state == VAR_SAVE_WANTED) {
		*check = 1;
		return NOTIFY_STATUS_OK;
	}

	return NOTIFY_STATUS_OK;
}

static int
save_once(void)
{
	int ok;

	set_background_qos();
	fd = open(DEVICE, O_RDWR, 0);
	if (fd < 0) {
		printf("var_store: %s: %s\n", DEVICE, strerror(errno));
		return 0;
	}
	ok = claim() && save();
	if (ok) {
		klog_flush();
	}
	release();
	close(fd);
	fd = -1;
	return ok;
}

static int
delta_save_once(void)
{
	int ok;

	set_background_qos();
	fd = open(DEVICE, O_RDWR, 0);
	if (fd < 0) {
		printf("var_store: %s: %s\n", DEVICE, strerror(errno));
		return 0;
	}
	ok = claim() && delta_save();
	if (ok) {
		klog_flush();
	}
	release();
	close(fd);
	fd = -1;
	return ok;
}

static int
watch(int seconds)
{
	struct dir_snapshot prev_snap, curr_snap;
	uint64_t last_saved = 0;
	uint64_t last_checked = 0;
	uint64_t last_klog = 0;
	int interval = seconds > 0 ? seconds : 3;
	int token = 0;
	int asked = 0;

	set_background_qos();
	notify_register_check("com.redwood.var_save", &token);

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
	printf("var_store: watching: auto snapshot check every %d s, notify listener active\n", interval);

	compute_snapshot(&prev_snap);
	{
		struct timeval tv = { 0, 0 };
		gettimeofday(&tv, NULL);
		last_saved = (uint64_t)tv.tv_sec;
		last_checked = (uint64_t)tv.tv_sec;
		last_klog = (uint64_t)tv.tv_sec;
	}

	for (;;) {
		struct timeval tv = { 0, 0 };
		int state = VAR_SAVE_NONE;
		int notify_triggered = 0;

		if (sysctl_int("kern.redwood.var_save", &state) == 0 && state == VAR_SAVE_WANTED) {
			asked = 1;
		}

		notify_check(token, &notify_triggered);
		gettimeofday(&tv, NULL);

		if (asked) {
			int ok = save_once();
			if (!ok) {
				printf("var_store: this save came to nothing; the store keeps what it had\n");
			}
			(void)sysctl_set_int("kern.redwood.var_save", VAR_SAVE_DONE);
			return ok;
		}

		if (notify_triggered) {
			printf("var_store: save triggered by com.redwood.var_save notification\n");
			int ok = 0;
			if (delta_is_near_full()) {
				printf("var_store: delta slot near full (>=60 MiB): merging base slot\n");
				ok = save_once();
			} else {
				ok = delta_save_once();
			}
			if (ok) {
				last_saved = (uint64_t)tv.tv_sec;
				compute_snapshot(&prev_snap);
			}
		} else if ((uint64_t)tv.tv_sec - last_checked >= (uint64_t)interval) {
			last_checked = (uint64_t)tv.tv_sec;
			compute_snapshot(&curr_snap);

			if (snapshot_is_dirty(&curr_snap, &prev_snap)) {
				if ((uint64_t)tv.tv_sec - last_saved >= 2) {
					printf("var_store: changes detected in watched paths, performing incremental save\n");
					int ok = 0;
					if (delta_is_near_full()) {
						printf("var_store: delta slot near full (>=60 MiB): merging base slot\n");
						ok = save_once();
					} else {
						ok = delta_save_once();
					}
					if (ok) {
						last_saved = (uint64_t)tv.tv_sec;
						prev_snap = curr_snap;
					}
				}
			}
		}

		if ((uint64_t)tv.tv_sec - last_klog >= 25) {
			last_klog = (uint64_t)tv.tv_sec;
			fd = open(DEVICE, O_RDWR, 0);
			if (fd >= 0) {
				if (claim()) {
					klog_flush();
					release();
				}
				close(fd);
				fd = -1;
			}
		}

		usleep(100000);
	}
}

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
