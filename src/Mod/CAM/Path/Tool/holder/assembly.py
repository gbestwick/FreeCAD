# SPDX-License-Identifier: LGPL-2.1-or-later

"""Create a visual tool-and-holder assembly preview in the active document."""

import FreeCAD
import FreeCADGui
import Part
from PySide import QtGui
from PySide.QtCore import QT_TRANSLATE_NOOP
from ..camassets import cam_assets
from .assets import HolderDefinition, ToolAssembly


def _global_placement(obj):
    getter = getattr(obj, "getGlobalPlacement", None)
    if getter:
        return getter()
    placement = getattr(obj, "Placement", FreeCAD.Placement())
    parent = next(
        (
            candidate
            for candidate in getattr(obj, "InList", []) or []
            if getattr(candidate, "TypeId", "") == "App::Part"
        ),
        None,
    )
    return _global_placement(parent).multiply(placement) if parent else placement


def _holder_for(obj):
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
    datums = {
        getattr(child, "InterfaceRole", ""): child
        for child in getattr(holder, "Group", []) or []
        if getattr(child, "InterfaceRole", "") in ("Tool-side", "Machine-side")
    }
    if set(datums) != {"Tool-side", "Machine-side"}:
        return None, None
    return holder, datums


def _tool_bit_for(obj):
    """Resolve a selected ToolBit or ToolController to its generated body."""
    tool = getattr(obj, "Tool", obj)
    body = getattr(tool, "BitBody", None)
    if body is None or not hasattr(body, "Shape") or body.Shape.isNull():
        return None, None
    return tool, body


def _selection_pair():
    holder = datums = tool = body = None
    for obj in FreeCADGui.Selection.getSelection():
        candidate_holder, candidate_datums = _holder_for(obj)
        candidate_tool, candidate_body = _tool_bit_for(obj)
        if candidate_holder is not None:
            if holder is not None:
                return None
            holder, datums = candidate_holder, candidate_datums
        elif candidate_body is not None:
            if tool is not None:
                return None
            tool, body = candidate_tool, candidate_body
    if holder is None or body is None:
        return None
    return holder, datums, tool, body


def _preview_shapes(holder, tool_datum, body, stick_out):
    """Build normalized +Z shapes for the current source geometry and stick-out."""
    holder_world = _global_placement(holder)
    # A holder definition stores canonical geometry in its local frame. Its
    # App::Part placement only preserves the imported source orientation, so
    # retain the location but omit that rotation for this machine-axis preview.
    preview_frame = FreeCAD.Placement(holder_world.Base, FreeCAD.Rotation())
    tool_frame = preview_frame.multiply(tool_datum.Placement)
    axis = tool_frame.Rotation.multVec(FreeCAD.Vector(0, 0, 1))
    tip_position = tool_frame.Base.sub(axis.multiply(stick_out))
    tool_transform = FreeCAD.Placement(tip_position, tool_frame.Rotation)

    holder_shapes = []
    for child in getattr(holder, "Group", []) or []:
        try:
            shape = child.Shape
            if shape.isNull() or not shape.Solids:
                continue
            shape = shape.copy()
            child_frame = preview_frame.multiply(
                getattr(child, "Placement", FreeCAD.Placement())
            )
            shape.transformShape(child_frame.toMatrix(), True)
            holder_shapes.append(shape)
        except Exception:
            continue
    if not holder_shapes:
        raise ValueError("The selected holder has no solid geometry")

    if body is None:
        raise ValueError("The selected ToolBit no longer has a valid generated shape")
    tool_shape = body.Shape.copy()
    bounds = tool_shape.BoundBox
    tolerance = max(1e-5, max(bounds.XLength, bounds.YLength, bounds.ZLength) * 1e-7)
    if abs(bounds.ZMin) > tolerance:
        raise ValueError(
            "This preview expects ToolBit geometry with its cutting tip at local Z=0 "
            "and the shank extending toward +Z"
        )
    if abs((bounds.XMin + bounds.XMax) * 0.5) > tolerance or abs(
        (bounds.YMin + bounds.YMax) * 0.5
    ) > tolerance:
        raise ValueError("This preview expects the ToolBit axis to be centered on local Z")
    tool_shape.transformShape(tool_transform.toMatrix(), True)
    if not tool_shape.isValid():
        raise ValueError("The ToolBit shape could not be placed for preview")
    return Part.makeCompound(holder_shapes), tool_shape


