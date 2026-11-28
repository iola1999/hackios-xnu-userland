/*
 * Actuator virtual audio device properties and /dev/annibale-haptic I/O.
 *
 * AudioToolbox (_AudioToolboxServerInit / SetupVADRoutingObservation) looks up
 * "VirtualAudioDevice_Actuator", reads 'dacp' (CFArray of port AudioObjectIDs),
 * and checks 'ptyp' == 'pact' on the active port before starting Haptic 2.0.
 * VirtualAudio.plugin (Device_Actuator_Aspen.cpp) also inspects 'mute', 'vold',
 * and 'ssrc' on the actuator device, and CoreAudio HAL queries 'cust', 'clok',
 * 'cstb', 'siso', 'accs', 'exsm', 'drte', and 'auto'.
 */

enum {
	kAudioServerPlugInCustomPropertyDataTypeNone           = 0,
	kAudioServerPlugInCustomPropertyDataTypeCFString       = 'cfst',
	kAudioServerPlugInCustomPropertyDataTypeCFPropertyList = 'plst',
	kAudioServerPlugInCustomPropertyDataTypeUInt32         = 'ui32',

	kAudioObjectPropertyCustomPropertyInfoList             = 'cust',
	kAudioDevicePropertyClockAlgorithm                     = 'clok',
	kAudioDevicePropertyClockIsStable                      = 'cstb',
	kAudioDeviceClockAlgorithmRaw                          = 'raww',

	PROP_ACTUATOR_ACTIVE_PORTS                             = 'dacp',
	PROP_ACTUATOR_PORT_TYPE                                = 'ptyp',
	PORT_TYPE_ACTUATOR                                     = 'pact',

	PROP_DEVICE_SISO                                       = 'siso',
	PROP_DEVICE_ACCS                                       = 'accs',
	PROP_DEVICE_EXSM                                       = 'exsm',
	PROP_DEVICE_DRTE                                       = 'drte',
	PROP_DEVICE_AUTO                                       = 'auto',

	/* iOS CoreAudio HALS_System selectors (AudioToolbox 0x1bc4aeb38: 'duid' + AudioValueTranslation). */
	PROP_SYSTEM_TRANSLATE_UID_TO_DEVICE                    = 'duid',
	PROP_SYSTEM_DEVICES                                    = 'dev#',
};

static UInt32 actuator_muted = 0;
static Float32 actuator_volume_db = 0.0f;
static UInt32 actuator_source = PORT_TYPE_ACTUATOR;
static AudioObjectID actuator_hal_id = 0;
static CFArrayRef actuator_dacp_array = NULL;
static CFArrayRef actuator_empty_array = NULL;

typedef OSStatus (*actuator_hal_get_prop_fn)(AudioObjectID, const AudioObjectPropertyAddress *, UInt32, const void *,
    UInt32 *, void *);
static actuator_hal_get_prop_fn actuator_hal_get_prop_override = NULL;

static CFArrayRef
actuator_make_port_array(AudioObjectID port_id)
{
	SInt32 id_val = (SInt32)port_id;
	CFNumberRef num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &id_val);
	CFArrayRef arr = NULL;

	if (num != NULL) {
		const void *vals[1] = { num };
		arr = CFArrayCreate(kCFAllocatorDefault, vals, 1, &kCFTypeArrayCallBacks);
		CFRelease(num);
	}
	return arr;
}

static CFArrayRef
actuator_get_dacp_array(void)
{
	if (actuator_dacp_array == NULL) {
		actuator_dacp_array = actuator_make_port_array(actuator_hal_id != 0 ? actuator_hal_id : OBJECT_ACTUATOR);
	}
	return actuator_dacp_array;
}

static CFArrayRef
actuator_get_empty_array(void)
{
	if (actuator_empty_array == NULL) {
		actuator_empty_array = CFArrayCreate(kCFAllocatorDefault, NULL, 0, &kCFTypeArrayCallBacks);
	}
	return actuator_empty_array;
}

