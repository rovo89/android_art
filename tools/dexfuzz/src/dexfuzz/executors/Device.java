/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package dexfuzz.executors;

/**
 * Handles execution either on a remote device, or locally.
 * Currently only remote execution, on an ADB-connected device, is supported.
 */
public class Device {
  private boolean isLocal;
  private String deviceName;
  private boolean usingSpecificDevice;
  private boolean noBootImage;

  /**
   * The constructor for a local "device". Not yet supported.
   */
  public Device() {
    this.isLocal = true;
    throw new UnsupportedOperationException("Currently local execution is not supported.");
  }

  /**
   * The constructor for an ADB connected device.
   */
  public Device(String deviceName, boolean noBootImage) {
    if (!deviceName.isEmpty()) {
      this.deviceName = deviceName;
      this.usingSpecificDevice = true;
    }
    this.noBootImage = noBootImage;
  }

  /**
   * Get the name that would be provided to adb -s to communicate specifically with this device.
   */
  public String getName() {
    if (isLocal) {
      return "LOCAL DEVICE";
    }
    return deviceName;
  }

  public boolean isLocal() {
    return isLocal;
  }

  /**
   * Certain AOSP builds of Android may not have a full boot.art built. This will be set if
   * we use --no-boot-image, and is used by Executors when deciding the arguments for dalvikvm
   * and dex2oat when performing host-side verification.
   */
  public boolean noBootImageAvailable() {
    return noBootImage;
  }

  /**
   * Get the command prefix for this device if we want to use adb shell.
   */
  public String getExecutionShellPrefix() {
    if (isLocal) {
      return "";
    }
    return getExecutionPrefixWithAdb("shell");
  }

  /**
   * Get the command prefix for this device if we want to use adb push.
   */
  public String getExecutionPushPrefix() {
    if (isLocal) {
      return "";
    }
    return getExecutionPrefixWithAdb("push");
  }

  private String getExecutionPrefixWithAdb(String command) {
    if (usingSpecificDevice) {
      return String.format("adb -s %s %s ", deviceName, command);
    } else {
      return String.format("adb %s ", command);
    }
  }
}
