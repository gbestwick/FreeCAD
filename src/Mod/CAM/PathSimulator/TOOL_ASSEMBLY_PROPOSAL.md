# CAM Tool Assembly Proposal

Status: design note with a first implementation now in source. The CAM asset manager stores versioned holder and assembly assets; numbered `.fctl` entries can reference an assembly; adding a library ToolBit to a job copies the URI to the ToolController; and the simulator resolves the external holder shape without inserting it in the job document. FreeCADCmd round-trip checks pass. GUI workflow and portability still need user validation. The requirements and open questions below remain useful for follow-up work.

## Goal

Describe a tool and its holder stack well enough to place a conservative collision envelope in the machine coordinate frame. Keep reusable component definitions and configured tool assemblies in a reusable library, separate from CAM jobs. A job should reference an assembly and its machine-specific assignment without importing the assembly geometry into the document tree.

## User experience requirements

- Users should be able to prepare a reusable inventory of their real cutting tools, holders, and assembled combinations (for example, a drill fitted to a chuck with measured stick-out), then select those assemblies in multiple CAM jobs.
- Holder and assembly geometry must be stored in the library or another external asset store, not copied into every job document. Opening a job should resolve the reference and load geometry for simulation as needed.
- Normal job setup should add only compact references and job-specific settings to existing ToolControllers, such as tool number or magazine pocket. It should not add a forest of geometry, datum, and preview objects to the Model tree.
- Any geometry created only to draw a preview should be transient or hidden behind a single, clearly named setup object; it should not persist as multiple ordinary document objects by default.
- Library identity and missing/changed asset behavior must be explicit. A job should not silently switch to different geometry when a library entry is edited. Stable IDs and revision/pinning or an equivalent policy need to be evaluated before implementation.
- Reuse should be simple for users who organize their favorite machine inventory separately from a particular job. A machine's tool-changer pocket/tool number belongs to a machine or job setup, while the underlying tool assembly remains reusable.

The holder authoring command now saves geometry and datums as an external versioned CAM asset instead of creating a persistent holder object in the source document. The document-level assembly preview remains available as a visual validation tool; normal job use stores only a reference on the ToolController.

## CAM asset-system inspection

Source inspection on 2026-10-22 found that FreeCAD already has a suitable asset framework to extend:

- `Path.Tool.camassets` creates a `CamAssetManager` with a per-user filesystem store rooted at the configured `CamAssets` directory, plus a built-in fallback store. The local CAM asset tree already separates ToolBits, ToolBit libraries, shapes, and machine configuration files.
- `Asset`, `AssetSerializer`, `AssetUri`, and `AssetManager` support named asset types, serialization, dependency resolution, and in-memory asset construction. `FileStore` can store arbitrary byte data and has versioned path support. This makes a separate holder-library subsystem unnecessary as a first choice.
- ToolBits are stored as `.fctb` parameter data and refer to reusable parametric ToolBitShape assets. `.fctl` ToolBit libraries contain tool-number-to-ToolBit-ID mappings. The library browser loads assets in memory and creates a ToolController plus an embedded ToolBit object in the job when the user chooses **Add to Job**.
- A ToolController stores a link to its job's ToolBit document object. That object carries the persistent ToolBit ID used by the tool-update workflow, and its generated body is hidden from the tree. This is an established pattern for compact job data with a local tool snapshot, though it should not be copied literally for detailed holder CAD.
- `FileStore` supports numbered versions, but the current CAM asset mappings for `toolbit` and `toolbitlibrary` do not include a `{version}` path component. Their current URIs and `.fctb`/`.fctl` references therefore resolve to the current unversioned file. `AssetUri` can express a version, but current ToolBit library dependencies do not pin one.
- The manager can resolve asset dependencies recursively. Its cache is optional; `CamAssetManager` currently registers its local and built-in stores without enabling that cache. A holder-geometry resolver will need to cache parsed CAD shapes at runtime without creating document objects.

### First storage model

The first implementation extends the existing CAM asset manager with two asset types rather than introducing another library root or placing holder solids in every job:

1. A **holder definition asset** stores a stable ID, name, shape, tool-side and machine-side datum placements, and interface metadata in a versioned native FCStd payload (`.fcholder`). Multi-solid shape geometry stays outside the project document.
2. A **tool-assembly asset** (`.fctasm`) references the existing ToolBit ID, one holder-definition revision, and the actual stick-out. It represents a configured combination such as “5 mm drill in Albrecht chuck, 22 mm stick-out.” It is reusable across jobs and does not contain a copy of the ToolBit geometry.

