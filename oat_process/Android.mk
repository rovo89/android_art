LOCAL_PATH:= $(call my-dir)

local_src_files:= \
	app_main.cpp

local_shared_libraries := \
	libcutils \
	libutils \
	libbinder

include $(CLEAR_VARS)
LOCAL_MODULE:= oat_process
LOCAL_MODULE_TAGS:= optional
LOCAL_SRC_FILES:= $(local_src_files)
LOCAL_SHARED_LIBRARIES := liboat_runtimed $(local_shared_libraries)
include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)
LOCAL_MODULE_TAGS:= optional
LOCAL_MODULE:= oat_processd
LOCAL_SRC_FILES:= $(local_src_files)
LOCAL_SHARED_LIBRARIES := liboat_runtimed $(local_shared_libraries)
include $(BUILD_EXECUTABLE)
