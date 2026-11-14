/*
 * sleep_hold [SECONDS]: keep the system from idle-sleeping while a scene runs.
 *
 * A scene cannot wake a sleeping phone: the system's idle sleep suspends every scene process
 * (the taps of rwtouch included), and three Wi-Fi runs stalled on exactly that (CHECKLIST,
 * 2026-11-15). powerd does the same thing when an application holds a display-sleep assertion,
 * with IOPMrootDomain's setAggressiveness: the type is kPMMinutesToSleep (3) and the value 0 is
 * "never". The call goes through IOPMrootDomain's user client, method 0 (kPMSetAggressiveness in
 * IOKit/pwr_mgt/IOPMLibDefs.h), and the other method used here is 1 (kPMGetAggressiveness).
 *
 *   sleep_hold 600      hold for ten minutes, then put the previous setting back
 *   sleep_hold          hold until the process is stopped (SIGTERM), then put it back
 *
 * The value is system-wide: hold only for a test, never in the daily image.
 */
#include <IOKit/IOKitLib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PM_AGGRESSIVENESS_TYPE_MINUTES_TO_SLEEP 3      /* kPMMinutesToSleep */
#define PM_SET_AGGRESSIVENESS                   0      /* kPMSetAggressiveness */
#define PM_GET_AGGRESSIVENESS                   1      /* kPMGetAggressiveness */

static volatile sig_atomic_t stopped;

static void
stop(int signal_number)
{
	(void)signal_number;
	stopped = 1;
}

static int
set_minutes_to_sleep(io_connect_t connection, uint64_t minutes)
{
	uint64_t input[2] = { PM_AGGRESSIVENESS_TYPE_MINUTES_TO_SLEEP, minutes };
	uint64_t output[1] = { 0 };
	uint32_t output_count = 1;
	kern_return_t result = IOConnectCallScalarMethod(connection, PM_SET_AGGRESSIVENESS, input, 2, output, &output_count);

	if (result != KERN_SUCCESS) {
		printf("sleep_hold: kPMMinutesToSleep=%llu refused: 0x%x%s\n", minutes, result,
		    result == kIOReturnNotPrivileged ? " (not an administrator)" : "");
		return -1;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	io_service_t service;
	io_connect_t connection = 0;
	uint64_t input[1] = { PM_AGGRESSIVENESS_TYPE_MINUTES_TO_SLEEP };
	uint64_t previous = 0;
	uint32_t previous_count = 1;
	unsigned seconds = argc > 1 ? (unsigned)atoi(argv[1]) : 0;
	kern_return_t result;

	service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOPMrootDomain"));
	if (service == 0) {
		printf("sleep_hold: there is no IOPMrootDomain here\n");
		return 1;
	}
	result = IOServiceOpen(service, mach_task_self(), 0, &connection);
	IOObjectRelease(service);
	if (result != KERN_SUCCESS) {
		printf("sleep_hold: the root domain will not open a user client: 0x%x\n", result);
		return 1;
	}
	if (IOConnectCallScalarMethod(connection, PM_GET_AGGRESSIVENESS, input, 1, &previous, &previous_count) != KERN_SUCCESS ||
	    previous_count == 0) {
		/* without the old value there is nothing to put back: the hold would outlive the test */
		printf("sleep_hold: the root domain will not say what kPMMinutesToSleep was; not holding\n");
		IOServiceClose(connection);
		return 1;
	}
	if (set_minutes_to_sleep(connection, 0) != 0) {
		IOServiceClose(connection);
		return 1;
	}
	printf("sleep_hold: idle sleep held off (kPMMinutesToSleep was %llu%s)\n", previous,
	    previous_count == 0 ? ", unread" : "");

	if (argc > 1) {
		for (unsigned elapsed = 0; elapsed < seconds && !stopped; elapsed++) {
			sleep(1);
		}
	} else {
		while (!stopped) {
			sleep(1);
		}
	}
	if (previous_count != 0) {
		(void)set_minutes_to_sleep(connection, previous);
		printf("sleep_hold: kPMMinutesToSleep put back to %llu\n", previous);
	}
	IOServiceClose(connection);
	return 0;
}
