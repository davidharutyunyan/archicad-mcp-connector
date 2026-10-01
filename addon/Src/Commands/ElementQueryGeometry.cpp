// *****************************************************************************
// ElementQueryGeometry — GetElement2DGeometry / GetElement3DGeometry
// (family "element-query").
//
//   GetElement2DGeometry  ACAPI_Element_ShapePrims: the 2D drawing primitives Archicad
//                         draws for an element (lines, arcs, polylines, polygons,
//                         texts ...), counts, extent. Coordinates are [x, y] in meters.
//   GetElement3DGeometry  ACAPI_Element_Get3DInfo + ACAPI_3D_GetComponent: bodies,
//                         vertex/edge/polygon counts, world bounding box, materials and
//                         (mode "mesh") vertices [x, y, z] + polygon vertex indices.
// *****************************************************************************

#include "Commands/ElementQueryShared.hpp"
#include "Core/Command.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

namespace cc {
namespace eq {

namespace {

// =============================================================================
// 2D primitives
// =============================================================================

enum PrimKindBits : UInt32 {
	PK_Point	= 0x01,
	PK_Line		= 0x02,
	PK_Arc		= 0x04,		// arc, circle, ellipse, ellipticArc
	PK_Polyline	= 0x08,
	PK_Polygon	= 0x10,		// polygons and triangles
	PK_Text		= 0x20,
	PK_Picture	= 0x40,
	PK_All		= 0xFF
};


UInt32 ParseKinds (const OS& params)
{
	GS::Array<OS> items = ArrayItems (params, "kinds");
	if (items.IsEmpty ())
		return PK_All;
	UInt32 bits = 0;
	for (const OS& item : items) {
		GS::UniString k = GetString (item, "v");
		if (EqualsIgnoreCase (k, "point"))											bits |= PK_Point;
		else if (EqualsIgnoreCase (k, "line"))										bits |= PK_Line;
		else if (EqualsIgnoreCase (k, "arc") || EqualsIgnoreCase (k, "circle") || EqualsIgnoreCase (k, "ellipse")) bits |= PK_Arc;
		else if (EqualsIgnoreCase (k, "polyline"))									bits |= PK_Polyline;
		else if (EqualsIgnoreCase (k, "polygon"))									bits |= PK_Polygon;
		else if (EqualsIgnoreCase (k, "text"))										bits |= PK_Text;
		else if (EqualsIgnoreCase (k, "picture"))									bits |= PK_Picture;
		else Fail ("kinds values must be point, line, arc, polyline, polygon, text or picture.");
	}
	return bits;
}


GS::Array<double> Pt (const API_Coord& c)
{
	GS::Array<double> p;
	p.Push (c.x);
	p.Push (c.y);
	return p;
}


class PrimCollector {
public:
	Int32		maxPrimitives = 300;
	Int32		maxPoints = 20000;
	UInt32		kinds = PK_All;
	bool		summaryOnly = false;
	bool		includeFillPatterns = false;

	GS::Array<OS>					prims;
	std::map<std::string, Int32>	counts;
	GS::Array<GS::UniString>		references;
	Int32		total = 0;
	Int32		pointsUsed = 0;
	bool		truncated = false;
	bool		hasBox = false;
	API_Box		box = {};

	void Handle (const API_PrimElement& p, const void* par1, const void* par2, const void* par3)
	{
		switch (p.header.typeID) {
			case API_PrimCtrl_HatchBorderBegID:		++hatchBorder;		return;
			case API_PrimCtrl_HatchBorderEndID:		--hatchBorder;		return;
			case API_PrimCtrl_HatchLinesBegID:		++hatchLines;		return;
			case API_PrimCtrl_HatchLinesEndID:		--hatchLines;		return;
			case API_PrimCtrl_HoledimLinesBegID:	++holeDim;			return;
			case API_PrimCtrl_HoledimLinesEndID:	--holeDim;			return;
			case API_PrimCtrl_ArrowBegID:			++arrow;			return;
			case API_PrimCtrl_ArrowEndID:			--arrow;			return;
			case API_PrimCtrl_PlacedBorderBegID:	++placedBorder;		return;
			case API_PrimCtrl_PlacedBorderEndID:	--placedBorder;		return;
			case API_PrimCtrl_ElementRefID:
				if (par1 != nullptr) {
					const API_PrimElemRef* ref = reinterpret_cast<const API_PrimElemRef*> (par1);
					GS::UniString g = GuidStr (ref->guid);
					if (ref->guid != APINULLGuid && !references.Contains (g))
						references.Push (g);
				}
				return;
			case API_PrimPointID:	Point (p);						return;
			case API_PrimLineID:	Line (p);						return;
			case API_PrimArcID:		Arc (p);						return;
			case API_PrimTextID:	Text (p, par2);					return;
			case API_PrimPLineID:	Polyline (p, par1, par3);		return;
			case API_PrimTriID:		Triangle (p);					return;
			case API_PrimPolyID:	Polygon (p, par1, par2, par3);	return;
			case API_PrimPictID:	Picture (p);					return;
			default:												return;
		}
	}

private:
	Int32	hatchBorder = 0, hatchLines = 0, holeDim = 0, arrow = 0, placedBorder = 0;

