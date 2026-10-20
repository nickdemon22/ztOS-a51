#!/bin/sh
# The generic core test can execute WFE after syscore suspend. Never arm
# hardware there: preparation now runs within cpu_suspend's saved context.
echo "Disabled: use prepare_only=1 with the platform enter path instead." >&2
exit 1
