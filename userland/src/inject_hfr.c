/*
 * libhakios.hfr.dylib: lightweight global high-refresh-rate hook for all UIKit apps on HakiOS.
 *
 * On a 90 Hz panel (Xiaomi Mi 10S / thyme), QuartzCore's integer divisor table maps any 60 Hz
 * request down to 90 / 2 = 45 Hz (22.2 ms), and CoreAnimation's default CAAnimation rate drops
 * to 24-60 Hz. This small constructor disables CoreAnimation's high-frame-rate restriction and
 * lifts interactive (>= 48 Hz) CADisplayLink and default CAAnimation requests to full panel rate,
 * while preserving intentional low-rate timers (< 48 Hz, e.g. 10 Hz clocks and 24/30 fps video).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

typedef struct { float minimum, maximum, preferred; } hakios_frr_t;

static id (*orig_dl_with)(id, SEL, id, SEL);
static void (*orig_set_fps)(id, SEL, long);
static void (*orig_set_range)(id, SEL, hakios_frr_t);
static void (*orig_add_anim)(id, SEL, id, id);
static char hfr_anim_mgr_key;

static bool hfr_boot_disabled(void) {
    char args[1024] = "";
    size_t sz = sizeof(args) - 1;
    if (sysctlbyname("kern.bootargs", args, &sz, NULL, 0) != 0) return false;
    const char *p = strstr(args, "anim_max=0");
    return p != NULL && (p == args || p[-1] == ' ');
}

static id hfr_dl_with(id self, SEL sel, id target, SEL action) {
    id link = orig_dl_with(self, sel, target, action);
    if (link && target && strcmp(object_getClassName(target), "UIViewInProcessAnimationManager") == 0) {
        objc_setAssociatedObject(link, &hfr_anim_mgr_key, (id)kCFBooleanTrue, OBJC_ASSOCIATION_ASSIGN);
    }
    return link;
}

static void hfr_set_fps(id self, SEL sel, long fps) {
    if (objc_getAssociatedObject(self, &hfr_anim_mgr_key) != NULL || (fps >= 48 && fps < 90)) {
        fps = 0; /* 0 = native panel rate (90 Hz), avoids 90/2 = 45 Hz drop */
    }
    orig_set_fps(self, sel, fps);
}

static void hfr_set_range(id self, SEL sel, hakios_frr_t r) {
    if (objc_getAssociatedObject(self, &hfr_anim_mgr_key) != NULL ||
        (r.maximum >= 48.0f && r.maximum < 90.0f) ||
        (r.preferred >= 48.0f && r.preferred < 90.0f)) {
        r = (hakios_frr_t){ 0.0f, 0.0f, 0.0f };
    }
    orig_set_range(self, sel, r);
}

static void hfr_add_anim(id self, SEL sel, id anim, id key) {
    static SEL s_get, s_set, s_copy, s_rel;
    if (!s_get) {
        s_get = sel_registerName("preferredFrameRateRange");
        s_set = sel_registerName("setPreferredFrameRateRange:");
        s_copy = sel_registerName("copy");
        s_rel = sel_registerName("release");
    }
    if (anim && class_respondsToSelector(object_getClass(anim), s_set)) {
        hakios_frr_t asked = ((hakios_frr_t (*)(id, SEL))objc_msgSend)(anim, s_get);
        if (asked.maximum == 0.0f || (asked.maximum >= 48.0f && asked.maximum < 90.0f) ||
            (asked.maximum >= 48.0f && asked.minimum < 80.0f)) {
            id copy = ((id (*)(id, SEL))objc_msgSend)(anim, s_copy);
            if (copy) {
                ((void (*)(id, SEL, hakios_frr_t))objc_msgSend)(copy, s_set, (hakios_frr_t){ 80.0f, 120.0f, 120.0f });
                orig_add_anim(self, sel, copy, key);
                ((void (*)(id, SEL))objc_msgSend)(copy, s_rel);
                return;
            }
        }
    }
    orig_add_anim(self, sel, anim, key);
}

__attribute__((constructor))
static void hakios_hfr_init(void) {
    setenv("CA_ENABLE_HIGH_FRAME_RATE_RESTRICTION", "0", 1);
    if (hfr_boot_disabled()) return;
    fprintf(stderr, "hakios: hfr 90Hz active in %s\n", getprogname() ? getprogname() : "?");
    Class dl = objc_getClass("CADisplayLink");
    if (dl) {
        Method m;
        if ((m = class_getClassMethod(dl, sel_registerName("displayLinkWithTarget:selector:"))))
            orig_dl_with = (id (*)(id, SEL, id, SEL))method_setImplementation(m, (IMP)hfr_dl_with);
        if ((m = class_getInstanceMethod(dl, sel_registerName("setPreferredFramesPerSecond:"))))
            orig_set_fps = (void (*)(id, SEL, long))method_setImplementation(m, (IMP)hfr_set_fps);
        if ((m = class_getInstanceMethod(dl, sel_registerName("setPreferredFrameRateRange:"))))
            orig_set_range = (void (*)(id, SEL, hakios_frr_t))method_setImplementation(m, (IMP)hfr_set_range);
    }
    Class layer = objc_getClass("CALayer");
    if (layer) {
        Method add = class_getInstanceMethod(layer, sel_registerName("addAnimation:forKey:"));
        if (add) orig_add_anim = (void (*)(id, SEL, id, id))method_setImplementation(add, (IMP)hfr_add_anim);
    }
}
