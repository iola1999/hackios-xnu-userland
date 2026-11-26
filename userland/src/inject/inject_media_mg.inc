/*
 * The front camera's mounting angle as MobileGestalt answers it inside the capture daemons
 * (included by inject_media.c).
 *
 * This is the experiment rig for a piece of hardware description: the value is K90's
 * front-cam-rotation-isp, and CMCapture's own orientation algebra consumes it
 * unchanged. The donor iPhone's device tree says 270; K90's sensor can have a different mounting
 * angle. ANNIBALE_FRONT_CAM_ROTATION_ISP, or for a daemon whose environment is not the rig's
 * to set /private/var/tmp/annibale_front_cam_rotation_isp, overrides the answer with one of the
 * four quarter-turn candidates. CMCapture reads the answer once per process; the watcher below
 * keeps that cached angle in step with the file so a new Camera.app session can use a changed value.
 */

#define FRONT_CAM_ROTATION_ENV     "ANNIBALE_FRONT_CAM_ROTATION_ISP"
#define FRONT_CAM_ROTATION_PATH    "/private/var/tmp/annibale_front_cam_rotation_isp"
/* CMCapture's cached frontCameraRotationAngle in the 23G90 shared cache, before the slide. */
#define FRONT_CAM_ANGLE_CACHE      0x1ED0F2D30UL

extern int32_t MGGetSInt32Answer(CFStringRef key, int32_t default_value);

/* env's value, else the file's first bytes; NULL: neither */
static const char *
front_cam_rotation_text(char *buf, size_t bytes)
{
	const char *v = getenv(FRONT_CAM_ROTATION_ENV);
	int fd;

	if (v != NULL) {
		return v;
	}
	fd = open(FRONT_CAM_ROTATION_PATH, O_RDONLY);
	if (fd < 0) {
		return NULL;
	}
	ssize_t n = read(fd, buf, bytes - 1);

	close(fd);
	if (n <= 0) {
		return NULL;
	}
	buf[n] = '\0';
	return buf;
}

/*
 * The quarter-turn value named by the rigging: 1 and out is set when valid, 0 when no override
 * is present, -1 when one is present but not a quarter turn.
 */
static int
front_cam_rotation_override(int *out)
{
	char buf[16];
	const char *v = front_cam_rotation_text(buf, sizeof(buf));
	int value = 0, digits = 0;

	if (v == NULL) {
		return 0;
	}
	while (*v >= '0' && *v <= '9') {
		value = value * 10 + *v++ - '0';
		digits++;
	}
	if (!digits || (*v != '\0' && *v != '\n') ||
	    (value != 0 && value != 90 && value != 180 && value != 270)) {
		return -1;
	}
	*out = value;
	return 1;
}

static int32_t
media_hook_MGGetSInt32Answer(CFStringRef key, int32_t default_value)
{
	if (key != NULL && CFEqual(key, CFSTR("FrontCameraRotationForISP"))) {
		int value = 0;
		int status = front_cam_rotation_override(&value);
		static int said_value = -2;
		static bool invalid_said;

		if (status > 0) {
			if (value != said_value) {
				said_value = value;
				invalid_said = false;
				say("annibale: media: FrontCameraRotationForISP %d (K90 mounting-angle rig)\n", value);
			}
			return value;
		}
		if (status < 0 && !invalid_said) {
			invalid_said = true;
			say("annibale: media: FrontCameraRotationForISP rig value invalid; the device tree answers\n");
		}
	}
	return MGGetSInt32Answer(key, default_value);
}

/*
 * CMCapture caches the angle once in each process. The interposed getter makes the first read use
 * the rigging's value where dyld interposition reaches it; this thread also keeps the cache itself
 * in step, so changing the file is enough for a new Camera.app session even if that first read has
 * already happened. The cache holds CMCapture's internal angle, normalize360(raw + 180), while the
 * file holds the hardware description's raw front-cam-rotation-isp value.
 */
static void *
front_cam_rotation_watch(void *unused)
{
	volatile int32_t *cached = (volatile int32_t *)(FRONT_CAM_ANGLE_CACHE + slide);

	(void)unused;
	int last_value = -2;
	bool invalid_said = false;

	for (;;) {
		int value = 0;
		int status = front_cam_rotation_override(&value);

		if (status > 0) {
			int32_t angle = (value + 180) % 360;
			if (*cached != angle) {
				*cached = angle;
			}
			if (value != last_value) {
				last_value = value;
				invalid_said = false;
				say("annibale: media: frontCameraRotationAngle cache %d for front-cam-rotation-isp %d\n",
				    angle, value);
			}
		} else if (status < 0 && !invalid_said) {
			invalid_said = true;
			say("annibale: media: front camera rotation rig value invalid; cache left alone\n");
		}
		usleep(100000);
	}
	return NULL;
}

static void
front_cam_rotation_watch_start(void)
{
	pthread_t thread;

	if (pthread_create(&thread, NULL, front_cam_rotation_watch, NULL) == 0) {
		pthread_detach(thread);
	} else {
		say("annibale: media: front camera rotation watch did not start\n");
	}
}

__attribute__((used)) static const struct {
	const void *ours, *theirs;
} mg_interpose[] __attribute__((section("__DATA,__interpose"))) = {
	{ media_hook_MGGetSInt32Answer, MGGetSInt32Answer },
};
