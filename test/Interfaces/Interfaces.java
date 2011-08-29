// Copyright 2011 Google Inc. All Rights Reserved.

class Interfaces {
    interface I {
        public void i();
    }
    interface J {
        public void j1();
        public void j2();
    }
    class A implements I, J {
        public void i() {};
        public void j1() {};
        public void j2() {};
    }
}
