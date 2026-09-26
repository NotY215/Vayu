// benchmarks/java/Matmul.java
public class Matmul {
    public static void main(String[] args) {
        final int N = 128;
        double[] a = new double[N * N];
        double[] b = new double[N * N];
        double[] c = new double[N * N];

        for (int i = 0; i < N * N; i++) {
            a[i] = (double)((i * 7) % 100) * 0.01;
            b[i] = (double)((i * 13) % 100) * 0.01;
        }

        for (int ii = 0; ii < N; ii++) {
            for (int jj = 0; jj < N; jj++) {
                double s = 0.0;
                for (int kk = 0; kk < N; kk++) {
                    s += a[ii * N + kk] * b[kk * N + jj];
                }
                c[ii * N + jj] = s;
            }
        }

        double total = 0.0;
        for (int i = 0; i < N * N; i++) total += c[i];
        System.out.printf("%.6f%n", total);
    }
}