# CAM Simulator Feature Roadmap

Updated: 2026-10-21

## Purpose and scope

Develop the OpenGL CAM simulator into a practical machining preview and verification tool, with a longer-term goal of representing the complete machining experience.

This is the feature backlog for this local development effort. It is not an approved upstream FreeCAD roadmap or a delivery schedule. Checked items describe work implemented in this checkout, not released functionality. Unchecked items are planned or exploratory.

Related upstream document: [Simulation and Verification](../Roadmap/Functionality/Simulation%20and%20Verification.md).

## Implemented local foundation

- [x] Bottom information bar showing programmed feed, spindle RPM/direction, and independent flood/mist coolant states.
- [x] Constant-speed travel timing for supported linear, arc, and helical moves, with configured rapid limits and playback multipliers.
- [x] Estimated total time and simulated elapsed time for the selected operations.
- [x] Explicit indication when missing feed rates require assumed speeds.
- [x] Regression checks for parsing, timing, coolant state, playback state, and information-bar layout.

These estimates account for G4 and supported canned-cycle dwell durations, expand generated G73/G83 peck moves, and repeat absolute-position drilling cycles for `L`. Expansion is capped at 100,000 generated motions per cycle command to prevent pathological programs from overwhelming the preview; hitting the cap is reported as incomplete. G73/G83 accept explicit `D` clearance values, with a visible fallback estimate when absent. Parser/timing tests cover these paths; validation on representative posted output and jobs remains open. Acceleration/deceleration, tool-change delays, and spindle spin-up remain unmodeled. The simulator currently consumes internal CAM commands rather than verifying a posted NC file.

## Priority 1 — Next agreed features

### SIM-01 — Operation, tool, and command readouts

- [x] Show the current operation name and position within the selected operation sequence.
- [x] Show tool number, tool name, and useful identifying dimensions.
- [x] Show the current CAM command and its index within the operation.
- [x] Preserve source-command identity through expanded cycles and simulation segments.
- [x] Keep all readouts correct when playing, pausing, stepping, seeking, restarting, and changing the selected operations.

**Implemented:** Resizable, hideable CAM Commands sidebar with operation/tool headings, tool diameter, active-line highlighting, automatic following, manual browsing, click-to-seek, and source-command stepping. Feed values are displayed in mm/min. Zero-duration commands can be inspected individually; playback passes them immediately. This does not add support for previously unsupported commands or cycles.

**Validation:** Parser regression tests, isolated Qt panel interaction/layout checks, and production-engine checks for command seeking/stepping, modal coolant state, state restoration, and drilling-cycle mapping. User testing on simple projects confirms the feature is functioning correctly; future layout feedback may follow.

**Acceptance:** A user can identify which operation, tool, and source command produced the displayed movement. Internal CAM commands are clearly distinguished from posted G-code lines. Missing metadata is shown explicitly.

### SIM-02 — Pause-on-problem navigation

**Status:** Usable first pass; policy refinement is deferred while timing work proceeds. User testing on simple projects has not identified any issues requiring immediate changes.

- [x] Establish a structured issue list with severity, explanation, operation, tool, and source-command reference.
- [x] Add next/previous issue navigation and jump to the affected simulation position.
- [x] Add an option to pause when playback reaches a detected issue.
- [ ] Highlight the relevant toolpath segment or object where possible.
- [x] Begin with checks supported by existing data: missing/invalid movement rates, missing tools, and unsupported commands that affect simulation accuracy.
- [ ] Define which issues can continue with an estimate and which prevent meaningful simulation.

**Acceptance:** A detected issue can be located and explained without manually searching the toolpath. Rewinding and resuming give predictable pause behavior. This feature does not depend on holder/fixture collision detection being available.

**Implemented first pass:** The command sidebar lists findings with severity, operation, tool, command reference, and explanation. Selecting a finding seeks to its command; previous/next issue controls browse the list. The bottom bar keeps the incomplete/approximate-preview indication visible even when the sidebar is hidden. Automatic pauses reveal the sidebar and highlight the source command. Separate 3D highlighting remains planned.

**Continue policy:** Pause on issues defaults to on for a new run and can be switched off. Continue acknowledges all findings on the current command once. Continue without pausing disables subsequent pauses for the run without clearing findings. Rewinding rearms earlier issues when pausing is enabled. Playback state restoration preserves the preference and acknowledged position. Explicit single-step navigation remains available.

