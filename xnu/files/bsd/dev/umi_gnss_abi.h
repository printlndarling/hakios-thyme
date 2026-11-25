/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef UMI_GNSS_ABI_H
#define UMI_GNSS_ABI_H
#include <stdint.h>
#define UMI_GNSS_ABI 1u
#define UMI_GNSS_POSITION 1u
#define UMI_GNSS_ALTITUDE 2u
#define UMI_GNSS_UTC 4u
#define UMI_GNSS_USED 8u
#define UMI_GNSS_VIEW 16u
/* kern.hakios.gnss_fix: root-only, binary, latest fix only. Never dump this structure.
 * IEEE-754 bit patterns travel as integers: no floating point in XNU.
 * UTC is milliseconds since Unix epoch; age_ms uses kernel monotonic receipt time.
 * Unknown satellite counts are distinguished by flags. No position when disabled.
 */
struct umi_gnss_fix {
    uint32_t version, sequence, flags, age_ms;
    uint64_t latitude_bits, longitude_bits, utc_ms;
    uint32_t accuracy_bits, altitude_bits, vertical_accuracy_bits;
    uint32_t satellites_view, satellites_used, session_status;
};
#endif
