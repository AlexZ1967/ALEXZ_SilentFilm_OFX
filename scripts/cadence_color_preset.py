"""Generate clip-specific Color DRX presets from the observed Resolve 21.1.2 template schema."""

import ctypes
import ctypes.util
from dataclasses import dataclass
from functools import lru_cache
import hashlib
import math
from pathlib import Path
import re
import uuid
import xml.etree.ElementTree as ET


PLUGIN_ID = b"com.alexz.silentfilm.crankexposure"
MAX_BODY = 16 * 1024 * 1024


def require(value, message):
    if not value:
        raise ValueError(message)
    return value


@dataclass
class Field:
    number: int
    wire: int
    value: object


def varint(data, position):
    value = 0
    for shift in range(0, 70, 7):
        require(position < len(data), "Оборванный varint в DRX")
        byte = data[position]
        position += 1
        require(shift != 63 or byte <= 1, "Слишком длинный varint в DRX")
        value |= (byte & 127) << shift
        if byte < 128:
            return value, position
    raise ValueError("Неверный varint в DRX")


def encode_varint(value):
    result = bytearray()
    while value >= 128:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return bytes(result)


def unpack(data):
    """Read protobuf wire fields without interpreting arbitrary strings as nested messages."""
    result, position = [], 0
    while position < len(data):
        key, position = varint(data, position)
        number, wire = key >> 3, key & 7
        require(0 < number <= 0x1fffffff, "Неверный номер поля DRX")
        if wire == 0:
            value, position = varint(data, position)
        else:
            if wire == 2:
                size, position = varint(data, position)
            else:
                require(wire in (1, 5), "Неподдерживаемый wire type DRX")
                size = 8 if wire == 1 else 4
            require(size <= len(data) - position, "Оборванное поле DRX")
            value = data[position:position + size]
            position += size
        result.append(Field(number, wire, value))
    return result


def pack(fields):
    result = bytearray()
    for field in fields:
        result.extend(encode_varint(field.number * 8 + field.wire))
        if field.wire == 0:
            result.extend(encode_varint(field.value))
        else:
            if field.wire == 2:
                result.extend(encode_varint(len(field.value)))
            result.extend(field.value)
    return bytes(result)


def one(fields, number, wire):
    matches = [field for field in fields if field.number == number]
    require(len(matches) == 1 and matches[0].wire == wire, "Неподдерживаемая структура DRX: поле " + str(number))
    return matches[0]


class Zstd:
    def __init__(self):
        name = ctypes.util.find_library("zstd")
        require(name, "Для Color-пресета нужна системная libzstd (Debian/Ubuntu: libzstd1)")
        self.lib = ctypes.CDLL(name)
        for name, args, result in (
            ("ZSTD_getFrameContentSize", [ctypes.c_void_p, ctypes.c_size_t], ctypes.c_ulonglong),
            ("ZSTD_decompress", [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t], ctypes.c_size_t),
            ("ZSTD_compressBound", [ctypes.c_size_t], ctypes.c_size_t),
            ("ZSTD_compress", [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int], ctypes.c_size_t),
            ("ZSTD_isError", [ctypes.c_size_t], ctypes.c_uint),
        ):
            function = getattr(self.lib, name)
            function.argtypes, function.restype = args, result

    def decode(self, data):
        size = self.lib.ZSTD_getFrameContentSize(data, len(data))
        require(0 < size <= MAX_BODY, "Неподдерживаемый размер Zstd DRX")
        output = ctypes.create_string_buffer(size)
        written = self.lib.ZSTD_decompress(output, size, data, len(data))
        require(not self.lib.ZSTD_isError(written) and written == size, "Повреждённый Zstd-блок DRX")
        return output.raw[:written]

    def encode(self, data):
        require(0 < len(data) <= MAX_BODY, "DRX превышает ограничение размера")
        capacity = self.lib.ZSTD_compressBound(len(data))
        output = ctypes.create_string_buffer(capacity)
        written = self.lib.ZSTD_compress(output, capacity, data, len(data), 3)
        require(not self.lib.ZSTD_isError(written), "Не удалось сжать Color-пресет DRX")
        return output.raw[:written]


@lru_cache(maxsize=1)
def codec():
    return Zstd()


