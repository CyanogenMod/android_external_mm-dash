ifeq ($(call is-board-platform-in-list,msm8909 msm8916 msm8974 msm8994),true)

QCOM_MEDIA_ROOT := $(call my-dir)
ifneq ($(TARGET_DISABLE_DASH),true)
include $(QCOM_MEDIA_ROOT)/jni/Android.mk
include $(QCOM_MEDIA_ROOT)/QCMediaPlayer/Android.mk
include $(QCOM_MEDIA_ROOT)/QCMediaPlayer/native/Android.mk
include $(QCOM_MEDIA_ROOT)/dashplayer/Android.mk
endif

endif
