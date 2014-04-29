QCOM_MEDIA_ROOT := $(call my-dir)

ifneq ($(filter msm8974 msm8960 msm8226 apq8084 mpq8092 msm8610 msm_bronze msm8916_32_k64 msm8916_32 msm8916_32_512,$(TARGET_PRODUCT)),)
include $(QCOM_MEDIA_ROOT)/QCMediaPlayer/Android.mk
include $(QCOM_MEDIA_ROOT)/dashplayer/Android.mk
endif

