# SPDX-License-Identifier: LGPL-2.1-or-later

"""Guided GUI authoring for a first CAM tool-holder definition."""

import FreeCAD
import FreeCADGui
import Part
from PySide import QtGui
from PySide.QtCore import QT_TRANSLATE_NOOP, Qt
from .assets import HolderDefinition
from ..camassets import cam_assets


def _global_placement(obj):
    getter = getattr(obj, "getGlobalPlacement", None)
    return getter() if getter else getattr(obj, "Placement", FreeCAD.Placement())


def _is_document_group(obj):
    return getattr(obj, "TypeId", "") == "App::DocumentObjectGroup"


def _object_shape(obj):
    try:
        shape = obj.Shape
        return shape if not shape.isNull() else None
    except Exception:
        return None


def _holder_sources(selected):
    """Expand selected groups into solid sources and face-pickable descendants."""
    sources = []
    pickable_names = set()
    visited = set()
    covered_descendants = set()

    def add_pickables(obj):
        if obj.Name in visited:
            return
        visited.add(obj.Name)
        if not _is_document_group(obj) and _object_shape(obj) is not None:
            pickable_names.add(obj.Name)
        for child in getattr(obj, "Group", []) or []:
            add_pickables(child)

    def add_geometry(obj):
        if _is_document_group(obj):
            for child in getattr(obj, "Group", []) or []:
                add_geometry(child)
            return
        shape = _object_shape(obj)
        if shape is not None and shape.Solids:
            sources.append(obj)

            def mark_children(parent):
                for child in getattr(parent, "Group", []) or []:
                    covered_descendants.add(child.Name)
                    mark_children(child)

            mark_children(obj)
            return
        for child in getattr(obj, "Group", []) or []:
            add_geometry(child)

    for obj in selected:
        add_pickables(obj)
        add_geometry(obj)

    unique_sources = []
    source_names = set()
    for obj in sources:
        if obj.Name not in source_names and obj.Name not in covered_descendants:
            source_names.add(obj.Name)
            unique_sources.append(obj)
    return unique_sources, pickable_names


def _interface_frame(selection):
    """Return a face center and outward normal in document coordinates."""
    obj = selection.Object
    sub = selection.SubElementNames[0]
    if not sub.startswith("Face"):
        raise ValueError("Select one planar face for the interface datum")
    face = obj.Shape.getElement(sub)
    if not isinstance(face.Surface, Part.Plane):
        raise ValueError("This first authoring version supports planar interface faces")
    u0, u1, v0, v1 = face.ParameterRange
    normal = face.normalAt((u0 + u1) * 0.5, (v0 + v1) * 0.5)
    placement = _global_placement(obj)
    return placement.multVec(face.CenterOfMass), placement.Rotation.multVec(normal)


def _canonical_holder_frame(frames, rows):
    """Build a world frame centered between the two interface origins, +Z to machine side."""
    tool_center = frames["Tool-side"][0]
    machine_center = frames["Machine-side"][0]
    axis = FreeCAD.Vector(machine_center).sub(FreeCAD.Vector(tool_center))
    if axis.Length < 1e-6:
        raise ValueError("The two interface datums must be separated to define the holder axis")
    axis = axis.multiply(1.0 / axis.Length)
    origins = {
        "Tool-side": FreeCAD.Vector(tool_center).sub(
            FreeCAD.Vector(axis).multiply(rows["Tool-side"][1].value())
        ),
        "Machine-side": FreeCAD.Vector(machine_center).add(
            FreeCAD.Vector(axis).multiply(rows["Machine-side"][1].value())
        ),
    }
    direction = origins["Machine-side"].sub(origins["Tool-side"])
    if direction.Length < 1e-6:
        raise ValueError("The interface offsets overlap the two datum origins")
    if direction.dot(axis) <= 0:
        raise ValueError("The interface offsets reverse the tool-to-machine axis")
    direction = direction.multiply(1.0 / direction.Length)
    midpoint = origins["Tool-side"].add(origins["Machine-side"]).multiply(0.5)
    return FreeCAD.Placement(
        midpoint,
        FreeCAD.Rotation(FreeCAD.Vector(0, 0, 1), direction),
    ), origins


class _SelectionObserver:
    def __init__(self, dialog):
        self.dialog = dialog

    def addSelection(self, doc, obj, sub, point):
        self.dialog.accept_face_pick(doc, obj, sub, point)


