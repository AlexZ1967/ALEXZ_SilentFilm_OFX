"""Serve processed Resolve PNG frames on demand over a local authenticated socket."""

from collections import OrderedDict
import json
import os
from pathlib import Path
import secrets
import socket
import struct
import time


def atomic_state(path, state):
    """Publish provider progress without exposing partial JSON."""
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(state, ensure_ascii=False), encoding="utf-8")
    temporary.replace(path)


def serve_frames(directory, state, export):
    """Serve only requested frame indices, with a bounded PNG cache and no intermediate movie."""
    directory = Path(directory)
    state_path = directory / "bake_state.json"
    metadata = directory / "frame_source.ini"
    token = secrets.token_hex(32)
    name = "silentfilm-" + str(os.getpid()) + "-" + secrets.token_hex(8)
    started = time.monotonic()
    last_progress = started
    cache, cache_bytes = OrderedDict(), 0
    cache_limit = 64 * 1024 * 1024
    state.update(provider_active=True, served_requests=0, exported_frames=0, cache_hits=0,
                 transferred_bytes=0, export_seconds=0.0)

    def cancelled():
        if (directory / "cancel.request").exists():
            raise RuntimeError("Получение кадров Resolve отменено")

    def receive(connection, count, deadline):
        data = bytearray()
        while len(data) < count:
            cancelled()
            if time.monotonic() > deadline:
                raise RuntimeError("Истекло время ожидания клиента кадров Resolve")
            try:
                chunk = connection.recv(count - len(data))
            except socket.timeout:
                continue
            if not chunk:
                if data:
                    raise RuntimeError("Неполный запрос кадра")
                return None
            data.extend(chunk)
        return data

    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
            server.bind("\0" + name)
            server.listen(1)
            server.settimeout(0.5)
            text = ("SILENTFILM_FRAME_SOURCE_V1\n" + f"socket_name={name}\ntoken={token}\n"
                    + f"width={state['width']}\nheight={state['height']}\nfps={state['fps']}\nframe_count={state['frame_count']}\n")
            temporary = metadata.with_suffix(".tmp")
            descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(descriptor, "w", encoding="utf-8") as output:
                output.write(text)
            temporary.replace(metadata)
            state.update(state="complete", input=str(metadata), progress=100, source_mode="resolve_frames")
            atomic_state(state_path, state)
            deadline = time.monotonic() + 300
            while True:
                cancelled()
                if time.monotonic() > deadline:
                    raise RuntimeError("Каденс не подключился к источнику кадров; создай новое задание")
                try:
                    connection, _ = server.accept()
                    break
                except socket.timeout:
                    pass
            with connection:
                connection.settimeout(0.5)
                received = receive(connection, 64, time.monotonic() + 30)
                if received is None or not secrets.compare_digest(bytes(received), token.encode("ascii")):
                    raise RuntimeError("Ошибка аутентификации источника кадров")
                connection.sendall(b"SF01")
                while True:
                    request = receive(connection, 8, time.monotonic() + 300)
                    if request is None:
                        break
                    index = struct.unpack("<Q", request)[0]
                    try:
                        if index >= state["frame_count"]:
                            raise RuntimeError("Запрошен кадр вне выбранного монтажного диапазона")
                        state["served_requests"] += 1
                        if index in cache:
                            payload = cache.pop(index)
                            cache[index] = payload
                            state["cache_hits"] += 1
                        else:
                            before = time.monotonic()
                            payload = export(index)
                            state["export_seconds"] += time.monotonic() - before
                            if len(payload) < 26 or payload[:8] != b"\x89PNG\r\n\x1a\n":
                                raise RuntimeError("Resolve вернул некорректный PNG")
                            w, h = struct.unpack(">II", payload[16:24])
                            if (w, h) != (state["width"], state["height"]):
                                raise RuntimeError(f"Размер кадра Resolve {w}×{h} отличается от таймлайна")
                            if state.get("require_alpha") and payload[25] not in (4, 6):
                                raise RuntimeError("Resolve не предоставил alpha в PNG")
                            state["exported_frames"] += 1
                            while cache and (len(cache) >= 8 or cache_bytes + len(payload) > cache_limit):
                                _, old = cache.popitem(last=False)
                                cache_bytes -= len(old)
                            if len(payload) <= cache_limit:
                                cache[index] = payload
                                cache_bytes += len(payload)
                        connection.settimeout(300)
                        connection.sendall(struct.pack("<BQ", 0, len(payload)))
                        connection.sendall(payload)
                        connection.settimeout(0.5)
                        state["transferred_bytes"] += len(payload)
                    except Exception as error:
                        error_data = str(error).encode("utf-8")[:8192]
                        try:
                            connection.settimeout(2)
                            connection.sendall(struct.pack("<BQ", 1, len(error_data)) + error_data)
                        except OSError:
                            pass
                        raise
                    if time.monotonic() - last_progress >= 0.5:
                        atomic_state(state_path, state)
                        last_progress = time.monotonic()
    except Exception as error:
        state.update(state="cancelled" if (directory / "cancel.request").exists() else "error", error=str(error))
        raise
    finally:
        state.update(provider_active=False, provider_seconds=round(time.monotonic() - started, 3))
        atomic_state(state_path, state)
