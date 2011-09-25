

public class MemUsage {
  public static final int NUM_1D_ARRAYS = 1000;
  public static final int INCREMENT = 300;

  public static void main(String [] args) {
    int sz = 1000;
    double[][] RelocationArray = new double[NUM_1D_ARRAYS][];
    while (true) {
      for (int i = 0; i < NUM_1D_ARRAYS; i++) {
        RelocationArray[i] = new double[sz];
        if (sz + INCREMENT > 0) {
          sz += INCREMENT;
        }
      }
    }
  }
}
