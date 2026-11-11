"""Compare native flow with the actual read-only Comfy CPU or CUDA reference on identical RGBA frames."""

import argparse
import ast
from pathlib import Path
import struct
import subprocess
import tempfile

import cv2
import numpy as np
import torch
import torch.nn.functional as F


def reference_functions(path):
    """Load only sampler definitions from the specified source, avoiding Comfy startup and dependencies."""
    names = {"_frame_to_numpy", "_gray_u8_from_frame", "_dense_pair_flow", "_warp_np_with_flow",
             "_flow_consistency_confidence", "_lerp_frame_flow", "_trim_frame_caches", "_TorchExposureSampler"}
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    definitions = [node for node in tree.body if isinstance(node, (ast.FunctionDef, ast.ClassDef)) and node.name in names]
    if {node.name for node in definitions} != names:
        raise RuntimeError("Не найдены ожидаемые функции optical flow в Comfy-референсе")
    namespace = {"np": np, "torch": torch, "cv2": cv2, "F": F, "math": __import__("math")}
    exec(compile(ast.Module(body=definitions, type_ignores=[]), str(path), "exec"), namespace)
    return namespace


def scene(width, height, displacement):
    """Build a moving textured object, static textured background and a static foreground occluder."""
    y, x = np.mgrid[:height, :width].astype(np.float32)
    base = 0.35 + 0.12 * np.sin(x * 0.31) * np.cos(y * 0.27)
    image = np.stack((base, base * 0.7, base * 0.4 + 0.1, np.ones_like(base)), axis=-1)
    left, top = int(width * 0.2) + displacement, int(height * 0.2)
    mask = (x >= left) & (x < left + width * 0.28) & (y >= top) & (y < height * 0.8)
    texture = 0.65 + 0.18 * np.sin((x - displacement) * 0.41) * np.cos(y * 0.37)
    image[..., 0] = np.where(mask, texture, image[..., 0])
    image[..., 1] = np.where(mask, 0.1 + texture * 0.3, image[..., 1])
    image[..., 2] = np.where(mask, 0.15 + texture * 0.1, image[..., 2])
    pole = (x >= width * 0.43) & (x < width * 0.46)
    image[pole, :3] = np.stack((base, base * 0.8, base * 0.9), axis=-1)[pole]
    return np.ascontiguousarray(image, dtype=np.float32)


def compare(args):
    """Generate reference fixtures temporarily and delegate numeric comparison to the native test executable."""
    functions = reference_functions(args.reference)
    device = torch.device(args.backend)
    if args.backend == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("Для сравнения с CUDA-веткой Comfy нужен доступный CUDA PyTorch")
    print(f"Comfy reference: {args.reference}\nPython OpenCV {cv2.__version__}, backend={args.backend}", flush=True)
    failed = False
    with tempfile.TemporaryDirectory(prefix="silentfilm-comfy-flow-") as temporary:
        for width, height, displacement in ((320, 180, 8), (320, 180, 24), (320, 180, 48), (960, 540, 48)):
            first, second = scene(width, height, 0), scene(width, height, displacement)
            images = torch.from_numpy(np.stack((first, second)))
            caches = ({}, {}, {})
            sampler = functions["_TorchExposureSampler"](images, device, *caches) if args.backend == "cuda" else None
            fixture = Path(temporary) / f"motion-{width}-{displacement}.bin"
            mixes = (0.2, 0.5, 0.8)
            with fixture.open("wb") as output:
                output.write(b"SFF1" + struct.pack("<III", width, height, len(mixes)))
                output.write(first.astype("<f4").tobytes())
                output.write(second.astype("<f4").tobytes())
                for mix in mixes:
                    mix = float(np.float32(mix))
                    if sampler is not None:
                        result = sampler.sample(mix, True)
                    else:
                        result = functions["_lerp_frame_flow"](images, mix, *caches)
                    output.write(struct.pack("<f", mix))
                    output.write(result.detach().cpu().numpy().astype("<f4").tobytes())
            print(f"Static background / moving object / static occluder: displacement={displacement}", flush=True)
            result = subprocess.run([str(args.native_test.resolve()), "--compare-comfy", str(fixture)], check=False)
            failed |= result.returncode != 0
    return 1 if failed else 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--reference", type=Path, default=Path("/mnt/comfy/ComfyUI/custom_nodes/ComfyUI_ALEXZ_SilentFilm/nodes/video_silent_film_cadence.py"))
    parser.add_argument("--native-test", type=Path, default=Path("build-cuda/silentfilm_cadence_flow_tests"))
    parser.add_argument("--backend", choices=("cpu", "cuda"), default="cpu")
    raise SystemExit(compare(parser.parse_args()))
