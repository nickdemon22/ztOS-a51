# The BCM4773 sensor hub's kernel side: Broadcom's SPI transport and "bbd"
# device nodes from Samsung's exynos9810 kernel (GPL), built as one module.
obj-m += zrn-bbd.o
zrn-bbd-y := bbd.o bbd_rpc_lh.o bcm_gps_spi.o bcm_gps_regs.o bcm_gps_mem.o zrn_bbd_board.o
ccflags-y += -I$(src) -include $(src)/zrn_compat.h -Wno-unused-function -Wno-unused-variable -Wno-missing-prototypes -Wno-declaration-after-statement
