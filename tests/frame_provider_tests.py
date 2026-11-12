"""Exercise the production local frame provider and native decoder without Resolve or GTK."""

import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import threading
import time

spec = importlib.util.spec_from_file_location("frame_provider", Path(__file__).resolve().parents[1] / "scripts/cadence_frame_provider.py")
provider = importlib.util.module_from_spec(spec)
spec.loader.exec_module(provider)


def run(args):
    Path(args.root).mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix="frame-provider-", dir=args.root)).resolve()
    png = directory / "fixture.png"
    subprocess.run([args.ffmpeg, "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=64x48:rate=25",
                    "-frames:v", "1", "-c:v", "png", "-threads", "1", str(png)], check=True, timeout=30)
    payload = png.read_bytes()
    exported, errors = [], []
    state = {"state": "rendering", "width": 64, "height": 48, "fps": 25, "frame_count": 100,
             "require_alpha": False, "method": "frame_provider"}
    def export(index):
        exported.append(index)
        return payload
    def serve():
        try:
            provider.serve_frames(directory, state, export)
        except Exception as error:
            errors.append(error)
    worker = threading.Thread(target=serve, daemon=True)
    worker.start()
    try:
        deadline = time.monotonic() + 10
        while not (directory / "frame_source.ini").exists():
            if errors or time.monotonic() > deadline:
                raise RuntimeError("Frame provider did not become ready")
            time.sleep(0.02)
        request = directory / "request.ini"
        request.write_text("schema=1\nprocessing=capture_rgb\nsource_mode=resolve_frames\n"
                           f"input={directory / 'frame_source.ini'}\n"
                           "frame_count=100\nexpected_source_fps=25\noutput_fps=25\nformat=prores\ndevice=cpu\n"
                           "fps_min=1\nfps_max=1\nvariation=0\nirregularity=0\nshutter=0\nmotion_blur=0\n"
                           "samples=1\nshake=0\nexposure_link=0\n", encoding="utf-8")
        subprocess.run([args.helper, "--job", str(request)], check=True, timeout=60)
        worker.join(timeout=10)
        if worker.is_alive() or errors:
            raise RuntimeError("Frame provider did not terminate cleanly: " + str(errors))
        result = dict(line.split("=", 1) for line in (directory / "result.ini").read_text().splitlines() if "=" in line)
        if result.get("frame_count") != "4" or result.get("source_mode") != "resolve_frames":
            raise RuntimeError("Direct frame source changed cadence duration")
        if not 1 <= len(exported) <= 9 or len(exported) != len(set(exported)):
            raise RuntimeError("Native source requested unnecessary or duplicate frames: " + str(exported))
        if (directory / "resolve_processed.mkv").exists():
            raise RuntimeError("Unexpected intermediate movie")
        final = json.loads((directory / "bake_state.json").read_text())
        if final["provider_active"] or final["exported_frames"] != len(exported):
            raise RuntimeError("Incorrect provider lifecycle counters")
        print("Direct Resolve frame transport: PASS; exported indices:", exported)
    finally:
        if worker.is_alive():
            (directory / "cancel.request").write_text("cancel\n")
            worker.join(timeout=5)
    # Проверяем реальные пиксели разных независимых 16-bit PNG, а не только
    # завершение транспорта. Не должно быть delta-сложения с предыдущим PNG.
    pixel_dir = directory / "pixel-integrity"
    pixel_dir.mkdir()
    first, second = pixel_dir / "first.png", pixel_dir / "second.png"
    for image, filters in ((first, "format=rgb48be"), (second, "negate,format=rgb48be")):
        subprocess.run([args.ffmpeg, "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=64x48:rate=25",
                        "-vf", filters, "-frames:v", "1", "-c:v", "png", "-threads", "1", str(image)], check=True, timeout=30)
    images = (first.read_bytes(), second.read_bytes())
    errors.clear()
    def serve_pixels():
        try:
            provider.serve_frames(pixel_dir, dict(state), lambda index: images[index % 2])
        except Exception as error:
            errors.append(error)
    worker = threading.Thread(target=serve_pixels, daemon=True)
    worker.start()
    try:
        deadline = time.monotonic() + 10
        while not (pixel_dir / "frame_source.ini").exists():
            if errors or time.monotonic() > deadline:
                raise RuntimeError("Pixel provider did not become ready")
            time.sleep(0.02)
        subprocess.run([args.media_test, "--provider", str(pixel_dir / "frame_source.ini"), str(first), str(second)], check=True, timeout=30)
        worker.join(timeout=10)
        if worker.is_alive() or errors:
            raise RuntimeError("Pixel provider failed: " + str(errors))
    finally:
        if worker.is_alive():
            (pixel_dir / "cancel.request").write_text("cancel\n")
            worker.join(timeout=5)
    # Ошибка Resolve должна попасть в native status, а не оставить зависший сокет.
    failed_dir = directory / "provider-error"
    failed_dir.mkdir()
    errors.clear()
    def fail_export(index):
        raise RuntimeError("Injected Resolve export failure")
    def fail_serve():
        try:
            provider.serve_frames(failed_dir, dict(state), fail_export)
        except Exception as error:
            errors.append(error)
    worker = threading.Thread(target=fail_serve, daemon=True)
    worker.start()
    try:
        deadline = time.monotonic() + 10
        while not (failed_dir / "frame_source.ini").exists():
            if errors or time.monotonic() > deadline:
                raise RuntimeError("Error provider did not become ready")
            time.sleep(0.02)
        failure_request = failed_dir / "request.ini"
        failure_request.write_text(request.read_text().replace(str(directory / "frame_source.ini"), str(failed_dir / "frame_source.ini")))
        failure = subprocess.run([args.helper, "--job", str(failure_request)], capture_output=True, text=True, timeout=30)
        worker.join(timeout=10)
        if failure.returncode == 0 or "Injected Resolve export failure" not in failure.stderr or worker.is_alive():
            raise RuntimeError("Provider error was not propagated to the native decoder")
        if (failed_dir / "result.ini").exists():
            raise RuntimeError("Provider failure published a successful result")
        print("Provider error propagation: PASS")
    finally:
        if worker.is_alive():
            (failed_dir / "cancel.request").write_text("cancel\n")
            worker.join(timeout=5)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--helper", required=True)
    parser.add_argument("--media-test", required=True)
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--root", required=True)
    run(parser.parse_args())