static AudioObjectID
actuator_lookup_via_devlist(actuator_hal_get_prop_fn get_prop, CFStringRef want_uid)
{
	AudioObjectPropertyAddress devs_addr = {
		PROP_SYSTEM_DEVICES, kAudioObjectPropertyScopeGlobal, 0
	};
	AudioObjectPropertyAddress uid_addr = {
		kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, 0
	};
	AudioObjectID devs[64];
	UInt32 devs_size = sizeof(devs);

	if (get_prop(1 /* kAudioObjectSystemObject */, &devs_addr, 0, NULL, &devs_size, devs) != 0) {
		return 0;
	}
	UInt32 count = devs_size / sizeof(AudioObjectID);
	for (UInt32 i = 0; i < count; i++) {
		CFStringRef dev_uid = NULL;
		UInt32 uid_size = sizeof(dev_uid);

		if (devs[i] == 0) {
			continue;
		}
		if (get_prop(devs[i], &uid_addr, 0, NULL, &uid_size, &dev_uid) == 0 && dev_uid != NULL) {
			Boolean match = CFEqual(dev_uid, want_uid);
			CFRelease(dev_uid);
			if (match) {
				return devs[i];
			}
		}
	}
	return 0;
}

static void *
actuator_hal_id_resolver_main(__unused void *arg)
{
	actuator_hal_get_prop_fn get_prop = actuator_hal_get_prop_override;
	AudioObjectPropertyAddress duid_addr = {
		PROP_SYSTEM_TRANSLATE_UID_TO_DEVICE, kAudioObjectPropertyScopeGlobal, 0
	};
	CFStringRef uid = CFSTR("VirtualAudioDevice_Actuator");
	useconds_t step_us = get_prop != NULL ? 1000 : 100000;
	bool err_logged = false;

	if (get_prop == NULL) {
		get_prop = (actuator_hal_get_prop_fn)dlsym(RTLD_DEFAULT, "AudioObjectGetPropertyData");
	}
	if (get_prop == NULL) {
		return NULL;
	}
	for (unsigned attempt = 0; attempt < 50; attempt++) {
		AudioObjectID resolved = 0;
		AudioValueTranslation trans = {
			.mInputData = &uid,
			.mInputDataSize = sizeof(uid),
			.mOutputData = &resolved,
			.mOutputDataSize = sizeof(resolved),
		};
		UInt32 data_size = sizeof(trans);
		OSStatus st = get_prop(1 /* kAudioObjectSystemObject */, &duid_addr, 0, NULL, &data_size, &trans);

		if (st != 0) {
			if (!err_logged) {
				err_logged = true;
				say("annibale: audio: actuator HAL 'duid' lookup failed: status=%d (0x%x), trying 'dev#'\n",
				    (int)st, (unsigned)st);
			}
			resolved = actuator_lookup_via_devlist(get_prop, uid);
		} else if (resolved == 0) {
			resolved = actuator_lookup_via_devlist(get_prop, uid);
		}

		if (resolved != 0) {
			CFArrayRef next = actuator_make_port_array(resolved);
			CFArrayRef old = NULL;
			AudioServerPlugInHostRef notify_host = NULL;

			if (next == NULL) {
				return NULL;
			}
			pthread_mutex_lock(&lock);
			actuator_hal_id = resolved;
			old = actuator_dacp_array;
			actuator_dacp_array = next;
			notify_host = host;
			pthread_mutex_unlock(&lock);
			if (old != NULL) {
				CFRelease(old);
			}
			say("annibale: audio: actuator HAL object ID resolved: %u\n", (unsigned)resolved);
			if (notify_host != NULL) {
				AudioObjectPropertyAddress dacp_changed = {
					PROP_ACTUATOR_ACTIVE_PORTS, kAudioObjectPropertyScopeGlobal, 0
				};
				notify_host->PropertiesChanged(notify_host, OBJECT_ACTUATOR, 1, &dacp_changed);
			}
			return NULL;
		}
		usleep(step_us);
	}
	say("annibale: audio: actuator HAL object ID not resolved after 50 attempts\n");
	return NULL;
}

