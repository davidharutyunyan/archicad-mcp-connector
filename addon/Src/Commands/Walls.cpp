// *****************************************************************************
// Walls — reference implementation of an element adapter.
//
// Create / modify fields (all lengths in meters, angles in degrees):
//   begin {x,y}*, end {x,y}*         (* required on create)
//   height, thickness, endThickness (makes a trapezoid wall), bottomOffset (from home story),
//   topLinkedStory (relative story offset, 0 = not linked), topOffset
//     NOTE: giving "height" without "topLinkedStory" unlinks the top so the height is honoured,
//   arcAngle (curved wall, 0 = straight), referenceLine ("Outside"|"Center"|"Inside"|
//   "CoreOutside"|"CoreCenter"|"CoreInside"), flipped, offset (base line offset from ref line),
//   buildingMaterial | composite | profile, refSurface / oppSurface / sideSurface (overrides),
//   zoneRelation ("Boundary"|"ReduceArea"|"None"), slantAlpha, slantBeta (degrees),
//   + common: layer, storyIndex, renovationStatus, elementId
// *****************************************************************************

#include "Commands/Commands.hpp"
#include "Core/Command.hpp"
#include "Core/Elements.hpp"
#include "Core/Enums.hpp"

#include <cmath>

namespace cc {

namespace {

const NamedValue kRefLines[] = {
	{ "Outside",		APIWallRefLine_Outside },
	{ "Center",			APIWallRefLine_Center },
	{ "Inside",			APIWallRefLine_Inside },
	{ "CoreOutside",	APIWallRefLine_CoreOutside },
	{ "CoreCenter",		APIWallRefLine_CoreCenter },
	{ "CoreInside",		APIWallRefLine_CoreInside },
};

const NamedValue kZoneRels[] = {
	{ "Boundary",		APIZRel_Boundary },
	{ "ReduceArea",		APIZRel_ReduceArea },
	{ "None",			APIZRel_None },
};


// Applies wall fields; mask == nullptr when creating.
void ApplyWallFields (API_WallType& wall, API_Element* mask, const OS& spec)
{
#define WALL_SET(field) if (mask) ACAPI_ELEMENT_MASK_SET (*mask, API_WallType, field)

	if (auto c = OptCoord (spec, "begin"))			{ wall.begC = *c; WALL_SET (begC); }
	if (auto c = OptCoord (spec, "end"))			{ wall.endC = *c; WALL_SET (endC); }
	if (auto v = OptDouble (spec, "height"))		{
		wall.height = *v; WALL_SET (height);
		// An explicit height only takes effect on an unlinked wall (tool defaults are often top-linked).
		if (!spec.Contains ("topLinkedStory")) { wall.relativeTopStory = 0; WALL_SET (relativeTopStory); }
	}
	if (auto v = OptDouble (spec, "thickness"))	{
		wall.thickness = *v; WALL_SET (thickness);
		if (!spec.Contains ("endThickness") && wall.type == APIWtyp_Trapez) {
			wall.thickness1 = *v; WALL_SET (thickness1);
		}
	}
	if (auto v = OptDouble (spec, "endThickness"))	{
		wall.thickness1 = *v; WALL_SET (thickness1);
		wall.type = std::fabs (*v - wall.thickness) > 1e-9 ? APIWtyp_Trapez : APIWtyp_Normal; WALL_SET (type);
	}
	if (auto v = OptDouble (spec, "bottomOffset"))	{ wall.bottomOffset = *v; WALL_SET (bottomOffset); }
	if (auto v = OptInt (spec, "topLinkedStory"))	{ wall.relativeTopStory = (short) *v; WALL_SET (relativeTopStory); }
	if (auto v = OptDouble (spec, "topOffset"))	{ wall.topOffset = *v; WALL_SET (topOffset); }
	if (auto v = OptAngle (spec, "arcAngle"))		{ wall.angle = *v; WALL_SET (angle); }
	if (auto v = OptBool (spec, "flipped"))		{ wall.flipped = *v; WALL_SET (flipped); }
	if (auto v = OptDouble (spec, "offset"))		{ wall.offset = *v; WALL_SET (offset); }
	if (Has (spec, "referenceLine")) {
		wall.referenceLineLocation = (API_WallReferenceLineLocationID) ParseNamed (kRefLines, spec, "referenceLine");
		WALL_SET (referenceLineLocation);
	}
	if (Has (spec, "zoneRelation")) {
		wall.zoneRel = (API_ZoneRelID) ParseNamed (kZoneRels, spec, "zoneRelation");
		WALL_SET (zoneRel);
	}
	if (ApplyStructure (spec, wall.modelElemStructureType, wall.buildingMaterial, wall.composite, &wall.profileAttr)) {
		WALL_SET (modelElemStructureType); WALL_SET (buildingMaterial); WALL_SET (composite); WALL_SET (profileAttr);
		if (wall.modelElemStructureType == API_ProfileStructure) {
			wall.profileType = APISect_Poly; WALL_SET (profileType);
		} else if (wall.profileType == APISect_Poly) {
			wall.profileType = APISect_Normal; WALL_SET (profileType);
		}
	}
	if (ApplyOverriddenSurface (spec, "refSurface", wall.refMat))	{ WALL_SET (refMat); }
	if (ApplyOverriddenSurface (spec, "oppSurface", wall.oppMat))	{ WALL_SET (oppMat); }
	if (ApplyOverriddenSurface (spec, "sideSurface", wall.sidMat))	{ WALL_SET (sidMat); }
	if (auto v = OptAngle (spec, "slantAlpha"))	{ wall.slantAlpha = *v; WALL_SET (slantAlpha); }
	if (auto v = OptAngle (spec, "slantBeta"))		{ wall.slantBeta = *v; WALL_SET (slantBeta); }
	if (Has (spec, "slantAlpha") || Has (spec, "slantBeta")) {
		const bool alphaVertical = std::fabs (wall.slantAlpha - kPi / 2) < 1e-6;
		const bool betaVertical = std::fabs (wall.slantBeta - kPi / 2) < 1e-6;
		wall.profileType = (alphaVertical && betaVertical) ? APISect_Normal : (betaVertical || std::fabs (wall.slantAlpha - wall.slantBeta) < 1e-9 ? APISect_Slanted : APISect_Trapez);
		WALL_SET (profileType);
	}
#undef WALL_SET
}


API_Guid CreateWall (const OS& spec)
{
	API_Element element = NewElement (API_WallID);
	GetDefaults (element, nullptr);

	if (!Has (spec, "begin") || !Has (spec, "end"))
		Fail ("Wall requires 'begin' and 'end' points.");

	ApplyCommonFields (element, nullptr, spec);
	// Which side of begin->end the body lies on depends on 'flipped' together with the reference line.
	// The Wall tool default for 'flipped' varies (template, favorites), so new walls are deterministic:
	// flipped = false unless given -> referenceLine "Outside" = body to the RIGHT of begin->end (verified live).
	element.wall.flipped = false;
	ApplyWallFields (element.wall, nullptr, spec);

	Check (ACAPI_Element_Create (&element, nullptr), "Cannot create wall");
	return element.header.guid;
}


void SerializeWall (const API_Element& element, OS& out)
{
	const API_WallType& wall = element.wall;
	out.Add ("begin", CoordObj (wall.begC));
	out.Add ("end", CoordObj (wall.endC));
	const double chord = std::hypot (wall.endC.x - wall.begC.x, wall.endC.y - wall.begC.y);
	double length = chord;
	if (std::fabs (wall.angle) > 1e-9) {
		const double radius = chord / (2.0 * std::sin (std::fabs (wall.angle) / 2.0));
		length = radius * std::fabs (wall.angle);
	}
	out.Add ("length", length);
	out.Add ("height", wall.height);
	out.Add ("thickness", wall.thickness);
	if (wall.type == APIWtyp_Trapez)
		out.Add ("endThickness", wall.thickness1);
	out.Add ("wallType", GS::UniString (wall.type == APIWtyp_Normal ? "Normal" : wall.type == APIWtyp_Trapez ? "Trapezoid" : "Polygonal"));
	out.Add ("bottomOffset", wall.bottomOffset);
	out.Add ("topLinkedStory", (Int32) wall.relativeTopStory);
	out.Add ("topOffset", wall.topOffset);
	AddAngle (out, "arcAngle", wall.angle);
	out.Add ("referenceLine", NameOf (kRefLines, wall.referenceLineLocation));
	out.Add ("flipped", wall.flipped);
	out.Add ("offset", wall.offset);
	out.Add ("zoneRelation", NameOf (kZoneRels, wall.zoneRel));
	AddStructureJson (out, wall.modelElemStructureType, wall.buildingMaterial, wall.composite, wall.profileAttr);
	AddOverriddenSurfaceJson (out, "refSurface", wall.refMat);
	AddOverriddenSurfaceJson (out, "oppSurface", wall.oppMat);
	AddOverriddenSurfaceJson (out, "sideSurface", wall.sidMat);
	AddAngle (out, "slantAlpha", wall.slantAlpha);
	AddAngle (out, "slantBeta", wall.slantBeta);

	GS::Array<API_Guid> windows, doors;
	ACAPI_Element_GetConnectedElements (element.header.guid, API_WindowID, &windows);
	ACAPI_Element_GetConnectedElements (element.header.guid, API_DoorID, &doors);
	GS::Array<GS::UniString> w, d;
	for (const API_Guid& g : windows) w.Push (GuidStr (g));
	for (const API_Guid& g : doors) d.Push (GuidStr (g));
	out.Add ("windows", w);
	out.Add ("doors", d);
}


void ModifyWall (API_Element& element, API_Element& mask, API_ElementMemo&, UInt64&, const OS& patch)
{
	ApplyWallFields (element.wall, &mask, patch);
}

} // namespace


void RegisterWallCommands ()
{
	RegisterAdapter ({ API_WallID, CreateWall, SerializeWall, ModifyWall });
}

} // namespace cc
