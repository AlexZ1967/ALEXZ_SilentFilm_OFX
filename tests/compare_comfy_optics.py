"""Compare native camera optics with read-only Comfy functions on identical SDR frames and focus values."""

import argparse
import ast
import math
from pathlib import Path
import struct
import subprocess
import tempfile

import numpy as np
import torch
import torch.nn.functional as F


def reference_functions(path):
    """Extract only spatial lens functions without importing or starting ComfyUI."""
    names = {"_lens_edge_mask", "_lens_gaussian_blur", "_apply_variable_lens_blur", "_apply_lens_edges"}
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    definitions = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name in names]
    if {node.name for node in definitions} != names:
        raise RuntimeError("В референсе не найдены ожидаемые функции оптики")
    namespace = {"torch": torch, "F": F, "math": math, "check_interrupt": lambda: None}
    exec(compile(ast.Module(body=definitions, type_ignores=[]), str(path), "exec"), namespace)
    return namespace


def compare(args):
    """Evaluate actual Comfy RGB optics before luma and compare the native CPU or CUDA output."""
    functions = reference_functions(args.reference)
    device = torch.device(args.backend)
    if args.backend == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("Для CUDA-референса нужен доступный CUDA PyTorch")
    # vignette, edge softness, edge start/end, global softness, focus strength, focus signal.
    cases = (
        (65, 33, (0.6, 0, 0.6, 1, 0, 0, 0)),
        (160, 54, (0, 1, 0.25, 0.95, 0, 0, 0)),
        (257, 43, (0.3, 0.8, 0.25, 0.95, 0.7, 0.5, 0.3)),
        (1920, 54, (0, 0, 0.6, 1, 1, 0, 0)),
        (1920, 54, (0, 0, 0.6, 1, 0, 0.5, -1)),
        (1920, 54, (0, 0, 0.6, 1, 0, 0.5, 1)),
        (73, 2, (0.2, 1, 0.6, 1, 0.5, 0.5, 0.3)),
        (1, 1, (0.2, 0.3, 0.6, 1, 0.5, 0.5, 0)),
        (320, 180, (0.12, 0.15, 0.6, 1, 0.1, 0.03, 0.4)),
    )
    print(f"Comfy reference: {args.reference}\nTorch {torch.__version__}, backend={args.backend}", flush=True)
    failed = False
    with tempfile.TemporaryDirectory(prefix="silentfilm-comfy-optics-") as temporary:
        for index, (width, height, settings) in enumerate(cases):
            settings = tuple(float(np.float32(value)) for value in settings)
            vignette, edge, start, end, softness, focus, signal = settings
            y, x = np.mgrid[:height, :width].astype(np.float32)
            pattern = 0.5 + 0.3 * np.sin(x * 0.71) * np.cos(y * 0.39)
            image = np.stack((pattern, 1 - pattern, 0.2 + 0.5 * pattern, ((x + y) % 5) / 4), axis=-1)
            image = np.ascontiguousarray(image, dtype=np.float32)
            rgba = torch.from_numpy(image).unsqueeze(0).to(device)
            rgb = rgba[..., :3]
            if vignette or edge:
                mask = functions["_lens_edge_mask"](height, width, start, device, rgb.dtype, end)
                rgb = functions["_apply_lens_edges"](rgb, vignette, edge, start, mask, end)
            if softness:
                sigma = max(0.1, width / 1920.0 * softness * 4)
                rgb = functions["_lens_gaussian_blur"](rgb, sigma)
            if focus and signal > -1:
                mask = torch.full((1, 1, 1, 1), (signal + 1) / 2, device=device, dtype=rgb.dtype)
                rgb = functions["_apply_variable_lens_blur"](rgb, mask, width / 1920.0 * focus * 8)
            gray = rgb[..., :1] * 0.299 + rgb[..., 1:2] * 0.587 + rgb[..., 2:3] * 0.114
            expected = torch.cat((gray.expand(-1, -1, -1, 3), rgba[..., 3:4]), dim=-1)
            fixture = Path(temporary) / f"optics-{index}.bin"
            with fixture.open("wb") as output:
                output.write(b"SFO1" + struct.pack("<II7f", width, height, *settings))
                output.write(image.astype("<f4").tobytes())
                output.write(expected.detach().cpu().numpy().astype("<f4").tobytes())
            print(f"Camera Optics {width}x{height}, parameters={settings}", flush=True)
            result = subprocess.run([str(args.native_test.resolve()), "--compare-comfy", str(fixture)], check=False)
            failed |= result.returncode != 0
    return int(failed)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend", choices=("cpu", "cuda"), default="cpu")
    parser.add_argument("--native-test", type=Path)
    parser.add_argument("--reference", type=Path, default=Path(
        "/mnt/comfy/ComfyUI/custom_nodes/ComfyUI_ALEXZ_SilentFilm/nodes/video_silent_film_finish.py"))
    arguments = parser.parse_args()
    if arguments.native_test is None:
        name = "silentfilm_camera_optics_cuda_tests" if arguments.backend == "cuda" else "silentfilm_camera_optics_tests"
        arguments.native_test = Path("build-cuda") / name
    raise SystemExit(compare(arguments))
