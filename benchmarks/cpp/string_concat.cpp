// benchmarks/cpp/string_concat.cpp
#include <cstdio>
#include <string>

int main() {
    const int N = 100000;
    const char* chunk = "abcdefghijklmnop";
    std::string s;
    s.reserve((size_t)N * 16);
    for (int i = 0; i < N; ++i) s += chunk;
    std::printf("%zu\n", s.size());
    return 0;
}