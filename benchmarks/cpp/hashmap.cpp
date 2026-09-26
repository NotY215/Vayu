// benchmarks/cpp/hashmap.cpp
#include <cstdio>
#include <string>
#include <unordered_map>

static std::string key_of(int i) {
    long long h = (long long)i * 2654435761LL % 1000000LL;
    return "key_" + std::to_string(h);
}

int main() {
    const int N = 500000;
    std::unordered_map<std::string, long long> m;
    m.reserve((size_t)N * 2);
    for (int i = 0; i < N; ++i) m[key_of(i)] = i;

    long long total = 0;
    for (int i = 0; i < N; ++i) {
        auto it = m.find(key_of(i));
        if (it != m.end()) total += it->second;
    }
    std::printf("%lld\n%zu\n", total, m.size());
    return 0;
}