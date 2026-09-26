// benchmarks/cpp/mandelbrot.cpp
#include <cstdio>

int main() {
    const int W = 400, H = 400, MAX_ITER = 100;
    long long total = 0;
    for (int py = 0; py < H; ++py) {
        for (int px = 0; px < W; ++px) {
            double x0 = ((double)px / (double)W) * 3.5 - 2.5;
            double y0 = ((double)py / (double)H) * 2.0 - 1.0;
            double x = 0.0, y = 0.0;
            int it = 0;
            while (it < MAX_ITER) {
                double x2 = x * x;
                double y2 = y * y;
                if (x2 + y2 > 4.0) break;
                y = 2.0 * x * y + y0;
                x = x2 - y2 + x0;
                ++it;
            }
            total += it;
        }
    }
    std::printf("%lld\n", total);
    return 0;
}