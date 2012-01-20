#
# Copyright (C) 2011 The Android Open Source Project
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
#

LOCAL_PATH := $(call my-dir)

# These can be overridden via the environment or by editing to
# enable/disable certain build configuration.
ART_BUILD_TARGET_NDEBUG ?= true
ART_BUILD_TARGET_DEBUG ?= true
ART_BUILD_HOST_NDEBUG ?= true
ART_BUILD_HOST_DEBUG ?= true

build_path := $(LOCAL_PATH)/build
include $(build_path)/Android.common.mk

include $(build_path)/Android.libart.mk
include $(build_path)/Android.executable.mk

include $(build_path)/Android.oat.mk

include $(build_path)/Android.libarttest.mk
include $(build_path)/Android.gtest.mk
include $(build_path)/Android.oattest.mk

# "m build-art" for quick minimal build
.PHONY: build-art
build-art: \
    $(ART_TARGET_EXECUTABLES) \
    $(ART_TARGET_TEST_EXECUTABLES) \
    $(ART_HOST_EXECUTABLES) \
    $(ART_HOST_TEST_EXECUTABLES)

# "mm test-art" to build and run all tests on host and device
.PHONY: test-art
test-art: test-art-host test-art-target
	@echo test-art PASSED

.PHONY: test-art-gtest
test-art-gtest: test-art-host test-art-target-gtest
	@echo test-art-gtest PASSED

define run-host-tests-with
  $(foreach file,$(sort $(ART_HOST_TEST_EXECUTABLES)),$(1) $(file) &&) true
endef

ART_HOST_DEPENDENCIES   := $(ART_HOST_EXECUTABLES)   $(HOST_OUT_JAVA_LIBRARIES)/core-hostdex.jar $(HOST_OUT_SHARED_LIBRARIES)/libjavacore.so
ART_TARGET_DEPENDENCIES := $(ART_TARGET_EXECUTABLES) $(TARGET_OUT_JAVA_LIBRARIES)/core.jar     $(TARGET_OUT_SHARED_LIBRARIES)/libjavacore.so

ART_HOST_TEST_DEPENDENCIES   := $(ART_HOST_DEPENDENCIES)   $(ART_TEST_OAT_FILES)
ART_TARGET_TEST_DEPENDENCIES := $(ART_TARGET_DEPENDENCIES) $(ART_TEST_OAT_FILES)

########################################################################
# host test targets

# "mm test-art-host" to build and run all host tests
.PHONY: test-art-host
test-art-host: $(ART_HOST_TEST_DEPENDENCIES) $(ART_HOST_TEST_TARGETS)
	@echo test-art-host PASSED

# "mm valgrind-art-host" to build and run all host tests under valgrind.
.PHONY: valgrind-art-host
valgrind-art-host: $(ART_HOST_TEST_DEPENDENCIES)
	$(call run-host-tests-with,valgrind --leak-check=full)
	@echo valgrind-art-host PASSED

# "mm tsan-art-host" to build and run all host tests under tsan.
.PHONY: tsan-art-host
tsan-art-host: $(ART_HOST_TEST_DEPENDENCIES)
	$(call run-host-tests-with,"tsan")
	@echo tsan-art-host PASSED

########################################################################
# target test targets

# "mm test-art-target" to build and run all target tests
.PHONY: test-art-target
test-art-target: test-art-target-gtest test-art-target-oat test-art-target-run-test
	@echo test-art-target PASSED

.PHONY: test-art-target-sync
test-art-target-sync: $(ART_TARGET_TEST_DEPENDENCIES)
	adb remount
	adb sync
	adb shell mkdir -p $(ART_TEST_DIR)

.PHONY: test-art-target-gtest
test-art-target-gtest: $(ART_TARGET_TEST_TARGETS)

.PHONY: test-art-target-oat
test-art-target-oat: $(ART_TEST_OAT_TARGETS)
	@echo test-art-target-oat PASSED

