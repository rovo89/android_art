// Copyright 2011 Google Inc. All Rights Reserved.

class ProtoCompare {
    int m1(short x, int y, long z) { return x + y + (int)z; }
    int m2(short x, int y, long z) { return x + y + (int)z; }
    int m3(long x, int y, short z) { return (int)x + y + z; }
    long m4(long x, int y, short z) { return x + y + z; }
}
