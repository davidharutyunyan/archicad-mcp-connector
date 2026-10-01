// *****************************************************************************
// ComplexElementsMorph — Morph adapter (API_MorphID) + GetMorphGeometry.
//
// Geometry (exactly one on create; on modify it REPLACES the body):
//   box       {origin {x,y,z}, size {x,y,z}, rotation? (deg, around the vertical axis through origin),
//              topSurface?, bottomSurface?, sideSurface?}
//   extrusion {polygon (points/arcs/holes), zBottom, zTop, arcSegmentAngle? (deg, default 10),
//              topSurface?, bottomSurface?, sideSurface?}
//   mesh      {vertices [{x,y,z}], faces [{vertices [i,j,k,...], holes? [{vertices}|[i,...]], surface?} | [i,j,k,...]]}
//             faces counter-clockwise seen from outside (right-hand rule gives the outward normal),
//             every edge shared by two faces must be traversed in opposite directions.
//   x/y are project coordinates, z is relative to the home story level (all meters).
//   The body is built with the BREP body API (ACAPI_Body_*), the element transformation is identity.
//
// Other create / modify fields:
//   bodyType "Solid"|"Surface" (default: Solid for closed bodies), edgeType "SoftHidden"|"HardHidden"|"HardVisible",
//   surface (override of non-customized faces, false = none), faceSurface (all faces / faces without own surface),
//   buildingMaterial, castShadow, receiveShadow, floorPlanDisplay, viewDepth, cutLinePen, cutLineType,
//   uncutLinePen, uncutLineType, overheadLinePen, overheadLineType, cutFillPen, cutFillBackgroundPen,
//   showCoverFill, coverFill, coverFillPen, coverFillBackgroundPen, coverFillFromSurface
//   modify only: offset {x,y,z} (move), level (move vertically so the body bottom is at this height)
//   + common: layer, storyIndex, renovationStatus, elementId
// *****************************************************************************

// The Model3D headers pull in GSRoot/Algorithms.hpp, which still uses std::random_shuffle
// (removed from C++17 libc++); re-enable it before any standard header is included.
#ifndef _LIBCPP_ENABLE_CXX17_REMOVED_RANDOM_SHUFFLE
#define _LIBCPP_ENABLE_CXX17_REMOVED_RANDOM_SHUFFLE
#endif

#include "Commands/ComplexElementsCommon.hpp"

