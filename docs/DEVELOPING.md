# Developing archicad-connector

## Architecture

```
Claude ──MCP(stdio)──> MCP server (TypeScript, src/)
                           │  HTTP POST JSON  http://127.0.0.1:19723 (first free port 19723..19744)
                           ▼
                     Archicad 26 JSON API
                     ├── official commands  "API.GetAllElements", ...        (src/tools/official.ts & others)
                     └── API.ExecuteAddOnCommand → namespace "ClaudeConnector"
                                                    │
                                                    ▼
                                    Claude Connector add-on (C++, addon/)
                                    Core/  (framework, helpers)   Commands/ (one file per family)
```

* `src/archicad/client.ts` — HTTP client: `ac.api("API.X", params)` for official commands, `ac.addon("Name", params)` for add-on commands (throws on `{error}`).
* `src/tools/*.ts` — MCP tools. One file per family, registered in `src/tools/index.ts`.
* `addon/Src/Core/*` — add-on framework. `addon/Src/Commands/*.cpp` — one file per family, registered in `Main.cpp` via `Commands/Commands.hpp`.

## Conventions (apply to every command and tool)

* **Units**: lengths in meters, **angles in degrees** in all JSON (convert with `GetAngle`/`OptAngle`/`AddAngle`; the API uses radians).
* **JSON field names**: camelCase, descriptive (`bottomOffset`, `topLinkedStory`, `buildingMaterial`). Same names in C++ and in the zod schemas.
* **Element references**: GUID string or `{"guid": "..."}` (`GetGuid`, `GetGuidArray`, `GuidFromItem`). Output GUIDs as plain strings (`GuidStr`).
* **Attribute references**: index, exact name, or `{index}|{name}|{guid}` (`GetAttr`/`OptAttr`), output `{index, name, guid}` (`AttrRef`). Names are **localized** (this machine runs Russian Archicad).
* **Stories**: `storyIndex` as index (int), name, `{index}`, `{name}`, `{floorId}` (stable) or `{displayNumber}` (Navigator number) — `OptStory` / zod `StoryRef`.
* **Library parts**: name / index / `{guid}` (`FindLibPart`), names are localized.
* **Polygons**: `{points:[{x,y}], arcs?:[{index, angle}], holes?:[{points, arcs?}]}` or a plain point array (`GetPolygon`, `WritePolygonToMemo`, `PolygonToJson`). Orientation is normalized automatically.
* **Enums**: strings (`"Center"`, `"Composite"`), mapped with `NamedValue` tables (`Core/Enums.hpp`), errors list the allowed values.
* **Batching**: create/modify commands take arrays and run in ONE undo step (`Undoable`), with per-item results via `Try` (`[{guid}|{error}]`).
* **Errors**: throw `cc::Error` via `Fail ("message", code)` or `Check (err, "what failed")`. Messages must be actionable (say what to do next, e.g. "use search_library_parts").
* **Undo scope**: EVERY database modification (create/change/delete elements, attributes, stories, properties, layouts...) must run inside `Undoable (...)` — otherwise the API returns `APIERR_NEEDSUNDOSCOPE`. Read-only commands must NOT use it.
* All commands run on the main thread (`ScheduleForExecutionOnMainThread`, handled by the framework).

## Core API cheat sheet (`addon/Src/Core`)

| Header | Main helpers |
|---|---|
| `Json.hpp` | `OS` (= GS::ObjectState), `Fail`, `Check`, `ErrorName`, `Get/Opt{Double,Int,Bool,String,Angle,Coord,Guid}`, `GetObject`, `TryGetObject`, `GetObjectArray`, `GetStringArray`, `GetNumberArray`, `GetGuidArray`, `CoordObj`, `Coord3DObj`, `BoxObj`, `Box3DObj`, `GuidStr`, `GuidObj`, `AddAngle`, `RadToDeg`, `DegToRad`, `kPi`, `ToUni`, `ToStr`, `EqualsIgnoreCase` |
| `Command.hpp` | `RegisterCommand (name, description, handler)`, `Undoable (undoName, fn)`, `Try (fn)` |
| `Types.hpp` | `ElemTypeName`, `ParseElemType`, `GetElemType`, `AllElemTypes`, `AttrTypeName`, `ParseAttrType`, `GetAttrType`, `AllAttrTypes`, `AttrName`, `FindAttrByName`, `AttrExists`, `OptAttr`, `GetAttr`, `AttrRef`, `OptStory`, `CurrentStoryIndex`, `StoryLevel`, `StoryName` |
| `Elements.hpp` | `Memo` (RAII memo), `GetElement`, `GetHeader`, `ElementExists`, `LoadMemo`, `NewElement`, `GetDefaults`, `ListElements`, `Get/SetElementInfoString`, `HeaderToJson`, `ApplyCommonFields`, `ApplyPostFields`, `ApplyStructure`, `AddStructureJson`, `ApplyOverriddenSurface`, `AddOverriddenSurfaceJson`, `RegisterAdapter`, `FindAdapter`, `ElementToJson`, `CreateElementFromSpec`, `ModifyElementFromPatch` |
| `Polygon.hpp` | `GetPolygon`, `GetPolyline`, `WritePolygonToMemo`, `WritePolylineToMemo`, `PolygonToJson`, `PolylineToJson`, `SignedArea`, `NormalizeOrientation` |
| `LibParts.hpp` | `FindLibPart`, `GetLibPartByIndex`, `LibPartToJson`, `LibTypeName`, `ParamsHandle`, `LoadDefaultParams`, `ParamsToJson`, `ApplyParamValues`, `ChangeParamsWithScript`, `ChangeParamsWithScriptForLibPart`, `ParamTypeName` |
| `Enums.hpp` | `NamedValue`, `ParseNamed`, `OptNamed`, `NameOf` |

