// Copyright 2011 Google Inc. All Rights Reserved.

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "base64.h"
#include "heap.h"
#include "thread.h"
#include "stringprintf.h"
#include "class_linker.h"
#include "dex_file.h"

#include "unicode/uclean.h"
#include "unicode/uvernum.h"

#include "gtest/gtest.h"

namespace art {

// package java.lang;
// public class Object {}
//
// class MyClass {}
static const char kMyClassDex[] =
  "ZGV4CjAzNQA5Nm9IrCVm91COwepff7LhIE23GZIxGjgIAgAAcAAAAHhWNBIAAAAAAAAAAIABAAAG"
  "AAAAcAAAAAMAAACIAAAAAQAAAJQAAAAAAAAAAAAAAAIAAACgAAAAAgAAALAAAAAYAQAA8AAAABwB"
  "AAAkAQAALwEAAEMBAABRAQAAXgEAAAEAAAACAAAABQAAAAUAAAACAAAAAAAAAAAAAAAAAAAAAQAA"
  "AAAAAAABAAAAAQAAAP////8AAAAABAAAAAAAAABrAQAAAAAAAAAAAAAAAAAAAQAAAAAAAAADAAAA"
  "AAAAAHUBAAAAAAAAAQABAAAAAABhAQAAAQAAAA4AAAABAAEAAQAAAGYBAAAEAAAAcBABAAAADgAG"
  "PGluaXQ+AAlMTXlDbGFzczsAEkxqYXZhL2xhbmcvT2JqZWN0OwAMTXlDbGFzcy5qYXZhAAtPYmpl"
  "Y3QuamF2YQABVgACAAcOAAUABw4AAAABAAGBgATwAQAAAQAAgIAEhAIACwAAAAAAAAABAAAAAAAA"
  "AAEAAAAGAAAAcAAAAAIAAAADAAAAiAAAAAMAAAABAAAAlAAAAAUAAAACAAAAoAAAAAYAAAACAAAA"
  "sAAAAAEgAAACAAAA8AAAAAIgAAAGAAAAHAEAAAMgAAACAAAAYQEAAAAgAAACAAAAawEAAAAQAAAB"
  "AAAAgAEAAA==";

// class Nested {
//     class Inner {
//     }
// }
static const char kNestedDex[] =
  "ZGV4CjAzNQAQedgAe7gM1B/WHsWJ6L7lGAISGC7yjD2IAwAAcAAAAHhWNBIAAAAAAAAAAMQCAAAP"
  "AAAAcAAAAAcAAACsAAAAAgAAAMgAAAABAAAA4AAAAAMAAADoAAAAAgAAAAABAABIAgAAQAEAAK4B"
  "AAC2AQAAvQEAAM0BAADXAQAA+wEAABsCAAA+AgAAUgIAAF8CAABiAgAAZgIAAHMCAAB5AgAAgQIA"
  "AAIAAAADAAAABAAAAAUAAAAGAAAABwAAAAkAAAAJAAAABgAAAAAAAAAKAAAABgAAAKgBAAAAAAEA"
  "DQAAAAAAAQAAAAAAAQAAAAAAAAAFAAAAAAAAAAAAAAAAAAAABQAAAAAAAAAIAAAAiAEAAKsCAAAA"
  "AAAAAQAAAAAAAAAFAAAAAAAAAAgAAACYAQAAuAIAAAAAAAACAAAAlAIAAJoCAAABAAAAowIAAAIA"
  "AgABAAAAiAIAAAYAAABbAQAAcBACAAAADgABAAEAAQAAAI4CAAAEAAAAcBACAAAADgBAAQAAAAAA"
  "AAAAAAAAAAAATAEAAAAAAAAAAAAAAAAAAAEAAAABAAY8aW5pdD4ABUlubmVyAA5MTmVzdGVkJElu"
  "bmVyOwAITE5lc3RlZDsAIkxkYWx2aWsvYW5ub3RhdGlvbi9FbmNsb3NpbmdDbGFzczsAHkxkYWx2"
  "aWsvYW5ub3RhdGlvbi9Jbm5lckNsYXNzOwAhTGRhbHZpay9hbm5vdGF0aW9uL01lbWJlckNsYXNz"
  "ZXM7ABJMamF2YS9sYW5nL09iamVjdDsAC05lc3RlZC5qYXZhAAFWAAJWTAALYWNjZXNzRmxhZ3MA"
  "BG5hbWUABnRoaXMkMAAFdmFsdWUAAgEABw4AAQAHDjwAAgIBDhgBAgMCCwQADBcBAgQBDhwBGAAA"
  "AQEAAJAgAICABNQCAAABAAGAgATwAgAAEAAAAAAAAAABAAAAAAAAAAEAAAAPAAAAcAAAAAIAAAAH"
  "AAAArAAAAAMAAAACAAAAyAAAAAQAAAABAAAA4AAAAAUAAAADAAAA6AAAAAYAAAACAAAAAAEAAAMQ"
  "AAACAAAAQAEAAAEgAAACAAAAVAEAAAYgAAACAAAAiAEAAAEQAAABAAAAqAEAAAIgAAAPAAAArgEA"
  "AAMgAAACAAAAiAIAAAQgAAADAAAAlAIAAAAgAAACAAAAqwIAAAAQAAABAAAAxAIAAA==";

// class ProtoCompare {
//     int m1(short x, int y, long z) { return x + y + (int)z; }
//     int m2(short x, int y, long z) { return x + y + (int)z; }
//     int m3(long x, int y, short z) { return (int)x + y + z; }
//     long m4(long x, int y, short z) { return x + y + z; }
// }
static const char kProtoCompareDex[] =
  "ZGV4CjAzNQBLUetu+TVZ8gsYsCOFoij7ecsHaGSEGA8gAwAAcAAAAHhWNBIAAAAAAAAAAIwCAAAP"
  "AAAAcAAAAAYAAACsAAAABAAAAMQAAAAAAAAAAAAAAAYAAAD0AAAAAQAAACQBAADcAQAARAEAAN4B"
  "AADmAQAA6QEAAO8BAAD1AQAA+AEAAP4BAAAOAgAAIgIAADUCAAA4AgAAOwIAAD8CAABDAgAARwIA"
  "AAEAAAAEAAAABgAAAAcAAAAJAAAACgAAAAIAAAAAAAAAyAEAAAMAAAAAAAAA1AEAAAUAAAABAAAA"
  "yAEAAAoAAAAFAAAAAAAAAAIAAwAAAAAAAgABAAsAAAACAAEADAAAAAIAAAANAAAAAgACAA4AAAAD"
  "AAMAAAAAAAIAAAAAAAAAAwAAAAAAAAAIAAAAAAAAAHACAAAAAAAAAQABAAEAAABLAgAABAAAAHAQ"
  "BQAAAA4ABwAFAAAAAABQAgAABQAAAJAAAwSEUbAQDwAAAAcABQAAAAAAWAIAAAUAAACQAAMEhFGw"
  "EA8AAAAGAAUAAAAAAGACAAAEAAAAhCCwQLBQDwAJAAUAAAAAAGgCAAAFAAAAgXC7UIGCuyAQAAAA"
  "AwAAAAEAAAAEAAAAAwAAAAQAAAABAAY8aW5pdD4AAUkABElKSVMABElTSUoAAUoABEpKSVMADkxQ"
  "cm90b0NvbXBhcmU7ABJMamF2YS9sYW5nL09iamVjdDsAEVByb3RvQ29tcGFyZS5qYXZhAAFTAAFW"
  "AAJtMQACbTIAAm0zAAJtNAABAAcOAAIDAAAABw4AAwMAAAAHDgAEAwAAAAcOAAUDAAAABw4AAAAB"
  "BACAgATEAgEA3AIBAPgCAQCUAwEArAMAAAwAAAAAAAAAAQAAAAAAAAABAAAADwAAAHAAAAACAAAA"
  "BgAAAKwAAAADAAAABAAAAMQAAAAFAAAABgAAAPQAAAAGAAAAAQAAACQBAAABIAAABQAAAEQBAAAB"
  "EAAAAgAAAMgBAAACIAAADwAAAN4BAAADIAAABQAAAEsCAAAAIAAAAQAAAHACAAAAEAAAAQAAAIwC"
  "AAA=";

// class ProtoCompare2 {
//     int m1(short x, int y, long z) { return x + y + (int)z; }
//     int m2(short x, int y, long z) { return x + y + (int)z; }
//     int m3(long x, int y, short z) { return (int)x + y + z; }
//     long m4(long x, int y, short z) { return x + y + z; }
// }
static const char kProtoCompare2Dex[] =
  "ZGV4CjAzNQDVUXj687EpyTTDJZEZPA8dEYnDlm0Ir6YgAwAAcAAAAHhWNBIAAAAAAAAAAIwCAAAP"
  "AAAAcAAAAAYAAACsAAAABAAAAMQAAAAAAAAAAAAAAAYAAAD0AAAAAQAAACQBAADcAQAARAEAAN4B"
  "AADmAQAA6QEAAO8BAAD1AQAA+AEAAP4BAAAPAgAAIwIAADcCAAA6AgAAPQIAAEECAABFAgAASQIA"
  "AAEAAAAEAAAABgAAAAcAAAAJAAAACgAAAAIAAAAAAAAAyAEAAAMAAAAAAAAA1AEAAAUAAAABAAAA"
  "yAEAAAoAAAAFAAAAAAAAAAIAAwAAAAAAAgABAAsAAAACAAEADAAAAAIAAAANAAAAAgACAA4AAAAD"
  "AAMAAAAAAAIAAAAAAAAAAwAAAAAAAAAIAAAAAAAAAHICAAAAAAAAAQABAAEAAABNAgAABAAAAHAQ"
  "BQAAAA4ABwAFAAAAAABSAgAABQAAAJAAAwSEUbAQDwAAAAcABQAAAAAAWgIAAAUAAACQAAMEhFGw"
  "EA8AAAAGAAUAAAAAAGICAAAEAAAAhCCwQLBQDwAJAAUAAAAAAGoCAAAFAAAAgXC7UIGCuyAQAAAA"
  "AwAAAAEAAAAEAAAAAwAAAAQAAAABAAY8aW5pdD4AAUkABElKSVMABElTSUoAAUoABEpKSVMAD0xQ"
  "cm90b0NvbXBhcmUyOwASTGphdmEvbGFuZy9PYmplY3Q7ABJQcm90b0NvbXBhcmUyLmphdmEAAVMA"
  "AVYAAm0xAAJtMgACbTMAAm00AAEABw4AAgMAAAAHDgADAwAAAAcOAAQDAAAABw4ABQMAAAAHDgAA"
  "AAEEAICABMQCAQDcAgEA+AIBAJQDAQCsAwwAAAAAAAAAAQAAAAAAAAABAAAADwAAAHAAAAACAAAA"
  "BgAAAKwAAAADAAAABAAAAMQAAAAFAAAABgAAAPQAAAAGAAAAAQAAACQBAAABIAAABQAAAEQBAAAB"
  "EAAAAgAAAMgBAAACIAAADwAAAN4BAAADIAAABQAAAE0CAAAAIAAAAQAAAHICAAAAEAAAAQAAAIwC"
  "AAA=";

// javac MyClass.java && dx --dex --output=MyClass.dex
//   --core-library MyClass.class java/lang/Object.class && base64 MyClass.dex
// package java.lang;
// public class Object {}
// class MyClass {
//   native void foo();
//   native int fooI(int x);
//   native int fooII(int x, int y);
//   native double fooDD(double x, double y);
//   native Object fooIOO(int x, Object y, Object z);
//   static native Object fooSIOO(int x, Object y, Object z);
//   static synchronized native Object fooSSIOO(int x, Object y, Object z);
// }
static const char kMyClassNativesDex[] =
  "ZGV4CjAzNQA4WWrpXgdlkoTHR8Yubx4LJO4HbGsX1p1EAwAAcAAAAHhWNBIAAAAAAAAAALACAAAT"
  "AAAAcAAAAAUAAAC8AAAABQAAANAAAAAAAAAAAAAAAAkAAAAMAQAAAgAAAFQBAACwAQAAlAEAAOIB"
  "AADqAQAA7QEAAPIBAAD1AQAA+QEAAP4BAAAEAgAADwIAACMCAAAxAgAAPgIAAEECAABGAgAATQIA"
  "AFMCAABaAgAAYgIAAGsCAAABAAAAAwAAAAcAAAAIAAAACwAAAAIAAAAAAAAAwAEAAAQAAAABAAAA"
  "yAEAAAUAAAABAAAA0AEAAAYAAAADAAAA2AEAAAsAAAAEAAAAAAAAAAIABAAAAAAAAgAEAAwAAAAC"
  "AAAADQAAAAIAAQAOAAAAAgACAA8AAAACAAMAEAAAAAIAAwARAAAAAgADABIAAAADAAQAAAAAAAMA"
  "AAABAAAA/////wAAAAAKAAAAAAAAAH8CAAAAAAAAAgAAAAAAAAADAAAAAAAAAAkAAAAAAAAAiQIA"
  "AAAAAAABAAEAAAAAAHUCAAABAAAADgAAAAEAAQABAAAAegIAAAQAAABwEAgAAAAOAAIAAAAAAAAA"
  "AQAAAAEAAAACAAAAAQABAAMAAAABAAMAAwAGPGluaXQ+AAFEAANEREQAAUkAAklJAANJSUkABExJ"
  "TEwACUxNeUNsYXNzOwASTGphdmEvbGFuZy9PYmplY3Q7AAxNeUNsYXNzLmphdmEAC09iamVjdC5q"
  "YXZhAAFWAANmb28ABWZvb0REAARmb29JAAVmb29JSQAGZm9vSU9PAAdmb29TSU9PAAhmb29TU0lP"
  "TwADAAcOAAEABw4AAAABAAiBgASUAwAAAwUAgIAEqAMGiAIAAaiCCAABgAIAAYACAAGAAgABgAIA"
  "AYACAAwAAAAAAAAAAQAAAAAAAAABAAAAEwAAAHAAAAACAAAABQAAALwAAAADAAAABQAAANAAAAAF"
  "AAAACQAAAAwBAAAGAAAAAgAAAFQBAAABIAAAAgAAAJQBAAABEAAABAAAAMABAAACIAAAEwAAAOIB"
  "AAADIAAAAgAAAHUCAAAAIAAAAgAAAH8CAAAAEAAAAQAAALACAAA=";

// class CreateMethodDescriptor {
//     Float m1(int a, double b, long c, Object d) { return null; }
//     CreateMethodDescriptor m2(boolean x, short y, char z) { return null; }
// }
static const char kCreateMethodDescriptorDex[] =
  "ZGV4CjAzNQBSU7aKdNXwH+uOpti/mvZ4/Dk8wM8VtNbgAgAAcAAAAHhWNBIAAAAAAAAAAEwCAAAQ"
  "AAAAcAAAAAoAAACwAAAAAwAAANgAAAAAAAAAAAAAAAQAAAD8AAAAAQAAABwBAACkAQAAPAEAAJQB"
  "AACcAQAAnwEAALwBAAC/AQAAwgEAAMUBAADfAQAA5gEAAOwBAAD/AQAAEwIAABYCAAAZAgAAHAIA"
  "ACACAAABAAAAAwAAAAQAAAAFAAAABgAAAAkAAAAKAAAACwAAAAwAAAANAAAACAAAAAQAAAB8AQAA"
  "BwAAAAUAAACIAQAADAAAAAgAAAAAAAAABAACAAAAAAAEAAEADgAAAAQAAAAPAAAABgACAAAAAAAE"
  "AAAAAAAAAAYAAAAAAAAAAgAAAAAAAAA6AgAAAAAAAAEAAQABAAAAJAIAAAQAAABwEAMAAAAOAAgA"
  "BwAAAAAAKQIAAAIAAAASABEABQAEAAAAAAAyAgAAAgAAABIAEQADAAAACQAHAAAAAAAEAAAAAgAB"
  "AAMABgAGPGluaXQ+AAFDABtDcmVhdGVNZXRob2REZXNjcmlwdG9yLmphdmEAAUQAAUkAAUoAGExD"
  "cmVhdGVNZXRob2REZXNjcmlwdG9yOwAFTElESkwABExaU0MAEUxqYXZhL2xhbmcvRmxvYXQ7ABJM"
  "amF2YS9sYW5nL09iamVjdDsAAVMAAVYAAVoAAm0xAAJtMgABAAcOAAIEAAAAAAcOAAMDAAAABw4A"
  "AAABAgCAgAS8AgEA1AIBAOgCDAAAAAAAAAABAAAAAAAAAAEAAAAQAAAAcAAAAAIAAAAKAAAAsAAA"
  "AAMAAAADAAAA2AAAAAUAAAAEAAAA/AAAAAYAAAABAAAAHAEAAAEgAAADAAAAPAEAAAEQAAACAAAA"
  "fAEAAAIgAAAQAAAAlAEAAAMgAAADAAAAJAIAAAAgAAABAAAAOgIAAAAQAAABAAAATAIAAA==";

// class X {}
// class Y extends X {}
static const char kXandY[] =
  "ZGV4CjAzNQAlLMqyB72TxJW4zl5w75F072u4Ig6KvCMEAgAAcAAAAHhWNBIAAAAAAAAAAHwBAAAG"
  "AAAAcAAAAAQAAACIAAAAAQAAAJgAAAAAAAAAAAAAAAMAAACkAAAAAgAAALwAAAAIAQAA/AAAACwB"
  "AAA0AQAAOQEAAD4BAABSAQAAVQEAAAEAAAACAAAAAwAAAAQAAAAEAAAAAwAAAAAAAAAAAAAAAAAA"
  "AAEAAAAAAAAAAgAAAAAAAAAAAAAAAAAAAAIAAAAAAAAABQAAAAAAAABnAQAAAAAAAAEAAAAAAAAA"
  "AAAAAAAAAAAFAAAAAAAAAHEBAAAAAAAAAQABAAEAAABdAQAABAAAAHAQAgAAAA4AAQABAAEAAABi"
  "AQAABAAAAHAQAAAAAA4ABjxpbml0PgADTFg7AANMWTsAEkxqYXZhL2xhbmcvT2JqZWN0OwABVgAG"
  "WC5qYXZhAAIABw4AAwAHDgAAAAEAAICABPwBAAABAAGAgASUAgALAAAAAAAAAAEAAAAAAAAAAQAA"
  "AAYAAABwAAAAAgAAAAQAAACIAAAAAwAAAAEAAACYAAAABQAAAAMAAACkAAAABgAAAAIAAAC8AAAA"
  "ASAAAAIAAAD8AAAAAiAAAAYAAAAsAQAAAyAAAAIAAABdAQAAACAAAAIAAABnAQAAABAAAAEAAAB8"
  "AQAA";

// class Statics {
//   static boolean s0 = true;
//   static byte s1 = 5;
//   static char s2 = 'a';
//   static short s3 = (short) 65000;
//   static int s4 = 2000000000;
//   static long s5 = 0x123456789abcdefL;
//   static float s6 = 0.5f;
//   static double s7 = 16777217;
//   static Object s8 = "android";
//   static Object[] s9 = { "a", "b" };
// }
static const char kStatics[] =
  "ZGV4CjAzNQAYalInXcX4y0OBgb2yCw2/jGzZBSe34zmwAwAAcAAAAHhWNBIAAAAAAAAAABwDAAAc"
  "AAAAcAAAAAwAAADgAAAAAQAAABABAAAKAAAAHAEAAAMAAABsAQAAAQAAAIQBAAAMAgAApAEAADwC"
  "AABGAgAATgIAAFECAABUAgAAVwIAAFoCAABdAgAAYAIAAGsCAAB/AgAAggIAAJACAACTAgAAlgIA"
  "AKsCAACuAgAAtwIAALoCAAC+AgAAwgIAAMYCAADKAgAAzgIAANICAADWAgAA2gIAAN4CAAACAAAA"
  "AwAAAAQAAAAFAAAABgAAAAcAAAAIAAAACQAAAAoAAAAMAAAADQAAAA4AAAAMAAAACQAAAAAAAAAG"
  "AAoAEgAAAAYAAAATAAAABgABABQAAAAGAAgAFQAAAAYABAAWAAAABgAFABcAAAAGAAMAGAAAAAYA"
  "AgAZAAAABgAHABoAAAAGAAsAGwAAAAYAAAAAAAAABgAAAAEAAAAHAAAAAQAAAAYAAAAAAAAABwAA"
  "AAAAAAALAAAAAAAAAPUCAAAAAAAABAAAAAAAAADiAgAAOAAAABITagMAABJQawABABMAYQBsAAIA"
  "EwDo/W0AAwAUAACUNXdnAAQAGADvzauJZ0UjAWgABQAVAAA/ZwAGABgAAAAAEAAAcEFoAAcAGgAQ"
  "AGkACAASICMACwASARoCDwBNAgABGgERAE0BAANpAAkADgABAAEAAQAAAPACAAAEAAAAcBACAAAA"
  "DgAIPGNsaW5pdD4ABjxpbml0PgABQgABQwABRAABRgABSQABSgAJTFN0YXRpY3M7ABJMamF2YS9s"
  "YW5nL09iamVjdDsAAVMADFN0YXRpY3MuamF2YQABVgABWgATW0xqYXZhL2xhbmcvT2JqZWN0OwAB"
  "YQAHYW5kcm9pZAABYgACczAAAnMxAAJzMgACczMAAnM0AAJzNQACczYAAnM3AAJzOAACczkAAgAH"
  "HS08S0taeEt4SwABAAcOAAoAAgAACAEIAQgBCAEIAQgBCAEIAQgBCACIgASkAwGAgASkBAAAAAwA"
  "AAAAAAAAAQAAAAAAAAABAAAAHAAAAHAAAAACAAAADAAAAOAAAAADAAAAAQAAABABAAAEAAAACgAA"
  "ABwBAAAFAAAAAwAAAGwBAAAGAAAAAQAAAIQBAAABIAAAAgAAAKQBAAACIAAAHAAAADwCAAADIAAA"
  "AgAAAOICAAAAIAAAAQAAAPUCAAAAEAAAAQAAABwDAAA=";

//  class AllFields {
//    static boolean sZ;
//    static byte sB;
//    static char sC;
//    static double sD;
//    static float sF;
//    static int sI;
//    static long sJ;
//    static short sS;
//    static Object sObject;
//    static Object[] sObjectArray;
//
//    boolean iZ;
//    byte iB;
//    char iC;
//    double iD;
//    float iF;
//    int iI;
//    long iJ;
//    short iS;
//    Object iObject;
//    Object[] iObjectArray;
//  }
static const char kAllFields[] =
  "ZGV4CjAzNQCdaMDlt1s2Pw65nbVCJcCcZcmroYXvMF/AAwAAcAAAAHhWNBIAAAAAAAAAACwDAAAi"
  "AAAAcAAAAAwAAAD4AAAAAQAAACgBAAAUAAAANAEAAAIAAADUAQAAAQAAAOQBAAC8AQAABAIAABwC"
  "AAAkAgAANAIAADcCAAA6AgAAPQIAAEACAABDAgAARgIAAFMCAABnAgAAagIAAG0CAABwAgAAhQIA"
  "AIkCAACNAgAAkQIAAJUCAACZAgAAnQIAAKYCAAC0AgAAuAIAALwCAADAAgAAxAIAAMgCAADMAgAA"
  "0AIAANQCAADdAgAA6wIAAO8CAAACAAAAAwAAAAQAAAAFAAAABgAAAAcAAAAIAAAACQAAAAoAAAAL"
  "AAAADAAAAA0AAAALAAAACQAAAAAAAAAGAAAADgAAAAYAAQAPAAAABgACABAAAAAGAAMAEQAAAAYA"
  "BAASAAAABgAFABMAAAAGAAcAFAAAAAYACwAVAAAABgAIABYAAAAGAAoAFwAAAAYAAAAYAAAABgAB"
  "ABkAAAAGAAIAGgAAAAYAAwAbAAAABgAEABwAAAAGAAUAHQAAAAYABwAeAAAABgALAB8AAAAGAAgA"
  "IAAAAAYACgAhAAAABgAAAAAAAAAHAAAAAAAAAAYAAAAAAAAABwAAAAAAAAABAAAAAAAAAPgCAAAA"
  "AAAAAQABAAEAAADzAgAABAAAAHAQAQAAAA4ABjxpbml0PgAOQWxsRmllbGRzLmphdmEAAUIAAUMA"
  "AUQAAUYAAUkAAUoAC0xBbGxGaWVsZHM7ABJMamF2YS9sYW5nL09iamVjdDsAAVMAAVYAAVoAE1tM"
  "amF2YS9sYW5nL09iamVjdDsAAmlCAAJpQwACaUQAAmlGAAJpSQACaUoAB2lPYmplY3QADGlPYmpl"
  "Y3RBcnJheQACaVMAAmlaAAJzQgACc0MAAnNEAAJzRgACc0kAAnNKAAdzT2JqZWN0AAxzT2JqZWN0"
  "QXJyYXkAAnNTAAJzWgADAAcOAAoKAQAKCAEIAQgBCAEIAQgBCAEIAQgBCAAAAQABAAEAAQABAAEA"
  "AQABAAEAAICABIQEAAAMAAAAAAAAAAEAAAAAAAAAAQAAACIAAABwAAAAAgAAAAwAAAD4AAAAAwAA"
  "AAEAAAAoAQAABAAAABQAAAA0AQAABQAAAAIAAADUAQAABgAAAAEAAADkAQAAASAAAAEAAAAEAgAA"
  "AiAAACIAAAAcAgAAAyAAAAEAAADzAgAAACAAAAEAAAD4AgAAABAAAAEAAAAsAwAA";


// class Main {
//   public static void main(String args[]) {
//   }
// }
static const char kMainDex[] =
  "ZGV4CjAzNQAPNypTL1TulODHFdpEa2pP98I7InUu7uQgAgAAcAAAAHhWNBIAAAAAAAAAAIwBAAAI"
  "AAAAcAAAAAQAAACQAAAAAgAAAKAAAAAAAAAAAAAAAAMAAAC4AAAAAQAAANAAAAAwAQAA8AAAACIB"
  "AAAqAQAAMgEAAEYBAABRAQAAVAEAAFgBAABtAQAAAQAAAAIAAAAEAAAABgAAAAQAAAACAAAAAAAA"
  "AAUAAAACAAAAHAEAAAAAAAAAAAAAAAABAAcAAAABAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAADAAAA"
  "AAAAAH4BAAAAAAAAAQABAAEAAABzAQAABAAAAHAQAgAAAA4AAQABAAAAAAB4AQAAAQAAAA4AAAAB"
  "AAAAAwAGPGluaXQ+AAZMTWFpbjsAEkxqYXZhL2xhbmcvT2JqZWN0OwAJTWFpbi5qYXZhAAFWAAJW"
  "TAATW0xqYXZhL2xhbmcvU3RyaW5nOwAEbWFpbgABAAcOAAMBAAcOAAAAAgAAgIAE8AEBCYgCDAAA"
  "AAAAAAABAAAAAAAAAAEAAAAIAAAAcAAAAAIAAAAEAAAAkAAAAAMAAAACAAAAoAAAAAUAAAADAAAA"
  "uAAAAAYAAAABAAAA0AAAAAEgAAACAAAA8AAAAAEQAAABAAAAHAEAAAIgAAAIAAAAIgEAAAMgAAAC"
  "AAAAcwEAAAAgAAABAAAAfgEAAAAQAAABAAAAjAEAAA==";

// class StaticLeafMethods {
//   static void nop() {
//   }
//   static byte identity(byte x) {
//     return x;
//   }
//   static int identity(int x) {
//     return x;
//   }
//   static int sum(int a, int b) {
//     return a + b;
//   }
//   static int sum(int a, int b, int c) {
//     return a + b + c;
//   }
//   static int sum(int a, int b, int c, int d) {
//     return a + b + c + d;
//   }
//   static int sum(int a, int b, int c, int d, int e) {
//     return a + b + c + d + e;
//   }
//   static double identity(double x) {
//     return x;
//   }
//   static double sum(double a, double b) {
//     return a + b;
//   }
//   static double sum(double a, double b, double c) {
//     return a + b + c;
//   }
//   static double sum(double a, double b, double c, double d) {
//     return a + b + c + d;
//   }
//   static double sum(double a, double b, double c, double d, double e) {
//     return a + b + c + d + e;
//   }
// }
static const char kStaticLeafMethodsDex[] =
  "ZGV4CjAzNQD8gEpaFD0w5dM8dsPaCQ3wIh0xaUjfni+IBQAAcAAAAHhWNBIAAAAAAAAAAPQEAAAW"
  "AAAAcAAAAAYAAADIAAAADAAAAOAAAAAAAAAAAAAAAA4AAABwAQAAAQAAAOABAACIAwAAAAIAAK4D"
  "AAC2AwAAuQMAAL0DAADAAwAAxAMAAMkDAADPAwAA1gMAAN4DAADhAwAA5QMAAOoDAADwAwAA9wMA"
  "AP8DAAAUBAAAKAQAAEAEAABDBAAATQQAAFIEAAABAAAAAwAAAAkAAAAPAAAAEAAAABIAAAACAAAA"
  "AAAAADgDAAAEAAAAAQAAAEADAAAFAAAAAQAAAEgDAAAGAAAAAQAAAFADAAAHAAAAAQAAAFwDAAAI"
  "AAAAAQAAAGgDAAAKAAAAAgAAAHgDAAALAAAAAgAAAIADAAAMAAAAAgAAAIgDAAANAAAAAgAAAJQD"
  "AAAOAAAAAgAAAKADAAASAAAABQAAAAAAAAADAAsAAAAAAAMAAAATAAAAAwABABMAAAADAAYAEwAA"
  "AAMACwAUAAAAAwACABUAAAADAAMAFQAAAAMABAAVAAAAAwAFABUAAAADAAcAFQAAAAMACAAVAAAA"
  "AwAJABUAAAADAAoAFQAAAAQACwAAAAAAAwAAAAAAAAAEAAAAAAAAABEAAAAAAAAAtwQAAAAAAAAB"
  "AAEAAQAAAFcEAAAEAAAAcBANAAAADgABAAEAAAAAAFwEAAABAAAADwAAAAIAAgAAAAAAYgQAAAEA"
  "AAAQAAAAAQABAAAAAABoBAAAAQAAAA8AAAAAAAAAAAAAAG4EAAABAAAADgAAAAYABAAAAAAAcwQA"
  "AAMAAACrAAIEEAAAAAgABgAAAAAAegQAAAQAAACrAAIEy2AQAAoACAAAAAAAggQAAAUAAACrAAIE"
  "y2DLgBAAAAAMAAoAAAAAAIsEAAAGAAAAqwACBMtgy4DLoBAAAwACAAAAAACVBAAAAwAAAJAAAQIP"
  "AAAABAADAAAAAACcBAAABAAAAJAAAQKwMA8ABQAEAAAAAACkBAAABQAAAJAAAQKwMLBADwAAAAYA"
  "BQAAAAAArQQAAAYAAACQAAECsDCwQLBQDwABAAAAAAAAAAEAAAABAAAAAgAAAAEAAQADAAAAAQAB"
  "AAEAAAAEAAAAAQABAAEAAQAFAAAAAQABAAEAAQABAAAAAQAAAAIAAAACAAAAAgACAAMAAAACAAIA"
  "AgAAAAQAAAACAAIAAgACAAUAAAACAAIAAgACAAIABjxpbml0PgABQgACQkIAAUQAAkREAANEREQA"
  "BEREREQABUREREREAAZEREREREQAAUkAAklJAANJSUkABElJSUkABUlJSUlJAAZJSUlJSUkAE0xT"
  "dGF0aWNMZWFmTWV0aG9kczsAEkxqYXZhL2xhbmcvT2JqZWN0OwAWU3RhdGljTGVhZk1ldGhvZHMu"
  "amF2YQABVgAIaWRlbnRpdHkAA25vcAADc3VtAAEABw4ABQEABw4AFwEABw4ACAEABw4AAwAHDgAa"
  "AgAABw4AHQMAAAAHDgAgBAAAAAAHDgAjBQAAAAAABw4ACwIAAAcOAA4DAAAABw4AEQQAAAAABw4A"
  "FAUAAAAAAAcOAAAADQAAgIAEgAQBCJgEAQisBAEIwAQBCNQEAQjoBAEIgAUBCJgFAQi0BQEI0AUB"
  "COgFAQiABgEInAYAAAAMAAAAAAAAAAEAAAAAAAAAAQAAABYAAABwAAAAAgAAAAYAAADIAAAAAwAA"
  "AAwAAADgAAAABQAAAA4AAABwAQAABgAAAAEAAADgAQAAASAAAA0AAAAAAgAAARAAAAsAAAA4AwAA"
  "AiAAABYAAACuAwAAAyAAAA0AAABXBAAAACAAAAEAAAC3BAAAABAAAAEAAAD0BAAA";

//class Fibonacci {
//
//    static int fibonacci(int n) {
//        if (n == 0) {
//            return 0;
//        }
//        int x = 1;
//        int y = 1;
//        for (int i = 3; i <= n; i++) {
//            int z = x + y;
//            x = y;
//            y = z;
//        }
//        return y;
//    }
//
//    public static void main(String[] args) {
//        try {
//            if (args.length == 1) {
//                int x = Integer.parseInt(args[0]);
//                int y = fibonacci(x); /* to warm up cache */
//                System.out.printf("fibonacci(%d)=%d\n", x, y);
//                y = fibonacci(x +1);
//                System.out.printf("fibonacci(%d)=%d\n", x, y);
//            }
//        } catch (NumberFormatException ex) {}
//    }
//}
static const char kFibonacciDex[] =
"ZGV4CjAzNQBaslnMUQxaXYgC3gD9FGHjVb8cHZ60G8ckBQAAcAAAAHhWNBIAAAAAAAAAAIQEAAAa"
"AAAAcAAAAAsAAADYAAAABgAAAAQBAAABAAAATAEAAAcAAABUAQAAAgAAAIwBAABYAwAAzAEAAPoC"
"AAACAwAAEgMAABUDAAAZAwAAHQMAACoDAAAuAwAAMwMAAEoDAABfAwAAggMAAJYDAACqAwAAvgMA"
"AMsDAADOAwAA0gMAAOcDAAD8AwAABwQAABoEAAAgBAAAJQQAAC8EAAA3BAAAAgAAAAUAAAAIAAAA"
"CQAAAAoAAAALAAAADAAAAA0AAAAPAAAAEQAAABIAAAADAAAAAAAAANwCAAAEAAAAAAAAAOQCAAAH"
"AAAAAgAAAOwCAAAGAAAAAwAAANwCAAAPAAAACAAAAAAAAAAQAAAACAAAAPQCAAAHAAIAFgAAAAEA"
"BAAAAAAAAQAAABMAAAABAAUAFQAAAAIAAgAYAAAAAwABABcAAAADAAMAGQAAAAUABAAAAAAABQAA"
"AAEAAAD/////AAAAAA4AAAAAAAAAaAQAAAAAAAABAAAAAAAAAAUAAAAAAAAAAQAAAAAAAAByBAAA"
"AAAAAAEAAQAAAAAAQAQAAAEAAAAOAAAAAQABAAEAAABFBAAABAAAAHAQBgAAAA4ABQABAAAAAABK"
"BAAAEwAAABIROQQEABIADwASMAESARMBAQEwNkH6/7AC2AEBAQEjAQIBMCj4AAAIAAEAAwABAFcE"
"AABIAAAAEhEhcDMQQwASAEYABwBxEAQAAAAKAHEQAQAAAAoBYgIAABoDFAASJCNECQASBXEQBQAA"
"AAwGTQYEBRIVcRAFAAEADAFNAQQFbjADADIE2AEAAXEQAQABAAoBYgIAABoDFAASJCNECQASBXEQ"
"BQAAAAwATQAEBRIQcRAFAAEADAFNAQQAbjADADIEDgANACj+AQAAAEQAAQABAQRGAQAAAAAAAAAB"
"AAAABgAAAAIAAAAGAAkAAQAAAAoABjxpbml0PgAORmlib25hY2NpLmphdmEAAUkAAklJAAJJTAAL"
"TEZpYm9uYWNjaTsAAkxJAANMTEwAFUxqYXZhL2lvL1ByaW50U3RyZWFtOwATTGphdmEvbGFuZy9J"
"bnRlZ2VyOwAhTGphdmEvbGFuZy9OdW1iZXJGb3JtYXRFeGNlcHRpb247ABJMamF2YS9sYW5nL09i"
"amVjdDsAEkxqYXZhL2xhbmcvU3RyaW5nOwASTGphdmEvbGFuZy9TeXN0ZW07AAtPYmplY3QuamF2"
"YQABVgACVkwAE1tMamF2YS9sYW5nL09iamVjdDsAE1tMamF2YS9sYW5nL1N0cmluZzsACWZpYm9u"
"YWNjaQARZmlib25hY2NpKCVkKT0lZAoABG1haW4AA291dAAIcGFyc2VJbnQABnByaW50ZgAHdmFs"
"dWVPZgADAAcOAAEABw4ADwEABx0tJgJ7HXgcAB4BAAcdPHhLARgPaQEYERwAAAABAAaBgATMAwAA"
"AwAAgIAE4AMBCPgDAQmwBA0AAAAAAAAAAQAAAAAAAAABAAAAGgAAAHAAAAACAAAACwAAANgAAAAD"
"AAAABgAAAAQBAAAEAAAAAQAAAEwBAAAFAAAABwAAAFQBAAAGAAAAAgAAAIwBAAABIAAABAAAAMwB"
"AAABEAAABAAAANwCAAACIAAAGgAAAPoCAAADIAAABAAAAEAEAAAAIAAAAgAAAGgEAAAAEAAAAQAA"
"AIQEAAA=";

//class IntMath {
//
//    /*
//     * Try to cause some unary operations.
//     */
//    static int unopTest(int x) {
//        x = -x;
//        x ^= 0xffffffff;
//        return x;
//    }
//
//    static int shiftTest1() {
//        final int[] mBytes = {
//            0x11, 0x22, 0x33, 0x44, 0x88, 0x99, 0xaa, 0xbb
//        };
//        long l;
//        int i1, i2;
//
//        i1 = mBytes[0] | mBytes[1] << 8 | mBytes[2] << 16 | mBytes[3] << 24;
//        i2 = mBytes[4] | mBytes[5] << 8 | mBytes[6] << 16 | mBytes[7] << 24;
//        l = i1 | ((long)i2 << 32);
//
//        if (i1 != 0x44332211) { return 1; }
//        if (i2 != 0xbbaa9988) { return 2; }
//        if (l != 0xbbaa998844332211L) { return 3; }
//
//        l = (long)mBytes[0]
//            | (long)mBytes[1] << 8
//            | (long)mBytes[2] << 16
//            | (long)mBytes[3] << 24
//            | (long)mBytes[4] << 32
//            | (long)mBytes[5] << 40
//            | (long)mBytes[6] << 48
//            | (long)mBytes[7] << 56;
//
//        if (l != 0xbbaa998844332211L) { return 4; }
//        return 0;
//    }
//
//    static int shiftTest2() {
//
//        long    a = 0x11;
//        long    b = 0x22;
//        long    c = 0x33;
//        long    d = 0x44;
//        long    e = 0x55;
//        long    f = 0x66;
//        long    g = 0x77;
//        long    h = 0x88;
//
//        long    result = ((a << 56) | (b << 48) | (c << 40) | (d << 32) |
//                         (e << 24) | (f << 16) | (g <<  8) | h);
//
//        if (result != 0x1122334455667788L) { return 1; }
//        return 0;
//    }
//
//    static int unsignedShiftTest() {
//        byte b = -4;
//        short s = -4;
//        char c = 0xfffc;
//        int i = -4;
//
//        b >>>= 4;
//        s >>>= 4;
//        c >>>= 4;
//        i >>>= 4;
//
//        if ((int) b != -1) { return 1; }
//        if ((int) s != -1) { return 2; }
//        if ((int) c != 0x0fff) { return 3; }
//        if (i != 268435455) { return 4; }
//        return 0;
//    }
//
//    static int convTest() {
//
//        float f;
//        double d;
//        int i;
//        long l;
//
//        /* int --> long */
//        i = 7654;
//        l = (long) i;
//        if (l != 7654L) { return 1; }
//
//        i = -7654;
//        l = (long) i;
//        if (l != -7654L) { return 2; }
//
//        /* long --> int (with truncation) */
//        l = 5678956789L;
//        i = (int) l;
//        if (i != 1383989493) { return 3; }
//
//        l = -5678956789L;
//        i = (int) l;
//        if (i != -1383989493) { return 4; }
//        return 0;
//    }
//
//    static int charSubTest() {
//
//        char char1 = 0x00e9;
//        char char2 = 0xffff;
//        int i;
//
//        /* chars are unsigned-expanded to ints before subtraction */
//        i = char1 - char2;
//        if (i != 0xffff00ea) { return 1; }
//        return 0;
//    }
//
//    /*
//     * We pass in the arguments and return the results so the compiler
//     * doesn't do the math for us.  (x=70000, y=-3)
//     */
//    static int intOperTest(int x, int y) {
//        int[] results = new int[10];
//
//        /* this seems to generate "op-int" instructions */
//        results[0] = x + y;
//        results[1] = x - y;
//        results[2] = x * y;
//        results[3] = x * x;
//        results[4] = x / y;
//        results[5] = x % -y;
//        results[6] = x & y;
//        results[7] = x | y;
//        results[8] = x ^ y;
//
//        /* this seems to generate "op-int/2addr" instructions */
//        results[9] = x + ((((((((x + y) - y) * y) / y) % y) & y) | y) ^ y);
//
//        /* check this edge case while we're here (div-int/2addr) */
//        int minInt = -2147483648;
//        int negOne = -results[5];
//        int plusOne = 1;
//        int result = (((minInt + plusOne) - plusOne) / negOne) / negOne;
//
//        if (result != minInt) { return 1;};
//        if (results[0] != 69997) { return 2;};
//        if (results[1] != 70003) { return 3;};
//        if (results[2] != -210000) { return 4;};
//        if (results[3] != 605032704) { return 5;};
//        if (results[4] != -23333) { return 6;};
//        if (results[5] != 1) { return 7;};
//        if (results[6] != 70000) { return 8;};
//        if (results[7] != -3) { return 9;};
//        if (results[8] != -70003) { return 10;};
//        if (results[9] != 70000) { return 11;};
//
//        return 0;
//    }
//
//    /*
//     * More operations, this time with 16-bit constants.  (x=77777)
//     */
//    static int lit16Test(int x) {
//
//        int[] results = new int[8];
//
//        /* try to generate op-int/lit16" instructions */
//        results[0] = x + 1000;
//        results[1] = 1000 - x;
//        results[2] = x * 1000;
//        results[3] = x / 1000;
//        results[4] = x % 1000;
//        results[5] = x & 1000;
//        results[6] = x | -1000;
//        results[7] = x ^ -1000;
//
//        if (results[0] != 78777) { return 1; }
//        if (results[1] != -76777) { return 2; }
//        if (results[2] != 77777000) { return 3; }
//        if (results[3] != 77) { return 4; }
//        if (results[4] != 777) { return 5; }
//        if (results[5] != 960) { return 6; }
//        if (results[6] != -39) { return 7; }
//        if (results[7] != -76855) { return 8; }
//        return 0;
//    }
//
//    /*
//     * More operations, this time with 8-bit constants.  (x=-55555)
//     */
//    static int lit8Test(int x) {
//
//        int[] results = new int[8];
//
//        /* try to generate op-int/lit8" instructions */
//        results[0] = x + 10;
//        results[1] = 10 - x;
//        results[2] = x * 10;
//        results[3] = x / 10;
//        results[4] = x % 10;
//        results[5] = x & 10;
//        results[6] = x | -10;
//        results[7] = x ^ -10;
//        int minInt = -2147483648;
//        int result = minInt / -1;
//        if (result != minInt) {return 1; }
//        if (results[0] != -55545) {return 2; }
//        if (results[1] != 55565) {return 3; }
//        if (results[2] != -555550) {return 4; }
//        if (results[3] != -5555) {return 5; }
//        if (results[4] != -5) {return 6; }
//        if (results[5] != 8) {return 7; }
//        if (results[6] != -1) {return 8; }
//        if (results[7] != 55563) {return 9; }
//        return 0;
//    }
//
//
//    /*
//     * Shift some data.  (value=0xff00aa01, dist=8)
//     */
//    static int intShiftTest(int value, int dist) {
//        int results[] = new int[4];
//        results[0] = value << dist;
//        results[1] = value >> dist;
//        results[2] = value >>> dist;
//        results[3] = (((value << dist) >> dist) >>> dist) << dist;
//        if (results[0] != 0x00aa0100) {return 1; }
//        if (results[1] != 0xffff00aa) {return 2; }
//        if (results[2] != 0x00ff00aa) {return 3; }
//        if (results[3] != 0xaa00) {return 4; }
//        return 0;
//    }
//
//    /*
//     * We pass in the arguments and return the results so the compiler
//     * doesn't do the math for us.  (x=70000000000, y=-3)
//     */
//    static int longOperTest(long x, long y) {
//        long[] results = new long[10];
//
//        /* this seems to generate "op-long" instructions */
//        results[0] = x + y;
//        results[1] = x - y;
//        results[2] = x * y;
//        results[3] = x * x;
//        results[4] = x / y;
//        results[5] = x % -y;
//        results[6] = x & y;
//        results[7] = x | y;
//        results[8] = x ^ y;
//        /* this seems to generate "op-long/2addr" instructions */
//        results[9] = x + ((((((((x + y) - y) * y) / y) % y) & y) | y) ^ y);
//        /* check this edge case while we're here (div-long/2addr) */
//        long minLong = -9223372036854775808L;
//        long negOne = -results[5];
//        long plusOne = 1;
//        long result = (((minLong + plusOne) - plusOne) / negOne) / negOne;
//        if (result != minLong) { return 1; }
//        if (results[0] != 69999999997L) { return 2; }
//        if (results[1] != 70000000003L) { return 3; }
//        if (results[2] != -210000000000L) { return 4; }
//        if (results[3] != -6833923606740729856L) { return 5; }    // overflow
//        if (results[4] != -23333333333L) { return 6; }
//        if (results[5] != 1) { return 7; }
//        if (results[6] != 70000000000L) { return 8; }
//        if (results[7] != -3) { return 9; }
//        if (results[8] != -70000000003L) { return 10; }
//        if (results[9] != 70000000000L) { return 11; }
//        if (results.length != 10) { return 12; }
//        return 0;
//    }
//
//    /*
//     * Shift some data.  (value=0xd5aa96deff00aa01, dist=8)
//     */
//    static long longShiftTest(long value, int dist) {
//        long results[] = new long[4];
//        results[0] = value << dist;
//        results[1] = value >> dist;
//        results[2] = value >>> dist;
//        results[3] = (((value << dist) >> dist) >>> dist) << dist;
//        if (results[0] != 0x96deff00aa010000L) { return 1; }
//        if (results[1] != 0xffffd5aa96deff00L) { return 2; }
//        if (results[2] != 0x0000d5aa96deff00L) { return 3; }
//        if (results[3] != 0xffff96deff000000L) { return 4; }
//        if (results.length != 4) { return 5; }
//
//        return results[0];      // test return-long
//    }
//
//
//    public static void main(String[] args) {
//        int res = unopTest(38);
//        if (res == 37) {
//            System.out.printf("unopTest PASSED\n");
//        } else {
//            System.out.printf("unopTest FAILED: %d\n", res);
//        }
//        res = shiftTest1();
//        if (res == 0) {
//            System.out.printf("shiftTest1 PASSED\n");
//        } else {
//            System.out.printf("shiftTest1 FAILED: %d\n", res);
//        }
//        res = shiftTest2();
//        if (res == 0) {
//            System.out.printf("shiftTest2 PASSED\n");
//        } else {
//            System.out.printf("shiftTest2 FAILED: %d\n", res);
//        }
//        res = unsignedShiftTest();
//        if (res == 0) {
//            System.out.printf("unsignedShiftTest PASSED\n");
//        } else {
//            System.out.printf("unsignedShiftTest FAILED: %d\n", res);
//        }
//        res = convTest();
//        if (res == 0) {
//            System.out.printf("convTest PASSED\n");
//        } else {
//            System.out.printf("convTest FAILED: %d\n", res);
//        }
//        res = charSubTest();
//        if (res == 0) {
//            System.out.printf("charSubTest PASSED\n");
//        } else {
//            System.out.printf("charSubTest FAILED: %d\n", res);
//        }
//        res = intOperTest(70000, -3);
//        if (res == 0) {
//            System.out.printf("intOperTest PASSED\n");
//        } else {
//            System.out.printf("intOperTest FAILED: %d\n", res);
//        }
//        res = longOperTest(70000000000L, -3L);
//        if (res == 0) {
//            System.out.printf("longOperTest PASSED\n");
//        } else {
//            System.out.printf("longOperTest FAILED: %d\n", res);
//        }
//        long lres = longShiftTest(0xd5aa96deff00aa01L, 16);
//        if (lres == 0x96deff00aa010000L) {
//            System.out.printf("longShiftTest PASSED\n");
//        } else {
//            System.out.printf("longShiftTest FAILED: %d\n", res);
//        }
//    }
//}
static const char kIntMathDex[] =
"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"
"Fg6IABMQOACjAAAQExAwAKMCAhDBIBMCKACjAgQCwSATAiAAowIGAsEgEwIYAKMCCALBIBMCEACj"
"AgoCwSATAggAowIMAsEgweAYAoh3ZlVEMyIRMQAAAjgABAASEA8AEgAo/gIAAQAAAAAAqxMAAAQA"
"AAB7EN8AAP8PAAUAAAAAAAAAsxMAABcAAAAUAf///w8TBP8PEvONEI8RjkIyMAQAEhAPADIxBAAS"
"ICj8MkIEABIwKPgSACj2AAABAAAAAAAAAAIAAAAAAAAAAgAAAAEAAQACAAAAAQAAAAIAAAAGAAsA"
"AQAAAAwABjxpbml0PgABSQACSUkAA0lJSQADSUpKAAxJbnRNYXRoLmphdmEAAUoAA0pKSQACTEkA"
"CUxJbnRNYXRoOwADTExMABVMamF2YS9pby9QcmludFN0cmVhbTsAE0xqYXZhL2xhbmcvSW50ZWdl"
"cjsAEkxqYXZhL2xhbmcvT2JqZWN0OwASTGphdmEvbGFuZy9TdHJpbmc7ABJMamF2YS9sYW5nL1N5"
"c3RlbTsAC09iamVjdC5qYXZhAAFWAAJWTAACW0kAAltKABNbTGphdmEvbGFuZy9PYmplY3Q7ABNb"
"TGphdmEvbGFuZy9TdHJpbmc7AAtjaGFyU3ViVGVzdAAXY2hhclN1YlRlc3QgRkFJTEVEOiAlZAoA"
"E2NoYXJTdWJUZXN0IFBBU1NFRAoACGNvbnZUZXN0ABRjb252VGVzdCBGQUlMRUQ6ICVkCgAQY29u"
"dlRlc3QgUEFTU0VECgALaW50T3BlclRlc3QAF2ludE9wZXJUZXN0IEZBSUxFRDogJWQKABNpbnRP"
"cGVyVGVzdCBQQVNTRUQKAAxpbnRTaGlmdFRlc3QACWxpdDE2VGVzdAAIbGl0OFRlc3QADGxvbmdP"
"cGVyVGVzdAAYbG9uZ09wZXJUZXN0IEZBSUxFRDogJWQKABRsb25nT3BlclRlc3QgUEFTU0VECgAN"
"bG9uZ1NoaWZ0VGVzdAAZbG9uZ1NoaWZ0VGVzdCBGQUlMRUQ6ICVkCgAVbG9uZ1NoaWZ0VGVzdCBQ"
"QVNTRUQKAARtYWluAANvdXQABnByaW50ZgAKc2hpZnRUZXN0MQAWc2hpZnRUZXN0MSBGQUlMRUQ6"
"ICVkCgASc2hpZnRUZXN0MSBQQVNTRUQKAApzaGlmdFRlc3QyABZzaGlmdFRlc3QyIEZBSUxFRDog"
"JWQKABJzaGlmdFRlc3QyIFBBU1NFRAoACHVub3BUZXN0ABR1bm9wVGVzdCBGQUlMRUQ6ICVkCgAQ"
"dW5vcFRlc3QgUEFTU0VECgARdW5zaWduZWRTaGlmdFRlc3QAHXVuc2lnbmVkU2hpZnRUZXN0IEZB"
"SUxFRDogJWQKABl1bnNpZ25lZFNoaWZ0VGVzdCBQQVNTRUQKAAd2YWx1ZU9mAAMABw4AAQAHDgBw"
"AAcOAFQABw4tHgIOdwJ0HS0eiVoeeVoeeAB4AgAAB1lNS0tLS1paWlpr4y09WwIMLAJ1HZaWlpaW"
"abSWw9MA3AECAAAHWS1LS0t4exqWlqUAogEBAAdZTUtLS0tLWlpbfwJ5HZaWh4eWlsMAvgEBAAdZ"
"TUtLS0tLWlpdfwJ5HZaWh3iWlsMA7QECAAAHDk1aWlpaWmlaWmriLUsteAIMWQJ1HeHh4eHhtPDD"
"/wERD5YAkwICAAAHWS1LS0t41wJ7HeHh4WoAowIBAAcsaUuZSy2ZSy2ZSy2ZSy2ZSy2Zhy2ZtC2Z"
"tJaZAk4dAREUAREUAREUAREUAREUAREUAREUAREUAA0AB1l9AREPARQPagIOWQJzHXi1ATcXwwJo"
"HQAqAAcOLS0tLS0tLS4BIxGlAAcBAAcOHi0AQAAHaB4eID8aS0wAAAABABCBgATEBQAADgAAgIAE"
"2AUBCPAFAQiEBgEI+AYBCOgJAQj4CgEI9AwBCOwOAQjcEgEJpBQBCLQZAQiUHAEIpB0BCLwdAA0A"
"AAAAAAAAAQAAAAAAAAABAAAAOQAAAHAAAAACAAAADQAAAFQBAAADAAAACQAAAIgBAAAEAAAAAQAA"
"APQBAAAFAAAAEQAAAPwBAAAGAAAAAgAAAIQCAAABIAAADwAAAMQCAAABEAAABgAAAPwOAAACIAAA"
"OQAAACoPAAADIAAADwAAAHASAAAAIAAAAgAAAL8TAAAAEAAAAQAAAAgUAAA=";

static inline DexFile* OpenDexFileBase64(const char* base64,
                                         const std::string& location) {
  CHECK(base64 != NULL);
  size_t length;
  byte* dex_bytes = DecodeBase64(base64, &length);
  CHECK(dex_bytes != NULL);
  DexFile* dex_file = DexFile::OpenPtr(dex_bytes, length, location);
  CHECK(dex_file != NULL);
  return dex_file;
}

class ScratchFile {
 public:
  ScratchFile() {
    std::string filename_template;
    filename_template = getenv("ANDROID_DATA");
    filename_template += "/TmpFile-XXXXXX";
    filename_.reset(strdup(filename_template.c_str()));
    CHECK(filename_ != NULL);
    fd_ = mkstemp(filename_.get());
    CHECK_NE(-1, fd_);
  }