class HolderImporterDialog(QtGui.QDialog):
    """Collect imported geometry and two user-picked interface faces."""

    def __init__(self, sources, pickable_names, parent=None):
        super().__init__(parent or FreeCADGui.getMainWindow())
        self.setWindowTitle("Create CAM Tool Holder")
        self.setMinimumWidth(470)
        self.document = FreeCAD.ActiveDocument
        self.sources = sources
        self.frames = {"Tool-side": None, "Machine-side": None}
        self.datums = {}
        self.source_names = pickable_names
        self.pick_role = None
        self.observer = _SelectionObserver(self)

        layout = QtGui.QVBoxLayout(self)
        intro = QtGui.QLabel(
            "Create a reusable holder definition in the CAM asset library from selected solid objects or a container holding them. "
            "Pick the tool-side and machine-side mating faces, then verify the displayed datums before saving. "
            "The source objects stay unchanged, and the saved holder geometry will not be added to this project's tree."
        )
        intro.setWordWrap(True)
        layout.addWidget(intro)

        self.convention_ack = QtGui.QCheckBox(
            "Holder axis: +Z points from tool side to machine side"
        )
        layout.addWidget(self.convention_ack)

        self.name = QtGui.QLineEdit("Tool Holder")
        form = QtGui.QFormLayout()
        form.addRow("Definition name", self.name)
        solid_count = sum(len(_object_shape(obj).Solids) for obj in sources)
        self.source_label = QtGui.QLabel(
            "{} objects, {} solids: {}".format(
                len(sources), solid_count, ", ".join(o.Label for o in sources)
            )
        )
        self.source_label.setWordWrap(True)
        form.addRow("Geometry", self.source_label)
        layout.addLayout(form)

        self.rows = {}
        for role in ("Tool-side", "Machine-side"):
            box = QtGui.QGroupBox(role + " interface")
            row = QtGui.QGridLayout(box)
            status = QtGui.QLabel("No face selected")
            pick = QtGui.QPushButton("Pick face...")
            offset = QtGui.QDoubleSpinBox()
            offset.setRange(-10000.0, 10000.0)
            offset.setDecimals(3)
            offset.setSuffix(" mm")
            offset.setToolTip(
                "Move along the holder axis; positive values move outward from the holder end"
            )
            kind = QtGui.QLineEdit("Unspecified")
            kind.setPlaceholderText("e.g. cylindrical shank, spindle taper")
            row.addWidget(status, 0, 0, 1, 2)
            row.addWidget(pick, 1, 0, 1, 2)
            row.addWidget(QtGui.QLabel("Axial offset"), 2, 0)
            row.addWidget(offset, 2, 1)
            row.addWidget(QtGui.QLabel("Interface type"), 3, 0)
            row.addWidget(kind, 3, 1)
            pick.clicked.connect(lambda checked=False, r=role: self.begin_pick(r))
            offset.valueChanged.connect(lambda value, r=role: self.update_preview(r))
            self.rows[role] = (status, offset, kind)
            layout.addWidget(box)

        self.preview_label = QtGui.QLabel("Datum axes appear in the model after a face is picked.")
        self.preview_label.setWordWrap(True)
        layout.addWidget(self.preview_label)
        buttons = QtGui.QDialogButtonBox(QtGui.QDialogButtonBox.Save | QtGui.QDialogButtonBox.Cancel)
        buttons.accepted.connect(self.save_definition)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
        FreeCADGui.Selection.addObserver(self.observer)

    def begin_pick(self, role):
        self.pick_role = role
        self.rows[role][0].setText("Click a planar mating face in the 3D view...")
        FreeCADGui.Selection.clearSelection()

    def accept_face_pick(self, doc_name, obj_name, sub, point=None):
        if self.pick_role is None:
            return
        # SelectionObserver passes document and object internal names as strings.
        # Resolve against the document captured when the dialog was opened.
        if doc_name != self.document.Name:
            return
        obj = obj_name if hasattr(obj_name, "Shape") else self.document.getObject(obj_name)
        if obj is None or obj.Name not in self.source_names:
            self.rows[self.pick_role][0].setText("Pick a face on one of the selected holder geometry objects")
            return
        try:
            center, normal = _interface_frame(
                type("Selection", (), {"Object": obj, "SubElementNames": [sub]})
            )
        except Exception as exc:
            self.rows[self.pick_role][0].setText(str(exc))
            return
        role = self.pick_role
        self.frames[role] = (center, normal, obj, sub)
        self.rows[role][0].setText("{} / {}".format(obj.Label, sub))
        self.pick_role = None
        for interface_role in self.frames:
            self.update_preview(interface_role)

    def update_preview(self, role):
        frame = self.frames[role]
        if not frame:
            return
        _status, _offset, _kind = self.rows[role]
        if all(self.frames.values()):
            try:
                canonical_frame, origins = _canonical_holder_frame(self.frames, self.rows)
            except ValueError as exc:
                self.preview_label.setText(str(exc))
                return
            origin = origins[role]
            axis = canonical_frame.Rotation.multVec(FreeCAD.Vector(0, 0, 1))
        else:
            center, normal, _obj, _sub = frame
            axis = normal if role == "Machine-side" else normal.negative()
            origin = center.add(axis.multiply(_offset.value()))
        datum = self.datums.get(role)
        if datum is None:
            datum = self.document.addObject(
                "Part::LocalCoordinateSystem", "CAMHolder{}Preview".format(role.replace("-", ""))
            )
            datum.Label = role + " datum preview"
            self.datums[role] = datum
            self.document.recompute()
        # Coordinate-system view providers start hidden by default.
        datum.ViewObject.Visibility = True
        datum.Placement = FreeCAD.Placement(
            origin, FreeCAD.Rotation(FreeCAD.Vector(0, 0, 1), axis)
        )
        self.document.recompute()
        self.preview_label.setText(
            "Both interface datums are previewed."
            if all(self.frames.values())
            else "Pick the other interface to preview both datums."
        )

    def save_definition(self):
        if any(self.frames[role] is None for role in self.frames):
            QtGui.QMessageBox.warning(self, "Incomplete holder", "Pick both interface faces before saving.")
            return
        if not self.convention_ack.isChecked():
            QtGui.QMessageBox.warning(
                self,
                "Confirm holder axis",
                "Select the holder axis checkbox to continue.",
            )
            return
        label = self.name.text().strip()
        if not label:
            QtGui.QMessageBox.warning(self, "Missing name", "Enter a name for the holder definition.")
            return

        doc = self.document
        try:
            holder_frame, _interface_origins = _canonical_holder_frame(self.frames, self.rows)
            world_to_holder = holder_frame.inverse()
            shapes = []
            for source in self.sources:
                shape = source.Shape.copy()
                shape.transformShape(_global_placement(source).toMatrix(), True)
                shape.transformShape(world_to_holder.toMatrix(), True)
                shapes.append(shape)
            geometry_shape = Part.makeCompound(shapes)
            interfaces = {}
            datums = {}
            for role in ("Tool-side", "Machine-side"):
                side = role.lower()
                datums[side] = world_to_holder.multiply(self.datums[role].Placement)
                interfaces[side] = {
                    "type": self.rows[role][2].text().strip() or "Unspecified",
                    "size": "Unspecified",
                    "status": "Candidate - verify before use",
                }
        except ValueError as exc:
            QtGui.QMessageBox.warning(self, "Invalid holder axis", str(exc))
            return
        except Exception as exc:
            QtGui.QMessageBox.critical(
                self,
                "Could not prepare holder",
                "The holder geometry could not be transformed. No holder was created.\n\n{}".format(exc),
            )
            return
        try:
            holder_asset = HolderDefinition(
                label,
                geometry_shape,
                datums["tool-side"],
                datums["machine-side"],
                interfaces,
            )
            uri = cam_assets.add(holder_asset)
            if not uri.version:
                raise RuntimeError("The asset store did not return a pinned holder revision")
            for preview in self.datums.values():
                doc.removeObject(preview.Name)
            doc.recompute()
        except Exception as exc:
            QtGui.QMessageBox.critical(
                self,
                "Could not save holder asset",
                "The holder asset was not saved. Correct the geometry selection and try again.\n\n{}".format(exc),
            )
            return
        QtGui.QMessageBox.information(
            self,
            "Holder Saved to CAM Library",
            "Saved '{}' as {}. The holder geometry is stored outside the project document.".format(label, uri),
        )
        self.accept()

    def done(self, result):
        try:
            FreeCADGui.Selection.removeObserver(self.observer)
        except Exception:
            pass
        if result != QtGui.QDialog.Accepted:
            doc = self.document
            if doc:
                for datum in self.datums.values():
                    try:
                        doc.removeObject(datum.Name)
                    except Exception:
                        pass
                doc.recompute()
        super().done(result)


