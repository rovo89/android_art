/*
 * Copyright (C) 2013 The Android Open Source Project
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

/* Called by managed code to allocate an object */
TWO_ARG_DOWNCALL art_quick_alloc_object, artAllocObjectFromCode, RETURN_IF_RESULT_IS_NON_ZERO
TWO_ARG_DOWNCALL art_quick_alloc_object_instrumented, artAllocObjectFromCodeInstrumented, RETURN_IF_RESULT_IS_NON_ZERO
/* Called by managed code to allocate an object when the caller doesn't know whether it has access
 * to the created type. */
TWO_ARG_DOWNCALL art_quick_alloc_object_with_access_check, artAllocObjectFromCodeWithAccessCheck, RETURN_IF_RESULT_IS_NON_ZERO
TWO_ARG_DOWNCALL art_quick_alloc_object_with_access_check_instrumented, artAllocObjectFromCodeWithAccessCheckInstrumented, RETURN_IF_RESULT_IS_NON_ZERO
/* Called by managed code to allocate an array. */
THREE_ARG_DOWNCALL art_quick_alloc_array, artAllocArrayFromCode, RETURN_IF_RESULT_IS_NON_ZERO
THREE_ARG_DOWNCALL art_quick_alloc_array_instrumented, artAllocArrayFromCodeInstrumented, RETURN_IF_RESULT_IS_NON_ZERO
/* Called by managed code to allocate an array when the caller doesn't know whether it has access
 * to the created type. */
THREE_ARG_DOWNCALL art_quick_alloc_array_with_access_check, artAllocArrayFromCodeWithAccessCheck, RETURN_IF_RESULT_IS_NON_ZERO
THREE_ARG_DOWNCALL art_quick_alloc_array_with_access_check_instrumented, artAllocArrayFromCodeWithAccessCheckInstrumented, RETURN_IF_RESULT_IS_NON_ZERO
/* Called by managed code to allocate an array in a special case for FILLED_NEW_ARRAY. */
THREE_ARG_DOWNCALL art_quick_check_and_alloc_array, artCheckAndAllocArrayFromCode, RETURN_IF_RESULT_IS_NON_ZERO
THREE_ARG_DOWNCALL art_quick_check_and_alloc_array_instrumented, artCheckAndAllocArrayFromCodeInstrumented, RETURN_IF_RESULT_IS_NON_ZERO
/* Called by managed code to allocate an array in a special case for FILLED_NEW_ARRAY. */
THREE_ARG_DOWNCALL art_quick_check_and_alloc_array_with_access_check, artCheckAndAllocArrayFromCodeWithAccessCheck
THREE_ARG_DOWNCALL art_quick_check_and_alloc_array_with_access_check_instrumented, artCheckAndAllocArrayFromCodeWithAccessCheckInstrumented
