/*
 * iOS 26 (V57 / iPhone18,3) FigCaptureStream format descriptors and SupportedOutputs builders
 * (included by camera_device.c).
 *
 * FigCaptureSourceBackingsProvider resolves every MultiStream FigCaptureStreamFormats entry via
 * csu_resolveFormatIndexForNamedStreamFormat (matching SensorWidth/Height, VideoIsBinned,
 * SensorHDRMode, FrontEndScalerOutputWidth/Height presence and dimensions, VideoMaxFrameRate,
 * SIFREnabled/SIFRBinningFactor, QuadraBinningFactor, and OptimizedForMidFrameSynchronization)
 * and checks SupportedOutputs[*]["SupportedPixelFormats"] against every AVCaptureDeviceFormats
 * and Variants PixelFormatType ('420v', '420f', 'x420', 'x422').
 */

#define PF_420F         0x34323066      /* '420f' */
#define PF_420V         0x34323076      /* '420v' */
#define PF_X420         0x78343230      /* 'x420' */
#define PF_X422         0x78343232      /* 'x422' */
#define PF_XF20         0x78663230      /* 'xf20' */
#define PF_BGRA         0x42475241      /* 'BGRA' */
#define PF_L008         0x4c303038      /* 'L008' */

struct ios26_stream_format_spec {
	uint16_t        sensor_w, sensor_h;
	uint8_t         binned, hdr, fes;
	uint16_t        fes_w, fes_h;
	uint16_t        max_fps;
	uint8_t         sifr, sifr_bin, quadra, midframe;
};

/* V57/AVCaptureSession.plist AVCaptureDevices[0] ("Back Camera", PortTypeBack) */
static const struct ios26_stream_format_spec v57_back_specs[] = {
	{ 2112, 1192, 1, 0, 1, 1536,  864, 240, 0, 0, 0, 0 },
	{ 4224, 3024, 0, 0, 1, 1440, 1080,  60, 1, 2, 0, 0 },
	{ 4224, 3024, 0, 0, 0,    0,    0,  60, 1, 1, 0, 0 },
	{ 4224, 3024, 0, 0, 1, 1920, 1440,  60, 1, 1, 0, 0 },
	{ 4224, 3024, 0, 0, 1, 2464, 1764,  60, 1, 2, 0, 0 },
	{ 4224, 2640, 0, 0, 0,    0,    0,  30, 1, 2, 0, 0 },
	{ 4224, 2640, 0, 0, 1, 2464, 1540,  60, 1, 2, 0, 0 },
	{ 4224, 3024, 0, 0, 0,    0,    0,  30, 1, 2, 0, 0 },
	{ 2112, 1512, 1, 0, 0,    0,    0,   0, 0, 0, 0, 0 },
	{ 4224, 2640, 0, 0, 0,    0,    0,  60, 1, 2, 0, 0 },
	{ 4224, 3024, 0, 0, 0,    0,    0,  60, 1, 2, 0, 0 },
	{ 2112, 1512, 1, 0, 1, 1440, 1080,   0, 0, 0, 0, 0 },
	{ 4224, 3024, 0, 0, 1, 2464, 1764,  30, 1, 2, 0, 0 },
	{ 4224, 3024, 0, 0, 1, 2464, 1764,  30, 1, 1, 0, 0 },
	{ 4224, 2376, 0, 0, 1, 2304, 1296, 120, 0, 0, 0, 0 },
	{ 2112, 1192, 1, 0, 0,    0,    0, 240, 0, 0, 0, 0 },
};

/*
 * V57/FrontUltraWideCamera-DeviceInfoBase.plist + FrontVirtualCamera-DeviceInfoBase.plist
 * (PortTypeFrontSuperWide, shared by Front Ultra Wide Camera and Front Virtual Camera).
 */
