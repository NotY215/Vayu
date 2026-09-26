# benchmarks/python/json_parse.py
import json as _json

N = 20000

doc = "[" + ",".join(
    f'{{"id":{i},"name":"user_{i}","score":{(i * 7) % 1000}}}'
    for i in range(N)
) + "]"

parsed = _json.loads(doc)
total = sum(item["id"] for item in parsed)
print(len(parsed))
print(total)