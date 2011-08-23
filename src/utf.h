// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_UTF_H_
#define ART_SRC_UTF_H_

#include <stddef.h>
#include <stdint.h>

namespace art {

/*
 * Returns the number of UTF-16 characters in the given modified UTF-8 string.
 */
size_t CountModifiedUtf8Chars(const char* utf8);

/*
 * Returns the number of modified UTF-8 bytes needed to represent the given
 * UTF-16 string.
 */
size_t CountUtf8Bytes(const uint16_t* chars, size_t char_count);

/*
 * Convert Modified UTF-8 to UTF-16.
 * http://en.wikipedia.org/wiki/UTF-8#Modified_UTF-8
 */
void ConvertModifiedUtf8ToUtf16(uint16_t* utf16_data_out, const char* utf8_data_in);

/*
 * The java.lang.String hashCode() algorithm.
 */
int32_t ComputeUtf16Hash(const uint16_t* chars, size_t char_count);

/*
 * Retrieve the next UTF-16 character from a UTF-8 string.
 *
 * Advances "*utf8_data_in" to the start of the next character.
 *
 * WARNING: If a string is corrupted by dropping a '\0' in the middle
 * of a 3-byte sequence, you can end up overrunning the buffer with
 * reads (and possibly with the writes if the length was computed and
 * cached before the damage). For performance reasons, this function
 * assumes that the string being parsed is known to be valid (e.g., by
 * already being verified). Most strings we process here are coming
 * out of dex files or other internal translations, so the only real
 * risk comes from the JNI NewStringUTF call.
 */
uint16_t GetUtf16FromUtf8(const char** utf8_data_in);

}  // namespace art

#endif  // ART_SRC_UTF_H_
