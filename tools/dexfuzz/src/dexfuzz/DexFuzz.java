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

package dexfuzz;

import dexfuzz.fuzzers.Fuzzer;
import dexfuzz.fuzzers.FuzzerMultipleExecute;
import dexfuzz.fuzzers.FuzzerMultipleNoExecute;
import dexfuzz.fuzzers.FuzzerSingleExecute;
import dexfuzz.fuzzers.FuzzerSingleNoExecute;
import dexfuzz.listeners.BaseListener;
import dexfuzz.listeners.ConsoleLoggerListener;
import dexfuzz.listeners.LogFileListener;
import dexfuzz.listeners.MultiplexerListener;
import dexfuzz.listeners.UniqueProgramTrackerListener;
import dexfuzz.listeners.UpdatingConsoleListener;

/**
 * Entrypoint class for dexfuzz.
 */
public class DexFuzz {
  private static int majorVersion = 1;
  private static int minorVersion = 1;
  private static int seedChangeVersion = 0;

  /**
   * Entrypoint to dexfuzz.
   */
  public static void main(String[] args) {
    // Report the version number, which should be incremented every time something will cause
    // the same input seed to produce a different result than before.
    Log.always(String.format("DexFuzz v%d.%d.%d",
        majorVersion, minorVersion, seedChangeVersion));
    Log.always("");

    if (!Options.readOptions(args)) {
      Log.error("Failed to validate options.");
      Options.usage();
    }

    // Create the Listener, which will listen for events and report them.
    BaseListener listener = null;
    if (Options.repeat > 1 && Options.execute) {
      // Create a Listener that is responsible for multiple Listeners.
      MultiplexerListener multipleListener = new MultiplexerListener();
      multipleListener.setup();
      // Add the live updating listener, but only if we're not printing out lots of logs.
      if (!Log.likelyToLog()) {
        multipleListener.addListener(new UpdatingConsoleListener());
      } else {
        // If we are dumping out lots of logs, then use the ConsoleLogger instead.
        multipleListener.addListener(new ConsoleLoggerListener());
      }
      // Add the file logging listener.
      multipleListener.addListener(new LogFileListener(Options.reportLogFile));
      // Add the unique program tracker.
      multipleListener.addListener(new UniqueProgramTrackerListener(Options.uniqueDatabaseFile));
      listener = multipleListener;
    } else {
      // Just use the basic listener.
      listener = new ConsoleLoggerListener();
    }

    // Create the Fuzzer that uses a particular strategy for fuzzing.
    Fuzzer fuzzer = null;
    if ((Options.repeat > 1) && Options.execute) {
      fuzzer = new FuzzerMultipleExecute(listener);
    } else if ((Options.repeat > 1) && !Options.execute) {
      fuzzer = new FuzzerMultipleNoExecute(listener);
    } else if ((Options.repeat == 1) && Options.execute) {
      fuzzer = new FuzzerSingleExecute(listener);
    } else if ((Options.repeat == 1) && !Options.execute) {
      fuzzer = new FuzzerSingleNoExecute(listener);
    } else {
      Log.errorAndQuit("Invalid options provided, desired fuzzer unknown.");
    }
    // TODO: Implement FuzzerFindMinimalMutations.
    // TODO: Implement FuzzerGenerational.

    // Actually run the Fuzzer.
    fuzzer.run();
    fuzzer.printTimingInfo();
    fuzzer.shutdown();

    // Cleanup the Listener.
    listener.shutdown();
  }
}