## Patterns

### Element types → adapters
Reference: `addon/Src/Commands/Walls.cpp` + `src/tools/walls.ts` (and the minimal slab in `SlabsRoofs.cpp`).

```cpp
RegisterAdapter ({ API_WallID, CreateWall, SerializeWall, ModifyWall });
```
* `create (spec) -> API_Guid`: `NewElement` → `GetDefaults` → `ApplyCommonFields (element, nullptr, spec)` → type fields → memo (polygons, params) → `ACAPI_Element_Create`. Runs inside the undo scope of `CreateElements`.
* `serialize (element, out)`: add type-specific fields only (common header is added by the core). Load memo yourself when needed (`Memo memo; ACAPI_Element_GetMemo (...)`).
* `modify (element, mask, memo, memoMask, patch)`: change struct fields and `ACAPI_ELEMENT_MASK_SET (mask, API_XxxType, field)`; for memo changes fill `memo` and OR the `APIMemoMask_*` bits into `memoMask`. The core calls `ACAPI_Element_Change`.
* Use ONE apply-function for create and modify (`mask == nullptr` on create) so both accept the same fields.
* The generic MCP tools `create_elements` / `get_element_details` / `modify_elements` then work for the type automatically; add a typed `create_<type>s` tool with a fully described zod schema (Claude reads the descriptions).

### Other commands
```cpp
RegisterCommand ("GetStories", "Description Claude-facing...", [] (const OS& params) -> OS { ... return OS ("stories", list); });
```
and in TS:
```ts
defineTool(server, ctx, { name: "get_stories", title, description, input: {...zod}, annotations: READ_ONLY, handler: async (args, { ac }) => ac.addon("GetStories", args) });
```

## Gotchas learned so far

* **Top-linked walls/columns**: tool defaults are often linked to the story above; an explicit `height` must set `relativeTopStory = 0` unless the caller also passes `topLinkedStory`.
* Always start from `ACAPI_Element_GetDefaults` (current tool settings) and zero-init structs (`NewElement`, `BNZeroMemory`).
* Dispose memos (`Memo` RAII) and handles (`BMKillHandle`), delete `API_LibPart::location` and `API_MaterialType.texture.fileLoc` returned by getters.
* Slabs need `memo.edgeTrims` + `memo.sideMaterials` sized `nCoords + 1` (see `SlabsRoofs.cpp`).
* Never let a C++ exception escape into Archicad callbacks — `Undoable`/`Try`/the command wrapper already catch `cc::Error`.
* Russian Archicad: attribute/library names are Russian, e.g. layer "Конструктив - Стены Несущие". Never hardcode names; discover them.
* clangd "file not found" diagnostics are noise when compile_commands.json is missing — the real check is the compiler.
* Archicad caches nothing about the add-on across rebuilds, but it must be **restarted** to load a new build.
* MDID: licensed Archicad only loads add-ons with a Graphisoft-issued MDID (`addon/mdid.local.cmake`, see CMakeLists.txt).
* **Crashes**: Archicad writes a backtrace to `~/Library/Application Support/Graphisoft/BugReporting-26/*-FATAL.rpt` (XML, `<atos>` frames name our functions). After a crash the next start takes 3-5 min (autosave recovery) and reopens the crashed session instead of the template.
* Never `delete` pointers that Archicad fills into a struct unless the DevKit examples do so (e.g. `API_RefLevelsPrefs.level1UStr` points to Archicad-owned strings: deleting it aborts Archicad).
* The 3D projection settings (`APIEnv_Get/Change3DProjectionSetsID`) store angles in **degrees** (perspective azimuth/viewCone/rollAngle, axonometric azimuth, sun angles); element data uses radians.
* `ACAPI_Favorite_Get` crashes AC26 for favorites of external tools (structural analytical loads/supports) and fails for Opening/Drawing favorites: check the listed type first (`Collaboration.cpp`).
* `ACAPI_Element_GetQuantities` needs a non-null `API_QuantityPar` for walls (nullptr -> APIERR_BADPARS).
* `ACAPI_Element_GetElemList (API_ObjectID)` also returns the hidden GDL part objects of curtain walls / railings / stairs (`object.ownerType` set) and the flat marker heads of elevations/sections.
* `Undoable` returns the error code to Archicad when `fn` throws, so the partial changes of a failed command are rolled back. Duplicate command names are skipped at registration (logged in ClaudeConnector.log).
* zod input objects are not strict: unknown keys are silently dropped, so check field names against `mcp-call.ts --describe <tool>`.

