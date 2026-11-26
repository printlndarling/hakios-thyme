/*
 * Darwin-on-hakios: what stands between the two sides of the station boundary
 * (sys/hakios.sta.h).
 *
 * Whoever wants a network asks from wherever it happens to be: an ioctl of wifid's, the thread
 * that brings the radio up. The station side wants to be called by one thread, one call at a
 * time, and a scan or a join takes seconds. So what is asked for is noted here and done by a
 * thread of this file's; the asker returns at once and hears the outcome as an event. One
 * request of each kind can wait: a second scan asked for while the first has not begun
 * replaces it, as does a second join.
 *
 * What the last scan heard is kept here, so that it can be read without entering the station
 * side, and so is the list of channels.
 *
 * The network the kernel was given in a file is a standing request: joined at the start, and
 * again whenever the radio is turned back on (Settings' switch), until somebody asks for
 * another network -- as Darwin itself rejoins the networks it knows. If that other network cannot
 * be joined (a wrong passphrase), the standing one is joined again.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <sys/errno.h>
#include <sys/hakios.input.h>
#include <sys/hakios.sta.h>
#include <sys/systm.h>

extern void thread_set_thread_name(thread_t thread, const char *name);

#define HEARD_MOST      128
#define CHANNELS_MOST   96

static LCK_GRP_DECLARE(sta_lock_group, "hakios-sta");
static LCK_MTX_DECLARE(sta_lock, &sta_lock_group);

static const struct hakios_sta_ops *sta_ops;   /* the station side, while there is one */
static bool sta_thread_made;
static bool sta_calls;                          /* the thread is in a call of the station side's */
static unsigned sta_reads;                      /* ... and so many others are in its state() */
static bool sta_on;                             /* the radio's power, as last asked for */

/* what has been asked for and not begun */
static bool asked_power, asked_power_on, asked_scan, asked_join, asked_leave, asked_rejoin;
static struct hakios_sta_scan asked_scan_what;
static struct hakios_sta_join asked_join_what;
static struct hakios_sta_join standing;        /* the network joined whenever the radio comes on (ssid_length 0: none) */
static struct hakios_sta_join fallback;        /* ... as it was before a join that has not come through yet */
static thread_call_t rejoin_call;              /* delayed auto-rejoin after involuntary link loss */
static unsigned rejoin_attempts;
static uint32_t rejoin_gen;

static void
sta_rejoin_fire(__unused thread_call_param_t p0, thread_call_param_t p1)
{
	uint32_t gen = (uint32_t)(uintptr_t)p1;
	bool wake = false;

	lck_mtx_lock(&sta_lock);
	if (gen == rejoin_gen && sta_ops != NULL && sta_on && standing.ssid_length != 0 &&
	    !asked_leave && !asked_power && !asked_join) {
		asked_join_what = standing;
		asked_join = true;
		asked_rejoin = true;
		rejoin_attempts++;
		wake = true;
	}
	lck_mtx_unlock(&sta_lock);
	if (wake) {
		hakios_wifi_tell("wifi: automatic standing-network rejoin attempt %u\n", rejoin_attempts);
		thread_wakeup((event_t)&sta_ops);
	}
}

static struct hakios_sta_bss *heard;           /* by the last scan that came to an end */
static unsigned heard_count;
static struct hakios_sta_channel channels[CHANNELS_MOST];
static unsigned channel_count;

static void
heard_one(const struct hakios_sta_bss *bss, __unused void *context)
{
	/* (only the thread writes the list, and it reads the count without the lock) */
	if (heard_count < HEARD_MOST) {
		lck_mtx_lock(&sta_lock);
		heard[heard_count++] = *bss;
		lck_mtx_unlock(&sta_lock);
	}
}