static const struct ios26_stream_format_spec v57_front_specs[] = {
	{ 2448, 2448, 1, 0, 1, 1664, 1664,  30, 1, 2, 0, 0 },
	{ 2448, 2448, 1, 0, 1, 2448, 2448,  30, 1, 4, 0, 0 },
	{ 2448, 2448, 1, 0, 0,    0,    0,  30, 1, 2, 0, 0 },
	{ 4896, 4896, 0, 0, 1, 2496, 2496,  60, 0, 0, 0, 0 },
	{ 2448, 1392, 1, 0, 0,    0,    0, 120, 0, 0, 0, 0 },
	{ 4896, 4896, 0, 0, 1, 4608, 4608,  30, 1, 2, 0, 0 },
	{ 3072, 4896, 0, 0, 1, 1560, 2496,  60, 0, 0, 0, 0 },
	{ 4896, 4896, 0, 0, 1, 2496, 2496,  30, 1, 2, 0, 0 },
	{ 1392, 2448, 1, 0, 0,    0,    0, 120, 0, 0, 0, 0 },
	{ 4896, 4896, 0, 0, 1, 4608, 4608,  60, 0, 0, 0, 0 },
	{ 4896, 4896, 0, 0, 1, 1224, 1224,  60, 0, 0, 0, 0 },
	{ 3072, 4896, 0, 0, 1, 2880, 4608,  60, 0, 0, 0, 0 },
	{ 2448, 2448, 1, 0, 1, 2448, 2448,  30, 1, 2, 0, 0 },
	{ 4896, 4896, 0, 0, 1, 4608, 4608,   0, 1, 1, 0, 0 },
	{ 3072, 4896, 0, 0, 1, 1320, 2112,  60, 0, 0, 0, 0 },
	{ 4896, 4896, 0, 0, 1, 4032, 4032,   0, 1, 1, 0, 0 },
	{ 3680, 4896, 0, 0, 1,  852, 1136,  60, 0, 0, 0, 0 },
	{ 4896, 4896, 0, 0, 1, 4032, 4032,  30, 1, 2, 0, 0 },
	{ 1840, 2448, 1, 0, 1, 1080, 1440,  60, 1, 2, 0, 0 },
	{ 3072, 4896, 0, 0, 1, 2520, 4032,  60, 0, 0, 0, 0 },
	{ 1840, 2448, 1, 0, 0,    0,    0,  60, 1, 2, 0, 0 },
	{ 4896, 4896, 0, 0, 1, 2112, 2112,  30, 1, 2, 0, 0 },
	{ 4896, 4896, 0, 0, 1, 2496, 2496,   0, 1, 1, 0, 0 },
	{ 4896, 4896, 0, 0, 1, 2112, 2112,   0, 1, 1, 0, 0 },
	{ 1392, 2448, 1, 0, 1,  864, 1536, 120, 0, 0, 0, 0 },
	{ 1392, 2448, 1, 0, 1,  864, 1536,  60, 1, 2, 0, 0 },
	{ 1392, 2448, 1, 0, 0,    0,    0,  60, 1, 2, 0, 0 },
};

static CFDictionaryRef
output_info(int width, int height, double max_fps)
{
	CFMutableDictionaryRef o = (CFMutableDictionaryRef)new_dict();
	CFNumberRef pf[6] = {
		number_i(PF_420F), number_i(PF_420V),
		number_i(PF_X420), number_i(PF_X422),
		number_i(PF_XF20), number_i(PF_BGRA)
	};
	CFArrayRef pfs = CFArrayCreate(NULL, (const void **)pf, 6, &kCFTypeArrayCallBacks);

	for (int i = 0; i < 6; i++) {
		CFRelease(pf[i]);
	}
	put(o, "SupportedPixelFormats", pfs);
	put(o, "NativeWidth", number_i(width));
	put(o, "NativeHeight", number_i(height));
	put(o, "NativeWidthPostGeometricDistortionCorrection", number_i(width));
	put(o, "NativeHeightPostGeometricDistortionCorrection", number_i(height));
	put(o, "MaxDownscaleFactor", number_f(32));
	put(o, "MaxScaleFactor", number_f(1));
	put(o, "MinScaleFactor", number_f(1.0 / 32));
	put(o, "OutputWidthAlignmentRequirement", number_i(16));
	put(o, "MaxFrameRate", number_f(max_fps));
	put(o, "SashimiRawPixelFormat", number_i(PF_BGRA));
	put(o, "SushiRawPixelFormat", number_i(PF_BGRA));
	put(o, "FocusPixelDataSupported", CFRetain(kCFBooleanFalse));
	put(o, "GeometricDistortionCorrectionSupported", CFRetain(kCFBooleanTrue));
	return o;
}