## DevKit reference material

* Headers: `.devkit/devkit26/Support/Inc/*.h` (`APIdefs_Elements.h` = all element structs, `APIdefs_Attributes.h`, `APIdefs_Environment.h`, `APIdefs_Database.h`, `APIdefs_Automate.h`, `APIdefs_Goodies.h`, `APIdefs_Interface.h`, `APIdefs_LibraryParts.h`, `APIdefs_Properties.h`, `APIdefs_Navigator.h`, `ACAPinc.h` = function list).
* Examples (working code for almost every API): `.devkit/devkit26/Examples/*/Src/*.cpp`, especially `Element_Test` (creation/modification of every element type, curtain walls, dimensions, labels, hotlinks, trims, solid ops), `Attribute_Test`, `Database_Control` (layouts, drawings, databases), `Automate_Functions` (save/open/print/publish/zoom/render), `Environment_Control` (stories, preferences, 3D, view settings), `Goodie_Functions` (params, polygons, conversions), `LibPart_Test` (library parts, creating GDL objects), `Property_Test`, `Classification_Test`, `Navigator_Test`, `Teamwork_Control`, `MarkUp_Manager`, `Favorite_Test`, `IFC_Test`, `Interface_Functions`, `Selection_Manager`, `3D_Test`, `Model3DViewer_Test`.
* Official JSON API command list for AC26: 72 commands (see `src/tools/official.ts`).

## Build & test

```bash
# compile ONE family file only (fast, safe while others edit other files; use your own build dir):
cmake -S addon -B addon/build-<family> -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
ninja -C addon/build-<family> CMakeFiles/AddOn.dir/Src/Commands/<File>.cpp.o

# full build / install / restart Archicad (ONLY when you own the Archicad instance):
bash scripts/dev-cycle.sh

# TypeScript
npx tsc --noEmit
npx vitest run

# call MCP tools exactly like Claude does (against the running Archicad):
npx tsx scripts/mcp-call.ts --list
npx tsx scripts/mcp-call.ts create_walls '{"walls":[{"begin":{"x":0,"y":0},"end":{"x":5,"y":0},"height":3}]}'
# raw add-on command:
bash -c 'source scripts/lib.sh; ac_addon GetElementDetails "{\"elements\":[\"<guid>\"]}"'
```

Node 20+ must be on PATH (`NODE_BIN=<dir>` makes `test/live/run-all.sh` prepend another Node, e.g. an nvm install).

## Verified in live testing (sequential, full regression)

* **Wall side** depends on referenceLine AND flipped: Outside+unflipped = body RIGHT of begin→end, flipped mirrors it. The Wall
  tool default for flipped varies (template/favorites), so CreateWall forces flipped=false unless given. Slab `level` = top surface.
* **Current database**: model elements can only be created/changed while the floor plan database is current —
  `CreateElementFromSpec` / `ModifyElementFromPatch` wrap adapters in `ModelDatabaseScope` (Core/Elements.hpp).
  Commands outside the generic path that create model elements must do the same.
* **`APIFilt_IsEditable` is false for locked elements too** (not only locked/hidden layers) — check `head.lockId` first.
* **Zones**: Archicad refuses a second automatic zone in a room that already has one → probe/recreate code parks the
  original on another story (`ParkedZone`); `refPos` of an existing zone cannot be changed → `RelocateZones` re-creates.
* **Curtain wall grids**: a re-sized pattern needs at least one flexible module (APIERR_BADPARS otherwise).
* **Stories**: `APIStory_Delete` switches the current story — restore it afterwards.
* **3D pictures**: the 3D model is generated asynchronously in Archicad's idle loop; `capture_view` polls
  `Get3DModelStats` until stable and fits axonometric views before saving (otherwise empty/partial images).
