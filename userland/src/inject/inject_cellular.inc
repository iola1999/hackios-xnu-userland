/*
 * inject_cellular.inc — Network reachability, pdp_ip0 cellular data path, and
 * CoreTelephony / SystemStatus live status integration reading from /dev/redwood-modem
 * and kern.redwood.{modem_state,cellular_bars,cellular_rat,cellular_operator,cellular_sim_state}.
 * Included by src/inject_app.c.
 */

#include <sys/sysctl.h>

struct redwood_modem_user_status {
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
redwood_refresh_cellular_state(void)
{
	time_t now = time(NULL);
	size_t sz;
	int val = 0;

	if (now - cell_cache.last_poll < 2 && cell_cache.last_poll != 0) {
		return;
	}
	cell_cache.last_poll = now;

	sz = sizeof(val);
	if (sysctlbyname("kern.redwood.modem_state", &val, &sz, NULL, 0) == 0) {
		cell_cache.modem_state = val;
	}
	sz = sizeof(val);
	if (sysctlbyname("kern.redwood.cellular_bars", &val, &sz, NULL, 0) == 0) {
		cell_cache.bars = val < 0 ? 0 : (val > 4 ? 4 : val);
	}
	sz = sizeof(val);
	if (sysctlbyname("kern.redwood.cellular_sim_state", &val, &sz, NULL, 0) == 0) {
		cell_cache.sim_state = val;
	}
	sz = sizeof(val);
	if (sysctlbyname("kern.redwood.cellular_rsrp", &val, &sz, NULL, 0) == 0) {
		cell_cache.rsrp_dbm = val;
	}
	sz = sizeof(cell_cache.rat) - 1;
	if (sysctlbyname("kern.redwood.cellular_rat", cell_cache.rat, &sz, NULL, 0) == 0) {
		cell_cache.rat[sz] = '\0';
	}
	sz = sizeof(cell_cache.operator_name) - 1;
	if (sysctlbyname("kern.redwood.cellular_operator", cell_cache.operator_name, &sz, NULL, 0) == 0) {
		cell_cache.operator_name[sz] = '\0';
	}

	/* Also read binary snapshot from /dev/redwood-modem when MCC/MNC or full status is available. */
	int fd = open("/dev/redwood-modem", O_RDONLY | O_NONBLOCK);
	if (fd >= 0) {
		struct redwood_modem_user_status st;
		if (read(fd, &st, sizeof(st)) == (ssize_t)sizeof(st) && st.magic == 0x5244574du) {
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

static bool
en0_has_ipv4(void)
{
	static time_t last_check = 0;
	static bool cached = false;
	time_t now = time(NULL);

	if (now - last_check < 2 && last_check != 0) {
		return cached;
	}
	last_check = now;
	struct ifaddrs *list = NULL;
	if (getifaddrs(&list) == 0) {
		bool ok = false;
		for (struct ifaddrs *ifa = list; ifa != NULL; ifa = ifa->ifa_next) {
			if (ifa->ifa_name != NULL && ifa->ifa_addr != NULL &&
			    (ifa->ifa_flags & (IFF_UP | IFF_RUNNING)) &&
			    !(ifa->ifa_flags & IFF_LOOPBACK)) {
				if (ifa->ifa_addr->sa_family == AF_INET || ifa->ifa_addr->sa_family == AF_INET6) {
					ok = true;
					break;
				}
			}
		}
		freeifaddrs(list);
		cached = ok;
		return ok;
	}
	return cached;
}

static bool
pdp_ip0_active(void)
{
	redwood_refresh_cellular_state();
	return cell_cache.modem_state >= 6;
}

/* ObjC hooks for Network.framework, GeoServices, CoreTelephony, SystemStatus, and NSFileManager */
static IMP orig_geo_is_net_reachable;
static IMP orig_geo_is_wifi_reachable;
static IMP orig_geo_is_cell;
static IMP orig_geo_is_constrained;
static IMP orig_geo_is_cell_constrained;
static IMP orig_geo_net_reachable;
static IMP orig_geo_wifi_reachable;

static IMP orig_nwpath_status;
static IMP orig_nwpath_is_satisfied;
static IMP orig_nwpath_is_expensive;
static IMP orig_nwpath_is_constrained;
static IMP orig_nwpath_has_ipv4;
static IMP orig_nwpath_has_dns;
static IMP orig_nwpath_uses_interface;

static IMP orig_evaluator_is_satisfied;
static IMP orig_evaluator_status;

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
	redwood_refresh_cellular_state();
	if (cell_cache.operator_name[0] != '\0' && strcmp(cell_cache.operator_name, "None") != 0) {
		return nsstring_from_cstr(cell_cache.operator_name);
	}
	return orig_ctcarrier_name ? ((id (*)(id, SEL))orig_ctcarrier_name)(self, cmd) : nil;
}

static id
hooked_ctcarrier_mcc(id self, SEL cmd)
{
	redwood_refresh_cellular_state();
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
	redwood_refresh_cellular_state();
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
	redwood_refresh_cellular_state();
	if (cell_cache.modem_state >= 4 || en0_has_ipv4()) {
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
	redwood_refresh_cellular_state();
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
	redwood_refresh_cellular_state();
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
	redwood_refresh_cellular_state();
	if (cell_cache.modem_state >= 4 || cell_cache.bars > 0) {
		return (unsigned long)cell_cache.bars;
	}
	return orig_st_signal_bars ? ((unsigned long (*)(id, SEL))orig_st_signal_bars)(self, cmd) : 0;
}

static unsigned long
hooked_st_service_state(id self, SEL cmd)
{
	redwood_refresh_cellular_state();
	if (cell_cache.modem_state >= 5) {
		return 2; /* STTelephonyServiceStateInService */
	}
	return orig_st_service_state ? ((unsigned long (*)(id, SEL))orig_st_service_state)(self, cmd) : 0;
}

static signed char
hooked_geo_is_net_reachable(id self, SEL cmd)
{
	if (en0_has_ipv4()) {
		return 1;
	}
	return orig_geo_is_net_reachable ? ((signed char (*)(id, SEL))orig_geo_is_net_reachable)(self, cmd) : 1;
}

static signed char
hooked_geo_is_wifi_reachable(id self, SEL cmd)
{
	if (en0_has_ipv4()) {
		return 1;
	}
	return orig_geo_is_wifi_reachable ? ((signed char (*)(id, SEL))orig_geo_is_wifi_reachable)(self, cmd) : 1;
}

static signed char
hooked_geo_is_cell(id self, SEL cmd)
{
	(void)self; (void)cmd;
	return pdp_ip0_active() ? 1 : 0;
}

static signed char
hooked_geo_is_constrained(id self, SEL cmd)
{
	(void)self; (void)cmd;
	return 0;
}

static int
hooked_nwpath_status(id self, SEL cmd)
{
	int s = orig_nwpath_status ? ((int (*)(id, SEL))orig_nwpath_status)(self, cmd) : 1;
	if (s != 1 && en0_has_ipv4()) {
		return 1; /* nw_path_status_satisfied */
	}
	return s;
}

static signed char
hooked_nwpath_is_satisfied(id self, SEL cmd)
{
	if (en0_has_ipv4()) {
		return 1;
	}
	return orig_nwpath_is_satisfied ? ((signed char (*)(id, SEL))orig_nwpath_is_satisfied)(self, cmd) : 1;
}

static signed char
hooked_nwpath_is_expensive(id self, SEL cmd)
{
	if (en0_has_ipv4()) {
		return 0;
	}
	return orig_nwpath_is_expensive ? ((signed char (*)(id, SEL))orig_nwpath_is_expensive)(self, cmd) : 0;
}

static signed char
hooked_nwpath_is_constrained(id self, SEL cmd)
{
	if (en0_has_ipv4()) {
		return 0;
	}
	return orig_nwpath_is_constrained ? ((signed char (*)(id, SEL))orig_nwpath_is_constrained)(self, cmd) : 0;
}

static signed char
hooked_nwpath_has_ipv4(id self, SEL cmd)
{
	if (en0_has_ipv4()) {
		return 1;
	}
	return orig_nwpath_has_ipv4 ? ((signed char (*)(id, SEL))orig_nwpath_has_ipv4)(self, cmd) : 1;
}

static signed char
hooked_nwpath_has_dns(id self, SEL cmd)
{
	if (en0_has_ipv4()) {
		return 1;
	}
	return orig_nwpath_has_dns ? ((signed char (*)(id, SEL))orig_nwpath_has_dns)(self, cmd) : 1;
}

static signed char
hooked_nwpath_uses_interface(id self, SEL cmd, int type)
{
	if (type == 1 /* wifi */ && en0_has_ipv4()) {
		return 1;
	}
	if (type == 2 /* cellular */ && pdp_ip0_active()) {
		return 1;
	}
	return orig_nwpath_uses_interface ? ((signed char (*)(id, SEL, int))orig_nwpath_uses_interface)(self, cmd, type) : 0;
}

static signed char
hooked_evaluator_is_satisfied(id self, SEL cmd)
{
	if (en0_has_ipv4()) {
		return 1;
	}
	return orig_evaluator_is_satisfied ? ((signed char (*)(id, SEL))orig_evaluator_is_satisfied)(self, cmd) : 1;
}

static int
hooked_evaluator_status(id self, SEL cmd)
{
	int s = orig_evaluator_status ? ((int (*)(id, SEL))orig_evaluator_status)(self, cmd) : 1;
	if (s != 1 && en0_has_ipv4()) {
		return 1;
	}
	return s;
}

static id
watched_container_url_for_group(id self, SEL cmd, id groupId)
{
	id url = orig_container_url_for_group ? orig_container_url_for_group(self, cmd, groupId) : nil;
	if (url != nil) {
		return url;
	}
	if (groupId != nil) {
		const char *group_cstr = ((const char * (*)(id, SEL))objc_msgSend)(groupId, sel_registerName("UTF8String"));
		if (group_cstr != NULL && group_cstr[0] != '\0') {
			char path[512];
			snprintf(path, sizeof(path), "/private/var/mobile/Containers/Shared/AppGroup/%s", group_cstr);
			(void)mkdir("/private/var/mobile/Containers/Shared", 0755);
			(void)mkdir("/private/var/mobile/Containers/Shared/AppGroup", 0755);
			(void)mkdir(path, 0755);
			char sub[540];
			snprintf(sub, sizeof(sub), "%s/Library", path); (void)mkdir(sub, 0755);
			snprintf(sub, sizeof(sub), "%s/Library/Preferences", path); (void)mkdir(sub, 0755);
			snprintf(sub, sizeof(sub), "%s/Library/Caches", path); (void)mkdir(sub, 0755);
			snprintf(sub, sizeof(sub), "%s/Documents", path); (void)mkdir(sub, 0755);
			snprintf(sub, sizeof(sub), "%s/tmp", path); (void)mkdir(sub, 0755);

			Class urlClass = objc_getClass("NSURL");
			Class strClass = objc_getClass("NSString");
			if (urlClass && strClass) {
				id pathStr = ((id (*)(id, SEL, const char *))objc_msgSend)((id)strClass, sel_registerName("stringWithUTF8String:"), path);
				if (pathStr) {
					url = ((id (*)(id, SEL, id, signed char))objc_msgSend)((id)urlClass, sel_registerName("fileURLWithPath:isDirectory:"), pathStr, (signed char)1);
				}
			}
		}
	}
	return url;
}

static void
post_geo_network_reachable_notification(void)
{
	if (!en0_has_ipv4()) {
		return;
	}
	Class notifClass = objc_getClass("NSNotificationCenter");
	Class strClass = objc_getClass("NSString");
	if (notifClass && strClass) {
		id center = ((id (*)(id, SEL))objc_msgSend)((id)notifClass, sel_registerName("defaultCenter"));
		SEL strSel = sel_registerName("stringWithUTF8String:");
		SEL postSel = sel_registerName("postNotificationName:object:");
		if (center && class_getInstanceMethod(object_getClass(center), postSel)) {
			id name1 = ((id (*)(id, SEL, const char *))objc_msgSend)((id)strClass, strSel, "GEONetworkObserverNetworkReachableNotification");
			id name2 = ((id (*)(id, SEL, const char *))objc_msgSend)((id)strClass, strSel, "GEONetworkObserverReachable");
			if (name1) ((void (*)(id, SEL, id, id))objc_msgSend)(center, postSel, name1, nil);
			if (name2) ((void (*)(id, SEL, id, id))objc_msgSend)(center, postSel, name2, nil);
		}
	}
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
watch_network_classes(void)
{
	watch("GEONetworkObserver", "isNetworkReachable", (IMP)hooked_geo_is_net_reachable, &orig_geo_is_net_reachable);
	watch("GEONetworkObserver", "isWiFiReachable", (IMP)hooked_geo_is_wifi_reachable, &orig_geo_is_wifi_reachable);
	watch("GEONetworkObserver", "isCellConnection", (IMP)hooked_geo_is_cell, &orig_geo_is_cell);
	watch("GEONetworkObserver", "isNetworkConstrained", (IMP)hooked_geo_is_constrained, &orig_geo_is_constrained);
	watch("GEONetworkObserver", "isCellularPathConstrained", (IMP)hooked_geo_is_constrained, &orig_geo_is_cell_constrained);
	watch("GEONetworkObserver", "networkReachable", (IMP)hooked_geo_is_net_reachable, &orig_geo_net_reachable);
	watch("GEONetworkObserver", "wifiReachable", (IMP)hooked_geo_is_wifi_reachable, &orig_geo_wifi_reachable);

	Class geoMeta = object_getClass((id)objc_getClass("GEONetworkObserver"));
	if (geoMeta != NULL) {
		Method m = class_getInstanceMethod(geoMeta, sel_registerName("isNetworkReachable"));
		if (m != NULL && method_getImplementation(m) != (IMP)hooked_geo_is_net_reachable) {
			method_setImplementation(m, (IMP)hooked_geo_is_net_reachable);
		}
	}

	watch("NWPath", "status", (IMP)hooked_nwpath_status, &orig_nwpath_status);
	watch("NWPath", "isSatisfied", (IMP)hooked_nwpath_is_satisfied, &orig_nwpath_is_satisfied);
	watch("NWPath", "isExpensive", (IMP)hooked_nwpath_is_expensive, &orig_nwpath_is_expensive);
	watch("NWPath", "isConstrained", (IMP)hooked_nwpath_is_constrained, &orig_nwpath_is_constrained);
	watch("NWPath", "hasIPv4", (IMP)hooked_nwpath_has_ipv4, &orig_nwpath_has_ipv4);
	watch("NWPath", "hasDNS", (IMP)hooked_nwpath_has_dns, &orig_nwpath_has_dns);
	watch("NWPath", "usesInterfaceType:", (IMP)hooked_nwpath_uses_interface, &orig_nwpath_uses_interface);

	watch("NWConcrete_nw_path", "status", (IMP)hooked_nwpath_status, &orig_nwpath_status);
	watch("NWConcrete_nw_path", "isSatisfied", (IMP)hooked_nwpath_is_satisfied, &orig_nwpath_is_satisfied);
	watch("NWConcrete_nw_path", "isExpensive", (IMP)hooked_nwpath_is_expensive, &orig_nwpath_is_expensive);
	watch("NWConcrete_nw_path", "isConstrained", (IMP)hooked_nwpath_is_constrained, &orig_nwpath_is_constrained);
	watch("NWConcrete_nw_path", "hasIPv4", (IMP)hooked_nwpath_has_ipv4, &orig_nwpath_has_ipv4);
	watch("NWConcrete_nw_path", "hasDNS", (IMP)hooked_nwpath_has_dns, &orig_nwpath_has_dns);
	watch("NWConcrete_nw_path", "usesInterfaceType:", (IMP)hooked_nwpath_uses_interface, &orig_nwpath_uses_interface);

	watch("NWPathEvaluator", "isSatisfied", (IMP)hooked_evaluator_is_satisfied, &orig_evaluator_is_satisfied);
	watch("NWPathEvaluator", "status", (IMP)hooked_evaluator_status, &orig_evaluator_status);
	watch("NWConcrete_nw_path_evaluator", "isSatisfied", (IMP)hooked_evaluator_is_satisfied, &orig_evaluator_is_satisfied);
	watch("NWConcrete_nw_path_evaluator", "status", (IMP)hooked_evaluator_status, &orig_evaluator_status);

	watch("NSFileManager", "containerURLForSecurityApplicationGroupIdentifier:", (IMP)watched_container_url_for_group, &orig_container_url_for_group);
	watch_cellular_classes();
}

static void
app_dyld_image_added(const struct mach_header *mh, intptr_t vmaddr_slide)
{
	(void)mh;
	(void)vmaddr_slide;
	watch_network_classes();
	if (!app_vp9_hook_installed) {
		app_enable_webkit_vp9();
	}
}
