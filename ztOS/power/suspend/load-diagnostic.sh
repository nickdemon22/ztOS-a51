#!/bin/sh
# Register without platform powerdown writes or firmware entry.
# The guarded backend holds ACPM MIF sequencing disabled while registered.
set -eu
KO=${1:-$(dirname -- "$0")/zrn-system-sleep.ko}
S=$(awk '$3 == "cpu_suspend" { print $1 }' /proc/kallsyms)
R=$(awk '$3 == "cpu_resume" { print $1 }' /proc/kallsyms)
[ -n "$S" ] && [ "$S" != 0000000000000000 ]
[ -n "$R" ] && [ "$R" != 0000000000000000 ]
insmod "$KO" enable_deep=1 dry_run=1 suspend_enter=0x$S resume=0x$R
# Registration may choose deep as the kernel default; retain s2idle until
# an explicit staged platform test selects deep for its own duration.
echo s2idle > /sys/power/mem_sleep
cat /sys/power/mem_sleep
