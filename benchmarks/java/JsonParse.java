// benchmarks/java/JsonParse.java
//
// Hand-written scanner mirroring the Vayu/C++ versions.  No third-party
// JSON library, so we compare parsers, not libraries.

public class JsonParse {
    static long parseLong(String s, int[] pos) {
        long v = 0;
        int i = pos[0];
        while (i < s.length() && s.charAt(i) >= '0' && s.charAt(i) <= '9') {
            v = v * 10 + (s.charAt(i) - '0');
            i++;
        }
        pos[0] = i;
        return v;
    }
    public static void main(String[] args) {
        final int N = 20000;
        StringBuilder sb = new StringBuilder();
        sb.append('[');
        for (int i = 0; i < N; i++) {
            if (i > 0) sb.append(',');
            sb.append("{\"id\":").append(i)
              .append(",\"name\":\"user_").append(i)
              .append("\",\"score\":").append(((long)i * 7) % 1000).append('}');
        }
        sb.append(']');
        String doc = sb.toString();

        long total = 0;
        int count = 0;
        int[] pos = new int[]{0};
        int n = doc.length();
        int i = 0;
        while (i < n && doc.charAt(i) != '[') i++;
        i++;
        while (i < n && doc.charAt(i) != ']') {
            if (doc.charAt(i) != '{') { i++; continue; }
            i++;
            long id = 0;
            while (i < n && doc.charAt(i) != '}') {
                if (doc.charAt(i) == '"') {
                    i++;
                    int ks = i;
                    while (doc.charAt(i) != '"') i++;
                    String key = doc.substring(ks, i);
                    i++;
                    while (i < n && doc.charAt(i) != ':') i++;
                    i++;
                    pos[0] = i;
                    long v = parseLong(doc, pos);
                    i = pos[0];
                    if (key.equals("id")) id = v;
                    while (i < n && doc.charAt(i) != ',' && doc.charAt(i) != '}') i++;
                } else i++;
            }
            total += id;
            count++;
            if (i < n) i++;
        }
        System.out.println(count);
        System.out.println(total);
    }
}