**Current coverage:** Missing/invalid cutting and rapid speeds (assumed 1 mm/s), missing tool profiles (movement/removal/time omitted), unsupported G/M codes and parameters, simplified cycles, and arcs lacking XY center offsets. Matching baseline modes G17/G21/G90/G94 are accepted. Findings describe the approximation or omission. Missing-tool findings are errors but can still be passed to inspect the remaining preview. This is not exhaustive validation, collision detection, or recovery from invalid geometry/OpenGL failures; a general fatal-issue policy remains outstanding.

**Validation:** 40 parser/timing/path tests pass. Production-engine checks cover accelerated playback, several findings at the same timestamp, acknowledgement, continue-without-pausing, rewind/forward seek, saved-state restoration, trailing commands, reset, and missing-tool runs with no renderable movement. Isolated Qt checks cover issue selection/navigation, preference synchronization, both Continue actions, and sidebar layout. Full application testing with representative jobs remains outstanding.

**Dependency:** SIM-01 source-command mapping. Collision findings can become another issue source later.

## Priority 2 — Everyday simulation and inspection

- [x] **SIM-03 — Linked command display.** Highlight the active command, select a command to seek, and retain operation context. Start with internal CAM commands; add posted-line mapping with SIM-12.
- [x] **SIM-04 — Playback navigation.** Step by source command, jump between operations, rewind, and optionally pause at M6 tool changes. Tool-change pauses seek to and highlight the source command; press Play to continue. The user confirmed both pause-enabled and pause-disabled behavior with a tool-change program.
- [ ] **SIM-05 — More complete cycle timing.** G4 and P dwell durations on G74/G82/G84/G86/G89 are included; G73/G83 pecks are expanded into plunges and retract motion. Absolute-position `L` repeats repeat cycles at the same location; incremental-position repeats remain unsupported and produce a finding. Expansion is capped at 100,000 generated motions per command and marked incomplete if reached. G73/G83 honor an explicit `D` clearance; without it, the simulator retains a visible 5%-of-Q fallback estimate. Tapping/boring synchronization and spindle stop/restart timing remain approximate or omitted. Parser tests cover the implemented cases; validate against representative posted cycles and controller semantics when available. Then consider configurable tool-change and spindle delays; add acceleration/deceleration when machine data is available.
- [x] **SIM-06 — Time breakdown (initial implementation).** The Commands sidebar reports modeled cutting, rapid, dwell, assumed-rate, and total time per operation and for the selection. Assumed-rate time is included in cutting/rapid and total. Unsupported or missing-tool motion marks the affected operation as incomplete. Tool-change, spindle-delay, and machine acceleration/deceleration time are not modeled. Validate the displayed totals against representative jobs before treating them as production estimates.
- [ ] **SIM-07 — Finished-part comparison.** Initial voxel comparison is implemented; manual UI validation, configurable tolerance, and representative-job validation remain. Compare simulated stock with the target model; show remaining material and gouges with an explicit resolution/coverage legend.
- [ ] **SIM-08 — Stock inspection.** Add section views, transparency, measurement, and export of intermediate/final stock.

**SIM-08 foundation and first visual path:** `VoxelStock` samples an OpenCascade solid into a bounded CPU occupancy grid, removes fixed-+Z axisymmetric cutter sweeps, reports approximate voxel volumes, and extracts exposed faces as a triangle mesh. It can consume a `MillPathSegment` using the simulator's tool profile, spindle state, and path interpolation. The Commands panel now has **Create final stock mesh preview…**; it reconstructs the final state on a worker thread and adds a separate, saveable `Mesh::Feature` to the CAM document, labeled with its voxel-cell size. The user confirmed that the preview object is created and visually matches the final part in a test job. The default grid limit is two million cells, extracted output is capped at two million triangles, and sweep work is bounded; budget failures are reported instead of silently truncated. Tests cover CAD-solid voxelization, volume changes, surface extraction, sweep-budget rejection, and a synthetic segment/tool-profile case. The model uses cell-center occupancy, has no rotary-axis support, and still needs validation on more representative jobs. Live playback still uses the existing GPU-depth renderer.

**SIM-08 intermediate stock snapshot:** The Commands panel also has **Create stock snapshot at current command…**. It reconstructs the initial stock after all cutting segments whose source command is at or before the current command, and adds a separately labeled, saveable mesh to the document. This first pass uses command boundaries (not partial interpolation within the active move); section views, transparency controls, and measurement remain future work.

