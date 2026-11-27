/*
 * umi_gnssd: root daemon bridging the SDX55M kernel QMI LOC GNSS driver
 * (kern.hakios.gnss_enable / kern.hakios.gnss_fix in xnu/files/bsd/dev/umi_gnss.inc)
 * to iOS locationd via CoreLocation's private CLSimulationManager.
 *
 * Behavior:
 *  - Obeys the mobile user's Location Services master switch
 *    (/var/mobile/Library/Preferences/com.apple.locationd.plist: LocationServicesEnabledIn8.0).
 *  - While enabled and baseband is up, renews kern.hakios.gnss_enable=1 every 5 s
 *    (kernel lease timeout is 15 s) and polls kern.hakios.gnss_fix at 1 Hz.
 *  - Only activates CLSimulationManager when a real fresh satellite fix arrives
 *    (UMI_GNSS_POSITION, age_ms < 5000). If sky lock is lost for > 15 s, stops
 *    simulation so locationd seamlessly falls back to Wi-Fi positioning.
 *  - Never logs raw private coordinates.
 */
#import <CoreLocation/CoreLocation.h>
#import <Foundation/Foundation.h>
#include <errno.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#include "../../xnu/files/bsd/dev/umi_gnss_abi.h"

#define LOG(fmt, ...) do { printf("umi_gnssd: " fmt "\n", ##__VA_ARGS__); fflush(stdout); } while (0)

static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

static bool location_services_enabled(void) {
    @autoreleasepool {
        NSDictionary *d = [NSDictionary dictionaryWithContentsOfFile:
            @"/var/mobile/Library/Preferences/com.apple.locationd.plist"];
        if (!d) return false;
        id v = d[@"LocationServicesEnabledIn8.0"];
        if (!v) v = d[@"LocationServicesEnabled"];
        return [v respondsToSelector:@selector(boolValue)] && [v boolValue];
    }
}

static int set_gnss_enable(int val) {
    int v = val ? 1 : 0;
    if (sysctlbyname("kern.hakios.gnss_enable", NULL, NULL, &v, sizeof(v)) != 0)
        return -errno;
    return 0;
}

static int get_gnss_fix(struct umi_gnss_fix *out) {
    size_t sz = sizeof(*out);
    memset(out, 0, sizeof(*out));
    if (sysctlbyname("kern.hakios.gnss_fix", out, &sz, NULL, 0) != 0)
        return -errno;
    return (sz == sizeof(*out) && out->version == UMI_GNSS_ABI) ? 0 : -EPROTO;
}

static double bits_to_double(uint64_t b) { double d; memcpy(&d, &b, sizeof(d)); return d; }
static float bits_to_float(uint32_t b) { float f; memcpy(&f, &b, sizeof(f)); return f; }

static id g_sim_mgr;
static bool g_sim_active;

static bool ensure_sim_manager(void) {
    if (g_sim_mgr) return true;
    Class cls = objc_getClass("CLSimulationManager");
    if (!cls) {
        LOG("CLSimulationManager class unavailable");
        return false;
    }
    id mgr = ((id (*)(id, SEL))objc_msgSend)((id)cls, sel_registerName("alloc"));
    mgr = ((id (*)(id, SEL))objc_msgSend)(mgr, sel_registerName("init"));
    if (!mgr) {
        LOG("CLSimulationManager init failed");
        return false;
    }
    SEL s_deliv = sel_registerName("setLocationDeliveryBehavior:");
    if ([mgr respondsToSelector:s_deliv])
        ((void (*)(id, SEL, uint8_t))objc_msgSend)(mgr, s_deliv, 2); /* immediate */
    SEL s_rep = sel_registerName("setLocationRepeatBehavior:");
    if ([mgr respondsToSelector:s_rep])
        ((void (*)(id, SEL, uint8_t))objc_msgSend)(mgr, s_rep, 0); /* no loop */
    g_sim_mgr = mgr;
    LOG("CLSimulationManager ready (real GNSS -> locationd bridge)");
    return true;
}

static void sim_stop(const char *why) {
    if (!g_sim_active || !g_sim_mgr) return;
    SEL s_stop = sel_registerName("stopLocationSimulation");
    SEL s_clr = sel_registerName("clearSimulatedLocations");
    SEL s_fl = sel_registerName("flush");
    if ([g_sim_mgr respondsToSelector:s_stop]) ((void (*)(id, SEL))objc_msgSend)(g_sim_mgr, s_stop);
    if ([g_sim_mgr respondsToSelector:s_clr]) ((void (*)(id, SEL))objc_msgSend)(g_sim_mgr, s_clr);
    if ([g_sim_mgr respondsToSelector:s_fl]) ((void (*)(id, SEL))objc_msgSend)(g_sim_mgr, s_fl);
    g_sim_active = false;
    LOG("stopped simulated delivery (%s); Wi-Fi positioning active", why);
}

