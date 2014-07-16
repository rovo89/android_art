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

#ifndef ART_COMPILER_DRIVER_COMPILER_OPTIONS_H_
#define ART_COMPILER_DRIVER_COMPILER_OPTIONS_H_

namespace art {

class CompilerOptions {
 public:
  enum CompilerFilter {
    kVerifyNone,          // Skip verification and compile nothing except JNI stubs.
    kInterpretOnly,       // Compile nothing except JNI stubs.
    kSpace,               // Maximize space savings.
    kBalanced,            // Try to get the best performance return on compilation investment.
    kSpeed,               // Maximize runtime performance.
    kEverything           // Force compilation (Note: excludes compilaton of class initializers).
  };

  // Guide heuristics to determine whether to compile method if profile data not available.
#if ART_SMALL_MODE
  static const CompilerFilter kDefaultCompilerFilter = kInterpretOnly;
#else
  static const CompilerFilter kDefaultCompilerFilter = kSpeed;
#endif
  static const size_t kDefaultHugeMethodThreshold = 10000;
  static const size_t kDefaultLargeMethodThreshold = 600;
  static const size_t kDefaultSmallMethodThreshold = 60;
  static const size_t kDefaultTinyMethodThreshold = 20;
  static const size_t kDefaultNumDexMethodsThreshold = 900;
  static constexpr double kDefaultTopKProfileThreshold = 90.0;
  static const bool kDefaultIncludeDebugSymbols = kIsDebugBuild;
  static const bool kDefaultIncludePatchInformation = false;

  CompilerOptions() :
    compiler_filter_(kDefaultCompilerFilter),
    huge_method_threshold_(kDefaultHugeMethodThreshold),
    large_method_threshold_(kDefaultLargeMethodThreshold),
    small_method_threshold_(kDefaultSmallMethodThreshold),
    tiny_method_threshold_(kDefaultTinyMethodThreshold),
    num_dex_methods_threshold_(kDefaultNumDexMethodsThreshold),
    generate_gdb_information_(false),
    include_patch_information_(kDefaultIncludePatchInformation),
    top_k_profile_threshold_(kDefaultTopKProfileThreshold),
    include_debug_symbols_(kDefaultIncludeDebugSymbols),
    implicit_null_checks_(false),
    implicit_so_checks_(false),
    implicit_suspend_checks_(false)
#ifdef ART_SEA_IR_MODE
    , sea_ir_mode_(false)
#endif
    {}

  CompilerOptions(CompilerFilter compiler_filter,
                  size_t huge_method_threshold,
                  size_t large_method_threshold,
                  size_t small_method_threshold,
                  size_t tiny_method_threshold,
                  size_t num_dex_methods_threshold,
                  bool generate_gdb_information,
                  bool include_patch_information,
                  double top_k_profile_threshold,
                  bool include_debug_symbols,
                  bool implicit_null_checks,
                  bool implicit_so_checks,
                  bool implicit_suspend_checks
#ifdef ART_SEA_IR_MODE
                  , bool sea_ir_mode
#endif
                  ) :  // NOLINT(whitespace/parens)
    compiler_filter_(compiler_filter),
    huge_method_threshold_(huge_method_threshold),
    large_method_threshold_(large_method_threshold),
    small_method_threshold_(small_method_threshold),
    tiny_method_threshold_(tiny_method_threshold),
    num_dex_methods_threshold_(num_dex_methods_threshold),
    generate_gdb_information_(generate_gdb_information),
    include_patch_information_(include_patch_information),
    top_k_profile_threshold_(top_k_profile_threshold),
    include_debug_symbols_(include_debug_symbols),
    implicit_null_checks_(implicit_null_checks),
    implicit_so_checks_(implicit_so_checks),
    implicit_suspend_checks_(implicit_suspend_checks)
#ifdef ART_SEA_IR_MODE
    , sea_ir_mode_(sea_ir_mode)
#endif
    {}

  CompilerFilter GetCompilerFilter() const {
    return compiler_filter_;
  }

  void SetCompilerFilter(CompilerFilter compiler_filter) {
    compiler_filter_ = compiler_filter;
  }

  bool IsCompilationEnabled() const {
    return ((compiler_filter_ != CompilerOptions::kVerifyNone) &&
            (compiler_filter_ != CompilerOptions::kInterpretOnly));
  }

  bool IsVerificationEnabled() const {
    return (compiler_filter_ != CompilerOptions::kVerifyNone);
  }

  size_t GetHugeMethodThreshold() const {
    return huge_method_threshold_;
  }

  size_t GetLargeMethodThreshold() const {
    return large_method_threshold_;
  }

  size_t GetSmallMethodThreshold() const {
    return small_method_threshold_;
  }

  size_t GetTinyMethodThreshold() const {
    return tiny_method_threshold_;
  }

  bool IsHugeMethod(size_t num_dalvik_instructions) const {
    return num_dalvik_instructions > huge_method_threshold_;
  }

  bool IsLargeMethod(size_t num_dalvik_instructions) const {
    return num_dalvik_instructions > large_method_threshold_;
  }

  bool IsSmallMethod(size_t num_dalvik_instructions) const {
    return num_dalvik_instructions > small_method_threshold_;
  }

  bool IsTinyMethod(size_t num_dalvik_instructions) const {
    return num_dalvik_instructions > tiny_method_threshold_;
  }

  size_t GetNumDexMethodsThreshold() const {
    return num_dex_methods_threshold_;
  }

  double GetTopKProfileThreshold() const {
    return top_k_profile_threshold_;
  }

  bool GetIncludeDebugSymbols() const {
    return include_debug_symbols_;
  }

  bool GetImplicitNullChecks() const {
    return implicit_null_checks_;
  }

  void SetImplicitNullChecks(bool new_val) {
    implicit_null_checks_ = new_val;
  }

  bool GetImplicitStackOverflowChecks() const {
    return implicit_so_checks_;
  }

  void SetImplicitStackOverflowChecks(bool new_val) {
    implicit_so_checks_ = new_val;
  }

  bool GetImplicitSuspendChecks() const {
    return implicit_suspend_checks_;
  }

  void SetImplicitSuspendChecks(bool new_val) {
    implicit_suspend_checks_ = new_val;
  }

#ifdef ART_SEA_IR_MODE
  bool GetSeaIrMode();
#endif

  bool GetGenerateGDBInformation() const {
    return generate_gdb_information_;
  }

  bool GetIncludePatchInformation() const {
    return include_patch_information_;
  }

 private:
  CompilerFilter compiler_filter_;
  size_t huge_method_threshold_;
  size_t large_method_threshold_;
  size_t small_method_threshold_;
  size_t tiny_method_threshold_;
  size_t num_dex_methods_threshold_;
  bool generate_gdb_information_;
  bool include_patch_information_;
  // When using a profile file only the top K% of the profiled samples will be compiled.
  double top_k_profile_threshold_;
  bool include_debug_symbols_;
  bool implicit_null_checks_;
  bool implicit_so_checks_;
  bool implicit_suspend_checks_;
#ifdef ART_SEA_IR_MODE
  bool sea_ir_mode_;
#endif
};

}  // namespace art

#endif  // ART_COMPILER_DRIVER_COMPILER_OPTIONS_H_
