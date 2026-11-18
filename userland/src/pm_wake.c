/*
 * pm_wake [SECONDS] [sleep]: schedule a system wake, then sleep the system.
 *
 * The one Wi-Fi path a scene could not reach is wake-from-sleep: the idle sleep suspends
 * every userspace process (a tap never happens), and the Wi-Fi keepalive's rejoin after a
 * wake needed a human's touch to wake the phone.  IOPM can do both halves itself: schedule
 * an RTC wake event and put the system to sleep immediately.  When the scheduled time
 * arrives the kernel wakes (full wake, not a dark wake if the wake type is "wake"), the
 * demo job resumes, and the rest of the scene (net_test show/resolve) runs on the far side
 * of a real sleep.
 *
 * The tool needs root (it runs from the scene, which is root).  If no wake arrives, the
 * phone stays asleep until it is touched or the power button is pressed -- the negative
 * result this test is prepared to show.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* IOPMLibPrivate.h is not in the SDK, but IOKit exports both: */
extern IOReturn IOPMSchedulePowerEvent(CFAbsoluteTime time_to_wake, CFStringRef my_id, CFStringRef type);
extern io_connect_t IOPMFindPowerManagement(mach_port_t master_port);
extern IOReturn IOPMSleepSystem(io_connect_t root_domain_port);

int
main(int argc, char **argv)
{
	unsigned seconds = argc > 1 ? (unsigned)atoi(argv[1]) : 40;
	int sleep_now = argc > 2 && strcmp(argv[2], "sleep") == 0;
	CFAbsoluteTime at;
	IOReturn result;

	at = CFAbsoluteTimeGetCurrent() + (CFAbsoluteTime)seconds;
	result = IOPMSchedulePowerEvent(at, CFSTR("com.hakios.pmwake"), CFSTR("wake"));
	printf("pm_wake: wake scheduled %u s ahead -> 0x%x\n", seconds, result);
	fflush(stdout);
	if (!sleep_now) {
		return result == kIOReturnSuccess ? 0 : 1;
	}
	if (result != kIOReturnSuccess) {
		printf("pm_wake: not sleeping: the wake could not be scheduled\n");
		return 1;
	}
	io_connect_t connection = IOPMFindPowerManagement(kIOMainPortDefault);

	if (connection == 0) {
		printf("pm_wake: IOPMFindPowerManagement returned no connection\n");
		return 1;
	}
	printf("pm_wake: putting the system to sleep now\n");
	fflush(stdout);
	result = IOPMSleepSystem(connection);
	printf("pm_wake: IOPMSleepSystem returned 0x%x (this line runs after the wake)\n", result);
	fflush(stdout);
	return 0;
}
