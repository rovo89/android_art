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

#ifndef ART_COMPILER_UTILS_ASSEMBLER_TEST_H_
#define ART_COMPILER_UTILS_ASSEMBLER_TEST_H_

#include "assembler.h"

#include "common_runtime_test.h"  // For ScratchFile

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sys/stat.h>

namespace art {

// Use a glocal static variable to keep the same name for all test data. Else we'll just spam the
// temp directory.
static std::string tmpnam_;

template<typename Ass, typename Reg, typename Imm>
class AssemblerTest : public testing::Test {
 public:
  Ass* GetAssembler() {
    return assembler_.get();
  }

  typedef std::string (*TestFn)(Ass* assembler);

  void DriverFn(TestFn f, std::string test_name) {
    Driver(f(assembler_.get()), test_name);
  }

  // This driver assumes the assembler has already been called.
  void DriverStr(std::string assembly_string, std::string test_name) {
    Driver(assembly_string, test_name);
  }

  std::string RepeatR(void (Ass::*f)(Reg), std::string fmt) {
    const std::vector<Reg*> registers = GetRegisters();
    std::string str;
    for (auto reg : registers) {
      (assembler_.get()->*f)(*reg);
      std::string base = fmt;

      size_t reg_index = base.find("{reg}");
      if (reg_index != std::string::npos) {
        std::ostringstream sreg;
        sreg << *reg;
        std::string reg_string = sreg.str();
        base.replace(reg_index, 5, reg_string);
      }

      if (str.size() > 0) {
        str += "\n";
      }
      str += base;
    }
    // Add a newline at the end.
    str += "\n";
    return str;
  }

  std::string RepeatRR(void (Ass::*f)(Reg, Reg), std::string fmt) {
    const std::vector<Reg*> registers = GetRegisters();
    std::string str;
    for (auto reg1 : registers) {
      for (auto reg2 : registers) {
        (assembler_.get()->*f)(*reg1, *reg2);
        std::string base = fmt;

        size_t reg1_index = base.find("{reg1}");
        if (reg1_index != std::string::npos) {
          std::ostringstream sreg;
          sreg << *reg1;
          std::string reg_string = sreg.str();
          base.replace(reg1_index, 6, reg_string);
        }

        size_t reg2_index = base.find("{reg2}");
        if (reg2_index != std::string::npos) {
          std::ostringstream sreg;
          sreg << *reg2;
          std::string reg_string = sreg.str();
          base.replace(reg2_index, 6, reg_string);
        }

        if (str.size() > 0) {
          str += "\n";
        }
        str += base;
      }
    }
    // Add a newline at the end.
    str += "\n";
    return str;
  }

  std::string RepeatRI(void (Ass::*f)(Reg, const Imm&), size_t imm_bytes, std::string fmt) {
    const std::vector<Reg*> registers = GetRegisters();
    std::string str;
    std::vector<int64_t> imms = CreateImmediateValues(imm_bytes);
    for (auto reg : registers) {
      for (int64_t imm : imms) {
        Imm* new_imm = CreateImmediate(imm);
        (assembler_.get()->*f)(*reg, *new_imm);
        delete new_imm;
        std::string base = fmt;

        size_t reg_index = base.find("{reg}");
        if (reg_index != std::string::npos) {
          std::ostringstream sreg;
          sreg << *reg;
          std::string reg_string = sreg.str();
          base.replace(reg_index, 5, reg_string);
        }

        size_t imm_index = base.find("{imm}");
        if (imm_index != std::string::npos) {
          std::ostringstream sreg;
          sreg << imm;
          std::string imm_string = sreg.str();
          base.replace(imm_index, 5, imm_string);
        }

        if (str.size() > 0) {
          str += "\n";
        }
        str += base;
      }
    }
    // Add a newline at the end.
    str += "\n";
    return str;
  }

