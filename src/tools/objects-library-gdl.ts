/**
 * Static texts for the objects-library tools: built-in subtype keywords (must match kSubtypes in
 * addon/Src/Commands/ObjectsLibrary.cpp) and the GDL quick reference shown in create_library_part.
 */

export const SUBTYPE_KEYWORDS = [
  "GeneralGDLObject",
  "ModelElement",
  "BuildingElement",
  "Furnishing",
  "Beds",
  "Structure",
  "Column",
  "Beam",
  "Slab",
  "Wall",
  "Roof",
  "Stair",
  "Railing",
  "Ramp",
  "Covering",
  "Footing",
  "Plant",
  "People",
  "Animal",
  "Traffic",
  "TransportElement",
  "StreetFurniture",
  "SportField",
  "DistributionElement",
  "ElectricalElement",
  "FlowTerminal",
  "FlowEquipment",
  "SolarPVPanels",
  "DrawingSymbol",
  "DocumentationElement",
  "Marker",
  "PropertyObjects",
  "Light",
  "WindowWall",
  "CornerWindow",
  "DoorWall",
  "WallOpening",
  "WallEnd",
  "Skylight",
  "Label",
  "ZoneStamp",
] as const;

export const GDL_CHEAT_SHEET = `GDL QUICK REFERENCE (units: meters, angles in DEGREES):
- Local system: origin = insertion point, X along A (width), Y along B (depth), Z up, ZZYZX = height. Every parameter is a variable in all scripts; A, B (and ZZYZX for objects) always exist.
- masterScript runs before the 2D, 3D and parameter scripts: compute shared variables there. One statement per line; '!' starts a comment; strings in "..." or '...'.
- 3D transformations (a stack): ADDX dx / ADDY dy / ADDZ dz / ADD dx, dy, dz; ROTX a / ROTY a / ROTZ a; MULX f / MUL fx, fy, fz; DEL n (undo the last n), DEL TOP (undo all).
- 3D bodies at the current origin: BLOCK a, b, c (box along +X +Y +Z); PRISM_ n, h, x1, y1, s1, ..., xn, yn, sn (vertical extrusion of a polygon; s = 15 for visible edges); CPRISM_ topMat, botMat, sideMat, n, h, x1, y1, s1, ...; CYLIND h, r (along Z); SPHERE r; ELLIPS h, r; CONE h, r1, r2, 90, 90; REVOLVE n, alpha, mask, x1, y1, s1, ... (profile in the XY plane revolved around X); EXTRUDE n, dx, dy, dz, mask, x1, y1, s1, ...; TUBE; RULED; SLAB_.
- Attributes: MATERIAL m (surface name, index or a Surface parameter) before the bodies; PEN p; RESOL n (segments of curved surfaces); HOTSPOT x, y, z (3D editing point).
- 2D script (plan symbol): PROJECT2 3, 270, 2 (top view of the 3D model), or draw: LINE2 x1, y1, x2, y2; RECT2 x1, y1, x2, y2; POLY2_ n, frameFill, x1, y1, s1, ... (frameFill 1 = contour, 2 = fill, 4 = close; 7 = all); CIRCLE2 x, y, r; ARC2 x, y, r, a1, a2; FILL f before a filled POLY2_; HOTSPOT2 x, y; TEXT2 x, y, "text"; ADD2 dx, dy / ROT2 a / MUL2 fx, fy / DEL n.
- parameterScript: VALUES "len" 0.6, 0.8, 1.2 | VALUES "len" RANGE [0.3, 2.4] | VALUES "style" "Modern", "Classic"; LOCK "param"; HIDEPARAMETER "param"; PARAMETERS param = expression (store a computed value).
- Flow: IF c THEN ... ELSE ... ENDIF; FOR i = 1 TO n ... NEXT i; WHILE c DO ... ENDWHILE; GOSUB "sub" ... END ... "sub": ... RETURN. Operators + - * / ^ MOD = <> < > <= >= AND OR NOT; functions SIN COS TAN ATN (degrees), SQR (square root), ABS, MIN, MAX, INT, FRA, STR, PI.
- Example: table with parameters [{name:'topThk', type:'Length', value:0.04}, {name:'legW', type:'Length', value:0.05}, {name:'mat', type:'Surface', value:<surface name>}], a: 1.6, b: 0.8, height: 0.75, script3D:
  MATERIAL mat
  ADDZ ZZYZX - topThk
  BLOCK A, B, topThk
  DEL 1
  FOR i = 0 TO 1
    FOR j = 0 TO 1
      ADD i * (A - legW), j * (B - legW), 0
      BLOCK legW, legW, ZZYZX - topThk
      DEL 1
    NEXT j
  NEXT i
  (script2D omitted = PROJECT2 3, 270, 2)`;
