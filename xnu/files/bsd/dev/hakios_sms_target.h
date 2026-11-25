/* Out-of-tree, by hand, never committed (docs/cellular.md): the number the
 * phone itself reports for this SIM -- Android's telephony on the same
 * device answered +86192991300141 through iphonesubinfo 16 and 17 on
 * 2026-11-21, with the SIM in the phone.  It is the self-send target for
 * the HakiOS SMS test because the card's EF_MSISDN is blank and the
 * modem's DMS Get MSISDN answers NOT_PROVISIONED (QMI error 16).
 * Delete this file (or the tree) to build without a fallback number. */
#define HAKIOS_SMS_TARGET "+86192991300141"
