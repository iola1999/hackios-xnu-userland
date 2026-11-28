/* Native profile installation and completed RAW10 frame reads. Included by camera_device.c. */
#include <sys/ioctl.h>
#include <sys/stat.h>

#define CAMERA_FRONT_PROFILE "/annibale/fw/camera-ov20b-front-mode4.tab"
#define CAMERA_BACK_PROFILE "/annibale/fw/camera-ovx8000-wide-mode24.tab"

/* close() releases the installed profile, so every device session installs it once. */
static int
camera_native_open(uint32_t sensor, struct annibale_camera_profile *profile)
{
	struct {
		struct annibale_camera_command command;
		uint8_t bytes[ANNIBALE_CAMERA_BACK_PROFILE_BYTES];
	} packet = { .command = { .command = ANNIBALE_CAMERA_PROFILE } };
	struct annibale_camera_info info;
	struct stat st;
	int source = -1, device = -1, error;
	size_t offset = 0;

	if (sensor != 1 && sensor != 2) {
		errno = ENOTSUP;
		return -1;
	}
	uint32_t profile_bytes = sensor == 1 ? ANNIBALE_CAMERA_BACK_PROFILE_BYTES : ANNIBALE_CAMERA_FRONT_PROFILE_BYTES;
	uint32_t mode = sensor == 1 ? 24u : 4u;
	uint32_t capability = sensor == 1 ? ANNIBALE_CAM_INFO_FLAG_BACK_MODE24 : ANNIBALE_CAM_INFO_FLAG_FRONT_MODE4;
	packet.command.bytes = profile_bytes;
	source = open(sensor == 1 ? CAMERA_BACK_PROFILE : CAMERA_FRONT_PROFILE, O_RDONLY);
	if (source < 0) {
		return -1;
	}
	if (fstat(source, &st) != 0) {
		goto fail;
	}
	if (st.st_size != profile_bytes) {
		errno = EINVAL;
		goto fail;
	}
	while (offset < profile_bytes) {
		ssize_t n = read(source, packet.bytes + offset, profile_bytes - offset);

		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			if (n == 0) errno = EIO;
			goto fail;
		}
		offset += (size_t)n;
	}
	close(source);
	source = -1;
	memcpy(profile, packet.bytes, sizeof(*profile));
	if (memcmp(profile->magic, ANNIBALE_CAMERA_PROFILE_MAGIC, sizeof(profile->magic)) != 0 ||
	    profile->bytes != profile_bytes || profile->sensor != sensor || profile->mode != mode) {
		errno = EINVAL;
		goto fail;
	}
	for (int tries = 0; tries < 20; tries++) {
		device = open(ANNIBALE_CAMERA_DEVICE, O_RDWR);
		if (device >= 0 || errno != EBUSY) {
			break;
		}
		usleep(25000);
	}
	if (device < 0 || ioctl(device, CAMIOC_G_INFO, &info) != 0) {
		goto fail;
	}
	if ((info.flags & capability) == 0) {
		errno = ENOTSUP;
		goto fail;
	}
	size_t packet_bytes = sizeof(packet.command) + profile_bytes;
	ssize_t written = write(device, &packet, packet_bytes);
	if (written != (ssize_t)packet_bytes) {
		if (written >= 0) errno = EIO;
		goto fail;
	}
	if (ioctl(device, CAMIOC_G_INFO, &info) != 0) {
		goto fail;
	}
	if ((info.flags & ANNIBALE_CAM_INFO_FLAG_NATIVE_PROFILE) == 0) {
		errno = EIO;
		goto fail;
	}
	return device;

fail:
	error = errno;
	if (source >= 0) close(source);
	if (device >= 0) close(device);
	errno = error;
	return -1;
}

/* Read cached rear camera EEPROM via paged CAMIOC_G_EEPROM ioctl. Returns bytes copied (0 on unavailable). */
static size_t
camera_native_read_eeprom(int device, uint8_t *out, size_t max_bytes)
{
	if (device < 0 || out == NULL || max_bytes == 0) {
		return 0;
	}
	size_t offset = 0, total = max_bytes;
	while (offset < total) {
		struct annibale_camera_eeprom req;
		memset(&req, 0, sizeof(req));
		req.offset = (uint32_t)offset;
		size_t want = total - offset;
		req.length = (uint32_t)(want > ANNIBALE_CAM_EEPROM_CHUNK ? ANNIBALE_CAM_EEPROM_CHUNK : want);
		if (ioctl(device, CAMIOC_G_EEPROM, &req) != 0) {
			return 0;
		}
		if ((req.flags & ANNIBALE_CAM_EEPROM_FLAG_VALID) == 0 || req.total_bytes == 0) {
			return 0;
		}
		if (req.total_bytes < total) {
			total = req.total_bytes;
		}
		if (req.length == 0 || req.length > ANNIBALE_CAM_EEPROM_CHUNK || offset + req.length > max_bytes) {
			break;
		}
		memcpy(out + offset, req.data, req.length);
		offset += req.length;
	}
	return offset;
}

/* A failed or HELD capture ends the worker; pixels are read only from a frame the kernel says is whole. */
static ssize_t
camera_native_frame(int device, const struct annibale_camera_profile *profile, void *buffer, size_t bytes,
    struct annibale_camera_capture *capture)
{
	memset(capture, 0, sizeof(*capture));
	if (ioctl(device, CAMIOC_CAPTURE, capture) != 0) {
		return -1;
	}
	if (!ANNIBALE_CAM_CAPTURE_HAS_FRAME(capture)) {
		errno = capture->error != 0 ? (int)capture->error : EIO;
		return -1;
	}
	size_t main_total = sizeof(struct annibale_camera_frame) + (size_t)capture->bytes;
	if (capture->width != profile->width || capture->height != profile->height ||
	    capture->stride != profile->stride || capture->bytes != (uint64_t)profile->stride * profile->height ||
	    bytes < main_total) {
		errno = EIO;
		return -1;
	}
	ssize_t n = read(device, buffer, bytes);
	if (n < (ssize_t)main_total) {
		if (n >= 0) errno = EIO;
		return -1;
	}
	const struct annibale_camera_frame *frame = buffer;
	if (memcmp(frame->magic, ANNIBALE_CAMERA_MAGIC, sizeof(frame->magic)) != 0 ||
	    frame->header_bytes != sizeof(*frame) || frame->width != profile->width || frame->height != profile->height ||
	    frame->stride != profile->stride || frame->format != 1 || frame->bayer != profile->bayer ||
	    frame->sensor != profile->sensor || frame->black > 1023 ||
	    frame->sequence != capture->sequence || frame->time != capture->time) {
		errno = EIO;
		return -1;
	}
	size_t expected = main_total;
	if ((frame->pd_flags & ANNIBALE_CAM_PD_VALID) != 0 && frame->pd_bytes > 0) {
		size_t avail = bytes - main_total;
		expected += frame->pd_bytes < avail ? frame->pd_bytes : avail;
	}
	if ((size_t)n != expected) {
		errno = EIO;
		return -1;
	}
	return n;
}
