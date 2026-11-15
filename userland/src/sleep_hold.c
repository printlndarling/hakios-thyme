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
#define PM_AGGRESSIVENESS_TYPE_MINUTES_TO_DIM   1      /* kPMMinutesToDim */
#define PM_SET_AGGRESSIVENESS                   0      /* kPMSetAggressiveness */
#define PM_GET_AGGRESSIVENESS                   1      /* kPMGetAggressiveness */

static volatile sig_atomic_t stopped;

static void
stop(int signal_number)
{
	(void)signal_number;
	stopped = 1;
}


int
main(int argc, char **argv)
{
	io_service_t service;
	io_connect_t connection = 0;
	uint64_t input[2] = { PM_AGGRESSIVENESS_TYPE_MINUTES_TO_SLEEP, 0 };
	uint64_t previous[2] = { 0, 0 };
	int absent[2] = { 0, 0 };
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
	/* Both timers are held: the system's sleep (kPMMinutesToSleep) and the display's dimming and
	 * sleep (kPMMinutesToDim). With only the first held the display still went away, and the
	 * demo job was suspended with it (logs/095245-run.log). */
	for (unsigned i = 0; i < 2; i++) {
		uint64_t type = i == 0 ? PM_AGGRESSIVENESS_TYPE_MINUTES_TO_SLEEP : PM_AGGRESSIVENESS_TYPE_MINUTES_TO_DIM;
		uint64_t was = 0;
		uint32_t was_count = 1;

		input[0] = type;
		result = IOConnectCallScalarMethod(connection, PM_GET_AGGRESSIVENESS, input, 1, &was, &was_count);
		if (result == kIOReturnInvalid && was_count == 1) {
			/*
			 * No record for this type yet: the root domain says so with kIOReturnInvalid and
			 * answers 0 (IOPMrootDomain::getAggressiveness). powerd sets the records later in a
			 * run; until then the timers are the system's own. Hold anyway, and put a minute
			 * back on exit rather than the absent 0.
			 */
			absent[i] = 1;
			was = 0;
		} else if (result != KERN_SUCCESS || was_count == 0) {
			printf("sleep_hold: the root domain will not say what timer %llu was (get 0x%x, %u value(s)); not holding\n",
			    type, result, was_count);
			IOServiceClose(connection);
			return 1;
		}
		previous[i] = was;
	}
	for (unsigned i = 0; i < 2; i++) {
		uint64_t type = i == 0 ? PM_AGGRESSIVENESS_TYPE_MINUTES_TO_SLEEP : PM_AGGRESSIVENESS_TYPE_MINUTES_TO_DIM;

		input[0] = type;
		input[1] = 0;
		{
			uint64_t output[1] = { 0 };
			uint32_t output_count = 1;

			result = IOConnectCallScalarMethod(connection, PM_SET_AGGRESSIVENESS, input, 2, output, &output_count);
		}
		if (result != KERN_SUCCESS) {
			printf("sleep_hold: timer %llu = 0 refused: 0x%x%s\n", type, result,
			    result == kIOReturnNotPrivileged ? " (not an administrator)" : "");
			IOServiceClose(connection);
			return 1;
		}
	}
	printf("sleep_hold: idle sleep and display dim held off (sleep was %llu, dim was %llu)\n", previous[0], previous[1]);

	if (argc > 1) {
		for (unsigned elapsed = 0; elapsed < seconds && !stopped; elapsed++) {
			sleep(1);
		}
	} else {
		while (!stopped) {
			sleep(1);
		}
	}
	for (unsigned i = 0; i < 2; i++) {
		uint64_t type = i == 0 ? PM_AGGRESSIVENESS_TYPE_MINUTES_TO_SLEEP : PM_AGGRESSIVENESS_TYPE_MINUTES_TO_DIM;
		uint64_t value = absent[i] ? 1 : previous[i];

		input[0] = type;
		input[1] = value;
		{
			uint64_t output[1] = { 0 };
			uint32_t output_count = 1;

			(void)IOConnectCallScalarMethod(connection, PM_SET_AGGRESSIVENESS, input, 2, output, &output_count);
		}
		printf("sleep_hold: timer %llu %s %llu\n", type, absent[i] ? "was absent; set to" : "put back to", value);
	}
	IOServiceClose(connection);
	return 0;
}
