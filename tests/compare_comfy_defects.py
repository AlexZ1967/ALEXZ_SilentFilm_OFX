"""Compare native fixed-geometry masks with the actual read-only Comfy particle functions."""

import argparse
import importlib.util
from pathlib import Path
import subprocess
import tempfile
from unittest.mock import patch

import numpy as np
import torch


class Uniforms:
    """Supply an explicit geometry stream instead of comparing different RNG algorithms."""

    def __init__(self, values):
        self.values = iter(values)

    def uniform(self, low, high):
        value = next(self.values)
        if not low <= value <= high:
            raise ValueError("Значение геометрии вне диапазона Comfy RNG")
        return value


class Knot:
    def __init__(self, channel, frame):
        self.channel, self.frame = channel, frame

    def uniform(self, low, high):
        return -0.4 if self.frame == 4 else 0.8

    def random(self):
        return 0.15 if self.frame == 4 else 0.75


def compare(path, expected, tolerance):
    actual = np.fromfile(path, dtype=np.float32).reshape(expected.shape)
    error = np.abs(actual - expected.cpu().numpy())
    if not np.isfinite(actual).all() or error.max() > tolerance:
        raise RuntimeError(f"{path.name}: max_error={error.max():.8g}, допуск={tolerance}")
    return float(error.max())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-test", type=Path, default=Path("build-cuda/silentfilm_defects_tests"))
    parser.add_argument("--reference", type=Path,
                        default=Path("/mnt/comfy/ComfyUI/custom_nodes/ComfyUI_ALEXZ_SilentFilm/utils/film_defects.py"))
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location("comfy_defects_reference", args.reference)
    reference = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reference)
    width, height, irregularity = 31, 19, 0.6
    with tempfile.TemporaryDirectory(prefix="silentfilm-defects-") as temporary:
        folder = Path(temporary)
        subprocess.run([str(args.native_test.resolve()), "--dump-comfy", str(folder)], check=True)
        maximum = 0.0
        for pattern in range(3):
            mask = torch.zeros((height, width), dtype=torch.float32)
            for slot in range(2):
                diameter = (5.5 if pattern == 2 else 1.2 if pattern == 1 else 3.2) if slot == 0 else 0.35
                cx, cy = (7.25, 8.75) if slot == 0 else (8.25, 9.25)
                if pattern == 1:
                    rng = Uniforms([0.9 if slot == 0 else 1.2])
                    with patch.object(reference, "_position", return_value=(cx, cy)), \
                            patch.object(reference, "random_stream", side_effect=lambda seed, channel, frame: Knot(channel, frame)):
                        reference._scratch(mask, rng, 4, 123, diameter, irregularity, 0)
                else:
                    rng = Uniforms([0.3 if slot == 0 else -0.6, 0.7 if slot == 0 else 1.1,
                                    -0.4 if slot == 0 else 0.8, 0.8 if slot == 0 else 0.65])
                    reference._particle(mask, rng, "stain" if pattern == 2 else "dust", cx, cy, diameter, irregularity)
            maximum = max(maximum, compare(folder / f"mask_{pattern}.f32", mask, 5e-6))
            coverage = mask * 0.37
            source = torch.tensor([0.002, 0.3, 0.9]).expand(height, width, 3)
            for material in range(5):
                for damage in range(1, 4):
                    if material in (0, 2, 4) or (damage == 2 and pattern == 0):
                        rgb = source * (1 - coverage[..., None])
                    elif damage == 2:
                        clear = 10 ** -0.1 if material == 1 else 1.0
                        rgb = source + (source.clamp_min(clear) - source) * coverage[..., None]
                    else:
                        rgb = source * torch.pow(10.0, -(1.5 if damage == 3 else 0.65) * coverage[..., None])
                    expected = torch.cat((rgb.clamp(0, 1), torch.full((height, width, 1), 0.5)), dim=-1)
                    maximum = max(maximum, compare(folder / f"image_{pattern}_{material}_{damage}.f32", expected, 5e-6))
        print(f"Comfy dust/scratch/stain masks + 45 material cases: max_error={maximum:.8g}")
        print("Сравнение использует одинаковую явную геометрию; совпадение генераторов RNG и всей цепочки не проверяется.")


if __name__ == "__main__":
    main()
