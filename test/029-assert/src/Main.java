// Copyright 2007 The Android Open Source Project

/**
 * Test Java language asserts.
 */
public class Main {
    public static void main(String[] args) {
        assert true;
        try {
            assert false;
            System.out.println("didn't assert (is '-ea' implemented?)");
        } catch (AssertionError ae) {
            System.out.println("caught expected assert exception");
        }
    }
}
