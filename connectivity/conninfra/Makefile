
# Default kernel source directory - adjust as needed
KERNEL_SRC ?= /usr/src/linux-headers-$(shell uname -r)

# Include any possible Makefile.include
include $(wildcard $(KERNEL_SRC)/$(DEVICE_MODULES_REL_DIR)/Makefile.include)

ifneq ($(_FSM_DENPENDENCY_SYMBOLS),)
	EXTRA_SYMBOLS += $(_FSM_DENPENDENCY_SYMBOLS)
	EXTRA_CFLAGS := -D CFG_CONNINFRA_EAP_COCLOCK=1
	CONNINFRA_COCLOCK_SUPPORT := y
else
	EXTRA_CFLAGS := -D CFG_CONNINFRA_EAP_COCLOCK=0
	CONNINFRA_COCLOCK_SUPPORT := n
endif

# Additional CFLAGS needed for kernel compatibility
EXTRA_CFLAGS += -I$(PWD)/include 
EXTRA_CFLAGS += -I$(PWD)/base/include 
EXTRA_CFLAGS += -I$(PWD)/conf/include 
EXTRA_CFLAGS += -I$(PWD)/adaptor/include 
EXTRA_CFLAGS += -I$(PWD)/adaptor/connsyslog 
EXTRA_CFLAGS += -I$(PWD)/adaptor/coredump 
EXTRA_CFLAGS += -I$(PWD)/adaptor/include 
EXTRA_CFLAGS += -I$(PWD)/conn_drv/connv2/src 
EXTRA_CFLAGS += -I$(PWD)/conn_drv/connv2/core/include 
EXTRA_CFLAGS += -I$(PWD)/conn_drv/connv2/drv_init/include 
EXTRA_CFLAGS += -I$(PWD)/conn_drv/connv2/platform/include 
EXTRA_CFLAGS += -I$(PWD)/conn_drv/connv2/debug_utility 
EXTRA_CFLAGS += -I$(PWD)/conn_drv/connv2/debug_utility/include 
EXTRA_CFLAGS += -I$(PWD)/conn_drv/connv2/debug_utility/connsyslog 
EXTRA_CFLAGS += -I$(PWD)/conn_drv/connv2/debug_utility/connsyslog/platform/include 
EXTRA_CFLAGS += -I$(PWD)/conn_drv/connv2/debug_utility/coredump 
EXTRA_CFLAGS += -I$(PWD)/conn_drv/connv2/debug_utility/coredump/platform/include 
EXTRA_CFLAGS += -I$(PWD)/conn_drv/connv2/debug_utility/metlog 
EXTRA_CFLAGS += -I$(PWD)/conn_drv/connv3/src 
EXTRA_CFLAGS += -I$(PWD)/conn_drv/connv3/core/include 
EXTRA_CFLAGS += -I$(PWD)/conn_drv/connv3/platform/include 
EXTRA_CFLAGS += -I$(PWD)/conn_drv/connv3/debug_utility 
EXTRA_CFLAGS += -I$(PWD)/conn_drv/connv3/debug_utility/include 
EXTRA_CFLAGS += -I$(PWD)/test/connv2/include 
EXTRA_CFLAGS += -I$(PWD)/test/connv3/include 
EXTRA_CFLAGS += -I$(PWD)/drivers/misc/mediatek/include 
EXTRA_CFLAGS += -I$(PWD)/drivers/misc/mediatek/include/mt-plat 
EXTRA_CFLAGS += -I$(PWD)/drivers/misc/mediatek/base/power/include/clkbuf_v1 
EXTRA_CFLAGS += -I$(PWD)/drivers/misc/mediatek/clkbuf/src/ 
EXTRA_CFLAGS += -I$(PWD)/drivers/misc/mediatek/connectivity/common 
EXTRA_CFLAGS += -I$(PWD)/drivers/misc/mediatek/connectivity/power_throttling 
EXTRA_CFLAGS += -I$(PWD)/drivers/misc/mediatek/pmic/include/ 
EXTRA_CFLAGS += -I$(PWD)/include/linux/soc/mediatek/ 
EXTRA_CFLAGS += -I$(PWD)/drivers/gpu/drm/mediatek/mediatek_v2 
EXTRA_CFLAGS += -I$(PWD)/drivers/memory/mediatek/
EXTRA_CFLAGS += -Wno-error -Wno-missing-prototypes -Wno-missing-declarations -Wno-unused-variable -Wno-unused-function

modules modules_install clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) $(KBUILD_OPTIONS) EXTRA_CFLAGS="$(EXTRA_CFLAGS)" KBUILD_EXTRA_SYMBOLS="$(EXTRA_SYMBOLS)" CFG_CONNINFRA_COCLOCK_SUPPORT="$(CONNINFRA_COCLOCK_SUPPORT)" $(@)

# Only run the copy commands if build was run from kernel tree (M != PWD)
# This condition is mainly for kernel tree integration, not needed for external build
# The kernel build system handles copying automatically when appropriate
