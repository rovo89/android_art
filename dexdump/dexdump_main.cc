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
 *
 * Main driver of the dexdump utility.
 *
 * This is a re-implementation of the original dexdump utility that was
 * based on Dalvik functions in libdex into a new dexdump that is now
 * based on Art functions in libart instead. The output is identical to
 * the original for correct DEX files. Error messages may differ, however.
 * Also, ODEX files are no longer supported.
 */

#include "dexdump.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "mem_map.h"
#include "runtime.h"

namespace art {

static const char* gProgName = "dexdump";

/*
 * Shows usage.
 */
static void usage(void) {
  fprintf(stderr, "Copyright (C) 2007 The Android Open Source Project\n\n");
  fprintf(stderr, "%s: [-c] [-d] [-e] [-f] [-h] [-i] [-l layout] [-o outfile]"
                  " [-t tempfile] dexfile...\n", gProgName);
  fprintf(stderr, "\n");
  fprintf(stderr, " -c : verify checksum and exit\n");
  fprintf(stderr, " -d : disassemble code sections\n");
  fprintf(stderr, " -e : display exported items only\n");
  fprintf(stderr, " -f : display summary information from file header\n");
  fprintf(stderr, " -g : dump CFG for dex\n");
  fprintf(stderr, " -h : display file header details\n");
  fprintf(stderr, " -i : ignore checksum failures\n");
  fprintf(stderr, " -l : output layout, either 'plain' or 'xml'\n");
  fprintf(stderr, " -o : output file name (defaults to stdout)\n");
  fprintf(stderr, " -t : temp file name (defaults to /sdcard/dex-temp-*)\n");
}

/*
 * Main driver of the dexdump utility.
 */
int dexdumpDriver(int argc, char** argv) {
  // Art specific set up.
  InitLogging(argv);
  MemMap::Init();

  // Reset options.
  bool wantUsage = false;
  memset(&gOptions, 0, sizeof(gOptions));
  gOptions.verbose = true;

  // Parse all arguments.
  while (1) {
    const int ic = getopt(argc, argv, "cdefghil:t:o:");
    if (ic < 0) {
      break;  // done
    }
    switch (ic) {
      case 'c':  // verify the checksum then exit
        gOptions.checksumOnly = true;
        break;
      case 'd':  // disassemble Dalvik instructions
        gOptions.disassemble = true;
        break;
      case 'e':  // exported items only
        gOptions.exportsOnly = true;
        break;
      case 'f':  // dump outer file header
        gOptions.showFileHeaders = true;
        break;
      case 'g':  // dump cfg
        gOptions.cfg = true;
        break;
      case 'h':  // dump section headers, i.e. all meta-data
        gOptions.showSectionHeaders = true;
        break;
      case 'i':  // continue even if checksum is bad
        gOptions.ignoreBadChecksum = true;
        break;
      case 'l':  // layout
        if (strcmp(optarg, "plain") == 0) {
          gOptions.outputFormat = OUTPUT_PLAIN;
        } else if (strcmp(optarg, "xml") == 0) {
          gOptions.outputFormat = OUTPUT_XML;
          gOptions.verbose = false;
        } else {
          wantUsage = true;
        }
        break;
      case 't':  // temp file, used when opening compressed Jar
        gOptions.tempFileName = optarg;
        break;
      case 'o':  // output file
        gOptions.outputFileName = optarg;
        break;
      default:
        wantUsage = true;
        break;
    }  // switch
  }  // while

  // Detect early problems.
  if (optind == argc) {
    fprintf(stderr, "%s: no file specified\n", gProgName);
    wantUsage = true;
  }
  if (gOptions.checksumOnly && gOptions.ignoreBadChecksum) {
    fprintf(stderr, "Can't specify both -c and -i\n");
    wantUsage = true;
  }
  if (wantUsage) {
    usage();
    return 2;
  }

  // Open alternative output file.
  if (gOptions.outputFileName) {
    gOutFile = fopen(gOptions.outputFileName, "w");
    if (!gOutFile) {
      fprintf(stderr, "Can't open %s\n", gOptions.outputFileName);
      return 1;
    }
  }

  // Process all files supplied on command line.
  int result = 0;
  while (optind < argc) {
    result |= processFile(argv[optind++]);
  }  // while
  return result != 0;
}

}  // namespace art

int main(int argc, char** argv) {
  return art::dexdumpDriver(argc, argv);
}