#include "Model3D/MeshBody.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace cc {
namespace complex {

namespace {

const NamedValue kBodyTypes[] = {
	{ "Solid",		APIMorphBodyType_SolidBody },
	{ "Surface",	APIMorphBodyType_SurfaceBody },
};

const NamedValue kEdgeTypes[] = {
	{ "SoftHidden",		APIMorphEdgeType_SoftHiddenEdge },
	{ "HardHidden",		APIMorphEdgeType_HardHiddenEdge },
	{ "HardVisible",	APIMorphEdgeType_HardVisibleEdge },
};

const NamedValue kDisplayOptions[] = {
	{ "Projected",				API_Standard },
	{ "ProjectedWithOverhead",	API_StandardWithAbstract },
	{ "CutOnly",				API_CutOnly },
	{ "OutlinesOnly",			API_OutLinesOnly },
	{ "OverheadAll",			API_AbstractAll },
	{ "SymbolicCut",			API_CutAll },
	{ "Symbolic",				API_CutAll },
};

const NamedValue kViewDepth[] = {
	{ "ToFloorPlanRange",	API_ToFloorPlanRange },
	{ "ToAbsoluteLimit",	API_ToAbsoluteLimit },
	{ "EntireElement",		API_EntireElement },
};

constexpr UInt32 kMaxMeshItems = 200000;

// --- Mesh model ------------------------------------------------------------------------

struct MeshFace {
	GS::Array<Int32>				outer;
	GS::Array<GS::Array<Int32>>		holes;
	API_OverriddenAttribute			surface;		// overridden == false -> the element's surface
};

struct MeshData {
	GS::Array<API_Coord3D>	vertices;
	GS::Array<MeshFace>		faces;
};


API_OverriddenAttribute NoOverride ()
{
	API_OverriddenAttribute a;
	BNZeroMemory (&a, sizeof (a));
	a.overridden = false;
	return a;
}


API_OverriddenAttribute SurfaceOf (const OS& os, const char* key, const API_OverriddenAttribute& def)
{
	API_OverriddenAttribute attr = def;
	ApplyOverriddenSurface (os, key, attr);
	return attr;
}


GS::Array<Int32> ToIndexLoop (const GS::Array<double>& values, const GS::UniString& what)
{
	GS::Array<Int32> loop;
	for (double v : values) {
		if (!std::isfinite (v) || std::fabs (v - std::round (v)) > 1e-9 || v < 0)
			Fail (what + ": vertex indices must be non-negative integers (0-based into 'vertices').");
		loop.Push ((Int32) std::llround (v));
	}
	return loop;
}


// Reads an array of index loops: [{vertices: [...]}] or [[...], [...]].
GS::Array<GS::Array<Int32>> GetIndexLoops (const OS& os, const char* key, const GS::UniString& what)
{
	GS::Array<GS::Array<Int32>> out;
	if (!os.Contains (key))
		return out;
	if (!os.IsList (key))
		Fail (what + " must be an array.");

	GS::Array<OS> objects;
	if (os.Get (key, objects) && (objects.IsEmpty () || objects[0].Contains ("vertices"))) {
		for (const OS& o : objects)
			out.Push (ToIndexLoop (GetNumberArray (o, "vertices", true), what));
		return out;
	}
	GS::Array<GS::Array<Int64>> ints;
	if (os.Get (key, ints)) {
		for (const GS::Array<Int64>& l : ints) {
			GS::Array<double> d;
			for (Int64 v : l) d.Push ((double) v);
			out.Push (ToIndexLoop (d, what));
		}
		return out;
	}
	GS::Array<GS::Array<double>> reals;
	if (os.Get (key, reals)) {
		for (const GS::Array<double>& l : reals)
			out.Push (ToIndexLoop (l, what));
		return out;
	}
	Fail (what + " must be an array of index arrays, e.g. [[0,1,2],[2,3,0]] or [{\"vertices\": [0,1,2]}].");
}


void ValidateLoop (const GS::Array<Int32>& loop, Int32 nVertices, const GS::UniString& what)
{
	if (loop.GetSize () < 3)
		Fail (what + " needs at least 3 vertex indices.");
	for (UIndex i = 0; i < loop.GetSize (); ++i) {
		if (loop[i] >= nVertices) {
			GS::UniString msg;
			msg.Printf (": vertex index %d is out of range (the mesh has %d vertices, indices are 0-based).", (int) loop[i], (int) nVertices);
			Fail (what + msg);
		}
		for (UIndex j = i + 1; j < loop.GetSize (); ++j) {
			if (loop[i] == loop[j]) {
				GS::UniString msg;
				msg.Printf (": vertex %d is used twice in the same loop; split the face or remove the duplicate.", (int) loop[i]);
				Fail (what + msg);
			}
		}
	}
}


MeshData MeshFromSpec (const OS& geom, const API_OverriddenAttribute& defaultFace)
{
	MeshData mesh;
	const GS::Array<OS> vertices = GetObjectArray (geom, "vertices");
	if (vertices.GetSize () < 3 || vertices.GetSize () > kMaxMeshItems)
		Fail ("mesh.vertices needs 3..200000 points {x, y, z} (meters, z relative to the home story).");
	for (const OS& v : vertices) {
		if (!v.Contains ("x") || !v.Contains ("y") || !v.Contains ("z"))
			Fail ("mesh.vertices items must be {x, y, z} objects (meters).");
		mesh.vertices.Push (Coord3DFrom (v));
	}
	const Int32 nV = (Int32) mesh.vertices.GetSize ();

	if (!geom.Contains ("faces") || !geom.IsList ("faces"))
		Fail ("mesh.faces is required: an array of faces, each [i, j, k, ...] or {vertices: [...], holes?, surface?}.");

	GS::Array<OS> faceObjects;
	const bool objectForm = geom.Get ("faces", faceObjects) && !faceObjects.IsEmpty () && faceObjects[0].Contains ("vertices");
	if (objectForm) {
		if (faceObjects.GetSize () > kMaxMeshItems)
			Fail ("mesh.faces: at most 200000 faces.");
		for (UIndex i = 0; i < faceObjects.GetSize (); ++i) {
			GS::UniString what;
			what.Printf ("mesh.faces[%d]", (int) i);
			const OS& f = faceObjects[i];
			MeshFace face;
			face.outer = ToIndexLoop (GetNumberArray (f, "vertices", true), what);
			ValidateLoop (face.outer, nV, what);
			face.holes = GetIndexLoops (f, "holes", what + ".holes");
			for (UIndex h = 0; h < face.holes.GetSize (); ++h) {
				GS::UniString tail;
				tail.Printf (".holes[%d]", (int) h);
				const GS::UniString hw = what + tail;
				ValidateLoop (face.holes[h], nV, hw);
			}
			face.surface = SurfaceOf (f, "surface", defaultFace);
			mesh.faces.Push (face);
		}
	} else {
		GS::Array<GS::Array<Int32>> loops = GetIndexLoops (geom, "faces", "mesh.faces");
		if (loops.GetSize () > kMaxMeshItems)
			Fail ("mesh.faces: at most 200000 faces.");
		for (UIndex i = 0; i < loops.GetSize (); ++i) {
			GS::UniString what;
			what.Printf ("mesh.faces[%d]", (int) i);
			ValidateLoop (loops[i], nV, what);
			MeshFace face;
			face.outer = loops[i];
			face.surface = defaultFace;
			mesh.faces.Push (face);
		}
	}
	if (mesh.faces.IsEmpty ())
		Fail ("mesh.faces must contain at least one face.");
	return mesh;
}


API_Coord3D V3 (double x, double y, double z)
{
	API_Coord3D c;
	c.x = x; c.y = y; c.z = z;
	return c;
}


MeshData BoxFromSpec (const OS& geom, const API_OverriddenAttribute& defaultFace)
{
	const API_Coord3D o = GetCoord3D (geom, "origin");
	const OS size = GetObject (geom, "size");
	const double sx = GetDouble (size, "x"), sy = GetDouble (size, "y"), sz = GetDouble (size, "z");
	if (!(sx > 0.0) || !(sy > 0.0) || !(sz > 0.0))
		Fail ("box.size x, y and z must all be greater than 0 (meters).");
	const double rot = GetAngle (geom, "rotation", 0.0);
	const double c = std::cos (rot), s = std::sin (rot);

	MeshData mesh;
	const double lx[4] = { 0.0, sx, sx, 0.0 };
	const double ly[4] = { 0.0, 0.0, sy, sy };
	for (int level = 0; level < 2; ++level) {
		for (int i = 0; i < 4; ++i)
			mesh.vertices.Push (V3 (o.x + c * lx[i] - s * ly[i], o.y + s * lx[i] + c * ly[i], o.z + (level == 0 ? 0.0 : sz)));
	}

	const API_OverriddenAttribute top = SurfaceOf (geom, "topSurface", defaultFace);
	const API_OverriddenAttribute bottom = SurfaceOf (geom, "bottomSurface", defaultFace);
	const API_OverriddenAttribute side = SurfaceOf (geom, "sideSurface", defaultFace);
	auto addFace = [&] (std::initializer_list<Int32> idx, const API_OverriddenAttribute& surface) {
		MeshFace f;
		for (Int32 i : idx) f.outer.Push (i);
		f.surface = surface;
		mesh.faces.Push (f);
	};
	addFace ({ 0, 3, 2, 1 }, bottom);
	addFace ({ 4, 5, 6, 7 }, top);
	addFace ({ 0, 1, 5, 4 }, side);
	addFace ({ 1, 2, 6, 5 }, side);
	addFace ({ 2, 3, 7, 6 }, side);
	addFace ({ 3, 0, 4, 7 }, side);
	return mesh;
}


// Replaces arc edges by straight segments (maxSegAngle radians per segment).
GS::Array<API_Coord> TessellateContour (const Contour& c, double maxSegAngle)
{
	GS::Array<API_Coord> out;
	const Int32 n = (Int32) c.points.GetSize ();
	for (Int32 i = 0; i < n; ++i) {
		const API_Coord& p = c.points[i];
		const API_Coord& q = c.points[(i + 1) % n];
		out.Push (p);
		double angle = 0.0;
		for (const auto& a : c.arcs) {
			if (a.first == i)
				angle = a.second;
		}
		const double dx = q.x - p.x, dy = q.y - p.y;
		const double len = std::hypot (dx, dy);
		if (std::fabs (angle) < 1e-9 || len < 1e-9)
			continue;
		const double d = (len / 2.0) / std::tan (angle / 2.0);
		const double cx = (p.x + q.x) / 2.0 - d * dy / len;
		const double cy = (p.y + q.y) / 2.0 + d * dx / len;
		const double r = std::hypot (p.x - cx, p.y - cy);
		const double a0 = std::atan2 (p.y - cy, p.x - cx);
		const Int32 segs = std::max<Int32> (2, (Int32) std::ceil (std::fabs (angle) / maxSegAngle));
		for (Int32 k = 1; k < segs; ++k) {
			const double t = a0 + angle * (double) k / (double) segs;
			API_Coord m;
			m.x = cx + r * std::cos (t);
			m.y = cy + r * std::sin (t);
			out.Push (m);
		}
	}
	// drop coincident neighbours
	GS::Array<API_Coord> clean;
	for (UIndex i = 0; i < out.GetSize (); ++i) {
		const API_Coord& a = out[i];
		const API_Coord& b = out[(i + 1) % out.GetSize ()];
		if (std::hypot (b.x - a.x, b.y - a.y) > 1e-7)
			clean.Push (a);
	}
	return clean;
}


MeshData ExtrusionFromSpec (const OS& geom, const API_OverriddenAttribute& defaultFace)
{
	PolygonData polygon = GetPolygon (geom, "polygon");
	const double zBottom = GetDouble (geom, "zBottom");
	const double zTop = GetDouble (geom, "zTop");
	if (!(zTop - zBottom > 1e-6))
		Fail ("extrusion.zTop must be greater than extrusion.zBottom (meters, relative to the home story).");
	const double segDeg = GetDouble (geom, "arcSegmentAngle", 10.0);
	if (!(segDeg >= 0.5 && segDeg <= 90.0))
		Fail ("extrusion.arcSegmentAngle must be between 0.5 and 90 degrees.");

	NormalizeOrientation (polygon);		// outline counter-clockwise, holes clockwise
	GS::Array<GS::Array<API_Coord>> rings;
	rings.Push (TessellateContour (polygon.outline, DegToRad (segDeg)));
	for (const Contour& h : polygon.holes)
		rings.Push (TessellateContour (h, DegToRad (segDeg)));
	for (const auto& r : rings) {
		if (r.GetSize () < 3 || std::fabs (SignedArea (r)) < 1e-9)
			Fail ("extrusion.polygon (or one of its holes) is degenerate: it needs at least 3 distinct points enclosing an area.");
	}

	const API_OverriddenAttribute top = SurfaceOf (geom, "topSurface", defaultFace);
	const API_OverriddenAttribute bottom = SurfaceOf (geom, "bottomSurface", defaultFace);
	const API_OverriddenAttribute side = SurfaceOf (geom, "sideSurface", defaultFace);

	MeshData mesh;
	GS::Array<Int32> firstBottom, firstTop;
	for (const auto& r : rings) {
		firstBottom.Push ((Int32) mesh.vertices.GetSize ());
		for (const API_Coord& p : r) mesh.vertices.Push (V3 (p.x, p.y, zBottom));
		firstTop.Push ((Int32) mesh.vertices.GetSize ());
		for (const API_Coord& p : r) mesh.vertices.Push (V3 (p.x, p.y, zTop));
	}

	MeshFace bottomFace, topFace;
	bottomFace.surface = bottom;
	topFace.surface = top;
	for (UIndex ri = 0; ri < rings.GetSize (); ++ri) {
		const Int32 n = (Int32) rings[ri].GetSize ();
		GS::Array<Int32> b, t;
		for (Int32 i = n - 1; i >= 0; --i) b.Push (firstBottom[ri] + i);	// reversed: normal points down
		for (Int32 i = 0; i < n; ++i) t.Push (firstTop[ri] + i);
		if (ri == 0) {
			bottomFace.outer = b;
			topFace.outer = t;
		} else {
			bottomFace.holes.Push (b);
			topFace.holes.Push (t);
		}
		for (Int32 i = 0; i < n; ++i) {
			const Int32 j = (i + 1) % n;
			MeshFace sideFace;
			sideFace.surface = side;
			sideFace.outer.Push (firstBottom[ri] + i);
			sideFace.outer.Push (firstBottom[ri] + j);
			sideFace.outer.Push (firstTop[ri] + j);
			sideFace.outer.Push (firstTop[ri] + i);
			mesh.faces.Push (sideFace);
		}
	}
	mesh.faces.Push (bottomFace);
	mesh.faces.Push (topFace);
	return mesh;
}


bool HasGeometry (const OS& spec)
{
	return spec.Contains ("box") || spec.Contains ("extrusion") || spec.Contains ("mesh");
}


MeshData GeometryFromSpec (const OS& spec)
{
	const int given = (spec.Contains ("box") ? 1 : 0) + (spec.Contains ("extrusion") ? 1 : 0) + (spec.Contains ("mesh") ? 1 : 0);
	if (given != 1)
		Fail ("Give exactly one morph geometry: 'box' {origin, size}, 'extrusion' {polygon, zBottom, zTop} or 'mesh' {vertices, faces}.");
	const API_OverriddenAttribute defaultFace = SurfaceOf (spec, "faceSurface", NoOverride ());
	if (spec.Contains ("box"))
		return BoxFromSpec (GetObject (spec, "box"), defaultFace);
	if (spec.Contains ("extrusion"))
		return ExtrusionFromSpec (GetObject (spec, "extrusion"), defaultFace);
	return MeshFromSpec (GetObject (spec, "mesh"), defaultFace);
}

// --- BREP body construction -----------------------------------------------------------------

class BodyDataGuard {
public:
	BodyDataGuard () = default;
	~BodyDataGuard () { if (data != nullptr) ACAPI_Body_Dispose (&data); }
	BodyDataGuard (const BodyDataGuard&) = delete;
	BodyDataGuard& operator= (const BodyDataGuard&) = delete;