.PHONY: test-art-target-run-test
test-art-target-run-test: test-art-target-run-test-002
	@echo test-art-target-run-test PASSED

.PHONY: test-art-target-run-test-002
test-art-target-run-test-002: test-art-target-sync
	art/test/run-test 002
	@echo test-art-target-run-test-002 PASSED

########################################################################
# oat test targets

# $(1): jar or apk name
define art-cache-oat
  $(ART_CACHE_OUT)/$(subst /,@,$(1).oat)
endef

ART_CACHE_OATS :=
# $(1): name
define build-art-cache-oat
  $(call build-art-oat,$(PRODUCT_OUT)/$(1),$(call art-cache-oat,$(1)),$(TARGET_BOOT_IMG))
  ART_CACHE_OATS += $(call art-cache-oat,$(1))
endef


########################################################################
# oat-target-sync

ifeq ($(TARGET_PRODUCT),mysid)

$(eval $(call build-art-cache-oat,system/app/ApplicationsProvider.apk))
$(eval $(call build-art-cache-oat,system/app/BackupRestoreConfirmation.apk))
$(eval $(call build-art-cache-oat,system/app/BIP.apk))
$(eval $(call build-art-cache-oat,system/app/Bluetooth.apk))
$(eval $(call build-art-cache-oat,system/app/BooksTablet.apk))
$(eval $(call build-art-cache-oat,system/app/BrowserGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/Calculator.apk))
$(eval $(call build-art-cache-oat,system/app/CalendarGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/CalendarProvider.apk))
$(eval $(call build-art-cache-oat,system/app/CameraGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/CertInstaller.apk))
$(eval $(call build-art-cache-oat,system/app/ChromeBookmarksSyncAdapter.apk))
$(eval $(call build-art-cache-oat,system/app/Contacts.apk))
$(eval $(call build-art-cache-oat,system/app/ContactsProvider.apk))
$(eval $(call build-art-cache-oat,system/app/DefaultContainerService.apk))
$(eval $(call build-art-cache-oat,system/app/DeskClockGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/Development.apk))
$(eval $(call build-art-cache-oat,system/app/DownloadProvider.apk))
$(eval $(call build-art-cache-oat,system/app/DownloadProviderUi.apk))
$(eval $(call build-art-cache-oat,system/app/DrmProvider.apk))
$(eval $(call build-art-cache-oat,system/app/EmailGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/ExchangeGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/GalleryGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/GenieWidget.apk))
$(eval $(call build-art-cache-oat,system/app/Gmail.apk))
$(eval $(call build-art-cache-oat,system/app/GoogleBackupTransport.apk))
$(eval $(call build-art-cache-oat,system/app/GoogleContactsSyncAdapter.apk))
$(eval $(call build-art-cache-oat,system/app/GoogleEarth.apk))
$(eval $(call build-art-cache-oat,system/app/GoogleFeedback.apk))
$(eval $(call build-art-cache-oat,system/app/GoogleLoginService.apk))
$(eval $(call build-art-cache-oat,system/app/GooglePackageVerifier.apk))
$(eval $(call build-art-cache-oat,system/app/GooglePackageVerifierUpdater.apk))
$(eval $(call build-art-cache-oat,system/app/GooglePartnerSetup.apk))
$(eval $(call build-art-cache-oat,system/app/GoogleQuickSearchBox.apk))
$(eval $(call build-art-cache-oat,system/app/GoogleServicesFramework.apk))
$(eval $(call build-art-cache-oat,system/app/GoogleTTS.apk))
$(eval $(call build-art-cache-oat,system/app/HTMLViewer.apk))
$(eval $(call build-art-cache-oat,system/app/IMSFramework.apk))
$(eval $(call build-art-cache-oat,system/app/HoloSpiralWallpaper.apk))
$(eval $(call build-art-cache-oat,system/app/KeyChain.apk))
$(eval $(call build-art-cache-oat,system/app/LatinImeDictionaryPack.apk))
$(eval $(call build-art-cache-oat,system/app/LatinImeGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/Launcher2.apk))
$(eval $(call build-art-cache-oat,system/app/LiveWallpapers.apk))
$(eval $(call build-art-cache-oat,system/app/LiveWallpapersPicker.apk))
$(eval $(call build-art-cache-oat,system/app/Maps.apk))
$(eval $(call build-art-cache-oat,system/app/MarketUpdater.apk))
$(eval $(call build-art-cache-oat,system/app/MediaProvider.apk))
$(eval $(call build-art-cache-oat,system/app/MediaUploader.apk))
$(eval $(call build-art-cache-oat,system/app/Microbes.apk))
$(eval $(call build-art-cache-oat,system/app/Mms.apk))
$(eval $(call build-art-cache-oat,system/app/Music2.apk))
$(eval $(call build-art-cache-oat,system/app/MusicFX.apk))
$(eval $(call build-art-cache-oat,system/app/MyVerizon.apk))
$(eval $(call build-art-cache-oat,system/app/NetSpeed.apk))
$(eval $(call build-art-cache-oat,system/app/NetworkLocation.apk))
$(eval $(call build-art-cache-oat,system/app/Nfc.apk))
$(eval $(call build-art-cache-oat,system/app/OneTimeInitializer.apk))
$(eval $(call build-art-cache-oat,system/app/PackageInstaller.apk))
$(eval $(call build-art-cache-oat,system/app/Phone.apk))
$(eval $(call build-art-cache-oat,system/app/Phonesky.apk))
$(eval $(call build-art-cache-oat,system/app/PlusOne.apk))
$(eval $(call build-art-cache-oat,system/app/RTN.apk))
$(eval $(call build-art-cache-oat,system/app/SDM.apk))
$(eval $(call build-art-cache-oat,system/app/Settings.apk))
$(eval $(call build-art-cache-oat,system/app/SettingsProvider.apk))
$(eval $(call build-art-cache-oat,system/app/SetupWizard.apk))
$(eval $(call build-art-cache-oat,system/app/SharedStorageBackup.apk))
$(eval $(call build-art-cache-oat,system/app/SoundRecorder.apk))
$(eval $(call build-art-cache-oat,system/app/SpeechRecorder.apk))
$(eval $(call build-art-cache-oat,system/app/SPG.apk))
$(eval $(call build-art-cache-oat,system/app/StingrayProgramMenu.apk))
$(eval $(call build-art-cache-oat,system/app/StingrayProgramMenuSystem.apk))
$(eval $(call build-art-cache-oat,system/app/Stk.apk))
$(eval $(call build-art-cache-oat,system/app/Street.apk))
$(eval $(call build-art-cache-oat,system/app/SyncMLSvc.apk))
$(eval $(call build-art-cache-oat,system/app/SystemUI.apk))
$(eval $(call build-art-cache-oat,system/app/TagGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/Talk.apk))
$(eval $(call build-art-cache-oat,system/app/TelephonyProvider.apk))
$(eval $(call build-art-cache-oat,system/app/Thinkfree.apk))
$(eval $(call build-art-cache-oat,system/app/UserDictionaryProvider.apk))
$(eval $(call build-art-cache-oat,system/app/VerizonSSO.apk))
$(eval $(call build-art-cache-oat,system/app/VideoEditorGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/Videos.apk))
$(eval $(call build-art-cache-oat,system/app/VisualizationWallpapers.apk))
$(eval $(call build-art-cache-oat,system/app/VoiceDialer.apk))
$(eval $(call build-art-cache-oat,system/app/VoiceSearch.apk))
$(eval $(call build-art-cache-oat,system/app/VpnDialogs.apk))
$(eval $(call build-art-cache-oat,system/app/VZWAPNLib.apk))
$(eval $(call build-art-cache-oat,system/app/VZWAPNService.apk))
$(eval $(call build-art-cache-oat,system/app/VZWBackupAssistant.apk))
$(eval $(call build-art-cache-oat,system/app/YouTube.apk))
$(eval $(call build-art-cache-oat,system/app/talkback.apk))
$(eval $(call build-art-cache-oat,system/framework/am.jar))
$(eval $(call build-art-cache-oat,system/framework/android.test.runner.jar))
$(eval $(call build-art-cache-oat,system/framework/bmgr.jar))
$(eval $(call build-art-cache-oat,system/framework/bu.jar))
$(eval $(call build-art-cache-oat,system/framework/com.android.future.usb.accessory.jar))
$(eval $(call build-art-cache-oat,system/framework/com.android.location.provider.jar))
$(eval $(call build-art-cache-oat,system/framework/com.android.nfc_extras.jar))
$(eval $(call build-art-cache-oat,system/framework/com.google.android.maps.jar))
$(eval $(call build-art-cache-oat,system/framework/com.google.android.media.effects.jar))
$(eval $(call build-art-cache-oat,system/framework/ext.jar))
$(eval $(call build-art-cache-oat,system/framework/ime.jar))
$(eval $(call build-art-cache-oat,system/framework/input.jar))
$(eval $(call build-art-cache-oat,system/framework/javax.obex.jar))
$(eval $(call build-art-cache-oat,system/framework/monkey.jar))
$(eval $(call build-art-cache-oat,system/framework/pm.jar))
$(eval $(call build-art-cache-oat,system/framework/send_bug.jar))
$(eval $(call build-art-cache-oat,system/framework/svc.jar))

