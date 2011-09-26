LOCAL_PATH:= $(call my-dir)

local_src_files:= \
	app_main.cpp

local_shared_libraries := \
	libcutils \
	libutils \
	libbinder \
	libstlport

include $(CLEAR_VARS)
include external/stlport/libstlport.mk
LOCAL_MODULE:= oat_process
LOCAL_MODULE_TAGS:= optional
LOCAL_SRC_FILES:= $(local_src_files)
LOCAL_SHARED_LIBRARIES := liboat_runtime libart $(local_shared_libraries)
LOCAL_C_INCLUDES += $(ART_C_INCLUDES)
LOCAL_CFLAGS := $(ART_TARGET_CFLAGS)
include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)
include external/stlport/libstlport.mk
LOCAL_MODULE_TAGS:= optional
LOCAL_MODULE:= oat_processd
LOCAL_SRC_FILES:= $(local_src_files)
LOCAL_SHARED_LIBRARIES := liboat_runtimed libartd $(local_shared_libraries)
LOCAL_C_INCLUDES += $(ART_C_INCLUDES)
LOCAL_CFLAGS := $(ART_TARGET_CFLAGS)
include $(BUILD_EXECUTABLE)
