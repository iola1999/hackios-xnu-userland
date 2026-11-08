/*
 * inject_cellular.inc — CoreTelephony / SystemStatus live status integration
 * reading from /dev/annibale-modem
 * and kern.annibale.{modem_state,cellular_bars,cellular_rat,cellular_operator,cellular_sim_state}.
 * Included by src/inject_app.c.
 */

#include <sys/sysctl.h>

struct annibale_modem_user_status {
	uint32_t magic;
	uint32_t state;
	uint32_t sim_state;
	uint32_t reg_state;
	uint32_t signal_bars;
	int32_t  rsrp_dbm;
	uint16_t mcc;
	uint16_t mnc;
	uint32_t ipv4_addr;
	uint32_t ipv4_gw;
	uint32_t ipv4_dns1;
	uint32_t ipv4_dns2;
	uint32_t mtu;
	uint32_t call_state;
	uint32_t sms_sent;
	uint32_t sms_received;
	char     rat[16];
	char     operator_name[32];
};

/* Live kernel cellular state cache (refreshed at most once every 2 seconds). */
static struct {
	time_t   last_poll;
	int      modem_state;
	int      bars;
	int      sim_state;
	int      rsrp_dbm;
	uint16_t mcc;
	uint16_t mnc;
	char     rat[16];
	char     operator_name[32];
} cell_cache = { .rsrp_dbm = -140, .rat = "None" };

static void
annibale_refresh_cellular_state(void)
{
	time_t now = time(NULL);
	size_t sz;
	int val = 0;

	if (now - cell_cache.last_poll < 2 && cell_cache.last_poll != 0) {
		return;
	}
	cell_cache.last_poll = now;

	sz = sizeof(val);
	if (sysctlbyname("kern.annibale.modem_state", &val, &sz, NULL, 0) == 0) {
		cell_cache.modem_state = val;
	}
	sz = sizeof(val);
	if (sysctlbyname("kern.annibale.cellular_bars", &val, &sz, NULL, 0) == 0) {
		cell_cache.bars = val < 0 ? 0 : (val > 4 ? 4 : val);
	}
	sz = sizeof(val);
	if (sysctlbyname("kern.annibale.cellular_sim_state", &val, &sz, NULL, 0) == 0) {
		cell_cache.sim_state = val;
	}
	sz = sizeof(val);
	if (sysctlbyname("kern.annibale.cellular_rsrp", &val, &sz, NULL, 0) == 0) {
		cell_cache.rsrp_dbm = val;
	}
	sz = sizeof(cell_cache.rat) - 1;
	if (sysctlbyname("kern.annibale.cellular_rat", cell_cache.rat, &sz, NULL, 0) == 0) {
		cell_cache.rat[sz] = '\0';
	}
	sz = sizeof(cell_cache.operator_name) - 1;
	if (sysctlbyname("kern.annibale.cellular_operator", cell_cache.operator_name, &sz, NULL, 0) == 0) {
		cell_cache.operator_name[sz] = '\0';
	}

	/* Also read binary snapshot from /dev/annibale-modem when MCC/MNC or full status is available. */
	int fd = open("/dev/annibale-modem", O_RDONLY | O_NONBLOCK);
	if (fd >= 0) {
		struct annibale_modem_user_status st;
		if (read(fd, &st, sizeof(st)) == (ssize_t)sizeof(st) && st.magic == 0x414e4e4du) {
			cell_cache.modem_state = (int)st.state;
			cell_cache.bars = (int)(st.signal_bars <= 4 ? st.signal_bars : 4);
			cell_cache.sim_state = (int)st.sim_state;
			cell_cache.rsrp_dbm = st.rsrp_dbm;
			cell_cache.mcc = st.mcc;
			cell_cache.mnc = st.mnc;
			if (st.rat[0] != '\0') {
				strlcpy(cell_cache.rat, st.rat, sizeof(cell_cache.rat));
			}
			if (st.operator_name[0] != '\0') {
				strlcpy(cell_cache.operator_name, st.operator_name, sizeof(cell_cache.operator_name));
			}
		}
		close(fd);
	}
}

/* CoreTelephony and SystemStatus read the modem status exported by the kernel. */
static IMP orig_ctcarrier_name;
static IMP orig_ctcarrier_mcc;
static IMP orig_ctcarrier_mnc;
static IMP orig_ctcarrier_voip;
static IMP orig_ctnetinfo_rat;
static IMP orig_ctnetinfo_svc_rat;
static IMP orig_st_signal_bars;
static IMP orig_st_service_state;

static id (*orig_container_url_for_group)(id, SEL, id) = NULL;

static id
nsstring_from_cstr(const char *s)
{
	Class strCls = objc_getClass("NSString");
	if (strCls == NULL || s == NULL) {
		return nil;
	}
	return ((id (*)(id, SEL, const char *))objc_msgSend)((id)strCls, sel_registerName("stringWithUTF8String:"), s);
}

static id
hooked_ctcarrier_name(id self, SEL cmd)
{
	annibale_refresh_cellular_state();
	if (cell_cache.operator_name[0] != '\0' && strcmp(cell_cache.operator_name, "None") != 0) {
		return nsstring_from_cstr(cell_cache.operator_name);
	}
	return orig_ctcarrier_name ? ((id (*)(id, SEL))orig_ctcarrier_name)(self, cmd) : nil;
}

static id
hooked_ctcarrier_mcc(id self, SEL cmd)
{
	annibale_refresh_cellular_state();
	if (cell_cache.mcc != 0) {
		char buf[16];
		snprintf(buf, sizeof(buf), "%03u", cell_cache.mcc);
		return nsstring_from_cstr(buf);
	}
	return orig_ctcarrier_mcc ? ((id (*)(id, SEL))orig_ctcarrier_mcc)(self, cmd) : nil;
}

