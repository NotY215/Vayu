// benchmarks/cpp/nbody.cpp
#include <cstdio>
#include <cmath>

int main() {
    const int BODIES = 5;
    const int STEPS = 100000;
    const double DT = 0.01;

    double x[BODIES] = { 0.0, 1.0, 2.0, 3.0, 4.0 };
    double y[BODIES] = { 0.0, -1.0, 0.5, -0.5, 2.0 };
    double z[BODIES] = { 0.0, 1.5, -1.5, 0.5, -2.0 };
    double vx[BODIES] = { 0.0, 0.1, -0.1, 0.2, -0.2 };
    double vy[BODIES] = { 0.1, 0.0, 0.2, -0.1, -0.2 };
    double vz[BODIES] = { -0.1, 0.2, 0.0, -0.2, 0.1 };
    double mass[BODIES] = { 1.0, 2.0, 3.0, 2.5, 1.5 };

    for (int step = 0; step < STEPS; ++step) {
        for (int i = 0; i < BODIES; ++i) {
            for (int j = i + 1; j < BODIES; ++j) {
                double dx = x[i] - x[j];
                double dy = y[i] - y[j];
                double dz = z[i] - z[j];
                double d2 = dx * dx + dy * dy + dz * dz + 0.01;
                double inv = 1.0 / (d2 * std::sqrt(d2));
                double fi = mass[j] * inv;
                double fj = mass[i] * inv;
                vx[i] -= dx * fi * DT; vy[i] -= dy * fi * DT; vz[i] -= dz * fi * DT;
                vx[j] += dx * fj * DT; vy[j] += dy * fj * DT; vz[j] += dz * fj * DT;
            }
        }
        for (int i = 0; i < BODIES; ++i) {
            x[i] += vx[i] * DT;
            y[i] += vy[i] * DT;
            z[i] += vz[i] * DT;
        }
    }

    double e = 0.0;
    for (int i = 0; i < BODIES; ++i)
        e += 0.5 * mass[i] * (vx[i] * vx[i] + vy[i] * vy[i] + vz[i] * vz[i]);
    std::printf("%.6f\n", e);
    return 0;
}