static void
populate_outputs(CFMutableDictionaryRef f, uint32_t width, uint32_t height, double fps)
{
	CFMutableDictionaryRef outputs = (CFMutableDictionaryRef)new_dict();

	put(outputs, "PrimaryScaler", output_info((int)width, (int)height, fps));
	put(outputs, "SecondaryScaler", output_info((int)width, (int)height, fps));
	put(outputs, "StillImage", output_info((int)width, (int)height, fps));
	put(outputs, "StillImageRaw", output_info((int)width, (int)height, fps));
	put(f, "SupportedOutputs", outputs);
}

static CFDictionaryRef
native_format(const struct annibale_camera_profile *profile, uint32_t width, uint32_t height, double field_of_view)
{
	CFMutableDictionaryRef f = (CFMutableDictionaryRef)new_dict();
	CMVideoFormatDescriptionRef fd = NULL;
	double fps = profile->fps_milli / 1000.0;

	CMVideoFormatDescriptionCreate(NULL, PF_420F, (int32_t)width, (int32_t)height, NULL, &fd);
	if (fd != NULL) {
		put(f, "FormatDescription", fd);
	}
	put(f, "VideoMinFrameRate", number_f(fps));
	put(f, "VideoMaxFrameRate", number_f(fps));
	put(f, "VideoRawBitDepth", number_i(10));
	put(f, "VideoMaxWidth", number_i(width));
	put(f, "VideoMaxHeight", number_i(height));
	put(f, "VideoFieldOfView", number_f(field_of_view));
	put(f, "Experimental", CFRetain(kCFBooleanFalse));
	put(f, "VideoIsBinned", CFRetain(kCFBooleanFalse));
	put(f, "SensorHDRMode", number_i(0));
	put(f, "SIFREnabled", CFRetain(kCFBooleanFalse));
	put(f, "QuadraBinningFactor", number_i(0));
	put(f, "OptimizedForMidFrameSynchronization", CFRetain(kCFBooleanFalse));
	put(f, "SensorWidth", number_i(profile->width));
	put(f, "SensorHeight", number_i(profile->height));
	put(f, "SashimiRawPixelFormat", number_i(PF_BGRA));
	put(f, "SushiRawPixelFormat", number_i(PF_BGRA));
	put(f, "FocusPixelDataSupported", CFRetain(kCFBooleanFalse));
	put(f, "SensorCropWidth", number_i(profile->width));
	put(f, "SensorCropHeight", number_i(profile->height));
	put(f, "PreGDCSensorCropWidth", number_i(profile->width));
	put(f, "PreGDCSensorCropHeight", number_i(profile->height));
	put(f, "PostGDCCropWidth", number_i(width));
	put(f, "PostGDCCropHeight", number_i(height));
	put(f, "GeometricDistortionCorrectedNativeWidth", number_i(width));
	put(f, "GeometricDistortionCorrectedNativeHeight", number_i(height));
	put(f, "FrontEndScalerOutputWidth", number_i(width));
	put(f, "FrontEndScalerOutputHeight", number_i(height));
	populate_outputs(f, width, height, fps);
	return f;
}

