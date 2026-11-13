"""Compare workflow presets, grain structure and projection against the actual read-only Comfy source."""

import argparse
import ast
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile
from unittest.mock import patch


def workflow_nodes(path):
    """Read either the original workflow or its checked-in parameter snapshot."""
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schema") == 1:
        return {n["node"]: dict(n, id=n["node"], widgets_values=n["widgets"]) for n in data["nodes"]}
    nodes = {n["id"]: n for n in data["nodes"]}
    links = {link[0]: link for link in data["links"]}
    for node in nodes.values():
        node["linked_seeds"] = {}
        for socket in node.get("inputs", []):
            if socket["name"] == "seed" and socket.get("link") in links:
                origin = nodes[links[socket["link"]][1]]
                node["linked_seeds"]["seed"] = origin["widgets_values"][0]
    return nodes


def check_presets(args):
    """Check production preset values against serialized widgets and linked PrimitiveInt seeds."""
    native = json.loads(subprocess.check_output([str(args.native_test.resolve()), "--dump-presets"], text=True))
    nodes = workflow_nodes(args.reference)
    mappings = {
        "camera": (2, {"camera_softness": 10, "vignette_strength": 11, "edge_softness": 12, "edge_start": 13, "edge_end": 14, "focus_drift": 15}),
        "negative": (3, {"encoding": "srgb", "exposure_ev": 1, "halation_strength": 2, "halation_size": 3, "contrast": 4,
                         "shadow_softness": 5, "highlight_softness": 6, "fog_strength": 7, "grain_strength": 9, "grain_size": 10}),
        "print": (4, {"encoding": "from_negative", "exposure_ev": 1, "halation_strength": 2, "halation_size": 3, "contrast": 4,
                      "shadow_softness": 5, "highlight_softness": 6, "fog_strength": 7, "grain_strength": 10, "grain_size": 11}),
        "projector": (5, {"gate_weave_px": 1, "glow_strength": 3, "glow_size": 4, "lamp_flicker": 9, "lamp_breathing": 10}),
    }
    for stage, (identity, mapping) in mappings.items():
        node = nodes[identity]
        values = native[stage][0]["values"]
        for key, index in mapping.items():
            if isinstance(index, str):
                if node["widgets_values"][0] != index:
                    raise RuntimeError(f"Изменилось кодирование reference {stage}")
                expected = 1
            else:
                expected = node["widgets_values"][index]
            if not math.isclose(values[key], expected, abs_tol=1e-9):
                raise RuntimeError(f"Preset {stage}/{key}: native={values[key]}, Comfy={expected}")
        seed_key = "focus_seed" if stage == "camera" else "projector_seed" if stage == "projector" else "grain_seed"
        if values[seed_key] != node["linked_seeds"]["seed"]:
            raise RuntimeError(f"Не учтён подключённый seed ноды {identity}")
    for preset in native["defects"]:
        if preset["node"] == 0:
            if preset["values"] != {"encoding": 1}:
                raise RuntimeError("Comfy color only должен менять только encoding на sRGB")
            continue
        node = nodes[preset["node"]]; widgets = node["widgets_values"]; values = preset["values"]
        development = node["type"].endswith("Development")
        damage = node["type"].endswith("Damage")
        expected = {"strength": widgets[0], "blend": 1, "mix_response": 0 if development else 1, "encoding": 1, "size": widgets[2 if development else 4 if damage else 3],
                    "persistence": widgets[4 if development else 5 if damage else 4], "seed": node["linked_seeds"]["seed"]}
        if not development:
            expected["amount"] = widgets[3 if damage else 2]
            expected["edge_bias"] = widgets[7 if damage else 6]
        if development:
            expected["damage_type"] = 0
        elif damage:
            expected["damage_type"] = {"surface": 1, "emulsion_loss": 2, "dirty": 3}[widgets[2]]
        else:
            expected["damage_type"] = 1
        expected["irregularity"] = widgets[5 if development else 6 if damage else 5]
        expected.update(size_spread=35, thickness_spread=35, amount_variation=0, amount_drift_time=48)
        scratches = widgets[1] == "scratches"
        expected["scratch_wander"] = 0
        expected["scratch_wander_time"] = 1 if scratches else 8
        for key, value in expected.items():
            if not math.isclose(values[key], value, abs_tol=1e-9):
                raise RuntimeError(f"Mapped preset node {node['id']}/{key}: {values[key]} != {value}")
    print("Workflow preset values: PASS (mapped defects are not mathematical parity)", flush=True)


def definitions(path, names, namespace):
    """Load selected functions without importing or starting ComfyUI."""
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    nodes = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name in names]
    if {node.name for node in nodes} != set(names):
        raise RuntimeError(f"Reference functions changed in {path}")
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(path), "exec"), namespace)