  std::string RepeatI(void (Ass::*f)(const Imm&), size_t imm_bytes, std::string fmt) {
    std::string str;
    std::vector<int64_t> imms = CreateImmediateValues(imm_bytes);
    for (int64_t imm : imms) {
      Imm* new_imm = CreateImmediate(imm);
      (assembler_.get()->*f)(*new_imm);
      delete new_imm;
      std::string base = fmt;

      size_t imm_index = base.find("{imm}");
      if (imm_index != std::string::npos) {
        std::ostringstream sreg;
        sreg << imm;
        std::string imm_string = sreg.str();
        base.replace(imm_index, 5, imm_string);
      }

      if (str.size() > 0) {
        str += "\n";
      }
      str += base;
    }
    // Add a newline at the end.
    str += "\n";
    return str;
  }

  // This is intended to be run as a test.
  bool CheckTools() {
    if (!FileExists(GetAssemblerCommand())) {
      return false;
    }
    LOG(INFO) << "Chosen assembler command: " << GetAssemblerCommand();

    if (!FileExists(GetObjdumpCommand())) {
      return false;
    }
    LOG(INFO) << "Chosen objdump command: " << GetObjdumpCommand();

    // Disassembly is optional.
    std::string disassembler = GetDisassembleCommand();
    if (disassembler.length() != 0) {
      if (!FileExists(disassembler)) {
        return false;
      }
      LOG(INFO) << "Chosen disassemble command: " << GetDisassembleCommand();
    } else {
      LOG(INFO) << "No disassembler given.";
    }

    return true;
  }

 protected:
  void SetUp() OVERRIDE {
    assembler_.reset(new Ass());

    // Fake a runtime test for ScratchFile
    CommonRuntimeTest::SetUpAndroidData(android_data_);

    SetUpHelpers();
  }

  void TearDown() OVERRIDE {
    // We leave temporaries in case this failed so we can debug issues.
    CommonRuntimeTest::TearDownAndroidData(android_data_, false);
    tmpnam_ = "";
  }

  // Override this to set up any architecture-specific things, e.g., register vectors.
  virtual void SetUpHelpers() {}

  virtual std::vector<Reg*> GetRegisters() = 0;

  // Get the typically used name for this architecture, e.g., aarch64, x86_64, ...
  virtual std::string GetArchitectureString() = 0;

  // Get the name of the assembler, e.g., "as" by default.
  virtual std::string GetAssemblerCmdName() {
    return "as";
  }

  // Switches to the assembler command. Default none.
  virtual std::string GetAssemblerParameters() {
    return "";
  }

  // Return the host assembler command for this test.
  virtual std::string GetAssemblerCommand() {
    // Already resolved it once?
    if (resolved_assembler_cmd_.length() != 0) {
      return resolved_assembler_cmd_;
    }

    std::string line = FindTool(GetAssemblerCmdName());
    if (line.length() == 0) {
      return line;
    }

    resolved_assembler_cmd_ = line + GetAssemblerParameters();

    return line;
  }

  // Get the name of the objdump, e.g., "objdump" by default.
  virtual std::string GetObjdumpCmdName() {
    return "objdump";
  }

  // Switches to the objdump command. Default is " -h".
  virtual std::string GetObjdumpParameters() {
    return " -h";
  }

  // Return the host objdump command for this test.
  virtual std::string GetObjdumpCommand() {
    // Already resolved it once?
    if (resolved_objdump_cmd_.length() != 0) {
      return resolved_objdump_cmd_;
    }

    std::string line = FindTool(GetObjdumpCmdName());
    if (line.length() == 0) {
      return line;
    }

    resolved_objdump_cmd_ = line + GetObjdumpParameters();

    return line;
  }

  // Get the name of the objdump, e.g., "objdump" by default.
  virtual std::string GetDisassembleCmdName() {
    return "objdump";
  }

  // Switches to the objdump command. As it's a binary, one needs to push the architecture and
  // such to objdump, so it's architecture-specific and there is no default.
  virtual std::string GetDisassembleParameters() = 0;

  // Return the host disassembler command for this test.
  virtual std::string GetDisassembleCommand() {
    // Already resolved it once?
    if (resolved_disassemble_cmd_.length() != 0) {
      return resolved_disassemble_cmd_;
    }

    std::string line = FindTool(GetDisassembleCmdName());
    if (line.length() == 0) {
      return line;
    }

    resolved_disassemble_cmd_ = line + GetDisassembleParameters();

    return line;
  }

