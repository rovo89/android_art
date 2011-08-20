// Copyright 2011 Google Inc. All Rights Reserved.

#include "jni_internal.h"

#include "assembler.h"
#include "object.h"

namespace art {


#define __ assembler->

// Creates a function which invokes a managed method with an array of
// arguments.
//
// Immediately after the call, the environment looks like this:
//
// [SP+0 ] = Return address
// [SP+4 ]= method pointer
// [SP+8 ] = receiver pointer or NULL for static methods
// [SP+12] = (managed) thread pointer
// [SP+16] = argument array or NULL for no argument methods
// [SP+20] = JValue* result or NULL for void returns
//
// As the JNI call has already transitioned the thread into the
// "running" state the remaining responsibilities of this routine are
// to save the native registers and set up the managed registers. On
// return, the return value must be store into the result JValue.
void CreateInvokeStub(Assembler* assembler, Method* method) {
  // Size of frame - spill of EDI + Method* + possible receiver + arg array
  size_t frame_size = (2 * kPointerSize) +
                      (method->IsStatic() ? 0 : kPointerSize) +
                      method->NumArgArrayBytes();
  size_t pad_size = RoundUp(frame_size, kStackAlignment) - frame_size;

  __ pushl(EDI);                   // preserve EDI
  __ movl(EDI, Address(ESP, 8));   // EDI = method
  __ movl(EAX, Address(ESP, 12));  // EAX = receiver
  __ movl(EDX, Address(ESP, 20));  // EDX = arg array

  // Push padding
  if (pad_size != 0) {
    __ addl(ESP, Immediate(-pad_size));
  }

  // Push/copy arguments
  for (size_t off = method->NumArgArrayBytes(); off > 0; off -= kPointerSize) {
    __ pushl(Address(EDX, off - kPointerSize));
  }
  if (!method->IsStatic()) {
    __ pushl(EAX);
  }
  // Push 0 as NULL Method* thereby terminating managed stack crawls
  __ pushl(Immediate(0));
  __ call(Address(EDI, method->GetCodeOffset()));  // Call code off of method

  // pop arguments and padding up to saved EDI
  __ addl(ESP, Immediate(frame_size + pad_size - kPointerSize));
  char ch = method->GetShorty()[0];
  if (ch != 'V') {
    // Load the result JValue pointer.
    __ movl(EDI, Address(ESP, 24));
    switch(ch) {
      case 'D':
        __ fstpl(Address(EDI, 0));
        break;
      case 'F':
        __ fstps(Address(EDI, 0));
        break;
      case 'J':
        __ movl(Address(EDI, 0), EAX);
        __ movl(Address(EDI, 4), EDX);
        break;
      default:
        __ movl(Address(EDI, 0), EAX);
        break;
    }
  }
  __ popl(EDI); // restore EDI
  __ ret();
}

}  // namespace art
