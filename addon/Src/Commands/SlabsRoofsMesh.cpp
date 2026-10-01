// *****************************************************************************
// SlabsRoofsMesh — Mesh adapter (terrain / free-form surfaces).
//
// Create / modify fields (meters, degrees):
//   polygon* {points [{x, y, z}], arcs?, holes?} — z = height of the vertex above the mesh base (level)
//   level (base plane elevation from the home story), skirt "SolidBody"|"SkirtWithoutBottom"|"SurfaceOnly",
//   skirtLevel (distance of the bottom/skirt from the base plane), ridges "AllSharp"|"AllSmooth"|"UserDefined",
//   showAllLines, levelLines [{points: [{x, y, z}]}] ([] clears them), buildingMaterial,
//   topSurface / bottomSurface / sideSurface, surfacesChained, contourPen, levelLinePen, contourLineType,
//   cutContourPen, cutFillPen, cutFillBackgroundPen, showCoverFill, coverFill, coverFillPen,
//   coverFillBackgroundPen, storyVisibility + common fields
// Modification follows the pattern proven live for meshes: the memo read from the element is
// resized in place and sent back with ACAPI_Element_Change (APIMemoMask_Polygon | MeshPolyZ | MeshLevel).
// *****************************************************************************

#include "Commands/Commands.hpp"
#include "Commands/SlabsRoofsCommon.hpp"
#include "Core/Command.hpp"

#include <algorithm>
#include <cmath>