  // Create a couple of immediate values up to the number of bytes given.
  virtual std::vector<int64_t> CreateImmediateValues(size_t imm_bytes) {
    std::vector<int64_t> res;
    res.push_back(0);
    res.push_back(-1);
    res.push_back(0x12);
    if (imm_bytes >= 2) {
      res.push_back(0x1234);
      res.push_back(-0x1234);
      if (imm_bytes >= 4) {
        res.push_back(0x12345678);
        res.push_back(-0x12345678);
        if (imm_bytes >= 6) {
          res.push_back(0x123456789ABC);
          res.push_back(-0x123456789ABC);
          if (imm_bytes >= 8) {
            res.push_back(0x123456789ABCDEF0);
            res.push_back(-0x123456789ABCDEF0);
          }
        }
      }
    }
    return res;
  }

  // Create an immediate from the specific value.
  virtual Imm* CreateImmediate(int64_t imm_value) = 0;

 private:
  // Driver() assembles and compares the results. If the results are not equal and we have a
  // disassembler, disassemble both and check whether they have the same mnemonics (in which case
  // we just warn).
  void Driver(std::string assembly_text, std::string test_name) {
    EXPECT_NE(assembly_text.length(), 0U) << "Empty assembly";

    NativeAssemblerResult res;
    Compile(assembly_text, &res, test_name);

    EXPECT_TRUE(res.ok) << res.error_msg;
    if (!res.ok) {
      // No way of continuing.
      return;
    }

    size_t cs = assembler_->CodeSize();
    std::unique_ptr<std::vector<uint8_t>> data(new std::vector<uint8_t>(cs));
    MemoryRegion code(&(*data)[0], data->size());
    assembler_->FinalizeInstructions(code);

    if (*data == *res.code) {
      Clean(&res);
    } else {
      if (DisassembleBinaries(*data, *res.code, test_name)) {
        if (data->size() > res.code->size()) {
          // Fail this test with a fancy colored warning being printed.
          EXPECT_TRUE(false) << "Assembly code is not identical, but disassembly of machine code "
              "is equal: this implies sub-optimal encoding! Our code size=" << data->size() <<
              ", gcc size=" << res.code->size();
        } else {
          // Otherwise just print an info message and clean up.
          LOG(INFO) << "GCC chose a different encoding than ours, but the overall length is the "
              "same.";
          Clean(&res);
        }
      } else {
        // This will output the assembly.
        EXPECT_EQ(*data, *res.code) << "Outputs (and disassembly) not identical.";
      }
    }
  }

  // Structure to store intermediates and results.
  struct NativeAssemblerResult {
    bool ok;
    std::string error_msg;
    std::string base_name;
    std::unique_ptr<std::vector<uint8_t>> code;
    uintptr_t length;
  };

  // Compile the assembly file from_file to a binary file to_file. Returns true on success.
  bool Assemble(const char* from_file, const char* to_file, std::string* error_msg) {
    bool have_assembler = FileExists(GetAssemblerCommand());
    EXPECT_TRUE(have_assembler) << "Cannot find assembler:" << GetAssemblerCommand();
    if (!have_assembler) {
      return false;
    }

    std::vector<std::string> args;

    args.push_back(GetAssemblerCommand());
    args.push_back("-o");
    args.push_back(to_file);
    args.push_back(from_file);

    return Exec(args, error_msg);
  }

  // Runs objdump -h on the binary file and extracts the first line with .text.
  // Returns "" on failure.
  std::string Objdump(std::string file) {
    bool have_objdump = FileExists(GetObjdumpCommand());
    EXPECT_TRUE(have_objdump) << "Cannot find objdump: " << GetObjdumpCommand();
    if (!have_objdump) {
      return "";
    }

    std::string error_msg;
    std::vector<std::string> args;

    args.push_back(GetObjdumpCommand());
    args.push_back(file);
    args.push_back(">");
    args.push_back(file+".dump");
    std::string cmd = Join(args, ' ');

    args.clear();
    args.push_back("/bin/sh");
    args.push_back("-c");
    args.push_back(cmd);

    if (!Exec(args, &error_msg)) {
      EXPECT_TRUE(false) << error_msg;
    }

    std::ifstream dump(file+".dump");

    std::string line;
    bool found = false;
    while (std::getline(dump, line)) {
      if (line.find(".text") != line.npos) {
        found = true;
        break;
      }
    }

    dump.close();

    if (found) {
      return line;
    } else {
      return "";
    }
  }

