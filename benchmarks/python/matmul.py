# benchmarks/python/matmul.py
N = 128

a = [((i * 7) % 100) * 0.01 for i in range(N * N)]
b = [((i * 13) % 100) * 0.01 for i in range(N * N)]
c = [0.0] * (N * N)

for ii in range(N):
    for jj in range(N):
        s = 0.0
        for kk in range(N):
            s += a[ii * N + kk] * b[kk * N + jj]
        c[ii * N + jj] = s

print(f"{sum(c):.6f}")