def compare(args):
    """Use actual Comfy noise/normalization and transport/light/FFT functions on common input fixtures."""
    import numpy as np
    import torch
    import torch.nn.functional as F
    device = torch.device(args.backend)
    if args.backend == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("Нет доступного CUDA PyTorch для независимого reference")
    root = args.reference_root
    ns = {"torch": torch, "F": F, "math": math, "hashlib": hashlib}
    definitions(root / "nodes/video_silent_film_finish.py", ("_normalize_field", "_scaled_noise", "_build_emulsion_grain_field", "_apply_gate_weave", "_temporal_signal"), ns)
    definitions(root / "utils/film_optics.py", ("_diffuse_axis", "projection_glow"), ns)
    definitions(root / "utils/film_sensitometry.py", ("decode_transfer", "encode_transfer"), ns)
    definitions(root / "nodes/video_silent_film_pipeline.py", ("_effect_seed",), ns)
    failed = False
    print(f"Comfy film reference: {root}; Torch {torch.__version__}, backend={args.backend}", flush=True)
    with tempfile.TemporaryDirectory(prefix="silentfilm-comfy-film-") as temporary:
        folder = Path(temporary)
        for width, height, size in ((257, 43, 0.4), (257, 43, 2), (320, 180, 5.3), (1, 1, 2)):
            lattices = []
            original = torch.randn
            def recorded(*a, **kw):
                result = original(*a, **kw); lattices.append(result.clone()); return result
            with patch.object(torch, "randn", side_effect=recorded):
                expected = ns["_build_emulsion_grain_field"](1, height, width, size, 1925, device, torch.float32)
            fixture = folder / "grain.bin"
            with fixture.open("wb") as output:
                output.write(b"SFG1" + struct.pack("<II", width, height))
                for lattice in lattices:
                    output.write(struct.pack("<II", lattice.shape[-1], lattice.shape[-2]))
                    output.write(lattice.numpy().astype("<f4").tobytes())
                output.write(expected.cpu().numpy().astype("<f4").tobytes())
            print(f"Grain normalized structure {width}x{height}, size={size} (common Gaussian lattices)", flush=True)
            failed |= subprocess.run([str(args.native_test.resolve()), "--compare-grain", str(fixture)], check=False).returncode != 0
        width, height, count = 73, 29, 37
        y, x = np.mgrid[:height, :width].astype(np.float32)
        pattern = 0.5 + 0.3 * np.sin(x * 0.43) * np.cos(y * 0.7)
        source = np.ascontiguousarray(np.stack((pattern, pattern * 0.7, 1 - pattern, ((x + y) % 5) / 4), axis=-1), dtype=np.float32)
        rgba = torch.from_numpy(source).unsqueeze(0).to(device)
        node = workflow_nodes(args.reference)[5]["widgets_values"]
        def signal(effect, strength, smoothing):
            return ns["_temporal_signal"](count, ns["_effect_seed"](1925, "projector", effect), strength, smoothing, torch.device("cpu"), torch.float32)
        sx = signal("gate_x", node[1], 0.55); sy = signal("gate_y", node[1] * 0.8, 0.6)
        gain = (1 + signal("lamp_flicker", node[9], 0.18) + signal("lamp_breathing", node[10], 0.92)).clamp_min(0)
        for frame in (0, 11, 36):
            for glow in (0.0, 0.3):
                woven = ns["_apply_gate_weave"](rgba, sx[frame:frame + 1].to(device), sy[frame:frame + 1].to(device))
                linear = ns["decode_transfer"](woven[..., :3], "srgb") * gain[frame].to(device)
                linear = ns["projection_glow"](linear, glow, 1)
                rgb = ns["encode_transfer"](linear.clamp(0, 1), "srgb")
                expected = torch.cat((rgb, woven[..., 3:4]), dim=-1)
                fixture = folder / "projector.bin"
                with fixture.open("wb") as output:
                    output.write(b"SFP1" + struct.pack("<II4f", width, height, float(sx[frame]), float(sy[frame]), float(gain[frame]), glow))
                    output.write(source.astype("<f4").tobytes())
                    output.write(expected.cpu().numpy().astype("<f4").tobytes())
                print(f"Projector frame={frame}, glow={glow} (common weave/lamp values)", flush=True)
                failed |= subprocess.run([str(args.native_test.resolve()), "--compare-projector", str(fixture)], check=False).returncode != 0
    return int(failed)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--reference-root", type=Path, default=Path("/mnt/comfy/ComfyUI/custom_nodes/ComfyUI_ALEXZ_SilentFilm"))
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--backend", choices=("cpu", "cuda"), default="cpu")
    parser.add_argument("--native-test", type=Path)
    parser.add_argument("--presets-only", action="store_true")
    options = parser.parse_args()
    if options.reference is None:
        options.reference = options.reference_root / "workflows/Видео-в-Хронику-по-этапам_with_defects.json"
    if options.native_test is None:
        options.native_test = Path("build-cuda") / ("silentfilm_film_cuda_tests" if options.backend == "cuda" else "silentfilm_film_reference_tests")
    check_presets(options)
    raise SystemExit(0 if options.presets_only else compare(options))