  // Disassemble both binaries and compare the text.
  bool DisassembleBinaries(std::vector<uint8_t>& data, std::vector<uint8_t>& as,
                           std::string test_name) {
    std::string disassembler = GetDisassembleCommand();
    if (disassembler.length() == 0) {
      LOG(WARNING) << "No dissassembler command.";
      return false;
    }

    std::string data_name = WriteToFile(data, test_name + ".ass");
    std::string error_msg;
    if (!DisassembleBinary(data_name, &error_msg)) {
      LOG(INFO) << "Error disassembling: " << error_msg;
      std::remove(data_name.c_str());
      return false;
    }

    std::string as_name = WriteToFile(as, test_name + ".gcc");
    if (!DisassembleBinary(as_name, &error_msg)) {
      LOG(INFO) << "Error disassembling: " << error_msg;
      std::remove(data_name.c_str());
      std::remove((data_name + ".dis").c_str());
      std::remove(as_name.c_str());
      return false;
    }

    bool result = CompareFiles(data_name + ".dis", as_name + ".dis");

    if (result) {
      std::remove(data_name.c_str());
      std::remove(as_name.c_str());
      std::remove((data_name + ".dis").c_str());
      std::remove((as_name + ".dis").c_str());
    }

    return result;
  }

  bool DisassembleBinary(std::string file, std::string* error_msg) {
    std::vector<std::string> args;

    args.push_back(GetDisassembleCommand());
    args.push_back(file);
    args.push_back("| sed -n \'/<.data>/,$p\' | sed -e \'s/.*://\'");
    args.push_back(">");
    args.push_back(file+".dis");
    std::string cmd = Join(args, ' ');

    args.clear();
    args.push_back("/bin/sh");
    args.push_back("-c");
    args.push_back(cmd);

    return Exec(args, error_msg);
  }

  std::string WriteToFile(std::vector<uint8_t>& buffer, std::string test_name) {
    std::string file_name = GetTmpnam() + std::string("---") + test_name;
    const char* data = reinterpret_cast<char*>(buffer.data());
    std::ofstream s_out(file_name + ".o");
    s_out.write(data, buffer.size());
    s_out.close();
    return file_name + ".o";
  }

  bool CompareFiles(std::string f1, std::string f2) {
    std::ifstream f1_in(f1);
    std::ifstream f2_in(f2);

    bool result = std::equal(std::istreambuf_iterator<char>(f1_in),
                             std::istreambuf_iterator<char>(),
                             std::istreambuf_iterator<char>(f2_in));

    f1_in.close();
    f2_in.close();

    return result;
  }

  // Compile the given assembly code and extract the binary, if possible. Put result into res.
  bool Compile(std::string assembly_code, NativeAssemblerResult* res, std::string test_name) {
    res->ok = false;
    res->code.reset(nullptr);

    res->base_name = GetTmpnam() + std::string("---") + test_name;

    // TODO: Lots of error checking.

    std::ofstream s_out(res->base_name + ".S");
    s_out << assembly_code;
    s_out.close();

    if (!Assemble((res->base_name + ".S").c_str(), (res->base_name + ".o").c_str(),
                  &res->error_msg)) {
      res->error_msg = "Could not compile.";
      return false;
    }

    std::string odump = Objdump(res->base_name + ".o");
    if (odump.length() == 0) {
      res->error_msg = "Objdump failed.";
      return false;
    }

    std::istringstream iss(odump);
    std::istream_iterator<std::string> start(iss);
    std::istream_iterator<std::string> end;
    std::vector<std::string> tokens(start, end);

    if (tokens.size() < OBJDUMP_SECTION_LINE_MIN_TOKENS) {
      res->error_msg = "Objdump output not recognized: too few tokens.";
      return false;
    }

    if (tokens[1] != ".text") {
      res->error_msg = "Objdump output not recognized: .text not second token.";
      return false;
    }

    std::string lengthToken = "0x" + tokens[2];
    std::istringstream(lengthToken) >> std::hex >> res->length;

    std::string offsetToken = "0x" + tokens[5];
    uintptr_t offset;
    std::istringstream(offsetToken) >> std::hex >> offset;

    std::ifstream obj(res->base_name + ".o");
    obj.seekg(offset);
    res->code.reset(new std::vector<uint8_t>(res->length));
    obj.read(reinterpret_cast<char*>(&(*res->code)[0]), res->length);
    obj.close();

    res->ok = true;
    return true;
  }