A ToolController should keep its existing ToolBit snapshot for path generation and add only a compact assembly asset reference and resolved display name/revision metadata. Assignment must verify that the assembly's ToolBit ID matches the controller's ToolBit ID. The machine/job ToolNumber stays on the ToolController; a future machine tool-set asset may map magazine pockets to assembly references, but should not be conflated with the reusable assembly itself.

The simulator should resolve the assembly and holder geometry on demand, cache the resulting shape in memory, and render it through the simulator's existing holder-render path. It must not create persistent Part/Part::Feature objects in the job tree. Missing or incompatible assets should produce a clear unresolved-setup finding and mark holder geometry coverage unchecked.

### Revision and portability policy to prototype

Use immutable numbered revisions for holder definitions and assemblies, and save an exact asset URI/revision in the ToolController reference. Updating geometry should create a new revision; it must not silently change an existing job's assembly. The assembly should reference exact revisions of holder/adapter assets. The job's existing embedded ToolBit snapshot remains the geometry used by its operations; the stable ToolBit ID is checked against the assembly, and FreeCAD's existing tool-update flow remains responsible for updating that snapshot.

Holder and assembly mappings now use numbered versions, and jobs pin exact URIs. Cross-machine project portability still needs either a configured shared asset folder, a clear “asset missing” repair flow, or a deliberate “pack required assets” export. Do not silently embed geometry into the FCStd job as a fallback because that would violate the no-tree/no-duplicate-geometry requirement and obscure where the authoritative asset lives.

The first library workflow now extends the existing ToolBit browser. A right-click **Assign Tool Assembly…** action associates an exact assembly URI with the numbered ToolBit, and **Add to Job** propagates that reference to the controller. This lets a `.fctl` library represent a favorite machine-changer set without duplicating holder geometry.

This is a good initial fit for the example workflow, but it does not mean tool number belongs to the assembly. The reusable assembly describes the physical tool/holder combination; the `.fctl` library associates that combination with a particular tool number for a chosen machine inventory. Keep that distinction so the same physical assembly can occupy different pockets in other machines.

Implementation details for follow-up validation:

- Confirm optional assembly URI metadata survives external `.fctl` import/export paths as intended; the native CAM `.fctl` serializer preserves it and old entries remain compatible.
- Keep the per-job ToolBit document object as FreeCAD currently does; it is the compact parametric tool snapshot used by CAM operations. Do not add the external holder CAD model or a persistent copied assembly preview to the job document.
- When **Add to Job** creates the controller, it now writes the assembly URI to a string property on that existing ToolController. The simulator resolves each unique assembly once per run. Confirm missing assets and ToolBit mismatches remain visible through a user-facing repair/status flow; the first slice currently logs a warning and continues without holder geometry.
- Defer assigning a complete machine `.fcm` profile to a tool library until the `.fctl` extension is validated; users can already choose the correct tool library for the machine, and a later link can improve convenience.

## First scope

Start with static 3-axis setups and holders that can be represented by an axisymmetric envelope. A holder may be described by an axial profile or by a CAD shape. The cutting tool continues to come from its CAM ToolBit where possible. Do not require spindle CAD geometry for the first holder-to-stock/workholding checks; report spindle and machine-body checks as unchecked when those shapes are absent.

## Holder definition

A reusable holder definition should contain:

- Stable identifier, display name, holder family/type, and units.
- A geometry source: an axial envelope profile or a referenced CAD shape. The source and approximation level must be visible to the user.
- A local axis and explicit local coordinate frame.
- A **tool-side interface**: interface type, axis/orientation, mating datum, supported tool/shank diameter or range where relevant, and insertion depth (or a per-instance stick-out).
- A **machine-side interface**: interface type, axis/orientation, mating datum, and the compatible spindle or upstream-adapter interface.
- Optional notes and source/provenance for dimensions.

The two interfaces are independent of the external collision geometry. Geometry alone must never imply either mounting relationship.

## How a user defines mount interfaces