static id
hooked_ctcarrier_mnc(id self, SEL cmd)
{
	annibale_refresh_cellular_state();
	if (cell_cache.mnc != 0) {
		char buf[16];
		snprintf(buf, sizeof(buf), "%02u", cell_cache.mnc);
		return nsstring_from_cstr(buf);
	}
	return orig_ctcarrier_mnc ? ((id (*)(id, SEL))orig_ctcarrier_mnc)(self, cmd) : nil;
}

static signed char
hooked_ctcarrier_voip(id self, SEL cmd)
{
	annibale_refresh_cellular_state();
	if (cell_cache.modem_state >= 4) {
		return 1;
	}
	return orig_ctcarrier_voip ? ((signed char (*)(id, SEL))orig_ctcarrier_voip)(self, cmd) : 1;
}

static const char *
ct_rat_constant_for_kernel_rat(const char *rat)
{
	if (rat == NULL || rat[0] == '\0' || strcmp(rat, "None") == 0) {
		return NULL;
	}
	if (strncmp(rat, "5G", 2) == 0 || strncmp(rat, "NR", 2) == 0) {
		return "CTRadioAccessTechnologyNR";
	}
	if (strcmp(rat, "3G") == 0 || strcmp(rat, "UMTS") == 0) {
		return "CTRadioAccessTechnologyWCDMA";
	}
	if (strcmp(rat, "GSM") == 0) {
		return "CTRadioAccessTechnologyGSM";
	}
	return "CTRadioAccessTechnologyLTE";
}

static id
hooked_ctnetinfo_rat(id self, SEL cmd)
{
	annibale_refresh_cellular_state();
	const char *ct_rat = ct_rat_constant_for_kernel_rat(cell_cache.rat);
	if (cell_cache.modem_state >= 4 && ct_rat != NULL) {
		return nsstring_from_cstr(ct_rat);
	}
	return orig_ctnetinfo_rat ? ((id (*)(id, SEL))orig_ctnetinfo_rat)(self, cmd) : nil;
}

static id
hooked_ctnetinfo_svc_rat(id self, SEL cmd)
{
	id orig = orig_ctnetinfo_svc_rat ? ((id (*)(id, SEL))orig_ctnetinfo_svc_rat)(self, cmd) : nil;
	if (orig != nil) {
		return orig;
	}
	annibale_refresh_cellular_state();
	const char *ct_rat = ct_rat_constant_for_kernel_rat(cell_cache.rat);
	if (cell_cache.modem_state >= 4 && ct_rat != NULL) {
		Class dictCls = objc_getClass("NSDictionary");
		id k = nsstring_from_cstr("0000000100000001");
		id v = nsstring_from_cstr(ct_rat);
		if (dictCls != NULL && k != nil && v != nil) {
			return ((id (*)(id, SEL, id, id))objc_msgSend)((id)dictCls,
			    sel_registerName("dictionaryWithObject:forKey:"), v, k);
		}
	}
	return nil;
}

static unsigned long
hooked_st_signal_bars(id self, SEL cmd)
{
	annibale_refresh_cellular_state();
	if (cell_cache.modem_state >= 4 || cell_cache.bars > 0) {
		return (unsigned long)cell_cache.bars;
	}
	return orig_st_signal_bars ? ((unsigned long (*)(id, SEL))orig_st_signal_bars)(self, cmd) : 0;
}

static unsigned long
hooked_st_service_state(id self, SEL cmd)
{
	annibale_refresh_cellular_state();
	if (cell_cache.modem_state >= 5) {
		return 2; /* STTelephonyServiceStateInService */
	}
	return orig_st_service_state ? ((unsigned long (*)(id, SEL))orig_st_service_state)(self, cmd) : 0;
}

static id
watched_container_url_for_group(id self, SEL cmd, id groupId)
{
	return orig_container_url_for_group ? orig_container_url_for_group(self, cmd, groupId) : nil;
}

static void
watch_cellular_classes(void)
{
	watch("CTCarrier", "carrierName", (IMP)hooked_ctcarrier_name, &orig_ctcarrier_name);
	watch("CTCarrier", "mobileCountryCode", (IMP)hooked_ctcarrier_mcc, &orig_ctcarrier_mcc);
	watch("CTCarrier", "mobileNetworkCode", (IMP)hooked_ctcarrier_mnc, &orig_ctcarrier_mnc);
	watch("CTCarrier", "allowsVOIP", (IMP)hooked_ctcarrier_voip, &orig_ctcarrier_voip);

	watch("CTTelephonyNetworkInfo", "currentRadioAccessTechnology", (IMP)hooked_ctnetinfo_rat, &orig_ctnetinfo_rat);
	watch("CTTelephonyNetworkInfo", "serviceCurrentRadioAccessTechnology", (IMP)hooked_ctnetinfo_svc_rat, &orig_ctnetinfo_svc_rat);

	watch("STTelephonyStatusDomainSIMInfo", "signalStrengthBars", (IMP)hooked_st_signal_bars, &orig_st_signal_bars);
	watch("STTelephonyStatusDomainSIMInfo", "serviceState", (IMP)hooked_st_service_state, &orig_st_service_state);
}

static void
app_dyld_image_added(const struct mach_header *mh, intptr_t vmaddr_slide)
{
	(void)mh;
	(void)vmaddr_slide;
	watch_cellular_classes();
	watch("NSFileManager", "containerURLForSecurityApplicationGroupIdentifier:", (IMP)watched_container_url_for_group, &orig_container_url_for_group);
	if (!app_vp9_hook_installed) {
		app_enable_webkit_vp9();
	}
}
