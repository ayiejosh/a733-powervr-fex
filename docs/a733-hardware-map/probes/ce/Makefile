# Out-of-tree CE (crypto engine) driver for A733 / sun60iw2 -> v5 register layer.
# Source: github.com/radxa/allwinner-bsp @ cubie-aiot-v1.4.8 : drivers/ce/
# Uses AW_CE_IOCTL (userspace-reachable) only. AW_HWRNG_DRIVER is EXCLUDED because this
# kernel has no CONFIG_HW_RANDOM, so devm_hwrng_register is unresolved. The AW_CE_SOCKET
# (AF_ALG) and SECURE_KEY (needs smc_process) paths are excluded.
# ponytail: ioctl + hwrng only; add the socket path if AF_ALG is ever wanted.
obj-m += sunxi-ce-ioctl.o
sunxi-ce-ioctl-y := \
	ce/sunxi_ce_cdev.o \
	ce/v5/sunxi_ce_reg.o \
	ce/v5/sunxi_ce_cdev_comm.o
ccflags-y += -I$(src)/ce -I$(src)/ce/v5 \
	-DCONFIG_AW_CE_IOCTL=1 -DCONFIG_ARCH_SUN60IW2=1 -DSS_SUPPORT_CE_V5=1

# The SOCKET (AF_ALG) path registers the engine with the KERNEL crypto API instead of
# exposing a per-call ioctl, which is the interface /proc/crypto and the kernel's
# skcipher/hash users go through. Built alongside the ioctl path.
obj-m += sunxi-ce.o
sunxi-ce-y := ce/sunxi_ce.o ce/sunxi_ce_proc_comm.o ce/v5/sunxi_ce_reg.o ce/v5/sunxi_ce_proc.o
ccflags-sunxi-ce.o := -DCONFIG_AW_CE_SOCKET=1 -DCONFIG_ARCH_SUN60IW2=1

KDIR ?= /usr/src/linux-headers-$(shell uname -r)
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
