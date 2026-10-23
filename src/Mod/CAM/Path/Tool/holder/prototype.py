# SPDX-License-Identifier: LGPL-2.1-or-later

"""Small native-document prototype for CAM tool-holder assemblies.

This is intentionally not connected to the simulator or a holder library yet.
It creates an App::Part holder definition containing a Part::Feature envelope
and two visible Part::LocalCoordinateSystem interface datums.
"""

from math import isfinite
from typing import Iterable, Sequence, Tuple

import FreeCAD
import Part


def make_axisymmetric_envelope(stations: Iterable[Tuple[float, float]]):
    """Return a solid revolved from ordered ``(z, radius)`` stations.

    Coordinates are millimetres in a local frame whose axis is +Z. Values are
    deliberately plain numbers in this prototype; library serialization and
    unit conversion are not defined yet.
    """
    profile = [(float(z), float(radius)) for z, radius in stations]
    if len(profile) < 2:
        raise ValueError("An envelope needs at least two axial stations")

    previous_z = None
    for z, radius in profile:
        if not isfinite(z) or not isfinite(radius) or radius <= 0:
            raise ValueError("Envelope stations need finite z and positive radius")
        if previous_z is not None and z <= previous_z:
            raise ValueError("Envelope stations must be ordered by increasing z")
        previous_z = z

    z0, r0 = profile[0]
    z1, _ = profile[-1]
    points = [FreeCAD.Vector(0, 0, z0), FreeCAD.Vector(r0, 0, z0)]
    points.extend(FreeCAD.Vector(radius, 0, z) for z, radius in profile[1:])
    points.extend((FreeCAD.Vector(0, 0, z1), FreeCAD.Vector(0, 0, z0)))

    wire = Part.makePolygon(points)
    face = Part.Face(wire)
    shape = face.revolve(
        FreeCAD.Vector(0, 0, 0), FreeCAD.Vector(0, 0, 1), 360.0
    )
    if not shape.isValid() or not shape.Solids:
        raise ValueError("The axial envelope did not produce a valid solid")
    return shape


def _add_string_property(obj, name, group, description):
    if not hasattr(obj, name):
        obj.addProperty("App::PropertyString", name, group, description)


def _create_interface_datum(document, assembly, name, label, role, interface_type, placement):
    """Create a visible local coordinate system for one holder interface."""
    datum = document.addObject("Part::LocalCoordinateSystem", name)
    datum.Label = label
    datum.Placement = placement
    datum.addProperty(
        "App::PropertyString", "InterfaceRole", "Holder Interface", "Which end this datum describes"
    )
    datum.addProperty(
        "App::PropertyString", "InterfaceType", "Holder Interface", "User-confirmed mating type"
    )
    datum.addProperty(
        "App::PropertyString", "InterfaceSize", "Holder Interface", "Nominal size or supported range"
    )
    datum.addProperty(
        "App::PropertyString", "DefinitionStatus", "Holder Interface", "Whether this datum is verified"
    )
    datum.InterfaceRole = role
    datum.InterfaceType = str(interface_type)
    datum.InterfaceSize = "Unspecified"
    datum.DefinitionStatus = "Candidate — verify before use"
    assembly.addObject(datum)
    if FreeCAD.GuiUp:
        for child in getattr(datum, "OriginFeatures", []) or []:
            view = getattr(child, "ViewObject", None)
            if view is None:
                continue
            if hasattr(view, "Selectable"):
                view.Selectable = False
            if getattr(child, "Role", "").endswith("_Plane"):
                view.Visibility = False
    return datum


def add_holder_interface_datums(
    document,
    holder_part,
    tool_side_interface,
    machine_side_interface,
    tool_side_datum,
    machine_side_datum,
):
    """Add two visible, labeled interface frames to an existing App::Part."""
    if getattr(holder_part, "TypeId", None) != "App::Part":
        raise TypeError("Holder interfaces must be added to an App::Part")
    roles = {
        getattr(obj, "InterfaceRole", "")
        for obj in getattr(holder_part, "Group", []) or []
    }
    if "Tool-side" in roles or "Machine-side" in roles:
        raise ValueError("This holder already has interface datum objects")

    tool_datum = _create_interface_datum(
        document,
        holder_part,
        "ToolSideDatum",
        "Tool-side interface (candidate)",
        "Tool-side",
        tool_side_interface,
        tool_side_datum,
    )
    machine_datum = _create_interface_datum(
        document,
        holder_part,
        "MachineSideDatum",
        "Machine-side interface (candidate)",
        "Machine-side",
        machine_side_interface,
        machine_side_datum,
    )
    return tool_datum, machine_datum


def create_holder_definition(
    document,
    label,
    holder_stations: Sequence[Tuple[float, float]],
    tool_side_interface,
    machine_side_interface,
    tool_side_datum,
    machine_side_datum,
):
    """Create a basic App::Part holder definition from an axial envelope.

    Interface values and placements are stored independently from the envelope;
    interface matching and automatic placement are intentionally not implemented
    yet. Datum placements are expressed in the holder's local frame, in
    millimetres. Tool insertion depth belongs to a later assembly instance.
    """
    assembly = document.addObject("App::Part", "CAMToolHolderDefinition")
    assembly.Label = label

    holder = document.addObject("Part::Feature", "CAMToolHolderEnvelope")
    holder.Label = f"{label} envelope"
    holder.Shape = make_axisymmetric_envelope(holder_stations)
    _add_string_property(
        holder, "GeometrySource", "Holder", "Geometry source or approximation description"
    )
    holder.GeometrySource = "Parametric axisymmetric envelope (prototype)"
    assembly.addObject(holder)

    add_holder_interface_datums(
        document,
        assembly,
        tool_side_interface,
        machine_side_interface,
        tool_side_datum,
        machine_side_datum,
    )
    document.recompute()
    return assembly
