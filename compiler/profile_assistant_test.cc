/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include <gtest/gtest.h>

#include "base/unix_file/fd_file.h"
#include "common_runtime_test.h"
#include "compiler/profile_assistant.h"
#include "jit/offline_profiling_info.h"

namespace art {

class ProfileAssistantTest : public CommonRuntimeTest {
 protected:
  void SetupProfile(const std::string& id,
                    uint32_t checksum,
                    uint16_t number_of_methods,
                    const ScratchFile& profile,
                    ProfileCompilationInfo* info,
                    uint16_t start_method_index = 0) {
    std::string dex_location1 = "location1" + id;
    uint32_t dex_location_checksum1 = checksum;
    std::string dex_location2 = "location2" + id;
    uint32_t dex_location_checksum2 = 10 * checksum;
    for (uint16_t i = start_method_index; i < start_method_index + number_of_methods; i++) {
      ASSERT_TRUE(info->AddData(dex_location1, dex_location_checksum1, i));
      ASSERT_TRUE(info->AddData(dex_location2, dex_location_checksum2, i));
    }
    ASSERT_TRUE(info->Save(GetFd(profile)));
    ASSERT_EQ(0, profile.GetFile()->Flush());
    ASSERT_TRUE(profile.GetFile()->ResetOffset());
  }