else

ifeq ($(TARGET_PRODUCT),$(filter $(TARGET_PRODUCT),soju sojus))

$(eval $(call build-art-cache-oat,system/app/ApplicationsProvider.apk))
$(eval $(call build-art-cache-oat,system/app/BackupRestoreConfirmation.apk))
$(eval $(call build-art-cache-oat,system/app/Bluetooth.apk))
$(eval $(call build-art-cache-oat,system/app/BooksTablet.apk))
$(eval $(call build-art-cache-oat,system/app/BrowserGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/Calculator.apk))
$(eval $(call build-art-cache-oat,system/app/CalendarGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/CalendarProvider.apk))
$(eval $(call build-art-cache-oat,system/app/CameraGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/CarHomeGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/CertInstaller.apk))
$(eval $(call build-art-cache-oat,system/app/ChromeBookmarksSyncAdapter.apk))
$(eval $(call build-art-cache-oat,system/app/Contacts.apk))
$(eval $(call build-art-cache-oat,system/app/ContactsProvider.apk))
$(eval $(call build-art-cache-oat,system/app/DefaultContainerService.apk))
$(eval $(call build-art-cache-oat,system/app/DeskClockGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/Development.apk))
$(eval $(call build-art-cache-oat,system/app/DownloadProvider.apk))
$(eval $(call build-art-cache-oat,system/app/DownloadProviderUi.apk))
$(eval $(call build-art-cache-oat,system/app/DrmProvider.apk))
$(eval $(call build-art-cache-oat,system/app/EmailGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/ExchangeGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/GalleryGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/GenieWidget.apk))
$(eval $(call build-art-cache-oat,system/app/Gmail.apk))
$(eval $(call build-art-cache-oat,system/app/GoogleBackupTransport.apk))
$(eval $(call build-art-cache-oat,system/app/GoogleContactsSyncAdapter.apk))
$(eval $(call build-art-cache-oat,system/app/GoogleEarth.apk))
$(eval $(call build-art-cache-oat,system/app/GoogleFeedback.apk))
$(eval $(call build-art-cache-oat,system/app/GoogleLoginService.apk))
$(eval $(call build-art-cache-oat,system/app/GooglePackageVerifier.apk))
$(eval $(call build-art-cache-oat,system/app/GooglePackageVerifierUpdater.apk))
$(eval $(call build-art-cache-oat,system/app/GooglePartnerSetup.apk))
$(eval $(call build-art-cache-oat,system/app/GoogleQuickSearchBox.apk))
$(eval $(call build-art-cache-oat,system/app/GoogleServicesFramework.apk))
$(eval $(call build-art-cache-oat,system/app/GoogleTTS.apk))
$(eval $(call build-art-cache-oat,system/app/HTMLViewer.apk))
$(eval $(call build-art-cache-oat,system/app/HoloSpiralWallpaper.apk))
$(eval $(call build-art-cache-oat,system/app/KeyChain.apk))
$(eval $(call build-art-cache-oat,system/app/LatinImeDictionaryPack.apk))
$(eval $(call build-art-cache-oat,system/app/LatinImeGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/Launcher2.apk))
$(eval $(call build-art-cache-oat,system/app/LiveWallpapers.apk))
$(eval $(call build-art-cache-oat,system/app/LiveWallpapersPicker.apk))
$(eval $(call build-art-cache-oat,system/app/Maps.apk))
$(eval $(call build-art-cache-oat,system/app/MarketUpdater.apk))
$(eval $(call build-art-cache-oat,system/app/MediaProvider.apk))
$(eval $(call build-art-cache-oat,system/app/MediaUploader.apk))
$(eval $(call build-art-cache-oat,system/app/Microbes.apk))
$(eval $(call build-art-cache-oat,system/app/Mms.apk))
$(eval $(call build-art-cache-oat,system/app/Music2.apk))
$(eval $(call build-art-cache-oat,system/app/MusicFX.apk))
$(eval $(call build-art-cache-oat,system/app/NetSpeed.apk))
$(eval $(call build-art-cache-oat,system/app/NetworkLocation.apk))
$(eval $(call build-art-cache-oat,system/app/Nfc.apk))
$(eval $(call build-art-cache-oat,system/app/OneTimeInitializer.apk))
$(eval $(call build-art-cache-oat,system/app/PackageInstaller.apk))
$(eval $(call build-art-cache-oat,system/app/Phone.apk))
$(eval $(call build-art-cache-oat,system/app/Phonesky.apk))
$(eval $(call build-art-cache-oat,system/app/PlusOne.apk))
$(eval $(call build-art-cache-oat,system/app/Settings.apk))
$(eval $(call build-art-cache-oat,system/app/SettingsProvider.apk))
$(eval $(call build-art-cache-oat,system/app/SetupWizard.apk))
$(eval $(call build-art-cache-oat,system/app/SharedStorageBackup.apk))
$(eval $(call build-art-cache-oat,system/app/SoundRecorder.apk))
$(eval $(call build-art-cache-oat,system/app/SpeechRecorder.apk))
$(eval $(call build-art-cache-oat,system/app/StingrayProgramMenu.apk))
$(eval $(call build-art-cache-oat,system/app/StingrayProgramMenuSystem.apk))
$(eval $(call build-art-cache-oat,system/app/Street.apk))
$(eval $(call build-art-cache-oat,system/app/SystemUI.apk))
$(eval $(call build-art-cache-oat,system/app/TagGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/Talk.apk))
$(eval $(call build-art-cache-oat,system/app/TelephonyProvider.apk))
$(eval $(call build-art-cache-oat,system/app/Thinkfree.apk))
$(eval $(call build-art-cache-oat,system/app/UserDictionaryProvider.apk))
$(eval $(call build-art-cache-oat,system/app/VideoEditorGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/Videos.apk))
$(eval $(call build-art-cache-oat,system/app/VisualizationWallpapers.apk))
$(eval $(call build-art-cache-oat,system/app/VoiceDialer.apk))
$(eval $(call build-art-cache-oat,system/app/VoiceSearch.apk))
$(eval $(call build-art-cache-oat,system/app/VpnDialogs.apk))
$(eval $(call build-art-cache-oat,system/app/YouTube.apk))
$(eval $(call build-art-cache-oat,system/app/googlevoice.apk))
$(eval $(call build-art-cache-oat,system/app/talkback.apk))
$(eval $(call build-art-cache-oat,system/framework/am.jar))
$(eval $(call build-art-cache-oat,system/framework/android.test.runner.jar))
$(eval $(call build-art-cache-oat,system/framework/bmgr.jar))
$(eval $(call build-art-cache-oat,system/framework/bu.jar))
$(eval $(call build-art-cache-oat,system/framework/com.android.future.usb.accessory.jar))
$(eval $(call build-art-cache-oat,system/framework/com.android.location.provider.jar))
$(eval $(call build-art-cache-oat,system/framework/com.android.nfc_extras.jar))
$(eval $(call build-art-cache-oat,system/framework/com.google.android.maps.jar))
$(eval $(call build-art-cache-oat,system/framework/com.google.android.media.effects.jar))
$(eval $(call build-art-cache-oat,system/framework/ext.jar))
$(eval $(call build-art-cache-oat,system/framework/ime.jar))
$(eval $(call build-art-cache-oat,system/framework/input.jar))
$(eval $(call build-art-cache-oat,system/framework/javax.obex.jar))
$(eval $(call build-art-cache-oat,system/framework/monkey.jar))
$(eval $(call build-art-cache-oat,system/framework/pm.jar))
$(eval $(call build-art-cache-oat,system/framework/send_bug.jar))
$(eval $(call build-art-cache-oat,system/framework/svc.jar))

