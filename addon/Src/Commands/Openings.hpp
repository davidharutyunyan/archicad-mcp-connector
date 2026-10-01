// *****************************************************************************
// Openings.hpp — private helpers shared by the openings family
// (Openings.cpp: windows / doors / skylights, OpeningsExtrusion.cpp: Opening tool).
// Not part of the Core API — do not include from other families.
// *****************************************************************************

#pragma once

#include "Core/Json.hpp"
#include "Core/Types.hpp"

#include <optional>

namespace cc {
namespace openings {

// --- Host walls ------------------------------------------------------------------

struct WallGeometry {
	API_Guid	guid		= APINULLGuid;
	short		floorInd	= 0;
	API_Coord	begC		= {};
	API_Coord	endC		= {};
	double		arcAngle	= 0.0;		// radians, 0 = straight
	double		chord		= 0.0;		// begC -> endC distance
	double		length		= 0.0;		// length along the reference line (arc length for curved walls)
	double		thickness	= 0.0;
	double		height		= 0.0;
	double		bottomOffset = 0.0;
	bool		curved		= false;
	bool		flipped		= false;
	double		bodyCenterOffset = 0.0;	// signed offset of the body centre from the reference line
										// towards the "inside" (right of begC->endC when not flipped)
};

struct WallProjection {
	double		distance	= 0.0;		// along the reference line from begC
	API_Coord	point		= {};		// the projected point on the reference line
	double		tangentAngle = 0.0;		// direction of the reference line at the point (radians, CCW from +X)
	double		offset		= 0.0;		// signed distance of the input point from the reference line
};

// Loads the wall a window / door / opening is placed into. Throws an actionable error when the
// GUID is not a wall or the wall is polygonal (Archicad cannot host openings in those).
WallGeometry		LoadHostWall (const API_Guid& guid, const char* elementNoun);

// Projects a point perpendicularly onto the wall reference line (straight or curved).
// Throws when the projection falls outside the wall (beyond begin / end).
WallProjection		ProjectOntoWall (const WallGeometry& wall, const API_Coord& p, const char* fieldName);

// Point + tangent (radians, direction of travel begC -> endC) at a distance along the wall
// reference line, for straight and curved walls. Curved walls: Archicad 26 stores the arc angle
// as a positive value with the arc centre on the LEFT of begC -> endC (verified live: begin
// (820,0), end (830,0), 60 deg bulges towards -y); a negative angle is treated as the mirror case.
// Returns false for degenerate walls.
bool				PointOnWall (const WallGeometry& wall, double distance, API_Coord& point, double& tangentAngle);

// Unit normal (plan) pointing from the reference line into the wall body at the given tangent
// direction: to the right of the direction of travel for non-flipped walls, to the left for
// flipped ones, reversed when the body lies on the other side of the reference line
// (e.g. reference line "Inside"). Returns false when the body is centred on the reference line.
bool				WallBodyNormal (const WallGeometry& wall, double tangentAngle, API_Coord& normal);

// Throws when a centre distance lies outside [0, wall.length].
void				CheckDistanceOnWall (const WallGeometry& wall, double distance, const char* what);

// --- Current database --------------------------------------------------------------

// Makes the floor plan the CURRENT database for the lifetime of the object (restores the previous
// one afterwards). Archicad refuses to create windows / doors (and their markers) with
// APIERR_BADDATABASE while e.g. a section, elevation or layout is the current database.
class FloorPlanDatabaseScope {
public:
	FloorPlanDatabaseScope ();
	~FloorPlanDatabaseScope ();
	FloorPlanDatabaseScope (const FloorPlanDatabaseScope&) = delete;
	FloorPlanDatabaseScope& operator= (const FloorPlanDatabaseScope&) = delete;

private:
	API_DatabaseInfo	previous;
	bool				switched = false;
};

// --- Library parts / GDL parameters -------------------------------------------------

// Resolves the library part of a window / door / skylight (spec[key]: name, index, {guid}...).
// Archicad 26 reports API_LibPart::typeID as "Object" for every library part, so the Core type
// filter of FindLibPart cannot be used: the part's creator tool (APIAny_GetLibPartToolVariationID)
// is checked against elemType instead. Throws an actionable error for non-placeable parts or
// parts of another tool.
API_LibPart				FindOpeningLibPart (const OS& spec, const char* key, API_ElemTypeID elemType);

// {"index", "name", "guid", "fileName"} (+ "missing": true when the part is not in the loaded libraries).
OS						LibPartBrief (Int32 libInd);

// Compact {name: value} object: A, B, ZZYZX first, then the first visible, enabled scalar
// parameters (Archicad-managed "ac_*" parameters skipped) up to `maxCount` entries.
// *total receives the number of such candidate parameters.
OS						GdlParamsSummary (API_AddParType** params, UInt32 maxCount, UInt32* total);

// Real value of a named (non-array) parameter, if present.
std::optional<double>	ParamReal (API_AddParType** params, const char* name);

// Copy of `values` with "A" / "B" added (when given and not already present, case-insensitive).
OS						WithSizeParams (const OS& values, std::optional<double> a, std::optional<double> b);

// --- Misc readers -------------------------------------------------------------------

// Pen index in [minPen, 255].
std::optional<short>	OptPen (const OS& os, const char* key, short minPen = 1);

// Element GUID from the first present key of `keys` (e.g. {"wall", "owner"}).
std::optional<API_Guid>	OptGuidAny (const OS& os, std::initializer_list<const char*> keys);

// Registers the API_OpeningID (Opening tool) adapter — implemented in OpeningsExtrusion.cpp.
void					RegisterOpeningToolAdapter ();

} // namespace openings
} // namespace cc
