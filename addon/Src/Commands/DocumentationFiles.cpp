// *****************************************************************************
// DocumentationFiles — file writers the Archicad 26 API does not offer natively
// (family "documentation").
//
//   ExportDxf        {path (.dxf), view? | layout? | database? | storyIndex?, elements?, units?, fillPatterns?,
//                     fillBoundaries?, includeMasterLayout?, maxEntities?, restoreWindow?, overwrite?, createFolders?}
//                    DXF (AutoCAD R12 ASCII) of a 2D window, written from the drawing primitives Archicad
//                    generates for every visible element (ACAPI_Element_ShapePrims) — so it looks like the view.
//   Export3DModel    {path (.obj | .stl | .gsm), format?, elements? | useSelection? | source?, units?, upAxis?,
//                     materials?, binary?, includeInvisible?, includeSubelements?, overwrite?, createFolders?}
//                    Wavefront OBJ (+ MTL) / STL from the 3D model (ACAPI_3D_GetComponent), or a GDL object
//                    (.gsm) of the 3D window through Archicad's own "Save as Object".
// *****************************************************************************

#include "Commands/DocumentationShared.hpp"
#include "Commands/ElementQueryShared.hpp"		// eq::IsHierarchicalType / eq::CollectSubelements (read-only reuse)

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace cc {
namespace doc {

namespace {

namespace fs = std::filesystem;

// =============================================================================
// Shared helpers
// =============================================================================

fs::path FsPathOf (const GS::UniString& path)
{
	return fs::u8path (Utf8 (path));
}


std::string Num (double v)
{
	if (!std::isfinite (v))
		v = 0.0;
	char buf[64];
	std::snprintf (buf, sizeof (buf), "%.6f", v);
	char* end = buf + std::strlen (buf) - 1;
	if (std::strchr (buf, '.') != nullptr) {
		while (end > buf && *end == '0')
			*end-- = 0;
		if (*end == '.')
			*end = 0;
	}
	if (std::strcmp (buf, "-0") == 0)
		return "0";
	return buf;
}


struct UnitInfo {
	double	scale;			// meters -> file units
	Int32	insUnits;		// DXF $INSUNITS
	const char* name;
};


UnitInfo ParseUnits (const OS& params, const char* def)
{
	const GS::UniString u = GetString (params, "units", def);
	if (EqualsIgnoreCase (u, "mm"))		return { 1000.0, 4, "mm" };
	if (EqualsIgnoreCase (u, "cm"))		return { 100.0, 5, "cm" };
	if (EqualsIgnoreCase (u, "m"))		return { 1.0, 6, "m" };
	if (EqualsIgnoreCase (u, "in"))		return { 1.0 / 0.0254, 1, "in" };
	if (EqualsIgnoreCase (u, "ft"))		return { 1.0 / 0.3048, 2, "ft" };
	Fail ("'units' must be mm, cm, m, in or ft.");
}


bool Is2DWindow (API_WindowTypeID t)
{
	switch (t) {
		case APIWind_FloorPlanID:
		case APIWind_SectionID:
		case APIWind_DetailID:
		case APIWind_ElevationID:
		case APIWind_InteriorElevationID:
		case APIWind_WorksheetID:
		case APIWind_DocumentFrom3DID:
		case APIWind_LayoutID:
		case APIWind_MasterLayoutID:
			return true;
		default:
			return false;
	}
}


// Drawing scale denominator of the current database (100 for 1:100); def when unknown.
double CurrentScaleDenominator (double def)
{
	double scale = 0.0;
	if (ACAPI_Database (APIDb_GetDrawingScaleID, &scale, nullptr) != NoError || !(scale > 0.0))
		return def;
	return scale < 1.0 ? 1.0 / scale : scale;
}


// Contour ranges [b, e] (1-based, inclusive) of a primitive polygon; pends may be memo layout or a plain list.
std::vector<std::pair<Int32, Int32>> ContourRanges (Int32 nCoords, Int32 nSubPolys, const Int32* pe)
{
	std::vector<std::pair<Int32, Int32>> ranges;
	if (pe != nullptr && nSubPolys > 1) {
		auto valid = [&] (const Int32* list, Int32 count) {
			Int32 prev = 0;
			for (Int32 k = 0; k < count; ++k) {
				if (list[k] <= prev || list[k] > nCoords)
					return false;
				prev = list[k];
			}
			return prev == nCoords;
		};
		const Int32* list = nullptr;
		if (pe[0] == 0 && valid (pe + 1, nSubPolys))
			list = pe + 1;
		else if (valid (pe, nSubPolys))
			list = pe;
		if (list != nullptr) {
			Int32 b = 1;
			for (Int32 k = 0; k < nSubPolys; ++k) {
				ranges.push_back ({ b, list[k] });
				b = list[k] + 1;
			}
		}
	}
	if (ranges.empty ())
		ranges.push_back ({ 1, nCoords });
	return ranges;
}

// =============================================================================
// DXF writer (R12 / AC1009 ASCII)
// =============================================================================

std::string DxfString (const GS::UniString& s, bool symbolName)
{
	std::string out;
	for (UIndex i = 0; i < s.GetLength (); ++i) {
		const GS::UniChar::Layout c = GS::UniChar (s[i]);
		if (c == '\r' || c == '\n' || c == '\t') {
			out += ' ';
			continue;
		}
		if (c < 32)
			continue;
		if (symbolName && c < 128 && std::strchr ("<>/\\\":;?*|=,`", (int) c) != nullptr) {
			out += '_';
			continue;
		}
		if (c < 128) {
			out += (char) c;
		} else {
			char buf[16];
			std::snprintf (buf, sizeof (buf), "\\U+%04X", (unsigned) c);
			out += buf;
		}
	}
	if (symbolName) {
		while (!out.empty () && out.back () == ' ')
			out.pop_back ();
		if (out.empty ())
			out = "0";
		if (out.size () > 255)
			out.resize (255);
	}
	return out;
}


class DxfWriter {
public:
	double	unit = 1000.0;			// meters -> file units
	double	textScale = 100.0;		// paper mm -> model mm factor (scale denominator; 1 on layouts)
	bool	fillPatterns = true;
	bool	fillBoundaries = false;
	Int64	maxEntities = 3000000;

	std::string							body;
	std::map<std::string, Int32>		counts;
	std::vector<std::string>			layers;
	Int64	entities = 0;
	bool	truncated = false;
	bool	hasBox = false;
	double	xMin = 0.0, yMin = 0.0, xMax = 0.0, yMax = 0.0;		// file units

	void BeginElement ()
	{
		hatchBorder = hatchLines = 0;
	}

	void Handle (const API_PrimElement& p, const void* par1, const void* par2, const void* par3)
	{
		switch (p.header.typeID) {
			case API_PrimCtrl_HatchBorderBegID:		++hatchBorder;		return;
			case API_PrimCtrl_HatchBorderEndID:		if (hatchBorder > 0) --hatchBorder;	return;
			case API_PrimCtrl_HatchLinesBegID:		++hatchLines;		return;
			case API_PrimCtrl_HatchLinesEndID:		if (hatchLines > 0) --hatchLines;	return;
			case API_PrimLineID:	Line (p);						return;
			case API_PrimArcID:		Arc (p);						return;
			case API_PrimTextID:	Text (p, par2);					return;
			case API_PrimPLineID:	Polyline (p, par1, par3);		return;
			case API_PrimTriID:		Triangle (p);					return;
			case API_PrimPolyID:	Polygon (p, par1, par2, par3);	return;
			case API_PrimPictID:	++counts["skippedPictures"];	return;
			case API_PrimPointID:	++counts["skippedPoints"];		return;
			default:												return;
		}
	}

	// Writes the complete DXF (header, tables, the collected entities) without copying the entity buffer.
	void WriteTo (std::ostream& f, Int32 insUnits) const
	{
		std::string out;
		out.reserve (4096 + layers.size () * 64);
		auto g = [&] (int code, const std::string& v) {
			out += std::to_string (code);
			out += '\n';
			out += v;
			out += '\n';
		};
		g (999, "Exported from Archicad by the Claude Connector add-on");
		g (0, "SECTION");	g (2, "HEADER");
		g (9, "$ACADVER");	g (1, "AC1009");
		g (9, "$DWGCODEPAGE");	g (3, "ANSI_1252");
		g (9, "$INSUNITS");	g (70, std::to_string (insUnits));
		g (9, "$EXTMIN");	g (10, Num (hasBox ? xMin : 0.0));	g (20, Num (hasBox ? yMin : 0.0));	g (30, "0");
		g (9, "$EXTMAX");	g (10, Num (hasBox ? xMax : 0.0));	g (20, Num (hasBox ? yMax : 0.0));	g (30, "0");
		g (0, "ENDSEC");

		g (0, "SECTION");	g (2, "TABLES");
		g (0, "TABLE");		g (2, "LTYPE");		g (70, "1");
		g (0, "LTYPE");		g (2, "CONTINUOUS");	g (70, "0");	g (3, "Solid line");	g (72, "65");	g (73, "0");	g (40, "0");
		g (0, "ENDTAB");
		g (0, "TABLE");		g (2, "LAYER");		g (70, std::to_string (layers.size () + 1));
		g (0, "LAYER");		g (2, "0");			g (70, "0");	g (62, "7");	g (6, "CONTINUOUS");
		for (const std::string& l : layers) {
			if (l == "0")
				continue;
			g (0, "LAYER");	g (2, l);	g (70, "0");	g (62, "7");	g (6, "CONTINUOUS");
		}
		g (0, "ENDTAB");
		g (0, "TABLE");		g (2, "STYLE");		g (70, "1");
		g (0, "STYLE");		g (2, "STANDARD");	g (70, "0");	g (40, "0");	g (41, "1");	g (50, "0");	g (71, "0");
		g (42, "2.5");		g (3, "txt");		g (4, "");
		g (0, "ENDTAB");
		g (0, "ENDSEC");
		g (0, "SECTION");	g (2, "ENTITIES");
		f.write (out.data (), (std::streamsize) out.size ());
		f.write (body.data (), (std::streamsize) body.size ());
		static const char kFooter[] = "0\nENDSEC\n0\nEOF\n";
		f.write (kFooter, (std::streamsize) (sizeof (kFooter) - 1));
	}

private:
	Int32	hatchBorder = 0;
	Int32	hatchLines = 0;
	std::map<Int32, std::string>	layerCache;
	std::set<std::string>			layerSet;

	const std::string& LayerName (API_AttributeIndex index)
	{
		auto it = layerCache.find ((Int32) index);
		if (it != layerCache.end ())
			return it->second;
		std::string name = "0";
		if (index > 0) {
			const GS::UniString n = AttrName (API_LayerID, index);
			if (!n.IsEmpty ())
				name = DxfString (n, true);
		}
		std::string upper = name;
		std::transform (upper.begin (), upper.end (), upper.begin (), [] (unsigned char ch) { return (char) std::toupper (ch); });
		if (layerSet.insert (upper).second)
			layers.push_back (name);
		return layerCache.emplace ((Int32) index, name).first->second;
	}

	static int Color (short penIndex)
	{
		return (penIndex >= 1 && penIndex <= 255) ? penIndex : 7;
	}

	bool Room ()
	{
		if (entities >= maxEntities) {
			truncated = true;
			return false;
		}
		++entities;
		return true;
	}

	void G (int code, const std::string& v)
	{
		body += std::to_string (code);
		body += '\n';
		body += v;
		body += '\n';
	}

	void GN (int code, double v)		{ G (code, Num (v)); }
	void GI (int code, int v)			{ G (code, std::to_string (v)); }

	void Extend (double x, double y)
	{
		if (!hasBox) {
			xMin = xMax = x;
			yMin = yMax = y;
			hasBox = true;
			return;
		}
		xMin = std::min (xMin, x);	xMax = std::max (xMax, x);
		yMin = std::min (yMin, y);	yMax = std::max (yMax, y);
	}

	// Point in meters -> group codes code / code+10 / code+20 in file units.
	void XY (int code, double x, double y)
	{
		const double fx = x * unit, fy = y * unit;
		Extend (fx, fy);
		GN (code, fx);
		GN (code + 10, fy);
		G (code + 20, "0");
	}

	void Head (const char* type, const API_Prim_Head& h, short penOverride = 0)
	{
		G (0, type);
		G (8, LayerName (h.layer));
		GI (62, Color (penOverride > 0 ? penOverride : h.pen.penIndex));
	}

	bool SkipAsFillPattern ()
	{
		if (hatchLines > 0 && !fillPatterns) {
			++counts["skippedFillPatternPrimitives"];
			return true;
		}
		return false;
	}

	bool SkipAsFillArea (bool solid)
	{
		if ((hatchBorder > 0 || solid) && !fillBoundaries) {
			++counts["skippedFillAreas"];
			return true;
		}
		return false;
	}

	void EmitPolyline (const API_Prim_Head& h, const std::vector<API_Coord>& pts, const std::vector<double>& bulges, bool closed, short pen = 0)
	{
		if (pts.size () < 2 || !Room ())
			return;
		Head ("POLYLINE", h, pen);
		GI (66, 1);
		G (10, "0");	G (20, "0");	G (30, "0");
		GI (70, closed ? 1 : 0);
		const std::string& layer = LayerName (h.layer);
		for (size_t i = 0; i < pts.size (); ++i) {
			G (0, "VERTEX");
			G (8, layer);
			XY (10, pts[i].x, pts[i].y);
			if (i < bulges.size () && std::fabs (bulges[i]) > 1e-12)
				GN (42, bulges[i]);
		}
		G (0, "SEQEND");
		G (8, layer);
		++counts["polyline"];
	}

	void Line (const API_PrimElement& p)
	{
		if (SkipAsFillPattern () || !Room ())
			return;
		Head ("LINE", p.header);
		XY (10, p.line.c1.x, p.line.c1.y);
		XY (11, p.line.c2.x, p.line.c2.y);
		++counts["line"];
	}

	void Arc (const API_PrimElement& p)
	{
		if (SkipAsFillPattern ())
			return;
		const API_PrimArc& a = p.arc;
		const double r = std::fabs (a.r);
		if (r < 1e-12)
			return;
		const bool elliptic = std::fabs (a.ratio) > 1e-12 && std::fabs (a.ratio - 1.0) > 1e-9;
		if (!elliptic) {
			if (!Room ())
				return;
			if (a.whole) {
				Head ("CIRCLE", p.header);
				XY (10, a.orig.x, a.orig.y);
				GN (40, r * unit);
				++counts["circle"];
			} else {
				double b = a.begAng, e = a.endAng;
				if (a.reflected)
					std::swap (b, e);
				Head ("ARC", p.header);
				XY (10, a.orig.x, a.orig.y);
				GN (40, r * unit);
				GN (50, RadToDeg (b));
				GN (51, RadToDeg (e));
				++counts["arc"];
			}
			return;
		}
		// Ellipse / elliptic arc -> polyline approximation (R12 has no ELLIPSE).
		const double ra = r, rb = r / std::fabs (a.ratio);
		double t0 = 0.0, sweep = 2.0 * kPi;
		if (!a.whole) {
			t0 = a.begAng;
			sweep = a.endAng - a.begAng;
			if (!a.reflected) {
				while (sweep <= 0.0) sweep += 2.0 * kPi;
			} else {
				while (sweep >= 0.0) sweep -= 2.0 * kPi;
			}
		}
		const Int32 n = std::max<Int32> (8, (Int32) std::ceil (std::fabs (sweep) / (2.0 * kPi) * 72.0));
		const double ca = std::cos (a.angle), sa = std::sin (a.angle);
		std::vector<API_Coord> pts;
		for (Int32 k = 0; k <= n; ++k) {
			if (a.whole && k == n)
				break;
			const double t = t0 + sweep * k / n;
			const double lx = ra * std::cos (t), ly = rb * std::sin (t);
			pts.push_back ({ a.orig.x + lx * ca - ly * sa, a.orig.y + lx * sa + ly * ca });
		}
		EmitPolyline (p.header, pts, {}, a.whole);
		++counts["ellipseApproximated"];
	}

	void Text (const API_PrimElement& p, const void* uniText)
	{
		if (uniText == nullptr)
			return;
		const GS::UniString content (reinterpret_cast<const GS::UniChar::Layout*> (uniText));
		GS::UniString trimmed = content;
		trimmed.Trim ();
		if (trimmed.IsEmpty () || !Room ())
			return;
		const API_PrimText& t = p.text;
		const double height = t.heightMM / 1000.0 * textScale / 1000.0 * unit;		// paper mm -> model m -> file units
		Head ("TEXT", p.header);
		XY (10, t.loc.x, t.loc.y);
		GN (40, height > 0.0 ? height : 1.0);
		G (1, DxfString (content, false));
		if (std::fabs (t.angle) > 1e-12)
			GN (50, RadToDeg (t.angle));
		if (t.widthFactor > 1e-6 && std::fabs (t.widthFactor - 1.0) > 1e-6)
			GN (41, t.widthFactor);
		G (7, "STANDARD");
		++counts["text"];
	}

	void Polyline (const API_PrimElement& p, const void* coords, const void* arcs)
	{
		if (SkipAsFillPattern ())
			return;
		const API_PrimPLine& pl = p.pline;
		if (coords == nullptr || pl.nCoords < 2)
			return;
		const API_Coord* c = reinterpret_cast<const API_Coord*> (coords);
		std::vector<API_Coord> pts;
		for (Int32 i = 1; i <= pl.nCoords; ++i)
			pts.push_back (c[i]);
		std::vector<double> bulges (pts.size (), 0.0);
		if (arcs != nullptr && pl.nArcs > 0) {
			const API_PolyArc* a = reinterpret_cast<const API_PolyArc*> (arcs);
			for (Int32 k = 0; k < pl.nArcs; ++k) {
				if (a[k].begIndex >= 1 && a[k].begIndex < pl.nCoords)
					bulges[a[k].begIndex - 1] = std::tan (a[k].arcAngle / 4.0);
			}
		}
		EmitPolyline (p.header, pts, bulges, false);
	}

	void Triangle (const API_PrimElement& p)
	{
		if (SkipAsFillPattern () || SkipAsFillArea (p.tri.solid))
			return;
		if (p.tri.solid) {
			if (!Room ())
				return;
			Head ("SOLID", p.header);
			XY (10, p.tri.c[0].x, p.tri.c[0].y);
			XY (11, p.tri.c[1].x, p.tri.c[1].y);
			XY (12, p.tri.c[2].x, p.tri.c[2].y);
			XY (13, p.tri.c[2].x, p.tri.c[2].y);
			++counts["solid"];
			return;
		}
		std::vector<API_Coord> pts = { p.tri.c[0], p.tri.c[1], p.tri.c[2] };
		EmitPolyline (p.header, pts, {}, true);
	}

	void Polygon (const API_PrimElement& p, const void* coords, const void* ends, const void* arcs)
	{
		const API_PrimPoly& pg = p.poly;
		if (p.header.pen.penIndex < 0)		// background-colored mask polygon
			return;
		if (SkipAsFillPattern () || SkipAsFillArea (pg.solid))
			return;
		if (coords == nullptr || pg.nCoords < 3)
			return;
		const API_Coord* c = reinterpret_cast<const API_Coord*> (coords);
		const API_PolyArc* a = (arcs != nullptr && pg.nArcs > 0) ? reinterpret_cast<const API_PolyArc*> (arcs) : nullptr;
		const short pen = (pg.solid && pg.fillPen.penIndex > 0) ? pg.fillPen.penIndex : 0;
		for (const auto& range : ContourRanges (pg.nCoords, pg.nSubPolys, reinterpret_cast<const Int32*> (ends))) {
			Int32 b = range.first, e = range.second;
			if (e > b && std::fabs (c[e].x - c[b].x) < 1e-9 && std::fabs (c[e].y - c[b].y) < 1e-9)
				--e;		// closed contours repeat their first vertex
			if (e - b + 1 < 2)
				continue;
			std::vector<API_Coord> pts;
			for (Int32 i = b; i <= e; ++i)
				pts.push_back (c[i]);
			std::vector<double> bulges (pts.size (), 0.0);
			if (a != nullptr) {
				for (Int32 k = 0; k < pg.nArcs; ++k) {
					if (a[k].begIndex >= b && a[k].begIndex <= e)
						bulges[a[k].begIndex - b] = std::tan (a[k].arcAngle / 4.0);
				}
			}
			EmitPolyline (p.header, pts, bulges, true, pen);
		}
	}
};


DxfWriter* g_dxfWriter = nullptr;


GSErrCode __ACENV_CALL DxfPrimCallback (const API_PrimElement* prim, const void* par1, const void* par2, const void* par3)
{
	if (g_dxfWriter == nullptr || prim == nullptr)
		return NoError;
	try {
		g_dxfWriter->Handle (*prim, par1, par2, par3);
	} catch (...) {
		// never let an exception cross the API boundary
	}
	return NoError;
}


struct DxfWriterScope {
	explicit DxfWriterScope (DxfWriter* w)	{ g_dxfWriter = w; }
	~DxfWriterScope ()						{ g_dxfWriter = nullptr; }
	DxfWriterScope (const DxfWriterScope&) = delete;
	DxfWriterScope& operator= (const DxfWriterScope&) = delete;
};


// Elements of the current database in drawing order that pass the filter.
GS::Array<API_Elem_Head> DrawOrderHeadsFiltered (API_ElemFilterFlags filter)
{
	GS::Array<API_Elem_Head> heads;
	if (ACAPI_Database (APIDb_DrawOrderInitID, nullptr, nullptr) != NoError)
		return heads;
	API_Elem_Head head;
	BNZeroMemory (&head, sizeof (head));
	while (ACAPI_Database (APIDb_DrawOrderGetNextID, reinterpret_cast<void*> ((GS::IntPtr) filter), &head) == NoError) {
		heads.Push (head);
		if (heads.GetSize () > 5000000)
			break;
	}
	ACAPI_Database (APIDb_DrawOrderTermID, nullptr, nullptr);
	return heads;
}


// Visible elements of the current database in drawing order: visible layers, renovation filter, partial
// structure display (+ the active story on plans). Falls back to layers (+ story) only when the extended
// filter yields nothing (some filter bits may not be supported by the draw-order iterator).
GS::Array<API_Elem_Head> DrawOrderHeads (bool floorPlan)
{
	const API_ElemFilterFlags basic = APIFilt_OnVisLayer | (floorPlan ? APIFilt_OnActFloor : 0);
	GS::Array<API_Elem_Head> heads = DrawOrderHeadsFiltered (basic | APIFilt_IsVisibleByRenovation | APIFilt_IsInStructureDisplay);
	if (heads.IsEmpty ())
		heads = DrawOrderHeadsFiltered (basic);
	return heads;
}


struct DrawStats {
	Int32	elements = 0;
	Int32	failed = 0;
};


void DrawHeads (DxfWriter& writer, const GS::Array<API_Elem_Head>& heads, DrawStats& stats)
{
	DxfWriterScope scope (&writer);
	for (const API_Elem_Head& h : heads) {
		if (writer.truncated)
			break;
		writer.BeginElement ();
		const GSErrCode err = ACAPI_Element_ShapePrims (h, DxfPrimCallback);
		if (err == NoError)
			++stats.elements;
		else
			++stats.failed;
	}
}


class MasterLayoutGuard {
public:
	explicit MasterLayoutGuard (const API_DatabaseUnId& layout)
	{
		API_DatabaseUnId id = layout;
		active = ACAPI_Goodies (APIAny_SetMasterLayoutOnLayoutID, &id, nullptr) == NoError;
	}
	~MasterLayoutGuard ()
	{
		if (active)
			ACAPI_Goodies (APIAny_SetMasterLayoutOnLayoutID, nullptr, nullptr);
	}
	MasterLayoutGuard (const MasterLayoutGuard&) = delete;
	MasterLayoutGuard& operator= (const MasterLayoutGuard&) = delete;
private:
	bool active = false;
};


void WriteDxfOrFail (const GS::UniString& path, const DxfWriter& writer, Int32 insUnits)
{
	std::ofstream f (FsPathOf (path), std::ios::binary | std::ios::trunc);
	if (!f)
		Fail ("Cannot open '" + path + "' for writing.", APIERR_GENERAL);
	writer.WriteTo (f, insUnits);
	f.close ();
	if (!f)
		Fail ("Writing '" + path + "' failed (disk full or no permission?).", APIERR_GENERAL);
}


OS ExportDxf (const OS& params)
{
	GS::Array<GS::UniString> exts;
	exts.Push ("dxf");
	const GS::UniString path = PrepareOutputPath (params, "path", exts, "dxf");
	const UnitInfo units = ParseUnits (params, "mm");
	const GS::Array<API_Guid> only = GetGuidArray (params, "elements", false);

	WindowRestorer restorer;
	restorer.Arm (GetBool (params, "restoreWindow", true));
	TargetWindow tw = OpenTargetWindow (params);
	const API_WindowTypeID wt = tw.window.typeID;
	if (!Is2DWindow (wt)) {
		Fail ("DXF export needs a 2D window (floor plan, section, elevation, detail, worksheet, 3D document or layout); the target is a " +
			  DbTypeName (wt) + ". For 3D use export_3d_model.", APIERR_BADWINDOW);
	}
	API_DatabaseInfo db = tw.window;
	DatabaseScope scope (db);
	const bool isLayout = wt == APIWind_LayoutID || wt == APIWind_MasterLayoutID;

	DxfWriter writer;
	writer.unit = units.scale;
	writer.textScale = isLayout ? 1.0 : CurrentScaleDenominator (100.0);
	writer.fillPatterns = GetBool (params, "fillPatterns", true);
	writer.fillBoundaries = GetBool (params, "fillBoundaries", false);
	writer.maxEntities = (Int64) std::max<Int32> (1000, std::min<Int32> (GetInt (params, "maxEntities", 3000000), 20000000));

	DrawStats stats;
	GS::Array<OS> notFound;
	if (!only.IsEmpty ()) {
		GS::Array<API_Elem_Head> heads;
		for (const API_Guid& g : only) {
			API_Elem_Head h;
			BNZeroMemory (&h, sizeof (h));
			h.guid = g;
			if (ACAPI_Element_GetHeader (&h) == NoError)
				heads.Push (h);
			else
				notFound.Push (OS ("guid", GuidStr (g), "error", GS::UniString ("not found in the exported window's database")));
		}
		DrawHeads (writer, heads, stats);
	} else {
		// Master layout below / above the layout content, like Archicad draws it.
		API_DatabaseInfo master;
		BNZeroMemory (&master, sizeof (master));
		bool withMaster = false, masterBelow = true;
		if (wt == APIWind_LayoutID && GetBool (params, "includeMasterLayout", true)) {
			API_DatabaseInfo cur = CurrentDatabase ();
			if (cur.masterLayoutUnId.elemSetId != APINULLGuid) {
				master.typeID = APIWind_MasterLayoutID;
				master.databaseUnId = cur.masterLayoutUnId;
				API_LayoutInfo li;
				BNZeroMemory (&li, sizeof (li));
				API_DatabaseUnId mid = master.databaseUnId;
				if (ACAPI_Environment (APIEnv_GetLayoutSetsID, &li, &mid, nullptr) == NoError)
					masterBelow = li.showMasterBelow;
				delete li.customData;
				withMaster = true;
			}
		}
		auto drawMaster = [&] () {
			OS r = Try ([&] () -> OS {
				DatabaseScope ms (master);
				ACAPI_Database (APIDb_RebuildCurrentDatabaseID, nullptr, nullptr);
				MasterLayoutGuard guard (db.databaseUnId);
				DrawHeads (writer, DrawOrderHeads (false), stats);
				return OS ();
			});
			if (r.Contains ("error"))
				++writer.counts["masterLayoutErrors"];
		};
		if (withMaster && masterBelow)
			drawMaster ();
		DrawHeads (writer, DrawOrderHeads (wt == APIWind_FloorPlanID), stats);
		if (withMaster && !masterBelow)
			drawMaster ();
	}

	if (writer.entities == 0 && stats.elements == 0) {
		Fail ("Nothing to export: no visible element in " + tw.description + (only.IsEmpty () ? GS::UniString () :
			  GS::UniString (" among the given elements (they must be in the exported window's database)")) +
			  ". Check layer visibility / the story, or pass another view.", APIERR_BADPARS);
	}

	WriteDxfOrFail (path, writer, units.insUnits);
	restorer.Finish ();

	OS out;
	out.Add ("file", FileJson (path));
	out.Add ("format", GS::UniString ("DXF R12 (AC1009)"));
	out.Add ("exported", tw.description);
	out.Add ("target", tw.json);
	out.Add ("units", GS::UniString (units.name));
	if (!isLayout)
		out.Add ("drawingScale", writer.textScale);
	out.Add ("elementCount", stats.elements);
	if (stats.failed > 0)
		out.Add ("elementsWithoutDrawing", stats.failed);
	out.Add ("entityCount", (double) writer.entities);
	OS counts;
	for (const auto& kv : writer.counts)
		counts.Add (GS::String (kv.first.c_str ()), kv.second);
	out.Add ("counts", counts);
	out.Add ("layerCount", (Int32) writer.layers.size ());
	if (writer.hasBox) {
		OS ext;
		ext.Add ("xMin", writer.xMin);	ext.Add ("yMin", writer.yMin);
		ext.Add ("xMax", writer.xMax);	ext.Add ("yMax", writer.yMax);
		out.Add ("extentInFileUnits", ext);
	}
	if (writer.truncated)
		out.Add ("warning", GS::UniString ("maxEntities reached: the file is incomplete. Raise maxEntities or export fewer elements."));
	if (!notFound.IsEmpty ())
		out.Add ("notExported", notFound);
	return out;
}

// =============================================================================
// Triangulation (ear clipping with hole bridging) for 3D polygons
// =============================================================================

struct V2 { double x, y; };

double Cross (const V2& o, const V2& a, const V2& b)
{
	return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}


double Area2 (const std::vector<Int32>& ring, const std::vector<V2>& p)
{
	double a = 0.0;
	for (size_t i = 0, n = ring.size (); i < n; ++i) {
		const V2& u = p[ring[i]];
		const V2& v = p[ring[(i + 1) % n]];
		a += u.x * v.y - v.x * u.y;
	}
	return a;
}


bool SegmentsCross (const V2& a, const V2& b, const V2& c, const V2& d)
{
	const double d1 = Cross (c, d, a), d2 = Cross (c, d, b), d3 = Cross (a, b, c), d4 = Cross (a, b, d);
	return ((d1 > 1e-12 && d2 < -1e-12) || (d1 < -1e-12 && d2 > 1e-12)) &&
		   ((d3 > 1e-12 && d4 < -1e-12) || (d3 < -1e-12 && d4 > 1e-12));
}


bool Same (const V2& a, const V2& b)
{
	return std::fabs (a.x - b.x) < 1e-9 && std::fabs (a.y - b.y) < 1e-9;
}


// rings[0] = outer, rest = holes; indices into pts (2D projection). Output triangles CCW in the projection.
std::vector<std::array<Int32, 3>> Triangulate (std::vector<std::vector<Int32>> rings, const std::vector<V2>& pts)
{
	std::vector<std::array<Int32, 3>> tris;
	if (rings.empty () || rings[0].size () < 3)
		return tris;
	if (Area2 (rings[0], pts) < 0.0)
		std::reverse (rings[0].begin (), rings[0].end ());
	std::vector<std::vector<Int32>> holes;
	for (size_t h = 1; h < rings.size (); ++h) {
		if (rings[h].size () < 3)
			continue;
		if (Area2 (rings[h], pts) > 0.0)
			std::reverse (rings[h].begin (), rings[h].end ());
		holes.push_back (rings[h]);
	}

	// Bridge holes into the outer ring, rightmost hole first.
	std::vector<Int32> poly = rings[0];
	auto maxX = [&] (const std::vector<Int32>& r) {
		size_t best = 0;
		for (size_t i = 1; i < r.size (); ++i)
			if (pts[r[i]].x > pts[r[best]].x)
				best = i;
		return best;
	};
	std::sort (holes.begin (), holes.end (), [&] (const std::vector<Int32>& a, const std::vector<Int32>& b) {
		return pts[a[maxX (a)]].x > pts[b[maxX (b)]].x;
	});
	for (size_t hi = 0; hi < holes.size (); ++hi) {
		const std::vector<Int32>& hole = holes[hi];
		const size_t mi = maxX (hole);
		const V2& m = pts[hole[mi]];
		// Candidate bridge vertices sorted by distance.
		std::vector<size_t> order (poly.size ());
		for (size_t i = 0; i < poly.size (); ++i)
			order[i] = i;
		std::sort (order.begin (), order.end (), [&] (size_t a, size_t b) {
			const V2& pa = pts[poly[a]];
			const V2& pb = pts[poly[b]];
			return (pa.x - m.x) * (pa.x - m.x) + (pa.y - m.y) * (pa.y - m.y) < (pb.x - m.x) * (pb.x - m.x) + (pb.y - m.y) * (pb.y - m.y);
		});
		size_t bridge = poly.size ();
		for (size_t oi = 0; oi < order.size () && bridge == poly.size (); ++oi) {
			const size_t i = order[oi];
			const V2& p = pts[poly[i]];
			bool ok = true;
			for (size_t k = 0, n = poly.size (); k < n && ok; ++k) {
				const V2& a = pts[poly[k]];
				const V2& b = pts[poly[(k + 1) % n]];
				if (Same (a, p) || Same (b, p))
					continue;
				ok = !SegmentsCross (m, p, a, b);
			}
			for (size_t hj = hi; hj < holes.size () && ok; ++hj) {
				const std::vector<Int32>& r = holes[hj];
				for (size_t k = 0, n = r.size (); k < n && ok; ++k) {
					const V2& a = pts[r[k]];
					const V2& b = pts[r[(k + 1) % n]];
					if (Same (a, m) || Same (b, m))
						continue;
					ok = !SegmentsCross (m, p, a, b);
				}
			}
			if (ok)
				bridge = i;
		}
		if (bridge == poly.size ())
			continue;		// no visible bridge: drop the hole
		std::vector<Int32> merged;
		merged.reserve (poly.size () + hole.size () + 2);
		for (size_t k = 0; k <= bridge; ++k)
			merged.push_back (poly[k]);
		for (size_t k = 0; k <= hole.size (); ++k)
			merged.push_back (hole[(mi + k) % hole.size ()]);
		for (size_t k = bridge; k < poly.size (); ++k)
			merged.push_back (poly[k]);
		poly.swap (merged);
	}

	// Ear clipping.
	std::vector<Int32> v = poly;
	size_t guard = 0;
	while (v.size () > 3 && guard < 100000) {
		++guard;
		const size_t n = v.size ();
		bool clipped = false;
		for (size_t i = 0; i < n; ++i) {
			const Int32 ia = v[(i + n - 1) % n], ib = v[i], ic = v[(i + 1) % n];
			const V2& a = pts[ia];
			const V2& b = pts[ib];
			const V2& c = pts[ic];
			const double cr = Cross (a, b, c);
			if (cr <= 1e-14) {
				if (std::fabs (cr) <= 1e-14 && (Same (a, b) || Same (b, c))) {
					v.erase (v.begin () + (std::ptrdiff_t) i);		// degenerate vertex
					clipped = true;
					break;
				}
				continue;
			}
			bool inside = false;
			for (size_t k = 0; k < n && !inside; ++k) {
				const Int32 ip = v[k];
				if (ip == ia || ip == ib || ip == ic)
					continue;
				const V2& p = pts[ip];
				if (Same (p, a) || Same (p, b) || Same (p, c))
					continue;
				inside = Cross (a, b, p) > 1e-12 && Cross (b, c, p) > 1e-12 && Cross (c, a, p) > 1e-12;
			}
			if (inside)
				continue;
			tris.push_back ({ ia, ib, ic });
			v.erase (v.begin () + (std::ptrdiff_t) i);
			clipped = true;
			break;
		}
		if (!clipped) {
			// Self-intersecting or numerically degenerate: finish with a fan.
			for (size_t k = 1; k + 1 < v.size (); ++k)
				tris.push_back ({ v[0], v[k], v[k + 1] });
			v.clear ();
		}
	}
	if (v.size () == 3 && Cross (pts[v[0]], pts[v[1]], pts[v[2]]) > 1e-14)
		tris.push_back ({ v[0], v[1], v[2] });
	return tris;
}

// =============================================================================
// 3D model writer (OBJ / STL)
// =============================================================================

enum class MeshFormat { Obj, Stl };

struct V3 { double x, y, z; };

V3 Sub (const V3& a, const V3& b)		{ return { a.x - b.x, a.y - b.y, a.z - b.z }; }
V3 CrossV (const V3& a, const V3& b)	{ return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
double Dot (const V3& a, const V3& b)	{ return a.x * b.x + a.y * b.y + a.z * b.z; }


class MeshWriter {
public:
	MeshFormat	format = MeshFormat::Obj;
	double		unit = 1.0;
	bool		yUp = true;
	bool		binary = true;			// STL
	bool		materials = true;		// OBJ + MTL
	bool		includeInvisible = false;

	Int32	bodies = 0, polygons = 0, faces = 0, triangles = 0, skippedPolygons = 0;
	Int64	vertices = 0;
	bool	hasBox = false;
	API_Box3D box {};
	GS::HashSet<API_Guid>	elements;

	MeshWriter ()
	{
		offset.x = offset.y = 0.0;
		ACAPI_Database (APIDb_GetOffsetID, &offset, nullptr);
	}

	void Open (const GS::UniString& path, const GS::UniString& mtlPath)
	{
		out.open (FsPathOf (path), std::ios::binary | std::ios::trunc);
		if (!out)
			Fail ("Cannot open '" + path + "' for writing.", APIERR_GENERAL);
		if (format == MeshFormat::Obj) {
			out << "# Exported from Archicad by the Claude Connector add-on\n";
			out << "# units: 1 = " << Num (1.0 / unit) << " m, " << (yUp ? "Y" : "Z") << " up\n";
			if (materials && !mtlPath.IsEmpty ()) {
				mtlFile = mtlPath;
				mtl.open (FsPathOf (mtlPath), std::ios::binary | std::ios::trunc);
				if (!mtl)
					Fail ("Cannot open '" + mtlPath + "' for writing.", APIERR_GENERAL);
				mtl << "# Materials exported from Archicad\n";
				out << "mtllib " << Utf8 (FsBaseName (mtlPath)) << "\n";
			}
		} else if (binary) {
			char header[80];
			std::memset (header, 0, sizeof (header));
			std::snprintf (header, sizeof (header), "Archicad STL export (Claude Connector), units %s", unit == 1.0 ? "m" : "scaled");
			out.write (header, 80);
			const UInt32 zero = 0;
			out.write (reinterpret_cast<const char*> (&zero), 4);
		} else {
			out << "solid archicad\n";
		}
	}

	void Close ()
	{
		if (format == MeshFormat::Stl) {
			if (binary) {
				const UInt32 n = (UInt32) triangles;
				out.seekp (80, std::ios::beg);
				out.write (reinterpret_cast<const char*> (&n), 4);
			} else {
				out << "endsolid archicad\n";
			}
		}
		out.close ();
		if (!out)
			Fail ("Writing the 3D file failed (disk full or no permission?).", APIERR_GENERAL);
		if (mtl.is_open ()) {
			mtl.close ();
			if (!mtl)
				Fail ("Writing the material file failed.", APIERR_GENERAL);
		}
	}

	bool WroteMaterials () const	{ return !mtlFile.IsEmpty (); }
	const GS::UniString& MaterialFile () const	{ return mtlFile; }
	Int32 MaterialCount () const	{ return (Int32) materialNames.size (); }

	// Writes one body of the current 3D model once.
	void WriteBody (Int32 ibody)
	{
		if (seenBodies.Contains (ibody))
			return;
		seenBodies.Add (ibody);
		API_Component3D comp;
		if (!GetComp (API_BodyID, ibody, comp))
			return;
		const API_BodyType body = comp.body;
		const API_Tranmat tm = body.tranmat;
		const Int32 nVert = std::max<Int32> (body.nVert, 0);
		const Int32 nPgon = std::max<Int32> (body.nPgon, 0);
		if (nVert < 3 || nPgon < 1)
			return;
		++bodies;
		elements.Add (body.parent.guid);

		std::vector<V3> verts ((size_t) nVert, V3 { 0.0, 0.0, 0.0 });
		for (Int32 j = 1; j <= nVert; ++j) {
			if (!GetComp (API_VertID, j, comp))
				continue;
			const double x = comp.vert.x, y = comp.vert.y, z = comp.vert.z;
			V3 w;
			w.x = tm.tmx[0] * x + tm.tmx[1] * y + tm.tmx[2] * z + tm.tmx[3] + offset.x;
			w.y = tm.tmx[4] * x + tm.tmx[5] * y + tm.tmx[6] * z + tm.tmx[7] + offset.y;
			w.z = tm.tmx[8] * x + tm.tmx[9] * y + tm.tmx[10] * z + tm.tmx[11];
			verts[(size_t) (j - 1)] = w;
			Extend (w);
		}

		Int64 base = 0;
		if (format == MeshFormat::Obj) {
			out << "o " << ObjName (body.parent) << "_b" << ibody << "\n";
			for (const V3& v : verts) {
				const V3 f = ToFile (v);
				out << "v " << Num (f.x) << ' ' << Num (f.y) << ' ' << Num (f.z) << "\n";
			}
			base = vertices;
			lastMaterial = -1;
		}
		vertices += nVert;

		for (Int32 j = 1; j <= nPgon; ++j) {
			if (!GetComp (API_PgonID, j, comp))
				continue;
			const API_PgonType pgon = comp.pgon;
			if ((pgon.status & APIPgon_Invis) != 0 && !includeInvisible)
				continue;
			// Contours: vertex indices (0-based, this body).
			std::vector<std::vector<Int32>> rings (1);
			for (Int32 k = pgon.fpedg; k <= pgon.lpedg; ++k) {
				if (!GetComp (API_PedgID, k, comp))
					continue;
				const Int32 pedg = comp.pedg.pedg;
				if (pedg == 0) {
					rings.emplace_back ();
					continue;
				}
				if (!GetComp (API_EdgeID, std::abs (pedg), comp))
					continue;
				const Int32 vi = pedg > 0 ? comp.edge.vert1 : comp.edge.vert2;
				if (vi >= 1 && vi <= nVert)
					rings.back ().push_back (vi - 1);
			}
			if (rings[0].size () < 3) {
				++skippedPolygons;		// degenerate outer contour
				continue;
			}
			rings.erase (std::remove_if (rings.begin () + 1, rings.end (), [] (const std::vector<Int32>& r) { return r.size () < 3; }), rings.end ());
			if (rings.empty ()) {
				++skippedPolygons;
				continue;
			}
			++polygons;

			// Wanted orientation: the polygon normal (ivect) when available, else the contour's own winding.
			const V3 newell = NewellNormal (rings[0], verts);
			V3 wanted = newell;
			if (pgon.ivect != 0 && GetComp (API_VectID, std::abs (pgon.ivect), comp)) {
				const double s = pgon.ivect < 0 ? -1.0 : 1.0;
				const double vx = s * comp.vect.x, vy = s * comp.vect.y, vz = s * comp.vect.z;
				wanted = { tm.tmx[0] * vx + tm.tmx[1] * vy + tm.tmx[2] * vz,
						   tm.tmx[4] * vx + tm.tmx[5] * vy + tm.tmx[6] * vz,
						   tm.tmx[8] * vx + tm.tmx[9] * vy + tm.tmx[10] * vz };
			}
			if (Dot (wanted, wanted) < 1e-24)
				wanted = newell;
			if (Dot (wanted, wanted) < 1e-24) {
				++skippedPolygons;		// degenerate (zero area)
				continue;
			}

			const bool simple = rings.size () == 1 && (pgon.status & APIPgon_Complex) == 0;
			std::vector<std::array<Int32, 3>> tris;
			if (!(simple && format == MeshFormat::Obj)) {
				if (simple) {
					for (size_t k = 1; k + 1 < rings[0].size (); ++k)
						tris.push_back ({ rings[0][0], rings[0][k], rings[0][k + 1] });
				} else {
					tris = TriangulateRings (rings, verts, wanted);
				}
				if (tris.empty ()) {
					++skippedPolygons;
					continue;
				}
			}

			const Int32 material = pgon.iumat != 0 ? (Int32) pgon.iumat : (Int32) body.iumat;
			if (format == MeshFormat::Obj) {
				if (materials && WroteMaterials () && material != lastMaterial) {
					out << "usemtl " << MaterialName (material) << "\n";
					lastMaterial = material;
				}
				if (simple) {
					std::vector<Int32> ring = rings[0];
					if (Dot (newell, wanted) < 0.0)
						std::reverse (ring.begin (), ring.end ());
					out << "f";
					for (Int32 vi : ring)
						out << ' ' << (base + vi + 1);
					out << "\n";
					++faces;
				} else {
					for (const auto& t : tris) {
						out << "f " << (base + t[0] + 1) << ' ' << (base + t[1] + 1) << ' ' << (base + t[2] + 1) << "\n";
						++faces;
					}
				}
				continue;
			}

			// STL
			for (const auto& t : tris) {
				std::array<V3, 3> tv = { verts[(size_t) t[0]], verts[(size_t) t[1]], verts[(size_t) t[2]] };
				V3 n = CrossV (Sub (tv[1], tv[0]), Sub (tv[2], tv[0]));
				if (Dot (n, wanted) < 0.0) {
					std::swap (tv[1], tv[2]);
					n = { -n.x, -n.y, -n.z };
				}
				const double len = std::sqrt (Dot (n, n));
				if (len < 1e-18)
					continue;
				n = { n.x / len, n.y / len, n.z / len };
				WriteStlTriangle (ToFileDir (n), ToFile (tv[0]), ToFile (tv[1]), ToFile (tv[2]));
				++triangles;
			}
		}
	}

private:
	API_Coord					offset;
	std::ofstream				out;
	std::ofstream				mtl;
	GS::UniString				mtlFile;
	GS::HashSet<Int32>			seenBodies;
	std::map<Int32, std::string>	materialNames;
	std::set<std::string>		usedMaterialNames;
	Int32						lastMaterial = -1;

	static GS::UniString FsBaseName (const GS::UniString& path)
	{
		UIndex start = 0;
		for (UIndex i = 0; i < path.GetLength (); ++i) {
			if (path[i] == '/' || path[i] == '\\')
				start = i + 1;
		}
		return path.GetSubstring (start, path.GetLength () - start);
	}

	static bool GetComp (API_3DTypeID type, Int32 index, API_Component3D& comp)
	{
		BNZeroMemory (&comp, sizeof (comp));
		comp.header.typeID = type;
		comp.header.index = index;
		return ACAPI_3D_GetComponent (&comp) == NoError;
	}

	void Extend (const V3& c)
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

	V3 ToFile (const V3& w) const
	{
		if (yUp)
			return { w.x * unit, w.z * unit, -w.y * unit };
		return { w.x * unit, w.y * unit, w.z * unit };
	}

	V3 ToFileDir (const V3& n) const
	{
		if (yUp)
			return { n.x, n.z, -n.y };
		return n;
	}

	static V3 NewellNormal (const std::vector<Int32>& ring, const std::vector<V3>& verts)
	{
		V3 n { 0.0, 0.0, 0.0 };
		for (size_t i = 0, cnt = ring.size (); i < cnt; ++i) {
			const V3& a = verts[(size_t) ring[i]];
			const V3& b = verts[(size_t) ring[(i + 1) % cnt]];
			n.x += (a.y - b.y) * (a.z + b.z);
			n.y += (a.z - b.z) * (a.x + b.x);
			n.z += (a.x - b.x) * (a.y + b.y);
		}
		return n;
	}

	// Projects the rings onto the plane most perpendicular to the normal and triangulates them.
	// Triangles come back wound counter-clockwise around 'wanted'.
	static std::vector<std::array<Int32, 3>> TriangulateRings (const std::vector<std::vector<Int32>>& rings, const std::vector<V3>& verts, const V3& wanted)
	{
		const double ax = std::fabs (wanted.x), ay = std::fabs (wanted.y), az = std::fabs (wanted.z);
		// Drop the dominant axis; keep a right-handed (u, v) so that CCW in 2D = CCW around +axis.
		int drop = (ax >= ay && ax >= az) ? 0 : (ay >= az ? 1 : 2);
		const double sign = drop == 0 ? (wanted.x >= 0 ? 1.0 : -1.0) : drop == 1 ? (wanted.y >= 0 ? 1.0 : -1.0) : (wanted.z >= 0 ? 1.0 : -1.0);
		std::vector<V2> pts (verts.size ());
		for (size_t i = 0; i < verts.size (); ++i) {
			const V3& v = verts[i];
			V2 p;
			if (drop == 0)		p = { v.y, v.z };		// +X: (y, z)
			else if (drop == 1)	p = { v.z, v.x };		// +Y: (z, x)
			else				p = { v.x, v.y };		// +Z: (x, y)
			if (sign < 0.0)
				p.x = -p.x;
			pts[i] = p;
		}
		return Triangulate (rings, pts);
	}

	std::string ObjName (const API_Elem_Head& parent) const
	{
		std::string type = Utf8 (ElemTypeName (parent.type));
		for (char& ch : type) {
			if (ch == ' ')
				ch = '_';
		}
		return type + "_" + Utf8 (GuidStr (parent.guid));
	}

	const std::string& MaterialName (Int32 iumat)
	{
		auto it = materialNames.find (iumat);
		if (it != materialNames.end ())
			return it->second;

		API_Component3D comp;
		BNZeroMemory (&comp, sizeof (comp));
		comp.header.typeID = API_UmatID;
		comp.header.index = iumat;
		GS::UniString uname;
		comp.umat.mater.head.uniStringNamePtr = &uname;
		const GSErrCode err = ACAPI_3D_GetComponent (&comp);
		if (comp.umat.mater.texture.fileLoc != nullptr) {
			delete comp.umat.mater.texture.fileLoc;
			comp.umat.mater.texture.fileLoc = nullptr;
		}
		GS::UniString display = uname;
		if (display.IsEmpty () && err == NoError)
			display = GS::UniString (comp.umat.mater.head.name, CC_Default);
		std::string base = Utf8 (display);
		for (char& ch : base) {
			if (ch == ' ' || ch == '\t' || ch == '#')
				ch = '_';
		}
		if (base.empty ())
			base = "material";
		std::string name = "m" + std::to_string (iumat) + "_" + base;
		if (!usedMaterialNames.insert (name).second)
			name += "_" + std::to_string (materialNames.size ());

		if (mtl.is_open ()) {
			double r = 0.8, g = 0.8, b = 0.8, d = 1.0, ns = 10.0;
			if (err == NoError) {
				r = comp.umat.mater.surfaceRGB.f_red;
				g = comp.umat.mater.surfaceRGB.f_green;
				b = comp.umat.mater.surfaceRGB.f_blue;
				d = 1.0 - std::max (0, std::min<int> (100, comp.umat.mater.transpPc)) / 100.0;
				ns = std::max (0, std::min<int> (10000, comp.umat.mater.shine)) / 100.0 * 10.0;
			}
			mtl << "newmtl " << name << "\n";
			mtl << "Kd " << Num (r) << ' ' << Num (g) << ' ' << Num (b) << "\n";
			mtl << "Ka " << Num (r * 0.2) << ' ' << Num (g * 0.2) << ' ' << Num (b * 0.2) << "\n";
			mtl << "Ks 0.1 0.1 0.1\nNs " << Num (ns) << "\n";
			mtl << "d " << Num (d) << "\nillum 2\n\n";
		}
		return materialNames.emplace (iumat, name).first->second;
	}

	void WriteStlTriangle (const V3& n, const V3& a, const V3& b, const V3& c)
	{
		if (binary) {
			float data[12] = { (float) n.x, (float) n.y, (float) n.z, (float) a.x, (float) a.y, (float) a.z,
							   (float) b.x, (float) b.y, (float) b.z, (float) c.x, (float) c.y, (float) c.z };
			out.write (reinterpret_cast<const char*> (data), sizeof (data));
			const UInt16 attr = 0;
			out.write (reinterpret_cast<const char*> (&attr), 2);
			return;
		}
		out << "facet normal " << Num (n.x) << ' ' << Num (n.y) << ' ' << Num (n.z) << "\n outer loop\n";
		for (const V3* v : { &a, &b, &c })
			out << "  vertex " << Num (v->x) << ' ' << Num (v->y) << ' ' << Num (v->z) << "\n";
		out << " endloop\nendfacet\n";
	}
};


GS::Array<API_Guid> SelectedGuids ()
{
	API_SelectionInfo info;
	BNZeroMemory (&info, sizeof (info));
	GS::Array<API_Neig> neigs;
	const GSErrCode err = ACAPI_Selection_Get (&info, &neigs, false, true);
	BMKillHandle (reinterpret_cast<GSHandle*> (&info.marquee.coords));
	if (err != NoError && err != APIERR_NOSEL)
		Check (err, "Cannot read the selection");
	GS::Array<API_Guid> guids;
	for (const API_Neig& n : neigs) {
		if (!guids.Contains (n.guid))
			guids.Push (n.guid);
	}
	return guids;
}


OS ExportGsm (const OS& params, const GS::UniString& path)
{
	WindowRestorer restorer;
	restorer.Arm (GetBool (params, "restoreWindow", true));
	API_WindowInfo w;
	BNZeroMemory (&w, sizeof (w));
	w.typeID = APIWind_3DModelID;
	if (!SwitchToWindow (w))
		Fail ("Cannot open the 3D window.", APIERR_BADWINDOW);

	API_SavePars_Object pars;
	BNZeroMemory (&pars, sizeof (pars));
	const GS::UniString mode = GetString (params, "gdlMode", "Binary");
	if (EqualsIgnoreCase (mode, "Binary"))
		pars.libItMode = APIConvMod_SymBinGDL;
	else if (EqualsIgnoreCase (mode, "Text"))
		pars.libItMode = APIConvMod_SymTxtGDL;
	else
		Fail ("gdlMode must be Binary or Text.");
	pars.removeLine = GetBool (params, "removeRedundantLines", true);
	pars.saveSelOnly = false;
	pars.view2D = false;
	pars.isPlaceable = GetBool (params, "placeable", true);

	IO::Location loc = ToLocation (path);
	API_FileSavePars fsp;
	BNZeroMemory (&fsp, sizeof (fsp));
	fsp.fileTypeID = APIFType_ObjectFile;
	fsp.file = &loc;
	const GSErrCode err = ACAPI_Automate (APIDo_SaveID, &fsp, &pars);
	fsp.file = nullptr;
	if (err != NoError)
		Fail ("Saving the 3D window as a GDL object failed: " + ErrorName (err) + ". The 3D window must show something (use the views tools: "
			  "show elements in 3D) and no dialog may be open.", err);
	restorer.Finish ();

	OS out;
	out.Add ("file", FileJson (path));
	out.Add ("format", GS::UniString ("gsm"));
	out.Add ("note", GS::UniString ("GDL object of what the 3D window showed (Archicad 'Save as Object'). Load its folder as a library to place it."));
	return out;
}


OS Export3DModel (const OS& params)
{
	GS::Array<GS::UniString> exts;
	exts.Push ("obj");
	exts.Push ("stl");
	exts.Push ("gsm");
	GS::UniString format = GetString (params, "format", GS::UniString ());
	format.SetToLowerCase ();
	if (!format.IsEmpty () && !exts.Contains (format))
		Fail ("'format' must be obj, stl or gsm.");
	const GS::UniString path = PrepareOutputPath (params, "path", exts, format.IsEmpty () ? GS::UniString ("obj") : format);
	if (format.IsEmpty ())
		format = FileExtension (path);
	if (format != FileExtension (path))
		Fail ("The file extension of 'path' does not match format '" + format + "'.");

	if (format == "gsm")
		return ExportGsm (params, path);

	MeshWriter writer;
	writer.format = format == "stl" ? MeshFormat::Stl : MeshFormat::Obj;
	writer.unit = ParseUnits (params, "m").scale;
	const GS::UniString up = GetString (params, "upAxis", writer.format == MeshFormat::Obj ? "Y" : "Z");
	if (EqualsIgnoreCase (up, "Y"))			writer.yUp = true;
	else if (EqualsIgnoreCase (up, "Z"))	writer.yUp = false;
	else Fail ("upAxis must be Y or Z.");
	writer.binary = GetBool (params, "binary", true);
	writer.materials = GetBool (params, "materials", true);
	writer.includeInvisible = GetBool (params, "includeInvisible", false);
	const bool withSubs = GetBool (params, "includeSubelements", true);

	GS::Array<API_Guid> guids = GetGuidArray (params, "elements", false);
	const bool useSelection = GetBool (params, "useSelection", false);
	if (!guids.IsEmpty () && useSelection)
		Fail ("Pass either 'elements' or useSelection: true, not both.");
	if (useSelection) {
		guids = SelectedGuids ();
		if (guids.IsEmpty ())
			Fail ("Nothing is selected. Select elements first (set_selection) or pass 'elements'.", APIERR_NOSEL);
	}
	GS::UniString source = GetString (params, "source", guids.IsEmpty () ? "3DWindow" : "Elements");
	if (!guids.IsEmpty ())
		source = "Elements";
	if (!EqualsIgnoreCase (source, "3DWindow") && !EqualsIgnoreCase (source, "AllElements") && !EqualsIgnoreCase (source, "Elements"))
		Fail ("'source' must be 3DWindow (what the 3D window shows) or AllElements (every 3D element of the project).");

	GS::UniString mtlPath;
	if (writer.format == MeshFormat::Obj && writer.materials) {
		mtlPath = path.GetSubstring (0, path.GetLength () - 4) + ".mtl";
		if (PathExists (mtlPath) && !GetBool (params, "overwrite", false))
			Fail ("The material file '" + mtlPath + "' already exists. Pass overwrite: true or materials: false.");
	}

	WindowRestorer restorer;
	restorer.Arm (GetBool (params, "restoreWindow", true));
	GS::UniString note;
	Int32 elementsWithout3D = 0;

	writer.Open (path, mtlPath);
	bool done = false;
	if (EqualsIgnoreCase (source, "3DWindow")) {
		API_WindowInfo w;
		BNZeroMemory (&w, sizeof (w));
		w.typeID = APIWind_3DModelID;
		SwitchToWindow (w);
		Int32 nBody = 0;
		if (ACAPI_3D_GetNum (API_BodyID, &nBody) == NoError && nBody > 0) {
			for (Int32 i = 1; i <= nBody; ++i)
				writer.WriteBody (i);
			done = true;
		} else {
			note = "The 3D window model was empty; exported every 3D element of the project instead (source AllElements).";
		}
	}
	if (!done) {
		DatabaseScope plan (ListDatabases (APIWind_FloorPlanID)[0]);
		GS::Array<API_Guid> list = guids;
		if (list.IsEmpty ())
			Check (ACAPI_Element_GetElemList (API_ElemType (API_ZombieElemID), &list), "Cannot list the elements");
		for (const API_Guid& g : list) {
			API_Elem_Head head;
			BNZeroMemory (&head, sizeof (head));
			head.guid = g;
			if (ACAPI_Element_GetHeader (&head) != NoError) {
				++elementsWithout3D;
				continue;
			}
			GS::Array<API_Elem_Head> heads;
			heads.Push (head);
			if (withSubs && eq::IsHierarchicalType (head.type.typeID)) {
				for (const eq::SubElemInfo& s : eq::CollectSubelements (g, head.type.typeID, false)) {
					API_Elem_Head full;
					BNZeroMemory (&full, sizeof (full));
					full.guid = s.head.guid;
					if (ACAPI_Element_GetHeader (&full) == NoError)
						heads.Push (full);
				}
			}
			bool any = false;
			for (const API_Elem_Head& h : heads) {
				API_ElemInfo3D info;
				BNZeroMemory (&info, sizeof (info));
				if (ACAPI_Element_Get3DInfo (h, &info) != NoError || info.fbody <= 0 || info.lbody < info.fbody)
					continue;
				any = true;
				for (Int32 ib = info.fbody; ib <= info.lbody; ++ib)
					writer.WriteBody (ib);
			}
			if (!any)
				++elementsWithout3D;
		}
	}
	writer.Close ();
	restorer.Finish ();

	if (writer.bodies == 0) {
		std::error_code ec;
		fs::remove (FsPathOf (path), ec);
		if (!mtlPath.IsEmpty ())
			fs::remove (FsPathOf (mtlPath), ec);
		Fail ("No 3D geometry was found (" + source + "). For source 3DWindow, show elements in the 3D window first (views tools); for "
			  "'elements' pass 3D element guids (walls, slabs, objects ...), not 2D elements.", APIERR_BADPARS);
	}

	OS out;
	out.Add ("file", FileJson (path));
	if (writer.WroteMaterials ())
		out.Add ("materialFile", FileJson (writer.MaterialFile ()));
	out.Add ("format", format);
	out.Add ("source", source);
	out.Add ("units", GS::UniString (ParseUnits (params, "m").name));
	out.Add ("upAxis", GS::UniString (writer.yUp ? "Y" : "Z"));
	out.Add ("elementCount", (Int32) writer.elements.GetSize ());
	out.Add ("bodyCount", writer.bodies);
	out.Add ("vertexCount", (double) writer.vertices);
	out.Add ("polygonCount", writer.polygons);
	if (writer.format == MeshFormat::Obj)
		out.Add ("faceCount", writer.faces);
	else
		out.Add ("triangleCount", writer.triangles);
	if (writer.WroteMaterials ())
		out.Add ("materialCount", writer.MaterialCount ());
	if (writer.skippedPolygons > 0)
		out.Add ("skippedPolygons", writer.skippedPolygons);
	if (elementsWithout3D > 0 && EqualsIgnoreCase (source, "Elements"))
		out.Add ("elementsWithout3D", elementsWithout3D);
	if (writer.hasBox)
		out.Add ("boundingBox", Box3DObj (writer.box));
	if (!note.IsEmpty ())
		out.Add ("note", note);
	return out;
}

} // namespace


void RegisterDocumentationFileCommands ()
{
	RegisterCommand ("ExportDxf",
		"Writes a DXF (R12 ASCII) of a 2D window from the primitives Archicad draws for its visible elements (lines, arcs, circles, "
		"polylines with arcs, texts, fill pattern lines; layers = Archicad layers, color = pen index). Input: {path (.dxf), view? | "
		"layout? | database? | storyIndex? (default: front window), elements?: [guid] (only these), units?: mm|cm|m|in|ft (default "
		"mm), fillPatterns?: bool (default true), fillBoundaries?: bool (default false), includeMasterLayout?: bool (default true), "
		"maxEntities?, restoreWindow?: bool (default true), overwrite?, createFolders?}.",
		[] (const OS& params) -> OS { return ExportDxf (params); });

	RegisterCommand ("Export3DModel",
		"Exports the 3D model: .obj (+ .mtl materials), .stl (binary or ASCII) written from the 3D bodies, or .gsm (Archicad "
		"'Save as Object' of the 3D window). Input: {path, format?: obj|stl|gsm, elements?: [guid] | useSelection?: true | source?: "
		"3DWindow (default) | AllElements, includeSubelements?: bool (default true), units?: m|mm|cm|in|ft (default m), upAxis?: Y|Z "
		"(default Y for obj, Z for stl), materials?: bool (obj, default true), binary?: bool (stl, default true), includeInvisible?, "
		"gdlMode?: Binary|Text (gsm), placeable? (gsm), restoreWindow?, overwrite?, createFolders?}.",
		[] (const OS& params) -> OS { return Export3DModel (params); });
}

} // namespace doc
} // namespace cc