else

$(warning do not know what jars to compile for $(TARGET_PRODUCT))

endif

endif

.PHONY: oat-target-sync
oat-target-sync: $(ART_TARGET_DEPENDENCIES) $(TARGET_BOOT_OAT) $(ART_CACHE_OATS)
	adb remount
	adb sync

########################################################################
# oatdump targets

.PHONY: dump-oat
dump-oat: dump-oat-core dump-oat-boot dump-oat-Calculator

.PHONY: dump-oat-core
dump-oat-core: $(TARGET_CORE_OAT) $(OATDUMP)
	$(OATDUMP) --image=$(TARGET_CORE_IMG) --host-prefix=$(PRODUCT_OUT) --output=/tmp/core.oatdump.txt
	@echo Output in /tmp/core.oatdump.txt

.PHONY: dump-oat-boot
dump-oat-boot: $(TARGET_BOOT_OAT) $(OATDUMP)
	$(OATDUMP) --image=$(TARGET_BOOT_IMG) --host-prefix=$(PRODUCT_OUT) --output=/tmp/boot.oatdump.txt
	@echo Output in /tmp/boot.oatdump.txt

.PHONY: dump-oat-Calculator
dump-oat-Calculator: $(call art-cache-oat,system/app/Calculator.apk) $(TARGET_BOOT_OAT) $(OATDUMP)
	$(OATDUMP) --oat-file=$< --boot-image=$(TARGET_BOOT_IMG) --host-prefix=$(PRODUCT_OUT) --output=/tmp/Calculator.oatdump.txt
	@echo Output in /tmp/Calculator.oatdump.txt


########################################################################
# clean-oat target
#

.PHONY: clean-oat
clean-oat:
	rm -f $(ART_TEST_OUT)/*.oat
	rm -f $(ART_TEST_OUT)/*.art
	rm -f $(ART_CACHE_OUT)/*.oat
	rm -f $(ART_CACHE_OUT)/*.art
	adb shell rm $(ART_TEST_DIR)/*.oat
	adb shell rm $(ART_TEST_DIR)/*.art
	adb shell rm $(ART_CACHE_DIR)/*.oat
	adb shell rm $(ART_CACHE_DIR)/*.art

########################################################################
# cpplint target

# "mm cpplint-art" to style check art source files
.PHONY: cpplint-art
cpplint-art:
	./art/tools/cpplint.py \
	    --filter=-whitespace/comments,-whitespace/line_length,-build/include,-build/header_guard,-readability/streams,-readability/todo,-runtime/references \
	    $(ANDROID_BUILD_TOP)/art/src/*.h $(ANDROID_BUILD_TOP)/art/src/*.cc

########################################################################

include $(call all-makefiles-under,$(LOCAL_PATH))
