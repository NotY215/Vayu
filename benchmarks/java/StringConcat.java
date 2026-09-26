// benchmarks/java/StringConcat.java
public class StringConcat {
    public static void main(String[] args) {
        final int N = 100000;
        final String chunk = "abcdefghijklmnop";
        StringBuilder sb = new StringBuilder(N * 16);
        for (int i = 0; i < N; i++) sb.append(chunk);
        System.out.println(sb.length());
    }
}