;;
;; Copyright (C) 2012 The Android Open Source Project
;;
;; Licensed under the Apache License, Version 2.0 (the "License");
;; you may not use this file except in compliance with the License.
;; You may obtain a copy of the License at
;;
;;      http://www.apache.org/licenses/LICENSE-2.0
;;
;; Unless required by applicable law or agreed to in writing, software
;; distributed under the License is distributed on an "AS IS" BASIS,
;; WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
;; See the License for the specific language governing permissions and
;; limitations under the License.
;;


;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Type
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

%JavaObject = type opaque

%ShadowFrame = type { %ShadowFrame*        ; Previous frame
                    , %JavaObject*         ; Method object pointer
                    , i32                  ; Line number for stack backtrace
                    , i32                  ; Number of references
                    ; [0 x %JavaObject*]   ; References
                    }

declare void @__art_type_list(%JavaObject*, %ShadowFrame*)


;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Thread
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

declare %JavaObject* @art_get_current_thread_from_code()
declare void @art_set_current_thread_from_code(%JavaObject*)

declare void @art_lock_object_from_code(%JavaObject*)
declare void @art_unlock_object_from_code(%JavaObject*)

declare void @art_test_suspend_from_code()

declare void @art_push_shadow_frame_from_code(%ShadowFrame*)
declare void @art_pop_shadow_frame_from_code()



;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Exception
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

declare i1 @art_is_exception_pending_from_code()

declare void @art_throw_div_zero_from_code()
declare void @art_throw_array_bounds_from_code(i32, i32)
declare void @art_throw_no_such_method_from_code(i32)
declare void @art_throw_null_pointer_exception_from_code(i32)
declare void @art_throw_stack_overflow_from_code()
declare void @art_throw_exception_from_code(%JavaObject*)
declare void @art_throw_verification_error_from_code(%JavaObject*, i32, i32)

declare i32 @art_find_catch_block_from_code(%JavaObject*, i32)



;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Object Space
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

declare %JavaObject* @art_alloc_object_from_code(i32, %JavaObject*)
declare %JavaObject* @art_alloc_object_from_code_with_access_check(
  i32, %JavaObject*)

declare %JavaObject* @art_alloc_array_from_code(i32, %JavaObject*, i32)
declare %JavaObject* @art_alloc_array_from_code_with_access_check(
  i32, %JavaObject*, i32)
declare %JavaObject* @art_check_and_alloc_array_from_code(
  i32, %JavaObject*, i32)
declare %JavaObject* @art_check_and_alloc_array_from_code_with_access_check(
  i32, %JavaObject*, i32)

declare void @art_find_instance_field_from_code(i32, %JavaObject*)
declare void @art_find_static_field_from_code(i32, %JavaObject*)

declare %JavaObject* @art_find_static_method_from_code_with_access_check(
  i32, %JavaObject*, %JavaObject*)
declare %JavaObject* @art_find_direct_method_from_code_with_access_check(
  i32, %JavaObject*, %JavaObject*)
declare %JavaObject* @art_find_virtual_method_from_code_with_access_check(
  i32, %JavaObject*, %JavaObject*)
declare %JavaObject* @art_find_super_method_from_code_with_access_check(
  i32, %JavaObject*, %JavaObject*)
declare %JavaObject* @art_find_interface_method_from_code_with_access_check(
  i32, %JavaObject*, %JavaObject*)
declare %JavaObject* @art_find_interface_method_from_code(
  i32, %JavaObject*, %JavaObject*)

declare %JavaObject* @art_initialize_static_storage_from_code(i32, %JavaObject*)
declare %JavaObject* @art_initialize_type_from_code(i32, %JavaObject*)
declare %JavaObject* @art_initialize_type_and_verify_access_from_code(
  i32, %JavaObject*)

declare %JavaObject* @art_resolve_string_from_code(%JavaObject*, i32)

declare i32 @art_set32_static_from_code(i32, %JavaObject*, i32)
declare i32 @art_set64_static_from_code(i32, %JavaObject*, i64)
declare i32 @art_set_obj_static_from_code(i32, %JavaObject*, %JavaObject*)

declare i32 @art_get32_static_from_code(i32, %JavaObject*)
declare i64 @art_get64_static_from_code(i32, %JavaObject*)
declare %JavaObject* @art_get_obj_static_from_code(i32, %JavaObject*)

declare i32 @art_set32_instance_from_code(i32,
                                          %JavaObject*,
                                          %JavaObject*,
                                          i32)

declare i32 @art_set64_instance_from_code(i32,
                                          %JavaObject*,
                                          %JavaObject*,
                                          i64)

declare i32 @art_set_obj_instance_from_code(i32,
                                            %JavaObject*,
                                            %JavaObject*,
                                            %JavaObject*)

declare i32 @art_get32_instance_from_code(i32,
                                          %JavaObject*,
                                          %JavaObject*)

declare i64 @art_get64_instance_from_code(i32,
                                          %JavaObject*,
                                          %JavaObject*)

declare %JavaObject* @art_get_obj_instance_from_code(i32,
                                                     %JavaObject*,
                                                     %JavaObject*)

declare %JavaObject* @art_decode_jobject_in_thread(%JavaObject*,
                                                   %JavaObject*)


;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Type Checking, in the nature of casting
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

declare i32 @art_is_assignable_from_code(%JavaObject*, %JavaObject*)
declare void @art_check_cast_from_code(%JavaObject*, %JavaObject*)
declare void @art_check_put_array_element_from_code(%JavaObject*, %JavaObject*)

declare %JavaObject* @art_ensure_resolved_from_code(%JavaObject*,
                                                    %JavaObject*,
                                                    i32,
                                                    i1)

declare %JavaObject* @art_fix_stub_from_code(%JavaObject*)