**SIM-07 initial implementation:** **Compare final stock to target…** samples the target model onto the final-stock grid, then creates separate amber excess-stock and red missing-target-material Mesh features. It reports the two approximate voxel volumes and cell pitch, and warns if the target's bounding box extends outside the initial stock bounds. Tests cover both deviation directions, common-grid comparison, and out-of-bounds detection. This is a first pass: thresholds are voxel-cell based, classification uses cell centers, and manual UI/representative-job validation is pending.
- [x] **SIM-09 — Simulation report (initial implementation).** Export an HTML report with per-operation timing and tools, findings, a simulator screenshot, assumptions, document identity/modification state, and a SHA-256 fingerprint of the internal command stream and operation/rate metadata. Validate report contents and screenshot capture in FreeCAD.

## Feasibility gate — Holder and fixture collisions

### SIM-10 — Establish available geometry and placement

**Status:** Source audit complete; the native-Part holder definition, assembly preview, job assignment, and an experimental holder-to-stock check are implemented in this local checkout. Workholding assignment and the broader collision requirements remain open.

Initial inspection shows that the simulator receives stock, a base shape, and a revolved tool profile. It does not currently receive a separate holder/collet assembly or a list of physical workholding objects. The CAM Job's `Fixtures` property is a list of work coordinate systems such as `G54`; it is not evidence of vice or clamp geometry. A targeted source audit is recorded below; validating transforms against representative saved jobs remains part of the prototype gate.

#### Audit findings (2026-10-20)

- CAM ToolBits are reusable parametric tool definitions and create a document `BitBody`/`Shape`. Common types expose useful cutting-tool dimensions (for example, a drill defines diameter, overall length, and tip angle; some other types define shank diameter). This is a usable starting point for cutter geometry, but it is not a consistent assembly description. ToolBit/ToolController data inspected here does not define a holder or chuck, insertion depth/stick-out relative to one, or a common mounting datum for the completed assembly.
- The current OpenGL simulator obtains `tool.Shape`, extracts a revolved side profile, and adds that profile to the stock simulator. This represents the tool profile used for cutting/display; it does not preserve a multi-component tool assembly for holder collision checks.
- A Job links its model and stock, and workplanes have placements/attachments. The Job's `Fixtures` and each workplane's `Fixture` identify WCS selections (G54 and related offsets), not physical obstacles. There is no dedicated Job property for assigning vise, clamp, table, or other workholding geometry. Existing document shapes could be referenced by a future setup object without turning each WCS into a solid.
- The CAM machine configuration already describes linear/rotary axes, roles, limits, joint origins, and kinematics. In the inspected machine data model it does not provide CAD shapes for the spindle, head, table, or machine enclosure. It can inform later motion transforms, but does not currently provide machine collision geometry.

#### Minimal setup-data proposal

Keep this as a proposal pending a feasibility decision. A future collision-checkable job setup could contain:

1. **Tool assembly instance:** selected ToolBit plus an explicit stick-out/insertion depth, and zero or more holder components. Each component references either a reusable library definition, a document shape, or an explicitly declared simplified envelope. A holder definition has two distinct interface references: a tool-side seat (what tool/shaft attaches and how far it inserts) and a machine-side mount (what spindle, arbor, or upstream adapter it attaches to). The interfaces define their mating geometry/type and relative pose; the assembled top locates to a spindle gauge line/reference.
2. **Workholding assignments:** links to physical document objects (stock remains the existing Job stock; other assigned objects can be jaws, clamps, parallels, fixtures, or table). Each has an enabled/ignored state and a placement resolved into the setup frame.
3. **Coordinate contract:** identify the job/setup frame, stock/model frame, work offsets, spindle axis/gauge line, and transforms between them. WCS values select coordinate frames; physical geometry is assigned separately.
4. **Coverage declaration:** identify which component shapes and motion classes are present and checked. Missing assembly parts, obstacles, machine shapes, or unsupported motion result in a partial/unchecked report rather than a clear result.

For a Jacobs chuck and drill, the first useful fixture-free setup needs drill geometry, chuck geometry/envelope, drill insertion depth or stick-out at the chuck's tool-side seat, and a separate machine-side mounting interface (direct spindle mount or an arbor/adapter connecting it to the spindle reference). These two ends must not be inferred from the outer shape. Spindle CAD geometry can be omitted only if spindle/body collisions are explicitly outside the check scope. A reusable library would improve setup speed, but custom document geometry and simple envelopes are needed so the first prototype is not blocked on curating a comprehensive catalog.