static bool sim_push_fix(const struct umi_gnss_fix *fix) {
    if (!ensure_sim_manager()) return false;
    double lat = bits_to_double(fix->latitude_bits);
    double lon = bits_to_double(fix->longitude_bits);
    double hacc = (double)bits_to_float(fix->accuracy_bits);
    double alt = (fix->flags & UMI_GNSS_ALTITUDE) ? (double)bits_to_float(fix->altitude_bits) : 0.0;
    double vacc = (fix->flags & UMI_GNSS_ALTITUDE) ? (double)bits_to_float(fix->vertical_accuracy_bits) : -1.0;
    NSDate *ts = (fix->flags & UMI_GNSS_UTC) && fix->utc_ms > 1500000000000ULL
        ? [NSDate dateWithTimeIntervalSince1970:(double)fix->utc_ms / 1000.0]
        : [NSDate date];
    CLLocation *loc = [[CLLocation alloc] initWithCoordinate:CLLocationCoordinate2DMake(lat, lon)
                                                    altitude:alt
                                          horizontalAccuracy:hacc
                                            verticalAccuracy:vacc
                                                   timestamp:ts];
    SEL s_clr = sel_registerName("clearSimulatedLocations");
    SEL s_app = sel_registerName("appendSimulatedLocation:");
    SEL s_start = sel_registerName("startLocationSimulation");
    SEL s_fl = sel_registerName("flush");
    if (!loc || ![g_sim_mgr respondsToSelector:s_app] ||
        (!g_sim_active && ![g_sim_mgr respondsToSelector:s_start])) return false;
    if ([g_sim_mgr respondsToSelector:s_clr]) ((void (*)(id, SEL))objc_msgSend)(g_sim_mgr, s_clr);
    ((void (*)(id, SEL, id))objc_msgSend)(g_sim_mgr, s_app, loc);
    if (!g_sim_active) {
        ((void (*)(id, SEL))objc_msgSend)(g_sim_mgr, s_start);
        g_sim_active = true;
    }
    if ([g_sim_mgr respondsToSelector:s_fl]) ((void (*)(id, SEL))objc_msgSend)(g_sim_mgr, s_fl);
    return true;
}

int main(void) {
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    LOG("starting (uid=%u)", (unsigned)getuid());

    bool waited_bb = false;
    bool lease_on = false;
    time_t lease_since = 0;
    time_t last_renew = 0;
    time_t last_queued = 0;
    uint32_t last_seq = 0;
    uint32_t last_view = 0xffffffffu;
    uint32_t last_status = 0xffffffffu;
    uint32_t queued = 0;
    int exit_status = 0;
    unsigned retries = 0;

    /* Give CommCenter a few seconds to bring SDX55M out of LPM before opening QMI LOC */
    sleep(8);

    while (!g_stop) {
        @autoreleasepool {
            struct timespec monotonic;
            if (clock_gettime(CLOCK_MONOTONIC, &monotonic) != 0) {
                LOG("cannot read the GNSS lease clock (err=%d)", errno);
                exit_status = 1;
                break;
            }
            // The modem lease uses elapsed time. UTC is only for CLLocation timestamps.
            time_t now = monotonic.tv_sec;
            bool want = location_services_enabled();
            if (!want) {
                if (lease_on) {
                    (void)set_gnss_enable(0);
                    lease_on = false;
                    LOG("Location Services disabled; stopped baseband GNSS lease");
                }
                sim_stop("Location Services disabled");
                sleep(2);
                continue;
            }

            if (!lease_on || now - last_renew >= 5) {
                int rc = set_gnss_enable(1);
                if (rc != 0) {
                    if (!waited_bb) {
                        LOG("baseband GNSS unavailable (err=%d); waiting for SDX55M modem", -rc);
                        waited_bb = true;
                    }
                    lease_on = false;
                    sim_stop("baseband unavailable");
                    sleep(3);
                    continue;
                }
                if (!lease_on) {
                    LOG("GNSS lease active on SDX55M QMI LOC");
                    waited_bb = false;
                    lease_on = true;
                    lease_since = now;
                }
                last_renew = now;
            }

            struct umi_gnss_fix fix;
            if (get_gnss_fix(&fix) == 0) {
                if (fix.session_status != last_status) {
                    last_status = fix.session_status;
                    LOG("QMI LOC status=%u (0=fix, 1=searching) flags=0x%x view=%u used=%u",
                        fix.session_status, fix.flags, fix.satellites_view, fix.satellites_used);
                }
                if ((fix.flags & UMI_GNSS_VIEW) && fix.satellites_view != last_view) {
                    last_view = fix.satellites_view;
                    LOG("satellites in view=%u used=%u", fix.satellites_view, fix.satellites_used);
                }
                if ((fix.flags & UMI_GNSS_POSITION) && fix.sequence != last_seq && fix.age_ms < 5000) {
                    last_seq = fix.sequence;
                    if (sim_push_fix(&fix)) {
                        last_queued = now;
                        queued++;
                        if (queued <= 5 || queued % 30 == 0) {
                            LOG("queued GNSS fix #%u for locationd (acc=%.1f m, view=%u, used=%u, age=%u ms)",
                                queued, (double)bits_to_float(fix.accuracy_bits),
                                fix.satellites_view, fix.satellites_used, fix.age_ms);
                        }
                    }
                }
            }
            if (g_sim_active && last_queued > 0 && now - last_queued > 15) {
                sim_stop("GNSS fix stale > 15 s");
            }
            /* If early boot raced CommCenter ONLINE (QMI_LOC_START returned error and latched failed),
             * cycle the lease once after 20 s / 45 s so the kernel retries QMI LOC Start cleanly. */
            if (lease_on && retries < 3 && last_seq == 0 && last_view == 0xffffffffu &&
                now - lease_since >= (time_t)(20 + retries * 15)) {
                retries++;
                LOG("no QMI LOC SV/fix yet after %ld s; cycling GNSS lease (retry %u/3)",
                    (long)(now - lease_since), retries);
                (void)set_gnss_enable(0);
                lease_on = false;
                sleep(2);
                continue;
            }
        }
        sleep(1);
    }
    if (lease_on) (void)set_gnss_enable(0);
    sim_stop("shutdown");
    LOG("exiting (fixes queued=%u)", queued);
    return exit_status;
}
