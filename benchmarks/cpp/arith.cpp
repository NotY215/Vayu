// benchmarks/cpp/arith.cpp
#include <cstdio>

int main() {
    const long long n = 1000000;

    long long sum_int = 0;
    for (long long i = 1; i <= n; ++i)
        sum_int += i * 3 - i / 4;

    double sum_float = 0.0;
    for (long long i = 1; i <= n; ++i) {
        double fi = (double)i;
        sum_float += fi * 1.5 / (fi + 1.0);
    }

    std::printf("%lld\n%.15g\n", sum_int, sum_float);
    return 0;
}