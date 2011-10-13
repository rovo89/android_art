/*
 * Main entry of app process.
 *
 * Starts the interpreted runtime, then starts up the application.
 *
 */

#define LOG_TAG "appproc"

#include "stringprintf.h"
#include "logging.h"

#include <binder/IPCThreadState.h>
#include <binder/ProcessState.h>
#include <utils/Log.h>
#include <cutils/process_name.h>
#include <cutils/memory.h>
#include <android_runtime/AndroidRuntime.h>

#include <stdio.h>
#include <unistd.h>

namespace android {

void app_usage()
{
    fprintf(stderr,
        "Usage: oat_process [java-options] cmd-dir start-class-name [options]\n");
}

class AppRuntime : public AndroidRuntime
{
public:
    AppRuntime()
        : mParentDir(NULL)
        , mClassName(NULL)
        , mClass(NULL)
        , mArgC(0)
        , mArgV(NULL)
    {
    }

#if 0
    // this appears to be unused
    const char* getParentDir() const
    {
        return mParentDir;
    }
#endif

    const char* getClassName() const
    {
        return mClassName;
    }

    virtual void onVmCreated(JNIEnv* env)
    {
        if (mClassName == NULL) {
            return; // Zygote. Nothing to do here.
        }

        /*
         * This is a little awkward because the JNI FindClass call uses the
         * class loader associated with the native method we're executing in.
         * If called in onStarted (from RuntimeInit.finishInit because we're
         * launching "am", for example), FindClass would see that we're calling
         * from a boot class' native method, and so wouldn't look for the class
         * we're trying to look up in CLASSPATH. Unfortunately it needs to,
         * because the "am" classes are not boot classes.
         *
         * The easiest fix is to call FindClass here, early on before we start
         * executing boot class Java code and thereby deny ourselves access to
         * non-boot classes.
         */
        char* slashClassName = toSlashClassName(mClassName);
        mClass = env->FindClass(slashClassName);
        if (mClass == NULL) {
            LOG(FATAL) << "Could not find class: " << mClassName;
        }
        free(slashClassName);

        mClass = reinterpret_cast<jclass>(env->NewGlobalRef(mClass));
    }

    virtual void onStarted()
    {
        sp<ProcessState> proc = ProcessState::self();
        LOGV("App process: starting thread pool.\n");
        proc->startThreadPool();

        AndroidRuntime* ar = AndroidRuntime::getRuntime();
        ar->callMain(mClassName, mClass, mArgC, mArgV);

        IPCThreadState::self()->stopProcess();
    }

    virtual void onZygoteInit()
    {
        sp<ProcessState> proc = ProcessState::self();
        LOGV("App process: starting thread pool.\n");
        proc->startThreadPool();
    }

    virtual void onExit(int code)
    {
        if (mClassName == NULL) {
            // if zygote
            IPCThreadState::self()->stopProcess();
        }

        AndroidRuntime::onExit(code);
    }


    const char* mParentDir;
    const char* mClassName;
    jclass mClass;
    int mArgC;
    const char* const* mArgV;
};

}

using namespace android;

/*
 * sets argv0 to as much of newArgv0 as will fit
 */
static void setArgv0(const char *argv0, const char *newArgv0)
{
    strlcpy(const_cast<char *>(argv0), newArgv0, strlen(argv0));
}

int main(int argc, const char* argv[])
{
    // These are global variables in ProcessState.cpp
    mArgC = argc;
    mArgV = argv;

    mArgLen = 0;
    for (int i=0; i<argc; i++) {
        mArgLen += strlen(argv[i]) + 1;
    }
    mArgLen--;

    AppRuntime runtime;
    const char* argv0 = argv[0];

    // Process command line arguments
    // ignore argv[0]
    argc--;
    argv++;

    // ignore /system/bin/app_process when invoked via WrapperInit
    if (strcmp(argv[0], "/system/bin/app_process") == 0) {
        LOG(INFO) << "Removing /system/bin/app_process argument";
        argc--;
        argv++;
        for (int i = 0; i < argc; i++) {
            LOG(INFO) << StringPrintf("argv[%d]=%s", i, argv[i]);
        }
    }

    // TODO: remove when we default the boot image
    int oatArgc = argc + 1;
    const char* oatArgv[oatArgc];
    if (strcmp(argv[0], "-Ximage:/data/art-cache/boot.art") != 0) {
        LOG(INFO) << "Adding image arguments";
        oatArgv[0] = "-Ximage:/data/art-cache/boot.art";
        memcpy(oatArgv + (oatArgc - argc), argv, argc * sizeof(*argv));
        argv = oatArgv;
        argc = oatArgc;
        for (int i = 0; i < argc; i++) {
            LOG(INFO) << StringPrintf("argv[%d]=%s", i, argv[i]);
        }
    }

    // TODO: remove the heap arguments when implicit garbage collection enabled
    LOG(INFO) << "Adding heap arguments";
    int heapArgc = argc + 2;
    const char* heapArgv[heapArgc];
    heapArgv[0] = "-Xms64m";
    heapArgv[1] = "-Xmx64m";
    memcpy(heapArgv + (heapArgc - argc), argv, argc * sizeof(*argv));
    argv = heapArgv;
    argc = heapArgc;
    for (int i = 0; i < argc; i++) {
        LOG(INFO) << StringPrintf("argv[%d]=%s", i, argv[i]);
    }

    // TODO: change the system default to not perform preloading
    LOG(INFO) << "Disabling preloading";
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "preload") == 0) {
            argv[i] = "nopreload";
            break;
        }
    }
    for (int i = 0; i < argc; i++) {
        LOG(INFO) << StringPrintf("argv[%d]=%s", i, argv[i]);
    }

    // Everything up to '--' or first non '-' arg goes to the vm

    int i = runtime.addVmArguments(argc, argv);

    // Parse runtime arguments.  Stop at first unrecognized option.
    bool zygote = false;
    bool startSystemServer = false;
    bool noPreload = false;
    bool application = false;
    const char* parentDir = NULL;
    const char* niceName = NULL;
    const char* className = NULL;
    while (i < argc) {
        const char* arg = argv[i++];
        if (!parentDir) {
            parentDir = arg;
        } else if (strcmp(arg, "--zygote") == 0) {
            zygote = true;
            niceName = "zygote";
        } else if (strcmp(arg, "--start-system-server") == 0) {
            startSystemServer = true;
        } else if (strcmp(arg, "--no-preload") == 0) {
            noPreload = true;
        } else if (strcmp(arg, "--application") == 0) {
            application = true;
        } else if (strncmp(arg, "--nice-name=", 12) == 0) {
            niceName = arg + 12;
        } else {
            className = arg;
            break;
        }
    }

    if (niceName && *niceName) {
        setArgv0(argv0, niceName);
        set_process_name(niceName);
    }

    runtime.mParentDir = parentDir;

    if (zygote) {
        std::string options;
        if (startSystemServer) {
            options += "start-system-server ";
        }
        if (noPreload) {
            options += "no-preload ";
        }
        runtime.start("com.android.internal.os.ZygoteInit", options.c_str());
    } else if (className) {
        // Remainder of args get passed to startup class main()
        runtime.mClassName = className;
        runtime.mArgC = argc - i;
        runtime.mArgV = argv + i;
        runtime.start("com.android.internal.os.RuntimeInit",
                application ? "application" : "tool");
    } else {
        fprintf(stderr, "Error: no class name or --zygote supplied.\n");
        app_usage();
        LOG(FATAL) << "oat_process: no class name or --zygote supplied.";
        return 10;
    }
}
