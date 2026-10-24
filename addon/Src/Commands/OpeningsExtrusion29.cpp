// *****************************************************************************
// OpeningsExtrusion29 — the Opening tool (API_OpeningID) on Archicad 29 and newer, where the opening element left
// API_Element and is handled through the C++ API (ACAPI::Element::Opening / OpeningDefault). Same JSON as the
// Archicad 26-28 adapter in OpeningsExtrusion.cpp (see the field list there), plus:
//   owner can be changed on Archicad 29 (the opening moves to the new host);
//   cutElements [guid] (create; output: all elements the opening cuts besides its owner), addCutElements / removeCutElements
//   [guid] (modify): an Archicad 29 opening can cut further elements than its host.
// Placement: the opening default is set up from the spec, then placed into the host at the anchor point; an explicit
// orientation (extrusionDirection / xAxis / rotation) is applied afterwards as a transformation of the opening.
// *****************************************************************************

#if CC_AC_VERSION >= 29

#include "Commands/Openings.hpp"
#include "Core/Command.hpp"
#include "Core/Elements.hpp"
#include "Core/Enums.hpp"

#include "ACAPI/Element/Opening/Opening.hpp"
#include "ACAPI/Element/Opening/OpeningDefault.hpp"
#include "ACAPI/Element/Opening/OpeningExtrusionParameters.hpp"
#include "ACAPI/Element/Opening/OpeningFloorPlanParameters.hpp"
#include "ACAPI/Element/Opening/OpeningGeometry.hpp"

#include <cmath>
#include <exception>