def _holder_interface_datums(holder):
    """Find the tool-side and machine-side datum objects in a holder part."""
    datums = {}
    for obj in getattr(holder, "Group", []) or []:
        role = getattr(obj, "InterfaceRole", "")
        if role in ("Tool-side", "Machine-side"):
            datums[role] = obj
    return datums


class HolderDefinitionEditor(QtGui.QDialog):
    """Review holder interface metadata and expose each datum's placement."""

    def __init__(self, holder, datums, parent=None):
        super().__init__(parent or FreeCADGui.getMainWindow())
        self.holder = holder
        self.datums = datums
        self.setWindowTitle("Edit CAM Tool Holder")
        self.setMinimumWidth(510)

        layout = QtGui.QVBoxLayout(self)
        intro = QtGui.QLabel(
            "Review the two mating interfaces. To reposition an origin or its axis, "
            "select its datum in the model tree and edit Placement in the property editor."
        )
        intro.setWordWrap(True)
        layout.addWidget(intro)

        form = QtGui.QFormLayout()
        self.name = QtGui.QLineEdit(holder.Label)
        form.addRow("Definition name", self.name)
        layout.addLayout(form)

        self.fields = {}
        for role in ("Tool-side", "Machine-side"):
            datum = datums[role]
            box = QtGui.QGroupBox(role + " interface")
            fields = QtGui.QFormLayout(box)
            placement = datum.Placement
            position = placement.Base
            location = QtGui.QLabel(
                "Origin: X {:.3f}, Y {:.3f}, Z {:.3f} mm".format(
                    position.x, position.y, position.z
                )
            )
            location.setTextInteractionFlags(Qt.TextSelectableByMouse)
            interface_type = QtGui.QLineEdit(getattr(datum, "InterfaceType", "Unspecified"))
            interface_size = QtGui.QLineEdit(getattr(datum, "InterfaceSize", "Unspecified"))
            status = QtGui.QComboBox()
            status.addItems(["Candidate - verify before use", "Verified", "Incomplete"])
            current_status = getattr(datum, "DefinitionStatus", "Candidate - verify before use")
            index = status.findText(current_status)
            status.setCurrentIndex(index if index >= 0 else 0)
            fields.addRow("Datum", QtGui.QLabel(datum.Label))
            fields.addRow("Placement", location)
            fields.addRow("Interface type", interface_type)
            fields.addRow("Nominal size or range", interface_size)
            fields.addRow("Definition status", status)
            layout.addWidget(box)
            self.fields[role] = (interface_type, interface_size, status)

        buttons = QtGui.QDialogButtonBox(QtGui.QDialogButtonBox.Save | QtGui.QDialogButtonBox.Cancel)
        buttons.accepted.connect(self.save)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    def save(self):
        label = self.name.text().strip()
        if not label:
            QtGui.QMessageBox.warning(self, "Missing name", "Enter a name for the holder definition.")
            return
        doc = self.holder.Document
        doc.openTransaction("Edit CAM Tool Holder")
        try:
            self.holder.Label = label
            for role, datum in self.datums.items():
                interface_type, interface_size, status = self.fields[role]
                datum.InterfaceType = interface_type.text().strip() or "Unspecified"
                datum.InterfaceSize = interface_size.text().strip() or "Unspecified"
                datum.DefinitionStatus = status.currentText()
            doc.recompute()
            doc.commitTransaction()
        except Exception as exc:
            doc.abortTransaction()
            QtGui.QMessageBox.critical(
                self,
                "Could not update holder",
                "The holder definition was not updated.\n\n{}".format(exc),
            )
            return
        self.accept()


