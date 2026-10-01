# Archicad Connector — guide for Claude

You are driving a **live Graphisoft Archicad 26** instance. Every tool call changes or reads the real project the user sees.
This guide tells you how the connector thinks, the conventions that make results correct on the first try, the working loop,
worked recipes, and the known limits of the Archicad 26 API. `get_connector_guide` returns this document.

---

## 1. Mental model

* **Two layers.** Archicad's built-in JSON API (always available: element lists, properties, classifications, navigator,
  layouts, attribute folders) + the **Claude Connector add-on** (create/modify/delete everything, views, images, exports,
  GDL, Teamwork, issues ...). `archicad_status` tells you whether the add-on is loaded — without it only the official
  tools work.
* **The project** = stories (floor plans) holding **model elements** (walls, slabs, columns, beams, roofs, shells, meshes,
  morphs, zones, doors/windows/skylights/openings, objects/lamps, curtain walls, stairs, railings) + **2D elements** that
  belong to a view (lines, arcs, polylines, splines, hatches, texts, labels, dimensions, hotspots, pictures) + **attributes**
  (layers, composites, building materials, surfaces, fills, line types, pens, zone categories, profiles, layer
  combinations ...) + **library parts** (GDL objects: doors, windows, furniture, stamps) + **views & layouts**.
* **Everything has a GUID.** Tools return GUIDs; pass them back to read, modify, host openings, dimension, classify ...
* **One call = one undo step** named "... (Claude)". Batches are atomic per item: `[{guid} | {error}]` in input order;
  one bad item never aborts the others. `undo` / `redo` step through Archicad's undo history (they also undo the
  user's own edits — use them only for your own recent steps).

## 2. Conventions (verified live — follow them exactly)

| Topic | Rule |
|---|---|
| Units | **meters** for lengths, **degrees** for angles, m² / m³ for quantities. Angles are counter-clockwise from +X. |
| Coordinates | Project coordinates: X right, Y up on the floor plan, Z up. 2D points `{x, y}`, 3D `{x, y, z}`. |
| Elevations | **Relative to the element's home story level** (`bottomOffset`, `level`, `elevation`, `sillHeight` ...). Absolute story levels come from `get_stories`. |
| Stories | `storyIndex`: 0 = ground floor, negative = basements. Default = the **current** story (check `get_project_info.currentStory`; switch with `set_current_story`). Also accepted: story name, `{floorId}` (stable), `{displayNumber}`. |
| Names | Attribute and library-part names are **localized** (this installation is Russian: e.g. layer "Конструктив - Стены Несущие", window "Арочное Окно 26"). **Never guess** — look them up with `get_attributes` / `search_library_parts` and copy the exact string. Indices also work. |
| Polygons | `[{x,y}, ...]` or `{points, arcs?: [{index, angle}], holes?: [{points, arcs?}]}`. Do **not** repeat the first point. Orientation is normalized automatically. `arcs[].index` = the edge from `points[index]` to `points[index+1]`; `angle` in degrees. |
| Walls | `begin`→`end` is the reference line. With `referenceLine: "Outside"` the reference line is the wall's **outer face** and the body lies to the **RIGHT** of begin→end; `"Inside"` = body to the LEFT; `"Center"` straddles it (`flipped: true` mirrors this; new walls default to `flipped: false` whatever the tool default). → Draw exterior walls **clockwise** with `"Outside"` so the walls stay inside the outline. A given `height` unlinks the wall top from the story above (tool defaults are often top-linked) unless you also pass `topLinkedStory`. The wall spans `bottomOffset .. bottomOffset + height`. Curved walls: `arcAngle` in degrees. Chain walls end-to-start so Archicad joins the corners. |
| Slabs | `level` = elevation of the **top** surface relative to the story; the slab hangs down by `thickness` (level 0, thickness 0.25 → z −0.25..0). |
| Openings | Windows/doors need a host `wall` GUID and `position` = distance along the wall reference line from the wall **begin** point to the opening **centre** (`positionReference: "Begin"|"End"` for edge-based). `sillHeight` above the wall bottom. Skylights need a roof/shell `owner` + `point`. Openings move with their host. |
| Zones | Automatic zone = `referencePoint` inside a room fully enclosed by walls/columns/room separators. Manual zone = `polygon`. Changing the reference point re-creates the zone (new GUID, settings kept). `update_zones` re-detects boundaries after walls moved. |
| Current window | Model elements are always created/changed on the floor plan database (the connector switches automatically). **2D elements are drawn into the current window** (plan, section, detail, worksheet, layout) — open the right view first (`open_view`). Element editing tools (move/rotate/trim ...) act in the current database. |
| Inputs | Tool inputs are **strict**: a misspelled field is an error that names it. Read the error and fix the key. |
| Output | Numbers are rounded to 1e-6. Big lists are paginated (`offset`/`limit`, `hasMore`). |