**Audit conclusion (before implementation):** the required inputs could be assembled from existing ToolBit and document-shape concepts plus new setup/assignment data, but were not then available as one coherent CAM setup. The work proceeded as a small 3-axis prototype around assigned tool assemblies, without making a broad tool-library project a prerequisite.

**Design artifact:** [Tool Assembly Proposal](TOOL_ASSEMBLY_PROPOSAL.md) defines holder interfaces and collision geometry. It now records the user's external-library/no-tree-litter requirements, source-audit findings, and a recommended asset-backed model.

**Prototype progress:** The earlier `App::Part` holder and document-level assembly preview established the placement convention and were validated against a physical drill/chuck. The current importer instead writes versioned external CAM assets, and the asset editor revises datum and interface data without adding holder geometry to a project. The document-object commands remain available for older prototype documents. Curved-face axis inference, stable datum attachment after source edits, and explicit unit confirmation remain future work.

#### SIM-10A — Tool-holder importer and interface authoring

**Status:** Holder creation, external versioned asset storage, an asset metadata/datum editor, and ToolBit stick-out preview are implemented. The earlier document-object workflow was manually validated, including a physical drill/chuck comparison. The new library editor and revision reassignment path still need GUI validation.

- [x] Copy selected holder CAD geometry into a named component; preserve multiple selected objects as one compound. Shape coordinates use FreeCAD's internal millimetre units; explicit unit confirmation is still needed.
- [x] Guide users to define tool-side and machine-side interfaces by selecting planar faces. Placement can be refined through native datum editing; curved-face inference is future work.
- [x] Store each interface as a placement plus editable type, size/range text, and verification status in the external holder asset; provide a versioned asset editor.
- [x] Preview a selected CAM ToolBit at a specified stick-out and keep the preview placement synchronized when that value changes. A real drill/chuck comparison confirmed the placement convention. Arbor/spindle mating and interface compatibility checks are not implemented.
- [x] Prototype an editable document-based holder representation; the production authoring path now stores geometry, datums, and interface metadata in a versioned external asset instead.

The current importer writes holder assets outside the project document. **Edit CAM Holder Library Asset…** changes datum placement and interface metadata as a new revision; existing assemblies remain pinned to their previous holder revision. The earlier document-object authoring/editing commands remain only for legacy prototypes. Job-linked assemblies are consumed by the simulator, and an experimental stock-collision prototype uses their holder shapes. Full details are in [Tool Assembly Proposal](TOOL_ASSEMBLY_PROPOSAL.md).

The asset-backed workflow and current revision behavior are documented in [Tool Assembly Workflow](TOOL_ASSEMBLY_WORKFLOW.md). Routine job setup stores only an assembly URI and label on the ToolController. The current remaining UX work is validation in FreeCAD, a standalone assembly inventory/machine-set view, and portability/repair behavior. See [Tool Assembly Proposal](TOOL_ASSEMBLY_PROPOSAL.md) for the requirements.

#### SIM-10B — Job-linked tool assembly

**Status:** Visual integration validated by the user with two tool holders and a tool change; the simulator swapped setups correctly. ToolControllers have an optional `CAMToolAssembly` link to a document-level assembly preview. When assigned, the simulator checks that the preview uses the controller's ToolBit, includes its holder and stick-out in the operation/tool heading, and sends its tessellated holder geometry to a separate render-only path keyed by tool number. The holder follows the interpolated tool position and is excluded from stock removal. Mismatches are reported and ignored. Existing controllers gain the link when restored. The current transform assumes the holder's canonical +Z spindle axis and 3-axis toolpath coordinates; rotary orientation is not supported.

- [x] Store a link to the assembly preview on the ToolController and identify its holder/stick-out in simulation context; report mismatched ToolBits.
- [x] Provide a focused assignment workflow and enforce the selected assembly's ToolBit match before simulation; the CAM menu has a command to assign a selected preview to a matching ToolController.
- [x] Resolve the holder relative to the assembly's tool-side datum and translate it with the active simulated tool position for the canonical +Z, 3-axis case.
- [x] Display holder geometry separately from the cutter/removal profile, keyed by tool number; the user confirmed the chuck/drill placement follows correctly through simulation.
- [x] Validate a second tool/holder combination and tool changes; report missing or mismatched setup data clearly.