  ~ScratchFile() {
    int unlink_result = unlink(filename_.get());
    CHECK_EQ(0, unlink_result);
    int close_result = close(fd_);
    CHECK_EQ(0, close_result);
  }

  const char* GetFilename() const {
    return filename_.get();
  }

  int GetFd() const {
    return fd_;
  }

 private:
  scoped_ptr_malloc<char> filename_;
  int fd_;
};

class CommonTest : public testing::Test {
 protected:
  virtual void SetUp() {
    is_host_ = getenv("ANDROID_BUILD_TOP") != NULL;

    if (is_host_) {
      // $ANDROID_ROOT is set on the device, but not on the host.
      // We need to set this so that icu4c can find its locale data.
      std::string root;
      root += getenv("ANDROID_BUILD_TOP");
      root += "/out/host/linux-x86";
      setenv("ANDROID_ROOT", root.c_str(), 1);
    }

    android_data_.reset(strdup(is_host_ ? "/tmp/art-data-XXXXXX" : "/sdcard/art-data-XXXXXX"));
    ASSERT_TRUE(android_data_ != NULL);
    const char* android_data_modified = mkdtemp(android_data_.get());
    // note that mkdtemp side effects android_data_ as well
    ASSERT_TRUE(android_data_modified != NULL);
    setenv("ANDROID_DATA", android_data_modified, 1);
    art_cache_.append(android_data_.get());
    art_cache_.append("/art-cache");
    int mkdir_result = mkdir(art_cache_.c_str(), 0700);
    ASSERT_EQ(mkdir_result, 0);

    java_lang_dex_file_.reset(GetLibCoreDex());

    std::vector<const DexFile*> boot_class_path;
    boot_class_path.push_back(java_lang_dex_file_.get());

    runtime_.reset(Runtime::Create(boot_class_path));
    ASSERT_TRUE(runtime_ != NULL);
    class_linker_ = runtime_->GetClassLinker();
  }

