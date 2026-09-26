// benchmarks/java/Arith.java
public class Arith {
    public static void main(String[] args) {
        final long n = 1000000L;

        long sumInt = 0;
        for (long i = 1; i <= n; i++)
            sumInt += i * 3 - i / 4;

        double sumFloat = 0.0;
        for (long i = 1; i <= n; i++) {
            double fi = (double)i;
            sumFloat += fi * 1.5 / (fi + 1.0);
        }

        System.out.println(sumInt);
        System.out.println(sumFloat);
    }
}