  // Remove temporary files.
  void Clean(const NativeAssemblerResult* res) {
    std::remove((res->base_name + ".S").c_str());
    std::remove((res->base_name + ".o").c_str());
    std::remove((res->base_name + ".o.dump").c_str());
  }

  // Check whether file exists. Is used for commands, so strips off any parameters: anything after
  // the first space. We skip to the last slash for this, so it should work with directories with
  // spaces.
  static bool FileExists(std::string file) {
    if (file.length() == 0) {
      return false;
    }

    // Need to strip any options.
    size_t last_slash = file.find_last_of('/');
    if (last_slash == std::string::npos) {
      // No slash, start looking at the start.
      last_slash = 0;
    }
    size_t space_index = file.find(' ', last_slash);

    if (space_index == std::string::npos) {
      std::ifstream infile(file.c_str());
      return infile.good();
    } else {
      std::string copy = file.substr(0, space_index - 1);

      struct stat buf;
      return stat(copy.c_str(), &buf) == 0;
    }
  }

  static std::string GetGCCRootPath() {
    return "prebuilts/gcc/linux-x86";
  }

  static std::string GetRootPath() {
    // 1) Check ANDROID_BUILD_TOP
    char* build_top = getenv("ANDROID_BUILD_TOP");
    if (build_top != nullptr) {
      return std::string(build_top) + "/";
    }

    // 2) Do cwd
    char temp[1024];
    return getcwd(temp, 1024) ? std::string(temp) + "/" : std::string("");
  }

  std::string FindTool(std::string tool_name) {
    // Find the current tool. Wild-card pattern is "arch-string*tool-name".
    std::string gcc_path = GetRootPath() + GetGCCRootPath();
    std::vector<std::string> args;
    args.push_back("find");
    args.push_back(gcc_path);
    args.push_back("-name");
    args.push_back(GetArchitectureString() + "*" + tool_name);
    args.push_back("|");
    args.push_back("sort");
    args.push_back("|");
    args.push_back("tail");
    args.push_back("-n");
    args.push_back("1");
    std::string tmp_file = GetTmpnam();
    args.push_back(">");
    args.push_back(tmp_file);
    std::string sh_args = Join(args, ' ');

    args.clear();
    args.push_back("/bin/sh");
    args.push_back("-c");
    args.push_back(sh_args);

    std::string error_msg;
    if (!Exec(args, &error_msg)) {
      EXPECT_TRUE(false) << error_msg;
      return "";
    }

    std::ifstream in(tmp_file.c_str());
    std::string line;
    if (!std::getline(in, line)) {
      in.close();
      std::remove(tmp_file.c_str());
      return "";
    }
    in.close();
    std::remove(tmp_file.c_str());
    return line;
  }

  // Use a consistent tmpnam, so store it.
  std::string GetTmpnam() {
    if (tmpnam_.length() == 0) {
      ScratchFile tmp;
      tmpnam_ = tmp.GetFilename() + "asm";
    }
    return tmpnam_;
  }

  std::unique_ptr<Ass> assembler_;

  std::string resolved_assembler_cmd_;
  std::string resolved_objdump_cmd_;
  std::string resolved_disassemble_cmd_;

  std::string android_data_;

  static constexpr size_t OBJDUMP_SECTION_LINE_MIN_TOKENS = 6;
};

}  // namespace art

#endif  // ART_COMPILER_UTILS_ASSEMBLER_TEST_H_
