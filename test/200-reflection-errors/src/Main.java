import java.lang.reflect.Field;
import java.lang.reflect.Method;

public class Main {
  public static void main(String[] args) throws Exception {
    // Can't assign Integer to a String field.
    try {
      Field field = A.class.getField("b");
      field.set(new A(), 5);
    } catch (IllegalArgumentException expected) {
      System.out.println(expected.getMessage());
    }

    // Can't unbox null to a primitive.
    try {
      Field field = A.class.getField("i");
      field.set(new A(), null);
    } catch (IllegalArgumentException expected) {
      System.out.println(expected.getMessage());
    }

    // Can't unbox String to a primitive.
    try {
      Field field = A.class.getField("i");
      field.set(new A(), "hello, world!");
    } catch (IllegalArgumentException expected) {
      System.out.println(expected.getMessage());
    }

    // Can't pass an Integer as a String.
    try {
      Method m = A.class.getMethod("m", int.class, String.class);
      m.invoke(new A(), 2, 2);
    } catch (IllegalArgumentException expected) {
      System.out.println(expected.getMessage());
    }

    // Can't pass null as an int.
    try {
      Method m = A.class.getMethod("m", int.class, String.class);
      m.invoke(new A(), null, "");
    } catch (IllegalArgumentException expected) {
      System.out.println(expected.getMessage());
    }
  }
}

class A {
  public String b;
  public int i;
  public void m(int i, String s) {}
}
