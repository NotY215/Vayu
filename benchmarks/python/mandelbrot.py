# benchmarks/python/mandelbrot.py
W, H, MAX_ITER = 400, 400, 100

total = 0
for py in range(H):
    for px in range(W):
        x0 = (px / W) * 3.5 - 2.5
        y0 = (py / H) * 2.0 - 1.0
        x, y = 0.0, 0.0
        it = 0
        while it < MAX_ITER:
            x2 = x * x
            y2 = y * y
            if x2 + y2 > 4.0:
                break
            y = 2.0 * x * y + y0
            x = x2 - y2 + x0
            it += 1
        total += it

print(total)