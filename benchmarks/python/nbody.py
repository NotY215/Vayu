# benchmarks/python/nbody.py
import math

BODIES = 5
STEPS  = 100000
DT     = 0.01

x  = [0.0, 1.0, 2.0, 3.0, 4.0]
y  = [0.0, -1.0, 0.5, -0.5, 2.0]
z  = [0.0, 1.5, -1.5, 0.5, -2.0]
vx = [0.0, 0.1, -0.1, 0.2, -0.2]
vy = [0.1, 0.0, 0.2, -0.1, -0.2]
vz = [-0.1, 0.2, 0.0, -0.2, 0.1]
mass = [1.0, 2.0, 3.0, 2.5, 1.5]

for _ in range(STEPS):
    for i in range(BODIES):
        for j in range(i + 1, BODIES):
            dx = x[i] - x[j]
            dy = y[i] - y[j]
            dz = z[i] - z[j]
            d2 = dx*dx + dy*dy + dz*dz + 0.01
            inv = 1.0 / (d2 * math.sqrt(d2))
            fi = mass[j] * inv
            fj = mass[i] * inv
            vx[i] -= dx * fi * DT; vy[i] -= dy * fi * DT; vz[i] -= dz * fi * DT
            vx[j] += dx * fj * DT; vy[j] += dy * fj * DT; vz[j] += dz * fj * DT
    for i in range(BODIES):
        x[i] += vx[i] * DT
        y[i] += vy[i] * DT
        z[i] += vz[i] * DT

e = 0.0
for i in range(BODIES):
    e += 0.5 * mass[i] * (vx[i]*vx[i] + vy[i]*vy[i] + vz[i]*vz[i])
print(f"{e:.6f}")