	const char* Role () const
	{
		if (hatchLines > 0)		return "fillPattern";
		if (hatchBorder > 0)	return "fill";
		if (holeDim > 0)		return "openingDimension";
		if (arrow > 0)			return "arrow";
		if (placedBorder > 0)	return "drawingBorder";
		return nullptr;
	}

	void Extend (const API_Coord& c)
	{
		if (!hasBox) {
			box.xMin = box.xMax = c.x;
			box.yMin = box.yMax = c.y;
			hasBox = true;
			return;
		}
		box.xMin = std::min (box.xMin, c.x);
		box.xMax = std::max (box.xMax, c.x);
		box.yMin = std::min (box.yMin, c.y);
		box.yMax = std::max (box.yMax, c.y);
	}

	// Counts the primitive and decides whether it is listed (false = counted only).
	bool Begin (const char* kind, UInt32 kindBit, Int32 nPoints, OS& out, const API_Prim_Head& head)
	{
		const char* role = Role ();
		if (role != nullptr && std::string (role) == "fillPattern" && !includeFillPatterns) {
			++counts["fillPatternLines"];
			return false;
		}
		++total;
		++counts[kind];
		if (summaryOnly || (kinds & kindBit) == 0)
			return false;
		if ((Int32) prims.GetSize () >= maxPrimitives || pointsUsed + nPoints > maxPoints) {
			truncated = true;
			return false;
		}
		pointsUsed += nPoints;
		out.Add ("kind", GS::UniString (kind));
		if (role != nullptr)
			out.Add ("role", GS::UniString (role));
		out.Add ("pen", (Int32) head.pen.penIndex);
		return true;
	}

	void Point (const API_PrimElement& p)
	{
		Extend (p.point.loc);
		OS out;
		if (!Begin ("point", PK_Point, 1, out, p.header))
			return;
		out.Add ("at", Pt (p.point.loc));
		prims.Push (out);
	}

	void Line (const API_PrimElement& p)
	{
		Extend (p.line.c1);
		Extend (p.line.c2);
		OS out;
		if (!Begin ("line", PK_Line, 2, out, p.header))
			return;
		out.Add ("from", Pt (p.line.c1));
		out.Add ("to", Pt (p.line.c2));
		out.Add ("lineType", (Int32) p.line.ltypeInd);
		prims.Push (out);
	}

	void Arc (const API_PrimElement& p)
	{
		const API_PrimArc& a = p.arc;
		const double rx = std::fabs (a.r);
		const double ry = (std::fabs (a.ratio) > 1e-12) ? rx / std::fabs (a.ratio) : rx;
		const double rmax = std::max (rx, ry);
		Extend ({ a.orig.x - rmax, a.orig.y - rmax });
		Extend ({ a.orig.x + rmax, a.orig.y + rmax });
		const bool elliptic = std::fabs (a.ratio - 1.0) > 1e-9 && std::fabs (a.ratio) > 1e-12;
		const char* kind = a.whole ? (elliptic ? "ellipse" : "circle") : (elliptic ? "ellipticArc" : "arc");
		OS out;
		if (!Begin (kind, PK_Arc, 1, out, p.header))
			return;
		out.Add ("center", Pt (a.orig));
		out.Add ("radius", a.r);
		if (elliptic) {
			out.Add ("radiusB", ry);
			AddAngle (out, "axisAngle", a.angle);
		}
		if (!a.whole) {
			AddAngle (out, "beginAngle", a.begAng);
			AddAngle (out, "endAngle", a.endAng);
			if (a.reflected)
				out.Add ("clockwise", true);
		}
		if (a.solid)
			out.Add ("filled", true);
		out.Add ("lineType", (Int32) a.ltypeInd);
		prims.Push (out);
	}

