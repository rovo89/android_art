/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include <android/librsloader.h>
#include <cutils/log.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>

#include "utils_llvm.h"

#include "class_loader.h"
#include "object.h"
#include "object_utils.h"

namespace art {

std::string MangleForLLVM(const std::string& s) {
  std::string result;
  size_t char_count = CountModifiedUtf8Chars(s.c_str());
  const char* cp = &s[0];
  for (size_t i = 0; i < char_count; ++i) {
    uint16_t ch = GetUtf16FromUtf8(&cp);
    if (ch == '$' || ch == '<' || ch == '>' || ch > 127) {
      StringAppendF(&result, "_0%04x", ch);
    } else {
      switch (ch) {
      case '_':
        result += "_1";
        break;
      case ';':
        result += "_2";
        break;
      case '[':
        result += "_3";
        break;
      case '/':
        result += "_";
        break;
      default:
        result.push_back(ch);
        break;
      }
    }
  }
  return result;
}

std::string LLVMShortName(const Method* m) {
  MethodHelper mh(m);
  std::string class_name(mh.GetDeclaringClassDescriptor());
  // Remove the leading 'L' and trailing ';'...
  CHECK_EQ(class_name[0], 'L') << class_name;
  CHECK_EQ(class_name[class_name.size() - 1], ';') << class_name;
  class_name.erase(0, 1);
  class_name.erase(class_name.size() - 1, 1);

  std::string method_name(mh.GetName());

  std::string short_name;
  short_name += "Art_";
  short_name += MangleForLLVM(class_name);
  short_name += "_";
  short_name += MangleForLLVM(method_name);
  return short_name;
}

std::string LLVMLongName(const Method* m) {
  std::string long_name;
  long_name += LLVMShortName(m);
  long_name += "__";

  std::string signature(MethodHelper(m).GetSignature());
  signature.erase(0, 1);
  signature.erase(signature.begin() + signature.find(')'), signature.end());

  long_name += MangleForLLVM(signature);

  return long_name;
}

std::string LLVMStubName(const Method* m) {
  MethodHelper mh(m);
  std::string stub_name;
  if (m->IsStatic()) {
    stub_name += "ArtSUpcall_";
  } else {
    stub_name += "ArtUpcall_";
  }
  stub_name += mh.GetShorty();

  return stub_name;
}

// TODO: Remove these when art_llvm.ll runtime support is ready.
void art_push_shadow_frame_from_code(void* frame) {
}

void art_pop_shadow_frame_from_code() {
}

int art_is_exception_pending_from_code() {
  return 0;
}

void art_test_suspend_from_code() {
}

void art_set_current_thread_from_code(void* thread_object_addr) {
}

// Linker's call back function. Added some for debugging.
void* find_sym(void* context, char const* name) {
  struct func_entry_t {
    char const* name;
    size_t name_len;
    void* addr;
  };

  static struct func_entry_t const tab[] = {
#define DEF(NAME, ADDR) \
    { NAME, sizeof(NAME) - 1, (void *)(&(ADDR)) },

    DEF("art_push_shadow_frame_from_code", art_push_shadow_frame_from_code)
    DEF("art_pop_shadow_frame_from_code", art_pop_shadow_frame_from_code)
    DEF("art_is_exception_pending_from_code", art_is_exception_pending_from_code)
    DEF("art_test_suspend_from_code", art_test_suspend_from_code)
    DEF("art_set_current_thread_from_code", art_set_current_thread_from_code)
    DEF("printf", printf)
    DEF("scanf", scanf)
    DEF("__isoc99_scanf", scanf)
    DEF("rand", rand)
    DEF("time", time)
    DEF("srand", srand)
#undef DEF
  };

  static size_t const tab_size = sizeof(tab) / sizeof(struct func_entry_t);

  // Note: Since our table is small, we are using trivial O(n) searching
  // function.  For bigger table, it will be better to use binary
  // search or hash function.
  size_t i;
  size_t name_len = strlen(name);
  for (i = 0; i < tab_size; ++i) {
    if (name_len == tab[i].name_len && strcmp(name, tab[i].name) == 0) {
      return tab[i].addr;
    }
  }

  CHECK(0) << "Error: Can't find symbol " << name;
  return 0;
}

void LLVMLinkLoadMethod(const std::string& file_name, Method* method) {
  CHECK(method != NULL);

  int fd = open(file_name.c_str(), O_RDONLY);
  CHECK(fd >= 0) << "Error: ELF not found: " << file_name;

  struct stat sb;
  CHECK(fstat(fd, &sb) == 0) << "Error: Unable to stat ELF: " << file_name;

  unsigned char const *image = (unsigned char const *)
      mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  CHECK(image != MAP_FAILED) << "Error: Unable to mmap ELF: " << file_name;

  RSExecRef relocatable = rsloaderCreateExec(image, sb.st_size, find_sym, 0);
  CHECK(relocatable) << "Error: Unable to load ELF: " << file_name;

  const void *addr = rsloaderGetSymbolAddress(relocatable, LLVMLongName(method).c_str());
  CHECK(addr) << "Error: ELF (" << file_name << ") has no symbol " << LLVMLongName(method);
  method->SetCode(reinterpret_cast<const uint32_t*>(addr));

  method->SetFrameSizeInBytes(0);
  method->SetCoreSpillMask(0);
  method->SetFpSpillMask(0);
  method->SetMappingTable(reinterpret_cast<const uint32_t*>(NULL));
  method->SetVmapTable(reinterpret_cast<const uint16_t*>(NULL));
  method->SetGcMap(reinterpret_cast<const uint8_t*>(NULL));

  addr = rsloaderGetSymbolAddress(relocatable, LLVMStubName(method).c_str());
  CHECK(addr) << "Error: ELF (" << file_name << ") has no symbol " << LLVMStubName(method);
  method->SetInvokeStub(reinterpret_cast<void (*)(const art::Method*, art::Object*, art::Thread*,
                                                  art::byte*, art::JValue*)>(addr));

  close(fd);
}

}  // namespace art
