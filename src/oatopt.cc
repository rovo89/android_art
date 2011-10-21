// Copyright 2011 Google Inc. All Rights Reserved.

#include <stdio.h>
#include <stdlib.h>

#include "dex_file.h"
#include "file.h"
#include "logging.h"
#include "oat_file.h"
#include "os.h"
#include "UniquePtr.h"
#include "zip_archive.h"

namespace art {

int ProcessZipFile(int zip_fd, int cache_fd, const char* zip_name, const char *flags) {
  // TODO: need to read/write to installd opened file descriptors
  if (false) {
    UniquePtr<ZipArchive> zip_archive(ZipArchive::Open(zip_fd));
    if (zip_archive.get() == NULL) {
      LOG(ERROR) << "Failed to open " << zip_name << " when looking for classes.dex";
      return -1;
    }

    UniquePtr<ZipEntry> zip_entry(zip_archive->Find(DexFile::kClassesDex));
    if (zip_entry.get() == NULL) {
      LOG(ERROR) << "Failed to find classes.dex within " << zip_name;
      return -1;
    }

    UniquePtr<File> file(OS::FileFromFd("oatopt cache file descriptor", cache_fd));
    bool success = zip_entry->Extract(*file);
    if (!success) {
      LOG(ERROR) << "Failed to extract classes.dex from " << zip_name;
      return -1;
    }
  }

  // Opening a zip file for a dex will extract to art-cache
  UniquePtr<const DexFile> dex_file(DexFile::Open(zip_name, ""));
  if (dex_file.get() == NULL) {
    LOG(ERROR) << "Failed to open " << zip_name;
    return -1;
  }

  std::string dex_file_option("--dex-file=");
  dex_file_option += zip_name;

  std::string oat_file_option("--oat=");
  oat_file_option += GetArtCacheFilenameOrDie(OatFile::DexFilenameToOatFilename(dex_file.get()->GetLocation()));

  execl("/system/bin/dex2oatd",
        "/system/bin/dex2oatd",
        "-Xms64m",
        "-Xmx64m",
        "--boot-image=/data/art-cache/boot.art",
        dex_file_option.c_str(),
        oat_file_option.c_str(),
        NULL);
  PLOG(FATAL) << "execl(dex2oatd) failed";
  return -1;
}

// Parse arguments.  We want:
//   0. (name of command -- ignored)
//   1. "--zip"
//   2. zip fd (input, read-only)
//   3. cache fd (output, read-write, locked with flock)
//   4. filename of zipfile
//   5. flags
int FromZip(const int argc, const char* const argv[]) {
  if (argc != 6) {
    LOG(ERROR) << "Wrong number of args for --zip (found " << argc << ")";
    return -1;
  }

  // ignore program name

  // verify --zip
  CHECK_STREQ(argv[1], "--zip");

  char* zip_end;
  int zip_fd = strtol(argv[2], &zip_end, 0);
  if (*zip_end != '\0') {
    LOG(ERROR) << "bad zip fd: " << argv[2];
    return -1;
  }
#ifndef NDEBUG
  LOG(INFO) << "zip_fd=" << zip_fd;
#endif

  char* cache_end;
  int cache_fd = strtol(argv[3], &cache_end, 0);
  if (*cache_end != '\0') {
    LOG(ERROR) << "bad cache fd: " << argv[3];
    return -1;
  }
#ifndef NDEBUG
  LOG(INFO) << "cache_fd=" << cache_fd;
#endif

  const char* zip_name = argv[4];
#ifndef NDEBUG
  LOG(INFO) << "zip_name=" << zip_name;
#endif

  const char* flags = argv[5];
#ifndef NDEBUG
  LOG(INFO) << "flags=" << flags;
#endif

  return ProcessZipFile(zip_fd, cache_fd, zip_name, flags);
}

int oatopt(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);

  if (true) {
    for (int i = 0; i < argc; ++i) {
      LOG(INFO) << "oatopt: option[" << i << "]=" << argv[i];
    }
  }

  if (argc > 1) {
    if (strcmp(argv[1], "--zip") == 0) {
      return FromZip(argc, argv);
    }
  }

  fprintf(stderr,
          "Usage:\n\n"
          "Short version: Don't use this.\n\n"
          "Slightly longer version: This system-internal tool is used to extract\n"
          "dex files and produce oat files. See the source code for details.\n");

  return 1;
}

} // namespace art

int main(int argc, char** argv) {
  return art::oatopt(argc, argv);
}