#### SIM-10C — Asset-backed assemblies and clean job references

**Status:** Initial asset-backed workflow and saved-holder revision editor are implemented in source. Holder geometry and interface datums are saved as versioned CAM assets; tool assemblies reference a pinned holder revision; numbered `.fctl` entries can store an optional assembly URI; adding a ToolBit from that library copies the URI to the ToolController; and the simulator resolves external geometry without adding holder solids to the job tree. GUI validation and cross-machine asset portability remain outstanding.

- [x] Audit existing CAM asset IDs, serializers, dependency resolution, storage paths, versioning support, ToolBit library behavior, and ToolController persistence.
- [x] Define the requirement that jobs reference reusable assemblies without storing holder CAD geometry or persistent assembly preview solids in the Model tree.
- [x] Identify the existing `.fctl` ToolBit library as the likely first inventory UI: it already maps tool numbers to ToolBits and its **Add to Job** action creates ToolControllers. The proposed extension is an optional version-pinned assembly URI per numbered entry.
- [x] Prototype versioned holder-definition and tool-assembly assets using the existing CAM AssetManager; include geometry as an external dependency.
- [x] Store only an assembly URI/revision on ToolControllers, validate its ToolBit ID against the controller's existing ToolBit snapshot, and render/cache holder geometry without document objects.
- [x] Extend numbered `.fctl` library entries with an optional assembly URI and propagate it when adding a ToolBit to a Job. Tool number remains the machine/pocket mapping; it is not part of the assembly identity.
- [x] Add a CAM asset editor for holder datum placements and interface metadata; save changes as new revisions and explain that existing assemblies remain pinned.
- [ ] Validate asset save/reopen and the complete library-to-job-to-simulator flow in the running FreeCAD build.
- [x] Add a live 3D mating preview to the tool-assembly assignment dialog. It uses a temporary document, updates with holder/stick-out choices, and closes with the dialog.
- [x] Validate the live assembly preview in the running FreeCAD build; the user confirmed the modeless preview flow and default Z-up axonometric view work.
- [ ] Build a standalone inventory/machine-set browser. As a first usability step, numbered ToolBit library rows now show their assigned assembly (or missing/unassigned status), support searching by assembly name, and offer repair wording for stale references.
- [ ] Specify asset-missing repair and portability behavior (shared asset folder or explicit pack/export) and test it across projects.

- [x] Audit tool definitions for shank, cutting length, holder, collet, gauge length, and stick-out information. ToolBits provide type-specific geometry/dimensions, but no holder assembly or common stick-out/mount datum.
- [x] Determine whether existing document objects can be explicitly assigned as vices, clamps, table surfaces, and other obstacles. Ordinary document shapes are available, but the Job has no dedicated physical-workholding assignment property.
- [ ] Verify placements and coordinate transforms between tools, stock, work offsets, and obstacles.
- [x] Define a minimal setup-data workflow for missing information, including optional simplified holder shapes. A user-facing setup editor and job integration remain out of scope for the prototype.
- [ ] Separate intended cutter/stock contact from non-cutting-tool or fixture collisions.
- [ ] Define collision tolerances and coverage, including checking motion between displayed frames.
- [x] Add focused native tests for one known voxel-model collision and one known clear move using representative box geometry.
- [x] Move collision analysis out of simulator startup; add an explicit action with background progress and cancellation.
- [x] Validate deliberate tool/holder collision reporting in FreeCAD with assigned assemblies across two tools; the user confirmed both tools' collisions and holder collisions were all detected.
- [ ] **Deferred pending a suitable test job:** confirm known-clear paths remain clear and review false positives. Simulator responsiveness, progress, cancellation, and deliberate collision reporting can be checked when a controlled job is convenient.

#### Candidate requirements if collision checking proceeds

Treat collision checking as a set of explicitly scoped checks, rather than a single pass/fail claim. Every run must identify which checks were evaluated and which remain unverified.

**Setup data**