static CFDictionaryRef
v57_stream_format(const struct ios26_stream_format_spec *sp, double field_of_view)
{
	CFMutableDictionaryRef f = (CFMutableDictionaryRef)new_dict();
	CMVideoFormatDescriptionRef fd = NULL;
	uint32_t out_w = sp->fes ? sp->fes_w : sp->sensor_w;
	uint32_t out_h = sp->fes ? sp->fes_h : sp->sensor_h;
	double max_fps = sp->max_fps > 0 ? (double)sp->max_fps : 60.0;

	CMVideoFormatDescriptionCreate(NULL, PF_420F, (int32_t)out_w, (int32_t)out_h, NULL, &fd);
	if (fd != NULL) {
		put(f, "FormatDescription", fd);
	}
	put(f, "VideoMinFrameRate", number_f(1.0));
	put(f, "VideoMaxFrameRate", number_f(max_fps));
	put(f, "VideoRawBitDepth", number_i(10));
	put(f, "VideoMaxWidth", number_i(out_w));
	put(f, "VideoMaxHeight", number_i(out_h));
	put(f, "VideoFieldOfView", number_f(field_of_view));
	put(f, "Experimental", CFRetain(kCFBooleanFalse));
	put(f, "SensorWidth", number_i(sp->sensor_w));
	put(f, "SensorHeight", number_i(sp->sensor_h));
	put(f, "VideoIsBinned", CFRetain(sp->binned ? kCFBooleanTrue : kCFBooleanFalse));
	put(f, "SensorHDRMode", number_i(sp->hdr ? 1 : 0));
	if (sp->fes) {
		put(f, "FrontEndScalerOutputWidth", number_i(sp->fes_w));
		put(f, "FrontEndScalerOutputHeight", number_i(sp->fes_h));
	}
	put(f, "QuadraBinningFactor", number_i(sp->quadra));
	put(f, "SIFREnabled", CFRetain(sp->sifr ? kCFBooleanTrue : kCFBooleanFalse));
	if (sp->sifr) {
		put(f, "SIFRBinningFactor", number_i(sp->sifr_bin));
	}
	put(f, "OptimizedForMidFrameSynchronization", CFRetain(sp->midframe ? kCFBooleanTrue : kCFBooleanFalse));
	put(f, "SashimiRawPixelFormat", number_i(PF_BGRA));
	put(f, "SushiRawPixelFormat", number_i(PF_BGRA));
	put(f, "FocusPixelDataSupported", CFRetain(kCFBooleanFalse));
	put(f, "SensorCropWidth", number_i(sp->sensor_w));
	put(f, "SensorCropHeight", number_i(sp->sensor_h));
	put(f, "PreGDCSensorCropWidth", number_i(sp->sensor_w));
	put(f, "PreGDCSensorCropHeight", number_i(sp->sensor_h));
	put(f, "PostGDCCropWidth", number_i(out_w));
	put(f, "PostGDCCropHeight", number_i(out_h));
	put(f, "GeometricDistortionCorrectedNativeWidth", number_i(out_w));
	put(f, "GeometricDistortionCorrectedNativeHeight", number_i(out_h));
	populate_outputs(f, out_w, out_h, max_fps);
	return f;
}

static CFArrayRef
build_stream_formats(const struct annibale_camera_profile *profile, double field_of_view)
{
	CFDictionaryRef list[40];
	CFIndex nf = 0;

	if (profile != NULL) {
		uint32_t sizes[3][2] = { { profile->width, profile->height }, { 1920, 1080 }, { 1280, 720 } };
		const struct ios26_stream_format_spec *specs = NULL;
		size_t nspecs = 0;

		for (int i = 0; i < 3; i++) {
			list[nf++] = native_format(profile, sizes[i][0], sizes[i][1], field_of_view);
		}
		if (profile->sensor == 1) {
			specs = v57_back_specs;
			nspecs = sizeof(v57_back_specs) / sizeof(v57_back_specs[0]);
		} else if (profile->sensor == 2) {
			specs = v57_front_specs;
			nspecs = sizeof(v57_front_specs) / sizeof(v57_front_specs[0]);
		}
		for (size_t i = 0; i < nspecs; i++) {
			list[nf++] = v57_stream_format(&specs[i], field_of_view);
		}
	}
	CFArrayRef arr = CFArrayCreate(NULL, (const void **)list, nf, &kCFTypeArrayCallBacks);
	for (CFIndex i = 0; i < nf; i++) {
		CFRelease(list[i]);
	}
	return arr;
}