class CommandHolderEditor:
    def GetResources(self):
        return {
            "MenuText": QT_TRANSLATE_NOOP("CAM_ToolHolderEdit", "Edit Document Holder Prototype"),
            "ToolTip": QT_TRANSLATE_NOOP("CAM_ToolHolderEdit", "Edit a legacy holder definition stored as document objects"),
        }

    def _selection(self):
        selected = FreeCADGui.Selection.getSelection()
        if len(selected) != 1:
            return None, None
        obj = selected[0]
        if getattr(obj, "TypeId", "") == "App::Part":
            holder = obj
        else:
            holder = next(
                (
                    parent
                    for parent in getattr(obj, "InList", []) or []
                    if getattr(parent, "TypeId", "") == "App::Part"
                ),
                None,
            )
        if holder is None:
            return None, None
        datums = _holder_interface_datums(holder)
        if set(datums) != {"Tool-side", "Machine-side"}:
            return None, None
        return holder, datums

    def IsActive(self):
        return FreeCAD.ActiveDocument is not None and self._selection()[0] is not None

    def Activated(self):
        holder, datums = self._selection()
        if holder is None:
            QtGui.QMessageBox.information(
                FreeCADGui.getMainWindow(),
                "Tool Holder",
                "Select a holder definition or one of its interface datums first.",
            )
            return
        global _active_dialog
        _active_dialog = HolderDefinitionEditor(holder, datums)
        _active_dialog.show()
        _active_dialog.raise_()
        _active_dialog.activateWindow()