namespace cc {

namespace {

using namespace slabroof;

// API_MeshType::skirt: 1 = solid body with skirt, 2 = skirt without bottom, 3 = surface only.
const NamedValue kSkirts[] = {
	{ "SolidBody",					1 },
	{ "SkirtWithoutBottom",			2 },
	{ "SurfaceOnly",				3 },
	{ "SolidBodyWithSkirt",			1 },	// aliases
	{ "WithSkirt",					2 },
	{ "SurfaceOnlyWithoutSkirt",	3 },
};

const NamedValue kRidges[] = {
	{ "AllSharp",		APIRidge_AllSharp },
	{ "AllSmooth",		APIRidge_AllSmooth },
	{ "UserDefined",	APIRidge_UserSharp },
};

using LevelLine = GS::Array<API_Coord3D>;


GS::Array<LevelLine> LevelLinesFrom (const OS& spec)
{
	GS::Array<LevelLine> lines;
	GS::Array<OS> items;
	if (spec.IsList ("levelLines"))
		items = GetObjectArray (spec, "levelLines");
	else
		Fail ("'levelLines' must be an array of {points: [{x, y, z}, ...]} (use [] to remove all level lines).");
	for (UIndex k = 0; k < items.GetSize (); ++k) {
		const GS::Array<OS> pts = GetObjectArray (items[k], "points");
		if (pts.GetSize () < 2) {
			GS::UniString msg;
			msg = GS::UniString::Printf ("levelLines[%u] needs at least 2 points.", (unsigned) k);
			Fail (msg);
		}
		LevelLine line;
		for (const OS& p : pts) {
			API_Coord3D c;
			c.x = GetDouble (p, "x");
			c.y = GetDouble (p, "y");
			c.z = GetDouble (p, "z");
			line.Push (c);
		}
		lines.Push (line);
	}
	return lines;
}


void WriteLevelLines (API_ElementMemo& memo, API_MeshLevel& levels, const GS::Array<LevelLine>& lines)
{
	Int32 total = 0;
	for (const LevelLine& l : lines)
		total += (Int32) l.GetSize ();
	ResizeHandle (memo.meshLevelCoords, total);
	ResizeHandle (memo.meshLevelEnds, (Int32) lines.GetSize ());
	Int32 i = 0;
	for (UIndex k = 0; k < lines.GetSize (); ++k) {
		for (const API_Coord3D& c : lines[k]) {
			API_MeshLevelCoord& lc = (*memo.meshLevelCoords)[i++];
			BNZeroMemory (&lc, sizeof (lc));
			lc.c = c;		// vertexID 0 = new vertex (Archicad assigns the IDs)
		}
		(*memo.meshLevelEnds)[k] = i;		// cumulative end index (exclusive)
	}
	levels.nCoords = total;
	levels.nSubLines = (Int32) lines.GetSize ();
}


GS::Array<OS> LevelLinesJson (const API_ElementMemo& memo)
{
	GS::Array<OS> out;
	const Int32 nCoords = HandleCount (reinterpret_cast<GSConstHandle> (memo.meshLevelCoords), sizeof (API_MeshLevelCoord));
	const Int32 nLines = HandleCount (reinterpret_cast<GSConstHandle> (memo.meshLevelEnds), sizeof (Int32));
	Int32 i = 0;
	for (Int32 k = 0; k < nLines; ++k) {
		const Int32 end = std::min ((*memo.meshLevelEnds)[k], nCoords);
		GS::Array<OS> pts;
		for (; i < end; ++i)
			pts.Push (Coord3DObj ((*memo.meshLevelCoords)[i].c));
		if (!pts.IsEmpty ())
			out.Push (OS ("points", pts));
	}
	return out;
}


// Rewrites the polygon of a memo read from an existing mesh (handles resized in place; existing
// vertex IDs are kept, new vertices get ID 0), matching the live-proven ModifyMeshes pattern.
void WriteMeshPolygonInPlace (const PolygonData& input, API_Polygon& poly, API_ElementMemo& memo)
{
	PolygonData data = input;
	NormalizeOrientation (data);

	GS::Array<const Contour*> contours;
	contours.Push (&data.outline);
	for (const Contour& h : data.holes)
		contours.Push (&h);
	const Int32 nCoords = data.NumCoords ();
	Int32 nArcs = 0;
	for (const Contour* c : contours)
		nArcs += (Int32) c->arcs.GetSize ();

	ResizeHandle (memo.coords, nCoords + 1);
	ResizeHandle (memo.meshPolyZ, nCoords + 1);
	ResizeHandle (memo.pends, (Int32) contours.GetSize () + 1);
	if (nArcs > 0) {
		ResizeHandle (memo.parcs, nArcs);
	} else if (memo.parcs != nullptr) {
		GSHandle h = reinterpret_cast<GSHandle> (memo.parcs);
		BMKillHandle (&h);
		memo.parcs = nullptr;
	}
	if (memo.vertexIDs != nullptr)
		ResizeHandle (memo.vertexIDs, nCoords + 1);

	Int32 idx = 1;
	Int32 arcIdx = 0;
	(*memo.pends)[0] = 0;
	for (UIndex ci = 0; ci < contours.GetSize (); ++ci) {
		const Contour& c = *contours[ci];
		const Int32 first = idx;
		const Int32 n = (Int32) c.points.GetSize ();
		for (Int32 i = 0; i < n; ++i) {
			(*memo.coords)[idx] = c.points[i];
			(*memo.meshPolyZ)[idx] = c.z.IsEmpty () ? 0.0 : c.z[i];
			++idx;
		}
		(*memo.coords)[idx] = c.points[0];
		(*memo.meshPolyZ)[idx] = c.z.IsEmpty () ? 0.0 : c.z[0];
		(*memo.pends)[ci + 1] = idx;
		++idx;
		for (const auto& a : c.arcs) {
			(*memo.parcs)[arcIdx].begIndex = first + a.first;
			(*memo.parcs)[arcIdx].endIndex = first + a.first + 1;
			(*memo.parcs)[arcIdx].arcAngle = a.second;
			++arcIdx;
		}
	}
	poly.nCoords = nCoords;
	poly.nSubPolys = (Int32) contours.GetSize ();
	poly.nArcs = nArcs;
}


void ApplyMeshFields (API_Element& element, API_Element* mask, const OS& spec)
{
	API_MeshType& mesh = element.mesh;
	const FieldMask m (element, mask);
	if (Has (spec, "composite") || Has (spec, "profile"))
		Fail ("Meshes have a single 'buildingMaterial' (no composite or profile).", APIERR_NOTSUPPORTED);

	if (ApplyLength (spec, "level", mesh.level))				m.Set (&mesh.level);
	if (ApplyLength (spec, "skirtLevel", mesh.skirtLevel))		m.Set (&mesh.skirtLevel);
	if (Has (spec, "skirt")) {
		mesh.skirt = (short) ParseNamed (kSkirts, spec, "skirt");
		m.Set (&mesh.skirt);
	}
	if (Has (spec, "ridges")) {
		mesh.smoothRidges = (char) ParseNamed (kRidges, spec, "ridges");
		m.Set (&mesh.smoothRidges);
	}
	if (auto b = OptBool (spec, "showAllLines")) {
		mesh.showLines = *b ? 1 : 0;
		m.Set (&mesh.showLines);
	}
	if (ApplyAttr (spec, "buildingMaterial", API_BuildingMaterialID, mesh.buildingMaterial))	m.Set (&mesh.buildingMaterial);
	if (ApplyOverriddenSurface (spec, "topSurface", mesh.topMat))		m.Set (&mesh.topMat);
	if (ApplyOverriddenSurface (spec, "bottomSurface", mesh.botMat))	m.Set (&mesh.botMat);
	if (ApplyOverriddenSurface (spec, "sideSurface", mesh.sideMat))		m.Set (&mesh.sideMat);
	if (ApplyFlag (spec, "surfacesChained", mesh.materialsChained))		m.Set (&mesh.materialsChained);
	if (ApplyPen (spec, "contourPen", mesh.contPen))							m.Set (&mesh.contPen);
	if (ApplyPen (spec, "levelLinePen", mesh.levelPen))						m.Set (&mesh.levelPen);
	if (ApplyAttr (spec, "contourLineType", API_LinetypeID, mesh.ltypeInd))	m.Set (&mesh.ltypeInd);
	if (ApplyPen (spec, "cutContourPen", mesh.sectContPen))					m.Set (&mesh.sectContPen);
	if (ApplyPenOverride (spec, mesh.penOverride))							m.Set (&mesh.penOverride);
	if (ApplyFlag (spec, "showCoverFill", mesh.useFloorFill))				m.Set (&mesh.useFloorFill);
	if (ApplyAttr (spec, "coverFill", API_FilltypeID, mesh.floorFillInd))	m.Set (&mesh.floorFillInd);
	if (ApplyPen (spec, "coverFillPen", mesh.floorFillPen))					m.Set (&mesh.floorFillPen);
	if (ApplyPen (spec, "coverFillBackgroundPen", mesh.floorFillBGPen))		m.Set (&mesh.floorFillBGPen);
	if (ApplyStoryVisibility (spec, mesh.visibilityCont, mesh.visibilityFill)) {
		m.Set (&mesh.visibilityCont);
		m.Set (&mesh.visibilityFill);
	}
}


API_Guid CreateMesh (const OS& spec)
{
	if (!Has (spec, "polygon"))
		Fail ("Mesh requires 'polygon' (at least 3 points {x, y, z}; z = height above the mesh level).");

	API_Element element = NewElement (API_MeshID);
	GetDefaults (element, nullptr);
	ApplyCommonFields (element, nullptr, spec);
	ApplyMeshFields (element, nullptr, spec);

	const PolygonData polygon = GetPolygon (spec, "polygon");
	Memo memo;
	WritePolygonToMemo (polygon, element.mesh.poly, *memo, true);
	element.mesh.levelLines.nCoords = 0;
	element.mesh.levelLines.nSubLines = 0;
	if (Has (spec, "levelLines")) {
		const GS::Array<LevelLine> lines = LevelLinesFrom (spec);
		if (!lines.IsEmpty ())
			WriteLevelLines (*memo, element.mesh.levelLines, lines);
	}

	GSErrCode err = ACAPI_Element_Create (&element, memo.Ptr ());
	if (err == APIERR_IRREGULARPOLY && RegularizeMemoPolygon (*memo, element.mesh.poly, "Mesh"))
		err = ACAPI_Element_Create (&element, memo.Ptr ());
	Check (err, "Cannot create mesh" + GS::UniString (err == APIERR_IRREGULARPOLY || err == APIERR_BADPOLY
		? " (the polygon is irregular: self-intersecting, repeated points, or holes outside the outline — fix the points)" : ""));
	return element.header.guid;
}


void SerializeMesh (const API_Element& element, OS& out)
{
	const API_MeshType& mesh = element.mesh;
	out.Add ("level", mesh.level);
	out.Add ("skirt", NameOf (kSkirts, mesh.skirt));
	out.Add ("skirtLevel", mesh.skirtLevel);
	out.Add ("ridges", NameOf (kRidges, mesh.smoothRidges));
	out.Add ("showAllLines", mesh.showLines != 0);
	out.Add ("buildingMaterial", AttrRef (API_BuildingMaterialID, mesh.buildingMaterial));
	AddOverriddenSurfaceJson (out, "topSurface", mesh.topMat);
	AddOverriddenSurfaceJson (out, "bottomSurface", mesh.botMat);
	AddOverriddenSurfaceJson (out, "sideSurface", mesh.sideMat);
	out.Add ("surfacesChained", mesh.materialsChained);

	Memo memo;
	if (ACAPI_Element_GetMemo (element.header.guid, memo.Ptr (), APIMemoMask_Polygon | APIMemoMask_MeshPolyZ | APIMemoMask_MeshLevel) == NoError) {
		out.Add ("polygon", PolygonToJson (PolygonCounts (memo->coords, memo->pends, memo->parcs), *memo, true));
		out.Add ("levelLines", LevelLinesJson (*memo));
	}

	out.Add ("contourPen", (Int32) mesh.contPen);
	out.Add ("levelLinePen", (Int32) mesh.levelPen);
	out.Add ("contourLineType", AttrRef (API_LinetypeID, mesh.ltypeInd));
	out.Add ("cutContourPen", (Int32) mesh.sectContPen);
	AddPenOverrideJson (out, mesh.penOverride);
	out.Add ("showCoverFill", mesh.useFloorFill);
	out.Add ("coverFill", AttrRef (API_FilltypeID, mesh.floorFillInd));
	out.Add ("coverFillPen", (Int32) mesh.floorFillPen);
	out.Add ("coverFillBackgroundPen", (Int32) mesh.floorFillBGPen);
	out.Add ("storyVisibility", StoryVisibilityJson (mesh.visibilityCont));
}


void ModifyMesh (API_Element& element, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask, const OS& patch)
{
	ApplyMeshFields (element, &mask, patch);
	const bool polygonChange = Has (patch, "polygon");
	const bool linesGiven = Has (patch, "levelLines");
	if (!polygonChange && !linesGiven)
		return;

	GS::Array<LevelLine> lines;
	if (linesGiven)
		lines = LevelLinesFrom (patch);
	const bool clearing = linesGiven && lines.IsEmpty ();

	UInt64 load = 0;
	if (polygonChange || clearing)
		load |= APIMemoMask_Polygon | APIMemoMask_MeshPolyZ;
	if (linesGiven && !clearing)
		load |= APIMemoMask_MeshLevel;
	LoadMemo (element.header.guid, memo, load);

	if (polygonChange) {
		WriteMeshPolygonInPlace (GetPolygon (patch, "polygon"), element.mesh.poly, memo);
		memoMask |= APIMemoMask_Polygon | APIMemoMask_MeshPolyZ;
	}
	if (linesGiven) {
		if (clearing) {
			// Archicad ignores empty level-line handles, so send two tiny lines just outside the polygon's
			// bounding box: Archicad clips them away and the mesh ends up with no level lines.
			const Int32 nCoords = HandleCount (reinterpret_cast<GSConstHandle> (memo.coords), sizeof (API_Coord)) - 1;
			if (nCoords < 1)
				Fail ("The mesh has no polygon; cannot clear its level lines.", APIERR_BADPOLY);
			double xMin = (*memo.coords)[1].x;
			double yMin = (*memo.coords)[1].y;
			for (Int32 j = 2; j <= nCoords; ++j) {
				xMin = std::min (xMin, (*memo.coords)[j].x);
				yMin = std::min (yMin, (*memo.coords)[j].y);
			}
			const double ox = xMin - 1.0;
			const double oy = yMin - 1.0;
			LevelLine a, b;
			a.Push (API_Coord3D { ox, oy, 0.0 });		a.Push (API_Coord3D { ox + 0.5, oy, 0.0 });
			b.Push (API_Coord3D { ox, oy + 0.5, 0.0 });	b.Push (API_Coord3D { ox + 0.5, oy + 0.5, 0.0 });
			lines.Push (a);
			lines.Push (b);
		}
		WriteLevelLines (memo, element.mesh.levelLines, lines);
		FieldMask (element, &mask).Set (&element.mesh.levelLines);
		memoMask |= APIMemoMask_MeshLevel;
	}
}

} // namespace


void RegisterMeshAdapter ()
{
	RegisterAdapter ({ API_MeshID, CreateMesh, SerializeMesh, ModifyMesh });
}

} // namespace cc
