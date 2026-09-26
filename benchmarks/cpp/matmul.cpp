// benchmarks/cpp/matmul.cpp
#include <cstdio>
#include <vector>

int main() {
    const int N = 128;
    std::vector<double> a(N * N), b(N * N), c(N * N);

    for (int i = 0; i < N * N; ++i) {
        a[i] = (double)((i * 7) % 100) * 0.01;
        b[i] = (double)((i * 13) % 100) * 0.01;
    }

    for (int ii = 0; ii < N; ++ii) {
        for (int jj = 0; jj < N; ++jj) {
            double s = 0.0;
            for (int kk = 0; kk < N; ++kk) {
                s += a[ii * N + kk] * b[kk * N + jj];
            }
            c[ii * N + jj] = s;
        }
    }

    double total = 0.0;
    for (int i = 0; i < N * N; ++i) total += c[i];
    std::printf("%.6f\n", total);
    return 0;
}