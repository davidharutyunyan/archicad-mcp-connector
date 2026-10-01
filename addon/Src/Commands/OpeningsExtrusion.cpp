// *****************************************************************************
// OpeningsExtrusion — adapter for the Opening tool (API_OpeningID, AC23+): a rectangular
// or circular extrusion body that cuts its host element (wall, slab, roof, shell, beam, ...).
//
// Create / modify fields (meters, degrees):
//   owner*            GUID of the host element (alias "host"; cannot be changed afterwards)
//   position          wall hosts: distance of the ANCHOR point along the wall reference line
//   point             {x, y, z?}: plan position of the anchor point (projected onto the wall
//                     reference line for wall hosts); z = anchor elevation above the home story
//   bottomElevation   elevation of the opening's bottom edge above the home story (horizontal /
//                     aligned constraint, e.g. wall openings)  |  anchorAltitude (anchor point)
//   shape             "Rectangular" | "Circular"; width*, height* (circular: diameter), linkedSize
//   anchor            which point of the base shape sits at the anchor point: "TopLeft" |
//                     "TopCenter" | "TopRight" | "CenterLeft" | "Center" (default) | "CenterRight" |
//                     "BottomLeft" | "BottomCenter" | "BottomRight"
//   constraint        "Horizontal" (default for walls/beams/columns) | "Vertical" (default for
//                     slabs/roofs/shells/meshes) | "Aligned" | "Free"
//   extrusionDirection {x,y,z}, xAxis {x,y,z}, rotation (deg about the extrusion direction)
//   limit             "Infinite" | "Finite" | "HalfInfinite"; startOffset, depth (Finite length)
//   floorPlanDisplay  "Symbolic" | "SymbolicCut" | "SymbolicOverhead"; connectionMode
//                     "Connected" | "Disconnected"; showReferenceAxis
//   + common: layer, renovationStatus, elementId   (the story is always the host's story)
// *****************************************************************************

#include "Commands/Openings.hpp"
#include "Core/Command.hpp"
#include "Core/Elements.hpp"
#include "Core/Enums.hpp"

#include <cmath>

