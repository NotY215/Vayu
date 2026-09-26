// benchmarks/java/Sort.java
public class Sort {
    static void quicksort(long[] a, int lo, int hi) {
        if (lo >= hi) return;
        long pivot = a[(lo + hi) / 2];
        int i = lo, j = hi;
        while (i <= j) {
            while (a[i] < pivot) i++;
            while (a[j] > pivot) j--;
            if (i <= j) {
                long t = a[i]; a[i] = a[j]; a[j] = t;
                i++; j--;
            }
        }
        if (lo < j) quicksort(a, lo, j);
        if (i < hi) quicksort(a, i, hi);
    }
    public static void main(String[] args) {
        final int N = 500000;
        long[] a = new long[N];
        long seed = 12345;
        for (int i = 0; i < N; i++) {
            seed = (seed * 1103515245L + 12345L) % 2147483648L;
            a[i] = seed % 1000000;
        }
        quicksort(a, 0, N - 1);
        long checksum = 0;
        for (int i = 0; i < N; i++)
            checksum = (checksum + a[i] * (i + 1)) % 1000000007;
        System.out.println(checksum);
    }
}