## 3. The working loop

1. **Orient** — `archicad_status`, `get_project_info` (current story/window, file), `get_stories`.
2. **Discover** what exists and what names are available:
   `find_elements` / `get_element_counts`, `get_attributes {type: "Composite" | "BuildingMaterial" | "Layer" | "Surface" | ...}`,
   `search_library_parts {type: "Door" | "Window" | "Object" | "Lamp", query}`, `get_tool_defaults {type}`.
3. **Plan** the geometry in meters (sketch coordinates, story levels, wall reference lines, heights).
4. **Build in batches**, structural first: stories → walls → slabs → columns/beams → roofs/shells → openings (need wall
   GUIDs) → stairs/railings → objects → zones → annotation.
5. **Verify every step**: read back with `get_element_details` (or `get_element_quantities`, `get_bounding_boxes`), and
   **look at it** with `capture_view` (3D axonometric for massing, floor plan for layout, section for heights).
   Fix with `modify_elements` / the `modify_*` tools rather than deleting and redrawing.
6. **Document**: sections/elevations (`create_sections`, `create_elevations`), dimensions (`dimension_walls`,
   `create_dimensions`), labels, zones with areas, layouts (`create_layout` + `place_drawing`), exports
   (`export_pdf`, `export_ifc`, `export_dwg` as DXF), `publish_publisher_set`.
7. **Enrich data**: properties (`create_property_definitions`, `set_property_values`), classifications
   (`set_element_classifications`), IFC (`set_ifc_properties`), quantities.

## 4. Recipes

### 4.1 A small house (ground floor + roof)
```jsonc
// 1. discover names
get_attributes      {"type": "Composite", "nameFilter": "Кирпич", "limit": 20}
search_library_parts {"type": "Door", "limit": 10}
search_library_parts {"type": "Window", "limit": 10}
// 2. outer walls, CLOCKWISE with "Outside" -> bodies inside the 10 x 8 m outline
create_walls {"walls": [
  {"begin": {"x": 0, "y": 0},  "end": {"x": 0, "y": 8},  "height": 3, "thickness": 0.3, "referenceLine": "Outside"},
  {"begin": {"x": 0, "y": 8},  "end": {"x": 10, "y": 8}, "height": 3, "thickness": 0.3, "referenceLine": "Outside"},
  {"begin": {"x": 10, "y": 8}, "end": {"x": 10, "y": 0}, "height": 3, "thickness": 0.3, "referenceLine": "Outside"},
  {"begin": {"x": 10, "y": 0}, "end": {"x": 0, "y": 0},  "height": 3, "thickness": 0.3, "referenceLine": "Outside"},
  {"begin": {"x": 6, "y": 0.3}, "end": {"x": 6, "y": 7.7}, "height": 3, "thickness": 0.12, "referenceLine": "Center"}]}
// 3. floor slab (top at 0) and a roof on the wall tops
create_slabs {"slabs": [{"polygon": [{"x":0,"y":0},{"x":10,"y":0},{"x":10,"y":8},{"x":0,"y":8}], "thickness": 0.25, "level": 0}]}
create_roofs {"roofs": [{"pivotPolygon": [{"x":0,"y":0},{"x":10,"y":0},{"x":10,"y":8},{"x":0,"y":8}], "level": 3, "slopeAngle": 30, "eavesOverhang": 0.5, "thickness": 0.25}]}
// 4. openings into the returned wall GUIDs
// (position = distance from the wall's BEGIN point; the south wall runs from x=10 to x=0)
create_doors   {"doors":   [{"wall": "<south wall>", "position": 8,   "width": 0.9, "height": 2.1}]}
create_windows {"windows": [{"wall": "<south wall>", "position": 2.5, "width": 1.5, "height": 1.4, "sillHeight": 0.9},
                            {"wall": "<north wall>", "position": 5,   "width": 1.2, "height": 1.4, "sillHeight": 0.9}]}
// 5. rooms (automatic zones find the walls) and a look
create_zones  {"zones": [{"referencePoint": {"x": 3, "y": 4}, "name": "Living", "number": "01"},
                         {"referencePoint": {"x": 8, "y": 4}, "name": "Bedroom", "number": "02"}]}
capture_view  {"threeD": {"mode": "axonometric", "azimuth": 225, "altitude": 35}, "format": "jpeg"}
capture_view  {"view": {"window": "FloorPlan", "story": 0}, "zoom": {"mode": "fit"}}
```