static void
actuator_start_hal_id_resolver(void)
{
	pthread_t tid;

	pthread_mutex_lock(&lock);
	(void)actuator_get_dacp_array();
	(void)actuator_get_empty_array();
	pthread_mutex_unlock(&lock);
	if (pthread_create(&tid, NULL, actuator_hal_id_resolver_main, NULL) == 0) {
		pthread_detach(tid);
	}
}

static inline bool
actuator_output_scope(UInt32 scope)
{
	return scope == kAudioObjectPropertyScopeOutput || scope == kAudioObjectPropertyScopeGlobal;
}

static bool
actuator_device_property(const AudioObjectPropertyAddress *address, struct value *v)
{
	UInt32 selector = address->mSelector, scope = address->mScope;

	switch (selector) {
	case kAudioObjectPropertyCustomPropertyInfoList:
		if (scope == kAudioObjectPropertyScopeGlobal) {
			v->u.custom[0].mSelector = PROP_ACTUATOR_ACTIVE_PORTS;
			v->u.custom[0].mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFPropertyList;
			v->u.custom[0].mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
			v->u.custom[1].mSelector = PROP_ACTUATOR_PORT_TYPE;
			v->u.custom[1].mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeUInt32;
			v->u.custom[1].mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
			v->size = 2 * sizeof(AudioServerPlugInCustomPropertyInfo);
			return true;
		}
		return false;
	case PROP_ACTUATOR_ACTIVE_PORTS:
		if (actuator_output_scope(scope)) {
			CFArrayRef arr = actuator_get_dacp_array();
			if (arr != NULL) {
				v->u.cf = arr;
				v->cf = true;
				v->size = sizeof(CFArrayRef);
				return true;
			}
		}
		return false;
	case PROP_ACTUATOR_PORT_TYPE:
		if (actuator_output_scope(scope)) {
			v->u.words[0] = PORT_TYPE_ACTUATOR;
			v->size = sizeof(UInt32);
			return true;
		}
		return false;
	case kAudioDevicePropertyDataSource:
		v->u.words[0] = actuator_source;
		v->size = sizeof(UInt32);
		return true;
	case kAudioDevicePropertyDataSources:
		v->u.words[0] = PORT_TYPE_ACTUATOR;
		v->size = sizeof(UInt32);
		return true;
	case kAudioDevicePropertyMute:
		if (actuator_output_scope(scope)) {
			v->u.words[0] = actuator_muted;
			v->size = sizeof(UInt32);
			return true;
		}
		return false;
	case kAudioDevicePropertyVolumeDecibels:
		if (actuator_output_scope(scope)) {
			v->u.decimal = actuator_volume_db;
			v->size = sizeof(Float32);
			return true;
		}
		return false;
	case kAudioDevicePropertyClockAlgorithm:
		if (scope == kAudioObjectPropertyScopeGlobal) {
			v->u.words[0] = kAudioDeviceClockAlgorithmRaw;
			v->size = sizeof(UInt32);
			return true;
		}
		return false;
	case kAudioDevicePropertyClockIsStable:
		if (scope == kAudioObjectPropertyScopeGlobal) {
			v->u.words[0] = 1;
			v->size = sizeof(UInt32);
			return true;
		}
		return false;
	case PROP_DEVICE_SISO:
	case PROP_DEVICE_EXSM:
	case PROP_DEVICE_DRTE:
	case PROP_DEVICE_AUTO:
		if (scope == kAudioObjectPropertyScopeGlobal) {
			v->u.words[0] = 0;
			v->size = sizeof(UInt32);
			return true;
		}
		return false;
	case PROP_DEVICE_ACCS:
		if (scope == kAudioObjectPropertyScopeGlobal) {
			CFArrayRef arr = actuator_get_empty_array();
			if (arr != NULL) {
				v->u.cf = arr;
				v->cf = true;
				v->size = sizeof(CFArrayRef);
				return true;
			}
		}
		return false;
	default:
		return false;
	}
}

