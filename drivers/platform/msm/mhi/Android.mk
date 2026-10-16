LOCAL_PATH := $(call my-dir)

ifeq ($(TARGET_DEVICE),ef67)
EF67_MHI_OUT := $(call intermediates-dir-for,ETC,mhi.ko)/mhi
EF67_MHI_FILES := $(wildcard $(LOCAL_PATH)/*.c $(LOCAL_PATH)/*.h) $(LOCAL_PATH)/Kbuild

include $(CLEAR_VARS)
LOCAL_MODULE := mhi.ko
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_PATH := $(TARGET_OUT)/lib/modules
LOCAL_PREBUILT_MODULE_FILE := $(EF67_MHI_OUT)/mhi.ko
include $(BUILD_PREBUILT)

$(EF67_MHI_OUT)/mhi.ko: TARGET_KERNEL_BINARIES $(EF67_MHI_FILES)
	mkdir -p $(EF67_MHI_OUT)
	cp $(EF67_MHI_FILES) $(EF67_MHI_OUT)/
	$(MAKE) -C $(TARGET_KERNEL_SOURCE) \
		O=$(abspath $(TARGET_OUT_INTERMEDIATES)/KERNEL_OBJ) \
		M=$(abspath $(EF67_MHI_OUT)) ARCH=$(TARGET_ARCH) \
		$(ARM_CROSS_COMPILE) modules
endif