Do not ask users to supply unexplained XYZ numbers or rely on undocumented conventions. Treat each mount point as an **interface datum**: a local coordinate system (origin plus orientation) paired with an interface type and size. The saved holder definition should show both datums in the tree and in the 3D view.

For a new imported holder, a guided workflow should:

1. Ask the user to select the holder geometry (one or more solids) and confirm units.
2. Ask for the tool-side interface, then the machine-side interface. For each, the user selects a cylindrical/conical face or axis to establish its centerline and a plane/face or point to establish its origin. The tool displays a triad and an arrow; the user can flip the direction or adjust the origin before accepting it. A manual placement editor is the fallback for geometry that does not expose suitable analytic faces.
3. Ask for an interface type/name and relevant nominal size or range. Keep mechanical compatibility data separate from the selected geometry. Insertion depth/stick-out is set on the tool assembly instance, with an optional nominal default on the holder definition.
4. Show a mating preview with a simple shaft/tool on one side and a simple spindle/arbor stub on the other. Prevent saving a supposedly complete holder if either interface is absent; allow an explicitly incomplete definition with visible warnings.
5. Save the definition to the reusable library or asset store. A standalone editable FreeCAD document can serve as an inspection/editing view, but embedding copied geometry into each job is not the intended storage model.

FreeCAD already uses `Part::LocalCoordinateSystem` objects for CAM workplanes, including placements and attachment to geometry. That is a strong native starting point for the visible interface datums. For imported CAD with unstable face numbering, the editor should show whether a datum is attached to a selected subelement or is a manually positioned local datum, and flag a broken attachment after the source geometry changes. See `Path/Main/Workplane.py` for the CAM workplane pattern.

The first CAM workbench authoring command copies selected individual solids or geometry held in a container into a named `App::Part`. It reports how many source objects and solids it found, then lets the user select planar faces on included objects for tool-side and machine-side datums. One checkbox confirms the canonical holder frame: local +Z runs from tool side (negative Z) toward machine side (positive Z). The line between selected interface centers defines the axis; geometry and datum placements are transformed into this local frame while the holder remains in its original document position. Users can set axial offsets and interface types. Saving is transactional to avoid leaving a partial definition if geometry processing fails. This first slice does not infer axes from cylindrical/conical faces, attach datums to source subelements for later updates, confirm unit interpretation, capture nominal size/range, or provide library export and mating preview. Datums remain marked as candidates until the user verifies them.

A follow-up CAM command edits an existing definition's name, interface type, nominal size/range, and status (`Candidate`, `Verified`, or `Incomplete`). Datum origins and axes remain native `Part::LocalCoordinateSystem` placements and can be edited directly in FreeCAD's property editor; the command displays each datum's current origin and points users to that native placement editor. This keeps geometry placement in FreeCAD's established editing model while making interface metadata and verification state easier to find.

A first assembly-preview prototype accepts one holder definition plus one CAM ToolBit or ToolController. It copies their shapes into a separate preview group, placing the ToolBit cutting tip at a user-entered stick-out from the holder's tool-side datum. The preview uses the holder's canonical local frame with its axis along global +Z, while retaining the holder definition's origin; the source model's saved world rotation is deliberately omitted. It expects the standard ToolBit frame (cutting tip at local Z=0, tool axis centered on +Z). It is a visual placement check only: interface fit, spindle-side mating, and collision clearance remain unchecked. This document-object preview is explicitly a prototype; the intended routine job workflow should resolve external assembly geometry transiently and avoid adding the copied holder and tool shapes to the project tree.

An assembly instance should reference the holder definition, the selected ToolBit, the actual insertion/stick-out value, and any adapters between holder and spindle. It must resolve each mating interface into a single transform chain ending at a declared spindle gauge line or equivalent machine reference. Invalid or unmatched interfaces leave the assembly unplaced and collision coverage unchecked.

## Geometry representation for the first prototype

For axisymmetric parts, use a sequence of axial stations `(z, radius)` in the component's local coordinates. Adjacent stations define conical or cylindrical envelope sections; the resulting profile is revolved about the local axis and closed at its ends. This is a conservative outer envelope, not a detailed mechanical model: it need not model jaws, teeth, screws, or internal cavities for initial clearance checks.

The profile must have finite, positive dimensions, ordered stations, a declared unit, and an explicit axis/datum. If a part is non-axisymmetric or the envelope would be misleading, use a CAD shape or report that this first profile representation cannot cover it. Imported/document CAD shapes should preserve a clear local datum and units when instantiated.