class ToolAssemblyPreviewProxy:
    """Keep copied preview shapes synchronized with their linked inputs."""

    def execute(self, obj):
        holder_shape_feature = tool_shape_feature = None
        for child in getattr(obj, "Group", []) or []:
            role = getattr(child, "AssemblyRole", "")
            if role == "HolderGeometry":
                holder_shape_feature = child
            elif role == "ToolGeometry":
                tool_shape_feature = child
        if not holder_shape_feature or not tool_shape_feature:
            return
        try:
            holder_shape, tool_shape = _preview_shapes(
                obj.HolderDefinition,
                obj.ToolSideDatum,
                obj.ToolBody,
                obj.StickOut.Value,
            )
            holder_shape_feature.Shape = holder_shape
            tool_shape_feature.Shape = tool_shape
            holder_shape_feature.purgeTouched()
            tool_shape_feature.purgeTouched()
        except Exception as exc:
            FreeCAD.Console.PrintError(
                "Could not update {}: {}\n".format(obj.Label, exc)
            )

    def onDocumentRestored(self, obj):
        obj.Proxy = self


def _make_preview(document, holder, datums, tool, body, stick_out):
    """Create a normalized +Z preview that remains editable by stick-out."""
    holder_shape, tool_shape = _preview_shapes(
        holder, datums["Tool-side"], body, stick_out
    )

    document.openTransaction("Create CAM Tool Assembly Preview")
    try:
        preview = document.addObject(
            "App::DocumentObjectGroupPython", "CAMToolAssemblyPreview"
        )
        preview.Label = "{} + {} preview".format(tool.Label, holder.Label)
        preview.addProperty("App::PropertyLink", "HolderDefinition", "Assembly")
        preview.addProperty("App::PropertyLink", "ToolBit", "Assembly")
        preview.addProperty("App::PropertyLink", "ToolBody", "Assembly")
        preview.addProperty("App::PropertyLink", "ToolSideDatum", "Assembly")
        preview.addProperty("App::PropertyLength", "StickOut", "Assembly")
        preview.addProperty(
            "App::PropertyString", "PreviewNote", "Assembly", "Limitations of this visual preview"
        )
        preview.HolderDefinition = holder
        preview.ToolBit = tool
        preview.ToolBody = body
        preview.ToolSideDatum = datums["Tool-side"]
        preview.StickOut = stick_out
        preview.PreviewNote = "Visual placement only; interface compatibility and collisions are unchecked."

        for name, label, role, shape in (
            ("CAMPreviewHolderGeometry", holder.Label + " geometry", "HolderGeometry", holder_shape),
            ("CAMPreviewToolGeometry", tool.Label + " geometry", "ToolGeometry", tool_shape),
        ):
            feature = document.addObject("Part::Feature", name)
            feature.Label = label
            feature.Shape = shape
            feature.addProperty("App::PropertyString", "AssemblyRole", "Assembly")
            feature.AssemblyRole = role
            preview.addObject(feature)
        preview.Proxy = ToolAssemblyPreviewProxy()
        document.recompute()
        document.commitTransaction()
    except Exception:
        document.abortTransaction()
        raise
    return preview