def body_text(document):
    require(len(document) <= 8 * 1024 * 1024 and "<!DOCTYPE" not in document and "<!ENTITY" not in document,
            "Неподдерживаемый XML DRX")
    # Resolve пишет имена Gallery::GyStill, не являющиеся XML namespace prefixes.
    view = re.sub(r'(<\/?)([A-Za-z_][\w:]*)(?=[\s/>])',
                  lambda match: match[1] + match[2].replace(":", "_"), document)
    root = ET.fromstring(view)
    matches = root.findall("./pClipFullVer/*/Body")
    require(len(matches) == 1 and matches[0].text, "В DRX нет единственного clip Color-графа")
    text = matches[0].text.strip()
    require(re.fullmatch(r"[0-9a-fA-F]+", text) and document.count(text) == 1, "Неоднозначный Body Color-графа")
    return text


def patch_body(data, new_path=None):
    """Patch only exposure_map along the message path observed in the user's exported template."""
    require(data[:5] == bytes.fromhex("8128b52ffd"), "Неизвестный формат Body DRX; нужен новый экспорт шаблона")
    root = unpack(codec().decode(data[1:]))
    graph_field = one(root, 1, 2)
    graph = unpack(graph_field.value)
    nodes = [field for field in graph if field.number == 7]
    require(1 <= len(nodes) <= 2 and all(field.wire == 2 for field in nodes), "Шаблон должен содержать только Crank Exposure и нейтральную ноду")
    found = []
    for node_field in nodes:
        node = unpack(node_field.value)
        properties_field = one(node, 10, 2)
        properties = unpack(properties_field.value)
        effects = []
        ids = []
        for entry_field in properties:
            require(entry_field.number == 1 and entry_field.wire == 2, "Неизвестные свойства Color-ноды")
            entry = unpack(entry_field.value)
            identifier = one(entry, 1, 0).value
            ids.append(identifier)
            if identifier == 3221225607:
                value_field = one(entry, 2, 2)
                value = unpack(value_field.value)
                effect_field = one(value, 21, 2)
                effect = unpack(effect_field.value)
                effects.append((entry_field, entry, value_field, value, effect_field, effect))
        if not effects:
            require(ids == [3221225473], "В шаблоне есть посторонняя Color-коррекция")
            continue
        require(len(effects) == 1 and set(ids) <= {3221225473, 3221225545, 3221225566, 3221225571, 3221225607, 3221225682},
                "В шаблоне есть посторонние свойства OFX")
        entry_field, entry, value_field, value, effect_field, effect = effects[0]
        require(one(effect, 2, 2).value == PLUGIN_ID and one(effect, 4, 0).value == 1,
                "Шаблон содержит другой или выключенный OFX")
        params = [field for field in effect if field.number == 5]
        require(len(params) == 1 and params[0].wire == 2,
                "Экспортируй шаблон с заводскими Blend1 и Frame Offset0")
        param = unpack(params[0].value)
        require(one(param, 1, 2).value == b"exposure_map", "В шаблоне нет единственного параметра exposure_map")
        param_value_field = one(param, 2, 2)
        param_value = unpack(param_value_field.value)
        path_field = one(param_value, 5, 2)
        old_path = path_field.value.decode("utf-8")
        require(old_path.endswith(".sfexposure"), "В шаблоне не назначена карта экспозиции")
        found.append(old_path)
        if new_path is not None:
            path_field.value = new_path.encode("utf-8")
            param_value_field.value = pack(param_value)
            params[0].value = pack(param)
            effect_field.value = pack(effect)
            value_field.value = pack(value)
            entry_field.value = pack(entry)
            properties_field.value = pack(properties)
            node_field.value = pack(node)
    require(len(found) == 1, "В шаблоне должен быть один SilentFilm Crank Exposure")
    if new_path is None:
        return {"map": found[0], "node_count": len(nodes)}
    graph_field.value = pack(graph)
    return b"\x81" + codec().encode(pack(root))


def template_path():
    directory = Path(__file__).resolve().parents[1] / "presets"
    canonical = directory / "crank_exposure_template.drx"
    if canonical.is_file():
        return canonical
    matches = sorted(directory.glob("crank_exposure_template*.drx"))
    require(len(matches) == 1, "Нужен один эталонный presets/crank_exposure_template*.drx")
    return matches[0]


def validate_color_template():
    """Check template schema and the runtime codec before spending time rendering a new job."""
    template = template_path()
    document = template.read_text(encoding="utf-8")
    metadata = patch_body(bytes.fromhex(body_text(document)))
    return dict(metadata, template=str(template))


