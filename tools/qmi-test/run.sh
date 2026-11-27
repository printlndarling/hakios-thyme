#!/bin/bash
# Build and run the tests of the QMI helpers (drivers/modem/hakios.qmi.c of the kernel tree) on this
# machine, under the address and undefined-behaviour sanitizers.
#
# With ath11k's sources at hand (tools/mk-ath11k.sh fetches them; ATH11K=<directory> to say where
# else they are) its own message tables are tested too; without, only what needs none of them.
# Set QMI_TARGET=thyme to test the device override as well.
# Nothing of ath11k is kept: its tables are taken out of qmi.c into the build directory each time.
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
K=$ROOT/xnu/files
target=${QMI_TARGET:-common}
case "$target" in
common) QMI=$ROOT/drivers/modem/hakios.qmi.c; target_flags= ;;
thyme) QMI=$ROOT/devices/thyme/drivers/wifi/hakios.thyme_qmi.c; target_flags=-DTEST_THYME ;;
*) echo "qmi-test: expected QMI_TARGET=common or thyme" >&2; exit 1 ;;
esac
ATH=${ATH11K:-$ROOT/xnu-deps/build/ath11k/src/ath11k}
B=$ROOT/xnu-deps/build/qmi-test/$target
rm -rf "$B"
mkdir -p "$B/inc/sys"
ln -s "$K/bsd/sys/hakios.qmi.h" "$K/bsd/sys/hakios.qrtr.h" "$B/inc/sys/"
FLAGS="$target_flags -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -pthread"
if [ -f "$ATH/qmi.c" ]; then
	"$HERE/extract.py" "$ATH" > "$B/tables.inc"
	${CC:-cc} $FLAGS -I"$B/inc" -I"$B" -I"$HERE/shim" -I"$ATH" -o "$B/test" "$HERE/test_qmi.c" "$QMI"
	echo "qmi-test: with ath11k's tables from $ATH ($(cat "$ATH/../.release" 2>/dev/null | cut -d' ' -f1))"
else
	echo "qmi-test: no ath11k sources in $ATH: only the tests that need none"
	${CC:-cc} $FLAGS -DNO_ATH11K -I"$B/inc" -o "$B/test" "$HERE/test_qmi.c" "$QMI"
fi
"$B/test"
