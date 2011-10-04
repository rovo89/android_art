/*
 * Main entry of app process.
 *
 * Starts the interpreted runtime, then starts up the application.
 *
 */

#define LOG_TAG "appproc"

#include "class_loader.h"
#include "jni_internal.h"
#include "stringprintf.h"
#include "thread.h"

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
            LOG(ERROR) << StringPrintf("ERROR: could not find class '%s'\n", mClassName);
        }
        free(slashClassName);

        mClass = reinterpret_cast<jclass>(env->NewGlobalRef(mClass));

        // TODO: remove this ClassLoader code
        jclass ApplicationLoaders = env->FindClass("android/app/ApplicationLoaders");
        jmethodID getDefault = env->GetStaticMethodID(ApplicationLoaders,
                                                      "getDefault",
                                                      "()Landroid/app/ApplicationLoaders;");
        jfieldID mLoaders = env->GetFieldID(ApplicationLoaders, "mLoaders", "Ljava/util/Map;");
        jclass BootClassLoader = env->FindClass("java/lang/BootClassLoader");
        jmethodID getInstance = env->GetStaticMethodID(BootClassLoader,
                                                       "getInstance",
                                                       "()Ljava/lang/BootClassLoader;");
        jclass ClassLoader = env->FindClass("java/lang/ClassLoader");
        jfieldID parent = env->GetFieldID(ClassLoader, "parent", "Ljava/lang/ClassLoader;");
        jclass Map = env->FindClass("java/util/Map");
        jmethodID put = env->GetMethodID(Map,
                                         "put",
                                         "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
        jclass BaseDexClassLoader = env->FindClass("dalvik/system/BaseDexClassLoader");
        jfieldID originalPath = env->GetFieldID(BaseDexClassLoader, "originalPath", "Ljava/lang/String;");
        jfieldID pathList = env->GetFieldID(BaseDexClassLoader, "pathList", "Ldalvik/system/DexPathList;");
        jclass DexPathList = env->FindClass("dalvik/system/DexPathList");
        jmethodID init = env->GetMethodID(DexPathList,
                                          "<init>",
                                          "(Ljava/lang/ClassLoader;Ljava/lang/String;Ljava/lang/String;Ljava/io/File;)V");

        // Set the parent of our pre-existing ClassLoader to the non-null BootClassLoader.getInstance()
        const art::ClassLoader* class_loader_object = art::Thread::Current()->GetClassLoaderOverride();
        jobject class_loader = art::AddLocalReference<jobject>(env, class_loader_object);
        jobject boot_class_loader = env->CallStaticObjectMethod(BootClassLoader, getInstance);
        env->SetObjectField(class_loader, parent, boot_class_loader);

        // Create a DexPathList
        jstring dex_path = env->NewStringUTF("/system/app/Calculator.apk");
        jstring library_path = env->NewStringUTF("/data/data/com.android.calculator2/lib");
        jobject dex_path_list = env->NewObject(DexPathList, init,
                                               boot_class_loader, dex_path, library_path, NULL);

        // Set DexPathList into our pre-existing ClassLoader
        env->SetObjectField(class_loader, pathList, dex_path_list);
        env->SetObjectField(class_loader, originalPath, dex_path);

        // Stash our pre-existing ClassLoader into ApplicationLoaders.getDefault().mLoaders
        // under the expected name.
        jobject application_loaders = env->CallStaticObjectMethod(ApplicationLoaders, getDefault);
        jobject loaders = env->GetObjectField(application_loaders, mLoaders);
        env->CallObjectMethod(loaders, put, dex_path, class_loader);
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

    // TODO: remove Calculator special case
    int oatArgc = argc + 2;
    const char* oatArgv[oatArgc];
    if (strcmp(argv[0], "-Ximage:/system/framework/boot.art") != 0) {
        LOG(INFO) << "Adding oat arguments";
        oatArgv[0] = "-Ximage:/system/framework/boot.art";
        oatArgv[1] = "-Ximage:/system/app/Calculator.art";
        setenv("CLASSPATH", "/system/app/Calculator.apk", 1);
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
        runtime.start("com.android.internal.os.ZygoteInit",
                startSystemServer ? "start-system-server" : "");
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
