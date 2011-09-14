// Copyright 2011 Google Inc. All Rights Reserved.

class ExceptionTest {

    public int ifoo;

    /* Test requires visual inspection of object code to verify */
    int noThrow(ExceptionTest nonNullA,
                ExceptionTest nonNullB,
                ExceptionTest nonNullC) {

        // "this" check should be eliminated on both IGET/IPUT
        ifoo++;

       // "this" check should be eliminated on both IGET/IPUT
       if (ifoo != 321) {
           // Check not eliminated
           nonNullA.ifoo = 12;
           // Check not eliminated
           nonNullB.ifoo = 21;
       } else {
           // Check not eliminated
           nonNullA.ifoo = 12;
       }

       // Check eliminated
       nonNullA.ifoo = 13;

       // Check not eliminated
       nonNullB.ifoo = 21;

       nonNullC = nonNullB;

       // Check eliminated
       nonNullC.ifoo = 32;

      // All null checks eliminated
      return ifoo + nonNullA.ifoo + nonNullB.ifoo + nonNullC.ifoo;
    }

    /* Test to ensure we don't remove necessary null checks */
    int checkThrow(ExceptionTest nonNullA,
                   ExceptionTest nonNullB,
                   ExceptionTest nonNullC,
                   ExceptionTest nullA,
                   ExceptionTest nullB,
                   ExceptionTest nullC) {

        // "this" check should be eliminated on both IGET/IPUT
        ifoo++;

       try {
           nullA.ifoo = 12;
           // Should not be reached
           return -1;
       } catch (NullPointerException npe) {
           ifoo++;
       }
       try {
           nullB.ifoo = 13;
           // Should not be reached
           return -2;
       } catch (NullPointerException npe) {
           ifoo++;
       }
       try {
           nullC.ifoo = 14;
           // Should not be reached
           return -3;
       } catch (NullPointerException npe) {
           ifoo++;
       }

       // "this" check should be eliminated
       if (ifoo != 321) {
           // Check not eliminated
           nonNullA.ifoo = 12;
           // Check not eliminated
           nonNullB.ifoo = 21;
           // Should throw here
           try {
               nullA.ifoo = 11;
               return -4;
           } catch (NullPointerException npe) {
           }
       } else {
           // Check not eliminated
           nonNullA.ifoo = 12;
           // Should throw here
           try {
               nullA.ifoo = 11;
               return -5;
           } catch (NullPointerException npe) {
           }
       }

       // Check not eliminated
       nonNullA.ifoo = 13;

       // Check not eliminated
       nonNullB.ifoo = 21;

       nonNullC = nonNullB;

       // Check eliminated
       nonNullC.ifoo = 32;

       // Should throw here
       try {
           nullA.ifoo = 13;
           return -6;
       } catch (NullPointerException npe) {
       }

      return ifoo + nonNullA.ifoo + nonNullB.ifoo + nonNullC.ifoo;
    }


    static int nullCheckTestNoThrow(int x) {
        ExceptionTest base = new ExceptionTest();
        ExceptionTest a = new ExceptionTest();
        ExceptionTest b = new ExceptionTest();
        ExceptionTest c = new ExceptionTest();
        base.ifoo = x;
        return base.noThrow(a,b,c);
    }

    static int nullCheckTestThrow(int x) {
        ExceptionTest base = new ExceptionTest();
        ExceptionTest a = new ExceptionTest();
        ExceptionTest b = new ExceptionTest();
        ExceptionTest c = new ExceptionTest();
        ExceptionTest d = null;
        ExceptionTest e = null;
        ExceptionTest f = null;
        base.ifoo = x;
        return base.checkThrow(a,b,c,d,e,f);
    }


    public static void main(String[] args) {
        int res;

        res = nullCheckTestNoThrow(1976);
        if (res == 2054) {
            System.out.println("nullCheckTestNoThrow PASSED");
        } else {
            System.out.println("nullCheckTestNoThrow FAILED: " + res);
        }

        res = nullCheckTestThrow(1976);
        if (res == 2057) {
            System.out.println("nullCheckTestNoThrow PASSED");
        } else {
            System.out.println("nullCheckTestNoThrow FAILED: " + res);
        }
    }
}
