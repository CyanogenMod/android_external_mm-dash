# This project has been split from caf media as of caf-kk3.10
ifeq ($(TARGET_QCOM_MEDIA_VARIANT),caf-kk3.10)

QCOM_MEDIA_ROOT := $(call my-dir)

include $(QCOM_MEDIA_ROOT)/QCMediaPlayer/Android.mk
include $(QCOM_MEDIA_ROOT)/dashplayer/Android.mk

endif
