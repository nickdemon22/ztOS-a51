obj-m += zrn-system-sleep.o
zrn-system-sleep-y := platform.o sequence.o
obj-m += zrn-sleep-table-check.o
zrn-sleep-table-check-y := table-check.o sequence.o

# Separate temporary backend for iterating without replacing pinned ops.
obj-m += zrn-sleep-init.o
zrn-sleep-init-y := platform.o sequence.o