static bool
actuator_stream_property(const AudioObjectPropertyAddress *address, struct value *v)
{
	if (address->mSelector == kAudioObjectPropertyCustomPropertyInfoList &&
	    address->mScope == kAudioObjectPropertyScopeGlobal) {
		v->empty = true;
		return true;
	}
	if (address->mSelector == PROP_ACTUATOR_PORT_TYPE &&
	    address->mScope == kAudioObjectPropertyScopeGlobal) {
		v->u.words[0] = PORT_TYPE_ACTUATOR;
		v->size = sizeof(UInt32);
		return true;
	}
	return false;
}

static bool
actuator_is_property_settable(AudioObjectID object, const AudioObjectPropertyAddress *address, Boolean *settable)
{
	if (object != OBJECT_ACTUATOR) {
		return false;
	}
	if (address->mSelector == kAudioDevicePropertyDataSource ||
	    (actuator_output_scope(address->mScope) &&
	    (address->mSelector == kAudioDevicePropertyMute ||
	    address->mSelector == kAudioDevicePropertyVolumeDecibels))) {
		*settable = true;
		return true;
	}
	return false;
}

static OSStatus
actuator_set_property_data(AudioObjectID object, const AudioObjectPropertyAddress *address, UInt32 size, const void *data,
    bool *handled)
{
	*handled = false;
	if (object != OBJECT_ACTUATOR) {
		return 0;
	}
	if (address->mSelector == kAudioDevicePropertyDataSource) {
		*handled = true;
		if (size != sizeof(UInt32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		UInt32 src = *(const UInt32 *)data;
		if (src != PORT_TYPE_ACTUATOR) {
			return kAudioHardwareIllegalOperationError;
		}
		pthread_mutex_lock(&lock);
		actuator_source = src;
		pthread_mutex_unlock(&lock);
		return 0;
	}
	if (address->mSelector == kAudioDevicePropertyMute && actuator_output_scope(address->mScope)) {
		*handled = true;
		if (size != sizeof(UInt32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		pthread_mutex_lock(&lock);
		actuator_muted = *(const UInt32 *)data != 0 ? 1u : 0u;
		pthread_mutex_unlock(&lock);
		return 0;
	}
	if (address->mSelector == kAudioDevicePropertyVolumeDecibels && actuator_output_scope(address->mScope)) {
		*handled = true;
		if (size != sizeof(Float32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		pthread_mutex_lock(&lock);
		actuator_volume_db = *(const Float32 *)data;
		pthread_mutex_unlock(&lock);
		return 0;
	}
	return 0;
}

/*
 * What iOS plays on the actuator -- the haptics: the system's feedbacks and the ringer's vibration,
 * CoreHaptics' patterns, UIKit's feedbacks, as waveforms of one channel -- goes to the kernel's motor
 * driver (/dev/annibale-haptic, bsd/dev/annibale/annibale_haptic.c, haptic=5), which plays Android's own
 * waveforms on the phone's motor by it. (iOS plays on this device at all only because the device
 * tree says the phone's haptics are "closed-loop": see mkidentity.py.) Said the first times, with
 * how loud.
 */
static int haptic = -1;          /* the motor driver; -2: not there (the kernel runs without haptic=5) */
static unsigned haptic_said;

static void
actuator_write(const int16_t *samples, UInt32 frames)
{
	int peak = 0;

	for (UInt32 i = 0; i < frames; i++) {
		int v = samples[i] < 0 ? -samples[i] : samples[i];

		peak = v > peak ? v : peak;
	}
	if (haptic == -1) {
		int fd = open("/dev/annibale-haptic", O_WRONLY | O_NONBLOCK);
		haptic = fd >= 0 ? fd : (errno == ENOENT ? -2 : -1);
	}
	if (haptic >= 0) {
		(void)write(haptic, samples, (size_t)frames * 2);
	}
	if (peak > 0 && haptic_said < 12) {
		haptic_said++;
		say("annibale: audio: the actuator plays: %u frames, peak %d of 32768%s\n", (unsigned)frames, peak,
		    haptic >= 0 ? "" : " (no /dev/annibale-haptic: not felt)");
	}
}
