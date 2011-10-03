// Copyright 2011 Google Inc. All Rights Reserved.
import java.io.IOException;

public class ExceptionHandle {
    int f() throws Exception {
        try {
            g(1);
        } catch (IOException e) {
            return 1;
        } catch (Exception e) {
            return 2;
        }
        try {
            g(2);
        } catch (IOException e) {
            return 3;
        }
        return 0;
    }
    void g(int doThrow) throws Exception {
        if (doThrow == 1) {
            throw new Exception();
        } else if (doThrow == 2) {
            throw new IOException();
        }
    }
}