static void
sta_runs(__unused void *parameter, __unused wait_result_t result)
{
	static struct hakios_sta_scan scan;
	static struct hakios_sta_join join;

	for (;;) {
		const struct hakios_sta_ops *ops;
		bool power = false, power_on = false, leave = false, do_join = false, do_scan = false, was_rejoin = false;
		int error = 0;

		lck_mtx_lock(&sta_lock);
		while (sta_ops == NULL || !(asked_power || asked_leave || asked_join || asked_scan)) {
			lck_mtx_sleep(&sta_lock, LCK_SLEEP_DEFAULT, (event_t)&sta_ops, THREAD_UNINT);
		}
		ops = sta_ops;
		/* one thing at a time, in the order that makes sense when several wait */
		if (asked_power) {
			power = true;
			power_on = asked_power_on;
			asked_power = false;
		} else if (asked_leave) {
			leave = true;
			asked_leave = false;
		} else if (asked_join) {
			do_join = true;
			was_rejoin = asked_rejoin;
			asked_rejoin = false;
			join = asked_join_what;
			bzero(&asked_join_what, sizeof(asked_join_what));
			asked_join = false;
		} else {
			do_scan = true;
			scan = asked_scan_what;
			asked_scan = false;
		}
		sta_calls = true;
		lck_mtx_unlock(&sta_lock);

		if (power) {
			struct hakios_sta_state *state = kalloc_data(sizeof(*state), Z_WAITOK | Z_ZERO);
			bool was_joined = false;

			if (state != NULL) {
				ops->state(state);
				was_joined = state->phase == HAKIOS_STA_JOINED;
				kfree_data(state, sizeof(*state));
			}
			error = ops->power(power_on);
			if (!power_on && was_joined) {
				hakios_apple80211_event(HAKIOS_STA_LINK, 0);
			}
			hakios_apple80211_event(HAKIOS_STA_POWER, error == 0 ? power_on : !power_on);
			lck_mtx_lock(&sta_lock);
			if (error == 0 && power_on && standing.ssid_length != 0 && !asked_join) {
				asked_join_what = standing;
				asked_join = true;
				asked_rejoin = true;
				rejoin_attempts = 0;
			}
			lck_mtx_unlock(&sta_lock);
		} else if (leave) {
			ops->leave();
			hakios_apple80211_event(HAKIOS_STA_LINK, 0);
		} else if (do_join) {
			bool schedule_retry = false;
			uint32_t retry_gen = 0, delay_s = 0;

			error = ops->join(&join);
			bzero(&join, sizeof(join));
			hakios_apple80211_event(HAKIOS_STA_JOIN_DONE, error);
			if (error == 0) {
				hakios_apple80211_event(HAKIOS_STA_LINK, 1);
			}
			/* a join that did not come through (a wrong passphrase) leaves things as they were: the standing network again */
			lck_mtx_lock(&sta_lock);
			if (error == 0) {
				rejoin_attempts = 0;
				rejoin_gen++;
			} else if (was_rejoin) {
				if (sta_ops != NULL && sta_on && standing.ssid_length != 0 &&
				    !asked_join && !asked_leave && !asked_power && rejoin_call != NULL) {
					delay_s = rejoin_attempts <= 1 ? 5 : rejoin_attempts == 2 ? 10 : 30;
					retry_gen = ++rejoin_gen;
					schedule_retry = true;
				}
			} else if (fallback.ssid_length != 0 && !asked_join && !asked_leave && !asked_power) {
				standing = asked_join_what = fallback;
				asked_join = true;
			}
			if (!was_rejoin) {
				bzero(&fallback, sizeof(fallback));
			}
			lck_mtx_unlock(&sta_lock);
			if (error == 0 && rejoin_call != NULL) {
				thread_call_cancel(rejoin_call);
			} else if (schedule_retry) {
				uint64_t deadline;

				clock_interval_to_deadline(delay_s, NSEC_PER_SEC, &deadline);
				thread_call_enter1_delayed(rejoin_call, (thread_call_param_t)(uintptr_t)retry_gen, deadline);
			}
		} else if (do_scan) {
			error = ops->scan(&scan);
			lck_mtx_lock(&sta_lock);
			heard_count = 0;
			lck_mtx_unlock(&sta_lock);
			ops->heard(heard_one, NULL);
			hakios_apple80211_event(HAKIOS_STA_SCAN_DONE, error);
		}

		lck_mtx_lock(&sta_lock);
		sta_calls = false;
		lck_mtx_unlock(&sta_lock);
		thread_wakeup((event_t)&sta_calls);
	}
}

