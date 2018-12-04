# Copyright (C) 2018 The Android Open Source Project
# Copyright (C) 2018 EPAM Systems Inc.
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

# Build kernel inside Android tree
ifeq ($(TARGET_PREBUILT_KERNEL),)

LOCAL_PATH := $(call my-dir)

ABS_TOP := $(abspath $(TOP))

HOSTCC = $(ABS_TOP)/prebuilts/gcc/linux-x86/host/x86_64-linux-glibc2.15-4.8/bin/x86_64-linux-gcc

KERNEL_SRC := $(TARGET_KERNEL_SOURCE)
KERNEL_DEFCONFIG := $(TARGET_KERNEL_CONFIG)
KERNEL_CONFIG := $(KERNEL_OUT)/.config

ifeq ($(OUT_DIR),out)
KERNEL_OUT := $(ABS_TOP)/$(TARGET_OUT_INTERMEDIATES)/KERNEL_OBJ
KERNEL_MODULES_OUT ?= $(TOP)/$(TARGET_OUT_INTERMEDIATES)/KERNEL_MODULES
else
KERNEL_OUT := $(PRODUCT_OUT)/obj/KERNEL_OBJ
KERNEL_MODULES_OUT ?= $(PRODUCT_OUT)/obj/KERNEL_MODULES
endif

KERNEL_TARGET_BINARY := $(KERNEL_OUT)/arch/$(TARGET_ARCH)/boot/Image

ANDROID_TOOLCHAIN_XX=$(ABS_TOP)/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/bin

ARM_CROSS_COMPILE:=CROSS_COMPILE="$(ccache) $(ANDROID_TOOLCHAIN_XX)/aarch64-linux-android-"

ifeq ($(TARGET_KERNEL_EXT_MODULES),)
    TARGET_KERNEL_EXT_MODULES := no-external-modules
endif

$(KERNEL_OUT):
	mkdir -p $(KERNEL_OUT)

$(KERNEL_MODULES_OUT):
	mkdir -p $(KERNEL_MODULES_OUT)

$(KERNEL_CONFIG): $(KERNEL_OUT)
	$(MAKE) -C $(KERNEL_SRC) O=$(KERNEL_OUT) ARCH=$(TARGET_ARCH) $(ARM_CROSS_COMPILE) $(KCFLAGS) HOSTCC=$(HOSTCC) $(KERNEL_DEFCONFIG)

$(KERNEL_TARGET_BINARY): $(KERNEL_OUT) $(KERNEL_CONFIG) $(KERNEL_MODULES_OUT)
	$(MAKE) -C $(KERNEL_SRC) O=$(KERNEL_OUT) ARCH=$(TARGET_ARCH) $(ARM_CROSS_COMPILE) $(KCFLAGS) HOSTCC=$(HOSTCC) Image

TARGET_KERNEL_MODULES: $(KERNEL_TARGET_BINARY)
	$(MAKE) -C $(KERNEL_SRC) O=$(KERNEL_OUT) INSTALL_MOD_PATH=$(KERNEL_MODULES_OUT) ARCH=$(TARGET_ARCH) $(ARM_CROSS_COMPILE) HOSTCC=$(HOSTCC) modules
	$(MAKE) -C $(KERNEL_SRC) O=$(KERNEL_OUT) INSTALL_MOD_PATH=$(KERNEL_MODULES_OUT) ARCH=$(TARGET_ARCH) $(ARM_CROSS_COMPILE) HOSTCC=$(HOSTCC) modules_install

$(TARGET_KERNEL_EXT_MODULES) : TARGET_KERNEL_MODULES

$(PRODUCT_OUT)/kernel: $(TARGET_KERNEL_EXT_MODULES)
	cp -v $(KERNEL_TARGET_BINARY) $(PRODUCT_OUT)/kernel

endif # TARGET_PREBUILT_KERNEL