  uint32_t GetFd(const ScratchFile& file) const {
    return static_cast<uint32_t>(file.GetFd());
  }
};

TEST_F(ProfileAssistantTest, AdviseCompilationEmptyReferences) {
  ScratchFile profile1;
  ScratchFile profile2;
  ScratchFile reference_profile1;
  ScratchFile reference_profile2;

  std::vector<uint32_t> profile_fds({
      GetFd(profile1),
      GetFd(profile2)});
  std::vector<uint32_t> reference_profile_fds({
      GetFd(reference_profile1),
      GetFd(reference_profile2)});

  const uint16_t kNumberOfMethodsToEnableCompilation = 100;
  ProfileCompilationInfo info1;
  SetupProfile("p1", 1, kNumberOfMethodsToEnableCompilation, profile1, &info1);
  ProfileCompilationInfo info2;
  SetupProfile("p2", 2, kNumberOfMethodsToEnableCompilation, profile2, &info2);

  // We should advise compilation.
  ProfileCompilationInfo* result;
  ASSERT_TRUE(ProfileAssistant::ProcessProfiles(profile_fds, reference_profile_fds, &result));
  ASSERT_TRUE(result != nullptr);

  // The resulting compilation info must be equal to the merge of the inputs.
  ProfileCompilationInfo expected;
  ASSERT_TRUE(expected.Load(info1));
  ASSERT_TRUE(expected.Load(info2));
  ASSERT_TRUE(expected.Equals(*result));

  // The information from profiles must be transfered to the reference profiles.
  ProfileCompilationInfo file_info1;
  ASSERT_TRUE(reference_profile1.GetFile()->ResetOffset());
  ASSERT_TRUE(file_info1.Load(GetFd(reference_profile1)));
  ASSERT_TRUE(file_info1.Equals(info1));

  ProfileCompilationInfo file_info2;
  ASSERT_TRUE(reference_profile2.GetFile()->ResetOffset());
  ASSERT_TRUE(file_info2.Load(GetFd(reference_profile2)));
  ASSERT_TRUE(file_info2.Equals(info2));

  // Initial profiles must be cleared.
  ASSERT_EQ(0, profile1.GetFile()->GetLength());
  ASSERT_EQ(0, profile2.GetFile()->GetLength());
}

TEST_F(ProfileAssistantTest, AdviseCompilationNonEmptyReferences) {
  ScratchFile profile1;
  ScratchFile profile2;
  ScratchFile reference_profile1;
  ScratchFile reference_profile2;

  std::vector<uint32_t> profile_fds({
      GetFd(profile1),
      GetFd(profile2)});
  std::vector<uint32_t> reference_profile_fds({
      GetFd(reference_profile1),
      GetFd(reference_profile2)});

  // The new profile info will contain the methods with indices 0-100.
  const uint16_t kNumberOfMethodsToEnableCompilation = 100;
  ProfileCompilationInfo info1;
  SetupProfile("p1", 1, kNumberOfMethodsToEnableCompilation, profile1, &info1);
  ProfileCompilationInfo info2;
  SetupProfile("p2", 2, kNumberOfMethodsToEnableCompilation, profile2, &info2);


  // The reference profile info will contain the methods with indices 50-150.
  const uint16_t kNumberOfMethodsAlreadyCompiled = 100;
  ProfileCompilationInfo reference_info1;
  SetupProfile("p1", 1, kNumberOfMethodsAlreadyCompiled, reference_profile1,
      &reference_info1, kNumberOfMethodsToEnableCompilation / 2);
  ProfileCompilationInfo reference_info2;
  SetupProfile("p2", 2, kNumberOfMethodsAlreadyCompiled, reference_profile2,
      &reference_info2, kNumberOfMethodsToEnableCompilation / 2);

  // We should advise compilation.
  ProfileCompilationInfo* result;
  ASSERT_TRUE(ProfileAssistant::ProcessProfiles(profile_fds, reference_profile_fds, &result));
  ASSERT_TRUE(result != nullptr);

  // The resulting compilation info must be equal to the merge of the inputs
  ProfileCompilationInfo expected;
  ASSERT_TRUE(expected.Load(info1));
  ASSERT_TRUE(expected.Load(info2));
  ASSERT_TRUE(expected.Load(reference_info1));
  ASSERT_TRUE(expected.Load(reference_info2));
  ASSERT_TRUE(expected.Equals(*result));

  // The information from profiles must be transfered to the reference profiles.
  ProfileCompilationInfo file_info1;
  ProfileCompilationInfo merge1;
  ASSERT_TRUE(merge1.Load(info1));
  ASSERT_TRUE(merge1.Load(reference_info1));
  ASSERT_TRUE(reference_profile1.GetFile()->ResetOffset());
  ASSERT_TRUE(file_info1.Load(GetFd(reference_profile1)));
  ASSERT_TRUE(file_info1.Equals(merge1));

  ProfileCompilationInfo file_info2;
  ProfileCompilationInfo merge2;
  ASSERT_TRUE(merge2.Load(info2));
  ASSERT_TRUE(merge2.Load(reference_info2));
  ASSERT_TRUE(reference_profile2.GetFile()->ResetOffset());
  ASSERT_TRUE(file_info2.Load(GetFd(reference_profile2)));
  ASSERT_TRUE(file_info2.Equals(merge2));

  // Initial profiles must be cleared.
  ASSERT_EQ(0, profile1.GetFile()->GetLength());
  ASSERT_EQ(0, profile2.GetFile()->GetLength());
}

TEST_F(ProfileAssistantTest, DoNotAdviseCompilation) {
  ScratchFile profile1;
  ScratchFile profile2;
  ScratchFile reference_profile1;
  ScratchFile reference_profile2;

  std::vector<uint32_t> profile_fds({
      GetFd(profile1),
      GetFd(profile2)});
  std::vector<uint32_t> reference_profile_fds({
      GetFd(reference_profile1),
      GetFd(reference_profile2)});

  const uint16_t kNumberOfMethodsToSkipCompilation = 1;
  ProfileCompilationInfo info1;
  SetupProfile("p1", 1, kNumberOfMethodsToSkipCompilation, profile1, &info1);
  ProfileCompilationInfo info2;
  SetupProfile("p2", 2, kNumberOfMethodsToSkipCompilation, profile2, &info2);

  // We should not advise compilation.
  ProfileCompilationInfo* result;
  ASSERT_TRUE(ProfileAssistant::ProcessProfiles(profile_fds, reference_profile_fds, &result));
  ASSERT_TRUE(result == nullptr);

  // The information from profiles must remain the same.
  ProfileCompilationInfo file_info1;
  ASSERT_TRUE(profile1.GetFile()->ResetOffset());
  ASSERT_TRUE(file_info1.Load(GetFd(profile1)));
  ASSERT_TRUE(file_info1.Equals(info1));

  ProfileCompilationInfo file_info2;
  ASSERT_TRUE(profile2.GetFile()->ResetOffset());
  ASSERT_TRUE(file_info2.Load(GetFd(profile2)));
  ASSERT_TRUE(file_info2.Equals(info2));

  // Reference profile files must remain empty.
  ASSERT_EQ(0, reference_profile1.GetFile()->GetLength());
  ASSERT_EQ(0, reference_profile2.GetFile()->GetLength());
}

TEST_F(ProfileAssistantTest, FailProcessingBecauseOfProfiles) {
  ScratchFile profile1;
  ScratchFile profile2;
  ScratchFile reference_profile1;
  ScratchFile reference_profile2;

  std::vector<uint32_t> profile_fds({
      GetFd(profile1),
      GetFd(profile2)});
  std::vector<uint32_t> reference_profile_fds({
      GetFd(reference_profile1),
      GetFd(reference_profile2)});

  const uint16_t kNumberOfMethodsToEnableCompilation = 100;
  // Assign different hashes for the same dex file. This will make merging of information to fail.
  ProfileCompilationInfo info1;
  SetupProfile("p1", 1, kNumberOfMethodsToEnableCompilation, profile1, &info1);
  ProfileCompilationInfo info2;
  SetupProfile("p1", 2, kNumberOfMethodsToEnableCompilation, profile2, &info2);

  // We should fail processing.
  ProfileCompilationInfo* result;
  ASSERT_FALSE(ProfileAssistant::ProcessProfiles(profile_fds, reference_profile_fds, &result));
  ASSERT_TRUE(result == nullptr);

  // The information from profiles must still remain the same.
  ProfileCompilationInfo file_info1;
  ASSERT_TRUE(profile1.GetFile()->ResetOffset());
  ASSERT_TRUE(file_info1.Load(GetFd(profile1)));
  ASSERT_TRUE(file_info1.Equals(info1));

  ProfileCompilationInfo file_info2;
  ASSERT_TRUE(profile2.GetFile()->ResetOffset());
  ASSERT_TRUE(file_info2.Load(GetFd(profile2)));
  ASSERT_TRUE(file_info2.Equals(info2));

  // Reference profile files must still remain empty.
  ASSERT_EQ(0, reference_profile1.GetFile()->GetLength());
  ASSERT_EQ(0, reference_profile2.GetFile()->GetLength());
}

TEST_F(ProfileAssistantTest, FailProcessingBecauseOfReferenceProfiles) {
  ScratchFile profile1;
  ScratchFile reference_profile;

  std::vector<uint32_t> profile_fds({
      GetFd(profile1)});
  std::vector<uint32_t> reference_profile_fds({
      GetFd(reference_profile)});

  const uint16_t kNumberOfMethodsToEnableCompilation = 100;
  // Assign different hashes for the same dex file. This will make merging of information to fail.
  ProfileCompilationInfo info1;
  SetupProfile("p1", 1, kNumberOfMethodsToEnableCompilation, profile1, &info1);
  ProfileCompilationInfo reference_info;
  SetupProfile("p1", 2, kNumberOfMethodsToEnableCompilation, reference_profile, &reference_info);

  // We should not advise compilation.
  ProfileCompilationInfo* result;
  ASSERT_TRUE(profile1.GetFile()->ResetOffset());
  ASSERT_TRUE(reference_profile.GetFile()->ResetOffset());
  ASSERT_FALSE(ProfileAssistant::ProcessProfiles(profile_fds, reference_profile_fds, &result));
  ASSERT_TRUE(result == nullptr);

  // The information from profiles must still remain the same.
  ProfileCompilationInfo file_info1;
  ASSERT_TRUE(profile1.GetFile()->ResetOffset());
  ASSERT_TRUE(file_info1.Load(GetFd(profile1)));
  ASSERT_TRUE(file_info1.Equals(info1));

  ProfileCompilationInfo file_info2;
  ASSERT_TRUE(reference_profile.GetFile()->ResetOffset());
  ASSERT_TRUE(file_info2.Load(GetFd(reference_profile)));
  ASSERT_TRUE(file_info2.Equals(reference_info));
}

}  // namespace art
