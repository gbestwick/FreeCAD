# Creating and reusing CAM tool/holder assemblies

The workflow keeps reusable holder geometry in the CAM asset library and stores a versioned assembly reference on each numbered ToolBit entry. A CAM job stores the selected assembly URI on its ToolController. No holder solid or permanent preview objects are added to the job tree.

## Build a reusable holder asset

1. Open or import the holder model in a working document. Multi-solid models can be selected as their component solids or as a containing group.
2. Select the geometry and choose **CAM → Create Tool Holder Definition**.
3. Pick the tool-side and machine-side interface faces. Set the face offsets as needed and confirm **+Z points from tool side toward machine side**.
4. Save. The holder shape and interface datums are stored as a versioned asset in the CAM asset directory. The source solids remain unchanged and no holder geometry is inserted into the document tree.

The face-derived datums are candidates. Review the axis and offsets against the real holder before using the asset for simulation. The asset library is user-local; projects that reference these assets need access to the same CAM asset directory or an imported copy of those assets.

To adjust a saved holder, choose **CAM → Edit CAM Holder Library Asset…**. FreeCAD opens its shape in a temporary preview document. Click **Pick origin and axis from a face…**, then click the planar interface face at the desired datum origin; the face normal sets the datum's +Z direction. The datum triads update in the preview, and you can fine-tune XYZ and direction in the dialog. Save to create a new revision; assemblies and jobs pinned to the previous revision are unchanged. To use the new revision, right-click the library ToolBit, choose **Change Tool Assembly…**, create an assembly using that holder revision, and save the library.

## Assemble a numbered ToolBit

1. Open the CAM ToolBit Library editor and select the library containing the numbered ToolBit.
2. Right-click the ToolBit and choose **Assign Tool Assembly…**.
3. Choose an existing assembly or create one by selecting a holder asset, naming the assembly, and entering measured stick-out from the holder's tool-side datum to the cutting tip.
4. The library records the versioned assembly URI alongside that ToolBit. Reassigning it later updates the library entry; older `.fctl` libraries without this optional field continue to work as before.

This associates the assembly with the ToolBit identity, while the library's tool number remains the machine/pocket number. The holder asset is versioned so later edits do not silently change an existing setup.

In a numbered ToolBit library, each tool row also shows its assembly assignment, or indicates that no assembly is assigned. Search matches assembly names, and a stale assembly reference is marked as missing with a **Repair Missing Tool Assembly…** context-menu action.

## Add the tool to a job

Add the numbered ToolBit to a job from the library as usual. The created ToolController automatically receives the assembly reference. The job tree contains the normal ToolController and ToolBit objects; holder geometry remains outside the document. For a ToolController already in a job, select it and choose **CAM → Configure Tool Assembly…**.

On simulator startup, the external holder asset is resolved once per assembly URI and reused across operations. If the assembly cannot be found or its ToolBit ID does not match, the simulator reports a warning and runs without that holder. A temporary **Preview Tool and Holder Assembly** document preview remains available for visually checking a setup, but it is not required for saving or using the job reference.

## Current limits

The assembly assignment dialog opens a separate, temporary preview document and updates the tool, holder, stick-out, and both interface datums as the selection changes. The library editor and assembly chooser are modeless so the preview can be rotated and inspected while making a selection. The preview closes with the chooser and does not add objects to the CAM job tree. It assumes a canonical +Z tool axis and does not validate mating-interface compatibility, spindle fit, fixture clearance, or collision clearance. A dedicated inventory browser for assemblies across libraries and portable project asset bundles remain future workflow items. Asset references are local CAM-library references.
