ifeq ($(strip $(BOARD_USES_QCOM_HARDWARE)), true)
ifneq ($(call is-board-platform-in-list,msm7x30 msm8660 msm8960),true)

QCOM_MEDIA_ROOT := $(call my-dir)
ifneq ($(TARGET_DISABLE_DASH),true)
include $(QCOM_MEDIA_ROOT)/jni/Android.mk
include $(QCOM_MEDIA_ROOT)/QCMediaPlayer/Android.mk
include $(QCOM_MEDIA_ROOT)/QCMediaPlayer/native/Android.mk
include $(QCOM_MEDIA_ROOT)/dashplayer/Android.mk
endif

endif # msm7x30 msm8660 msm8960
endif # BOARD_USES_QCOM_HARDWARE