class CommandToolAssemblyPreview:
    def GetResources(self):
        return {
            "MenuText": QT_TRANSLATE_NOOP("CAM_ToolAssemblyPreview", "Preview Tool and Holder Assembly"),
            "ToolTip": QT_TRANSLATE_NOOP("CAM_ToolAssemblyPreview", "Place a CAM ToolBit at a holder's tool-side datum using a specified stick-out"),
        }

    def IsActive(self):
        return FreeCAD.ActiveDocument is not None and _selection_pair() is not None

    def Activated(self):
        selection = _selection_pair()
        if selection is None:
            QtGui.QMessageBox.information(
                FreeCADGui.getMainWindow(),
                "Tool Assembly Preview",
                "Select one holder definition and one CAM ToolBit or ToolController.",
            )
            return

        holder, datums, tool, body = selection
        stick_out, accepted = QtGui.QInputDialog.getDouble(
            FreeCADGui.getMainWindow(),
            "Tool Assembly Preview",
            "Tool stick-out from the holder tool-side datum (mm). ToolBit tip is at local Z=0:",
            25.0,
            0.001,
            10000.0,
            3,
        )
        if not accepted:
            return
        try:
            preview = _make_preview(
                FreeCAD.ActiveDocument, holder, datums, tool, body, stick_out
            )
        except Exception as exc:
            QtGui.QMessageBox.critical(
                FreeCADGui.getMainWindow(),
                "Could not create preview",
                "No tool assembly preview was created.\n\n{}".format(exc),
            )
            return
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(preview)


def _assembly_controller_pair():
    preview = controller = None
    for obj in FreeCADGui.Selection.getSelection():
        if all(hasattr(obj, prop) for prop in ("HolderDefinition", "ToolBit", "StickOut")):
            if preview is not None:
                return None
            preview = obj
        elif hasattr(obj, "CAMToolAssembly") and hasattr(obj, "Tool"):
            if controller is not None:
                return None
            controller = obj
    if preview is None or controller is None:
        return None
    controller_tool, _body = _tool_bit_for(controller)
    preview_tool = preview.ToolBit
    if controller_tool is None or preview_tool is None:
        return None
    if controller_tool.Document.Name != preview_tool.Document.Name or controller_tool.Name != preview_tool.Name:
        return None
    if controller.Document.Name != preview.Document.Name:
        return None
    return preview, controller


class CommandAssignToolAssembly:
    def GetResources(self):
        return {
            "MenuText": QT_TRANSLATE_NOOP("CAM_AssignToolAssembly", "Assign Tool Assembly to Tool Controller"),
            "ToolTip": QT_TRANSLATE_NOOP("CAM_AssignToolAssembly", "Link a matching tool and holder preview to a CAM ToolController for simulation"),
        }

    def IsActive(self):
        return FreeCAD.ActiveDocument is not None and _assembly_controller_pair() is not None

    def Activated(self):
        pair = _assembly_controller_pair()
        if pair is None:
            QtGui.QMessageBox.information(
                FreeCADGui.getMainWindow(),
                "Assign Tool Assembly",
                "Select one tool assembly preview and the CAM ToolController that uses the same ToolBit.",
            )
            return
        preview, controller = pair
        document = controller.Document
        document.openTransaction("Assign CAM Tool Assembly")
        try:
            controller.CAMToolAssembly = preview
            document.recompute()
            document.commitTransaction()
        except Exception as exc:
            document.abortTransaction()
            QtGui.QMessageBox.critical(
                FreeCADGui.getMainWindow(),
                "Could not assign Tool Assembly",
                "The ToolController was not changed.\n\n{}".format(exc),
            )
            return
        QtGui.QMessageBox.information(
            FreeCADGui.getMainWindow(),
            "Tool Assembly Assigned",
            "'{}' is now linked to '{}'. Save the CAM project to keep this setup.".format(
                preview.Label, controller.Label
            ),
        )


