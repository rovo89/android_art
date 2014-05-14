/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "parsed_options.h"
#ifdef HAVE_ANDROID_OS
#include "cutils/properties.h"
#endif

#include "debugger.h"
#include "monitor.h"

namespace art {

ParsedOptions* ParsedOptions::Create(const Runtime::Options& options, bool ignore_unrecognized) {
  UniquePtr<ParsedOptions> parsed(new ParsedOptions());
  if (parsed->Parse(options, ignore_unrecognized)) {
    return parsed.release();
  }
  return nullptr;
}

// Parse a string of the form /[0-9]+[kKmMgG]?/, which is used to specify
// memory sizes.  [kK] indicates kilobytes, [mM] megabytes, and
// [gG] gigabytes.
//
// "s" should point just past the "-Xm?" part of the string.
// "div" specifies a divisor, e.g. 1024 if the value must be a multiple
// of 1024.
//
// The spec says the -Xmx and -Xms options must be multiples of 1024.  It
// doesn't say anything about -Xss.
//
// Returns 0 (a useless size) if "s" is malformed or specifies a low or
// non-evenly-divisible value.
//
size_t ParseMemoryOption(const char* s, size_t div) {
  // strtoul accepts a leading [+-], which we don't want,
  // so make sure our string starts with a decimal digit.
  if (isdigit(*s)) {
    char* s2;
    size_t val = strtoul(s, &s2, 10);
    if (s2 != s) {
      // s2 should be pointing just after the number.
      // If this is the end of the string, the user
      // has specified a number of bytes.  Otherwise,
      // there should be exactly one more character
      // that specifies a multiplier.
      if (*s2 != '\0') {
        // The remainder of the string is either a single multiplier
        // character, or nothing to indicate that the value is in
        // bytes.
        char c = *s2++;
        if (*s2 == '\0') {
          size_t mul;
          if (c == '\0') {
            mul = 1;
          } else if (c == 'k' || c == 'K') {
            mul = KB;
          } else if (c == 'm' || c == 'M') {
            mul = MB;
          } else if (c == 'g' || c == 'G') {
            mul = GB;
          } else {
            // Unknown multiplier character.
            return 0;
          }

          if (val <= std::numeric_limits<size_t>::max() / mul) {
            val *= mul;
          } else {
            // Clamp to a multiple of 1024.
            val = std::numeric_limits<size_t>::max() & ~(1024-1);
          }
        } else {
          // There's more than one character after the numeric part.
          return 0;
        }
      }
      // The man page says that a -Xm value must be a multiple of 1024.
      if (val % div == 0) {
        return val;
      }
    }
  }
  return 0;
}

static gc::CollectorType ParseCollectorType(const std::string& option) {
  if (option == "MS" || option == "nonconcurrent") {
    return gc::kCollectorTypeMS;
  } else if (option == "CMS" || option == "concurrent") {
    return gc::kCollectorTypeCMS;
  } else if (option == "SS") {
    return gc::kCollectorTypeSS;
  } else if (option == "GSS") {
    return gc::kCollectorTypeGSS;
  } else if (option == "CC") {
    return gc::kCollectorTypeCC;
  } else {
    return gc::kCollectorTypeNone;
  }
}

bool ParsedOptions::ParseXGcOption(const std::string& option) {
  std::vector<std::string> gc_options;
  Split(option.substr(strlen("-Xgc:")), ',', gc_options);
  for (const std::string& gc_option : gc_options) {
    gc::CollectorType collector_type = ParseCollectorType(gc_option);
    if (collector_type != gc::kCollectorTypeNone) {
      collector_type_ = collector_type;
    } else if (gc_option == "preverify") {
      verify_pre_gc_heap_ = true;
    } else if (gc_option == "nopreverify") {
      verify_pre_gc_heap_ = false;
    }  else if (gc_option == "presweepingverify") {
      verify_pre_sweeping_heap_ = true;
    } else if (gc_option == "nopresweepingverify") {
      verify_pre_sweeping_heap_ = false;
    } else if (gc_option == "postverify") {
      verify_post_gc_heap_ = true;
    } else if (gc_option == "nopostverify") {
      verify_post_gc_heap_ = false;
    } else if (gc_option == "preverify_rosalloc") {
      verify_pre_gc_rosalloc_ = true;
    } else if (gc_option == "nopreverify_rosalloc") {
      verify_pre_gc_rosalloc_ = false;
    } else if (gc_option == "presweepingverify_rosalloc") {
      verify_pre_sweeping_rosalloc_ = true;
    } else if (gc_option == "nopresweepingverify_rosalloc") {
      verify_pre_sweeping_rosalloc_ = false;
    } else if (gc_option == "postverify_rosalloc") {
      verify_post_gc_rosalloc_ = true;
    } else if (gc_option == "nopostverify_rosalloc") {
      verify_post_gc_rosalloc_ = false;
    } else if ((gc_option == "precise") ||
               (gc_option == "noprecise") ||
               (gc_option == "verifycardtable") ||
               (gc_option == "noverifycardtable")) {
      // Ignored for backwards compatibility.
    } else {
      Usage("Unknown -Xgc option %s\n", gc_option.c_str());
      return false;
    }
  }
  return true;
}

bool ParsedOptions::Parse(const Runtime::Options& options, bool ignore_unrecognized) {
  const char* boot_class_path_string = getenv("BOOTCLASSPATH");
  if (boot_class_path_string != NULL) {
    boot_class_path_string_ = boot_class_path_string;
  }
  const char* class_path_string = getenv("CLASSPATH");
  if (class_path_string != NULL) {
    class_path_string_ = class_path_string;
  }
  // -Xcheck:jni is off by default for regular builds but on by default in debug builds.
  check_jni_ = kIsDebugBuild;

  heap_initial_size_ = gc::Heap::kDefaultInitialSize;
  heap_maximum_size_ = gc::Heap::kDefaultMaximumSize;
  heap_min_free_ = gc::Heap::kDefaultMinFree;
  heap_max_free_ = gc::Heap::kDefaultMaxFree;
  heap_target_utilization_ = gc::Heap::kDefaultTargetUtilization;
  foreground_heap_growth_multiplier_ = gc::Heap::kDefaultHeapGrowthMultiplier;
  heap_growth_limit_ = 0;  // 0 means no growth limit .
  // Default to number of processors minus one since the main GC thread also does work.
  parallel_gc_threads_ = sysconf(_SC_NPROCESSORS_CONF) - 1;
  // Only the main GC thread, no workers.
  conc_gc_threads_ = 0;
  // The default GC type is set in makefiles.
#if ART_DEFAULT_GC_TYPE_IS_CMS
  collector_type_ = gc::kCollectorTypeCMS;
#elif ART_DEFAULT_GC_TYPE_IS_SS
  collector_type_ = gc::kCollectorTypeSS;
#elif ART_DEFAULT_GC_TYPE_IS_GSS
  collector_type_ = gc::kCollectorTypeGSS;
#else
#error "ART default GC type must be set"
#endif
  // If background_collector_type_ is kCollectorTypeNone, it defaults to the collector_type_ after
  // parsing options.
  background_collector_type_ = gc::kCollectorTypeNone;
  stack_size_ = 0;  // 0 means default.
  max_spins_before_thin_lock_inflation_ = Monitor::kDefaultMaxSpinsBeforeThinLockInflation;
  low_memory_mode_ = false;
  use_tlab_ = false;
  verify_pre_gc_heap_ = false;
  // Pre sweeping is the one that usually fails if the GC corrupted the heap.
  verify_pre_sweeping_heap_ = kIsDebugBuild;
  verify_post_gc_heap_ = false;
  verify_pre_gc_rosalloc_ = kIsDebugBuild;
  verify_pre_sweeping_rosalloc_ = false;
  verify_post_gc_rosalloc_ = false;

  compiler_callbacks_ = nullptr;
  is_zygote_ = false;
  if (kPoisonHeapReferences) {
    // kPoisonHeapReferences currently works only with the interpreter only.
    // TODO: make it work with the compiler.
    interpreter_only_ = true;
  } else {
    interpreter_only_ = false;
  }
  is_explicit_gc_disabled_ = false;

  long_pause_log_threshold_ = gc::Heap::kDefaultLongPauseLogThreshold;
  long_gc_log_threshold_ = gc::Heap::kDefaultLongGCLogThreshold;
  dump_gc_performance_on_shutdown_ = false;
  ignore_max_footprint_ = false;

  lock_profiling_threshold_ = 0;
  hook_is_sensitive_thread_ = NULL;

  hook_vfprintf_ = vfprintf;
  hook_exit_ = exit;
  hook_abort_ = NULL;  // We don't call abort(3) by default; see Runtime::Abort.

//  gLogVerbosity.class_linker = true;  // TODO: don't check this in!
//  gLogVerbosity.compiler = true;  // TODO: don't check this in!
//  gLogVerbosity.gc = true;  // TODO: don't check this in!
//  gLogVerbosity.heap = true;  // TODO: don't check this in!
//  gLogVerbosity.jdwp = true;  // TODO: don't check this in!
//  gLogVerbosity.jni = true;  // TODO: don't check this in!
//  gLogVerbosity.monitor = true;  // TODO: don't check this in!
//  gLogVerbosity.profiler = true;  // TODO: don't check this in!
//  gLogVerbosity.signals = true;  // TODO: don't check this in!
//  gLogVerbosity.startup = true;  // TODO: don't check this in!
//  gLogVerbosity.third_party_jni = true;  // TODO: don't check this in!
//  gLogVerbosity.threads = true;  // TODO: don't check this in!
//  gLogVerbosity.verifier = true;  // TODO: don't check this in!

  method_trace_ = false;
  method_trace_file_ = "/data/method-trace-file.bin";
  method_trace_file_size_ = 10 * MB;

  profile_ = false;
  profile_period_s_ = 10;           // Seconds.
  profile_duration_s_ = 20;          // Seconds.
  profile_interval_us_ = 500;       // Microseconds.
  profile_backoff_coefficient_ = 2.0;
  profile_start_immediately_ = true;
  profile_clock_source_ = kDefaultProfilerClockSource;

  verify_ = true;
  image_isa_ = kRuntimeISA;

  // Default to explicit checks.  Switch off with -implicit-checks:.
  // or setprop dalvik.vm.implicit_checks check1,check2,...
#ifdef HAVE_ANDROID_OS
  {
    char buf[PROP_VALUE_MAX];
    property_get("dalvik.vm.implicit_checks", buf, "none");
    std::string checks(buf);
    std::vector<std::string> checkvec;
    Split(checks, ',', checkvec);
    explicit_checks_ = kExplicitNullCheck | kExplicitSuspendCheck |
        kExplicitStackOverflowCheck;
    for (auto& str : checkvec) {
      std::string val = Trim(str);
      if (val == "none") {
        explicit_checks_ = kExplicitNullCheck | kExplicitSuspendCheck |
          kExplicitStackOverflowCheck;
      } else if (val == "null") {
        explicit_checks_ &= ~kExplicitNullCheck;
      } else if (val == "suspend") {
        explicit_checks_ &= ~kExplicitSuspendCheck;
      } else if (val == "stack") {
        explicit_checks_ &= ~kExplicitStackOverflowCheck;
      } else if (val == "all") {
        explicit_checks_ = 0;
      }
    }
  }
#else
  explicit_checks_ = kExplicitNullCheck | kExplicitSuspendCheck |
    kExplicitStackOverflowCheck;
#endif

  for (size_t i = 0; i < options.size(); ++i) {
    if (true && options[0].first == "-Xzygote") {
      LOG(INFO) << "option[" << i << "]=" << options[i].first;
    }
  }
  for (size_t i = 0; i < options.size(); ++i) {
    const std::string option(options[i].first);
    if (StartsWith(option, "-help")) {
      Usage(nullptr);
      return false;
    } else if (StartsWith(option, "-showversion")) {
      UsageMessage(stdout, "ART version %s\n", Runtime::GetVersion());
      Exit(0);
    } else if (StartsWith(option, "-Xbootclasspath:")) {
      boot_class_path_string_ = option.substr(strlen("-Xbootclasspath:")).data();
    } else if (option == "-classpath" || option == "-cp") {
      // TODO: support -Djava.class.path
      i++;
      if (i == options.size()) {
        Usage("Missing required class path value for %s\n", option.c_str());
        return false;
      }
      const StringPiece& value = options[i].first;
      class_path_string_ = value.data();
    } else if (option == "bootclasspath") {
      boot_class_path_
          = reinterpret_cast<const std::vector<const DexFile*>*>(options[i].second);
    } else if (StartsWith(option, "-Ximage:")) {
      if (!ParseStringAfterChar(option, ':', &image_)) {
        return false;
      }
    } else if (StartsWith(option, "-Xcheck:jni")) {
      check_jni_ = true;
    } else if (StartsWith(option, "-Xrunjdwp:") || StartsWith(option, "-agentlib:jdwp=")) {
      std::string tail(option.substr(option[1] == 'X' ? 10 : 15));
      // TODO: move parsing logic out of Dbg
      if (tail == "help" || !Dbg::ParseJdwpOptions(tail)) {
        if (tail != "help") {
          UsageMessage(stderr, "Failed to parse JDWP option %s\n", tail.c_str());
        }
        Usage("Example: -Xrunjdwp:transport=dt_socket,address=8000,server=y\n"
              "Example: -Xrunjdwp:transport=dt_socket,address=localhost:6500,server=n\n");
        return false;
      }
    } else if (StartsWith(option, "-Xms")) {
      size_t size = ParseMemoryOption(option.substr(strlen("-Xms")).c_str(), 1024);
      if (size == 0) {
        Usage("Failed to parse memory option %s\n", option.c_str());
        return false;
      }
      heap_initial_size_ = size;
    } else if (StartsWith(option, "-Xmx")) {
      size_t size = ParseMemoryOption(option.substr(strlen("-Xmx")).c_str(), 1024);
      if (size == 0) {
        Usage("Failed to parse memory option %s\n", option.c_str());
        return false;
      }
      heap_maximum_size_ = size;
    } else if (StartsWith(option, "-XX:HeapGrowthLimit=")) {
      size_t size = ParseMemoryOption(option.substr(strlen("-XX:HeapGrowthLimit=")).c_str(), 1024);
      if (size == 0) {
        Usage("Failed to parse memory option %s\n", option.c_str());
        return false;
      }
      heap_growth_limit_ = size;
    } else if (StartsWith(option, "-XX:HeapMinFree=")) {
      size_t size = ParseMemoryOption(option.substr(strlen("-XX:HeapMinFree=")).c_str(), 1024);
      if (size == 0) {
        Usage("Failed to parse memory option %s\n", option.c_str());
        return false;
      }
      heap_min_free_ = size;
    } else if (StartsWith(option, "-XX:HeapMaxFree=")) {
      size_t size = ParseMemoryOption(option.substr(strlen("-XX:HeapMaxFree=")).c_str(), 1024);
      if (size == 0) {
        Usage("Failed to parse memory option %s\n", option.c_str());
        return false;
      }
      heap_max_free_ = size;
    } else if (StartsWith(option, "-XX:HeapTargetUtilization=")) {
      if (!ParseDouble(option, '=', 0.1, 0.9, &heap_target_utilization_)) {
        return false;
      }
    } else if (StartsWith(option, "-XX:ForegroundHeapGrowthMultiplier=")) {
      if (!ParseDouble(option, '=', 0.1, 10.0, &foreground_heap_growth_multiplier_)) {
        return false;
      }
    } else if (StartsWith(option, "-XX:ParallelGCThreads=")) {
      if (!ParseUnsignedInteger(option, '=', &parallel_gc_threads_)) {
        return false;
      }
    } else if (StartsWith(option, "-XX:ConcGCThreads=")) {
      if (!ParseUnsignedInteger(option, '=', &conc_gc_threads_)) {
        return false;
      }
    } else if (StartsWith(option, "-Xss")) {
      size_t size = ParseMemoryOption(option.substr(strlen("-Xss")).c_str(), 1);
      if (size == 0) {
        Usage("Failed to parse memory option %s\n", option.c_str());
        return false;
      }
      stack_size_ = size;
    } else if (StartsWith(option, "-XX:MaxSpinsBeforeThinLockInflation=")) {
      if (!ParseUnsignedInteger(option, '=', &max_spins_before_thin_lock_inflation_)) {
        return false;
      }
    } else if (StartsWith(option, "-XX:LongPauseLogThreshold=")) {
      unsigned int value;
      if (!ParseUnsignedInteger(option, '=', &value)) {
        return false;
      }
      long_pause_log_threshold_ = MsToNs(value);
    } else if (StartsWith(option, "-XX:LongGCLogThreshold=")) {
      unsigned int value;
      if (!ParseUnsignedInteger(option, '=', &value)) {
        return false;
      }
      long_gc_log_threshold_ = MsToNs(value);
    } else if (option == "-XX:DumpGCPerformanceOnShutdown") {
      dump_gc_performance_on_shutdown_ = true;
    } else if (option == "-XX:IgnoreMaxFootprint") {
      ignore_max_footprint_ = true;
    } else if (option == "-XX:LowMemoryMode") {
      low_memory_mode_ = true;
    } else if (option == "-XX:UseTLAB") {
      use_tlab_ = true;
    } else if (StartsWith(option, "-D")) {
      properties_.push_back(option.substr(strlen("-D")));
    } else if (StartsWith(option, "-Xjnitrace:")) {
      jni_trace_ = option.substr(strlen("-Xjnitrace:"));
    } else if (option == "compilercallbacks") {
      compiler_callbacks_ =
          reinterpret_cast<CompilerCallbacks*>(const_cast<void*>(options[i].second));
    } else if (option == "imageinstructionset") {
      image_isa_ = GetInstructionSetFromString(
          reinterpret_cast<const char*>(options[i].second));
    } else if (option == "-Xzygote") {
      is_zygote_ = true;
    } else if (option == "-Xint") {
      interpreter_only_ = true;
    } else if (StartsWith(option, "-Xgc:")) {
      if (!ParseXGcOption(option)) {
        return false;
      }
    } else if (StartsWith(option, "-XX:BackgroundGC=")) {
      std::string substring;
      if (!ParseStringAfterChar(option, '=', &substring)) {
        return false;
      }
      gc::CollectorType collector_type = ParseCollectorType(substring);
      if (collector_type != gc::kCollectorTypeNone) {
        background_collector_type_ = collector_type;
      } else {
        Usage("Unknown -XX:BackgroundGC option %s\n", substring.c_str());
        return false;
      }
    } else if (option == "-XX:+DisableExplicitGC") {
      is_explicit_gc_disabled_ = true;
    } else if (StartsWith(option, "-verbose:")) {
      std::vector<std::string> verbose_options;
      Split(option.substr(strlen("-verbose:")), ',', verbose_options);
      for (size_t i = 0; i < verbose_options.size(); ++i) {
        if (verbose_options[i] == "class") {
          gLogVerbosity.class_linker = true;
        } else if (verbose_options[i] == "compiler") {
          gLogVerbosity.compiler = true;
        } else if (verbose_options[i] == "gc") {
          gLogVerbosity.gc = true;
        } else if (verbose_options[i] == "heap") {
          gLogVerbosity.heap = true;
        } else if (verbose_options[i] == "jdwp") {
          gLogVerbosity.jdwp = true;
        } else if (verbose_options[i] == "jni") {
          gLogVerbosity.jni = true;
        } else if (verbose_options[i] == "monitor") {
          gLogVerbosity.monitor = true;
        } else if (verbose_options[i] == "profiler") {
          gLogVerbosity.profiler = true;
        } else if (verbose_options[i] == "signals") {
          gLogVerbosity.signals = true;
        } else if (verbose_options[i] == "startup") {
          gLogVerbosity.startup = true;
        } else if (verbose_options[i] == "third-party-jni") {
          gLogVerbosity.third_party_jni = true;
        } else if (verbose_options[i] == "threads") {
          gLogVerbosity.threads = true;
        } else if (verbose_options[i] == "verifier") {
          gLogVerbosity.verifier = true;
        } else {
          Usage("Unknown -verbose option %s\n", verbose_options[i].c_str());
          return false;
        }
      }
    } else if (StartsWith(option, "-verbose-methods:")) {
      gLogVerbosity.compiler = false;
      Split(option.substr(strlen("-verbose-methods:")), ',', gVerboseMethods);
    } else if (StartsWith(option, "-Xlockprofthreshold:")) {
      if (!ParseUnsignedInteger(option, ':', &lock_profiling_threshold_)) {
        return false;
      }
    } else if (StartsWith(option, "-Xstacktracefile:")) {
      if (!ParseStringAfterChar(option, ':', &stack_trace_file_)) {
        return false;
      }
    } else if (option == "sensitiveThread") {
      const void* hook = options[i].second;
      hook_is_sensitive_thread_ = reinterpret_cast<bool (*)()>(const_cast<void*>(hook));
    } else if (option == "vfprintf") {
      const void* hook = options[i].second;
      if (hook == nullptr) {
        Usage("vfprintf argument was NULL");
        return false;
      }
      hook_vfprintf_ =
          reinterpret_cast<int (*)(FILE *, const char*, va_list)>(const_cast<void*>(hook));
    } else if (option == "exit") {
      const void* hook = options[i].second;
      if (hook == nullptr) {
        Usage("exit argument was NULL");
        return false;
      }
      hook_exit_ = reinterpret_cast<void(*)(jint)>(const_cast<void*>(hook));
    } else if (option == "abort") {
      const void* hook = options[i].second;
      if (hook == nullptr) {
        Usage("abort was NULL\n");
        return false;
      }
      hook_abort_ = reinterpret_cast<void(*)()>(const_cast<void*>(hook));
    } else if (option == "-Xmethod-trace") {
      method_trace_ = true;
    } else if (StartsWith(option, "-Xmethod-trace-file:")) {
      method_trace_file_ = option.substr(strlen("-Xmethod-trace-file:"));
    } else if (StartsWith(option, "-Xmethod-trace-file-size:")) {
      if (!ParseUnsignedInteger(option, ':', &method_trace_file_size_)) {
        return false;
      }
    } else if (option == "-Xprofile:threadcpuclock") {
      Trace::SetDefaultClockSource(kProfilerClockSourceThreadCpu);
    } else if (option == "-Xprofile:wallclock") {
      Trace::SetDefaultClockSource(kProfilerClockSourceWall);
    } else if (option == "-Xprofile:dualclock") {
      Trace::SetDefaultClockSource(kProfilerClockSourceDual);
    } else if (StartsWith(option, "-Xprofile:")) {
      if (!ParseStringAfterChar(option, ';', &profile_output_filename_)) {
        return false;
      }
      profile_ = true;
    } else if (StartsWith(option, "-Xprofile-period:")) {
      if (!ParseUnsignedInteger(option, ':', &profile_period_s_)) {
        return false;
      }
    } else if (StartsWith(option, "-Xprofile-duration:")) {
      if (!ParseUnsignedInteger(option, ':', &profile_duration_s_)) {
        return false;
      }
    } else if (StartsWith(option, "-Xprofile-interval:")) {
      if (!ParseUnsignedInteger(option, ':', &profile_interval_us_)) {
        return false;
      }
    } else if (StartsWith(option, "-Xprofile-backoff:")) {
      if (!ParseDouble(option, ':', 1.0, 10.0, &profile_backoff_coefficient_)) {
        return false;
      }
    } else if (option == "-Xprofile-start-lazy") {
      profile_start_immediately_ = false;
    } else if (StartsWith(option, "-implicit-checks:")) {
      std::string checks;
      if (!ParseStringAfterChar(option, ':', &checks)) {
        return false;
      }
      std::vector<std::string> checkvec;
      Split(checks, ',', checkvec);
      for (auto& str : checkvec) {
        std::string val = Trim(str);
        if (val == "none") {
          explicit_checks_ = kExplicitNullCheck | kExplicitSuspendCheck |
            kExplicitStackOverflowCheck;
        } else if (val == "null") {
          explicit_checks_ &= ~kExplicitNullCheck;
        } else if (val == "suspend") {
          explicit_checks_ &= ~kExplicitSuspendCheck;
        } else if (val == "stack") {
          explicit_checks_ &= ~kExplicitStackOverflowCheck;
        } else if (val == "all") {
          explicit_checks_ = 0;
        } else {
            return false;
        }
      }
    } else if (StartsWith(option, "-explicit-checks:")) {
      std::string checks;
      if (!ParseStringAfterChar(option, ':', &checks)) {
        return false;
      }
      std::vector<std::string> checkvec;
      Split(checks, ',', checkvec);
      for (auto& str : checkvec) {
        std::string val = Trim(str);
        if (val == "none") {
          explicit_checks_ = 0;
        } else if (val == "null") {
          explicit_checks_ |= kExplicitNullCheck;
        } else if (val == "suspend") {
          explicit_checks_ |= kExplicitSuspendCheck;
        } else if (val == "stack") {
          explicit_checks_ |= kExplicitStackOverflowCheck;
        } else if (val == "all") {
          explicit_checks_ = kExplicitNullCheck | kExplicitSuspendCheck |
            kExplicitStackOverflowCheck;
        } else {
          return false;
        }
      }
    } else if (option == "-Xcompiler-option") {
      i++;
      if (i == options.size()) {
        Usage("Missing required compiler option for %s\n", option.c_str());
        return false;
      }
      compiler_options_.push_back(options[i].first);
    } else if (option == "-Ximage-compiler-option") {
      i++;
      if (i == options.size()) {
        Usage("Missing required compiler option for %s\n", option.c_str());
        return false;
      }
      image_compiler_options_.push_back(options[i].first);
    } else if (StartsWith(option, "-Xverify:")) {
      std::string verify_mode = option.substr(strlen("-Xverify:"));
      if (verify_mode == "none") {
        verify_ = false;
      } else if (verify_mode == "remote" || verify_mode == "all") {
        verify_ = true;
      } else {
        Usage("Unknown -Xverify option %s\n", verify_mode.c_str());
        return false;
      }
    } else if (StartsWith(option, "-ea") ||
               StartsWith(option, "-da") ||
               StartsWith(option, "-enableassertions") ||
               StartsWith(option, "-disableassertions") ||
               (option == "--runtime-arg") ||
               (option == "-esa") ||
               (option == "-dsa") ||
               (option == "-enablesystemassertions") ||
               (option == "-disablesystemassertions") ||
               (option == "-Xrs") ||
               StartsWith(option, "-Xint:") ||
               StartsWith(option, "-Xdexopt:") ||
               (option == "-Xnoquithandler") ||
               StartsWith(option, "-Xjniopts:") ||
               StartsWith(option, "-Xjnigreflimit:") ||
               (option == "-Xgenregmap") ||
               (option == "-Xnogenregmap") ||
               StartsWith(option, "-Xverifyopt:") ||
               (option == "-Xcheckdexsum") ||
               (option == "-Xincludeselectedop") ||
               StartsWith(option, "-Xjitop:") ||
               (option == "-Xincludeselectedmethod") ||
               StartsWith(option, "-Xjitthreshold:") ||
               StartsWith(option, "-Xjitcodecachesize:") ||
               (option == "-Xjitblocking") ||
               StartsWith(option, "-Xjitmethod:") ||
               StartsWith(option, "-Xjitclass:") ||
               StartsWith(option, "-Xjitoffset:") ||
               StartsWith(option, "-Xjitconfig:") ||
               (option == "-Xjitcheckcg") ||
               (option == "-Xjitverbose") ||
               (option == "-Xjitprofile") ||
               (option == "-Xjitdisableopt") ||
               (option == "-Xjitsuspendpoll") ||
               StartsWith(option, "-XX:mainThreadStackSize=")) {
      // Ignored for backwards compatibility.
    } else if (!ignore_unrecognized) {
      Usage("Unrecognized option %s\n", option.c_str());
      return false;
    }
  }

  // If a reference to the dalvik core.jar snuck in, replace it with
  // the art specific version. This can happen with on device
  // boot.art/boot.oat generation by GenerateImage which relies on the
  // value of BOOTCLASSPATH.
#if defined(ART_TARGET)
  std::string core_jar("/core.jar");
  std::string core_libart_jar("/core-libart.jar");
#else
  // The host uses hostdex files.
  std::string core_jar("/core-hostdex.jar");
  std::string core_libart_jar("/core-libart-hostdex.jar");
#endif
  size_t core_jar_pos = boot_class_path_string_.find(core_jar);
  if (core_jar_pos != std::string::npos) {
    boot_class_path_string_.replace(core_jar_pos, core_jar.size(), core_libart_jar);
  }

  if (compiler_callbacks_ == nullptr && image_.empty()) {
    image_ += GetAndroidRoot();
    image_ += "/framework/boot.art";
  }
  if (heap_growth_limit_ == 0) {
    heap_growth_limit_ = heap_maximum_size_;
  }
  if (background_collector_type_ == gc::kCollectorTypeNone) {
    background_collector_type_ = collector_type_;
  }
  return true;
}  // NOLINT(readability/fn_size)

void ParsedOptions::Exit(int status) {
  hook_exit_(status);
}

void ParsedOptions::Abort() {
  hook_abort_();
}

void ParsedOptions::UsageMessageV(FILE* stream, const char* fmt, va_list ap) {
  hook_vfprintf_(stderr, fmt, ap);
}

void ParsedOptions::UsageMessage(FILE* stream, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  UsageMessageV(stream, fmt, ap);
  va_end(ap);
}

void ParsedOptions::Usage(const char* fmt, ...) {
  bool error = (fmt != nullptr);
  FILE* stream = error ? stderr : stdout;

  if (fmt != nullptr) {
    va_list ap;
    va_start(ap, fmt);
    UsageMessageV(stream, fmt, ap);
    va_end(ap);
  }

  const char* program = "dalvikvm";
  UsageMessage(stream, "%s: [options] class [argument ...]\n", program);
  UsageMessage(stream, "\n");
  UsageMessage(stream, "The following standard options are supported:\n");
  UsageMessage(stream, "  -classpath classpath (-cp classpath)\n");
  UsageMessage(stream, "  -Dproperty=value\n");
  UsageMessage(stream, "  -verbose:tag  ('gc', 'jni', or 'class')\n");
  UsageMessage(stream, "  -showversion\n");
  UsageMessage(stream, "  -help\n");
  UsageMessage(stream, "  -agentlib:jdwp=options\n");
  UsageMessage(stream, "\n");

  UsageMessage(stream, "The following extended options are supported:\n");
  UsageMessage(stream, "  -Xrunjdwp:<options>\n");
  UsageMessage(stream, "  -Xbootclasspath:bootclasspath\n");
  UsageMessage(stream, "  -Xcheck:tag  (e.g. 'jni')\n");
  UsageMessage(stream, "  -XmsN  (min heap, must be multiple of 1K, >= 1MB)\n");
  UsageMessage(stream, "  -XmxN  (max heap, must be multiple of 1K, >= 2MB)\n");
  UsageMessage(stream, "  -XssN  (stack size)\n");
  UsageMessage(stream, "  -Xint\n");
  UsageMessage(stream, "\n");

  UsageMessage(stream, "The following Dalvik options are supported:\n");
  UsageMessage(stream, "  -Xzygote\n");
  UsageMessage(stream, "  -Xjnitrace:substring (eg NativeClass or nativeMethod)\n");
  UsageMessage(stream, "  -Xstacktracefile:<filename>\n");
  UsageMessage(stream, "  -Xgc:[no]preverify\n");
  UsageMessage(stream, "  -Xgc:[no]postverify\n");
  UsageMessage(stream, "  -XX:+DisableExplicitGC\n");
  UsageMessage(stream, "  -XX:HeapGrowthLimit=N\n");
  UsageMessage(stream, "  -XX:HeapMinFree=N\n");
  UsageMessage(stream, "  -XX:HeapMaxFree=N\n");
  UsageMessage(stream, "  -XX:HeapTargetUtilization=doublevalue\n");
  UsageMessage(stream, "  -XX:ForegroundHeapGrowthMultiplier=doublevalue\n");
  UsageMessage(stream, "  -XX:LowMemoryMode\n");
  UsageMessage(stream, "  -Xprofile:{threadcpuclock,wallclock,dualclock}\n");
  UsageMessage(stream, "\n");

  UsageMessage(stream, "The following unique to ART options are supported:\n");
  UsageMessage(stream, "  -Xgc:[no]preverify_rosalloc\n");
  UsageMessage(stream, "  -Xgc:[no]postsweepingverify_rosalloc\n");
  UsageMessage(stream, "  -Xgc:[no]postverify_rosalloc\n");
  UsageMessage(stream, "  -Xgc:[no]presweepingverify\n");
  UsageMessage(stream, "  -Ximage:filename\n");
  UsageMessage(stream, "  -XX:ParallelGCThreads=integervalue\n");
  UsageMessage(stream, "  -XX:ConcGCThreads=integervalue\n");
  UsageMessage(stream, "  -XX:MaxSpinsBeforeThinLockInflation=integervalue\n");
  UsageMessage(stream, "  -XX:LongPauseLogThreshold=integervalue\n");
  UsageMessage(stream, "  -XX:LongGCLogThreshold=integervalue\n");
  UsageMessage(stream, "  -XX:DumpGCPerformanceOnShutdown\n");
  UsageMessage(stream, "  -XX:IgnoreMaxFootprint\n");
  UsageMessage(stream, "  -XX:UseTLAB\n");
  UsageMessage(stream, "  -XX:BackgroundGC=none\n");
  UsageMessage(stream, "  -Xmethod-trace\n");
  UsageMessage(stream, "  -Xmethod-trace-file:filename");
  UsageMessage(stream, "  -Xmethod-trace-file-size:integervalue\n");
  UsageMessage(stream, "  -Xprofile=filename\n");
  UsageMessage(stream, "  -Xprofile-period:integervalue\n");
  UsageMessage(stream, "  -Xprofile-duration:integervalue\n");
  UsageMessage(stream, "  -Xprofile-interval:integervalue\n");
  UsageMessage(stream, "  -Xprofile-backoff:integervalue\n");
  UsageMessage(stream, "  -Xcompiler-option dex2oat-option\n");
  UsageMessage(stream, "  -Ximage-compiler-option dex2oat-option\n");
  UsageMessage(stream, "\n");

  UsageMessage(stream, "The following previously supported Dalvik options are ignored:\n");
  UsageMessage(stream, "  -ea[:<package name>... |:<class name>]\n");
  UsageMessage(stream, "  -da[:<package name>... |:<class name>]\n");
  UsageMessage(stream, "   (-enableassertions, -disableassertions)\n");
  UsageMessage(stream, "  -esa\n");
  UsageMessage(stream, "  -dsa\n");
  UsageMessage(stream, "   (-enablesystemassertions, -disablesystemassertions)\n");
  UsageMessage(stream, "  -Xverify:{none,remote,all}\n");
  UsageMessage(stream, "  -Xrs\n");
  UsageMessage(stream, "  -Xint:portable, -Xint:fast, -Xint:jit\n");
  UsageMessage(stream, "  -Xdexopt:{none,verified,all,full}\n");
  UsageMessage(stream, "  -Xnoquithandler\n");
  UsageMessage(stream, "  -Xjniopts:{warnonly,forcecopy}\n");
  UsageMessage(stream, "  -Xjnigreflimit:integervalue\n");
  UsageMessage(stream, "  -Xgc:[no]precise\n");
  UsageMessage(stream, "  -Xgc:[no]verifycardtable\n");
  UsageMessage(stream, "  -X[no]genregmap\n");
  UsageMessage(stream, "  -Xverifyopt:[no]checkmon\n");
  UsageMessage(stream, "  -Xcheckdexsum\n");
  UsageMessage(stream, "  -Xincludeselectedop\n");
  UsageMessage(stream, "  -Xjitop:hexopvalue[-endvalue][,hexopvalue[-endvalue]]*\n");
  UsageMessage(stream, "  -Xincludeselectedmethod\n");
  UsageMessage(stream, "  -Xjitthreshold:integervalue\n");
  UsageMessage(stream, "  -Xjitcodecachesize:decimalvalueofkbytes\n");
  UsageMessage(stream, "  -Xjitblocking\n");
  UsageMessage(stream, "  -Xjitmethod:signature[,signature]* (eg Ljava/lang/String\\;replace)\n");
  UsageMessage(stream, "  -Xjitclass:classname[,classname]*\n");
  UsageMessage(stream, "  -Xjitoffset:offset[,offset]\n");
  UsageMessage(stream, "  -Xjitconfig:filename\n");
  UsageMessage(stream, "  -Xjitcheckcg\n");
  UsageMessage(stream, "  -Xjitverbose\n");
  UsageMessage(stream, "  -Xjitprofile\n");
  UsageMessage(stream, "  -Xjitdisableopt\n");
  UsageMessage(stream, "  -Xjitsuspendpoll\n");
  UsageMessage(stream, "  -XX:mainThreadStackSize=N\n");
  UsageMessage(stream, "\n");

  Exit((error) ? 1 : 0);
}

bool ParsedOptions::ParseStringAfterChar(const std::string& s, char c, std::string* parsed_value) {
  std::string::size_type colon = s.find(c);
  if (colon == std::string::npos) {
    Usage("Missing char %c in option %s\n", c, s.c_str());
    return false;
  }
  // Add one to remove the char we were trimming until.
  *parsed_value = s.substr(colon + 1);
  return true;
}

bool ParsedOptions::ParseInteger(const std::string& s, char after_char, int* parsed_value) {
  std::string::size_type colon = s.find(after_char);
  if (colon == std::string::npos) {
    Usage("Missing char %c in option %s\n", after_char, s.c_str());
    return false;
  }
  const char* begin = &s[colon + 1];
  char* end;
  size_t result = strtoul(begin, &end, 10);
  if (begin == end || *end != '\0') {
    Usage("Failed to parse integer from %s\n", s.c_str());
    return false;
  }
  *parsed_value = result;
  return true;
}

bool ParsedOptions::ParseUnsignedInteger(const std::string& s, char after_char,
                                         unsigned int* parsed_value) {
  int i;
  if (!ParseInteger(s, after_char, &i)) {
    return false;
  }
  if (i < 0) {
    Usage("Negative value %d passed for unsigned option %s\n", i, s.c_str());
    return false;
  }
  *parsed_value = i;
  return true;
}

bool ParsedOptions::ParseDouble(const std::string& option, char after_char,
                                double min, double max, double* parsed_value) {
  std::string substring;
  if (!ParseStringAfterChar(option, after_char, &substring)) {
    return false;
  }
  std::istringstream iss(substring);
  double value;
  iss >> value;
  // Ensure that we have a value, there was no cruft after it and it satisfies a sensible range.
  const bool sane_val = iss.eof() && (value >= min) && (value <= max);
  if (!sane_val) {
    Usage("Invalid double value %s for option %s\n", substring.c_str(), option.c_str());
    return false;
  }
  *parsed_value = value;
  return true;
}

}  // namespace art
