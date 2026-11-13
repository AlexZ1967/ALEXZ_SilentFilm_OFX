"""Replay actual Comfy development_field with native exported knots to compare CPU/CUDA math independently of RNG."""

import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
from unittest.mock import patch

import numpy as np
import torch


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", choices=("cpu", "cuda"), default="cpu")
    parser.add_argument("--native-test", type=Path)
    parser.add_argument("--reference", type=Path,
                        default=Path("/mnt/comfy/ComfyUI/custom_nodes/ComfyUI_ALEXZ_SilentFilm/utils/film_defects.py"))
    args = parser.parse_args()
    native = args.native_test or Path("build-cuda") / ("silentfilm_defects_cuda_tests" if args.backend == "cuda" else "silentfilm_defects_tests")
    spec = importlib.util.spec_from_file_location("comfy_development_reference", args.reference)
    reference = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reference)
    with tempfile.TemporaryDirectory(prefix="silentfilm-development-") as temporary:
        folder = Path(temporary)
        subprocess.run([str(native.resolve()), "--dump-development", str(folder)], check=True)
        maximum_field = maximum_image = 0.0
        for case in json.loads((folder / "cases.json").read_text()):
            number, height, width = case["number"], case["height"], case["width"]
            streams = []

            class Stream:
                def __init__(self, values):
                    self.values, self.index = values, 0

                def uniform(self, low, high):
                    if (low, high) != (-1, 1):
                        raise RuntimeError("Изменился контракт random_stream референса")
                    result = float(self.values[self.index])
                    self.index += 1
                    return result

            def random_stream(seed, channel, knot):
                if seed != case["seed"]:
                    raise RuntimeError("Изменился seed референса")
                if knot == "static":
                    suffix = "static"
                elif knot == case["knot"]:
                    suffix = "a"
                elif knot == case["knot"] + 1:
                    suffix = "b"
                else:
                    raise RuntimeError("Номер временной сетки native/reference не совпал")
                stream = Stream(np.fromfile(folder / f"{number}_{channel}_{suffix}.f32", dtype=np.float32))
                streams.append(stream)
                return stream

            with patch.object(reference, "random_stream", side_effect=random_stream):
                field = reference.development_field(height, width, case["frame"], case["seed"],
                    ("mottling", "streaks", "mixed")[case["mode"] - 1], case["size"], case["persistence"], case["irregularity"], "cpu")
            if any(stream.index != len(stream.values) for stream in streams):
                raise RuntimeError("Размеры coarse grid native/reference не совпали")
            native_field = np.fromfile(folder / f"{number}_field.f32", dtype=np.float32).reshape(height, width)
            field_error = float(np.abs(native_field - field.numpy()).max())
            density = case["strength"] * (0.35 * field + 0.3 * case["fog"] * (field + 1) / 2)
            rgb = (torch.tensor([0.002, 0.3, 0.9]) * torch.pow(10.0, -density[..., None])).clamp(0, 1)
            expected = torch.cat((rgb, torch.full((height, width, 1), 0.5)), dim=-1).numpy()
            actual = np.fromfile(folder / f"{number}_image.f32", dtype=np.float32).reshape(height, width, 4)
            image_error = float(np.abs(actual - expected).max())
            if not np.isfinite(actual).all() or not np.isfinite(native_field).all() or field_error > 2e-5 or image_error > 1e-5:
                raise RuntimeError(f"Case {number}: field_error={field_error:.8g}, image_error={image_error:.8g}")
            maximum_field, maximum_image = max(maximum_field, field_error), max(maximum_image, image_error)
        print(f"Comfy development_field, backend={args.backend}, cases=27: field_max={maximum_field:.8g}, image_max={maximum_image:.8g}")
        print("Общие сырые knot grids: проверяются temporal/bicubic/clamp/density; равенство RNG не заявляется.")


if __name__ == "__main__":
    main()
