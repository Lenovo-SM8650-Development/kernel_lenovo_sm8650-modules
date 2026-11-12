# SPDX-License-Identifier: GPL-2.0-only
#
# Lenovo TB321FU (kirby): install the CN .298 stock NT36xxx touch module.
#
# The public NT36XXX_SPI source predates the .298 touch firmware. It is still
# built (msm_drm links against its exports), then this step replaces the built
# nvt_36xxx.ko with the stock module before partitioning and depmod, so both
# vendor_boot and vendor_dlkm ship the same stock binary. List this directory
# last in TARGET_KERNEL_EXT_MODULES.

STOCK_MODULE := $(CURDIR)/nvt_36xxx.ko
STOCK_SHA256 := 808fee245e3553166d7b3c46ccee624f1e00ea4e015f8ae04110688079521612

all modules:
	@:

modules_install:
	@actual=$$(sha256sum $(STOCK_MODULE) | cut -d' ' -f1); \
	if [ "$$actual" != "$(STOCK_SHA256)" ]; then \
		echo "nvt-stock: $(STOCK_MODULE) has sha256 $$actual, expected $(STOCK_SHA256)" >&2; exit 1; \
	fi
	@targets=$$(find $(INSTALL_MOD_PATH)/lib/modules -type f -name nvt_36xxx.ko); \
	count=$$(echo "$$targets" | grep -c .); \
	if [ "$$count" != 1 ]; then \
		echo "nvt-stock: expected one built nvt_36xxx.ko, found $$count" >&2; exit 1; \
	fi; \
	install -m 0644 $(STOCK_MODULE) $$targets; \
	echo "nvt-stock: installed stock module at $$targets"

clean:
	@:

.PHONY: all modules modules_install clean