	void Text (const API_PrimElement& p, const void* uniText)
	{
		const API_PrimText& t = p.text;
		Extend (t.loc);
		OS out;
		if (!Begin ("text", PK_Text, 1, out, p.header))
			return;
		GS::UniString content;
		if (uniText != nullptr)
			content = GS::UniString (reinterpret_cast<const GS::UniChar::Layout*> (uniText));
		out.Add ("text", content);
		out.Add ("at", Pt (t.loc));
		if (std::fabs (t.angle) > 1e-12)
			AddAngle (out, "angle", t.angle);
		out.Add ("heightMM", t.heightMM);
		prims.Push (out);
	}

	// Coordinates are 1-based: c[1..n].
	void Polyline (const API_PrimElement& p, const void* coords, const void* arcs)
	{
		const API_PrimPLine& pl = p.pline;
		if (coords == nullptr || pl.nCoords < 1)
			return;
		const API_Coord* c = reinterpret_cast<const API_Coord*> (coords);
		for (Int32 i = 1; i <= pl.nCoords; ++i)
			Extend (c[i]);
		OS out;
		if (!Begin ("polyline", PK_Polyline, pl.nCoords, out, p.header))
			return;
		GS::Array<GS::Array<double>> pts;
		for (Int32 i = 1; i <= pl.nCoords; ++i)
			pts.Push (Pt (c[i]));
		out.Add ("points", pts);
		if (arcs != nullptr && pl.nArcs > 0) {
			const API_PolyArc* a = reinterpret_cast<const API_PolyArc*> (arcs);
			GS::Array<OS> arcList;
			for (Int32 k = 0; k < pl.nArcs; ++k) {
				if (a[k].begIndex >= 1 && a[k].begIndex < pl.nCoords && std::fabs (a[k].arcAngle) > 1e-12)
					arcList.Push (OS ("index", (Int32) (a[k].begIndex - 1), "angle", RadToDeg (a[k].arcAngle)));
			}
			if (!arcList.IsEmpty ())
				out.Add ("arcs", arcList);
		}
		out.Add ("lineType", (Int32) pl.ltypeInd);
		prims.Push (out);
	}

	void Triangle (const API_PrimElement& p)
	{
		for (Int32 i = 0; i < 3; ++i)
			Extend (p.tri.c[i]);
		OS out;
		if (!Begin ("polygon", PK_Polygon, 3, out, p.header))
			return;
		GS::Array<GS::Array<double>> pts;
		for (Int32 i = 0; i < 3; ++i)
			pts.Push (Pt (p.tri.c[i]));
		out.Add ("points", pts);
		if (p.tri.solid)
			out.Add ("filled", true);
		prims.Push (out);
	}

	static GS::Array<GS::Array<double>> ContourPoints (const API_Coord* c, Int32 b, Int32 e)
	{
		// b..e inclusive (1-based); the last vertex repeats the first one when the contour is closed.
		if (e > b && std::fabs (c[e].x - c[b].x) < 1e-9 && std::fabs (c[e].y - c[b].y) < 1e-9)
			--e;
		GS::Array<GS::Array<double>> pts;
		for (Int32 i = b; i <= e; ++i)
			pts.Push (Pt (c[i]));
		return pts;
	}