class CommandToolAssemblyGuide:
    def GetResources(self):
        return {
            "MenuText": QT_TRANSLATE_NOOP("CAM_ToolAssemblyGuide", "Tool and Holder Assembly Workflow"),
            "ToolTip": QT_TRANSLATE_NOOP("CAM_ToolAssemblyGuide", "How to create, store, reuse, and assign tool and holder assemblies"),
        }

    def IsActive(self):
        return FreeCAD.ActiveDocument is not None

    def Activated(self):
        QtGui.QMessageBox.information(
            FreeCADGui.getMainWindow(),
            "Tool and Holder Assembly Workflow",
            "Create and use a tool/holder assembly\n\n"
            "1. Import the holder model into a temporary or working document. If it has several solids, select the solids or their containing group.\n"
            "2. Run Create Tool Holder Definition. Pick the tool-side and machine-side mating faces, confirm +Z points from tool toward spindle, then save. This writes a versioned holder asset to the CAM asset library; it does not add holder geometry to the project tree.\n"
            "3. Open the ToolBit Library editor, select the numbered ToolBit, and use its context menu: Assign Tool Assembly. Choose the holder, give the assembly a name, and enter measured stick-out from the tool-side datum to the cutting tip.\n"
            "4. Add that ToolBit to a CAM Job from the library. Its ToolController receives the assembly reference automatically. Existing jobs can use Configure Tool Assembly on the selected ToolController.\n"
            "5. Save the project. The job stores only the versioned asset URI and assembly label; the holder solid stays in the CAM asset library. The simulator resolves the asset when it starts.\n\n"
            "The assembly is tied to the ToolBit identity and a specific holder revision. Older library files without assembly references continue to load unchanged. The preview command remains available for checking a setup visually, but its temporary geometry is not required in the job.\n\n"
            "The two interface datums begin as candidates. Confirm the axis and face offsets during import before relying on the setup. To correct a saved holder, use Edit CAM Holder Library Asset; this creates a new pinned revision. Create or reassign an assembly if you want a job to use it. The preview does not verify interface fit or machine/spindle clearance.",
        )