def generate_preset(template, map_path, output):
    path = str(Path(map_path).resolve())
    require(Path(path).is_file() and not any(char in path for char in "\x00\n\r"), "Карта экспозиции недоступна")
    document = Path(template).read_text(encoding="utf-8")
    old_body = body_text(document)
    original = bytes.fromhex(old_body)
    metadata = patch_body(original)
    modified = patch_body(original, path)
    require(patch_body(modified)["map"] == path, "Не подтверждена запись карты в DRX")
    destination = Path(output)
    temporary = destination.with_suffix(destination.suffix + ".tmp")
    temporary.write_text(document.replace(old_body, modified.hex(), 1), encoding="utf-8")
    temporary.replace(destination)
    return dict(metadata, map=path, preset=str(destination.resolve()),
                template=str(Path(template).resolve()), template_sha256=hashlib.sha256(document.encode("utf-8")).hexdigest())


def configure_color_exposure(item, map_path, diagnostics, fresh=False, clip_name=None):
    """Apply a generated grade only to a freshly imported result; never overwrite an existing user grade."""
    require(fresh, "Автоматический Color-пресет применяется только к новому результату; существующий граф сохранён")
    require(item.GetFusionCompCount() == 0, "На результате уже есть Fusion-композиция; Color-пресет не применён")
    before = (item.GetStart(), item.GetDuration())
    require(all(isinstance(value, (int, float)) and math.isfinite(value) for value in before), "Resolve не вернул позицию/длину результата")
    name = clip_name or "crank_exposure"
    require(isinstance(name, str) and name not in (".", "..")
            and not re.search(r'[<>:"/\\|?*%\x00-\x1f\x7f]', name)
            and len(name.encode("utf-8")) <= 240, "Некорректное имя Color-пресета результата")
    preset = Path(map_path).resolve().parent / (name + ".drx")
    diagnostics.update(generate_preset(template_path(), map_path, preset))
    original = item.GetCurrentVersion()
    require(isinstance(original, dict) and isinstance(original.get("versionName"), str)
            and original.get("versionType") in (0, 1), "Resolve не вернул исходную Color-версию")
    version = "SilentFilm Crank Exposure " + uuid.uuid4().hex[:8]
    require(version not in entries(item.GetVersionNameList(0) or []), "Color-версия уже существует")
    diagnostics["previous_version"] = original
    created = False
    try:
        require(item.AddVersion(version, 0), "Не удалось создать локальную Color-версию результата")
        created = True
        require(item.LoadVersionByName(version, 0), "Не удалось включить локальную Color-версию")
        require(version_matches(item.GetCurrentVersion(), version, 0), "Resolve не подтвердил локальную Color-версию")
        graph = require(item.GetNodeGraph(), "Resolve не вернул Color-граф результата")
        diagnostics["apply_attempted"] = True
        require(graph.ApplyGradeFromDRX(str(preset), 0), "Resolve не применил Color-пресет")
        # SDK инвалидирует прежний Graph после применения DRX.
        applied = require(item.GetNodeGraph(), "Resolve не вернул новый Color-граф")
        count = applied.GetNumNodes()
        require(count == diagnostics["node_count"], "Состав Color-графа не совпал с пресетом")
        nodes = [{"label": applied.GetNodeLabel(index), "tools": entries(applied.GetToolsInNode(index) or [])}
                 for index in range(1, int(count) + 1)]
        diagnostics["applied_nodes"] = nodes
        require(sum(node["label"] == "SilentFilm Crank Exposure" or any(isinstance(tool, str)
                    and (PLUGIN_ID.decode() in tool or tool == "SilentFilm Crank Exposure") for tool in node["tools"])
                    for node in nodes) == 1,
                "Resolve не подтвердил единственный Crank Exposure в Color")
        require((item.GetStart(), item.GetDuration()) == before, "Применение Color-пресета изменило позицию/длину клипа")
    except Exception:
        if created:
            try:
                require(item.LoadVersionByName(original["versionName"], original["versionType"]), "Не восстановлена исходная Color-версия")
                require(version_matches(item.GetCurrentVersion(), original["versionName"], original["versionType"]), "Восстановление Color-версии не подтверждено")
                item.DeleteVersionByName(version, 0)
                require(version not in entries(item.GetVersionNameList(0) or []), "Незавершённая Color-версия осталась")
            except Exception as error:
                diagnostics["rollback_error"] = str(error)
        raise
    return {"exposure_status": "configured_color", "exposure_effect_enabled": True,
            "exposure_color_preset": str(preset), "exposure_color_node_count": count, "exposure_color_version": version}


def entries(value):
    if isinstance(value, dict):
        return list(value.values())
    require(isinstance(value, (list, tuple)), "Resolve не вернул список инструментов Color-ноды")
    return list(value)


def version_matches(actual, name, kind):
    return isinstance(actual, dict) and actual.get("versionName") == name and actual.get("versionType") == kind