	void Polygon (const API_PrimElement& p, const void* coords, const void* ends, const void* arcs)
	{
		const API_PrimPoly& pg = p.poly;
		if (coords == nullptr || pg.nCoords < 1)
			return;
		const API_Coord* c = reinterpret_cast<const API_Coord*> (coords);
		for (Int32 i = 1; i <= pg.nCoords; ++i)
			Extend (c[i]);
		OS out;
		if (!Begin ("polygon", PK_Polygon, pg.nCoords, out, p.header))
			return;

		// Contour ranges [b, e] (1-based, inclusive).
		GS::Array<std::pair<Int32, Int32>> ranges;
		const Int32* pe = reinterpret_cast<const Int32*> (ends);
		const Int32 nSub = pg.nSubPolys;
		if (pe != nullptr && nSub > 1) {
			auto valid = [&] (const Int32* list, Int32 count) {
				Int32 prev = 0;
				for (Int32 k = 0; k < count; ++k) {
					if (list[k] <= prev || list[k] > pg.nCoords)
						return false;
					prev = list[k];
				}
				return prev == pg.nCoords;
			};
			const Int32* list = nullptr;
			if (pe[0] == 0 && valid (pe + 1, nSub))
				list = pe + 1;			// memo layout: pends[0] = 0, pends[1..n]
			else if (valid (pe, nSub))
				list = pe;				// plain list of contour ends
			if (list != nullptr) {
				Int32 b = 1;
				for (Int32 k = 0; k < nSub; ++k) {
					ranges.Push ({ b, list[k] });
					b = list[k] + 1;
				}
			}
		}
		if (ranges.IsEmpty ())
			ranges.Push ({ 1, pg.nCoords });

		const API_PolyArc* a = (arcs != nullptr && pg.nArcs > 0) ? reinterpret_cast<const API_PolyArc*> (arcs) : nullptr;
		auto arcsOf = [&] (Int32 b, Int32 e) {
			GS::Array<OS> list;
			if (a == nullptr)
				return list;
			for (Int32 k = 0; k < pg.nArcs; ++k) {
				if (a[k].begIndex >= b && a[k].begIndex < e && std::fabs (a[k].arcAngle) > 1e-12)
					list.Push (OS ("index", (Int32) (a[k].begIndex - b), "angle", RadToDeg (a[k].arcAngle)));
			}
			return list;
		};

		out.Add ("points", ContourPoints (c, ranges[0].first, ranges[0].second));
		GS::Array<OS> outerArcs = arcsOf (ranges[0].first, ranges[0].second);
		if (!outerArcs.IsEmpty ())
			out.Add ("arcs", outerArcs);
		if (ranges.GetSize () > 1) {
			GS::Array<OS> holes;
			for (UIndex k = 1; k < ranges.GetSize (); ++k) {
				OS hole ("points", ContourPoints (c, ranges[k].first, ranges[k].second));
				GS::Array<OS> holeArcs = arcsOf (ranges[k].first, ranges[k].second);
				if (!holeArcs.IsEmpty ())
					hole.Add ("arcs", holeArcs);
				holes.Push (hole);
			}
			out.Add ("holes", holes);
		}
		if (pg.solid)
			out.Add ("filled", true);
		if (pg.fillPen.penIndex > 0)
			out.Add ("fillPen", (Int32) pg.fillPen.penIndex);
		prims.Push (out);
	}

	void Picture (const API_PrimElement& p)
	{
		const API_Box& b = p.pict.destBox;
		Extend ({ b.xMin, b.yMin });
		Extend ({ b.xMax, b.yMax });
		OS out;
		if (!Begin ("picture", PK_Picture, 2, out, p.header))
			return;
		out.Add ("box", BoxObj (b));
		prims.Push (out);
	}
};


PrimCollector* g_primCollector = nullptr;


GSErrCode __ACENV_CALL ShapePrimCallback (const API_PrimElement* prim, const void* par1, const void* par2, const void* par3)
{
	if (g_primCollector == nullptr || prim == nullptr)
		return NoError;
	try {
		g_primCollector->Handle (*prim, par1, par2, par3);
	} catch (...) {
		// never let an exception cross the API boundary
	}
	return NoError;
}


struct CollectorScope {
	explicit CollectorScope (PrimCollector* c)	{ g_primCollector = c; }
	~CollectorScope ()							{ g_primCollector = nullptr; }
};

// =============================================================================
// 3D model
// =============================================================================

struct Mesh3DOptions {
	bool	mesh = false;
	bool	normals = false;
	bool	materials = true;
	Int32	maxVertices = 5000;
	Int32	maxPolygons = 5000;
};


class Model3DReader {
public:
	explicit Model3DReader (const Mesh3DOptions& opt) : opt (opt)
	{
		offset.x = offset.y = 0.0;
		ACAPI_Database (APIDb_GetOffsetID, &offset, nullptr);
	}

	// Per-element accumulators (reset by BeginElement).
	Int32		bodies = 0, vertices = 0, edges = 0, polygons = 0, lights = 0;
	bool		hasBox = false;
	API_Box3D	box = {};
	std::map<Int32, Int32>	materialPolygons;		// umat index -> polygon count
	GS::Array<OS>			bodyList;
	bool		truncated = false;

	void BeginElement ()
	{
		bodies = vertices = edges = polygons = lights = 0;
		hasBox = false;
		box = {};
		materialPolygons.clear ();
		bodyList.Clear ();
		truncated = false;
		usedVertices = usedPolygons = 0;
		seenBodies.Clear ();
	}