class ToolAssemblyLibraryDialog(QtGui.QDialog):
    def __init__(self, controller, toolbit_id, tool_label, parent=None, tool_source=None):
        super().__init__(parent or FreeCADGui.getMainWindow())
        self.controller = controller
        self.toolbit_id = toolbit_id
        self.tool_label = tool_label
        self.tool_source = tool_source
        self.return_doc_name = FreeCAD.ActiveDocument.Name if FreeCAD.ActiveDocument else None
        self.preview_doc = FreeCAD.newDocument("CAMToolAssemblyPreview")
        self.preview_doc.Label = "Temporary CAM Tool Assembly Preview"
        self.preview_holder = self.preview_doc.addObject("Part::Feature", "PreviewHolder")
        self.preview_holder.Label = "Holder (temporary preview)"
        self.preview_holder.ViewObject.ShapeColor = (0.72, 0.76, 0.82)
        self.preview_tool = self.preview_doc.addObject("Part::Feature", "PreviewTool")
        self.preview_tool.Label = "ToolBit (temporary preview)"
        self.preview_tool.ViewObject.ShapeColor = (0.85, 0.55, 0.18)
        self.preview_datums = {}
        self.tool_shape = None
        self._preview_fitted = False
        self.setWindowTitle("Tool Assembly for {}".format(getattr(controller, "Label", tool_label)))
        self.setMinimumWidth(500)

        layout = QtGui.QVBoxLayout(self)
        intro = QtGui.QLabel(
            "Choose a saved assembly for this ToolController, or create one. "
            "The assembly must use the controller's current ToolBit. Holder geometry stays in the CAM asset library."
        )
        intro.setWordWrap(True)
        layout.addWidget(intro)
        self.preview_status = QtGui.QLabel(
            "A temporary preview document is open. Move this dialog aside to inspect it; the preview closes with this dialog."
        )
        self.preview_status.setWordWrap(True)
        layout.addWidget(self.preview_status)

        form = QtGui.QFormLayout()
        self.assembly = QtGui.QComboBox()
        self.assembly.addItem("Create a new tool assembly…", "__new__")
        self.assembly_uris = {}
        for uri in cam_assets.list_assets("toolassembly", store="local"):
            try:
                asset = cam_assets.get(uri, store="local", depth=0)
            except Exception:
                continue
            if isinstance(asset, ToolAssembly) and asset.toolbit_id == toolbit_id:
                self.assembly_uris[str(uri)] = asset
                self.assembly.addItem("{} ({})".format(asset.label, uri.version), str(uri))
        form.addRow("Assembly", self.assembly)

        self.new_fields = QtGui.QWidget()
        new_form = QtGui.QFormLayout(self.new_fields)
        new_form.setContentsMargins(0, 0, 0, 0)
        self.holder = QtGui.QComboBox()
        self.holder_uris = {}
        for uri in cam_assets.list_assets("toolholder", store="local"):
            try:
                asset = cam_assets.get(uri, store="local", depth=0)
            except Exception:
                continue
            if isinstance(asset, HolderDefinition):
                self.holder_uris[str(uri)] = asset
                self.holder.addItem("{} ({})".format(asset.label, uri.version), str(uri))
        new_form.addRow("Holder", self.holder)
        self.name = QtGui.QLineEdit()
        new_form.addRow("Assembly name", self.name)
        self.stick_out = QtGui.QDoubleSpinBox()
        self.stick_out.setRange(0.001, 10000.0)
        self.stick_out.setDecimals(3)
        self.stick_out.setValue(25.0)
        self.stick_out.setSuffix(" mm")
        self.stick_out.setToolTip("Distance from the holder's tool-side datum to the ToolBit cutting tip.")
        new_form.addRow("Stick-out", self.stick_out)
        layout.addLayout(form)
        layout.addWidget(self.new_fields)
        if not self.holder_uris:
            self.new_fields.setEnabled(False)
            self.assembly.setItemText(0, "No saved holders found; create a holder definition first")
        self.assembly.currentIndexChanged.connect(self._update_mode)
        self.assembly.currentIndexChanged.connect(self._update_preview)
        self.holder.currentIndexChanged.connect(self._update_default_name)
        self.holder.currentIndexChanged.connect(self._update_preview)
        self.stick_out.valueChanged.connect(self._update_preview)
        self._update_default_name()
        buttons = QtGui.QDialogButtonBox(QtGui.QDialogButtonBox.Save | QtGui.QDialogButtonBox.Cancel)
        buttons.accepted.connect(lambda: self.done(QtGui.QDialog.Accepted))
        buttons.rejected.connect(lambda: self.done(QtGui.QDialog.Rejected))
        layout.addWidget(buttons)
        self._load_tool_shape()
        self._update_preview()

    def _load_tool_shape(self):
        source = self.tool_source
        if source is None:
            self.preview_status.setText("Tool geometry is unavailable for this ToolBit.")
            return
        body = getattr(source, "BitBody", None)
        if body is not None and hasattr(body, "Shape") and not body.Shape.isNull():
            self.tool_shape = body.Shape.copy()
            return
        shape_model = getattr(source, "_tool_bit_shape", None)
        if shape_model is None:
            self.preview_status.setText("Tool geometry is unavailable for this ToolBit.")
            return
        try:
            generated = shape_model.make_body(self.preview_doc)
            if generated is None or generated.Shape.isNull():
                raise ValueError("ToolBit shape did not generate a solid")
            self.tool_shape = generated.Shape.copy()
            for obj in list(self.preview_doc.Objects):
                if obj not in (self.preview_holder, self.preview_tool):
                    try:
                        self.preview_doc.removeObject(obj.Name)
                    except Exception:
                        # Helper objects remain confined to the temporary document.
                        pass
        except Exception as exc:
            self.preview_status.setText("Could not build ToolBit preview: {}".format(exc))

    def _preview_asset(self):
        uri = self.assembly.currentData()
        if uri == "__new__":
            return self.holder_uris.get(str(self.holder.currentData())), self.stick_out.value()
        assembly = cam_assets.get(uri, store="local", depth=None)
        holder = getattr(assembly, "holder", None)
        if holder is None:
            holder = cam_assets.get(assembly.holder_uri, store="local", depth=None)
        return holder, assembly.stick_out_mm

    def _update_preview(self, *_args):
        try:
            holder, stick_out = self._preview_asset()
            if holder is None or self.tool_shape is None:
                self.preview_status.setText("Choose a holder and ensure the ToolBit geometry is available.")
                return
            tool_shape = self.tool_shape.copy()
            bounds = tool_shape.BoundBox
            tolerance = max(1e-5, max(bounds.XLength, bounds.YLength, bounds.ZLength) * 1e-7)
            if (abs(bounds.ZMin) > tolerance or
                    abs((bounds.XMin + bounds.XMax) * 0.5) > tolerance or
                    abs((bounds.YMin + bounds.YMax) * 0.5) > tolerance):
                raise ValueError("ToolBit preview expects its cutting tip at local Z=0, centered on local Z")
            datum = holder.tool_side_datum
            axis = datum.Rotation.multVec(FreeCAD.Vector(0, 0, 1))
            tip = datum.Base.sub(axis.multiply(stick_out))
            tool_shape.transformShape(FreeCAD.Placement(tip, datum.Rotation).toMatrix(), True)
            self.preview_holder.Shape = holder.shape.copy()
            self.preview_tool.Shape = tool_shape
            for side, placement in (("Tool", holder.tool_side_datum), ("Machine", holder.machine_side_datum)):
                name = self.preview_datums.get(side)
                datum_obj = self.preview_doc.getObject(name) if name else None
                if datum_obj is None:
                    datum_obj = self.preview_doc.addObject("Part::LocalCoordinateSystem", side + "InterfaceDatum")
                    datum_obj.Label = side + "-side interface datum"
                    self.preview_datums[side] = datum_obj.Name
                datum_obj.Placement = placement
                datum_obj.ViewObject.Visibility = True
            self.preview_holder.ViewObject.Visibility = True
            self.preview_tool.ViewObject.Visibility = True
            self.preview_doc.recompute()
            self.preview_status.setText(
                "Preview: {:.3f} mm stick-out. Datums are shown; interface fit and collisions are not checked.".format(stick_out)
            )
            if not self._preview_fitted:
                gui_doc = FreeCADGui.activeDocument()
                if gui_doc:
                    view = gui_doc.activeView()
                    view.viewAxonometric()
                    view.fitAll()
                self._preview_fitted = True
        except Exception as exc:
            self.preview_status.setText("Preview unavailable: {}".format(exc))

    def done(self, result):
        preview_name = self.preview_doc.Name if self.preview_doc else None
        self.preview_doc = None
        try:
            if preview_name and preview_name in FreeCAD.listDocuments():
                FreeCAD.closeDocument(preview_name)
        except Exception:
            pass
        if self.return_doc_name and self.return_doc_name in FreeCAD.listDocuments():
            FreeCAD.setActiveDocument(self.return_doc_name)
        super().done(result)

    def closeEvent(self, event):
        if self.preview_doc is not None:
            self.done(QtGui.QDialog.Rejected)
        else:
            super().closeEvent(event)

    def _update_mode(self, _index):
        self.new_fields.setVisible(self.assembly.currentData() == "__new__")

    def _update_default_name(self, _index=0):
        holder = self.holder_uris.get(str(self.holder.currentData()))
        if holder and not self.name.text().strip():
            self.name.setText("{} + {}".format(self.tool_label, holder.label))

    def selected_asset(self):
        uri = self.assembly.currentData()
        if uri != "__new__":
            return str(uri), self.assembly_uris.get(str(uri))
        holder_uri = str(self.holder.currentData())
        holder = self.holder_uris.get(holder_uri)
        label = self.name.text().strip()
        if holder is None or not label:
            raise ValueError("Choose a holder and enter a name for the tool assembly")
        assembly = ToolAssembly(
            label,
            self.toolbit_id,
            holder_uri,
            self.stick_out.value(),
            holder,
        )
        uri = cam_assets.add(assembly, store="local")
        return str(uri), assembly


