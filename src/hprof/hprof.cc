/*
 * Copyright (C) 2008 The Android Open Source Project
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

/*
 * Preparation and completion of hprof data generation.  The output is
 * written into two files and then combined.  This is necessary because
 * we generate some of the data (strings and classes) while we dump the
 * heap, and some analysis tools require that the class and string data
 * appear first.
 */

#include "hprof.h"
#include "heap.h"
#include "debugger.h"
#include "stringprintf.h"
#include "thread_list.h"
#include "logging.h"

#include <sys/uio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/time.h>
#include <time.h>

namespace art {

namespace hprof {

#define kHeadSuffix "-hptemp"

// TODO: use File::WriteFully
int sysWriteFully(int fd, const void* buf, size_t count, const char* logMsg)
{
    while (count != 0) {
        ssize_t actual = TEMP_FAILURE_RETRY(write(fd, buf, count));
        if (actual < 0) {
            int err = errno;
            LOG(ERROR) << StringPrintf("%s: write failed: %s", logMsg, strerror(err));
            return err;
        } else if (actual != (ssize_t) count) {
            LOG(DEBUG) << StringPrintf("%s: partial write (will retry): (%d of %zd)",
                logMsg, (int) actual, count);
            buf = (const void*) (((const uint8_t*) buf) + actual);
        }
        count -= actual;
    }

    return 0;
}

/*
 * Finish up the hprof dump.  Returns true on success.
 */
bool Hprof::Finish()
{
    /* flush the "tail" portion of the output */
    StartNewRecord(HPROF_TAG_HEAP_DUMP_END, HPROF_TIME);
    FlushCurrentRecord();

    // Create a new Hprof for the start of the file (as opposed to this, which is the tail).
    Hprof headCtx(file_name_, fd_, true, direct_to_ddms_);
    headCtx.classes_ = classes_;
    headCtx.strings_ = strings_;

    LOG(INFO) << StringPrintf("hprof: dumping heap strings to \"%s\".", file_name_);
    headCtx.DumpStrings();
    headCtx.DumpClasses();

    /* Write a dummy stack trace record so the analysis
     * tools don't freak out.
     */
    headCtx.StartNewRecord(HPROF_TAG_STACK_TRACE, HPROF_TIME);
    headCtx.current_record_.AddU4(HPROF_NULL_STACK_TRACE);
    headCtx.current_record_.AddU4(HPROF_NULL_THREAD);
    headCtx.current_record_.AddU4(0);    // no frames

    headCtx.FlushCurrentRecord();

    /* flush to ensure memstream pointer and size are updated */
    fflush(headCtx.mem_fp_);
    fflush(mem_fp_);

    if (direct_to_ddms_) {
        /* send the data off to DDMS */
        struct iovec iov[2];
        iov[0].iov_base = headCtx.file_data_ptr_;
        iov[0].iov_len = headCtx.file_data_size_;
        iov[1].iov_base = file_data_ptr_;
        iov[1].iov_len = file_data_size_;
        Dbg::DdmSendChunkV(CHUNK_TYPE("HPDS"), iov, 2);
    } else {
        /*
         * Open the output file, and copy the head and tail to it.
         */
        CHECK_EQ(headCtx.fd_, fd_);

        int outFd;
        if (headCtx.fd_ >= 0) {
            outFd = dup(headCtx.fd_);
            if (outFd < 0) {
                LOG(ERROR) << StringPrintf("dup(%d) failed: %s", headCtx.fd_, strerror(errno));
                /* continue to fail-handler below */
            }
        } else {
            outFd = open(file_name_, O_WRONLY|O_CREAT|O_TRUNC, 0644);
            if (outFd < 0) {
                LOG(ERROR) << StringPrintf("can't open %s: %s", headCtx.file_name_, strerror(errno));
                /* continue to fail-handler below */
            }
        }
        if (outFd < 0) {
            return false;
        }

        int result = sysWriteFully(outFd, headCtx.file_data_ptr_,
            headCtx.file_data_size_, "hprof-head");
        result |= sysWriteFully(outFd, file_data_ptr_, file_data_size_, "hprof-tail");
        close(outFd);
        if (result != 0) {
            return false;
        }
    }

    /* throw out a log message for the benefit of "runhat" */
    LOG(INFO) << StringPrintf("hprof: heap dump completed (%dKB)",
        (headCtx.file_data_size_ + file_data_size_ + 1023) / 1024);

    return true;
}

Hprof::~Hprof() {
    /* we don't own ctx->fd_, do not close */

    if (mem_fp_ != NULL) {
        fclose(mem_fp_);
    }
    free(current_record_.body_);
    free(file_name_);
    free(file_data_ptr_);
}

/*
 * Visitor invoked on every root reference.
 */
void HprofRootVisitor(const Object* obj, void* arg) {
    CHECK(arg != NULL);
    Hprof* hprof = (Hprof*)arg;
    hprof->VisitRoot(obj);
}

void Hprof::VisitRoot(const Object* obj) {
    uint32_t threadId = 0;  // TODO
    /*RootType */ size_t type = 0; // TODO

    static const HprofHeapTag xlate[] = {
        HPROF_ROOT_UNKNOWN,
        HPROF_ROOT_JNI_GLOBAL,
        HPROF_ROOT_JNI_LOCAL,
        HPROF_ROOT_JAVA_FRAME,
        HPROF_ROOT_NATIVE_STACK,
        HPROF_ROOT_STICKY_CLASS,
        HPROF_ROOT_THREAD_BLOCK,
        HPROF_ROOT_MONITOR_USED,
        HPROF_ROOT_THREAD_OBJECT,
        HPROF_ROOT_INTERNED_STRING,
        HPROF_ROOT_FINALIZING,
        HPROF_ROOT_DEBUGGER,
        HPROF_ROOT_REFERENCE_CLEANUP,
        HPROF_ROOT_VM_INTERNAL,
        HPROF_ROOT_JNI_MONITOR,
    };

    CHECK_LT(type, sizeof(xlate) / sizeof(HprofHeapTag));
    if (obj == NULL) {
        return;
    }
    gc_scan_state_ = xlate[type];
    gc_thread_serial_number_ = threadId;
    MarkRootObject(obj, 0);
    gc_scan_state_ = 0;
    gc_thread_serial_number_ = 0;
}

/*
 * Visitor invoked on every heap object.
 */

static void HprofBitmapCallback(Object *obj, void *arg)
{
    CHECK(obj != NULL);
    CHECK(arg != NULL);
    Hprof *hprof = (Hprof*)arg;
    hprof->DumpHeapObject(obj);
}

/*
 * Walk the roots and heap writing heap information to the specified
 * file.
 *
 * If "fd" is >= 0, the output will be written to that file descriptor.
 * Otherwise, "file_name_" is used to create an output file.
 *
 * If "direct_to_ddms_" is set, the other arguments are ignored, and data is
 * sent directly to DDMS.
 *
 * Returns 0 on success, or an error code on failure.
 */
int DumpHeap(const char* fileName, int fd, bool directToDdms)
{
    CHECK(fileName != NULL);
    ScopedHeapLock lock;
    ScopedThreadStateChange tsc(Thread::Current(), Thread::kRunnable);

    ThreadList* thread_list = Runtime::Current()->GetThreadList();
    thread_list->SuspendAll();

    Hprof hprof(fileName, fd, false, directToDdms);
    Runtime::Current()->VisitRoots(HprofRootVisitor, &hprof);
    Heap::GetLiveBits()->Walk(HprofBitmapCallback, &hprof);
//TODO: write a HEAP_SUMMARY record
    int success = hprof.Finish() ? 0 : -1;
    thread_list->ResumeAll();
    return success;
}

}  // namespace hprof

}  // namespace art
