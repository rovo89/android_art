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

import dexfuzz.ExecutionResult;
import dexfuzz.Log;
import dexfuzz.Options;
import dexfuzz.StreamConsumer;
import dexfuzz.listeners.BaseListener;

import java.io.IOException;
import java.util.Map;

/**
 * Base class containing the common methods for executing a particular backend of ART.
 */
public abstract class Executor {
  private String androidHostOut;
  private String androidProductOut;

  private StreamConsumer outputConsumer;
  private StreamConsumer errorConsumer;

  protected ExecutionResult executionResult;
  protected String executeClass;

  // Set by subclasses.
  protected String name;
  protected int timeout;
  protected BaseListener listener;
  protected String testLocation;
  protected Architecture architecture;
  protected Device device;

  protected Executor(String name, int timeout, BaseListener listener, Architecture architecture,
      Device device) {
    executeClass = Options.executeClass;

    if (Options.shortTimeouts) {
      this.timeout = 2;
    } else {
      this.timeout = timeout;
    }

    this.name = name;
    this.listener = listener;
    this.architecture = architecture;
    this.device = device;

    this.testLocation = Options.executeDirectory;

    Map<String, String> envVars = System.getenv();
    androidProductOut = checkForEnvVar(envVars, "ANDROID_PRODUCT_OUT");
    androidHostOut = checkForEnvVar(envVars, "ANDROID_HOST_OUT");

    outputConsumer = new StreamConsumer();
    outputConsumer.start();
    errorConsumer = new StreamConsumer();
    errorConsumer.start();

    if (!device.isLocal()) {
      // Check for ADB.
      try {
        ProcessBuilder pb = new ProcessBuilder();
        pb.command("adb", "devices");
        Process process = pb.start();
        int exitValue = process.waitFor();
        if (exitValue != 0) {
          Log.errorAndQuit("Problem executing ADB - is it in your $PATH?");
        }
      } catch (IOException e) {
        Log.errorAndQuit("IOException when executing ADB, is it working?");
      } catch (InterruptedException e) {
        Log.errorAndQuit("InterruptedException when executing ADB, is it working?");
      }

      // Check we can run something on ADB.
      ExecutionResult result = executeOnDevice("true", true);
      if (result.getFlattenedAll().contains("device not found")) {
        Log.errorAndQuit("Couldn't connect to specified ADB device: " + device.getName());
      }
    }
  }

  private String checkForEnvVar(Map<String, String> envVars, String key) {
    if (!envVars.containsKey(key)) {
      Log.errorAndQuit("Cannot run a fuzzed program if $" + key + " is not set!");
    }
    return envVars.get(key);
  }

  private ExecutionResult executeCommand(String command, boolean captureOutput) {
    ExecutionResult result = new ExecutionResult();

    Log.info("Executing: " + command);

    try {
      ProcessBuilder processBuilder = new ProcessBuilder(command.split(" "));
      processBuilder.environment().put("ANDROID_ROOT", androidHostOut);
      Process process = processBuilder.start();

      if (captureOutput) {
        // Give the streams to the StreamConsumers.
        outputConsumer.giveStreamAndStartConsuming(process.getInputStream());
        errorConsumer.giveStreamAndStartConsuming(process.getErrorStream());
      }

      // Wait until the process is done - the StreamConsumers will keep the
      // buffers drained, so this shouldn't block indefinitely.
      // Get the return value as well.
      result.returnValue = process.waitFor();

      Log.info("Return value: " + result.returnValue);

      if (captureOutput) {
        // Tell the StreamConsumers to stop consuming, and wait for them to finish
        // so we know we have all of the output.
        outputConsumer.processFinished();
        errorConsumer.processFinished();
        result.output = outputConsumer.getOutput();
        result.error = errorConsumer.getOutput();

        // Always explicitly indicate the return code in the text output now.
        // NB: adb shell doesn't actually return exit codes currently, but this will
        // be useful if/when it does.
        result.output.add("RETURN CODE: " + result.returnValue);
      }

    } catch (IOException e) {
      Log.errorAndQuit("ExecutionResult.execute() caught an IOException");
    } catch (InterruptedException e) {
      Log.errorAndQuit("ExecutionResult.execute() caught an InterruptedException");
    }

    return result;
  }

  /**
   * Called by subclass Executors in their execute() implementations.
   */
  protected ExecutionResult executeOnDevice(String command, boolean captureOutput) {
    String timeoutString = "timeout " + timeout + " ";
    return executeCommand(timeoutString + device.getExecutionShellPrefix() + command,
        captureOutput);
  }