class CommandConfigureToolAssembly:
    def GetResources(self):
        return {
            "MenuText": QT_TRANSLATE_NOOP("CAM_ConfigureToolAssembly", "Configure Tool Assembly…"),
            "ToolTip": QT_TRANSLATE_NOOP("CAM_ConfigureToolAssembly", "Choose or create a reusable holder assembly for the selected ToolController"),
        }

    def _controller(self):
        selected = FreeCADGui.Selection.getSelection()
        if len(selected) != 1:
            return None
        obj = selected[0]
        return obj if hasattr(obj, "Tool") and hasattr(obj, "ToolNumber") else None

    def IsActive(self):
        controller = self._controller()
        return controller is not None and getattr(controller, "Tool", None) is not None

    def Activated(self):
        controller = self._controller()
        tool = getattr(controller, "Tool", None) if controller else None
        toolbit_id = getattr(tool, "ToolBitID", "") if tool else ""
        if not toolbit_id:
            QtGui.QMessageBox.information(
                FreeCADGui.getMainWindow(), "Tool Assembly",
                "Select a ToolController with a saved CAM ToolBit before configuring its holder assembly.",
            )
            return
        dialog = ToolAssemblyLibraryDialog(
            controller, toolbit_id, tool.Label, tool_source=tool
        )
        dialog.finished.connect(
            lambda result, dlg=dialog, tc=controller, bit_id=toolbit_id:
                self._finish_configuration(result, dlg, tc, bit_id)
        )
        dialog.show()
        dialog.raise_()
        dialog.activateWindow()

    def _finish_configuration(self, result, dialog, controller, toolbit_id):
        if result != QtGui.QDialog.Accepted:
            return
        try:
            uri, assembly = dialog.selected_asset()
            if assembly is None:
                assembly = cam_assets.get(uri, store="local", depth=None)
            if not isinstance(assembly, ToolAssembly) or assembly.toolbit_id != toolbit_id:
                raise ValueError("The selected assembly does not match this controller's ToolBit")
        except Exception as exc:
            QtGui.QMessageBox.critical(
                FreeCADGui.getMainWindow(), "Tool Assembly",
                "The assembly could not be saved or resolved. The ToolController was not changed.\n\n{}".format(exc),
            )
            return

        doc = controller.Document
        doc.openTransaction("Configure CAM Tool Assembly")
        try:
            controller.CAMToolAssemblyURI = uri
            controller.CAMToolAssemblyLabel = assembly.label
            # Clear the earlier document-object prototype link so the external
            # asset is the single authoritative setup for this controller.
            if hasattr(controller, "CAMToolAssembly"):
                controller.CAMToolAssembly = None
            doc.recompute()
            doc.commitTransaction()
        except Exception as exc:
            doc.abortTransaction()
            QtGui.QMessageBox.critical(
                FreeCADGui.getMainWindow(), "Tool Assembly",
                "The ToolController was not updated.\n\n{}".format(exc),
            )
            return
        QtGui.QMessageBox.information(
            FreeCADGui.getMainWindow(), "Tool Assembly Configured",
            "'{}' is assigned to T{}. Its holder geometry remains in the CAM asset library. Save the CAM project to keep the reference.".format(
                assembly.label, controller.ToolNumber
            ),
        )


if FreeCAD.GuiUp:
    FreeCADGui.addCommand("CAM_ToolAssemblyPreview", CommandToolAssemblyPreview())
FreeCADGui.addCommand("CAM_AssignToolAssembly", CommandAssignToolAssembly())
FreeCADGui.addCommand("CAM_ToolAssemblyGuide", CommandToolAssemblyGuide())
FreeCADGui.addCommand("CAM_ConfigureToolAssembly", CommandConfigureToolAssembly())