## Jacobs chuck and drill example

Represent the chuck as one holder component. Its tool-side interface describes the chuck's gripping bore/range and drill insertion depth; its machine-side interface describes its actual spindle connection. If it attaches through an arbor, represent the arbor as a separate component with its own two interfaces. Do not assume that every Jacobs chuck mounts directly to a spindle.

The chuck's outer envelope can initially be a measured axial profile. The drill is a ToolBit with an explicit stick-out from the chuck's tool-side seat. The resulting ordered stack is:

`drill cutting end → chuck tool seat → chuck body → chuck machine mount → (optional arbor) → spindle reference`

The example must use dimensions supplied by the user or a verified source; the proposal intentionally provides no default chuck dimensions.

### Candidate model inspected: `temp/albrecht-chuck.FCStd`

This is a native FreeCAD document (FCStd), rather than a STEP file at its current path. Read-only inspection found five valid solids: the chuck body, knurled collar, and three separate jaws. Their common axial direction is global Y. The document's unit system is Standard (mm, kg, s, degrees). The body spans approximately 49 mm in diameter and 90 mm along Y; the full model, including the jaw noses, spans about 99.2 mm axially (Y ≈ -9.23 to 90 mm).

This model is useful as a detailed holder-shape candidate and gives us a good orientation/placement test. To use it safely:

- Preserve the body, collar, and jaws as distinct linked solids inside one holder component or as a compound used for external collision checks. The source document itself remains untouched.
- Rotate the model so its Y axis aligns with the spindle/tool axis and define the spindle-side mating datum at the rear end (around Y=90 mm); confirm exact datum selection against the model before setting it as library metadata.
- Treat the three jaws as one fixed modeled opening. They do not express the chuck's full 1–13 mm gripping range or move with a selected drill. A conservative outer envelope or a configurable jaw envelope may be safer for collision checks.
- No separate arbor is included. The spindle-side interface therefore remains unknown until the chuck mounting style/dimensions are confirmed; represent an arbor as a separate component if required.
- Set drill insertion/stick-out explicitly from a chosen tool-side datum. The CAD model alone does not provide that job-specific value.

The geometry appears suitable for visualization and holder-to-obstacle checks once the axis and mount datum are assigned. It should not be treated as a complete chuck-and-spindle assembly or as proof of jaw position for a particular drill.

An untouched-source prototype copy is available at `temp/albrecht-chuck-holder-prototype.FCStd`. It groups the five solids in a native `App::Part` and maps source +Y to machine +Z, with the rear end at Z=0. It now contains visible candidate local coordinate systems for the tool-side and machine-side interfaces. The tool-side candidate is at the body-nose plane and the machine-side candidate at the rear plane; both are explicitly marked for verification. Reopening the saved document confirmed all five solids remain valid and the assembly envelope spans approximately Z=-99.23 to 0 mm. This remains a geometry/placement review file, not a configured production tool assembly.

## Initial validation cases

- A drill fits the declared chuck tool-side interface and has a valid insertion depth.
- A direct spindle mount and a chuck-plus-arbor stack both resolve to the same declared spindle reference.
- An incompatible interface, missing dimension, invalid profile, or missing spindle reference is reported as incomplete/unchecked, never as collision-free.
- A profile envelope is visible in the setup preview and is clearly identified as approximate.
- Moving the stick-out changes the assembled tool position and the holder/tool collision envelope by the expected amount.

## Open design questions

- Can FreeCAD's existing CAM asset/ToolBit library mechanisms represent holder definitions and complete tool assemblies, or is a separate asset type/store required?
- How should library IDs and revisions be resolved when projects move between machines or a library item changes? Should jobs pin a revision, track the latest version, or provide a user-controlled update?
- How can the simulator load and cache external geometry without leaving persistent document objects or adding Model-tree clutter?
- Should a first prototype store profile stations as a compact parameter list, or create and link a native FreeCAD Part shape?
- Which interface vocabulary is broad enough for common drill chucks, collets, tapers, and arbors without encoding a specific machine brand?
- Should tool insertion depth belong to the holder definition, the assembly instance, or both (nominal default plus per-instance override)?

Resolve these questions with a small prototype and representative job files before committing to a public data format or a comprehensive library.
