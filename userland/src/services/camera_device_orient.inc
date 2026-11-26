/*
 * Orientation: the stream's DeviceOrientation property, the photographs' ExifOrientation
 * attachment, and the test rigging's switches for both (included by camera_device.c).
 *
 * The frames go out in the sensor's own orientation: righting them is the capture pipeline's,
 * whose algebra assumes sensor-native input (camera-orientation.md §3). What used to be the
 * front camera's pixel-domain 180-degree rotation, and the constant ExifOrientation 6 on every
 * photograph, are kept only as the rigging's comparison cases, switchable within one boot:
 * the environment variable, or the file in /private/var/tmp for a daemon whose environment is
 * not the rig's to set, as inject_media.c's fig_notes does. The switch is read once when a
 * stream's worker starts, so closing and opening the camera is enough to change it.
 */

/* the rigging's switches, orientation's own or another concern's: env set and not "0", or the file there at all */
static bool
rigging_switch_on(const char *env, const char *path)
{
	const char *v = getenv(env);

	return (v != NULL && v[0] != '\0' && strcmp(v, "0") != 0) || access(path, F_OK) == 0;
}

/* env's value, else the file's first bytes; NULL: neither */
static const char *
rigging_switch_value(const char *env, const char *path, char *buf, size_t bytes)
{
	const char *v = getenv(env);
	int fd;

	if (v != NULL) {
		return v;
	}
	fd = open(path, O_RDONLY);
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
 * The DeviceOrientation the pipeline sets on the stream, kept for the photographs'
 * ExifOrientation and said of: whether it comes at all, and at what pace, is itself unknown
 * before the rigging's round (camera-orientation.md §5.3).
 */
static void
device_orientation_set(struct stream *s, CFTypeRef value)
{
	int64_t orientation = 0;
	bool changed;

	if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
		CFNumberGetValue(value, kCFNumberSInt64Type, &orientation);
	}
	pthread_mutex_lock(&s->lock);
	changed = orientation != s->device_orientation;
	s->device_orientation = orientation;
	pthread_mutex_unlock(&s->lock);
	if (changed) {
		say("stream: device orientation %lld", (long long)orientation);
	}
}

/*
 * The ExifOrientation of a sensor-native frame with the device held so (the numbers
 * backboardd's rawOrientation uses, 1 portrait, 2 upside down, 3 and 4 the two landscapes):
 * the sensor lies across the phone, upright with the phone's top to the left, so the picture
 * of a portrait hold is turned clockwise a quarter, of an upside-down hold counterclockwise a
 * quarter, of the other landscape half round. Front or back the same here: the sensor-mounted
 * compensation is the pipeline's own, applied on top (§3.4). 0: nothing to go by. The 3/4
 * assignment is a hypothesis of the numbering; the rigging's round reads it off the said values
 * against what the files come out as.
 */
static int
exif_from_device_orientation(int64_t orientation)
{
	switch (orientation) {
	case 1:
		return 6;
	case 2:
		return 8;
	case 3:
		return 1;
	case 4:
		return 3;
	default:
		return 0;
	}
}

/*
 * What a photograph's ExifOrientation attachment is to be (0: none written): the rigging's
 * "auto", or a number 1 to 8 as a constant (6 is what used to be written). Default -- neither,
 * or anything else -- is none: whether the pipeline then computes the orientation itself is
 * the round's question.
 */
static int
orient_exif_mode(void)
{
	char buf[16];
	const char *v = rigging_switch_value("ANNIBALE_EXIF_ORIENTATION", "/private/var/tmp/annibale_exif_orientation",
	    buf, sizeof(buf));

	if (v != NULL) {
		if (strncmp(v, "auto", 4) == 0 && (v[4] == '\0' || v[4] == '\n')) {
			return -1;
		}
		if (v[0] >= '1' && v[0] <= '8' && (v[1] == '\0' || v[1] == '\n')) {
			return v[0] - '0';
		}
	}
	return 0;
}

/* the photograph's ExifOrientation under the stream's mode (called under s->lock, as the shots' envs are made) */
static int32_t
still_exif_orientation(const struct stream *s)
{
	if (s->exif_mode > 0) {
		return s->exif_mode;
	}
	if (s->exif_mode < 0) {
		return exif_from_device_orientation(s->device_orientation);
	}
	return 0;
}