/* ---- the station side ---------------------------------------------------------------------------------- */

void
hakios_sta_attach(const struct hakios_sta_ops *ops)
{
	thread_t thread;

	if (ops == NULL) {
		lck_mtx_lock(&sta_lock);
		sta_ops = NULL;
		rejoin_gen++;
		asked_power = asked_scan = asked_join = asked_leave = asked_rejoin = false;
		lck_mtx_unlock(&sta_lock);
		if (rejoin_call != NULL) {
			thread_call_cancel_wait(rejoin_call);
		}
		lck_mtx_lock(&sta_lock);
		while (sta_calls || sta_reads != 0) {
			lck_mtx_sleep(&sta_lock, LCK_SLEEP_DEFAULT, (event_t)&sta_calls, THREAD_UNINT);
		}
		heard_count = channel_count = 0;
		lck_mtx_unlock(&sta_lock);
		return;
	}
	if (heard == NULL) {
		heard = kalloc_data(HEARD_MOST * sizeof(*heard), Z_WAITOK | Z_ZERO);
	}
	if (heard == NULL) {
		hakios_wifi_tell("wifi: no memory for what a scan hears: nobody can ask the radio for a network\n");
		return;
	}
	if (rejoin_call == NULL) {
		rejoin_call = thread_call_allocate(sta_rejoin_fire, NULL);
	}
	if (!sta_thread_made) {
		if (kernel_thread_start(sta_runs, NULL, &thread) != KERN_SUCCESS) {
			hakios_wifi_tell("wifi: no thread for the station: nobody can ask the radio for a network\n");
			return;
		}
		thread_set_thread_name(thread, "wifi-station");
		thread_deallocate(thread);
		sta_thread_made = true;
	}
	channel_count = ops->channels(channels, CHANNELS_MOST);
	lck_mtx_lock(&sta_lock);
	sta_ops = ops;
	sta_on = true;
	rejoin_attempts = 0;
	asked_rejoin = false;
	rejoin_gen++;
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	hakios_apple80211_event(HAKIOS_STA_POWER, 1);
}

void
hakios_sta_lost(int error)
{
	bool schedule = false;
	uint32_t gen = 0;
	int reason = error != 0 ? error : ECONNRESET;

	hakios_apple80211_event(HAKIOS_STA_LINK, reason);
	lck_mtx_lock(&sta_lock);
	if (sta_ops != NULL && sta_on && standing.ssid_length != 0 &&
	    !asked_leave && !asked_power && !asked_join && rejoin_call != NULL) {
		rejoin_attempts = 0;
		gen = ++rejoin_gen;
		schedule = true;
	}
	lck_mtx_unlock(&sta_lock);
	if (schedule) {
		uint64_t deadline;

		hakios_wifi_tell("wifi: link lost (error %d); scheduling automatic rejoin in 2 s\n", reason);
		clock_interval_to_deadline(2, NSEC_PER_SEC, &deadline);
		thread_call_enter1_delayed(rejoin_call, (thread_call_param_t)(uintptr_t)gen, deadline);
	}
}

/* ---- who wants a network ------------------------------------------------------------------------------- */

bool
hakios_sta_present(void)
{
	return sta_ops != NULL;
}

bool
hakios_sta_powered(void)
{
	return sta_ops != NULL && sta_on;
}

bool
hakios_sta_busy(void)
{
	bool busy;

	lck_mtx_lock(&sta_lock);
	busy = sta_ops != NULL && (sta_calls || asked_power || asked_leave || asked_join || asked_scan);
	lck_mtx_unlock(&sta_lock);
	return busy;
}

