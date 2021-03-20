# Copyright (C) 2018 The Android Open Source Project
# Copyright (C) 2021 EPAM Systems Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# Based on https://android.googlesource.com/kernel/msm/

#$(call inherit-product, device/xen/xenvm/build/common_build.mk)

# Build kernel inside Android tree
ifeq ($(TARGET_PREBUILT_KERNEL),)

LOCAL_PATH := $(call my-dir)

ABS_TOP := $(abspath $(TOP))

ifeq ($(OUT_DIR),out)
KERNEL_OUT := $(ABS_TOP)/$(TARGET_OUT_INTERMEDIATES)/KERNEL_OBJ
KERNEL_OUT_ABS              := $(abspath $(KERNEL_OUT))
KERNEL_MODULES_OUT ?= $(TOP)/$(TARGET_OUT_INTERMEDIATES)/KERNEL_MODULES
else
KERNEL_OUT := $(PRODUCT_OUT)/obj/KERNEL_OBJ
KERNEL_OUT_ABS              := $(abspath $(KERNEL_OUT))
KERNEL_MODULES_OUT ?= $(PRODUCT_OUT)/obj/KERNEL_MODULES
endif

KERNEL_MODULES              := $(KERNEL_OUT)/modules.order
KERNEL_MODULES_OUT          := $(PRODUCT_OUT)/obj/KERNEL_MODULES
KERNEL_MODULES_OUT_ABS      := $(abspath $(KERNEL_MODULES_OUT))

KERNEL_SRC := $(abspath $(TARGET_KERNEL_SOURCE))
KERNEL_DEFCONFIG := $(TARGET_KERNEL_CONFIG)
KERNEL_CONFIG := $(KERNEL_OUT)/.config
KERNEL_TARGET_BINARY := $(KERNEL_OUT)/arch/$(TARGET_ARCH)/boot/Image


KERNEL_CFLAGS := HOSTCC=$(ANDROID_CLANG_TOOLCHAIN) HOSTCFLAGS="-fuse-ld=lld" HOSTLDFLAGS=-fuse-ld=lld ARCH=$(TARGET_ARCH)
KERNEL_CFLAGS += CC=$(ANDROID_CLANG_TOOLCHAIN) CLANG_TRIPLE=$(BSP_GCC_CROSS_COMPILE) CROSS_COMPILE=$(BSP_GCC_CROSS_COMPILE)

#    TARGET_KERNEL_EXT_MODULES := no-external-modules
ifneq ($(TARGET_KERNEL_EXT_MODULES),)
$(TARGET_KERNEL_EXT_MODULES): $(KERNEL_TARGET_BINARY)
        $(ANDROID_MAKE) -C $(KERNEL_SRC) O=$(KERNEL_OUT) INSTALL_MOD_PATH=$(KERNEL_MODULES_OUT) $(KERNEL_CFLAGS) modules
        $(ANDROID_MAKE) -C $(KERNEL_SRC) O=$(KERNEL_OUT) INSTALL_MOD_PATH=$(KERNEL_MODULES_OUT) $(KERNEL_CFLAGS) modules_install
endif

$(KERNEL_EXT_MODULES): $(KERNEL_MODULES)
$(BOARD_VENDOR_KERNEL_MODULES): $(KERNEL_EXT_MODULES)

$(KERNEL_OUT):
	mkdir -p $(KERNEL_OUT)

$(KERNEL_MODULES_OUT):
	mkdir -p $(KERNEL_MODULES_OUT)

$(KERNEL_CONFIG): $(KERNEL_OUT)
	$(ANDROID_MAKE) -C $(KERNEL_SRC) O=$(KERNEL_OUT) $(KERNEL_CFLAGS) $(KERNEL_DEFCONFIG)

$(KERNEL_TARGET_BINARY): $(KERNEL_OUT) $(KERNEL_CONFIG) $(KERNEL_MODULES_OUT)
	$(ANDROID_MAKE) CONFIG_DEBUG_SECTION_MISMATCH=y -C $(KERNEL_SRC) O=$(KERNEL_OUT) $(KERNEL_CFLAGS) Image -j `$(NPROC)`

$(KERNEL_MODULES): $(KERNEL_TARGET_BINARY) $(KERNEL_MODULES_OUT)
	@rm -rf $(KERNEL_MODULES_OUT_ABS)/lib/modules
	$(ANDROID_MAKE) -C $(TARGET_KERNEL_SOURCE) O=$(KERNEL_OUT_ABS) INSTALL_MOD_PATH=$(KERNEL_MODULES_OUT_ABS) $(KERNEL_CFLAGS) modules
	$(ANDROID_MAKE) -C $(TARGET_KERNEL_SOURCE) O=$(KERNEL_OUT_ABS) INSTALL_MOD_PATH=$(KERNEL_MODULES_OUT_ABS) $(KERNEL_CFLAGS) modules_install
	find $(KERNEL_MODULES_OUT_ABS) -mindepth 2 -type f -name '*.ko' | grep "$(shell head -1 $(KERNEL_OUT_ABS)/include/config/kernel.release)" | $(XARGS) -I{} mv {} $(KERNEL_MODULES_OUT_ABS)/

$(PRODUCT_OUT)/kernel: $(KERNEL_TARGET_BINARY) $(TARGET_KERNEL_EXT_MODULES)
	cp -v $(KERNEL_TARGET_BINARY) $(PRODUCT_OUT)/kernel

endif # TARGET_PREBUILT_KERNEL
