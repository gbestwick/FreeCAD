# SPDX-License-Identifier: LGPL-2.1-or-later

"""External CAM holder and tool-assembly assets."""

from __future__ import annotations

import json
import pathlib
import tempfile
import uuid
from typing import Mapping, Optional

import FreeCAD
import Part

from ..assets import Asset, AssetUri, AssetSerializer
from ..shape.doc import ShapeDocFromBytes


class HolderDefinition(Asset):
    """A holder CAD shape and its two canonical interface datums."""

    asset_type = "toolholder"

    def __init__(
        self,
        label: str,
        shape: Part.Shape,
        tool_side_datum: FreeCAD.Placement,
        machine_side_datum: FreeCAD.Placement,
        interfaces: Optional[dict] = None,
        id: Optional[str] = None,
    ):
        super().__init__()
        if shape.isNull() or not shape.Solids:
            raise ValueError("A holder definition requires non-empty solid geometry")
        self.id = id or str(uuid.uuid4())
        self._label = label
        self.shape = shape.copy()
        self.tool_side_datum = FreeCAD.Placement(tool_side_datum)
        self.machine_side_datum = FreeCAD.Placement(machine_side_datum)
        self.interfaces = interfaces or {
            "tool-side": {
                "type": "Unspecified",
                "size": "Unspecified",
                "status": "Candidate - verify before use",
            },
            "machine-side": {
                "type": "Unspecified",
                "size": "Unspecified",
                "status": "Candidate - verify before use",
            },
        }

    @property
    def label(self) -> str:
        return self._label

    def get_id(self) -> str:
        return self.id


class ToolAssembly(Asset):
    """A reusable tool/holder configuration, stored without document objects."""

    asset_type = "toolassembly"

    def __init__(
        self,
        label: str,
        toolbit_id: str,
        holder_uri: str,
        stick_out_mm: float,
        holder: Optional[HolderDefinition] = None,
        id: Optional[str] = None,
    ):
        super().__init__()
        if not toolbit_id:
            raise ValueError("A tool assembly must identify its CAM ToolBit")
        uri = AssetUri(holder_uri)
        if uri.asset_type != HolderDefinition.asset_type or not uri.version:
            raise ValueError("A tool assembly must reference a versioned holder asset")
        if stick_out_mm <= 0:
            raise ValueError("Tool stick-out must be greater than zero")
        self.id = id or str(uuid.uuid4())
        self._label = label
        self.toolbit_id = toolbit_id
        self.holder_uri = str(uri)
        self.stick_out_mm = float(stick_out_mm)
        self.holder = holder

    @property
    def label(self) -> str:
        return self._label

    def get_id(self) -> str:
        return self.id


class HolderDefinitionSerializer(AssetSerializer):
    for_class = HolderDefinition
    extensions = (".fcholder",)
    mime_type = "application/x-freecad-cam-holder"

    @classmethod
    def get_label(cls) -> str:
        return "FreeCAD CAM Holder"

    @classmethod
    def extract_dependencies(cls, data: bytes) -> list[AssetUri]:
        return []

    @classmethod
    def serialize(cls, asset: Asset) -> bytes:
        if not isinstance(asset, HolderDefinition):
            raise TypeError("Expected HolderDefinition")
        doc = None
        try:
            doc = FreeCAD.newDocument("CAMHolderAsset", hidden=True)
            obj = doc.addObject("Part::Feature", "CAMHolderAsset")
            obj.Label = asset.label
            obj.Shape = asset.shape
            for name, value, role in (
                ("AssetId", asset.id, "Holder"),
                ("ToolSideDatum", asset.tool_side_datum, "Holder Interface"),
                ("MachineSideDatum", asset.machine_side_datum, "Holder Interface"),
            ):
                prop_type = "App::PropertyString" if name == "AssetId" else "App::PropertyPlacement"
                obj.addProperty(prop_type, name, role)
                setattr(obj, name, value)
            for side, values in asset.interfaces.items():
                prefix = "ToolSide" if side == "tool-side" else "MachineSide"
                for suffix, value in (
                    ("InterfaceType", values.get("type", "Unspecified")),
                    ("InterfaceSize", values.get("size", "Unspecified")),
                    ("DefinitionStatus", values.get("status", "Incomplete")),
                ):
                    name = prefix + suffix
                    obj.addProperty("App::PropertyString", name, "Holder Interface")
                    setattr(obj, name, value)
            doc.recompute()
            with tempfile.TemporaryDirectory() as temp_dir:
                path = pathlib.Path(temp_dir) / "Holder.FCStd"
                doc.saveAs(str(path))
                return path.read_bytes()
        finally:
            if doc:
                FreeCAD.closeDocument(doc.Name)

    @classmethod
    def deserialize(cls, data: bytes, id: str, dependencies: Optional[Mapping[AssetUri, Asset]]):
        with ShapeDocFromBytes(data) as doc:
            obj = doc.getObject("CAMHolderAsset")
            if obj is None:
                raise ValueError("Holder asset document has no CAMHolderAsset feature")
            interfaces = {
                "tool-side": {
                    "type": obj.ToolSideInterfaceType,
                    "size": obj.ToolSideInterfaceSize,
                    "status": obj.ToolSideDefinitionStatus,
                },
                "machine-side": {
                    "type": obj.MachineSideInterfaceType,
                    "size": obj.MachineSideInterfaceSize,
                    "status": obj.MachineSideDefinitionStatus,
                },
            }
            return HolderDefinition(
                obj.Label,
                obj.Shape.copy(),
                FreeCAD.Placement(obj.ToolSideDatum),
                FreeCAD.Placement(obj.MachineSideDatum),
                interfaces,
                id,
            )

    @classmethod
    def deep_deserialize(cls, data: bytes) -> HolderDefinition:
        return cls.deserialize(data, "", None)


class ToolAssemblySerializer(AssetSerializer):
    for_class = ToolAssembly
    extensions = (".fctasm",)
    mime_type = "application/x-freecad-cam-tool-assembly"

    @classmethod
    def get_label(cls) -> str:
        return "FreeCAD CAM Tool Assembly"

    @classmethod
    def extract_dependencies(cls, data: bytes) -> list[AssetUri]:
        payload = json.loads(data.decode("utf-8"))
        return [AssetUri(payload["holder_uri"])]

    @classmethod
    def serialize(cls, asset: Asset) -> bytes:
        if not isinstance(asset, ToolAssembly):
            raise TypeError("Expected ToolAssembly")
        payload = {
            "format_version": 1,
            "id": asset.id,
            "label": asset.label,
            "toolbit_id": asset.toolbit_id,
            "holder_uri": asset.holder_uri,
            "stick_out_mm": asset.stick_out_mm,
        }
        return json.dumps(payload, sort_keys=True, indent=2).encode("utf-8")

    @classmethod
    def deserialize(cls, data: bytes, id: str, dependencies: Optional[Mapping[AssetUri, Asset]]):
        payload = json.loads(data.decode("utf-8"))
        holder_uri = payload["holder_uri"]
        holder = dependencies.get(AssetUri(holder_uri)) if dependencies is not None else None
        if dependencies is not None and holder is None:
            raise FileNotFoundError("Holder asset dependency is missing: {}".format(holder_uri))
        return ToolAssembly(
            payload.get("label", id),
            payload["toolbit_id"],
            holder_uri,
            float(payload["stick_out_mm"]),
            holder,
            id,
        )

    @classmethod
    def deep_deserialize(cls, data: bytes) -> ToolAssembly:
        return cls.deserialize(data, "", None)
