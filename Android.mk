ifeq ($(call is-board-platform-in-list,msm8909 msm8916 msm8992 msm8994),true)

QCOM_MEDIA_ROOT := $(call my-dir)

include $(QCOM_MEDIA_ROOT)/QCMediaPlayer/Android.mk
include $(QCOM_MEDIA_ROOT)/dashplayer/Android.mk

endif
