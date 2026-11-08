/*
 * torch_test: the flashlight the way a program (and Control Center) works it: AVFlashlight, which
 * asks mediaserverd's FigFlashlight, which sets the back camera's torch level (on this machine:
 * libannibale-media.dylib in mediaserverd, the kernel's /dev/annibale-torch, torch=3).
 *
 * usage: torch_test STEP...
 *   wait SECONDS
 *   level L          the flashlight at L (0 to 1; Control Center's steps are fractions of 1)
 *   off              the flashlight off (level 0, then AVFlashlight's turnPowerOff)
 *   state            what the kernel says is lit now (thousandths of Android's current)
 *
 * Says first whether iOS believes there is a flashlight (AVFlashlight's hasFlashlight: MobileGestalt's
 * camera-flash) and whether it is available.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static id
send(id object, const char *selector)
{
	return object ? ((id (*)(id, SEL))objc_msgSend)(object, sel_registerName(selector)) : NULL;
}

static BOOL
yes(id object, const char *selector)
{
	return object ? ((BOOL (*)(id, SEL))objc_msgSend)(object, sel_registerName(selector)) : NO;
}

static const char *
text(id object)
{
	id description = send(object, "description");
	const char *utf8 = description ? ((const char *(*)(id, SEL))objc_msgSend)(description, sel_registerName("UTF8String")) : NULL;

	return utf8 ? utf8 : "(none)";
}

static void
kernel_state(void)
{
	int device = open("/dev/annibale-torch", O_RDONLY);
	uint32_t lit = 0;

	if (device < 0 || read(device, &lit, sizeof(lit)) != (ssize_t)sizeof(lit)) {
		printf("torch_test: the kernel has no flashlight (/dev/annibale-torch)\n");
	} else {
		printf("torch_test: the kernel says: %s%u of 1000\n", lit ? "LIT at " : "dark, ", lit);
	}
	if (device >= 0) {
		close(device);
	}
}

static int
kernel_write_str(const char *arg)
{
	int device = open("/dev/annibale-torch", O_WRONLY);
	ssize_t n;

	if (device < 0) {
		printf("torch_test: cannot open /dev/annibale-torch for write\n");
		return 1;
	}
	n = write(device, arg, strlen(arg));
	close(device);
	if (n < 0) {
		printf("torch_test: write /dev/annibale-torch \"%s\" failed\n", arg);
		return 1;
	}
	usleep(150000);
	printf("torch_test: wrote \"%s\" to /dev/annibale-torch; ", arg);
	kernel_state();
	return 0;
}

int
main(int argc, char **argv)
{
	Class flashlight_class = objc_getClass("AVFlashlight");
	id flashlight = NULL;
	BOOL has_av = NO;
	int failed = 0;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (flashlight_class != NULL) {
		has_av = yes((id)flashlight_class, "hasFlashlight");
		printf("torch_test: AVFlashlight says hasFlashlight %s\n", has_av ? "YES" : "NO");
		if (has_av) {
			flashlight = send(send((id)flashlight_class, "alloc"), "init");
			printf("torch_test: an AVFlashlight %s; available %s, overheated %s, level %.2f\n",
			    flashlight ? "made" : "NOT MADE",
			    yes(flashlight, "isAvailable") ? "YES" : "NO",
			    yes(flashlight, "isOverheated") ? "YES" : "NO",
			    flashlight ? ((float (*)(id, SEL))objc_msgSend)(flashlight, sel_registerName("flashlightLevel")) : -1.0f);
		}
	} else {
		printf("torch_test: no AVFlashlight class in process\n");
	}
	kernel_state();

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "wait") == 0 && i + 1 < argc) {
			usleep((useconds_t)(atof(argv[++i]) * 1e6));
		} else if (strcmp(argv[i], "direct") == 0 && i + 1 < argc) {
			failed |= kernel_write_str(argv[++i]);
		} else if (strcmp(argv[i], "on") == 0) {
			failed |= kernel_write_str("on");
		} else if (strcmp(argv[i], "level") == 0 && i + 1 < argc) {
			const char *arg = argv[++i];
			float level = (float)atof(arg);

			if (flashlight != NULL) {
				id error = NULL;
				BOOL done = ((BOOL (*)(id, SEL, float, id *))objc_msgSend)(flashlight,
				    sel_registerName("setFlashlightLevel:withError:"), level, &error);

				printf("torch_test: level %.2f: %s%s; available %s, level now %.2f\n", level,
				    done ? "done" : "FAILED: ", done ? "" : text(error),
				    yes(flashlight, "isAvailable") ? "YES" : "NO",
				    ((float (*)(id, SEL))objc_msgSend)(flashlight, sel_registerName("flashlightLevel")));
				if (!done) {
					failed |= kernel_write_str(arg);
				}
			} else {
				failed |= kernel_write_str(arg);
			}
		} else if (strcmp(argv[i], "off") == 0) {
			if (flashlight != NULL) {
				id error = NULL;

				((BOOL (*)(id, SEL, float, id *))objc_msgSend)(flashlight,
				    sel_registerName("setFlashlightLevel:withError:"), 0.0f, &error);
				send(flashlight, "turnPowerOff");
				printf("torch_test: off; level now %.2f\n", ((float (*)(id, SEL))objc_msgSend)(flashlight,
				    sel_registerName("flashlightLevel")));
			}
			failed |= kernel_write_str("off");
		} else if (strcmp(argv[i], "state") == 0) {
			kernel_state();
		} else {
			printf("torch_test: what is \"%s\"?\n", argv[i]);
			return 2;
		}
	}
	return failed;
}