int
hakios_sta_ask_power(bool on)
{
	lck_mtx_lock(&sta_lock);
	if (sta_ops == NULL) {
		lck_mtx_unlock(&sta_lock);
		return ENXIO;
	}
	sta_on = on;
	asked_power = true;
	asked_power_on = on;
	rejoin_gen++;
	rejoin_attempts = 0;
	asked_rejoin = false;
	if (!on) {
		asked_scan = asked_join = asked_leave = false;
	}
	lck_mtx_unlock(&sta_lock);
	if (rejoin_call != NULL) {
		thread_call_cancel(rejoin_call);
	}
	thread_wakeup((event_t)&sta_ops);
	return 0;
}

int
hakios_sta_ask_scan(const struct hakios_sta_scan *what)
{
	lck_mtx_lock(&sta_lock);
	if (sta_ops == NULL || !sta_on) {
		lck_mtx_unlock(&sta_lock);
		return sta_ops == NULL ? ENXIO : ENETDOWN;
	}
	asked_scan_what = *what;
	asked_scan = true;
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	return 0;
}

int
hakios_sta_ask_join_standing(const struct hakios_sta_join *what)
{
	int error = hakios_sta_ask_join(what);

	lck_mtx_lock(&sta_lock);
	if (error == 0) {
		standing = *what;
	}
	lck_mtx_unlock(&sta_lock);
	return error;
}

int
hakios_sta_ask_join(const struct hakios_sta_join *what)
{
	lck_mtx_lock(&sta_lock);
	if (sta_ops == NULL || !sta_on) {
		lck_mtx_unlock(&sta_lock);
		return sta_ops == NULL ? ENXIO : ENETDOWN;
	}
	if (standing.ssid_length != 0) {
		fallback = standing;
	}
	bzero(&standing, sizeof(standing));     /* (whoever asks for a network now has the say) */
	asked_join_what = *what;
	asked_join = true;
	asked_rejoin = false;
	asked_leave = false;
	rejoin_gen++;
	rejoin_attempts = 0;
	lck_mtx_unlock(&sta_lock);
	if (rejoin_call != NULL) {
		thread_call_cancel(rejoin_call);
	}
	thread_wakeup((event_t)&sta_ops);
	return 0;
}

int
hakios_sta_ask_leave(void)
{
	lck_mtx_lock(&sta_lock);
	if (sta_ops == NULL) {
		lck_mtx_unlock(&sta_lock);
		return ENXIO;
	}
	asked_leave = true;
	asked_join = false;
	asked_rejoin = false;
	rejoin_gen++;
	rejoin_attempts = 0;
	bzero(&asked_join_what, sizeof(asked_join_what));
	bzero(&fallback, sizeof(fallback));
	lck_mtx_unlock(&sta_lock);
	if (rejoin_call != NULL) {
		thread_call_cancel(rejoin_call);
	}
	thread_wakeup((event_t)&sta_ops);
	return 0;
}

bool
hakios_sta_heard(unsigned index, struct hakios_sta_bss *bss)
{
	bool there;

	lck_mtx_lock(&sta_lock);
	if ((there = index < heard_count)) {
		*bss = heard[index];
	}
	lck_mtx_unlock(&sta_lock);
	return there;
}

void
hakios_sta_state(struct hakios_sta_state *state)
{
	const struct hakios_sta_ops *ops;

	bzero(state, sizeof(*state));
	lck_mtx_lock(&sta_lock);
	if ((ops = sta_ops) != NULL) {
		sta_reads++;
	}
	lck_mtx_unlock(&sta_lock);
	if (ops != NULL) {
		ops->state(state);
		lck_mtx_lock(&sta_lock);
		sta_reads--;
		lck_mtx_unlock(&sta_lock);
		thread_wakeup((event_t)&sta_calls);
	}
}

unsigned
hakios_sta_channels(struct hakios_sta_channel *out, unsigned most)
{
	unsigned count;

	lck_mtx_lock(&sta_lock);
	count = channel_count < most ? channel_count : most;
	memcpy(out, channels, count * sizeof(*out));
	lck_mtx_unlock(&sta_lock);
	return count;
}

bool
hakios_sta_address(uint8_t *address)
{
	const struct hakios_sta_ops *ops = sta_ops;

	if (ops != NULL) {
		memcpy(address, ops->address, 6);
	}
	return ops != NULL;
}