### 4.2 Multi-storey
`create_stories {"stories": [{"name": "Level 2", "height": 3, "position": "Top"}]}` → build on it with `"storyIndex": 1`,
or copy a whole floor: `copy_elements_to_stories {"elements": [...], "stories": [1, 2]}`. Stairs:
`create_stairs {"stairs": [{"begin": {"x":1,"y":1}, "end": {"x":1,"y":5.5}, "height": 3, "width": 1.1, "riserCount": 17}]}`,
slab openings with `create_openings {"openings": [{"owner": "<slab>", "point": {...}, "width": 1.2, "height": 4.6}]}`.

### 4.3 Inspect and change an existing project
```jsonc
find_elements        {"types": ["Wall"], "storyIndex": 0, "limit": 200}
get_element_details  {"elements": ["<guid>", ...]}            // geometry, structure, library part, relations
get_element_quantities {"elements": [...]}                     // areas, volumes, lengths
modify_elements      {"elements": [{"guid": "<g>", "height": 3.2, "composite": "<exact name>"}]}
move_elements        {"elements": [...], "vector": {"x": 0.5, "y": 0}}
set_selection        {"mode": "set", "elements": [...]}        // show the user what you mean
```

### 4.4 Look before and after (your eyes)
* 3D overview: `capture_view {"threeD": {"mode": "axonometric", "azimuth": 225, "altitude": 30}}`
* Perspective: `capture_view {"threeD": {"mode": "perspective", "camera": {"x":-8,"y":-10,"z":6}, "target": {"x":5,"y":4,"z":1.5}}}`
* Plan of a story: `capture_view {"view": {"window": "FloorPlan", "story": 0}, "zoom": {"mode": "fit"}}`
* Only some elements in 3D: `show_in_3d {"mode": "elements", "elements": [...]}` then `capture_view`.
* Section: `create_sections` → `open_view {"window": "Section", "element": "<marker guid>"}` → `capture_view {"zoom": {"mode": "fit"}}`.

### 4.5 Documentation
```jsonc
create_layout   {"name": "A-101 Plans"}
list_views      {"include": ["viewMap"]}                                   // saved views with guids
place_drawing   {"drawings": [{"layout": "A-101 Plans", "view": "<view guid>", "position": {"x": 0.05, "y": 0.05}, "scale": 100}]}
update_drawings {"all": true}
export_pdf      {"path": "/Users/me/Desktop/A-101.pdf", "layout": "A-101 Plans"}
export_ifc      {"path": "/Users/me/Desktop/model.ifc"}
export_dwg      {"path": "/Users/me/Desktop/plan.dxf", "format": "dxf", "storyIndex": 0}
publish_publisher_set {"name": "<set name from get_publisher_sets>", "outputPath": "/Users/me/Desktop/out", "createFolders": true}
```
Dimensions: `dimension_walls {"walls": [...], "mode": "Chain", "includeOpenings": true}`; texts: `create_texts`; labels on
elements: `create_labels {"labels": [{"parent": "<guid>", "text": "..."}]}`.