class CommandHolderImporter:
    def GetResources(self):
        return {
            "MenuText": QT_TRANSLATE_NOOP("CAM_ToolHolderImport", "Create Tool Holder Definition"),
            "ToolTip": QT_TRANSLATE_NOOP("CAM_ToolHolderImport", "Create a holder definition by assigning tool-side and machine-side interface faces"),
        }

    def IsActive(self):
        return FreeCAD.ActiveDocument is not None

    def Activated(self):
        global _active_dialog
        sources, pickable_names = _holder_sources(FreeCADGui.Selection.getSelection())
        if not sources:
            QtGui.QMessageBox.information(
                FreeCADGui.getMainWindow(),
                "Tool Holder",
                "Select one or more imported solid objects, or a container that holds them, first.",
            )
            return
        _active_dialog = HolderImporterDialog(sources, pickable_names)
        _active_dialog.show()
        _active_dialog.raise_()
        _active_dialog.activateWindow()


class HolderAssetEditorDialog(QtGui.QDialog):
    """Edit a library holder asset without inserting geometry in a project."""

    def __init__(self, parent=None):
        super().__init__(parent or FreeCADGui.getMainWindow())
        self.setWindowTitle("Edit CAM Holder Library Asset")
        self.setMinimumWidth(560)
        self.asset_by_uri = {}
        self.preview_datums = {}
        self.pick_role = None
        self.return_doc_name = FreeCAD.ActiveDocument.Name if FreeCAD.ActiveDocument else None
        self.preview_doc = FreeCAD.newDocument("CAMHolderAssetPreview")
        self.preview_doc.Label = "Temporary CAM Holder Asset Preview"
        self.preview_shape = self.preview_doc.addObject("Part::Feature", "HolderGeometryPreview")
        self.preview_shape.Label = "Holder geometry (temporary preview)"
        self.preview_shape.ViewObject.Transparency = 65
        layout = QtGui.QVBoxLayout(self)
        intro = QtGui.QLabel(
            "The holder is shown in a temporary, unsaved document. Click a planar face "
            "to set an interface datum's origin and axis, or adjust its XYZ and direction "
            "fields below. Saving creates a new version; existing assemblies stay pinned "
            "to their current version."
        )
        intro.setWordWrap(True)
        layout.addWidget(intro)

        form = QtGui.QFormLayout()
        self.asset_combo = QtGui.QComboBox()
        from ..camassets import cam_assets
        for uri in cam_assets.list_assets("toolholder", store="local"):
            try:
                asset = cam_assets.get(uri, store="local", depth=0)
            except Exception:
                continue
            if isinstance(asset, HolderDefinition):
                key = str(uri)
                self.asset_by_uri[key] = asset
                self.asset_combo.addItem("{} — revision {}".format(asset.label, uri.version), key)
        form.addRow("Holder asset", self.asset_combo)
        self.name = QtGui.QLineEdit()
        form.addRow("Definition name", self.name)
        layout.addLayout(form)
        self.datum_fields = {}
        for side, title in (("tool-side", "Tool-side interface"), ("machine-side", "Machine-side interface")):
            box = QtGui.QGroupBox(title)
            grid = QtGui.QGridLayout(box)
            labels = ("Origin X", "Origin Y", "Origin Z", "Datum +Z X", "Datum +Z Y", "Datum +Z Z")
            fields = []
            for index, label in enumerate(labels):
                spin = QtGui.QDoubleSpinBox()
                spin.setDecimals(4 if index >= 3 else 3)
                spin.setRange(-10000.0, 10000.0) if index < 3 else spin.setRange(-1.0, 1.0)
                if index < 3:
                    spin.setSuffix(" mm")
                else:
                    spin.setSingleStep(0.1)
                    spin.setToolTip("Direction of this interface datum's local +Z axis")
                row, col = divmod(index, 3)
                grid.addWidget(QtGui.QLabel(label), row * 2, col)
                grid.addWidget(spin, row * 2 + 1, col)
                fields.append(spin)
            meta = QtGui.QFormLayout()
            pick_button = QtGui.QPushButton("Pick origin and axis from a face…")
            pick_button.clicked.connect(lambda checked=False, r=side: self.begin_face_pick(r))
            meta.addRow("3D selection", pick_button)
            interface_type = QtGui.QLineEdit()
            size = QtGui.QLineEdit()
            status = QtGui.QComboBox()
            status.addItems(["Candidate - verify before use", "Verified", "Incomplete"])
            meta.addRow("Interface type", interface_type)
            meta.addRow("Nominal size or range", size)
            meta.addRow("Definition status", status)
            grid.addLayout(meta, 4, 0, 1, 3)
            layout.addWidget(box)
            self.datum_fields[side] = (fields, interface_type, size, status)
            for field in fields:
                field.valueChanged.connect(self._update_preview_datums)

        self.asset_combo.currentIndexChanged.connect(self._load_selected)
        self.selection_observer = _SelectionObserver(self)
        FreeCADGui.Selection.addObserver(self.selection_observer)
        buttons = QtGui.QDialogButtonBox(QtGui.QDialogButtonBox.Save | QtGui.QDialogButtonBox.Cancel)
        buttons.accepted.connect(self.save_revision)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
        if not self.asset_by_uri:
            self.asset_combo.addItem("No holder assets found", "")
            self.asset_combo.setEnabled(False)
            buttons.button(QtGui.QDialogButtonBox.Save).setEnabled(False)
        else:
            self._load_selected()

    def _load_selected(self, *_args):
        asset = self.asset_by_uri.get(str(self.asset_combo.currentData()))
        if asset is None:
            return
        self.name.setText(asset.label)
        self.preview_shape.Shape = asset.shape
        for side, placement in (("tool-side", asset.tool_side_datum), ("machine-side", asset.machine_side_datum)):
            fields, interface_type, size, status = self.datum_fields[side]
            position = placement.Base
            axis = placement.Rotation.multVec(FreeCAD.Vector(0, 0, 1))
            values = (position.x, position.y, position.z, axis.x, axis.y, axis.z)
            for field, value in zip(fields, values):
                field.setValue(value)
            values = asset.interfaces.get(side, {})
            interface_type.setText(values.get("type", "Unspecified"))
            size.setText(values.get("size", "Unspecified"))
            index = status.findText(values.get("status", "Candidate - verify before use"))
            status.setCurrentIndex(index if index >= 0 else 0)
        self._update_preview_datums()
        self.preview_doc.recompute()
        FreeCADGui.activeDocument().activeView().fitAll()

    def begin_face_pick(self, side):
        self.pick_role = side
        self.setWindowTitle("Edit CAM Holder — pick {} interface face".format(side))
        FreeCADGui.Selection.clearSelection()

    def accept_face_pick(self, doc_name, obj_name, sub, point=None):
        if self.pick_role is None or doc_name != self.preview_doc.Name:
            return
        if obj_name != self.preview_shape.Name:
            return
        try:
            selection = type("Selection", (), {"Object": self.preview_shape, "SubElementNames": [sub]})
            _center, normal = _interface_frame(selection)
            origin = _center
            if point is not None:
                picked = FreeCAD.Vector(point)
                if self.preview_shape.Shape.getElement(sub).distToShape(Part.Vertex(picked))[0] < 0.05:
                    origin = picked
            if abs(normal.z) < 0.25:
                raise ValueError("Choose an end face whose normal follows the holder's +Z axis")
            if normal.z < 0:
                normal = normal.negative()
            fields, _kind, _size, _status = self.datum_fields[self.pick_role]
            for field, value in zip(fields, (origin.x, origin.y, origin.z, normal.x, normal.y, normal.z)):
                field.setValue(value)
            self.pick_role = None
            self.setWindowTitle("Edit CAM Holder Library Asset")
            self._update_preview_datums()
        except Exception as exc:
            QtGui.QMessageBox.warning(self, "Could not use selected face", str(exc))

    def _update_preview_datums(self, *_args):
        if not self.preview_doc or not self.preview_shape.Shape:
            return
        for side, (fields, _kind, _size, _status) in self.datum_fields.items():
            x, y, z, dx, dy, dz = (field.value() for field in fields)
            direction = FreeCAD.Vector(dx, dy, dz)
            if direction.Length < 1e-9:
                continue
            datum = self.preview_datums.get(side)
            if datum is None:
                datum = self.preview_doc.addObject("Part::LocalCoordinateSystem", "{}DatumPreview".format(side.title().replace("-", "")))
                datum.Label = "{} interface datum (temporary)".format(side.title())
                self.preview_datums[side] = datum
            datum.ViewObject.Visibility = True
            datum.Placement = FreeCAD.Placement(
                FreeCAD.Vector(x, y, z),
                FreeCAD.Rotation(FreeCAD.Vector(0, 0, 1), direction),
            )
        self.preview_doc.recompute()

    def save_revision(self):
        uri = str(self.asset_combo.currentData())
        asset = self.asset_by_uri.get(uri)
        label = self.name.text().strip()
        if asset is None or not label:
            QtGui.QMessageBox.warning(self, "Holder asset", "Choose a holder asset and enter a name.")
            return
        placements = {}
        interfaces = {}
        try:
            for side, (fields, interface_type, size, status) in self.datum_fields.items():
                x, y, z, ax, ay, az = (field.value() for field in fields)
                axis = FreeCAD.Vector(ax, ay, az)
                if axis.Length < 1e-9:
                    raise ValueError("{} datum axis cannot be zero".format(side))
                rotation = FreeCAD.Rotation(FreeCAD.Vector(0, 0, 1), axis)
                placements[side] = FreeCAD.Placement(FreeCAD.Vector(x, y, z), rotation)
                interfaces[side] = {
                    "type": interface_type.text().strip() or "Unspecified",
                    "size": size.text().strip() or "Unspecified",
                    "status": status.currentText(),
                }
            revised = HolderDefinition(
                label,
                asset.shape,
                placements["tool-side"],
                placements["machine-side"],
                interfaces,
                asset.get_id(),
            )
            new_uri = cam_assets.add(revised, store="local")
        except Exception as exc:
            QtGui.QMessageBox.critical(
                self, "Could not save holder revision", "No revision was saved.\n\n{}".format(exc)
            )
            return
        QtGui.QMessageBox.information(
            self,
            "Holder Revision Saved",
            "Saved {}. Existing tool assemblies remain pinned to their previous holder revision. "
            "To use this revision, create or assign a new assembly in the ToolBit Library.".format(new_uri),
        )
        self.accept()

    def done(self, result):
        try:
            FreeCADGui.Selection.removeObserver(self.selection_observer)
        except Exception:
            pass
        try:
            if self.preview_doc and FreeCAD.getDocument(self.preview_doc.Name):
                try:
                    FreeCADGui.getDocument(self.preview_doc.Name).Modified = False
                except Exception:
                    pass
                FreeCAD.closeDocument(self.preview_doc.Name)
        except Exception:
            pass
        if self.return_doc_name and FreeCAD.getDocument(self.return_doc_name):
            try:
                FreeCAD.setActiveDocument(self.return_doc_name)
                FreeCADGui.activateDocument(self.return_doc_name)
            except Exception:
                pass
        super().done(result)


class CommandHolderAssetEditor:
    def GetResources(self):
        return {
            "MenuText": QT_TRANSLATE_NOOP("CAM_ToolHolderAssetEdit", "Edit CAM Holder Library Asset…"),
            "ToolTip": QT_TRANSLATE_NOOP("CAM_ToolHolderAssetEdit", "Edit holder datums and metadata as a new versioned CAM asset"),
        }

    def IsActive(self):
        return True

    def Activated(self):
        global _active_dialog
        _active_dialog = HolderAssetEditorDialog()
        _active_dialog.show()
        _active_dialog.raise_()
        _active_dialog.activateWindow()


_active_dialog = None
FreeCADGui.addCommand("CAM_ToolHolderImport", CommandHolderImporter())
FreeCADGui.addCommand("CAM_ToolHolderEdit", CommandHolderEditor())
FreeCADGui.addCommand("CAM_ToolHolderAssetEdit", CommandHolderAssetEditor())
