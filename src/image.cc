// Copyright 2011 Google Inc. All Rights Reserved.

#include "image.h"

namespace art {

const byte ImageHeader::kImageMagic[] = { 'i', 'm', 'g', '\n' };
const byte ImageHeader::kImageVersion[] = { '0', '0', '1', '\0' };

}  // namespace art
