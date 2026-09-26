// benchmarks/java/Hashmap.java
import java.util.HashMap;

public class Hashmap {
    static String keyOf(int i) {
        long h = (long)i * 2654435761L % 1000000L;
        return "key_" + h;
    }
    public static void main(String[] args) {
        final int N = 500000;
        HashMap<String, Long> m = new HashMap<>(N * 2);
        for (int i = 0; i < N; i++) m.put(keyOf(i), (long)i);

        long total = 0;
        for (int i = 0; i < N; i++) {
            Long v = m.get(keyOf(i));
            if (v != null) total += v;
        }
        System.out.println(total);
        System.out.println(m.size());
    }
}