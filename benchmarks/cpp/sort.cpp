// benchmarks/cpp/sort.cpp
#include <cstdio>
#include <vector>

static void quicksort(std::vector<long long>& a, int lo, int hi) {
    if (lo >= hi) return;
    long long pivot = a[(lo + hi) / 2];
    int i = lo, j = hi;
    while (i <= j) {
        while (a[i] < pivot) ++i;
        while (a[j] > pivot) --j;
        if (i <= j) {
            long long t = a[i]; a[i] = a[j]; a[j] = t;
            ++i; --j;
        }
    }
    if (lo < j) quicksort(a, lo, j);
    if (i < hi) quicksort(a, i, hi);
}

int main() {
    const int N = 500000;
    std::vector<long long> a;
    a.reserve(N);
    long long seed = 12345;
    for (int i = 0; i < N; ++i) {
        seed = (seed * 1103515245LL + 12345LL) % 2147483648LL;
        a.push_back(seed % 1000000);
    }
    quicksort(a, 0, N - 1);
    long long checksum = 0;
    for (int i = 0; i < N; ++i)
        checksum = (checksum + a[i] * (i + 1)) % 1000000007;
    std::printf("%lld\n", checksum);
    return 0;
}