	// Reads the 3D bodies of one element (or sub-element). Returns the Get3DInfo error.
	GSErrCode ReadElement (const API_Elem_Head& head)
	{
		API_ElemInfo3D info;
		BNZeroMemory (&info, sizeof (info));
		GSErrCode err = ACAPI_Element_Get3DInfo (head, &info);
		if (err != NoError)
			return err;
		if (info.llight >= info.flight && info.flight > 0)
			lights += info.llight - info.flight + 1;
		if (info.fbody <= 0 || info.lbody < info.fbody)
			return NoError;
		for (Int32 ibody = info.fbody; ibody <= info.lbody; ++ibody) {
			// A hierarchical element and its parts may report the same bodies: count each body once.
			if (seenBodies.Contains (ibody))
				continue;
			seenBodies.Add (ibody);
			ReadBody (ibody, head.guid);
		}
		return NoError;
	}

	GS::Array<OS> MaterialsJson ()
	{
		GS::Array<OS> list;
		for (const auto& kv : materialPolygons) {
			OS m = MaterialInfo (kv.first);
			m.Add ("polygonCount", kv.second);
			list.Push (m);
		}
		return list;
	}

private:
	const Mesh3DOptions&		opt;
	API_Coord					offset;
	Int32						usedVertices = 0, usedPolygons = 0;
	GS::HashTable<Int32, OS>	materialCache;
	GS::HashSet<Int32>			seenBodies;

	API_Coord3D World (const API_Tranmat& tm, double x, double y, double z) const
	{
		API_Coord3D c;
		c.x = tm.tmx[0] * x + tm.tmx[1] * y + tm.tmx[2] * z + tm.tmx[3] + offset.x;
		c.y = tm.tmx[4] * x + tm.tmx[5] * y + tm.tmx[6] * z + tm.tmx[7] + offset.y;
		c.z = tm.tmx[8] * x + tm.tmx[9] * y + tm.tmx[10] * z + tm.tmx[11];
		return c;
	}

	void Extend (const API_Coord3D& c)
	{
		if (!hasBox) {
			box.xMin = box.xMax = c.x;
			box.yMin = box.yMax = c.y;
			box.zMin = box.zMax = c.z;
			hasBox = true;
			return;
		}
		box.xMin = std::min (box.xMin, c.x);	box.xMax = std::max (box.xMax, c.x);
		box.yMin = std::min (box.yMin, c.y);	box.yMax = std::max (box.yMax, c.y);
		box.zMin = std::min (box.zMin, c.z);	box.zMax = std::max (box.zMax, c.z);
	}

	OS MaterialInfo (Int32 iumat)
	{
		if (const OS* cached = materialCache.GetPtr (iumat))
			return *cached;
		OS out ("umatIndex", iumat);
		API_Component3D comp;
		BNZeroMemory (&comp, sizeof (comp));
		comp.header.typeID = API_UmatID;
		comp.header.index = iumat;
		GS::UniString uname;
		comp.umat.mater.head.uniStringNamePtr = &uname;
		GSErrCode err = ACAPI_3D_GetComponent (&comp);
		if (comp.umat.mater.texture.fileLoc != nullptr) {
			delete comp.umat.mater.texture.fileLoc;
			comp.umat.mater.texture.fileLoc = nullptr;
		}
		if (err == NoError) {
			const API_AttributeIndex attrIndex = comp.umat.mater.head.index;
			if (attrIndex > 0) {
				out.Add ("surface", AttrRef (API_MaterialID, attrIndex));
			} else {
				GS::UniString name = uname;
				if (name.IsEmpty ())
					name = GS::UniString (comp.umat.mater.head.name, CC_Default);
				if (!name.IsEmpty ())
					out.Add ("name", name);
				out.Add ("fromGDL", true);
			}
		}
		materialCache.Put (iumat, out);
		return out;
	}

	bool GetComp (API_3DTypeID type, Int32 index, API_Component3D& comp) const
	{
		BNZeroMemory (&comp, sizeof (comp));
		comp.header.typeID = type;
		comp.header.index = index;
		return ACAPI_3D_GetComponent (&comp) == NoError;
	}