namespace cc {
namespace openings {

namespace {

const NamedValue kShapes[] = {
	{ "Rectangular",		API_OpeningBasePolygonRectangular },
	{ "Circular",			API_OpeningBasePolygonCircular },
	{ "Custom",				API_OpeningBasePolygonCustom },			// output only (not creatable via the AC26 API)
};

const NamedValue kConstraints[] = {
	{ "Vertical",			API_OpeningForcedVertical },
	{ "Horizontal",			API_OpeningForcedHorizontal },
	{ "Aligned",			API_OpeningAligned },
	{ "Free",				API_OpeningFree },
};

const NamedValue kLimits[] = {
	{ "Infinite",			API_OpeningLimitInfinite },
	{ "Finite",				API_OpeningLimitFinite },
	{ "HalfInfinite",		API_OpeningLimitHalfInfinite },
};

const NamedValue kAnchors[] = {
	{ "TopLeft",			APIAnc_LT },
	{ "TopCenter",			APIAnc_MT },
	{ "TopRight",			APIAnc_RT },
	{ "CenterLeft",			APIAnc_LM },
	{ "Center",				APIAnc_MM },
	{ "CenterRight",		APIAnc_RM },
	{ "BottomLeft",			APIAnc_LB },
	{ "BottomCenter",		APIAnc_MB },
	{ "BottomRight",		APIAnc_RB },
};

const NamedValue kFloorPlanModes[] = {
	{ "Symbolic",			API_OpeningSymbolic },
	{ "SymbolicCut",		API_OpeningSymbolicCut },
	{ "SymbolicOverhead",	API_OpeningSymbolicOverhead },
};

const NamedValue kConnectionModes[] = {
	{ "Disconnected",		API_OpeningDisconnected },
	{ "Connected",			API_OpeningConnected },
};

const NamedValue kOutlineStyles[] = {
	{ "HideBorder",			API_OpeningHideBorder },
	{ "ShowUncutBorder",	API_OpeningShowUncutBorder },
	{ "ShowOverheadBorder",	API_OpeningShowOverheadBorder },
};

// --- Small 3D vector helpers -------------------------------------------------------------

API_Vector3D Vec (double x, double y, double z)
{
	API_Vector3D v;
	v.x = x; v.y = y; v.z = z;
	return v;
}

double Dot (const API_Vector3D& a, const API_Vector3D& b)	{ return a.x * b.x + a.y * b.y + a.z * b.z; }
double Len (const API_Vector3D& a)							{ return std::sqrt (Dot (a, a)); }

API_Vector3D Cross (const API_Vector3D& a, const API_Vector3D& b)
{
	return Vec (a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

API_Vector3D Scale (const API_Vector3D& a, double s)	{ return Vec (a.x * s, a.y * s, a.z * s); }
API_Vector3D Add (const API_Vector3D& a, const API_Vector3D& b)	{ return Vec (a.x + b.x, a.y + b.y, a.z + b.z); }

API_Vector3D Normalized (const API_Vector3D& a, const char* field)
{
	const double l = Len (a);
	if (l < 1e-9)
		Fail ("'" + ToUni (field) + "' must be a non-zero vector {x, y, z}.");
	return Scale (a, 1.0 / l);
}

// --- Host --------------------------------------------------------------------------------

struct HostInfo {
	API_Guid					guid = APINULLGuid;
	API_ElemTypeID				typeID = API_ZombieElemID;
	short						floorInd = 0;
	std::optional<WallGeometry>	wall;			// straight / curved (non-polygonal) walls
	std::optional<double>		direction;		// horizontal direction of a linear host (radians)
};


HostInfo LoadHost (const API_Guid& guid)
{
	const API_Element e = GetElement (guid);
	HostInfo h;
	h.guid = guid;
	h.typeID = e.header.type.typeID;
	h.floorInd = e.header.floorInd;
	if (h.typeID == API_WallID) {
		if (e.wall.type != APIWtyp_Poly) {
			h.wall = LoadHostWall (guid, "opening");
			if (!h.wall->curved)
				h.direction = std::atan2 (e.wall.endC.y - e.wall.begC.y, e.wall.endC.x - e.wall.begC.x);
		}
	} else if (h.typeID == API_BeamID) {
		h.direction = std::atan2 (e.beam.endC.y - e.beam.begC.y, e.beam.endC.x - e.beam.begC.x);
	}
	return h;
}


std::optional<API_OpeningConstraintTypeID> DefaultConstraint (API_ElemTypeID hostType)
{
	switch (hostType) {
		case API_WallID:
		case API_BeamID:
		case API_ColumnID:	return API_OpeningForcedHorizontal;
		case API_SlabID:
		case API_RoofID:
		case API_ShellID:
		case API_MeshID:	return API_OpeningForcedVertical;
		default:			return std::nullopt;
	}
}


// Height of the anchor point above the bottom edge of the base shape.
double AnchorHeightOffset (API_AnchorID anchor, double height)
{
	switch (anchor) {
		case APIAnc_LT: case APIAnc_MT: case APIAnc_RT:	return height;
		case APIAnc_LM: case APIAnc_MM: case APIAnc_RM:	return height / 2.0;
		default:											return 0.0;
	}
}


bool UsesAnchorAltitude (API_OpeningConstraintTypeID c)
{
	return c == API_OpeningForcedHorizontal || c == API_OpeningAligned;
}

// --- Apply -------------------------------------------------------------------------------

void ApplyOpeningFields (API_OpeningType& op, API_Element* mask, const OS& spec, const HostInfo& host, bool creating)
{
#define OP_SET(field) if (mask) ACAPI_ELEMENT_MASK_SET (*mask, API_OpeningType, field)
#define PAR_SET(field) OP_SET (extrusionGeometryData.parameters.field)
	API_OpeningExtrusionParameters& par = op.extrusionGeometryData.parameters;
	API_Plane3D& frame = op.extrusionGeometryData.frame;

	// --- shape and size
	if (spec.Contains ("shape")) {
		const Int32 shape = ParseNamed (kShapes, spec, "shape");
		if (shape == API_OpeningBasePolygonCustom)
			Fail ("Custom (polygonal) opening shapes cannot be created through the Archicad 26 API. Use 'Rectangular' or 'Circular'.", APIERR_NOTSUPPORTED);
		par.basePolygonType = (API_OpeningBasePolygonTypeTypeID) shape;
		PAR_SET (basePolygonType);
	}
	const bool circular = par.basePolygonType == API_OpeningBasePolygonCircular;
	auto width = OptDouble (spec, "width");
	auto height = OptDouble (spec, "height");
	if (circular && width.has_value () && !height.has_value ()) height = width;
	if (circular && height.has_value () && !width.has_value ()) width = height;
	if (width.has_value ()) {
		if (*width <= 0.0) Fail ("'width' must be greater than 0 (meters).");
		par.width = *width; PAR_SET (width);
	}
	if (height.has_value ()) {
		if (*height <= 0.0) Fail ("'height' must be greater than 0 (meters).");
		par.height = *height; PAR_SET (height);
	}
	if (auto v = OptBool (spec, "linkedSize")) {
		par.linkedStatus = *v ? API_OpeningLinked : API_OpeningNotLinked;
		PAR_SET (linkedStatus);
	}
	if (spec.Contains ("anchor")) {
		par.anchor = (API_AnchorID) ParseNamed (kAnchors, spec, "anchor");
		PAR_SET (anchor);
	}
	if (spec.Contains ("constraint")) {
		par.constraint = (API_OpeningConstraintTypeID) ParseNamed (kConstraints, spec, "constraint");
		PAR_SET (constraint);
	}

	// --- extrusion limits
	if (spec.Contains ("limit")) {
		par.limitType = (API_OpeningLimitTypeTypeID) ParseNamed (kLimits, spec, "limit");
		PAR_SET (limitType);
	}
	if (auto v = OptDouble (spec, "startOffset"))	{ par.extrusionStartOffset = *v; PAR_SET (extrusionStartOffset); }
	if (auto v = OptDouble (spec, "depth")) {
		if (*v <= 0.0) Fail ("'depth' must be greater than 0 (meters).");
		par.finiteBodyLength = *v; PAR_SET (finiteBodyLength);
	}

	// --- floor plan display
	if (spec.Contains ("floorPlanDisplay")) {
		op.floorPlanParameters.floorPlanDisplayMode = (API_OpeningFloorPlanDisplayModeTypeID) ParseNamed (kFloorPlanModes, spec, "floorPlanDisplay");
		OP_SET (floorPlanParameters.floorPlanDisplayMode);
	}
	if (spec.Contains ("connectionMode")) {
		op.floorPlanParameters.connectionMode = (API_OpeningFloorPlanConnectionModeTypeID) ParseNamed (kConnectionModes, spec, "connectionMode");
		OP_SET (floorPlanParameters.connectionMode);
	}
	if (spec.Contains ("outlinesStyle")) {
		op.floorPlanParameters.outlinesParameters.outlinesStyle = (API_OpeningFloorPlanOutlinesStyleTypeID) ParseNamed (kOutlineStyles, spec, "outlinesStyle");
		OP_SET (floorPlanParameters.outlinesParameters.outlinesStyle);
	}
	if (auto v = OptBool (spec, "showReferenceAxis")) {
		op.floorPlanParameters.referenceAxisParameters.showReferenceAxis = *v;
		OP_SET (floorPlanParameters.referenceAxisParameters.showReferenceAxis);
	}

	// --- placement (anchor point)
	const double storyLevel = StoryLevel (host.floorInd);
	API_Coord3D origin = frame.basePoint;
	bool originChanged = false;
	std::optional<double> tangent;						// horizontal host direction at the anchor
	std::optional<double> pointZ;

	if (spec.Contains ("position")) {
		if (spec.Contains ("point"))
			Fail ("Give either 'position' (distance along the wall) or 'point' ({x,y,z?}), not both.");
		if (!host.wall.has_value ())
			Fail ("'position' is only available for openings in straight or curved walls; give 'point' ({x, y, z?}) instead.");
		const double distance = GetDouble (spec, "position");
		CheckDistanceOnWall (*host.wall, distance, "The opening anchor ('position')");
		API_Coord p;
		double t = 0.0;
		if (!PointOnWall (*host.wall, distance, p, t))
			Fail ("The host wall is degenerate (zero length); 'position' cannot be resolved.");
		origin.x = p.x;
		origin.y = p.y;
		tangent = t;
		originChanged = true;
	} else if (spec.Contains ("point")) {
		const OS pt = GetObject (spec, "point");
		API_Coord p = CoordFrom (pt);
		if (host.wall.has_value ()) {
			const WallProjection proj = ProjectOntoWall (*host.wall, p, "point");
			p = proj.point;
			tangent = proj.tangentAngle;
		}
		origin.x = p.x;
		origin.y = p.y;
		if (pt.Contains ("z"))
			pointZ = GetDouble (pt, "z");
		originChanged = true;
	}
	if (!tangent.has_value () && host.direction.has_value ())
		tangent = host.direction;

	// --- vertical position
	std::optional<double> altitude;
	if (auto v = OptDouble (spec, "anchorAltitude")) {
		if (spec.Contains ("bottomElevation"))
			Fail ("Give either 'bottomElevation' or 'anchorAltitude', not both.");
		altitude = *v;
	} else if (auto b = OptDouble (spec, "bottomElevation")) {
		altitude = *b + AnchorHeightOffset (par.anchor, par.height);
	} else if (pointZ.has_value ()) {
		altitude = *pointZ;
	}
	if (altitude.has_value ()) {
		par.anchorAltitude = *altitude;
		PAR_SET (anchorAltitude);
		origin.z = storyLevel + *altitude;
		originChanged = true;
	} else if (creating) {
		origin.z = storyLevel + (UsesAnchorAltitude (par.constraint) ? par.anchorAltitude : 0.0);
	}

	if (originChanged || creating) {
		frame.basePoint = origin;
		OP_SET (extrusionGeometryData.frame.basePoint.x);
		OP_SET (extrusionGeometryData.frame.basePoint.y);
		OP_SET (extrusionGeometryData.frame.basePoint.z);
	}

	// --- orientation (frame axes, right-handed: extrusionDirection = xAxis x yAxis)
	const bool orientationGiven = spec.Contains ("extrusionDirection") || spec.Contains ("xAxis") || spec.Contains ("rotation");
	const bool followCurvedWall = originChanged && host.wall.has_value () && host.wall->curved && tangent.has_value ();
	if (creating || orientationGiven || spec.Contains ("constraint") || followCurvedWall) {
		const API_Vector3D up = Vec (0.0, 0.0, 1.0);
		API_Vector3D axisX, axisY, axisZ;
		if (spec.Contains ("extrusionDirection")) {
			axisZ = Normalized (Coord3DFrom (GetObject (spec, "extrusionDirection")), "extrusionDirection");
		} else if (!creating && !spec.Contains ("constraint") && !followCurvedWall) {
			axisZ = Normalized (frame.axisZ, "extrusionDirection");
		} else if (par.constraint == API_OpeningForcedVertical) {
			axisZ = up;
		} else if (tangent.has_value ()) {
			// horizontal extrusion perpendicular to the host (wall / beam) direction
			axisZ = Cross (Vec (std::cos (*tangent), std::sin (*tangent), 0.0), up);
		} else if (!creating) {
			axisZ = Normalized (frame.axisZ, "extrusionDirection");
		} else if (DefaultConstraint (host.typeID) == API_OpeningForcedVertical) {
			axisZ = up;											// planar hosts (slab, roof, shell, mesh): through the host
		} else {
			axisZ = Vec (0.0, -1.0, 0.0);						// e.g. columns: along project Y
		}

		if (spec.Contains ("xAxis")) {
			const API_Vector3D given = Coord3DFrom (GetObject (spec, "xAxis"));
			axisX = Add (given, Scale (axisZ, -Dot (given, axisZ)));	// orthogonalize against the extrusion direction
			axisX = Normalized (axisX, "xAxis (component perpendicular to extrusionDirection)");
		} else if (std::fabs (axisZ.z) > 0.999) {
			axisX = Vec (1.0, 0.0, 0.0);								// vertical extrusion: x along project X
		} else {
			// horizontal-ish extrusion: y points up, x lies horizontally
			const API_Vector3D y = Normalized (Add (up, Scale (axisZ, -Dot (up, axisZ))), "yAxis");
			axisX = Cross (y, axisZ);
		}
		axisY = Cross (axisZ, axisX);
		if (auto rot = OptAngle (spec, "rotation")) {
			const API_Vector3D x = Add (Scale (axisX, std::cos (*rot)), Scale (axisY, std::sin (*rot)));
			axisX = x;
			axisY = Cross (axisZ, axisX);
		}
		frame.axisX = axisX;
		frame.axisY = axisY;
		frame.axisZ = axisZ;
		OP_SET (extrusionGeometryData.frame.axisX.x); OP_SET (extrusionGeometryData.frame.axisX.y); OP_SET (extrusionGeometryData.frame.axisX.z);
		OP_SET (extrusionGeometryData.frame.axisY.x); OP_SET (extrusionGeometryData.frame.axisY.y); OP_SET (extrusionGeometryData.frame.axisY.z);
		OP_SET (extrusionGeometryData.frame.axisZ.x); OP_SET (extrusionGeometryData.frame.axisZ.y); OP_SET (extrusionGeometryData.frame.axisZ.z);
	}
#undef PAR_SET
#undef OP_SET
}


API_Guid CreateOpening (const OS& spec)
{
	const std::optional<API_Guid> ownerGuid = OptGuidAny (spec, { "owner", "host" });
	if (!ownerGuid.has_value ())
		Fail ("An opening needs 'owner': the GUID of the element it cuts (wall, slab, roof, shell, beam, column, mesh ...).");
	if (!spec.Contains ("position") && !spec.Contains ("point"))
		Fail ("An opening needs 'point' ({x, y, z?}: plan position of its anchor) or, in a wall, 'position' (m along the wall reference line).");
	const HostInfo host = LoadHost (*ownerGuid);

	API_Element element = NewElement (API_OpeningID);
	GetDefaults (element, nullptr);
	ApplyCommonFields (element, nullptr, spec);
	element.header.floorInd = host.floorInd;			// always the host's home story

	API_OpeningType& op = element.opening;
	op.owner = *ownerGuid;
	API_OpeningExtrusionParameters& par = op.extrusionGeometryData.parameters;
	if (par.basePolygonType == API_OpeningBasePolygonCustom)
		par.basePolygonType = API_OpeningBasePolygonRectangular;
	if (!spec.Contains ("anchor"))
		par.anchor = APIAnc_MM;
	if (!spec.Contains ("constraint")) {
		if (auto c = DefaultConstraint (host.typeID))
			par.constraint = *c;
	}
	const bool circularRequested = spec.Contains ("shape") && EqualsIgnoreCase (GetString (spec, "shape"), "Circular");
	if (!spec.Contains ("width") || (!spec.Contains ("height") && !circularRequested))
		Fail ("An opening needs 'width' and 'height' in meters (a circular opening needs only 'width' = diameter).");

	ApplyOpeningFields (op, nullptr, spec, host, true);

	FloorPlanDatabaseScope floorPlan;
	const GSErrCode err = ACAPI_Element_Create (&element, nullptr);
	if (err != NoError) {
		GS::UniString hint;
		if (err == APIERR_BADPARS || err == APIERR_REFUSEDPAR || err == APIERR_GENERAL)
			hint = " Check that 'owner' is an element the Opening tool can cut (wall, slab, roof, shell, beam, column, mesh) and that the anchor point lies on it.";
		else if (err == APIERR_NOTEDITABLE || err == APIERR_LOCKEDLAY || err == APIERR_HIDDENLAY)
			hint = " The host or the target layer is not editable (locked / hidden layer, or not reserved in Teamwork).";
		Fail ("Cannot create opening: " + ErrorName (err) + "." + hint, err);
	}
	return element.header.guid;
}


void SerializeOpening (const API_Element& element, OS& out)
{
	const API_OpeningType& op = element.opening;
	const API_OpeningExtrusionParameters& par = op.extrusionGeometryData.parameters;
	const API_Plane3D& frame = op.extrusionGeometryData.frame;

	out.Add ("owner", GuidStr (op.owner));
	API_Elem_Head head;
	BNZeroMemory (&head, sizeof (head));
	head.guid = op.owner;
	const bool hostOk = op.owner != APINULLGuid && ACAPI_Element_GetHeader (&head) == NoError;
	if (hostOk)
		out.Add ("ownerType", ElemTypeName (head.type));

	out.Add ("shape", NameOf (kShapes, par.basePolygonType));
	if (par.basePolygonType != API_OpeningBasePolygonCustom) {
		out.Add ("width", par.width);
		out.Add ("height", par.height);
		out.Add ("linkedSize", par.linkedStatus == API_OpeningLinked);
	}
	out.Add ("anchor", NameOf (kAnchors, par.anchor));
	out.Add ("constraint", NameOf (kConstraints, par.constraint));
	if (UsesAnchorAltitude (par.constraint)) {
		out.Add ("anchorAltitude", par.anchorAltitude);
		out.Add ("bottomElevation", par.anchorAltitude - AnchorHeightOffset (par.anchor, par.height));
	}
	out.Add ("limit", NameOf (kLimits, par.limitType));
	if (par.limitType != API_OpeningLimitInfinite)
		out.Add ("startOffset", par.extrusionStartOffset);
	if (par.limitType == API_OpeningLimitFinite)
		out.Add ("depth", par.finiteBodyLength);

	out.Add ("point", CoordObj (frame.basePoint.x, frame.basePoint.y));
	out.Add ("frame", OS ("origin", Coord3DObj (frame.basePoint),
						  "xAxis", Coord3DObj (frame.axisX),
						  "yAxis", Coord3DObj (frame.axisY),
						  "extrusionDirection", Coord3DObj (frame.axisZ)));
	if (hostOk && head.type.typeID == API_WallID) {
		try {
			API_Coord p;
			p.x = frame.basePoint.x;
			p.y = frame.basePoint.y;
			const WallGeometry wall = LoadHostWall (op.owner, "opening");
			out.Add ("position", ProjectOntoWall (wall, p, "anchor").distance);
		} catch (const Error&) {
			// anchor outside the wall range or polygonal wall: no position
		}
	}

	const API_OpeningFloorPlanParameters& fp = op.floorPlanParameters;
	out.Add ("floorPlanDisplay", NameOf (kFloorPlanModes, fp.floorPlanDisplayMode));
	out.Add ("connectionMode", NameOf (kConnectionModes, fp.connectionMode));
	out.Add ("outlinesStyle", NameOf (kOutlineStyles, fp.outlinesParameters.outlinesStyle));
	out.Add ("showReferenceAxis", fp.referenceAxisParameters.showReferenceAxis);
}


void ModifyOpening (API_Element& element, API_Element& mask, API_ElementMemo&, UInt64&, const OS& patch)
{
	API_OpeningType& op = element.opening;
	if (auto newOwner = OptGuidAny (patch, { "owner", "host" }); newOwner.has_value () && *newOwner != op.owner)
		Fail ("An opening cannot be moved to another host element (Archicad does not allow changing its connection). Delete it and create a new one.", APIERR_NOTSUPPORTED);
	HostInfo host;
	host.floorInd = element.header.floorInd;
	if (op.owner != APINULLGuid && (patch.Contains ("position") || patch.Contains ("point") || patch.Contains ("constraint") ||
									patch.Contains ("extrusionDirection") || patch.Contains ("xAxis") || patch.Contains ("rotation")))
		host = LoadHost (op.owner);
	ApplyOpeningFields (op, &mask, patch, host, false);
}

} // namespace


void RegisterOpeningToolAdapter ()
{
	RegisterAdapter ({ API_OpeningID, CreateOpening, SerializeOpening, ModifyOpening });
}

} // namespace openings
} // namespace cc
