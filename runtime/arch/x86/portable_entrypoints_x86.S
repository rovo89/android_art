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

#include "asm_support_x86.S"

    /*
     * Portable invocation stub.
     * On entry:
     *   [sp] = return address
     *   [sp + 4] = method pointer
     *   [sp + 8] = argument array or NULL for no argument methods
     *   [sp + 12] = size of argument array in bytes
     *   [sp + 16] = (managed) thread pointer
     *   [sp + 20] = JValue* result
     *   [sp + 24] = result type char
     */
DEFINE_FUNCTION art_portable_invoke_stub
    PUSH ebp                      // save ebp
    PUSH ebx                      // save ebx
    mov %esp, %ebp                // copy value of stack pointer into base pointer
    .cfi_def_cfa_register ebp
    mov 20(%ebp), %ebx            // get arg array size
    addl LITERAL(28), %ebx        // reserve space for return addr, method*, ebx, and ebp in frame
    andl LITERAL(0xFFFFFFF0), %ebx    // align frame size to 16 bytes
    subl LITERAL(12), %ebx        // remove space for return address, ebx, and ebp
    subl %ebx, %esp               // reserve stack space for argument array
    lea  4(%esp), %eax            // use stack pointer + method ptr as dest for memcpy
    pushl 20(%ebp)                // push size of region to memcpy
    pushl 16(%ebp)                // push arg array as source of memcpy
    pushl %eax                    // push stack pointer as destination of memcpy
    call SYMBOL(memcpy)           // (void*, const void*, size_t)
    addl LITERAL(12), %esp        // pop arguments to memcpy
    mov 12(%ebp), %eax            // move method pointer into eax
    mov %eax, (%esp)              // push method pointer onto stack
    call *METHOD_CODE_OFFSET(%eax) // call the method
    mov %ebp, %esp                // restore stack pointer
    POP ebx                       // pop ebx
    POP ebp                       // pop ebp
    mov 20(%esp), %ecx            // get result pointer
    cmpl LITERAL(68), 24(%esp)    // test if result type char == 'D'
    je return_double_portable
    cmpl LITERAL(70), 24(%esp)    // test if result type char == 'F'
    je return_float_portable
    mov %eax, (%ecx)              // store the result
    mov %edx, 4(%ecx)             // store the other half of the result
    ret
return_double_portable:
    fstpl (%ecx)                  // store the floating point result as double
    ret
return_float_portable:
    fstps (%ecx)                  // store the floating point result as float
    ret
END_FUNCTION art_portable_invoke_stub

DEFINE_FUNCTION art_portable_proxy_invoke_handler
    // Fake callee save ref and args frame set up, note portable doesn't use callee save frames.
    // TODO: just save the registers that are needed in artPortableProxyInvokeHandler.
    PUSH edi                      // Save callee saves
    PUSH esi
    PUSH ebp
    PUSH ebx                      // Save args
    PUSH edx
    PUSH ecx
    PUSH eax                      // Align stack, eax will be clobbered by Method*
    // Begin argument set up.
    PUSH esp                      // pass SP
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    .cfi_adjust_cfa_offset 4
    PUSH ecx                      // pass receiver
    PUSH eax                      // pass proxy method
    call SYMBOL(artPortableProxyInvokeHandler) // (proxy method, receiver, Thread*, SP)
    movd %eax, %xmm0              // place return value also into floating point return value
    movd %edx, %xmm1
    punpckldq %xmm1, %xmm0
    addl LITERAL(44), %esp        // pop arguments
    .cfi_adjust_cfa_offset -44
    ret
END_FUNCTION art_portable_proxy_invoke_handler

DEFINE_FUNCTION art_portable_resolution_trampoline
  pushl %ebp
  movl %esp, %ebp                 // save ESP
  subl LITERAL(8), %esp           // Align stack
  movl 8(%ebp), %eax              // Method* called
  leal 8(%ebp), %edx              // Method** called_addr
  pushl %edx                      // pass called_addr
  pushl %fs:THREAD_SELF_OFFSET    // pass thread
  pushl %ecx                      // pass receiver
  pushl %eax                      // pass called
  call SYMBOL(artPortableResolutionTrampoline)
  leave
  cmpl LITERAL(0), %eax
  je resolve_fail
  jmp * %eax
resolve_fail:
  ret
END_FUNCTION art_portable_resolution_trampoline

DEFINE_FUNCTION art_portable_to_interpreter_bridge
  pushl %ebp
  movl %esp, %ebp                 // save ESP
  subl LITERAL(12), %esp          // Align stack
  movl 8(%ebp), %eax              // Method* called
  leal 8(%ebp), %edx              // Method** called_addr
  pushl %edx                      // pass called_addr
  pushl %fs:THREAD_SELF_OFFSET    // pass thread
  pushl %eax                      // pass called
  call SYMBOL(artPortableToInterpreterBridge)
  leave
  ret
END_FUNCTION art_quick_to_interpreter_bridge
