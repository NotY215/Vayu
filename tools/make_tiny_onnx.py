#!/usr/bin/env python3
"""Generates examples/assets/tiny_mlp.onnx.

A 4-input -> 3-hidden -> 1-output MLP with ReLU and no bias, weights
chosen so that for x = [1, 0, 0, 0] the output is exactly 4.0.

Requires the `onnx` pip package:
    pip install onnx

Run from the repo root:
    python tools/make_tiny_onnx.py
"""
import os
import numpy as np
import onnx
from onnx import helper, TensorProto

# With transB=1, h1[j] = sum_k x[k] * W1[j, k].
# To have all three hidden units fire on x = [1, 0, 0, 0], set column 0 to
# 1 for every row and the rest to 0.
W1 = np.array([[1, 0, 0, 0],
               [1, 0, 0, 0],
               [1, 0, 0, 0]], dtype=np.float32)  # [3, 4]
# With transB=1, ONNX expects the weight at shape [N, K] = [1, 3].
W2 = np.array([[1, 1, 2]], dtype=np.float32)      # [1, 3]

gemm1 = helper.make_node("Gemm", ["x", "W1"], ["h1"],
                         transB=1, alpha=1.0, beta=0.0, name="gemm1")
relu  = helper.make_node("Relu", ["h1"], ["h2"], name="relu1")
gemm2 = helper.make_node("Gemm", ["h2", "W2"], ["y"],
                         transB=1, alpha=1.0, beta=0.0, name="gemm2")

init1 = helper.make_tensor("W1", TensorProto.FLOAT, W1.shape,
                           W1.flatten().tolist())
init2 = helper.make_tensor("W2", TensorProto.FLOAT, W2.shape,
                           W2.flatten().tolist())

graph = helper.make_graph(
    [gemm1, relu, gemm2], "tiny_mlp",
    inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 4])],
    outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 1])],
    initializer=[init1, init2],
)

model = helper.make_model(
    graph,
    opset_imports=[helper.make_opsetid("", 13)],
    ir_version=8,
    producer_name="vayu-tools",
)

out_dir = os.path.join("examples", "assets")
os.makedirs(out_dir, exist_ok=True)
out = os.path.join(out_dir, "tiny_mlp.onnx")
onnx.save(model, out)
print("wrote", out)