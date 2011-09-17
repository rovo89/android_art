// Copyright 2011 Google Inc. All Rights Reserved.

class Fibonacci {

    static int fibonacci(int n) {
        if (n == 0) {
            return 0;
        }
        int x = 1;
        int y = 1;
        for (int i = 3; i <= n; i++) {
            int z = x + y;
            x = y;
            y = z;
        }
        return y;
    }

    public static void main(String[] args) {
        try {
            if (args.length == 1) {
                int x = Integer.parseInt(args[0]);
                int y = fibonacci(x); /* to warm up cache */
                System.out.printf("fibonacci(%d)=%d\n", x, y);
                y = fibonacci(x + 1);
                System.out.printf("fibonacci(%d)=%d\n", x + 1, y);
            }
        } catch (NumberFormatException ex) {}
    }
}