  virtual void TearDown() {
    const char* android_data = getenv("ANDROID_DATA");
    ASSERT_TRUE(android_data != NULL);
    DIR* dir = opendir(art_cache_.c_str());
    ASSERT_TRUE(dir != NULL);
    while (true) {
      struct dirent entry;
      struct dirent* entry_ptr;
      int readdir_result = readdir_r(dir, &entry, &entry_ptr);
      ASSERT_EQ(0, readdir_result);
      if (entry_ptr == NULL) {
        break;
      }
      if ((strcmp(entry_ptr->d_name, ".") == 0) || (strcmp(entry_ptr->d_name, "..") == 0)) {
        continue;
      }
      std::string filename(art_cache_);
      filename.push_back('/');
      filename.append(entry_ptr->d_name);
      int unlink_result = unlink(filename.c_str());
      ASSERT_EQ(0, unlink_result);
    }
    closedir(dir);
    int rmdir_cache_result = rmdir(art_cache_.c_str());
    ASSERT_EQ(0, rmdir_cache_result);
    int rmdir_data_result = rmdir(android_data_.get());
    ASSERT_EQ(0, rmdir_data_result);

    // icu4c has a fixed 10-element array "gCommonICUDataArray".
    // If we run > 10 tests, we fill that array and u_setCommonData fails.
    // There's a function to clear the array, but it's not public...
    typedef void (*IcuCleanupFn)();
    void* sym = dlsym(RTLD_DEFAULT, "u_cleanup_" U_ICU_VERSION_SHORT);
    CHECK(sym != NULL);
    IcuCleanupFn icu_cleanup_fn = reinterpret_cast<IcuCleanupFn>(sym);
    (*icu_cleanup_fn)();
  }

  std::string GetLibCoreDexFileName() {
    if (is_host_) {
      const char* host_dir = getenv("ANDROID_HOST_OUT");
      CHECK(host_dir != NULL);
      return StringPrintf("%s/framework/core-hostdex.jar", host_dir);
    }
    return std::string("/system/framework/core.jar");
  }

  DexFile* GetLibCoreDex() {
    std::string libcore_dex_file_name = GetLibCoreDexFileName();
    return DexFile::OpenZip(libcore_dex_file_name);
  }

  PathClassLoader* AllocPathClassLoader(const DexFile* dex_file) {
    class_linker_->RegisterDexFile(dex_file);
    std::vector<const DexFile*> dex_files;
    dex_files.push_back(dex_file);
    return class_linker_->AllocPathClassLoader(dex_files);
  }

  bool is_host_;
  scoped_ptr_malloc<char> android_data_;
  std::string art_cache_;
  scoped_ptr<const DexFile> java_lang_dex_file_;
  scoped_ptr<Runtime> runtime_;
  ClassLinker* class_linker_;
};

}  // namespace art