- Model the tool assembly bottom-up as linked components: cutting tool, holder (for example, chuck/collet and any arbor or adapter), then spindle interface. Each component needs a mounting reference and a placement relative to the adjacent component; the assembled stack must locate against a defined spindle gauge line or equivalent machine reference.
- The cutting tool must provide usable geometry, including its non-cutting shank/neck where present, and a defined stick-out from its holder. Holders need usable geometry or dimensions and a defined insertion/mounting relationship. A simplified collision envelope is acceptable if its approximation is shown.
- Keep assembly instances separate from reusable component definitions. A library of common drills, chucks, collets, arbors, and spindle interfaces would make setup practical, but must not be a prerequisite: users need a way to define an instance from document geometry or simplified dimensions when no library entry exists.
- Spindle geometry is useful for checking tool-to-machine clearance, but can be a later scope layer. If it is absent, spindle and machine-body collision coverage must be explicitly reported as unchecked.
- The job must identify the current stock and target/workpiece, plus physical obstacles such as vise jaws, clamps, parallels, fixtures, and table. Each object needs a stable identity, valid shape, and placement in a documented setup coordinate system.
- Work offsets (G54 and similar) must remain coordinate transforms; they do not stand in for physical fixture geometry. The relationship among job coordinates, stock, workholding, tool, and machine coordinates must be explicit and testable.
- Missing, invalid, or ambiguous setup data must be reported as **unchecked**, not as a clear result. Users must be able to see what geometry is absent and either supply it or knowingly run a partial check.

**Collision semantics and motion coverage**

- Distinguish intended cutting contact between cutter and stock from unwanted contact. At minimum, classify cutter gouge/overcut, shank or holder against stock, tool assembly against workholding, and rapid/tool-change motion through stock or fixtures.
- Use the evolving stock state when deciding whether cutter contact is expected. Clearly state whether checks include only the cutter path or also the entire non-cutting assembly.
- Check the continuous swept motion along each supported segment, including arcs and cycle-generated moves; checking only rendered frames or endpoints is insufficient. Unsupported command types or rotary motion must reduce reported coverage to unchecked.
- Define a configurable geometric tolerance and contact policy, including tangency, near misses, tessellation/shape accuracy, and units. Report the tolerance used with every result.
- Each finding must identify the operation, tool, source command, approximate location, colliding objects/components, and severity, and must link into SIM-02 navigation. Findings remain advisory; playback can continue, and pause-on-problem can be disabled.

**Validation and acceptance for an initial implementation**

- A basic 3-axis check can be accepted only if it covers a configured tool assembly and explicitly assigned static workholding against stock and fixtures for all supported linear, rapid, arc, and expanded-cycle motion.
- Known clear, intentional cutting, gouge, holder-to-stock, tool-to-clamp, and rapid-through-stock cases must produce the expected classifications. Tests must include collisions between endpoints, transform/unit changes, near-tangency tolerance, and missing geometry.
- A clear result may be shown only for the checks and motion types actually evaluated. The summary must list unsupported or absent geometry, tolerance, and checks; it must never imply machine-wide safety when machine geometry or kinematics are not modeled.
- Checking must remain cancellable and responsive on representative jobs, with documented performance limits and no silent skipping of motion.

**Suggested scope boundary:** first consider static 3-axis setups with simplified or supplied tool-holder geometry and user-assigned static workholding. Treat rotary axes, moving machine parts, machine travel limits, tool changes, and machine self-collision as later work requiring machine kinematics and component geometry (SIM-13/SIM-14). This is a feasibility recommendation, not a commitment to implement either phase.

**Assembly direction:** define the stack from the cutting end upward (tool → holder → adapter/interface → spindle reference). The reusable library is a convenience and source of standard components; the collision checker consumes a fully placed assembly instance. A Jacobs chuck/drill setup is a representative early-use case. Its chuck and bit geometry plus stick-out are required for tool/workholding checks; spindle geometry is required only when spindle/body clearance is in the declared check scope.

**Decision:** Proceed only when required geometry, placement, and collision coverage are defined. Missing geometry must be reported as unverified coverage, never interpreted as a collision-free result.

### SIM-11 — Holder/fixture collision checking

- [x] Audit stock representations before choosing a collision algorithm.
- [x] Render the configured tool assembly; physical workholding geometry is still unassigned and unrendered.
- [x] Prototype holder-to-stock checks against evolving voxel stock for supported canonical +Z 3-axis paths; check cutter and holder during rapid moves and holder contact during feed moves.
- [ ] Extend detection to cutter gouges, exact non-cutting shank limits, assigned fixtures/workholding, and continuous-motion tolerance policy.
- [x] Feed prototype findings into SIM-02 issue navigation with source-command references and pause/continue behavior.
- [x] Show voxel resolution, checked scope, omitted categories, cancellation, and work-cap limitations in the coverage finding.
- [ ] Validate the prototype against representative FreeCAD jobs and record false positives, missed contacts, and runtime.

