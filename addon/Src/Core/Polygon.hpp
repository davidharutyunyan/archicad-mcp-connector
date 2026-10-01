// *****************************************************************************
// Polygon.hpp — conversion between JSON polygons and Archicad memo polygons.
//
// JSON polygon format (used by slabs, roofs, meshes, zones, hatches, ...):
//   {
//     "points": [{"x":0,"y":0}, {"x":5,"y":0}, {"x":5,"y":4}, {"x":0,"y":4}],
//     "arcs":   [{"index": 1, "angle": 90}],        // optional: edge points[1]->points[2] is an arc (degrees, +CCW)
//     "holes":  [ {"points": [...], "arcs": [...]} ] // optional, each hole is an object like the outline
//   }
// A plain array of points is accepted as a polygon without holes/arcs.
// Do NOT repeat the first point at the end (it is accepted and removed).
// Points may carry "z" (used by meshes: absolute height relative to the mesh level).
//
// Archicad memo layout: coords[0] unused, each contour closed by repeating its first
// vertex, pends[k] = index of the closing vertex of contour k, parcs use coord indices.
// *****************************************************************************

#pragma once

#include "Core/Json.hpp"

namespace cc {

struct Contour {
	GS::Array<API_Coord>	points;		// not closed
	GS::Array<double>		z;			// optional, same size as points when present
	GS::Array<std::pair<Int32, double>> arcs;	// (edge start index in points, arc angle in radians)
};

struct PolygonData {
	Contour				outline;
	GS::Array<Contour>	holes;
	bool				hasZ = false;

	Int32	NumCoords () const;		// memo coordinate count (with closing points)
};

// Parse from JSON value at key (object or plain point array). Throws on invalid input.
PolygonData		GetPolygon (const OS& os, const char* key);
PolygonData		PolygonFrom (const OS& polygonOrPoints, const GS::UniString& fieldName);

// Parse a polyline (open path): {"points": [...], "arcs": [...]} or a plain array.
Contour			GetPolyline (const OS& os, const char* key);

// Writes the polygon into memo.coords / pends / parcs (and meshPolyZ when withZ) and fills poly.
// Existing memo handles for these fields are released first.
void			WritePolygonToMemo (const PolygonData& data, API_Polygon& poly, API_ElementMemo& memo, bool withZ = false);

// Writes an open polyline into memo.coords / parcs (pends has a single contour).
void			WritePolylineToMemo (const Contour& line, API_Polygon& poly, API_ElementMemo& memo);

// Serializes an Archicad memo polygon into the JSON polygon format.
OS				PolygonToJson (const API_Polygon& poly, const API_ElementMemo& memo, bool withZ = false);
OS				PolylineToJson (const API_Polygon& poly, const API_ElementMemo& memo);

// Signed area of a contour (positive = counter-clockwise).
double			SignedArea (const GS::Array<API_Coord>& pts);

// Makes the outline counter-clockwise and holes clockwise (Archicad's expected orientation).
void			NormalizeOrientation (PolygonData& data);

} // namespace cc
