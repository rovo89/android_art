/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "compiler_options.h"

#include <fstream>

namespace art {

CompilerOptions::CompilerOptions()
    : compiler_filter_(kDefaultCompilerFilter),
      huge_method_threshold_(kDefaultHugeMethodThreshold),
      large_method_threshold_(kDefaultLargeMethodThreshold),
      small_method_threshold_(kDefaultSmallMethodThreshold),
      tiny_method_threshold_(kDefaultTinyMethodThreshold),
      num_dex_methods_threshold_(kDefaultNumDexMethodsThreshold),
      inline_depth_limit_(kUnsetInlineDepthLimit),
      inline_max_code_units_(kUnsetInlineMaxCodeUnits),
      no_inline_from_(nullptr),
      include_patch_information_(kDefaultIncludePatchInformation),
      top_k_profile_threshold_(kDefaultTopKProfileThreshold),
      debuggable_(false),
      generate_debug_info_(kDefaultGenerateDebugInfo),
      generate_mini_debug_info_(kDefaultGenerateMiniDebugInfo),
      implicit_null_checks_(true),
      implicit_so_checks_(true),
      implicit_suspend_checks_(false),
      compile_pic_(false),
      verbose_methods_(nullptr),
      abort_on_hard_verifier_failure_(false),
      init_failure_output_(nullptr),
      dump_cfg_file_name_(""),
      dump_cfg_append_(false),
      force_determinism_(false) {
}

CompilerOptions::~CompilerOptions() {
  // The destructor looks empty but it destroys a PassManagerOptions object. We keep it here
  // because we don't want to include the PassManagerOptions definition from the header file.
}

CompilerOptions::CompilerOptions(CompilerFilter::Filter compiler_filter,
                                 size_t huge_method_threshold,
                                 size_t large_method_threshold,
                                 size_t small_method_threshold,
                                 size_t tiny_method_threshold,
                                 size_t num_dex_methods_threshold,
                                 size_t inline_depth_limit,
                                 size_t inline_max_code_units,
                                 const std::vector<const DexFile*>* no_inline_from,
                                 bool include_patch_information,
                                 double top_k_profile_threshold,
                                 bool debuggable,
                                 bool generate_debug_info,
                                 bool implicit_null_checks,
                                 bool implicit_so_checks,
                                 bool implicit_suspend_checks,
                                 bool compile_pic,
                                 const std::vector<std::string>* verbose_methods,
                                 std::ostream* init_failure_output,
                                 bool abort_on_hard_verifier_failure,
                                 const std::string& dump_cfg_file_name,
                                 bool dump_cfg_append,
                                 bool force_determinism
                                 ) :  // NOLINT(whitespace/parens)
    compiler_filter_(compiler_filter),
    huge_method_threshold_(huge_method_threshold),
    large_method_threshold_(large_method_threshold),
    small_method_threshold_(small_method_threshold),
    tiny_method_threshold_(tiny_method_threshold),
    num_dex_methods_threshold_(num_dex_methods_threshold),
    inline_depth_limit_(inline_depth_limit),
    inline_max_code_units_(inline_max_code_units),
    no_inline_from_(no_inline_from),
    include_patch_information_(include_patch_information),
    top_k_profile_threshold_(top_k_profile_threshold),
    debuggable_(debuggable),
    generate_debug_info_(generate_debug_info),
    generate_mini_debug_info_(kDefaultGenerateMiniDebugInfo),
    implicit_null_checks_(implicit_null_checks),
    implicit_so_checks_(implicit_so_checks),
    implicit_suspend_checks_(implicit_suspend_checks),
    compile_pic_(compile_pic),
    verbose_methods_(verbose_methods),
    abort_on_hard_verifier_failure_(abort_on_hard_verifier_failure),
    init_failure_output_(init_failure_output),
    dump_cfg_file_name_(dump_cfg_file_name),
    dump_cfg_append_(dump_cfg_append),
    force_determinism_(force_determinism) {
}

void CompilerOptions::ParseHugeMethodMax(const StringPiece& option, UsageFn Usage) {
  ParseUintOption(option, "--huge-method-max", &huge_method_threshold_, Usage);
}

void CompilerOptions::ParseLargeMethodMax(const StringPiece& option, UsageFn Usage) {
  ParseUintOption(option, "--large-method-max", &large_method_threshold_, Usage);
}

void CompilerOptions::ParseSmallMethodMax(const StringPiece& option, UsageFn Usage) {
  ParseUintOption(option, "--small-method-max", &small_method_threshold_, Usage);
}

void CompilerOptions::ParseTinyMethodMax(const StringPiece& option, UsageFn Usage) {
  ParseUintOption(option, "--tiny-method-max", &tiny_method_threshold_, Usage);
}

void CompilerOptions::ParseNumDexMethods(const StringPiece& option, UsageFn Usage) {
  ParseUintOption(option, "--num-dex-methods", &num_dex_methods_threshold_, Usage);
}

void CompilerOptions::ParseInlineDepthLimit(const StringPiece& option, UsageFn Usage) {
  ParseUintOption(option, "--inline-depth-limit", &inline_depth_limit_, Usage);
}

void CompilerOptions::ParseInlineMaxCodeUnits(const StringPiece& option, UsageFn Usage) {
  ParseUintOption(option, "--inline-max-code-units", &inline_max_code_units_, Usage);
}

void CompilerOptions::ParseDumpInitFailures(const StringPiece& option,
                                            UsageFn Usage ATTRIBUTE_UNUSED) {
  DCHECK(option.starts_with("--dump-init-failures="));
  std::string file_name = option.substr(strlen("--dump-init-failures=")).data();
  init_failure_output_.reset(new std::ofstream(file_name));
  if (init_failure_output_.get() == nullptr) {
    LOG(ERROR) << "Failed to allocate ofstream";
  } else if (init_failure_output_->fail()) {
    LOG(ERROR) << "Failed to open " << file_name << " for writing the initialization "
               << "failures.";
    init_failure_output_.reset();
  }
}

bool CompilerOptions::ParseCompilerOption(const StringPiece& option, UsageFn Usage) {
  if (option.starts_with("--compiler-filter=")) {
    const char* compiler_filter_string = option.substr(strlen("--compiler-filter=")).data();
    if (!CompilerFilter::ParseCompilerFilter(compiler_filter_string, &compiler_filter_)) {
      Usage("Unknown --compiler-filter value %s", compiler_filter_string);
    }
  } else if (option == "--compile-pic") {
    compile_pic_ = true;
  } else if (option.starts_with("--huge-method-max=")) {
    ParseHugeMethodMax(option, Usage);
  } else if (option.starts_with("--large-method-max=")) {
    ParseLargeMethodMax(option, Usage);
  } else if (option.starts_with("--small-method-max=")) {
    ParseSmallMethodMax(option, Usage);
  } else if (option.starts_with("--tiny-method-max=")) {
    ParseTinyMethodMax(option, Usage);
  } else if (option.starts_with("--num-dex-methods=")) {
    ParseNumDexMethods(option, Usage);
  } else if (option.starts_with("--inline-depth-limit=")) {
    ParseInlineDepthLimit(option, Usage);
  } else if (option.starts_with("--inline-max-code-units=")) {
    ParseInlineMaxCodeUnits(option, Usage);
  } else if (option == "--generate-debug-info" || option == "-g") {
    generate_debug_info_ = true;
  } else if (option == "--no-generate-debug-info") {
    generate_debug_info_ = false;
  } else if (option == "--generate-mini-debug-info") {
    generate_mini_debug_info_ = true;
  } else if (option == "--no-generate-mini-debug-info") {
    generate_mini_debug_info_ = false;
  } else if (option == "--debuggable") {
    debuggable_ = true;
  } else if (option.starts_with("--top-k-profile-threshold=")) {
    ParseDouble(option.data(), '=', 0.0, 100.0, &top_k_profile_threshold_, Usage);
  } else if (option == "--include-patch-information") {
    include_patch_information_ = true;
  } else if (option == "--no-include-patch-information") {
    include_patch_information_ = false;
  } else if (option == "--abort-on-hard-verifier-error") {
    abort_on_hard_verifier_failure_ = true;
  } else if (option.starts_with("--dump-init-failures=")) {
    ParseDumpInitFailures(option, Usage);
  } else if (option.starts_with("--dump-cfg=")) {
    dump_cfg_file_name_ = option.substr(strlen("--dump-cfg=")).data();
  } else if (option.starts_with("--dump-cfg-append")) {
    dump_cfg_append_ = true;
  } else {
    // Option not recognized.
    return false;
  }
  return true;
}

}  // namespace art
