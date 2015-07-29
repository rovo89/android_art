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

#include "dex/pass_manager.h"

namespace art {

CompilerOptions::CompilerOptions()
    : compiler_filter_(kDefaultCompilerFilter),
      huge_method_threshold_(kDefaultHugeMethodThreshold),
      large_method_threshold_(kDefaultLargeMethodThreshold),
      small_method_threshold_(kDefaultSmallMethodThreshold),
      tiny_method_threshold_(kDefaultTinyMethodThreshold),
      num_dex_methods_threshold_(kDefaultNumDexMethodsThreshold),
      inline_depth_limit_(kDefaultInlineDepthLimit),
      inline_max_code_units_(kDefaultInlineMaxCodeUnits),
      include_patch_information_(kDefaultIncludePatchInformation),
      top_k_profile_threshold_(kDefaultTopKProfileThreshold),
      debuggable_(false),
      generate_debug_info_(kDefaultGenerateDebugInfo),
      implicit_null_checks_(true),
      implicit_so_checks_(true),
      implicit_suspend_checks_(false),
      compile_pic_(false),
      verbose_methods_(nullptr),
      pass_manager_options_(new PassManagerOptions),
      abort_on_hard_verifier_failure_(false),
      init_failure_output_(nullptr) {
}

CompilerOptions::~CompilerOptions() {
  // The destructor looks empty but it destroys a PassManagerOptions object. We keep it here
  // because we don't want to include the PassManagerOptions definition from the header file.
}

CompilerOptions::CompilerOptions(CompilerFilter compiler_filter,
                                 size_t huge_method_threshold,
                                 size_t large_method_threshold,
                                 size_t small_method_threshold,
                                 size_t tiny_method_threshold,
                                 size_t num_dex_methods_threshold,
                                 size_t inline_depth_limit,
                                 size_t inline_max_code_units,
                                 bool include_patch_information,
                                 double top_k_profile_threshold,
                                 bool debuggable,
                                 bool generate_debug_info,
                                 bool implicit_null_checks,
                                 bool implicit_so_checks,
                                 bool implicit_suspend_checks,
                                 bool compile_pic,
                                 const std::vector<std::string>* verbose_methods,
                                 PassManagerOptions* pass_manager_options,
                                 std::ostream* init_failure_output,
                                 bool abort_on_hard_verifier_failure
                                 ) :  // NOLINT(whitespace/parens)
    compiler_filter_(compiler_filter),
    huge_method_threshold_(huge_method_threshold),
    large_method_threshold_(large_method_threshold),
    small_method_threshold_(small_method_threshold),
    tiny_method_threshold_(tiny_method_threshold),
    num_dex_methods_threshold_(num_dex_methods_threshold),
    inline_depth_limit_(inline_depth_limit),
    inline_max_code_units_(inline_max_code_units),
    include_patch_information_(include_patch_information),
    top_k_profile_threshold_(top_k_profile_threshold),
    debuggable_(debuggable),
    generate_debug_info_(generate_debug_info),
    implicit_null_checks_(implicit_null_checks),
    implicit_so_checks_(implicit_so_checks),
    implicit_suspend_checks_(implicit_suspend_checks),
    compile_pic_(compile_pic),
    verbose_methods_(verbose_methods),
    pass_manager_options_(pass_manager_options),
    abort_on_hard_verifier_failure_(abort_on_hard_verifier_failure),
    init_failure_output_(init_failure_output) {
}

}  // namespace art