**Dependencies:** SIM-10 feasibility decision; SIM-02 issue navigation.

**Implementation gate:** The OpenGL simulator keeps the original stock mesh and represents cutting by replaying cutter sweeps in the render pipeline; it does not expose an updated stock solid for collision queries. The separate legacy `PathSimulator::cStock` updates a 2.5D height field initialized as a flat box from the stock bounding box, so it cannot represent arbitrary imported stock or overhangs accurately. Neither existing representation is a safe direct basis for holder-versus-remaining-stock findings. A bounded voxel model is now the experimental first approach; compare it with progressive OpenCascade swept-solid/B-rep stock updates on representative jobs. Keep the first scope to holder/shank versus evolving stock on canonical 3-axis paths, and measure false positives, missed between-frame contacts, memory, and runtime. Do not classify an unmodeled region as clear.

**Experimental prototype:** Collision analysis now starts only when the user selects **Check collisions**, after the simulator opens. It runs on a worker thread, displays progress, can be cancelled, and reports partial coverage if cancelled or if a work cap is reached. The bounded, coarse voxel analysis initializes occupancy from the actual stock B-rep, processes simulator path segments in source order, removes an axisymmetric approximation of the tool profile on feed moves, tests holder overlap against the remaining grid, and checks the cutter plus holder without removing stock on rapid moves. Findings enter the existing issue sidebar with source-command references and can use its pause/continue controls. Focused native tests pass for a known rapid collision, a known clear move, and cancellation reporting. To bound work, the prototype caps the stock grid at 20,000 cells, each sampled shape at 5,000 samples, and total motion/shape checks at 5,000,000. Stock and holder voxel dilation favors catching close contacts but can produce false positives, while sampling can miss contacts below its pitch. The cutter approximation treats the full EndMill profile as removable, so shank cutting limits are not represented. The prototype remains unvalidated against a representative FreeCAD job. It is not a clearance or safety guarantee. Fixtures, rotary motion, cutter gouges, and machine geometry remain out of scope.

## Longer-term machine simulation

- [ ] **SIM-12 — Posted NC verification.** Load the actual exported program with its tooling and setup, preserve line references, and compare execution with the internal CAM preview.
- [ ] **SIM-13 — Machine profiles.** Store axis travel, rapid and acceleration limits, spindle limits, timing allowances, and configurable auxiliary-output names/mappings.
- [ ] **SIM-14 — Full machine motion.** Model tables, spindle heads, rotary axes, and four-/five-axis kinematics. Check travel limits and, with complete geometry, machine-component collisions.
- [ ] **SIM-15 — Controller-specific behavior.** Add explicit controller profiles for offsets, compensation, canned cycles, subprograms, and eventually macros. Report unsupported behavior rather than silently accepting it.
- [ ] **SIM-16 — Cutting-load estimates.** Explore tool engagement, material removal rate, and demanding sections. Identify the tool/material data and validation needed before presenting force or power estimates.

## Recommended next step

**SIM-11 manual validation remains deferred** as requested until a suitable controlled job and geometry are available. The implemented **Check collisions** action remains available, and deliberate-collision reporting has already been confirmed across two tools. When testing resumes, check a known-clear path, review false positives, and confirm progress, cancellation, and source-command navigation. Record any missed contacts and runtime before changing the voxel policy or adding fixture checks. A reusable holder library can follow once instance geometry and placement conventions have survived these checks.

SIM-04 operation navigation and M6 pause behavior are implemented and user-tested. SIM-05 accounts for P dwell on supported cycles, absolute `L` repeats, and explicit G73/G83 `D` peck clearance; compare cycle paths and timing against representative posted output when available. The HTML report has been reviewed by the user and its initial implementation is complete. SIM-06 still needs a representative-job timing comparison. Tool-change duration, spindle delays, and machine acceleration/deceleration remain outside the time estimate. SIM-11 collision validation remains deferred.

## Delivery principles

- SIM-01 is complete and SIM-02 is a usable first pass; continue policy refinement independently of SIM-11, whose manual validation remains deferred.
- Keep estimates and unsupported behavior visible to the user.
- Preserve existing playback, seeking, coolant state, and timing behavior as features are added.
- Prefer incremental simulator builds and focused regression checks.
- Update this checklist as work is completed; record significant scope decisions alongside the affected feature.