	void ReadBody (Int32 ibody, const API_Guid& source)
	{
		API_Component3D comp;
		if (!GetComp (API_BodyID, ibody, comp))
			return;
		const API_BodyType body = comp.body;
		const API_Tranmat tm = body.tranmat;
		const Int32 nVert = std::max (body.nVert, (Int32) 0);
		const Int32 nPgon = std::max (body.nPgon, (Int32) 0);
		++bodies;
		vertices += nVert;
		edges += std::max (body.nEdge, (Int32) 0);
		polygons += nPgon;

		const bool emitMesh = opt.mesh && usedVertices + nVert <= opt.maxVertices && usedPolygons + nPgon <= opt.maxPolygons;
		if (opt.mesh && !emitMesh)
			truncated = true;

		// Vertices (world coordinates).
		GS::Array<GS::Array<double>> verts;
		for (Int32 j = 1; j <= nVert; ++j) {
			if (!GetComp (API_VertID, j, comp))
				continue;
			API_Coord3D w = World (tm, comp.vert.x, comp.vert.y, comp.vert.z);
			Extend (w);
			if (emitMesh) {
				GS::Array<double> v;
				v.Push (w.x); v.Push (w.y); v.Push (w.z);
				verts.Push (v);
			}
		}

		// Polygons (materials + mesh topology).
		GS::Array<OS> pgons;
		if (opt.materials || emitMesh) {
			for (Int32 j = 1; j <= nPgon; ++j) {
				if (!GetComp (API_PgonID, j, comp))
					continue;
				const API_PgonType pgon = comp.pgon;
				++materialPolygons[(Int32) pgon.iumat];
				if (!emitMesh)
					continue;

				GS::Array<GS::Array<Int32>> contours;
				contours.Push (GS::Array<Int32> ());
				for (Int32 k = pgon.fpedg; k <= pgon.lpedg; ++k) {
					if (!GetComp (API_PedgID, k, comp))
						continue;
					const Int32 pedg = comp.pedg.pedg;
					if (pedg == 0) {
						contours.Push (GS::Array<Int32> ());	// start of a hole
						continue;
					}
					if (!GetComp (API_EdgeID, std::abs (pedg), comp))
						continue;
					const Int32 vi = pedg > 0 ? comp.edge.vert1 : comp.edge.vert2;
					if (vi >= 1 && vi <= nVert)
						contours.GetLast ().Push (vi - 1);	// 0-based index into this body's vertices
				}
				OS pj;
				pj.Add ("v", contours[0]);
				if (contours.GetSize () > 1) {
					GS::Array<GS::Array<Int32>> holes;
					for (UIndex h = 1; h < contours.GetSize (); ++h) {
						if (!contours[h].IsEmpty ())
							holes.Push (contours[h]);
					}
					if (!holes.IsEmpty ())
						pj.Add ("holes", holes);
				}
				if (pgon.iumat != body.iumat)
					pj.Add ("material", (Int32) pgon.iumat);
				if ((pgon.status & APIPgon_Invis) != 0)
					pj.Add ("invisible", true);
				if (opt.normals && pgon.ivect != 0 && GetComp (API_VectID, std::abs (pgon.ivect), comp)) {
					double sx = pgon.ivect < 0 ? -1.0 : 1.0;
					const double vx = sx * comp.vect.x, vy = sx * comp.vect.y, vz = sx * comp.vect.z;
					GS::Array<double> n;
					n.Push (tm.tmx[0] * vx + tm.tmx[1] * vy + tm.tmx[2] * vz);
					n.Push (tm.tmx[4] * vx + tm.tmx[5] * vy + tm.tmx[6] * vz);
					n.Push (tm.tmx[8] * vx + tm.tmx[9] * vy + tm.tmx[10] * vz);
					pj.Add ("normal", n);
				}
				pgons.Push (pj);
			}
		}

		if (opt.mesh) {
			OS bj;
			bj.Add ("source", GuidStr (source));
			bj.Add ("bodyIndex", ibody);
			bj.Add ("vertexCount", nVert);
			bj.Add ("polygonCount", nPgon);
			bj.Add ("closed", (body.status & APIBody_Closed) != 0);
			if ((body.status & APIBody_Curved) != 0)
				bj.Add ("curved", true);
			bj.Add ("material", (Int32) body.iumat);
			if (emitMesh) {
				bj.Add ("vertices", verts);
				bj.Add ("polygons", pgons);
				usedVertices += nVert;
				usedPolygons += nPgon;
			} else {
				bj.Add ("omitted", GS::UniString ("vertex/polygon budget exhausted (raise maxVertices / maxPolygons)"));
			}
			bodyList.Push (bj);
		}
	}
};


OS WithGuid (OS result, const API_Guid& guid)
{
	if (result.Contains ("error") && !result.Contains ("guid"))
		result.Add ("guid", GuidStr (guid));
	return result;
}

} // namespace


void RegisterGeometryCommands ()
{
	RegisterCommand ("GetElement2DGeometry",
		"2D drawing primitives of elements as Archicad draws them in the active window (ACAPI_Element_ShapePrims). Input: {elements: "
		"[guid], maxPrimitives (per element, default 300), maxPoints (per element, default 20000), kinds?: [point|line|arc|polyline|"
		"polygon|text|picture], summaryOnly (default false), includeFillPatterns (default false: hatch pattern lines are only counted), "
		"includeHotspots (default false: element hotspots as [x, y, z])}. "
		"Output per element: {guid, type, total, counts: {line: n, ...}, extent: {xMin,yMin,xMax,yMax}, primitives: [{kind, pen, "
		"role?, ...}], truncated?, references?}. Coordinates are [x, y] arrays in meters, angles in degrees.",
		[] (const OS& params) -> OS {
			GS::Array<API_Guid> guids = GetGuidArray (params, "elements", true);
			if (guids.IsEmpty ())
				Fail ("Pass at least one element GUID in 'elements'.");
			const Int32 maxPrims = std::max (std::min (GetInt (params, "maxPrimitives", 300), (Int32) 20000), (Int32) 0);
			const Int32 maxPoints = std::max (std::min (GetInt (params, "maxPoints", 20000), (Int32) 200000), (Int32) 0);
			const UInt32 kinds = ParseKinds (params);
			const bool summaryOnly = GetBool (params, "summaryOnly", false);
			const bool fillPatterns = GetBool (params, "includeFillPatterns", false);
			const bool withHotspots = GetBool (params, "includeHotspots", false);

			GS::Array<OS> results;
			for (const API_Guid& guid : guids) {
				results.Push (WithGuid (Try ([&] () -> OS {
					API_Elem_Head head = GetHeader (guid);
					PrimCollector collector;
					collector.maxPrimitives = maxPrims;
					collector.maxPoints = maxPoints;
					collector.kinds = kinds;
					collector.summaryOnly = summaryOnly;
					collector.includeFillPatterns = fillPatterns;
					GSErrCode err;
					{
						CollectorScope scope (&collector);
						err = ACAPI_Element_ShapePrims (head, ShapePrimCallback);
					}
					if (err != NoError)
						Fail ("Cannot get the 2D drawing of this " + ElemTypeName (head.type) + ": " + ErrorName (err) +
							  ". The element must be drawn in the active window (open its story's floor plan, section or layout).", err);

					OS item;
					item.Add ("guid", GuidStr (guid));
					item.Add ("type", ElemTypeName (head.type));
					item.Add ("total", collector.total);
					OS counts;
					for (const auto& kv : collector.counts)
						counts.Add (GS::String (kv.first.c_str ()), kv.second);
					item.Add ("counts", counts);
					if (collector.hasBox)
						item.Add ("extent", BoxObj (collector.box));
					if (!summaryOnly)
						item.Add ("primitives", collector.prims);
					if (collector.truncated)
						item.Add ("truncated", GS::UniString ("More primitives than maxPrimitives/maxPoints; raise the limits or filter with 'kinds'."));
					if (!collector.references.IsEmpty ())
						item.Add ("references", collector.references);
					if (withHotspots) {
						GS::Array<API_ElementHotspot> hotspots;
						if (ACAPI_Element_GetHotspots (guid, &hotspots) == NoError) {
							GS::Array<GS::Array<double>> pts;
							for (const API_ElementHotspot& h : hotspots) {
								if ((Int32) pts.GetSize () >= maxPoints)
									break;
								GS::Array<double> p;
								p.Push (h.second.x); p.Push (h.second.y); p.Push (h.second.z);
								pts.Push (p);
							}
							item.Add ("hotspotCount", (Int32) hotspots.GetSize ());
							item.Add ("hotspots", pts);
						}
					}
					return item;
				}), guid));
			}
			return OS ("elements", results);
		});

	RegisterCommand ("GetElement3DGeometry",
		"3D model data of elements (ACAPI_Element_Get3DInfo / ACAPI_3D_GetComponent). Input: {elements: [guid], mode: \"summary\" "
		"(default: counts, bounding box, materials) | \"mesh\" (also vertices and polygons), includeSubelements (default true: curtain "
		"wall / stair / railing / beam / column parts), includeMaterials (default true), includeNormals (mesh mode, default false), "
		"maxVertices / maxPolygons (mesh budget per element, default 5000)}. Output per element: {guid, type, bodyCount, vertexCount, "
		"edgeCount, polygonCount, lightCount, boundingBox {xMin..zMax} (world coordinates, z absolute), materials: [{umatIndex, surface|name, "
		"polygonCount}], bodies?: [{source, bodyIndex, closed, material, vertices: [[x,y,z]], polygons: [{v: [vertex indices], holes?, "
		"material?, normal?}]}], parts without 3D?: [...]}.",
		[] (const OS& params) -> OS {
			GS::Array<API_Guid> guids = GetGuidArray (params, "elements", true);
			if (guids.IsEmpty ())
				Fail ("Pass at least one element GUID in 'elements'.");
			Mesh3DOptions opt;
			GS::UniString mode = GetString (params, "mode", "summary");
			if (EqualsIgnoreCase (mode, "mesh"))
				opt.mesh = true;
			else if (!EqualsIgnoreCase (mode, "summary"))
				Fail ("mode must be \"summary\" or \"mesh\".");
			opt.normals = GetBool (params, "includeNormals", false);
			opt.materials = GetBool (params, "includeMaterials", true);
			opt.maxVertices = std::max (std::min (GetInt (params, "maxVertices", 5000), (Int32) 200000), (Int32) 0);
			opt.maxPolygons = std::max (std::min (GetInt (params, "maxPolygons", 5000), (Int32) 200000), (Int32) 0);
			const bool withSubs = GetBool (params, "includeSubelements", true);

			Model3DReader reader (opt);
			GS::Array<OS> results;
			for (const API_Guid& guid : guids) {
				results.Push (WithGuid (Try ([&] () -> OS {
					API_Elem_Head head = GetHeader (guid);
					reader.BeginElement ();

					GS::Array<API_Elem_Head> heads;
					heads.Push (head);
					if (withSubs && IsHierarchicalType (head.type.typeID)) {
						for (const SubElemInfo& s : CollectSubelements (guid, head.type.typeID, false))
							heads.Push (s.head);
					}

					GSErrCode mainErr = NoError;
					Int32 partsWithout3D = 0;
					for (UIndex i = 0; i < heads.GetSize (); ++i) {
						API_Elem_Head h = heads[i];
						if (i > 0) {
							// complete sub-element headers (type/floor) before asking for 3D
							API_Elem_Head full;
							BNZeroMemory (&full, sizeof (full));
							full.guid = h.guid;
							if (ACAPI_Element_GetHeader (&full) == NoError)
								h = full;
						}
						GSErrCode err = reader.ReadElement (h);
						if (err != NoError) {
							if (i == 0) mainErr = err;
							else ++partsWithout3D;
						}
					}
					if (reader.bodies == 0 && mainErr != NoError)
						Fail ("This " + ElemTypeName (head.type) + " has no 3D model available (" + ErrorName (mainErr) + "). 2D-only elements have "
							  "no 3D; for 3D elements check that the layer is visible and the element is shown in 3D (3D filters / Show "
							  "All in 3D).", mainErr);

					OS item;
					item.Add ("guid", GuidStr (guid));
					item.Add ("type", ElemTypeName (head.type));
					item.Add ("bodyCount", reader.bodies);
					item.Add ("vertexCount", reader.vertices);
					item.Add ("edgeCount", reader.edges);
					item.Add ("polygonCount", reader.polygons);
					if (reader.lights > 0)
						item.Add ("lightCount", reader.lights);
					if (reader.hasBox)
						item.Add ("boundingBox", Box3DObj (reader.box));
					if (heads.GetSize () > 1)
						item.Add ("partsScanned", (Int32) heads.GetSize () - 1);
					if (partsWithout3D > 0)
						item.Add ("partsWithout3D", partsWithout3D);
					if (opt.materials)
						item.Add ("materials", reader.MaterialsJson ());
					if (opt.mesh)
						item.Add ("bodies", reader.bodyList);
					if (reader.truncated)
						item.Add ("truncated", GS::UniString ("Some bodies were omitted: raise maxVertices / maxPolygons or query fewer elements."));
					return item;
				}), guid));
			}
			return OS ("elements", results);
		});
}

} // namespace eq
} // namespace cc
