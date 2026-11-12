"""Bind a per-clip exposure map through documented Fusion inputs and verify the stored values."""

import math
from pathlib import Path


class FusionExposureError(RuntimeError):
    """Report configuration failure and whether a partially created composition remains."""

    def __init__(self, message, requires_recovery=False):
        super().__init__(message)
        self.requires_recovery = requires_recovery


def require(value, message):
    if not value:
        raise RuntimeError(message)
    return value


def entries(table):
    if isinstance(table, dict):
        return list(table.values())
    require(isinstance(table, (list, tuple)), "Fusion вернул неожиданный формат таблицы")
    return list(table)


def input_id(tool, key, label, diagnostics):
    """Prefer the exact OFX parameter ID; reject ambiguous display-name matches."""
    inputs = []
    for item in entries(tool.GetInputList()):
        attrs = item.GetAttrs()
        require(isinstance(attrs, dict), "Fusion не вернул атрибуты входа")
        inputs.append({name: attrs.get(name) for name in ("INPS_ID", "INPS_Name", "INPS_DataType")})
    diagnostics["inputs"] = inputs
    exact = [item["INPS_ID"] for item in inputs if item["INPS_ID"] == key]
    normalize = lambda value: "".join(char.lower() for char in str(value) if char.isalnum())
    matches = exact or [item["INPS_ID"] for item in inputs
                        if isinstance(item["INPS_ID"], str) and (normalize(item["INPS_ID"]) == normalize(key)
                        or normalize(item["INPS_Name"]) == normalize(label))]
    require(len(matches) == 1, "Fusion не нашёл однозначный параметр " + label)
    return matches[0]


def set_input(tool, identifier, value):
    """Read back each assignment instead of trusting SetInput's optional return value."""
    tool.SetInput(identifier, value)
    actual = tool.GetInput(identifier)
    if isinstance(value, str):
        require(isinstance(actual, str) and Path(actual).resolve() == Path(value).resolve(),
                "Fusion не подтвердил запись пути Exposure Map")
    else:
        require(isinstance(actual, (int, float)) and math.isfinite(actual) and abs(actual - value) < 1e-9,
                "Fusion не подтвердил значение " + identifier)


def connect(destination, source):
    """Verify connection ownership without comparing transient scripting proxy identities."""
    output = require(source.FindMainOutput(1), "У Fusion-ноды нет основного выхода")
    destination.ConnectTo(output)
    connected = require(destination.GetConnectedOutput(), "Fusion не соединил ноды")
    owner = require(connected.GetTool(), "Fusion не вернул владельца соединения")
    expected, actual = source.GetAttrs(), owner.GetAttrs()
    require(actual.get("TOOLS_Name") == expected.get("TOOLS_Name")
            and actual.get("TOOLS_RegID") == expected.get("TOOLS_RegID"), "Fusion подключил другой инструмент")


def configure_fusion_exposure(item, resolve, path, diagnostics):
    """Create only a fresh result composition; never replace or rewire an existing user graph."""
    comp = None
    name = None
    attempted = False
    try:
        require(resolve is not None, "Fusion API недоступен")
        count = item.GetFusionCompCount()
        require(isinstance(count, (int, float)) and count == 0,
                "На результате уже есть Fusion-композиция; её граф не изменён")
        before = entries(item.GetFusionCompNameList() or [])
        require(not before, "Список Fusion-композиций изменился; повтори импорт")
        fusion = require(resolve.Fusion(), "Resolve не вернул Fusion API")
        attempted = True
        comp = require(item.AddFusionComp(), "Resolve не создал Fusion-композицию")
        names = entries(item.GetFusionCompNameList())
        require(len(names) == 1 and isinstance(names[0], str), "Resolve не вернул имя созданной композиции")
        name = names[0]
        diagnostics["composition"] = name
        comp.Lock()
        try:
            tools = entries(comp.GetToolList(False))
            sources = [tool for tool in tools if tool.GetAttrs().get("TOOLS_RegID") == "MediaIn"]
            sinks = [tool for tool in tools if tool.GetAttrs().get("TOOLS_RegID") == "MediaOut"]
            require(len(sources) == 1 and len(sinks) == 1 and len(tools) == 2,
                    "Новая Fusion-композиция не содержит простой MediaIn/MediaOut граф")
            mask = comp.CT_Tool
            require(isinstance(mask, (int, float)) and mask > 0, "Fusion не предоставил CT_Tool")
            registry = []
            for entry in entries(fusion.GetRegList(mask)):
                attrs = entry if isinstance(entry, dict) else entry.GetAttrs()
                if isinstance(attrs, dict) and attrs.get("REGS_Name") == "SilentFilm Crank Exposure":
                    identifier = attrs.get("REGS_ID")
                    if isinstance(identifier, str) and identifier:
                        registry.append(identifier)
            diagnostics["registry_matches"] = registry
            require(len(registry) == 1, "Fusion не нашёл единственный SilentFilm Crank Exposure; обнови OFX bundle")
            tool = require(comp.AddTool(registry[0], False, 1, 0), "Fusion не создал OFX-инструмент")
            require(tool.GetAttrs().get("TOOLS_RegID") == registry[0], "Fusion создал другой тип инструмента")
            tool.SetAttrs({"TOOLB_PassThrough": True})
            require(tool.GetAttrs().get("TOOLB_PassThrough") in (True, 1), "Fusion не приостановил новый инструмент")
            binding = {"map": input_id(tool, "exposure_map", "Exposure Map", diagnostics),
                       "blend": input_id(tool, "blend", "Blend", diagnostics),
                       "offset": input_id(tool, "frame_offset", "Frame Offset", diagnostics)}
            diagnostics["binding"] = binding
            set_input(tool, binding["map"], str(Path(path).resolve()))
            set_input(tool, binding["blend"], 1)
            set_input(tool, binding["offset"], 0)
            connect(require(tool.FindMainInput(1), "У Crank Exposure нет входа изображения"), sources[0])
            connect(require(sinks[0].FindMainInput(1), "У MediaOut нет входа изображения"), tool)
            tool.SetAttrs({"TOOLB_PassThrough": False})
            require(tool.GetAttrs().get("TOOLB_PassThrough") in (False, 0), "Fusion не включил настроенную ноду")
            diagnostics["tool"] = tool.GetAttrs().get("TOOLS_Name")
            require(isinstance(diagnostics["tool"], str) and diagnostics["tool"], "Fusion не вернул имя Crank Exposure")
            diagnostics["registry_id"] = registry[0]
        finally:
            comp.Unlock()
        return {"exposure_status": "configured_fusion", "exposure_effect_enabled": True,
                "exposure_fusion_comp": name, "exposure_fusion_tool": diagnostics["tool"],
                "exposure_fusion_registry_id": registry[0]}
    except Exception as error:
        recovery = False
        if comp is not None:
            try:
                # Удаляем только созданную здесь композицию, не прежние графы.
                if name is None:
                    names = entries(item.GetFusionCompNameList())
                    require(len(names) == 1 and isinstance(names[0], str), "Не удалось определить новую композицию")
                    name = names[0]
                item.DeleteFusionCompByName(name)
                require(name not in entries(item.GetFusionCompNameList() or []), "Не удалось удалить незавершённую композицию")
            except Exception as cleanup_error:
                recovery = True
                diagnostics["rollback_error"] = str(cleanup_error)
        elif attempted:
            try:
                recovery = item.GetFusionCompCount() != 0
            except Exception:
                recovery = True
        diagnostics["error"] = str(error)
        raise FusionExposureError(str(error), recovery) from error