* **CineRender** may crash on this macOS (Cineware reports) → `render_view` returns a blank image with a warning.
* **Strict inputs**: `defineTool` makes every zod object strict (`makeStrict`) unless it is `.passthrough()`.
  Never reuse an input schema to parse Archicad responses.
* **Process detection**: after a crash Archicad runs as `Archicad -RECOVER ...`; match `MacOS/Archicad( |$)`.
* **Automatic zone boundaries come from the CURRENT story's walls**, not the zone's story (a story-1 zone created while
  story 0 was current took story 0's outline). `CreateZone` and `CreateAutomaticClone` wrap the create in
  `CurrentStoryScope` (Core/Types.hpp), which switches stories and restores the previous one.
* **Element ID**: `APIDb_ChangeElementInfoStringID` returns `APIERR_BADELEMENTTYPE` for lines, polylines, arcs, circles,
  splines, texts, labels, hotspots, pictures, linear/level dimensions (hatches work). `HasElementId` rejects these up front;
  `CreateElementFromSpec` deletes the new element when a post-create step fails, so an item error never leaves an orphan.
* **Edit menu titles** (undo/redo) refresh only when the main event loop runs → `PerformRun` does one step per main-queue
  turn (`dispatch_after_f`), otherwise every step reports the first title. 50 ms between steps was not always enough (the
  3rd of 3 undos read the 2nd title). The next step now waits, up to ~1 s, while the title still equals the previous one,
  and the TS undo/redo deadline allows for that wait (`undoTitleWaitMs`).
* **Restart + reopen** (`scripts/archicad.sh`) can open a .pln read-only (stale lock): `save_project_as` same path + overwrite.
  `QuitArchicad` discards unsaved changes, so `archicad.sh stop` saves the open project first (Save As onto the same path
  when it is read-only) and refuses to quit when that fails (`ARCHICAD_DISCARD=1` overrides).
* **Drawing placement**:
  * `drawing.anchorPoint` is ignored on placement. `ShiftDrawingToAnchor` moves `pos` AND the frame polygon (`poly`),
    which exists on unclipped drawings too.
  * `bounds` and the separate title object follow the frame, not the content; moving `pos` alone left the title behind.
  * Writing `poly` switches `isCutWithFrame` on, even when that field is masked to false in the same change. A second
    change restores it (`KeepUnclipped`): place / modify_drawings call it directly, the generic adapter through
    `ElementAdapter::afterModify`.
* **Saved view settings vs drawings**:
  * `APINavigator_ChangeNavigatorView` stores the new scale and zoom, but drawings placed afterwards still used the old
    ones (1:500 instead of 1:200; crop frame and content apart) until the view had been opened once.
  * `SetViewSettings {view}` therefore runs `APIDo_GoToView` and then brings back the window and the display state that
    GoToView overwrites (`DisplayStateKeeper`): layer states and combination, scale, structure display, renovation
    filter, model view options.
  * That state belongs to each window type: a layout in front kept its own layers while the floor plan behind it took
    the view's. So the command opens the view's window without its settings (`OpenNavItem`), takes the snapshot there,
    refreshes, restores it there, then goes back to the previous story and window. Without that, later live suites
    failed on hidden wall layers.
  * The pen set cannot be switched back (AC26 has no setter); the result reports it in `displayNote`.
  * `update_drawings` reported such drawings "UpToDate" without rebuilding them: re-place them.
* **Layout Book subsets**:
  * There is no API to modify a subset (only `APIDb_CreateSubSetID` / `GetSubSetID`).
  * `API.RenameNavigatorItem` with a new ID on an auto-numbered subset reads back fine, then reverts on the next Layout
    Book change. Create custom-numbered subsets instead.
* **Text memos**: `APIMemoMask_TextContent` / `Paragraph` take UTF-8, the `*Uni` masks UTF-16. Mixing them garbled
  Cyrillic and returned APIERR_PAROVERFLOW on modify.
* **Text labels**: `API_LabelType.nonBreaking` (true means do not wrap) is separate from `u.text.nonBreaking`. New labels
  set both (plus `width = 0`), otherwise Archicad wrapped them one character per line.
* **MEP Modeler parts** ("Прямолинейная Труба 26" etc.) cannot be created via the AC26 API: placing them as objects
  returns APIERR_BADPARS even with no parameters given. Pipes are modelled as circular beams instead (CLAUDE_GUIDE §4.9).

## Live regression

```bash
npm run test:live          # = bash test/live/run-all.sh: rebuild + reinstall + fresh project, then every suite
bash test/live/run-all.sh --no-build
npx tsx test/live/04-openings-objects-zones.live.ts   # one suite against the running Archicad
```
Suites use disjoint coordinate regions; `99-project.live.ts` (new/open/close) runs last.
