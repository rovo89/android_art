# Copyright 2006 The Android Open Source Project

LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	Main.cpp \
	Net.cpp

LOCAL_C_INCLUDES += \
	art/runtime

LOCAL_ADDITIONAL_DEPENDENCIES += $(LOCAL_PATH)/Android.mk

LOCAL_MODULE := jdwpspy

include $(BUILD_HOST_EXECUTABLE)