**A whole plan book** (verified live: 17 sheets АР + ЭО + ВК published as one PDF):
1. One saved view per sheet: `set_view_settings {view, layerCombination, drawingScale, zoom}`. The stored `zoom` becomes the
   crop frame of floor-plan drawings, so frame the building plus its dimensions, and use the same zoom for every story so
   the sheets line up. Each discipline gets its own layer combination (`create_layer_combinations`).
2. Sheets: `create_layout {name, master: "<A3 master>", parent: "<subset>"}` (a small house fits A3 at 1:100; the
   template's A2 masters leave the drawing tiny). One subset per discipline, created with
   `create_layout_subset {customNumbering: true, customNumber: "ЭО", ownPrefix: "ЭО-", useUpperPrefix: false, numberingStyle: "01"}`.
3. `place_drawing {layout, view, position}`: by default the drawing's centre sits on `position`. `anchor` picks another
   point of its frame. Schedules and indexes (`itemType` Schedule / TableOfContents) go onto the general-data sheet.
4. Title blocks read the Project Info autotexts: `set_project_info_fields` (PROJECTNAME, PROJECTNUMBER, PROJECTSTATUS,
   CONTACTCOMPANY, SITEADDRESS1, SITECITY…). SITEFULLADDRESS is computed from the address parts.
5. Check every sheet with `capture_view {"view": {"window": "Layout", "name": "..."}, "zoom": {"mode": "fit"}}`, then
   `publish_publisher_set` (the template's layout set writes one multi-page PDF of the whole Layout Book).

### 4.9 Engineering plans (electrical, plumbing)
Archicad 26's API cannot create MEP Modeler parts (pipes, ducts, cable trays fail with APIERR_BADPARS), so model the
systems with ordinary elements on the template's MEP layers (`MEP - Электрооборудование.ЭО`, `MEP - Водоснабжение и Канализация.ВК`):
* **Pipes**: circular beams (`diameter` makes the section circular), e.g. `create_beams {"beams": [{"begin": …, "end": …,
  "diameter": 0.11, "level": -0.45, "anchor": "Center", "slantAngle": -1.15, "buildingMaterial": "Пластик",
  "lines": {"uncut": {"pen": 75}, "cutContour": {"pen": 75}}, "layer": "MEP - Водоснабжение и Канализация.ВК",
  "elementId": "К1 выпуск"}]}`. With `anchor: "Center"`, `level` is the pipe axis height, and `slantAngle` gives the sewer
  fall. **Risers** are circular columns with `zoneRelation: "None"`. Give each system its own pen (template pens: cold
  water В1 35, hot water Т3 55, sewer К1 75).
* **Fixtures**: library objects (WC, basin, boiler) placed with `create_objects`.
* **Wiring**: 2D only. Use symbols (`create_objects` with 2D library parts, or `create_circles` / `create_texts`) and
  `create_polylines` runs (one pen per circuit, dashed line type for socket circuits) on the ЭО layer. Add a legend
  with `create_texts` and labels with `create_labels`.
* Per discipline: a layer combination that hides the other MEP layers, furniture and zone fills, plus a saved view per
  story. For a 3D plumbing schematic, a perspective view whose combination shows only ВК, sanitary objects and the slabs
  (as wireframe).

### 4.6 BIM data
```jsonc
create_property_groups      {"groups": [{"name": "Project data"}]}
create_property_definitions {"definitions": [{"group": "Project data", "name": "Fire rating", "type": "singleEnum", "enumValues": ["EI30","EI60","EI90"]}]}
set_property_values         {"values": [{"elements": [...], "property": "Project data/Fire rating", "value": "EI60"}]}
get_property_values         {"elements": [...], "properties": ["Project data/Fire rating"]}
get_classification_systems  {} ; get_classification_tree {"system": "<name>", "format": "flat"}
set_element_classifications {"assignments": [{"elements": [...], "system": "<name>", "item": "<item id>"}]}
set_ifc_properties          {"properties": [{"elements": [...], "propertySet": "Pset_WallCommon", "name": "IsExternal", "value": true}]}
```

### 4.7 Custom parametric objects (GDL)
`create_library_part {"name": "Planter Box", "type": "Object", "parameters": [{"name": "h", "type": "Length", "value": 0.6}],
"scripts": {"script3D": "BLOCK A, B, h", "script2D": "PROJECT2 3, 270, 2"}}` → place it with
`create_objects {"objects": [{"libraryPart": "Planter Box", "position": {...}, "sizeA": 1.2, "sizeB": 0.4}]}`; change
parameters later with `set_gdl_parameters`. Read existing parts' scripts with `get_library_part_scripts`.

### 4.8 Defaults, favorites, issues
* Set up a tool once, then draw many: `set_tool_defaults {"defaults": [{"type": "Wall", "fields": {"thickness": 0.2, "height": 2.8}}]}`.
* `get_favorites`, `apply_favorite {"name": "...", "target": "Defaults" | "Elements", "elements": [...]}`, `create_favorite`.
* Review loop: `create_issue {"issues": [{"name": "Check stair headroom", "comment": "...", "attach": {"highlight": [...]}}]}`,
  `export_bcf`, `import_bcf`.

## 5. Gotchas and limits of Archicad 26

* **Names are localized** — copy them from `get_attributes` / `search_library_parts`; partial names produce "Did you mean ..." hints.
* **Locked elements/layers**: edits fail with an actionable message; `unlock_elements` or `set_layer_states {"layers": [{"layer": "...", "locked": false, "hidden": false}]}`.
* **Current story** decides where elements go when `storyIndex` is omitted.
* **Zones**: two automatic zones cannot share a room; a zone's reference point cannot be moved in place (it is re-created).
  Automatic zones work on any story (the connector makes the zone's story current while Archicad detects the boundary).
  A room with a stair void: draw a manual `polygon` zone that leaves the void out.
* **Element ID** exists for model elements and hatches only — lines, polylines, arcs, circles, splines, texts, labels,
  hotspots, pictures and dimensions have none (their tools do not accept `elementId`).
* **Object insertion points differ per library part**: most use the bottom-left corner of the A×B box, but chairs and
  coffee tables use their centre, WCs and washbasins the middle of their back edge, some kitchen cabinets their front edge.
  After placing furniture, `capture_view` the floor plan zoomed to the room (`zoom {mode: 'box'}`) and fix offsets with
  `modify_elements {guid, position}` / `angle`. With `angle` (CCW) the A×B box rotates around the insertion point.
* **Saving after an Archicad restart** can fail with "read-only" (stale lock): `save_project_as` to the same path with
  `overwrite: true` clears it.
* **Hidden layers**: elements on a hidden layer cannot be modified or deleted. Switch the layer combination first
  (`apply_layer_combination`). While a layout or other non-plan window is in front, `find_elements` sees only that
  window's database: add `filters: ["FromFloorPlan"]` to search the model.
* **Drawings**:
  * They are built from a saved view's settings: change the view, not the drawing. `set_view_settings {view}` opens the
    view once so new drawings pick the change up.
  * Drawings already placed keep their old scale and crop. Re-place them (`place_drawing` + `delete_drawings`).
    `update_drawings` can report "UpToDate" while showing old content.
  * A drawing's frame (the crop polygon, also present on unclipped drawings) is what `bounds`, the anchor and the
    drawing title follow.
* **Layout Book**: layout IDs come from the subset numbering. Renaming an auto-numbered subset's ID does not last;
  create the subset with custom numbering (§4.5) and `move_navigator_item` the layouts into it. A subset's numbering
  cannot be changed after creation.
* **Text labels** (`create_labels` without a parent's auto text) are single-line unless you give `wrapWidth`.
* **Undo/redo** use Archicad's Edit menu (the AC26 API has none): they act on the whole undo history.
* **Render**: `render_view` uses the CineRender engine; if it returns a blank image (warning in the result) use `capture_view` of the 3D window.
* **DWG**: Archicad 26's API cannot write DWG; `export_dwg` writes DXF (any CAD opens it) and converts to .dwg only if the ODA File Converter is installed.
* **Stairs, railings, curtain walls**: their base line/path cannot be changed after creation (re-create them); curtain wall grids need at least one flexible module (default: the first).
* **Openings** cannot be placed into polygonal walls; the host of an existing opening cannot be changed.
* **Project operations** (`open_project`, `new_project`, `close_project`, `quit_archicad`) need an explicit `saveFirst` or `discardChanges`; closing the project or quitting ends API access until the user reopens a project.
* **Teamwork** tools report "not a Teamwork project" on solo projects.
* **Anything missing?** `list_addon_commands` + `execute_addon_command`, or `execute_json_api_command` for official `API.*` commands.

## 6. Which tool for what

| Need | Tools |
|---|---|
| Connection / meta | `archicad_status`, `select_archicad_instance`, `get_connector_guide`, `list_addon_commands`, `execute_*` |
| Project | `get_project_info`, `get/set_project_info_fields`, `save_project(_as)`, `open/new/close_project`, `get/set_preferences`, `get/set_geo_location`, `rebuild_model`, `undo`, `redo`, `quit_archicad` |
| Stories | `get_stories`, `create/modify/delete_stories`, `set_current_story` |
| Attributes | `get_attributes`, `create_layers/_layer_combinations/_composites/_building_materials/_surfaces/_fills/_line_types/_zone_categories/_profiles`, `duplicate_attributes`, `modify_attributes`, `modify_pens`, `delete_attributes`, `set_layer_states`, `apply_layer_combination`, attribute folders |
| Model elements | `create_walls`, `create_columns`, `create_beams`, `create_slabs`, `create_roofs`, `create_shells`, `create_meshes`, `create_morphs`, `create_curtain_walls`, `create_stairs`, `create_railings`, `create_zones`, `create_windows/_doors/_skylights/_openings`, `create_objects/_lamps`, matching `modify_*`, generic `create_elements` / `modify_elements` / `get_element_details` |
| 2D | `create_lines/_arcs/_circles/_polylines/_splines/_hatches/_texts/_labels/_hotspots/_pictures`, `create_dimensions/_level_dimensions/_radial_dimensions/_angle_dimensions`, `dimension_walls` |
| Query | `find_elements`, `get_element_counts`, `list_elements`, `get_element_quantities`, `get_connected_elements`, `get_element_relations`, `get_subelements`, `get_bounding_boxes`, `get_element_2d_geometry`, `get_element_3d_geometry`, `get_selection` |
| Edit | `move/copy/rotate/mirror/elevate/resize/delete_elements`, `copy_elements_to_stories`, `group/ungroup`, `lock/unlock`, `set_draw_order`, `trim_elements`, `merge_elements`, `solid_operation`, `set_selection` |
| Library / GDL | `search_library_parts`, `get_library_part_details`, `get_library_part_scripts`, `create_library_part`, `get/set_gdl_parameters`, `change_library_part`, `get/add/remove/reload_libraries` |
| Views | `get_current_window`, `list_views`, `open_view`, `go_to_view`, `zoom`, `get/set_3d_view`, `show_in_3d`, `capture_view`, `render_view`, `create_sections/_elevations/_interior_elevations/_details/_worksheets`, `get/set_view_settings` |
| Documentation | `create_layout(_subset)`, `get/set_layout_settings`, `get_databases`, `place_drawing`, `get_layout_drawings`, `modify/delete/update_drawings`, `export_pdf/_ifc/_dwg/_3d_model/_module`, `merge_file`, hotlinks, `get_publisher_sets`, `publish_publisher_set`, navigator tools |
| Data | `get_property_definitions`, property group/definition CRUD, `get/set_property_values`, attribute property values, classification systems/items CRUD, `get/set_element_classifications`, `get_ifc_data`, `set_ifc_properties`, components |
| Collaboration | Teamwork status/send/receive/reserve/release, issues + BCF, favorites, `get/set_tool_defaults`, revisions |

The complete, generated reference of every tool and field is `docs/TOOLS.md`.