	void* data = nullptr;
};


struct EdgeRec {
	Int32	index = 0;
	Int32	from = 0;
	bool	forwardUsed = false;
	bool	backwardUsed = false;
};


// Builds memo.morphBody / memo.morphMaterialMapTable from the mesh. Returns true when the body is closed.
bool BuildMorphBody (const MeshData& mesh, API_ElementMemo& memo)
{
	if (memo.morphBody != nullptr || memo.morphMaterialMapTable != nullptr)
		Fail ("Internal error: morph memo already has a body.", APIERR_GENERAL);

	BodyDataGuard body;
	Check (ACAPI_Body_Create (nullptr, nullptr, &body.data), "Cannot start the morph body");
	if (body.data == nullptr)
		Fail ("Cannot start the morph body (out of memory).", APIERR_MEMFULL);

	GS::Array<UInt32> vIdx;
	for (const API_Coord3D& v : mesh.vertices) {
		UInt32 index = 0;
		Check (ACAPI_Body_AddVertex (body.data, v, index), "Cannot add a vertex to the morph body");
		vIdx.Push (index);
	}

	std::unordered_map<UInt64, EdgeRec> edges;
	auto signedEdge = [&] (Int32 a, Int32 b, UIndex faceIndex) -> Int32 {
		const UInt64 key = ((UInt64) (UInt32) std::min (a, b) << 32) | (UInt64) (UInt32) std::max (a, b);
		auto it = edges.find (key);
		if (it == edges.end ()) {
			Int32 index = 0;
			Check (ACAPI_Body_AddEdge (body.data, vIdx[a], vIdx[b], index), "Cannot add an edge to the morph body");
			if (index == 0)
				Fail ("Cannot add an edge to the morph body (invalid edge index).", APIERR_GENERAL);
			EdgeRec rec;
			rec.index = index;
			rec.from = a;
			it = edges.emplace (key, rec).first;
		}
		EdgeRec& rec = it->second;
		const bool forward = rec.from == a;
		bool& used = forward ? rec.forwardUsed : rec.backwardUsed;
		if (used) {
			GS::UniString msg;
			msg.Printf ("Face %d traverses the edge %d->%d in the same direction as another face. Orient all faces consistently "
						"(counter-clockwise seen from outside) and do not share an edge between more than two faces.",
						(int) faceIndex, (int) a, (int) b);
			Fail (msg);
		}
		used = true;
		return forward ? rec.index : -rec.index;
	};

	for (UIndex fi = 0; fi < mesh.faces.GetSize (); ++fi) {
		const MeshFace& face = mesh.faces[fi];
		GS::Array<Int32> polyEdges;
		auto addLoop = [&] (const GS::Array<Int32>& loop) {
			const UIndex n = loop.GetSize ();
			for (UIndex k = 0; k < n; ++k)
				polyEdges.Push (signedEdge (loop[k], loop[(k + 1) % n], fi));
		};
		addLoop (face.outer);
		for (const GS::Array<Int32>& hole : face.holes) {
			polyEdges.Push (0);		// contour separator
			addLoop (hole);
		}
		UInt32 polyIndex = 0;
		GSErrCode err = ACAPI_Body_AddPolygon (body.data, polyEdges, 0, face.surface, polyIndex);
		if (err != NoError) {
			GS::UniString msg;
			msg.Printf ("Cannot add face %d to the morph body (check that it is planar and not self-intersecting)", (int) fi);
			Check (err, msg);
		}
	}

	Check (ACAPI_Body_Finish (body.data, &memo.morphBody, &memo.morphMaterialMapTable), "Cannot finish the morph body");
	if (memo.morphBody == nullptr)
		Fail ("Archicad returned an empty morph body; check the faces (planar, consistently oriented, non-degenerate).", APIERR_GENERAL);

	bool closed = true;
	for (const auto& kv : edges) {
		if (!kv.second.forwardUsed || !kv.second.backwardUsed) {
			closed = false;
			break;
		}
	}
	return closed;
}


double MinZ (const MeshData& mesh)
{
	double z = std::numeric_limits<double>::max ();
	for (const API_Coord3D& v : mesh.vertices)
		z = std::min (z, v.z);
	return mesh.vertices.IsEmpty () ? 0.0 : z;
}


API_Tranmat IdentityTranmat ()
{
	API_Tranmat t;
	BNZeroMemory (&t, sizeof (t));
	t.tmx[0] = t.tmx[5] = t.tmx[10] = 1.0;
	return t;
}


API_Coord3D Transform (const API_Tranmat& t, double x, double y, double z)
{
	const double* m = t.tmx;
	return V3 (m[0] * x + m[1] * y + m[2] * z + m[3],
			   m[4] * x + m[5] * y + m[6] * z + m[7],
			   m[8] * x + m[9] * y + m[10] * z + m[11]);
}

// --- Struct fields ------------------------------------------------------------------------------

void ApplyMorphFields (API_MorphType& morph, API_Element* mask, const OS& spec)
{
#define MORPH_SET(field) if (mask != nullptr) ACAPI_ELEMENT_MASK_SET (*mask, API_MorphType, field)

	if (Has (spec, "bodyType"))	{ morph.bodyType = (API_MorphBodyTypeID) ParseNamed (kBodyTypes, spec, "bodyType"); MORPH_SET (bodyType); }
	if (Has (spec, "edgeType"))	{ morph.edgeType = (API_MorphEdgeTypeID) ParseNamed (kEdgeTypes, spec, "edgeType"); MORPH_SET (edgeType); }
	if (ApplyFlag (spec, "castShadow", morph.castShadow))			MORPH_SET (castShadow);
	if (ApplyFlag (spec, "receiveShadow", morph.receiveShadow))		MORPH_SET (receiveShadow);
	if (Has (spec, "composite") || Has (spec, "profile"))
		Fail ("Morphs have a single building material: use 'buildingMaterial'.", APIERR_NOTSUPPORTED);
	if (ApplyAttrField (spec, "buildingMaterial", API_BuildingMaterialID, morph.buildingMaterial))	MORPH_SET (buildingMaterial);
	if (ApplyOverriddenSurface (spec, "surface", morph.material))	MORPH_SET (material);
	if (Has (spec, "floorPlanDisplay")) {
		morph.displayOption = (API_ElemDisplayOptionsID) ParseNamed (kDisplayOptions, spec, "floorPlanDisplay");
		MORPH_SET (displayOption);
	}
	if (Has (spec, "viewDepth")) {
		morph.viewDepthLimitation = (API_ElemViewDepthLimitationsID) ParseNamed (kViewDepth, spec, "viewDepth");
		MORPH_SET (viewDepthLimitation);
	}
	if (ApplyPenField (spec, "cutLinePen", morph.cutLinePen))										MORPH_SET (cutLinePen);
	if (ApplyAttrField (spec, "cutLineType", API_LinetypeID, morph.cutLineType))					MORPH_SET (cutLineType);
	if (ApplyPenField (spec, "uncutLinePen", morph.uncutLinePen))									MORPH_SET (uncutLinePen);
	if (ApplyAttrField (spec, "uncutLineType", API_LinetypeID, morph.uncutLineType))				MORPH_SET (uncutLineType);
	if (ApplyPenField (spec, "overheadLinePen", morph.overheadLinePen))								MORPH_SET (overheadLinePen);
	if (ApplyAttrField (spec, "overheadLineType", API_LinetypeID, morph.overheadLineType))			MORPH_SET (overheadLineType);
	if (ApplyCutFillPenOverride (spec, morph.penOverride))											MORPH_SET (penOverride);
	if (ApplyFlag (spec, "showCoverFill", morph.useCoverFillType))									MORPH_SET (useCoverFillType);
	if (ApplyAttrField (spec, "coverFill", API_FilltypeID, morph.coverFillType)) {
		MORPH_SET (coverFillType);
		if (!spec.Contains ("showCoverFill")) { morph.useCoverFillType = true; MORPH_SET (useCoverFillType); }
	}
	if (ApplyPenField (spec, "coverFillPen", morph.coverFillPen))									MORPH_SET (coverFillPen);
	if (ApplyPenField (spec, "coverFillBackgroundPen", morph.coverFillBGPen))						MORPH_SET (coverFillBGPen);
	if (ApplyFlag (spec, "coverFillFromSurface", morph.use3DHatching))								MORPH_SET (use3DHatching);
	if (ApplyFlag (spec, "outlineContourDisplay", morph.outlineContourDisplay))						MORPH_SET (outlineContourDisplay);
#undef MORPH_SET
}


GS::UniString CreateHint (GSErrCode err)
{
	if (err == APIERR_BADPARS || err == APIERR_GENERAL)
		return " (check the geometry: planar faces, consistent orientation, no degenerate faces; for open meshes use bodyType 'Surface')";
	return GS::UniString ();
}


void CheckBodyType (const API_MorphType& morph, bool closed, const OS& spec)
{
	if (morph.bodyType == APIMorphBodyType_SolidBody && !closed && spec.Contains ("bodyType"))
		Fail ("bodyType 'Solid' needs a closed mesh (every edge shared by exactly two faces in opposite directions). "
			  "Close the mesh or use bodyType 'Surface'.");
}


API_Guid CreateMorph (const OS& spec)
{
	API_Element element = NewElement (API_MorphID);
	GetDefaults (element, nullptr);
	ApplyCommonFields (element, nullptr, spec);
	ApplyStoryCreationMode (element.morph.linkToSettings, spec);

	const MeshData mesh = GeometryFromSpec (spec);
	ApplyMorphFields (element.morph, nullptr, spec);

	Memo memo;
	const bool closed = BuildMorphBody (mesh, *memo);
	if (!spec.Contains ("bodyType"))
		element.morph.bodyType = closed ? APIMorphBodyType_SolidBody : APIMorphBodyType_SurfaceBody;
	CheckBodyType (element.morph, closed, spec);

	element.morph.tranmat = IdentityTranmat ();
	element.morph.level = MinZ (mesh);

	GSErrCode err = ACAPI_Element_Create (&element, memo.Ptr ());
	Check (err, "Cannot create morph" + CreateHint (err));
	return element.header.guid;
}

// --- Serialization ------------------------------------------------------------------------------

OS TranmatJson (const API_Tranmat& t)
{
	GS::Array<double> m;
	for (double v : t.tmx) m.Push (v);
	return OS ("matrix", m, "origin", Coord3DObj (t.tmx[3], t.tmx[7], t.tmx[11]));
}


// Surface of a body polygon resolved through the material map table (best effort: polygon material index
// used as index into morphMaterialMapTable).
bool ResolveFaceSurface (const Modeler::MeshBody& body, ULong polygon, const API_ElementMemo& memo, API_OverriddenAttribute& out)
{
	const UInt32 tableSize = PtrCount (memo.morphMaterialMapTable);
	const GSAttributeIndex idx = body.GetConstPolygonAttributes (polygon).GetMaterialIndex ();
	if (idx < 0 || (UInt32) idx >= tableSize)
		return false;
	out = memo.morphMaterialMapTable[idx];
	return true;
}


void AddBodyJson (OS& out, const API_MorphType& morph, const API_ElementMemo& memo)
{
	const Modeler::MeshBody* body = memo.morphBody;
	if (body == nullptr) {
		out.Add ("body", OS ("empty", true));
		return;
	}
	const ULong nV = body->GetVertexCount ();
	OS b;
	b.Add ("vertexCount", (Int32) nV);
	b.Add ("edgeCount", (Int32) body->GetEdgeCount ());
	b.Add ("faceCount", (Int32) body->GetPolygonCount ());
	b.Add ("closed", body->IsClosedBody ());
	out.Add ("body", b);

	if (nV > 0) {
		API_Coord3D lo = V3 (1e300, 1e300, 1e300), hi = V3 (-1e300, -1e300, -1e300);
		for (ULong i = 0; i < nV; ++i) {
			const auto& v = body->GetConstVertex (i);
			const API_Coord3D w = Transform (morph.tranmat, v.x, v.y, v.z);
			lo.x = std::min (lo.x, w.x); lo.y = std::min (lo.y, w.y); lo.z = std::min (lo.z, w.z);
			hi.x = std::max (hi.x, w.x); hi.y = std::max (hi.y, w.y); hi.z = std::max (hi.z, w.z);
		}
		out.Add ("bodyBounds", OS ("min", Coord3DObj (lo), "max", Coord3DObj (hi)));
	}

	// Distinct per-face surface overrides stored in the material map table.
	GS::Array<API_AttributeIndex> seen;
	GS::Array<OS> overrides;
	const UInt32 tableSize = PtrCount (memo.morphMaterialMapTable);
	for (UInt32 i = 0; i < tableSize; ++i) {
		const API_OverriddenAttribute& a = memo.morphMaterialMapTable[i];
		if (!a.overridden || seen.Contains (a.attributeIndex))
			continue;
		seen.Push (a.attributeIndex);
		overrides.Push (AttrRef (API_MaterialID, a.attributeIndex));
	}
	out.Add ("faceSurfaceOverrides", overrides);
}


void SerializeMorph (const API_Element& element, OS& out)
{
	const API_MorphType& morph = element.morph;
	out.Add ("bodyType", NameOf (kBodyTypes, morph.bodyType));
	out.Add ("edgeType", NameOf (kEdgeTypes, morph.edgeType));
	out.Add ("level", morph.level);
	out.Add ("buildingMaterial", AttrRef (API_BuildingMaterialID, morph.buildingMaterial));
	AddOverriddenSurfaceJson (out, "surface", morph.material);
	out.Add ("castShadow", morph.castShadow);
	out.Add ("receiveShadow", morph.receiveShadow);
	out.Add ("transformation", TranmatJson (morph.tranmat));

	Memo memo;
	if (ACAPI_Element_GetMemo (element.header.guid, memo.Ptr (), APIMemoMask_All) == NoError)
		AddBodyJson (out, morph, *memo);
	AddBoundingBox (out, element.header);

	out.Add ("floorPlanDisplay", NameOf (kDisplayOptions, morph.displayOption));
	out.Add ("viewDepth", NameOf (kViewDepth, morph.viewDepthLimitation));
	out.Add ("cutLinePen", (Int32) morph.cutLinePen);
	out.Add ("cutLineType", AttrRef (API_LinetypeID, morph.cutLineType));
	out.Add ("uncutLinePen", (Int32) morph.uncutLinePen);
	out.Add ("uncutLineType", AttrRef (API_LinetypeID, morph.uncutLineType));
	out.Add ("overheadLinePen", (Int32) morph.overheadLinePen);
	out.Add ("overheadLineType", AttrRef (API_LinetypeID, morph.overheadLineType));
	AddCutFillPenOverrideJson (out, morph.penOverride);
	out.Add ("showCoverFill", morph.useCoverFillType);
	out.Add ("coverFill", AttrRef (API_FilltypeID, morph.coverFillType));
	out.Add ("coverFillPen", (Int32) morph.coverFillPen);
	out.Add ("coverFillBackgroundPen", (Int32) morph.coverFillBGPen);
	out.Add ("coverFillFromSurface", morph.use3DHatching);
	out.Add ("outlineContourDisplay", morph.outlineContourDisplay);
}

// --- Modification ---------------------------------------------------------------------------------

void CommitMorphChange (API_Element& element, API_Element& mask, API_ElementMemo* memo, const GS::UniString& what)
{
	GSErrCode err = ACAPI_Element_Change (&element, &mask, memo, 0, true);
	Check (err, what + " " + GuidStr (element.header.guid) + CreateHint (err));
	ACAPI_ELEMENT_MASK_CLEAR (mask);	// already committed: the core must not change it again
	API_Element fresh = GetElement (element.header.guid);
	element = fresh;
}


void ModifyMorph (API_Element& element, API_Element& mask, API_ElementMemo&, UInt64&, const OS& patch)
{
	API_MorphType& morph = element.morph;
	ApplyMorphFields (morph, &mask, patch);

	if (Has (patch, "level")) {
		const double level = GetDouble (patch, "level");
		morph.tranmat.tmx[11] += level - morph.level;
		morph.level = level;
		ACAPI_ELEMENT_MASK_SET (mask, API_MorphType, tranmat);
		ACAPI_ELEMENT_MASK_SET (mask, API_MorphType, level);
	}

	if (HasGeometry (patch)) {
		const MeshData mesh = GeometryFromSpec (patch);
		Memo memo;
		const bool closed = BuildMorphBody (mesh, *memo);
		if (!patch.Contains ("bodyType")) {
			morph.bodyType = closed ? APIMorphBodyType_SolidBody : APIMorphBodyType_SurfaceBody;
			ACAPI_ELEMENT_MASK_SET (mask, API_MorphType, bodyType);
		}
		CheckBodyType (morph, closed, patch);
		if (Has (patch, "level"))
			Fail ("Give either a new geometry (box/extrusion/mesh, whose z values define the height) or 'level', not both.");
		morph.tranmat = IdentityTranmat ();
		morph.level = MinZ (mesh);
		ACAPI_ELEMENT_MASK_SET (mask, API_MorphType, tranmat);
		ACAPI_ELEMENT_MASK_SET (mask, API_MorphType, level);
		if (Has (patch, "offset")) {
			const API_Coord3D d = GetCoord3D (patch, "offset");
			morph.tranmat.tmx[3] += d.x;
			morph.tranmat.tmx[7] += d.y;
			morph.tranmat.tmx[11] += d.z;
			morph.level += d.z;
		}
		// Remember the old body size to detect a silently ignored body replacement.
		ULong oldVertices = 0, oldFaces = 0;
		{
			Memo old;
			if (ACAPI_Element_GetMemo (element.header.guid, old.Ptr (), APIMemoMask_All) == NoError && old->morphBody != nullptr) {
				oldVertices = old->morphBody->GetVertexCount ();
				oldFaces = old->morphBody->GetPolygonCount ();
			}
		}
		const ULong newVertices = (ULong) mesh.vertices.GetSize ();
		const ULong newFaces = (ULong) mesh.faces.GetSize ();
		CommitMorphChange (element, mask, memo.Ptr (), "Cannot replace the body of morph");

		Memo check;
		if (ACAPI_Element_GetMemo (element.header.guid, check.Ptr (), APIMemoMask_All) == NoError && check->morphBody != nullptr) {
			const ULong v = check->morphBody->GetVertexCount ();
			const ULong f = check->morphBody->GetPolygonCount ();
			if (v == oldVertices && f == oldFaces && (v != newVertices || f != newFaces))
				Fail ("Archicad kept the old morph body (the other changes were applied). Delete the morph and create a new one with create_morphs.",
					  APIERR_GENERAL);
		}
		return;
	}

	if (Has (patch, "offset")) {
		const API_Coord3D d = GetCoord3D (patch, "offset");
		morph.tranmat.tmx[3] += d.x;
		morph.tranmat.tmx[7] += d.y;
		morph.tranmat.tmx[11] += d.z;
		morph.level += d.z;
		ACAPI_ELEMENT_MASK_SET (mask, API_MorphType, tranmat);
		ACAPI_ELEMENT_MASK_SET (mask, API_MorphType, level);
	}

	if (Has (patch, "faceSurface")) {
		// Every face gets the given override (false = all faces use the morph's 'surface').
		Memo memo;
		LoadMemo (element.header.guid, *memo, APIMemoMask_All);
		const API_OverriddenAttribute attr = SurfaceOf (patch, "faceSurface", NoOverride ());
		const UInt32 n = PtrCount (memo->morphMaterialMapTable);
		if (n == 0)
			Fail ("This morph has no per-face surface table; set 'surface' instead, or replace the body (box/extrusion/mesh) with 'faceSurface'.", APIERR_NOTSUPPORTED);
		for (UInt32 i = 0; i < n; ++i)
			memo->morphMaterialMapTable[i] = attr;
		CommitMorphChange (element, mask, memo.Ptr (), "Cannot change the face surfaces of morph");
	}
}

// --- GetMorphGeometry -----------------------------------------------------------------------------

OS MorphGeometryJson (const API_Guid& guid, UInt32 maxVertices)
{
	const API_Element element = GetElement (guid);
	if (element.header.type.typeID != API_MorphID)
		Fail ("Element " + GuidStr (guid) + " is a " + ElemTypeName (element.header.type) + ", not a Morph.");

	Memo memo;
	LoadMemo (guid, *memo, APIMemoMask_All);
	const Modeler::MeshBody* body = memo->morphBody;
	if (body == nullptr)
		Fail ("Morph " + GuidStr (guid) + " has no body data.", APIERR_GENERAL);

	const ULong nV = body->GetVertexCount ();
	const ULong nP = body->GetPolygonCount ();
	if (nV > maxVertices) {
		GS::UniString msg;
		msg.Printf ("Morph has %u vertices (more than maxVertices = %u). Raise maxVertices (max 200000) or use get_element_details for a summary.",
					(unsigned) nV, (unsigned) maxVertices);
		Fail (msg);
	}

	OS out;
	out.Add ("guid", GuidStr (guid));
	out.Add ("bodyType", NameOf (kBodyTypes, element.morph.bodyType));
	AddOverriddenSurfaceJson (out, "surface", element.morph.material);
	out.Add ("vertexCount", (Int32) nV);
	out.Add ("faceCount", (Int32) nP);

	GS::Array<OS> vertices;
	for (ULong i = 0; i < nV; ++i) {
		const auto& v = body->GetConstVertex (i);
		vertices.Push (Coord3DObj (Transform (element.morph.tranmat, v.x, v.y, v.z)));
	}
	out.Add ("vertices", vertices);

	GS::Array<OS> faces;
	for (ULong p = 0; p < nP; ++p) {
		GS::Array<ULong> idx;
		GS::Array<ULong> starts;
		body->GetPolygonVertices (p, idx, &starts);

		// Split into contours at the given start positions.
		GS::Array<UIndex> cuts;
		cuts.Push (0);
		for (ULong s : starts) {
			if (s > cuts[cuts.GetSize () - 1] && s < idx.GetSize ())
				cuts.Push ((UIndex) s);
		}
		cuts.Push (idx.GetSize ());
		GS::Array<GS::Array<Int32>> loops;
		for (UIndex c = 0; c + 1 < cuts.GetSize (); ++c) {
			GS::Array<Int32> loop;
			for (UIndex k = cuts[c]; k < cuts[c + 1]; ++k)
				loop.Push ((Int32) idx[k]);
			if (loop.GetSize () > 1 && loop[0] == loop[loop.GetSize () - 1])
				loop.Pop ();
			if (!loop.IsEmpty ())
				loops.Push (loop);
		}
		if (loops.IsEmpty ())
			continue;

		OS face;
		face.Add ("vertices", loops[0]);
		if (loops.GetSize () > 1) {
			GS::Array<OS> holes;
			for (UIndex h = 1; h < loops.GetSize (); ++h)
				holes.Push (OS ("vertices", loops[h]));
			face.Add ("holes", holes);
		}
		API_OverriddenAttribute surface;
		if (ResolveFaceSurface (*body, p, *memo, surface) && surface.overridden)
			face.Add ("surface", AttrRef (API_MaterialID, surface.attributeIndex));
		if (body->GetConstPolygonAttributes (p).IsInvisible ())
			face.Add ("hidden", true);
		faces.Push (face);
	}
	out.Add ("faces", faces);
	return out;
}

} // namespace


void RegisterMorphFamily ()
{
	RegisterAdapter ({ API_MorphID, CreateMorph, SerializeMorph, ModifyMorph });

	RegisterCommand ("GetMorphGeometry",
		"Returns the editable BREP geometry of morphs in the same format that the 'mesh' input of create/modify uses: "
		"{elements: [{guid, vertexCount, faceCount, vertices: [{x,y,z}], faces: [{vertices: [i,...], holes?: [{vertices}], surface?, hidden?}]}]}. "
		"x/y are project coordinates, z is relative to the home story (the morph transformation is already applied). "
		"Input: {elements: [guid], maxVertices?: 20000}.",
		[] (const OS& params) -> OS {
			const GS::Array<API_Guid> guids = GetGuidArray (params, "elements", true);
			const Int32 maxVertices = GetInt (params, "maxVertices", 20000);
			if (maxVertices < 1 || maxVertices > (Int32) kMaxMeshItems)
				Fail ("maxVertices must be between 1 and 200000.");
			GS::Array<OS> results;
			for (const API_Guid& guid : guids)
				results.Push (Try ([&] () { return MorphGeometryJson (guid, (UInt32) maxVertices); }));
			return OS ("elements", results);
		});
}

} // namespace complex
} // namespace cc
