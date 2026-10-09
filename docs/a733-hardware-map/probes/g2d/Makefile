# Out-of-tree build of the Allwinner G2D driver (RCQ variant) for 6.6.98-5-aw2511.
# Source: github.com/radxa/allwinner-bsp @ cubie-aiot-v1.4.8 : drivers/g2d/
#
# The BSP Makefile builds this inside the kernel tree; here it is built as a module
# against the shipped headers. RCQ is the modern register-command-queue driver; the
# legacy variant is excluded (its g2d_bsp_v2.h is not shipped).
#
# ponytail: only the RCQ path. Add g2d_legacy/ when a reason to prefer it appears.

obj-m += g2d_sunxi.o

g2d_sunxi-y := \
	g2d/g2d_rcq/g2d.o \
	g2d/g2d_rcq/g2d_top.o \
	g2d/g2d_rcq/g2d_debug.o \
	g2d/g2d_rcq/g2d_mixer.o \
	g2d/g2d_rcq/g2d_ovl_v.o \
	g2d/g2d_rcq/g2d_ovl_u.o \
	g2d/g2d_rcq/g2d_rcq.o \
	g2d/g2d_rcq/g2d_scal.o \
	g2d/g2d_rcq/g2d_wb.o \
	g2d/g2d_rcq/g2d_bld.o \
	g2d/g2d_rcq/g2d_rotate.o \
	g2d/syncfence.o

# tristate=m means CONFIG_AW_G2D_MODULE, and the driver's sub-options are bools.
ccflags-y += -I$(src)/g2d -I$(src)/g2d/g2d_rcq \
	-DCONFIG_AW_G2D -DCONFIG_AW_G2D_MODULE \
	-DCONFIG_G2D_RCQ=1 -DCONFIG_G2D_MIXER=1 -DCONFIG_G2D_ROTATE=1 \
	-DCONFIG_G2D_SYNCFENCE=1 -DDYNAMIC_DEBUG_MODULE

KDIR ?= /usr/src/linux-headers-$(shell uname -r)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