  private ExecutionResult pushToDevice(String command) {
    return executeCommand(device.getExecutionPushPrefix() + command, false);
  }

  /**
   * Call this to make sure the StreamConsumer threads are stopped.
   */
  public void shutdown() {
    outputConsumer.shutdown();
    errorConsumer.shutdown();
  }

  /**
   * Called by the Fuzzer after each execution has finished, to clear the results.
   */
  public void reset() {
    executionResult = null;
  }

  /**
   * Called by the Fuzzer to verify the mutated program using the host-side dex2oat.
   */
  public boolean verifyOnHost(String programName) {
    StringBuilder commandBuilder = new StringBuilder();
    commandBuilder.append("dex2oat ");

    // This assumes that the Architecture enum's name, when reduced to lower-case,
    // matches what dex2oat would expect.
    commandBuilder.append("--instruction-set=").append(architecture.toString().toLowerCase());
    commandBuilder.append(" --instruction-set-features=default ");

    // Select the correct boot image.
    commandBuilder.append("--boot-image=").append(androidProductOut);
    if (device.noBootImageAvailable()) {
      commandBuilder.append("/data/art-test/core.art ");
    } else {
      commandBuilder.append("/system/framework/boot.art ");
    }

    commandBuilder.append("--oat-file=output.oat ");
    commandBuilder.append("--android-root=").append(androidHostOut).append(" ");
    commandBuilder.append("--runtime-arg -classpath ");
    commandBuilder.append("--runtime-arg ").append(programName).append(" ");
    commandBuilder.append("--dex-file=").append(programName).append(" ");
    commandBuilder.append("--compiler-filter=interpret-only --runtime-arg -Xnorelocate ");

    ExecutionResult verificationResult = executeCommand(commandBuilder.toString(), true);

    boolean success = true;

    if (verificationResult.isSigabort()) {
      listener.handleHostVerificationSigabort(verificationResult);
      success = false;
    }

    if (success) {
      // Search for a keyword that indicates verification was not successful.
      // TODO: Determine if dex2oat crashed?
      for (String line : verificationResult.error) {
        if (line.contains("Verification error")
            || line.contains("Failure to verify dex file")) {
          success = false;
        }
        if (Options.dumpVerify) {
          // Strip out the start of the log lines.
          listener.handleDumpVerify(line.replaceFirst(".*(cc|h):\\d+] ",  ""));
        }
      }
    }

    if (!success) {
      listener.handleFailedHostVerification(verificationResult);
    }

    executeCommand("rm output.oat", false);

    return success;
  }

  /**
   * Called by the Fuzzer to upload the program to the target device.
   * TODO: Check if we're executing on a local device, and don't do this?
   */
  public void uploadToTarget(String programName) {
    pushToDevice(programName + " " + testLocation);
  }

  /**
   * Executor subclasses need to override this, to construct their arguments for dalvikvm
   * invocation correctly.
   */
  public abstract void execute(String programName);

  /**
   * Executor subclasses need to override this, to delete their generated OAT file correctly.
   */
  public abstract void deleteGeneratedOatFile(String programName);

  /**
   * Executor subclasses need to override this, to report if they need a cleaned code cache.
   */
  public abstract boolean needsCleanCodeCache();

  /**
   * Fuzzer.checkForArchitectureSplit() will use this determine the architecture of the Executor.
   */
  public Architecture getArchitecture() {
    return architecture;
  }

  /**
   * Used in each subclass of Executor's deleteGeneratedOatFile() method, to know what to delete.
   */
  protected String getOatFileName(String programName) {
    // Converts e.g. /data/art-test/file.dex to data@art-test@file.dex
    return (testLocation.replace("/", "@").substring(1) + "@" + programName);
  }

  /**
   * Used by the Fuzzer to get result of execution.
   */
  public ExecutionResult getResult() {
    return executionResult;
  }

  /**
   * Because dex2oat can accept a program with soft errors on the host, and then fail after
   * performing hard verification on the target, we need to check if the Executor detected
   * a target verification failure, before doing anything else with the resulting output.
   * Used by the Fuzzer.
   */
  public boolean verifyOnTarget() {
    // TODO: Remove this once host-verification can be forced to always fail?
    if (executionResult.getFlattenedOutput().contains("VerifyError")) {
      return false;
    }
    return true;
  }

  public String getName() {
    return name;
  }
}