namespace cc {
namespace openings {

namespace {

namespace AE = ACAPI::Element;

const NamedValue kShapes[] = {
	{ "Rectangular",		(Int32) AE::ShapeType::Rectangular },
	{ "Circular",			(Int32) AE::ShapeType::Circular },
	{ "Custom",				(Int32) AE::ShapeType::Polygonal },			// output only
};

const NamedValue kConstraints[] = {
	{ "Vertical",			(Int32) AE::Constraint::ForcedVertical },
	{ "Horizontal",			(Int32) AE::Constraint::ForcedHorizontal },
	{ "Aligned",			(Int32) AE::Constraint::Aligned },
	{ "Free",				(Int32) AE::Constraint::Free },
};

const NamedValue kLimits[] = {
	{ "Infinite",			(Int32) AE::LimitType::Infinite },
	{ "Finite",				(Int32) AE::LimitType::Finite },
	{ "HalfInfinite",		(Int32) AE::LimitType::HalfInfinite },
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
	{ "Symbolic",			(Int32) AE::OpeningFloorPlanDisplayMode::Symbolic },
	{ "SymbolicCut",		(Int32) AE::OpeningFloorPlanDisplayMode::SymbolicCut },
	{ "SymbolicOverhead",	(Int32) AE::OpeningFloorPlanDisplayMode::SymbolicOverhead },
	{ "Projected",			(Int32) AE::OpeningFloorPlanDisplayMode::Projected },
};

const NamedValue kConnectionModes[] = {
	{ "Disconnected",		(Int32) AE::DividedOpeningMode::Disconnected },
	{ "Connected",			(Int32) AE::DividedOpeningMode::Connected },
};

const NamedValue kOutlineStyles[] = {
	{ "HideBorder",			(Int32) AE::OpeningOpenEdgeStyle::Hide },
	{ "ShowUncutBorder",	(Int32) AE::OpeningOpenEdgeStyle::ShowUncut },
	{ "ShowOverheadBorder",	(Int32) AE::OpeningOpenEdgeStyle::ShowOverhead },
};

// --- Results of the C++ API ---------------------------------------------------------------

GS::UniString ApiErrorText (const ACAPI::Error& e)
{
	return GS::UniString (e.text.c_str (), CC_UTF8) + " (" + ErrorName (e.kind) + ")";
}


void MustOk (const ACAPI::Result<void>& r, const GS::UniString& what)
{
	if (r.IsErr ())
		Fail (what + ": " + ApiErrorText (r.UnwrapErr ()) + ".", r.UnwrapErr ().kind);
}


template <typename T>
T MustGet (ACAPI::Result<T>&& r, const GS::UniString& what)
{
	if (r.IsErr ())
		Fail (what + ": " + ApiErrorText (r.UnwrapErr ()) + ".", r.UnwrapErr ().kind);
	return std::move (r).Unwrap ();
}


ACAPI::UniqueID IdOf (const API_Guid& guid)
{
	return ACAPI::UniqueID (guid, ACAPI_GetToken ());
}

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

API_Vector3D Scale (const API_Vector3D& a, double s)				{ return Vec (a.x * s, a.y * s, a.z * s); }
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


std::optional<AE::Constraint> DefaultConstraint (API_ElemTypeID hostType)
{
	switch (hostType) {
		case API_WallID:
		case API_BeamID:
		case API_ColumnID:	return AE::Constraint::ForcedHorizontal;
		case API_SlabID:
		case API_RoofID:
		case API_ShellID:
		case API_MeshID:	return AE::Constraint::ForcedVertical;
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


bool UsesAnchorAltitude (AE::Constraint c)
{
	return c == AE::Constraint::ForcedHorizontal || c == AE::Constraint::Aligned;
}

// --- Parameters ----------------------------------------------------------------------------

// Parameters that do not depend on the placement; shared by the opening default (create) and existing openings.
void ApplyParameters (AE::ModifiableOpeningExtrusionParameters& par, AE::ModifiableOpeningFloorPlanParameters& fp, const OS& spec)
{
	if (spec.Contains ("shape")) {
		const AE::ShapeType shape = (AE::ShapeType) ParseNamed (kShapes, spec, "shape");
		if (shape == AE::ShapeType::Polygonal)
			Fail ("Custom (polygonal) opening shapes are not supported by the connector: use 'Rectangular' or 'Circular'.", APIERR_NOTSUPPORTED);
		par.SetShapeType (shape);
	}
	const bool circular = par.GetShapeType () == AE::ShapeType::Circular;
	auto width = OptDouble (spec, "width");
	auto height = OptDouble (spec, "height");
	if (circular && width.has_value () && !height.has_value ()) height = width;
	if (circular && height.has_value () && !width.has_value ()) width = height;
	// The tool default may link the sizes (either one then sets both): unlink for different width and height.
	const bool differentSizes = width.has_value () && height.has_value () && std::fabs (*width - *height) > 1e-9;
	if (auto v = OptBool (spec, "linkedSize")) {
		if (*v && differentSizes)
			Fail ("'linkedSize': true needs equal 'width' and 'height'.");
		MustOk (par.SetLinkedStatus (*v ? AE::LinkedStatus::Linked : AE::LinkedStatus::NotLinked), "Cannot set 'linkedSize'");
	} else if (differentSizes) {
		MustOk (par.SetLinkedStatus (AE::LinkedStatus::NotLinked), "Cannot unlink width and height");
	}
	if (width.has_value ()) {
		if (*width <= 0.0) Fail ("'width' must be greater than 0 (meters).");
		MustOk (par.SetWidth (*width), "Cannot set the opening width");
	}
	if (height.has_value ()) {
		if (*height <= 0.0) Fail ("'height' must be greater than 0 (meters).");
		MustOk (par.SetHeight (*height), "Cannot set the opening height");
	}
	if (spec.Contains ("anchor"))
		par.SetAnchor ((API_AnchorID) ParseNamed (kAnchors, spec, "anchor"));
	if (spec.Contains ("constraint"))
		MustOk (par.SetConstraint ((AE::Constraint) ParseNamed (kConstraints, spec, "constraint")), "Cannot set the constraint");

	if (spec.Contains ("limit"))
		par.SetLimitType ((AE::LimitType) ParseNamed (kLimits, spec, "limit"));
	if (auto v = OptDouble (spec, "startOffset"))
		MustOk (par.SetExtrusionStartOffset (*v), "Cannot set 'startOffset'");
	if (auto v = OptDouble (spec, "depth")) {
		if (*v <= 0.0) Fail ("'depth' must be greater than 0 (meters).");
		MustOk (par.SetFiniteBodyLength (*v), "Cannot set 'depth'");
	}

	if (spec.Contains ("floorPlanDisplay"))
		fp.SetFloorPlanDisplayMode ((AE::OpeningFloorPlanDisplayMode) ParseNamed (kFloorPlanModes, spec, "floorPlanDisplay"));
	if (spec.Contains ("connectionMode"))
		fp.SetDividedOpeningMode ((AE::DividedOpeningMode) ParseNamed (kConnectionModes, spec, "connectionMode"));
	if (spec.Contains ("outlinesStyle"))
		fp.SetOpenEdgeStyle ((AE::OpeningOpenEdgeStyle) ParseNamed (kOutlineStyles, spec, "outlinesStyle"));
	if (auto v = OptBool (spec, "showReferenceAxis")) {
		if (*v)
			fp.ShowReferenceAxisOn ();
		else
			fp.ShowReferenceAxisOff ();
	}
}


// Anchor altitude requested by the spec (anchorAltitude | bottomElevation | point.z), for the given anchor and height.
std::optional<double> RequestedAltitude (const OS& spec, API_AnchorID anchor, double height)
{
	if (auto v = OptDouble (spec, "anchorAltitude")) {
		if (spec.Contains ("bottomElevation"))
			Fail ("Give either 'bottomElevation' or 'anchorAltitude', not both.");
		return *v;
	}
	if (auto b = OptDouble (spec, "bottomElevation"))
		return *b + AnchorHeightOffset (anchor, height);
	if (spec.Contains ("point")) {
		const OS pt = GetObject (spec, "point");
		if (pt.Contains ("z"))
			return GetDouble (pt, "z");
	}
	return std::nullopt;
}


// Plan position of the anchor requested by the spec ('position' along a wall | 'point'), with the host tangent there.
std::optional<API_Coord> RequestedPlanPoint (const OS& spec, const HostInfo& host, std::optional<double>& tangent)
{
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
		tangent = t;
		return p;
	}
	if (spec.Contains ("point")) {
		API_Coord p = CoordFrom (GetObject (spec, "point"));
		if (host.wall.has_value ()) {
			const WallProjection proj = ProjectOntoWall (*host.wall, p, "point");
			p = proj.point;
			tangent = proj.tangentAngle;
		}
		return p;
	}
	return std::nullopt;
}

// --- Orientation ---------------------------------------------------------------------------

// Opening frames of Archicad 29 are left-handed: extrusionDirection (z) = yAxis x xAxis, i.e. y = x x z.
struct Frame {
	API_Coord3D		origin;
	API_Vector3D	x, y, z;
};


API_Vector3D FrameY (const API_Vector3D& x, const API_Vector3D& z)
{
	return Cross (x, z);
}


// The orientation the spec asks for (extrusionDirection / xAxis / rotation), starting from `current`. As on Archicad 26-28,
// rotation is measured from the default x axis of the extrusion direction (not added to the current rotation).
Frame RequestedFrame (const OS& spec, const Frame& current)
{
	const API_Vector3D up = Vec (0.0, 0.0, 1.0);
	Frame f = current;
	f.z = spec.Contains ("extrusionDirection") ? Normalized (Coord3DFrom (GetObject (spec, "extrusionDirection")), "extrusionDirection")
											   : Normalized (current.z, "extrusionDirection");
	if (spec.Contains ("xAxis")) {
		const API_Vector3D given = Coord3DFrom (GetObject (spec, "xAxis"));
		f.x = Normalized (Add (given, Scale (f.z, -Dot (given, f.z))), "xAxis (component perpendicular to extrusionDirection)");
	} else if (std::fabs (f.z.z) > 0.999) {
		f.x = Vec (1.0, 0.0, 0.0);										// vertical extrusion: x along project X
	} else {
		const API_Vector3D y = Normalized (Add (up, Scale (f.z, -Dot (up, f.z))), "yAxis");
		f.x = Cross (f.z, y);												// y points up, x lies horizontally
	}
	if (auto rot = OptAngle (spec, "rotation")) {						// counter-clockwise about the extrusion direction
		f.x = Add (Scale (f.x, std::cos (*rot)), Scale (Cross (f.z, f.x), std::sin (*rot)));
	}
	f.y = FrameY (f.x, f.z);
	return f;
}


// Snaps a near-axis-aligned unit vector to the exact axis (and tiny components to 0).
API_Vector3D Snapped (const API_Vector3D& v)
{
	auto snap = [] (double c) { return std::fabs (c) < 1e-9 ? 0.0 : (std::fabs (std::fabs (c) - 1.0) < 1e-9 ? (c > 0 ? 1.0 : -1.0) : c); };
	return Vec (snap (v.x), snap (v.y), snap (v.z));
}


// The rigid transformation that maps frame `from` onto frame `to` (rotation about from.origin + translation).
// Opening::Transform accepts only (numerically) exact rotation matrices, so a turn about the unchanged extrusion
// direction is built from its angle (Rodrigues) instead of from the axes Archicad returned, which carry rounding.
API_Tranmat FrameTransform (const Frame& from, const Frame& to)
{
	double r[3][3] = {};
	if (Len (Add (from.z, Scale (to.z, -1.0))) < 1e-9) {
		const API_Vector3D k = Snapped (from.z);
		const double angle = std::atan2 (Dot (Cross (from.x, to.x), k), Dot (from.x, to.x));
		const double c = std::cos (angle), s = std::sin (angle), v = 1.0 - c;
		r[0][0] = c + k.x * k.x * v;		r[0][1] = k.x * k.y * v - k.z * s;	r[0][2] = k.x * k.z * v + k.y * s;
		r[1][0] = k.y * k.x * v + k.z * s;	r[1][1] = c + k.y * k.y * v;		r[1][2] = k.y * k.z * v - k.x * s;
		r[2][0] = k.z * k.x * v - k.y * s;	r[2][1] = k.z * k.y * v + k.x * s;	r[2][2] = c + k.z * k.z * v;
	} else {
		// R = B * A^T (columns of A and B are the frame axes)
		const API_Vector3D a[3] = { from.x, from.y, from.z };
		const API_Vector3D b[3] = { to.x, to.y, to.z };
		for (int row = 0; row < 3; ++row) {
			for (int col = 0; col < 3; ++col) {
				double sum = 0.0;
				for (int k = 0; k < 3; ++k) {
					const double bk = row == 0 ? b[k].x : row == 1 ? b[k].y : b[k].z;
					const double ak = col == 0 ? a[k].x : col == 1 ? a[k].y : a[k].z;
					sum += bk * ak;
				}
				r[row][col] = sum;
			}
		}
	}
	for (auto& row : r) {
		for (double& e : row)
			e = std::fabs (e) < 1e-12 ? 0.0 : (std::fabs (std::fabs (e) - 1.0) < 1e-12 ? (e > 0 ? 1.0 : -1.0) : e);
	}
	const double o[3] = { from.origin.x, from.origin.y, from.origin.z };
	const double t[3] = { to.origin.x, to.origin.y, to.origin.z };
	API_Tranmat tm = {};
	for (int row = 0; row < 3; ++row) {
		tm.tmx[row * 4 + 0] = r[row][0];
		tm.tmx[row * 4 + 1] = r[row][1];
		tm.tmx[row * 4 + 2] = r[row][2];
		tm.tmx[row * 4 + 3] = t[row] - (r[row][0] * o[0] + r[row][1] * o[1] + r[row][2] * o[2]);
	}
	return tm;
}


// Archicad 29 (build 3100) returns the axis vectors offset by the host's origin (the wall's begin point, the slab's first
// vertex, ...): v = P + e, in a left-handed frame (e_z = e_y x e_x). With a = vx - vz and b = vy - vz the axes follow
// without knowing P: a x b = -(ex + ey + ez), so ez = (-(a x b) - a - b) / 3, ex = ez + a, ey = ez + b (also right
// when P = 0). The mirror-image right-handed solution (+(a x b)) fits the same numbers with another P; it is only used
// when the left-handed one does not give orthonormal axes.
Frame CurrentFrame (const AE::OpeningGeometry& geo)
{
	const API_Vector3D vx = geo.GetVectorX ();
	const API_Vector3D vy = geo.GetVectorY ();
	const API_Vector3D vz = geo.GetExtrusionDirection ();
	const API_Vector3D a = Add (vx, Scale (vz, -1.0));
	const API_Vector3D b = Add (vy, Scale (vz, -1.0));
	const API_Vector3D c = Cross (a, b);
	API_Vector3D ex {}, ey {}, ez {};
	for (double sign : { -1.0, 1.0 }) {
		ez = Scale (Add (Scale (c, sign), Scale (Add (a, b), -1.0)), 1.0 / 3.0);
		ex = Add (ez, a);
		ey = Add (ez, b);
		const double residual = std::fabs (Len (ex) - 1.0) + std::fabs (Len (ey) - 1.0) + std::fabs (Len (ez) - 1.0) +
								std::fabs (Dot (ex, ey)) + std::fabs (Dot (ex, ez)) + std::fabs (Dot (ey, ez));
		if (residual < 1e-6)
			break;
	}
	Frame f;
	f.origin = geo.GetAnchorPoint ();
	f.z = Normalized (ez, "extrusionDirection");
	f.x = Normalized (Add (ex, Scale (f.z, -Dot (ex, f.z))), "xAxis");
	f.y = FrameY (f.x, f.z);
	return f;
}


// Archicad 29 checks the turned frame of the opening with a near-zero tolerance; its axes are stored offset by the host's
// origin, so for many angles the rounding alone makes Opening::Transform refuse an exact rotation ("Can transform only with
// ortogonal matrix with no scaling"). A refused call changes nothing: retry with the target turned by up to ~1e-9 rad.
void TransformWithRetry (AE::ModifiableOpeningGeometry& geo, const Frame& current, const Frame& target)
{
	for (Int32 attempt = 0; ; ++attempt) {
		Frame turned = target;
		if (attempt > 0) {
			const double nudge = (attempt % 2 == 0 ? 1.0 : -1.0) * ((attempt + 1) / 2) * 3e-11;
			turned.x = Normalized (Add (Scale (target.x, std::cos (nudge)), Scale (Cross (target.z, target.x), std::sin (nudge))), "xAxis");
			turned.y = FrameY (turned.x, turned.z);
		}
		const ACAPI::Result<void> r = geo.Transform (FrameTransform (current, turned));
		if (r.IsOk ())
			return;
		if (r.UnwrapErr ().kind != APIERR_BADPARS || attempt >= 32)
			Fail ("Cannot move or rotate the opening: " + ApiErrorText (r.UnwrapErr ()) + ".", r.UnwrapErr ().kind);
	}
}


bool HasOrientation (const OS& spec)
{
	return spec.Contains ("extrusionDirection") || spec.Contains ("xAxis") || spec.Contains ("rotation");
}

// --- Modify an existing opening ----------------------------------------------------------------

// Runs fn inside Opening::Modify; a cc::Error thrown in fn is re-thrown after Archicad's undo scope has closed
// (exceptions must never cross the Archicad API).
void ModifyOpeningScope (AE::Opening& opening, const std::function<void (AE::Opening::Modifier&)>& fn)
{
	std::exception_ptr failure;
	const ACAPI::Result<void> r = opening.Modify ([&] (AE::Opening::Modifier& modifier) {
		try {
			fn (modifier);
		} catch (...) {
			failure = std::current_exception ();
		}
	}, "Modify opening (Claude)");
	if (failure)
		std::rethrow_exception (failure);
	MustOk (r, "Cannot modify the opening");
}


void ApplyToOpening (const API_Guid& guid, const OS& spec, const HostInfo* placedHost)
{
	AE::Opening opening = MustGet (AE::Opening::Get (IdOf (guid)), "Cannot read the opening");

	std::optional<API_Guid> newOwner = OptGuidAny (spec, { "owner", "host" });
	const ACAPI::Result<ACAPI::UniqueID> parent = opening.GetParentElement ();
	const API_Guid currentOwner = parent.IsOk () ? GSGuid2APIGuid (parent->GetGuid ()) : APINULLGuid;
	if (newOwner.has_value () && *newOwner == currentOwner)
		newOwner.reset ();

	HostInfo host;
	if (placedHost != nullptr)
		host = *placedHost;
	else if (newOwner.has_value ())
		host = LoadHost (*newOwner);
	else if (currentOwner != APINULLGuid)
		host = LoadHost (currentOwner);

	ModifyOpeningScope (opening, [&] (AE::Opening::Modifier& m) {
		if (newOwner.has_value ())
			MustOk (m.ChangeParentElement (IdOf (*newOwner)), "Cannot move the opening to the new host");
		for (const char* key : { "cutElements", "addCutElements" }) {
			for (const API_Guid& g : GetGuidArray (spec, key, false))
				MustOk (m.AddCutElement (IdOf (g)), "Cannot make the opening cut " + GuidStr (g));
		}
		for (const API_Guid& g : GetGuidArray (spec, "removeCutElements", false))
			MustOk (m.RemoveCutElement (IdOf (g)), "Cannot stop the opening cutting " + GuidStr (g));

		AE::ModifiableOpeningExtrusionParameters par = m.GetExtrusionParameters ();
		AE::ModifiableOpeningFloorPlanParameters fp = m.GetFloorPlanParameters ();
		if (placedHost == nullptr)			// on create the opening default carried these already
			ApplyParameters (par, fp, spec);

		const std::optional<double> altitude = RequestedAltitude (spec, par.GetAnchor (), par.GetHeight ().value_or (0.0));
		if (altitude.has_value () && placedHost == nullptr) {
			if (!UsesAnchorAltitude (par.GetConstraint ()))
				Fail ("'anchorAltitude' / 'bottomElevation' apply to Horizontal and Aligned openings; move other openings with 'point' {x, y, z}.");
			MustOk (par.SetAnchorAltitude (*altitude), "Cannot set the anchor altitude");
		}

		std::optional<double> tangent;
		const std::optional<API_Coord> planPoint = placedHost == nullptr ? RequestedPlanPoint (spec, host, tangent) : std::nullopt;
		if (!planPoint.has_value () && !HasOrientation (spec))
			return;

		AE::ModifiableOpeningGeometry geo = m.GetOpeningGeometry ();
		const Frame current = CurrentFrame (geo);
		Frame target = HasOrientation (spec) ? RequestedFrame (spec, current) : current;
		if (planPoint.has_value ()) {
			target.origin.x = planPoint->x;
			target.origin.y = planPoint->y;
		}
		if (!HasOrientation (spec)) {
			// A pure move: an exact translation matrix.
			API_Tranmat shift = {};
			shift.tmx[0] = shift.tmx[5] = shift.tmx[10] = 1.0;
			shift.tmx[3] = target.origin.x - current.origin.x;
			shift.tmx[7] = target.origin.y - current.origin.y;
			shift.tmx[11] = target.origin.z - current.origin.z;
			MustOk (geo.Transform (shift), "Cannot move the opening");
			return;
		}
		TransformWithRetry (geo, current, target);
	});
}

// --- Adapter -----------------------------------------------------------------------------------

API_Guid CreateOpening (const OS& spec)
{
	const std::optional<API_Guid> ownerGuid = OptGuidAny (spec, { "owner", "host" });
	if (!ownerGuid.has_value ())
		Fail ("An opening needs 'owner': the GUID of the element it cuts (wall, slab, roof, shell, beam, column, mesh ...).");
	if (!spec.Contains ("position") && !spec.Contains ("point"))
		Fail ("An opening needs 'point' ({x, y, z?}: plan position of its anchor) or, in a wall, 'position' (m along the wall reference line).");
	const bool circularRequested = spec.Contains ("shape") && EqualsIgnoreCase (GetString (spec, "shape"), "Circular");
	if (!spec.Contains ("width") || (!spec.Contains ("height") && !circularRequested))
		Fail ("An opening needs 'width' and 'height' in meters (a circular opening needs only 'width' = diameter).");
	if (spec.Contains ("constraint") && EqualsIgnoreCase (GetString (spec, "constraint"), "Free"))
		Fail ("Archicad 29 cannot create an opening with the 'Free' constraint: create it with another constraint, then modify_elements {constraint: 'Free'}.", APIERR_NOTSUPPORTED);
	const HostInfo host = LoadHost (*ownerGuid);

	// 1) Opening default from the spec (shape, size, constraint, anchor, limits, floor plan display).
	AE::OpeningDefault def = MustGet (AE::CreateOpeningDefault (), "Cannot get the Opening tool defaults");
	std::exception_ptr failure;
	API_AnchorID anchor = APIAnc_MM;
	double height = 0.0;
	AE::Constraint constraint = AE::Constraint::ForcedHorizontal;
	const ACAPI::Result<void> setUp = def.Modify ([&] (AE::OpeningDefault::Modifier& m) {
		try {
			AE::ModifiableOpeningExtrusionParameters par = m.GetExtrusionParameters ();
			AE::ModifiableOpeningFloorPlanParameters fp = m.GetFloorPlanParameters ();
			if (par.GetShapeType () == AE::ShapeType::Polygonal)
				par.SetShapeType (AE::ShapeType::Rectangular);
			if (!spec.Contains ("anchor"))
				par.SetAnchor (APIAnc_MM);
			if (!spec.Contains ("constraint")) {
				if (auto c = DefaultConstraint (host.typeID))
					MustOk (par.SetConstraint (*c), "Cannot set the constraint");
			}
			ApplyParameters (par, fp, spec);
			anchor = par.GetAnchor ();
			height = par.GetHeight ().value_or (0.0);
			constraint = par.GetConstraint ();
			if (auto altitude = RequestedAltitude (spec, anchor, height); altitude.has_value () && UsesAnchorAltitude (constraint))
				MustOk (par.SetAnchorAltitude (*altitude), "Cannot set the anchor altitude");
		} catch (...) {
			failure = std::current_exception ();
		}
	});
	if (failure)
		std::rethrow_exception (failure);
	MustOk (setUp, "Cannot set up the opening");

	// 2) Anchor point: plan position on the host + elevation.
	std::optional<double> tangent;
	const API_Coord plan = *RequestedPlanPoint (spec, host, tangent);
	const std::optional<double> altitude = RequestedAltitude (spec, anchor, height);
	API_Coord3D point;
	point.x = plan.x;
	point.y = plan.y;
	point.z = StoryLevel (host.floorInd) + altitude.value_or (0.0);

	// 3) Place it into the host.
	ACAPI::Result<ACAPI::UniqueID> placed = def.Place (IdOf (*ownerGuid), point);
	if (placed.IsErr ()) {
		const GSErrCode err = placed.UnwrapErr ().kind;
		GS::UniString hint;
		if (err == APIERR_BADPARS || err == APIERR_REFUSEDPAR || err == APIERR_GENERAL)
			hint = " Check that 'owner' is an element the Opening tool can cut (wall, slab, roof, shell, beam, column, mesh) and that the anchor point lies on it.";
		else if (err == APIERR_NOTEDITABLE || err == APIERR_LOCKEDLAY || err == APIERR_HIDDENLAY)
			hint = " The host or the target layer is not editable (locked / hidden layer, or not reserved in Teamwork).";
		Fail ("Cannot create opening: " + ApiErrorText (placed.UnwrapErr ()) + "." + hint, err);
	}
	const API_Guid guid = GSGuid2APIGuid (placed->GetGuid ());

	// 4) Explicit orientation, and the common fields (layer, renovation status ...) through the element header.
	if (HasOrientation (spec) || spec.Contains ("cutElements"))
		ApplyToOpening (guid, spec, &host);
	API_Element element = GetElement (guid);
	API_Element mask;
	ACAPI_ELEMENT_MASK_CLEAR (mask);
	ApplyCommonFields (element, &mask, spec);
	const char* maskBytes = reinterpret_cast<const char*> (&mask);
	bool anyMasked = false;
	for (size_t i = 0; i < sizeof (mask) && !anyMasked; ++i)
		anyMasked = maskBytes[i] != 0;
	if (anyMasked)
		Check (ACAPI_Element_Change (&element, &mask, nullptr, 0, true), "Cannot set the layer / renovation status of the new opening");
	return guid;
}


void SerializeOpening (const API_Element& element, OS& out)
{
	AE::Opening opening = MustGet (AE::Opening::Get (IdOf (element.header.guid)), "Cannot read the opening");

	const ACAPI::Result<ACAPI::UniqueID> parent = opening.GetParentElement ();
	const API_Guid owner = parent.IsOk () ? GSGuid2APIGuid (parent->GetGuid ()) : APINULLGuid;
	out.Add ("owner", GuidStr (owner));
	API_Elem_Head head;
	BNZeroMemory (&head, sizeof (head));
	head.guid = owner;
	const bool hostOk = owner != APINULLGuid && ACAPI_Element_GetHeader (&head) == NoError;
	if (hostOk)
		out.Add ("ownerType", ElemTypeName (head.type));

	const AE::OpeningExtrusionParameters par = opening.GetExtrusionParameters ();
	out.Add ("shape", NameOf (kShapes, (Int32) par.GetShapeType ()));
	const double height = par.GetHeight ().value_or (0.0);
	if (par.GetShapeType () != AE::ShapeType::Polygonal) {
		if (auto w = par.GetWidth ())
			out.Add ("width", *w);
		if (auto h = par.GetHeight ())
			out.Add ("height", *h);
		if (auto linked = par.GetLinkedStatus ())
			out.Add ("linkedSize", *linked == AE::LinkedStatus::Linked);
	}
	out.Add ("anchor", NameOf (kAnchors, par.GetAnchor ()));
	out.Add ("constraint", NameOf (kConstraints, (Int32) par.GetConstraint ()));
	if (auto altitude = par.GetAnchorAltitude (); altitude.has_value () && UsesAnchorAltitude (par.GetConstraint ())) {
		out.Add ("anchorAltitude", *altitude);
		out.Add ("bottomElevation", *altitude - AnchorHeightOffset (par.GetAnchor (), height));
	}
	out.Add ("limit", NameOf (kLimits, (Int32) par.GetLimitType ()));
	if (auto v = par.GetExtrusionStartOffset (); v.has_value () && par.GetLimitType () != AE::LimitType::Infinite)
		out.Add ("startOffset", *v);
	if (auto v = par.GetFiniteBodyLength (); v.has_value () && par.GetLimitType () == AE::LimitType::Finite)
		out.Add ("depth", *v);

	const AE::OpeningGeometry geo = opening.GetOpeningGeometry ();
	const Frame frame = CurrentFrame (geo);
	out.Add ("point", CoordObj (frame.origin.x, frame.origin.y));
	out.Add ("frame", OS ("origin", Coord3DObj (frame.origin),
						  "xAxis", Coord3DObj (frame.x),
						  "yAxis", Coord3DObj (frame.y),
						  "extrusionDirection", Coord3DObj (frame.z)));
	if (hostOk && head.type.typeID == API_WallID) {
		try {
			API_Coord p;
			p.x = frame.origin.x;
			p.y = frame.origin.y;
			const WallGeometry wall = LoadHostWall (owner, "opening");
			out.Add ("position", ProjectOntoWall (wall, p, "anchor").distance);
		} catch (const Error&) {
			// anchor outside the wall range or polygonal wall: no position
		}
	}

	const AE::OpeningFloorPlanParameters fp = opening.GetFloorPlanParameters ();
	out.Add ("floorPlanDisplay", NameOf (kFloorPlanModes, (Int32) fp.GetFloorPlanDisplayMode ()));
	out.Add ("connectionMode", NameOf (kConnectionModes, (Int32) fp.GetDividedOpeningMode ()));
	out.Add ("outlinesStyle", NameOf (kOutlineStyles, (Int32) fp.GetOpenEdgeStyle ()));
	out.Add ("showReferenceAxis", fp.IsShowReferenceAxis ());

	GS::Array<GS::UniString> cut;
	for (const ACAPI::UniqueID& id : opening.GetCutElements ()) {
		const API_Guid g = GSGuid2APIGuid (id.GetGuid ());
		if (g != owner)
			cut.Push (GuidStr (g));
	}
	out.Add ("cutElements", cut);
}


void ModifyOpening (API_Element& element, API_Element&, API_ElementMemo&, UInt64&, const OS& patch)
{
	static const char* const kOpeningFields[] = {
		"owner", "host", "shape", "width", "height", "linkedSize", "anchor", "constraint", "limit", "startOffset", "depth",
		"floorPlanDisplay", "connectionMode", "outlinesStyle", "showReferenceAxis", "position", "point", "anchorAltitude",
		"bottomElevation", "extrusionDirection", "xAxis", "rotation", "addCutElements", "removeCutElements",
	};
	bool any = false;
	for (const char* key : kOpeningFields)
		any = any || patch.Contains (key);
	if (any)
		ApplyToOpening (element.header.guid, patch, nullptr);
}

} // namespace


void RegisterOpeningToolAdapter ()
{
	RegisterAdapter ({ API_OpeningID, CreateOpening, SerializeOpening, ModifyOpening });
}

} // namespace openings
} // namespace cc

#endif // CC_AC_VERSION >= 29
