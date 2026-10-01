// *****************************************************************************
// Views3D — 3D projection / style / filter / cutting planes, image capture of
// any window and photo rendering.
//
// Commands:
//   Get3DView     {}
//   Set3DView     {mode?, camera?, target?, azimuth?, altitude?, distance?, viewCone?, roll?, twoPointPerspective?,
//                  projection?, tranmat?, sun?, style?, styleSettings?, windowSize?, stories?, elementTypes?, cutPlanes?, open3D?}
//   CaptureView   {path, format?: png|jpeg, width?, height?, keepSelectionHighlight?, cropToWindow?}
//   RenderView    {path, format?: png|jpeg, width?, height?, scene?}
//
// Angles in degrees. Azimuths are measured counter-clockwise from the +X axis (east);
// "azimuth" of set_3d_view orbit input = direction FROM the target TO the camera.
// *****************************************************************************

#include "Commands/ViewsCommon.hpp"
#include "Core/Command.hpp"
#include "Core/Elements.hpp"
#include "Core/Enums.hpp"
#include "Core/Types.hpp"

#include "FileSystem.hpp"
#include "Location.hpp"

#include <cmath>
#include <map>

namespace cc {
namespace views {

namespace {

constexpr double kEps = 1e-6;

#if defined (macintosh)
constexpr API_ColorDepthID kTrueColor = APIColorDepth_MiC;		// millions of colors (24 bit)
#else
constexpr API_ColorDepthID kTrueColor = APIColorDepth_TC24;
#endif

// --- Enums -----------------------------------------------------------------------------

const NamedValue kProjModes[] = {
	{ "SideView",			API_Projection_YZ },
	{ "FrontView",			API_Projection_XZ },
	{ "TopView",			API_Projection_XY },
	{ "Frontal",			API_Projection_Frontal },
	{ "Monometric",			API_Projection_Monometric },
	{ "Isometric",			API_Projection_Isometric },
	{ "Dimetric",			API_Projection_Dimetric },
	{ "BottomView",			API_Projection_Bottom },
	{ "FrontalBottom",		API_Projection_FrontalB },
	{ "MonometricBottom",	API_Projection_MonometricB },
	{ "IsometricBottom",	API_Projection_IzometricB },
	{ "DimetricBottom",		API_Projection_DimetricB },
	{ "Parallel",			API_Projection_Parallel },
	{ "CustomAxonometry",	API_Projection_FreeAx },
};

const NamedValue kModel3D[] = {
	{ "Block",				API3DModel_Block },
	{ "Wireframe",			API3DModel_WireFrame },
	{ "HiddenLine",			API3DModel_Hiddenline },
	{ "Shading",			API3DModel_Shading },
};

const NamedValue kShadContours[] = {
	{ "Draft",				APIShadContours_Draft },
	{ "Off",				APIShadContours_Off },
	{ "Best",				APIShadContours_Best },
};

const NamedValue kVectShadow[] = {
	{ "Off",							APIVectShad_Off },
	{ "AllSurfacesContoursOff",			APIVectShad_ContOff_AllSurf },
	{ "AllSurfacesContoursOn",			APIVectShad_ContOn_AllSurf },
	{ "OneLevelContoursOff",			APIVectShad_ContOff_OneLevel },
	{ "OneLevelContoursOn",				APIVectShad_ContOn_OneLevel },
};

const NamedValue kFilterModes[] = {
	{ "all",				API_FilterByRules },
	{ "selection",			API_FilterBySelection },
	{ "marquee",			API_FilterByMarqueeAndRules },
};

// --- Geometry helpers ---------------------------------------------------------------------------

struct Vec3 {
	double x = 0, y = 0, z = 0;
};

Vec3 V3 (double x, double y, double z) { Vec3 v; v.x = x; v.y = y; v.z = z; return v; }
Vec3 Cross (const Vec3& a, const Vec3& b) { return V3 (a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
double Len (const Vec3& a) { return std::sqrt (a.x * a.x + a.y * a.y + a.z * a.z); }

OS RgbJson (const API_RGBColor& c)
{
	return OS ("r", c.f_red, "g", c.f_green, "b", c.f_blue);
}

// Orthographic view matrix (rows: screen right, screen up, towards the viewer) for a camera looking
// from direction (azimuth, altitude) at the model.
API_Tranmat ViewTranmat (double azimuth, double altitude)
{
	const Vec3 back = V3 (std::cos (altitude) * std::cos (azimuth), std::cos (altitude) * std::sin (azimuth), std::sin (altitude));
	Vec3 right = V3 (-std::sin (azimuth), std::cos (azimuth), 0.0);
	const Vec3 up = Cross (back, right);
	API_Tranmat t = {};
	t.tmx[0] = right.x;	t.tmx[1] = right.y;	t.tmx[2] = right.z;		t.tmx[3] = 0.0;
	t.tmx[4] = up.x;	t.tmx[5] = up.y;	t.tmx[6] = up.z;		t.tmx[7] = 0.0;
	t.tmx[8] = back.x;	t.tmx[9] = back.y;	t.tmx[10] = back.z;		t.tmx[11] = 0.0;
	return t;
}


bool InvertTranmat (const API_Tranmat& m, API_Tranmat& inv)
{
	const double* a = m.tmx;
	const double det = a[0] * (a[5] * a[10] - a[6] * a[9]) - a[1] * (a[4] * a[10] - a[6] * a[8]) + a[2] * (a[4] * a[9] - a[5] * a[8]);
	if (std::fabs (det) < 1e-12)
		return false;
	const double id = 1.0 / det;
	double r[9];
	r[0] = (a[5] * a[10] - a[6] * a[9]) * id;
	r[1] = (a[2] * a[9] - a[1] * a[10]) * id;
	r[2] = (a[1] * a[6] - a[2] * a[5]) * id;
	r[3] = (a[6] * a[8] - a[4] * a[10]) * id;
	r[4] = (a[0] * a[10] - a[2] * a[8]) * id;
	r[5] = (a[2] * a[4] - a[0] * a[6]) * id;
	r[6] = (a[4] * a[9] - a[5] * a[8]) * id;
	r[7] = (a[1] * a[8] - a[0] * a[9]) * id;
	r[8] = (a[0] * a[5] - a[1] * a[4]) * id;
	const double tx = a[3], ty = a[7], tz = a[11];
	inv.tmx[0] = r[0]; inv.tmx[1] = r[1]; inv.tmx[2] = r[2];  inv.tmx[3] = -(r[0] * tx + r[1] * ty + r[2] * tz);
	inv.tmx[4] = r[3]; inv.tmx[5] = r[4]; inv.tmx[6] = r[5];  inv.tmx[7] = -(r[3] * tx + r[4] * ty + r[5] * tz);
	inv.tmx[8] = r[6]; inv.tmx[9] = r[7]; inv.tmx[10] = r[8]; inv.tmx[11] = -(r[6] * tx + r[7] * ty + r[8] * tz);
	return true;
}


// Story level range of the project (min level, max level + height of the top story when known).
void StoryLevelRange (double& zMin, double& zMax)
{
	zMin = 0.0;
	zMax = 3.0;
	API_StoryInfo info;
	BNZeroMemory (&info, sizeof (info));
	if (ACAPI_Environment (APIEnv_GetStorySettingsID, &info, nullptr) != NoError)
		return;
	if (info.data != nullptr) {
		const Int32 n = info.lastStory - info.firstStory + 1;
		const Int32 available = (Int32) (BMGetHandleSize ((GSConstHandle) info.data) / sizeof (API_StoryType));
		if (n > 0 && available > 0) {
			zMin = (*info.data)[0].level;
			zMax = (*info.data)[GS::Min (n, available) - 1].level + 3.0;
		}
	}
	BMKillHandle (reinterpret_cast<GSHandle*> (&info.data));
}


// Bounding box of the model: 3D model elements on visible layers, else Archicad's 3D extent, else floor plan extent + story levels.
API_Box3D ModelExtent ()
{
	API_Box3D box = {};
	// Union of the bounds of the 3D model elements on visible layers. Flat boxes are ignored: the GDL heads of
	// elevation/section markers are Object elements with zero height, and APIDb_GetExtent3DID includes them too,
	// which made "fit model" frame the markers around the origin instead of the building.
	{
		static const API_ElemTypeID kModelTypes[] = {
			API_WallID, API_ColumnID, API_BeamID, API_SlabID, API_RoofID, API_ShellID, API_MeshID, API_MorphID,
			API_ObjectID, API_LampID, API_CurtainWallID, API_StairID, API_RailingID, API_ZoneID, API_SkylightID
		};
		bool any = false;
		Int32 scanned = 0;
		API_Box3D u = {};
		for (API_ElemTypeID t : kModelTypes) {
			GS::Array<API_Guid> list;
			if (ACAPI_Element_GetElemList (API_ElemType (t), &list, APIFilt_OnVisLayer) != NoError)
				continue;
			for (const API_Guid& g : list) {
				if (++scanned > 20000)
					break;
				API_Elem_Head head {};
				head.guid = g;
				if (ACAPI_Element_GetHeader (&head) != NoError)
					continue;
				API_Box3D b {};
				if (ACAPI_Database (APIDb_CalcBoundsID, &head, &b) != NoError || b.xMax < b.xMin || b.zMax <= b.zMin + kEps)
					continue;
				if (!any) {
					u = b;
					any = true;
				} else {
					u.xMin = GS::Min (u.xMin, b.xMin); u.yMin = GS::Min (u.yMin, b.yMin); u.zMin = GS::Min (u.zMin, b.zMin);
					u.xMax = GS::Max (u.xMax, b.xMax); u.yMax = GS::Max (u.yMax, b.yMax); u.zMax = GS::Max (u.zMax, b.zMax);
				}
			}
		}
		if (any && u.xMax > u.xMin + kEps && u.yMax > u.yMin + kEps)
			return u;
	}
	if (ACAPI_Database (APIDb_GetExtent3DID, &box, nullptr) == NoError && box.xMax > box.xMin + kEps && box.yMax > box.yMin + kEps)
		return box;
	API_Box b2 = {};
	double zMin = 0.0, zMax = 3.0;
	StoryLevelRange (zMin, zMax);
	if (ACAPI_Database (APIDb_GetExtentID, &b2, nullptr) == NoError && b2.xMax > b2.xMin + kEps) {
		box.xMin = b2.xMin; box.yMin = b2.yMin; box.xMax = b2.xMax; box.yMax = b2.yMax;
	} else {
		box.xMin = -10.0; box.yMin = -10.0; box.xMax = 10.0; box.yMax = 10.0;
	}
	box.zMin = zMin;
	box.zMax = zMax;
	return box;
}

// --- 3D projection JSON -------------------------------------------------------------------------
// NOTE: unlike element data, the 3D projection settings (APIEnv_Get/Change3DProjectionSetsID) store their
// angles in DEGREES: perspective azimuth / viewCone / rollAngle, axonometric azimuth and the sun angles
// (verified live on AC26: a default perspective reads viewCone 75, azimuth 270; DevKit ModelAccess_Test
// also treats persp.rollAngle as degrees). Internal math below uses radians; convert only at this boundary.

double NormDeg (double deg)
{
	double d = std::fmod (deg, 360.0);
	if (d < 0.0)
		d += 360.0;
	return d;
}


OS SunJson (const API_SunAngleSettings& sun)
{
	OS out;
	out.Add ("azimuth", sun.sunAzimuth);
	out.Add ("altitude", sun.sunAltitude);
	out.Add ("givenBy", GS::UniString (sun.sunPosOpt == API_SunPosition_GivenByDate ? "date" : "angles"));
	if (sun.sunPosOpt == API_SunPosition_GivenByDate) {
		GS::UniString date;
		date.Printf ("%04u-%02u-%02uT%02u:%02u:%02u", (unsigned) sun.year, (unsigned) sun.month, (unsigned) sun.day,
					 (unsigned) sun.hour, (unsigned) sun.minute, (unsigned) sun.second);
		out.Add ("date", date);
		out.Add ("summerTime", sun.summerTime);
	}
	return out;
}


OS ProjectionJson (const API_3DProjectionInfo& proj)
{
	OS out;
	out.Add ("mode", GS::UniString (proj.isPersp ? "perspective" : "axonometric"));
	if (proj.camGuid != APINULLGuid)
		out.Add ("cameraGuid", GuidStr (proj.camGuid));
	if (proj.actCamSet != APINULLGuid)
		out.Add ("cameraSetGuid", GuidStr (proj.actCamSet));
	if (proj.isPersp) {
		const API_PerspPars& p = proj.u.persp;
		OS persp;
		persp.Add ("camera", Coord3DObj (p.pos.x, p.pos.y, p.cameraZ));
		persp.Add ("target", Coord3DObj (p.target.x, p.target.y, p.targetZ));
		persp.Add ("viewCone", p.viewCone);
		persp.Add ("roll", p.rollAngle);
		persp.Add ("distance", p.distance);
		persp.Add ("azimuth", p.azimuth);
		persp.Add ("twoPointPerspective", p.isTwoPointPersp);
		const double dx = p.target.x - p.pos.x, dy = p.target.y - p.pos.y, dz = p.targetZ - p.cameraZ;
		const double horiz = std::hypot (dx, dy);
		OS dir;
		dir.Add ("azimuth", RadToDeg (std::atan2 (dy, dx)));
		dir.Add ("altitude", RadToDeg (std::atan2 (dz, horiz)));
		dir.Add ("note", GS::UniString ("Direction from the camera to the target (azimuth CCW from +X; altitude > 0 = looking up)."));
		persp.Add ("viewDirection", dir);
		persp.Add ("sun", SunJson (p.sunAngSets));
		out.Add ("perspective", persp);
	} else {
		const API_AxonoPars& a = proj.u.axono;
		OS axo;
		axo.Add ("projection", NameOf (kProjModes, a.projMod));
		axo.Add ("projectionCode", (Int32) a.projMod);
		axo.Add ("azimuth", a.azimuth);
		GS::Array<double> tm;
		for (int i = 0; i < 12; ++i)
			tm.Push (a.tranmat.tmx[i]);
		axo.Add ("tranmat", tm);
		const double bx = a.tranmat.tmx[8], by = a.tranmat.tmx[9], bz = a.tranmat.tmx[10];
		const double bl = std::sqrt (bx * bx + by * by + bz * bz);
		if (bl > kEps) {
			OS derived;
			derived.Add ("viewFromAzimuth", RadToDeg (std::atan2 (by, bx)));
			derived.Add ("viewFromAltitude", RadToDeg (std::asin (GS::Max (-1.0, GS::Min (1.0, bz / bl)))));
			derived.Add ("note", GS::UniString ("Derived from the 3rd tranmat row (assumed = direction towards the viewer)."));
			axo.Add ("derived", derived);
		}
		axo.Add ("sun", SunJson (a.sunAngSets));
		out.Add ("axonometric", axo);
	}
	return out;
}


OS StyleJson ()
{
	OS out;
	GS::Array<GS::UniString> names;
	GS::UniString current;
	if (ACAPI_Environment (APIEnv_Get3DStyleListID, &names, &current) != NoError)
		return out;
	out.Add ("current", current);
	out.Add ("available", names);
	if (!current.IsEmpty ()) {
		API_3DStyle st;
		SetUStr (st.name, current);
		if (ACAPI_Environment (APIEnv_Get3DStyleID, &st, nullptr) == NoError) {
			out.Add ("engineId", (Int32) st.engineId);
			out.Add ("model", NameOf (kModel3D, st.model3D));
			out.Add ("transparency", st.transparency);
			out.Add ("monochrome", st.monochromeEnabled);
			out.Add ("contours", NameOf (kShadContours, st.shadCont));
			out.Add ("sunShadows", NameOf (kVectShadow, st.vectSunShadow));
			out.Add ("vectorHatching", st.vectHatchOn);
			out.Add ("castShadowPercent", (Int32) st.castShadowPercent);
			out.Add ("shadingPercent", (Int32) st.shadingPercent);
			out.Add ("backgroundAsInRendering", st.bkgAsInRendering);
			out.Add ("skyColor", RgbJson (st.bkgSkyColor));
			out.Add ("groundColor", RgbJson (st.backGroundRGB));
		}
	}
	return out;
}


OS FilterJson ()
{
	OS out;
	API_3DFilterAndCutSettings f {};
	if (ACAPI_Environment (APIEnv_Get3DImageSetsID, &f, nullptr) != NoError)
		return out;
	out.Add ("allStories", f.allStories);
	out.Add ("fromStory", (Int32) f.firstStory3D);
	out.Add ("toStory", (Int32) f.lastStory3D);
	out.Add ("trimToStoryRange", f.trimToStoryRange);
	out.Add ("mode", NameOf (kFilterModes, f.filterMode));
	OS types;
	for (const auto& kv : f.elemTypeFilter)
		types.Add (ToStr (kv.first == API_ZombieElemID ? GS::UniString ("all") : ElemTypeName (kv.first)), kv.second);
	out.Add ("elementTypes", types);
	return out;
}


OS CutPlanesJson ()
{
	OS out;
	API_3DCutPlanesInfo c;
	BNZeroMemory (&c, sizeof (c));
	if (ACAPI_Environment (APIEnv_Get3DCuttingPlanesID, &c, nullptr) != NoError)
		return out;
	out.Add ("enabled", c.isCutPlanes);
	GS::Array<OS> shapes;
	if (c.shapes != nullptr) {
		const Int32 available = (Int32) (BMGetHandleSize (reinterpret_cast<GSConstHandle> (c.shapes)) / sizeof (API_3DCutShapeType));
		for (Int32 i = 0; i < c.nShapes && i < available; ++i) {
			const API_3DCutShapeType& s = (*c.shapes)[i];
			OS shape ("status", (Int32) s.cutStatus, "pen", (Int32) s.cutPen, "material", (Int32) s.cutMater);
			shape.Add ("a", s.pa);
			shape.Add ("b", s.pb);
			shape.Add ("c", s.pc);
			shape.Add ("d", s.pd);
			shapes.Push (shape);
		}
	}
	out.Add ("shapes", shapes);
	BMKillHandle (reinterpret_cast<GSHandle*> (&c.shapes));
	return out;
}


OS WindowSizeJson ()
{
	OS out;
	API_3DWindowInfo w = {};
	if (ACAPI_Environment (APIEnv_Get3DWindowSetsID, &w, nullptr) == NoError) {
		out.Add ("width", (Int32) w.hSize);
		out.Add ("height", (Int32) w.vSize);
	}
	return out;
}


OS RenderingJson ()
{
	OS out;
	GS::Array<GS::UniString> scenes;
	if (ACAPI_Environment (APIEnv_GetRenderingSceneNamesID, &scenes) == NoError)
		out.Add ("scenes", scenes);
	API_RendImage img;
	BNZeroMemory (&img, sizeof (img));
	if (ACAPI_Environment (APIEnv_GetRenderingSetsID, &img, reinterpret_cast<void*> ((GS::IntPtr) APIRendSet_ImageID), nullptr) == NoError) {
		out.Add ("imageSize", OS ("width", (Int32) img.hSize, "height", (Int32) img.vSize));
		delete img.bkgPictFile;
	}
	return out;
}

// --- Setting the projection ------------------------------------------------------------------------

API_3DProjectionInfo GetProjection ()
{
	API_3DProjectionInfo proj;
	BNZeroMemory (&proj, sizeof (proj));
	Check (ACAPI_Environment (APIEnv_Get3DProjectionSetsID, &proj, nullptr), "Cannot read the 3D projection settings");
	return proj;
}


void ChangeProjection (const API_3DProjectionInfo& proj)
{
	CallApi ("Change 3D projection", [&] () {
		API_3DProjectionInfo p = proj;
		return ACAPI_Environment (APIEnv_Change3DProjectionSetsID, &p, nullptr);
	});
}


void ApplySun (const OS& params, API_SunAngleSettings& sun)
{
	OS s;
	if (!TryGetObject (params, "sun", s))
		return;
	// Projection-settings sun angles are stored in degrees.
	if (auto az = OptDouble (s, "azimuth"))	{ sun.sunAzimuth = NormDeg (*az); sun.sunPosOpt = API_SunPosition_GivenByAngles; }
	if (auto al = OptDouble (s, "altitude")) {
		if (*al < -90.0 || *al > 90.0)
			Fail ("sun.altitude must be between -90 and 90 degrees.");
		sun.sunAltitude = *al;
		sun.sunPosOpt = API_SunPosition_GivenByAngles;
	}
}


bool SameCamera (const API_PerspPars& p, const Vec3& cam, const Vec3& tgt)
{
	const double tol = 1e-3;
	return std::fabs (p.pos.x - cam.x) < tol && std::fabs (p.pos.y - cam.y) < tol && std::fabs (p.cameraZ - cam.z) < tol &&
		   std::fabs (p.target.x - tgt.x) < tol && std::fabs (p.target.y - tgt.y) < tol && std::fabs (p.targetZ - tgt.z) < tol;
}


// Writes a perspective camera. Archicad derives some fields (azimuth, distance) from others; the conventions
// are verified by reading the settings back, trying the alternatives when the camera did not land where asked.
OS ApplyPerspective (API_3DProjectionInfo proj, const Vec3& cam, const Vec3& tgt, double viewCone, double roll,
					 std::optional<bool> twoPoint, const OS& params)
{
	const double dx = tgt.x - cam.x, dy = tgt.y - cam.y, dz = tgt.z - cam.z;
	const double d2 = std::hypot (dx, dy);
	const double d3 = std::sqrt (d2 * d2 + dz * dz);
	if (d3 < 1e-4)
		Fail ("Camera and target must be different points.");
	const double dirAz = std::atan2 (dy, dx);

	struct Candidate { double azimuth; double distance; };
	const Candidate candidates[] = {
		{ dirAz, d2 }, { dirAz + kPi, d2 }, { dirAz, d3 }, { dirAz + kPi, d3 }
	};

	proj.isPersp = true;
	proj.camGuid = APINULLGuid;
	proj.actCamSet = APINULLGuid;
	API_PerspPars& p = proj.u.persp;
	p.pos.x = cam.x; p.pos.y = cam.y; p.cameraZ = cam.z;
	p.target.x = tgt.x; p.target.y = tgt.y; p.targetZ = tgt.z;
	p.viewCone = RadToDeg (viewCone);		// projection settings store degrees
	p.rollAngle = RadToDeg (roll);
	if (twoPoint.has_value ())
		p.isTwoPointPersp = *twoPoint;
	ApplySun (params, p.sunAngSets);

	bool verified = false;
	Int32 convention = -1;
	for (Int32 i = 0; i < 4 && !verified; ++i) {
		p.azimuth = NormDeg (RadToDeg (candidates[i].azimuth));
		p.distance = GS::Max (candidates[i].distance, 1e-3);
		ChangeProjection (proj);
		const API_3DProjectionInfo check = GetProjection ();
		if (check.isPersp && SameCamera (check.u.persp, cam, tgt)) {
			verified = true;
			convention = i;
		}
	}
	OS out;
	out.Add ("verified", verified);
	if (!verified)
		out.Add ("warning", GS::UniString ("Archicad did not place the camera exactly at the requested position; check get_3d_view / capture_view."));
	else
		out.Add ("convention", convention);
	return out;
}


void ApplyStories (const OS& params)
{
	OS s;
	if (!TryGetObject (params, "stories", s))
		return;
	API_3DFilterAndCutSettings f {};
	Check (ACAPI_Environment (APIEnv_Get3DImageSetsID, &f, nullptr), "Cannot read the 3D filter settings");
	if (auto all = OptBool (s, "all"))
		f.allStories = *all;
	if (auto from = OptStory (s, "from")) { f.firstStory3D = *from; f.allStories = false; }
	if (auto to = OptStory (s, "to"))		{ f.lastStory3D = *to; f.allStories = false; }
	if (f.firstStory3D > f.lastStory3D)
		std::swap (f.firstStory3D, f.lastStory3D);
	if (auto trim = OptBool (s, "trim"))
		f.trimToStoryRange = *trim;
	CallApi ("Change 3D story range", [&] () {
		API_3DFilterAndCutSettings copy = f;
		return ACAPI_Environment (APIEnv_Change3DImageSetsID, &copy, nullptr);
	});
}


void ApplyElementTypes (const OS& params)
{
	if (!params.Contains ("elementTypes"))
		return;
	const GS::Array<GS::UniString> names = GetStringArray (params, "elementTypes", true);
	if (names.IsEmpty ())
		Fail ("'elementTypes' must list element types (e.g. [\"Wall\", \"Slab\"]) or be [\"all\"].");
	API_3DFilterAndCutSettings f {};
	Check (ACAPI_Environment (APIEnv_Get3DImageSetsID, &f, nullptr), "Cannot read the 3D filter settings");
	bool all = false;
	GS::Array<API_ElemTypeID> types;
	for (const GS::UniString& n : names) {
		if (EqualsIgnoreCase (n, "all")) { all = true; continue; }
		auto t = ParseElemType (n);
		if (!t.has_value ())
			Fail ("Unknown element type '" + n + "' in elementTypes. Use names like Wall, Slab, Roof, Column, Beam, Window, Door, Object, Zone, Stair, Railing, CurtainWall, Morph, Mesh, Shell.");
		types.Push (*t);
	}
	auto change = [&] (const char* what) {
		CallApi (what, [&] () {
			API_3DFilterAndCutSettings copy = f;
			return ACAPI_Environment (APIEnv_Change3DImageSetsID, &copy, nullptr);
		});
	};
	f.elemTypeFilter.clear ();
	if (all) {
		f.elemTypeFilter.insert ({ API_ZombieElemID, true });
		change ("Show all element types in 3D");
		return;
	}
	f.elemTypeFilter.insert ({ API_ZombieElemID, false });
	change ("Hide all element types in 3D");
	f.elemTypeFilter.clear ();
	for (API_ElemTypeID t : types)
		f.elemTypeFilter.insert ({ t, true });
	change ("Show element types in 3D");
}


void ApplyCutPlanes (const OS& params)
{
	OS cp;
	if (!TryGetObject (params, "cutPlanes", cp))
		return;
	API_3DCutPlanesInfo c;
	BNZeroMemory (&c, sizeof (c));
	Check (ACAPI_Environment (APIEnv_Get3DCuttingPlanesID, &c, nullptr), "Cannot read the 3D cutting planes");
	struct HandleGuard {
		API_3DCutShapeType*** h;
		~HandleGuard () { BMKillHandle (reinterpret_cast<GSHandle*> (h)); }
	} guard { &c.shapes };

	if (auto enabled = OptBool (cp, "enabled"))
		c.isCutPlanes = *enabled;
	if (cp.Contains ("shapes")) {
		const GS::Array<OS> shapes = GetObjectArray (cp, "shapes", true);
		if (shapes.GetSize () > 64)
			Fail ("At most 64 cutting plane shapes are supported.");
		BMKillHandle (reinterpret_cast<GSHandle*> (&c.shapes));
		c.nShapes = (short) shapes.GetSize ();
		if (c.nShapes > 0) {
			c.shapes = reinterpret_cast<API_3DCutShapeType**> (BMAllocateHandle (c.nShapes * sizeof (API_3DCutShapeType), ALLOCATE_CLEAR, 0));
			if (c.shapes == nullptr)
				Fail ("Out of memory.", APIERR_MEMFULL);
			for (Int32 i = 0; i < c.nShapes; ++i) {
				const OS& s = shapes[i];
				API_3DCutShapeType& t = (*c.shapes)[i];
				t.cutStatus = (short) GetInt (s, "status", 2);
				t.cutPen = (short) GetInt (s, "pen", 1);
				if (auto mat = OptAttr (API_MaterialID, s, "material"))
					t.cutMater = *mat;
				t.pa = GetDouble (s, "a");
				t.pb = GetDouble (s, "b");
				t.pc = GetDouble (s, "c");
				t.pd = GetDouble (s, "d");
			}
		}
	}
	CallApi ("Change 3D cutting planes", [&] () {
		API_3DCutPlanesInfo copy = c;
		return ACAPI_Environment (APIEnv_Change3DCuttingPlanesID, &copy, nullptr);
	});
}


void ApplyWindowSize (const OS& params)
{
	OS ws;
	if (!TryGetObject (params, "windowSize", ws))
		return;
	API_3DWindowInfo w = {};
	Check (ACAPI_Environment (APIEnv_Get3DWindowSetsID, &w, nullptr), "Cannot read the 3D window settings");
	const Int32 width = GetInt (ws, "width", w.hSize);
	const Int32 height = GetInt (ws, "height", w.vSize);
	if (width < 16 || width > 8000 || height < 16 || height > 8000)
		Fail ("windowSize width/height must be 16..8000 pixels.");
	w.setWindowSize = true;
	w.hSize = (short) width;
	w.vSize = (short) height;
	w.setZoom = false;
	CallApi ("Change 3D window size", [&] () {
		API_3DWindowInfo copy = w;
		return ACAPI_Environment (APIEnv_Change3DWindowSetsID, &copy, nullptr);
	});
}


void ApplyStyle (const OS& params)
{
	auto style = OptString (params, "style");
	if (!style.has_value ())
		return;
	GS::Array<GS::UniString> names;
	GS::UniString current;
	ACAPI_Environment (APIEnv_Get3DStyleListID, &names, &current);
	GS::UniString found;
	for (const GS::UniString& n : names) {
		if (n == *style) { found = n; break; }
	}
	if (found.IsEmpty ()) {
		for (const GS::UniString& n : names) {
			if (EqualsIgnoreCase (n, *style)) { found = n; break; }
		}
	}
	if (found.IsEmpty ()) {
		GS::UniString list;
		for (const GS::UniString& n : names) {
			if (!list.IsEmpty ()) list += ", ";
			list += "'" + n + "'";
		}
		Fail ("3D style '" + *style + "' not found. Available: " + list + ".", APIERR_BADNAME);
	}
	CallApi ("Change 3D style", [&] () {
		GS::UniString s = found;
		return ACAPI_Environment (APIEnv_SetCurrent3DStyleID, &s, nullptr);
	});
}

API_RGBColor RgbFrom (const OS& c, const char* key)
{
	API_RGBColor rgb = {};
	rgb.f_red = GetDouble (c, "r");
	rgb.f_green = GetDouble (c, "g");
	rgb.f_blue = GetDouble (c, "b");
	if (rgb.f_red < 0.0 || rgb.f_red > 1.0 || rgb.f_green < 0.0 || rgb.f_green > 1.0 || rgb.f_blue < 0.0 || rgb.f_blue > 1.0)
		Fail ("'" + GS::UniString (key) + "' components r, g, b must be between 0 and 1.");
	return rgb;
}


short GetPercent (const OS& os, const char* key, short def)
{
	const Int32 v = GetInt (os, key, def);
	if (v < 0 || v > 100)
		Fail ("'" + GS::UniString (key) + "' must be 0..100.");
	return (short) v;
}


// Changes the definition of the CURRENT 3D style (the style attribute itself, like editing it in the 3D Styles dialog).
void ApplyStyleSettings (const OS& params)
{
	OS s;
	if (!TryGetObject (params, "styleSettings", s))
		return;
	GS::Array<GS::UniString> names;
	GS::UniString current;
	Check (ACAPI_Environment (APIEnv_Get3DStyleListID, &names, &current), "Cannot read the 3D styles");
	if (current.IsEmpty ())
		Fail ("There is no current 3D style to change. Pick one with 'style' (see get_3d_view style.available).", APIERR_GENERAL);
	API_3DStyle st;
	SetUStr (st.name, current);
	Check (ACAPI_Environment (APIEnv_Get3DStyleID, &st, nullptr), "Cannot read the current 3D style '" + current + "'");

	if (auto m = OptNamed (kModel3D, s, "model"))					st.model3D = (API_3DModelTypeID) *m;
	if (auto b = OptBool (s, "transparency"))						st.transparency = *b;
	if (auto b = OptBool (s, "monochrome"))							st.monochromeEnabled = *b;
	if (auto c = OptNamed (kShadContours, s, "contours"))			st.shadCont = (API_ShadingContoursID) *c;
	if (auto v = OptNamed (kVectShadow, s, "sunShadows"))			st.vectSunShadow = (API_VectorShadowID) *v;
	if (auto b = OptBool (s, "vectorHatching"))						st.vectHatchOn = *b;
	if (Has (s, "castShadowPercent"))								st.castShadowPercent = GetPercent (s, "castShadowPercent", st.castShadowPercent);
	if (Has (s, "shadingPercent"))									st.shadingPercent = GetPercent (s, "shadingPercent", st.shadingPercent);
	if (auto b = OptBool (s, "backgroundAsInRendering"))			st.bkgAsInRendering = *b;
	OS color;
	if (TryGetObject (s, "skyColor", color))						st.bkgSkyColor = RgbFrom (color, "skyColor");
	if (TryGetObject (s, "groundColor", color))						st.backGroundRGB = RgbFrom (color, "groundColor");

	CallApi ("Change the 3D style '" + current + "'", [&] () {
		API_3DStyle copy = st;
		return ACAPI_Environment (APIEnv_Change3DStyleID, &copy, nullptr);
	});
}

// --- Commands --------------------------------------------------------------------------------------

OS Get3DViewCmd (const OS&)
{
	OS out;
	out.Add ("window", CurrentWindowJson ());
	out.Add ("projection", ProjectionJson (GetProjection ()));
	out.Add ("style", StyleJson ());
	out.Add ("windowSize", WindowSizeJson ());
	out.Add ("filter", FilterJson ());
	out.Add ("cutPlanes", CutPlanesJson ());
	out.Add ("rendering", RenderingJson ());
	const API_Box3D ext = ModelExtent ();
	out.Add ("modelExtent", Box3DObj (ext));
	return out;
}


OS Set3DViewCmd (const OS& params)
{
	OS result;
	GS::Array<GS::UniString> changed;

	if (params.Contains ("style"))		{ ApplyStyle (params);			changed.Push ("style"); }
	if (params.Contains ("styleSettings")) { ApplyStyleSettings (params); changed.Push ("styleSettings"); }
	if (params.Contains ("windowSize"))	{ ApplyWindowSize (params);		changed.Push ("windowSize"); }
	if (params.Contains ("stories"))	{ ApplyStories (params);		changed.Push ("stories"); }
	if (params.Contains ("elementTypes")) { ApplyElementTypes (params);	changed.Push ("elementTypes"); }
	if (params.Contains ("cutPlanes"))	{ ApplyCutPlanes (params);		changed.Push ("cutPlanes"); }

	const bool hasPerspInput = params.Contains ("camera") || params.Contains ("target") || params.Contains ("viewCone") ||
							   params.Contains ("roll") || params.Contains ("twoPointPerspective") || params.Contains ("distance");
	const bool hasAxoInput = params.Contains ("projection") || params.Contains ("tranmat");
	const bool hasOrbit = params.Contains ("azimuth") || params.Contains ("altitude") || params.Contains ("distance");
	const bool hasSun = params.Contains ("sun");

	std::optional<bool> wantPersp;
	if (auto mode = OptString (params, "mode")) {
		if (EqualsIgnoreCase (*mode, "perspective"))
			wantPersp = true;
		else if (EqualsIgnoreCase (*mode, "axonometric") || EqualsIgnoreCase (*mode, "parallel"))
			wantPersp = false;
		else
			Fail ("mode must be 'perspective' or 'axonometric'.");
	} else if (hasPerspInput && !hasAxoInput) {
		wantPersp = true;
	} else if (hasAxoInput && !hasPerspInput) {
		wantPersp = false;
	}
	if (wantPersp.has_value () && *wantPersp && hasAxoInput)
		Fail ("'projection'/'tranmat' are axonometric settings; they cannot be combined with mode 'perspective'.");
	if (wantPersp.has_value () && !*wantPersp && hasPerspInput)
		Fail ("'camera'/'target'/'viewCone'/'roll'/'distance'/'twoPointPerspective' are perspective settings; use mode 'perspective'.");

	if (wantPersp.has_value () || hasOrbit || hasSun) {
		API_3DProjectionInfo proj = GetProjection ();
		const bool persp = wantPersp.value_or (proj.isPersp);
		if (persp != proj.isPersp) {
			Switch3DMode (persp);
			proj = GetProjection ();
		}
		if (persp) {
			const API_PerspPars& cur = proj.u.persp;
			Vec3 cam = V3 (cur.pos.x, cur.pos.y, cur.cameraZ);
			Vec3 tgt = V3 (cur.target.x, cur.target.y, cur.targetZ);
			const double viewCone = GetAngle (params, "viewCone", (cur.viewCone >= 1.0 && cur.viewCone <= 179.0) ? DegToRad (cur.viewCone) : DegToRad (60.0));
			if (viewCone < DegToRad (1.0) || viewCone > DegToRad (179.0))
				Fail ("viewCone must be between 1 and 179 degrees.");
			const double roll = GetAngle (params, "roll", DegToRad (cur.rollAngle));
			std::optional<bool> twoPoint = OptBool (params, "twoPointPerspective");

			if (params.Contains ("camera")) {
				const API_Coord3D c = GetCoord3D (params, "camera");
				cam = V3 (c.x, c.y, c.z);
			}
			bool targetGiven = false;
			if (params.Contains ("target")) {
				const API_Coord3D t = GetCoord3D (params, "target");
				tgt = V3 (t.x, t.y, t.z);
				targetGiven = true;
			}
			if (hasOrbit && !params.Contains ("camera")) {
				if (!targetGiven) {
					const API_Box3D ext = ModelExtent ();
					tgt = V3 ((ext.xMin + ext.xMax) / 2.0, (ext.yMin + ext.yMax) / 2.0, (ext.zMin + ext.zMax) / 2.0);
				}
				const double curAz = std::atan2 (cam.y - tgt.y, cam.x - tgt.x);
				const double az = GetAngle (params, "azimuth", curAz);
				const double alt = GetAngle (params, "altitude", DegToRad (30.0));
				if (alt < DegToRad (-89.9) || alt > DegToRad (89.9))
					Fail ("altitude must be between -89.9 and 89.9 degrees for a perspective (use mode 'axonometric' with projection 'TopView' for a plan view).");
				double dist = 0.0;
				if (auto d = OptDouble (params, "distance")) {
					if (*d <= 0.01)
						Fail ("distance must be > 0.01 m.");
					dist = *d;
				} else {
					const API_Box3D ext = ModelExtent ();
					const double radius = 0.5 * std::sqrt ((ext.xMax - ext.xMin) * (ext.xMax - ext.xMin) + (ext.yMax - ext.yMin) * (ext.yMax - ext.yMin) +
														   (ext.zMax - ext.zMin) * (ext.zMax - ext.zMin));
					const double vfov = 2.0 * std::atan (0.7 * std::tan (viewCone / 2.0));
					dist = GS::Max (1.0, radius / std::sin (GS::Min (viewCone, vfov) / 2.0) * 1.05);
				}
				cam = V3 (tgt.x + dist * std::cos (alt) * std::cos (az), tgt.y + dist * std::cos (alt) * std::sin (az), tgt.z + dist * std::sin (alt));
			}
			result.Add ("camera", ApplyPerspective (proj, cam, tgt, viewCone, roll, twoPoint, params));
			changed.Push ("projection");
		} else {
			API_AxonoPars& a = proj.u.axono;
			proj.isPersp = false;
			proj.camGuid = APINULLGuid;
			proj.actCamSet = APINULLGuid;
			if (params.Contains ("projection")) {
				const Int32 mode = ParseNamed (kProjModes, params, "projection");
				a.projMod = (short) mode;
			}
			if (auto az = OptDouble (params, "azimuth"))
				a.azimuth = NormDeg (*az);		// degrees in the projection settings
			if (params.Contains ("tranmat")) {
				const GS::Array<double> tm = GetNumberArray (params, "tranmat", true);
				if (tm.GetSize () != 12)
					Fail ("'tranmat' must have 12 numbers (3x4 row-major matrix, as returned by get_3d_view).");
				for (UIndex i = 0; i < 12; ++i)
					a.tranmat.tmx[i] = tm[i];
				if (!InvertTranmat (a.tranmat, a.invtranmat))
					Fail ("'tranmat' is singular (cannot be inverted).");
				if (!params.Contains ("projection"))
					a.projMod = API_Projection_Parallel;
			} else if (params.Contains ("altitude")) {
				const double alt = GetAngle (params, "altitude");
				if (alt < DegToRad (-90.0) || alt > DegToRad (90.0))
					Fail ("altitude must be between -90 and 90 degrees.");
				a.tranmat = ViewTranmat (DegToRad (a.azimuth), alt);
				InvertTranmat (a.tranmat, a.invtranmat);
				if (!params.Contains ("projection"))
					a.projMod = API_Projection_Parallel;
			}
			ApplySun (params, a.sunAngSets);
			const API_AxonoPars before = GetProjection ().u.axono;
			ChangeProjection (proj);
			changed.Push ("projection");

			// Report whether Archicad recomputed the view matrix for a preset (the API does not document which of
			// projMod / azimuth / tranmat wins), so the caller knows when to fall back to an explicit tranmat.
			const API_3DProjectionInfo after = GetProjection ();
			bool matrixChanged = false;
			for (int i = 0; i < 12; ++i) {
				if (std::fabs (after.u.axono.tranmat.tmx[i] - before.tranmat.tmx[i]) > 1e-9)
					matrixChanged = true;
			}
			const bool expectedChange = params.Contains ("tranmat") || params.Contains ("altitude") ||
										(params.Contains ("projection") && a.projMod != before.projMod) ||
										(params.Contains ("azimuth") && std::fabs (a.azimuth - before.azimuth) > 1e-6);
			OS axo ("viewMatrixChanged", matrixChanged);
			if (!matrixChanged && expectedChange)
				axo.Add ("warning", GS::UniString ("The 3D view matrix did not change. Check with capture_view; if the view is not the requested one, "
												   "pass 'tranmat' (copy it from get_3d_view of a view set up by hand) or azimuth + altitude."));
			result.Add ("axonometric", axo);
		}
	}

	if (GetBool (params, "open3D", true)) {
		API_WindowInfo w;
		BNZeroMemory (&w, sizeof (w));
		w.typeID = APIWind_3DModelID;
		OpenWindow (w, "the 3D window");
	}

	result.Add ("changed", changed);
	result.Add ("projection", ProjectionJson (GetProjection ()));
	result.Add ("window", CurrentWindowJson ());
	return result;
}

// --- Files --------------------------------------------------------------------------------------------

bool FileExists (const IO::Location& loc)
{
	bool contains = false;
	return IO::fileSystem.Contains (loc, &contains) == NoError && contains;
}


IO::Location OutputLocation (const OS& params)
{
	const GS::UniString path = GetString (params, "path");
#if defined (WINDOWS)
	const bool absolute = path.GetLength () > 2 && (path[1] == ':' || path.BeginsWith ("\\\\"));
#else
	const bool absolute = path.BeginsWith (GS::UniChar ('/'));
#endif
	if (!absolute)
		Fail ("'path' must be an absolute file path (e.g. /tmp/capture.png), got '" + path + "'.");
	IO::Location loc (path);
	if (loc.GetStatus () != NoError)
		Fail ("Invalid path '" + path + "'.");
	IO::Location folder (loc);
	folder.DeleteLastLocalName ();
	if (!FileExists (folder))
		Fail ("The folder of '" + path + "' does not exist.", APIERR_BADPARS);
	if (FileExists (loc) && IO::fileSystem.Delete (loc) != NoError)
		Fail ("Cannot overwrite '" + path + "'.", APIERR_READONLY);
	return loc;
}


bool IsJpeg (const OS& params)
{
	const GS::UniString format = GetString (params, "format", "png");
	if (EqualsIgnoreCase (format, "png"))
		return false;
	if (EqualsIgnoreCase (format, "jpeg") || EqualsIgnoreCase (format, "jpg"))
		return true;
	Fail ("format must be 'png' or 'jpeg'.");
}


// Temporarily resizes the 3D window (restored on destruction).
class Window3DSizeGuard {
public:
	Window3DSizeGuard () = default;
	~Window3DSizeGuard ()
	{
		if (active) {
			API_3DWindowInfo w = original;
			w.setWindowSize = true;
			w.setZoom = false;
			ACAPI_Environment (APIEnv_Change3DWindowSetsID, &w, nullptr);
		}
	}
	Window3DSizeGuard (const Window3DSizeGuard&) = delete;
	Window3DSizeGuard& operator= (const Window3DSizeGuard&) = delete;

	void Resize (std::optional<Int32> width, std::optional<Int32> height)
	{
		Check (ACAPI_Environment (APIEnv_Get3DWindowSetsID, &original, nullptr), "Cannot read the 3D window size");
		Int32 w = width.value_or (0), h = height.value_or (0);
		if (w <= 0 && h > 0 && original.vSize > 0)	w = (Int32) std::lround ((double) h * original.hSize / original.vSize);
		if (h <= 0 && w > 0 && original.hSize > 0)	h = (Int32) std::lround ((double) w * original.vSize / original.hSize);
		if (w < 16 || w > 8000 || h < 16 || h > 8000)
			Fail ("width/height must be 16..8000 pixels.");
		API_3DWindowInfo changed = original;
		changed.setWindowSize = true;
		changed.hSize = (short) w;
		changed.vSize = (short) h;
		changed.setZoom = false;
		CallApi ("Resize the 3D window", [&] () {
			API_3DWindowInfo copy = changed;
			return ACAPI_Environment (APIEnv_Change3DWindowSetsID, &copy, nullptr);
		});
		active = true;
	}

private:
	bool				active = false;
	API_3DWindowInfo	original = {};
};


OS CaptureViewCmd (const OS& params)
{
	const API_WindowInfo window = GetCurrentWindow ();
	const bool is3D = window.typeID == APIWind_3DModelID;
	if (!is3D && !Is2DWindow (window.typeID))
		Fail ("The active window (" + WindowTypeName (window.typeID) + ") cannot be saved as a picture. Open a floor plan, section, elevation, "
			  "detail, worksheet, layout or the 3D window first (open_view).", APIERR_BADWINDOW);

	const bool jpeg = IsJpeg (params);
	IO::Location loc = OutputLocation (params);

	Window3DSizeGuard sizeGuard;
	const std::optional<Int32> width = OptInt (params, "width");
	const std::optional<Int32> height = OptInt (params, "height");
	if (is3D && (width.has_value () || height.has_value ()))
		sizeGuard.Resize (width, height);

	API_FileSavePars fsp = {};
	fsp.fileTypeID = jpeg ? APIFType_JPEGFile : APIFType_PNGFile;
	fsp.file = &loc;

	API_SavePars_Picture pic = {};
	pic.colorDepth = kTrueColor;
	pic.dithered = false;
	pic.view2D = !is3D;
	pic.crop = GetBool (params, "cropToWindow", true);
	pic.keepSelectionHighlight = GetBool (params, "keepSelectionHighlight", false);

	auto save = [&] () -> GSErrCode {
		API_FileSavePars f = fsp;
		API_SavePars_Picture p = pic;
		return ACAPI_Automate (APIDo_SaveID, &f, &p);
	};
	GSErrCode err = save ();
	if (err == APIERR_NEEDSUNDOSCOPE)
		Undoable ("Save picture (Claude)", [&] () { err = save (); });
	if (err == APIERR_REFUSEDCMD)
		Fail ("Archicad refused to save the picture right now (a dialog may be open, or another operation is running). Close dialogs and retry.", err);
	Check (err, "Saving the " + WindowTypeName (window.typeID) + " window as a picture failed");
	if (!FileExists (loc))
		Fail ("Archicad reported success but did not write the picture file.", APIERR_GENERAL);

	OS out;
	out.Add ("path", GetString (params, "path"));
	out.Add ("format", GS::UniString (jpeg ? "jpeg" : "png"));
	out.Add ("window", CurrentWindowJson ());
	if (is3D)
		out.Add ("windowSize3D", WindowSizeJson ());
	return out;
}


// Holds the rendering image settings and restores them (and frees the background picture location).
class RenderSizeGuard {
public:
	RenderSizeGuard () { BNZeroMemory (&original, sizeof (original)); }
	~RenderSizeGuard ()
	{
		if (changed) {
			API_RendImage img = original;
			ACAPI_Environment (APIEnv_ChangeRenderingSetsID, &img, reinterpret_cast<void*> ((GS::IntPtr) APIRendSet_ImageID));
		}
		delete original.bkgPictFile;
	}
	RenderSizeGuard (const RenderSizeGuard&) = delete;
	RenderSizeGuard& operator= (const RenderSizeGuard&) = delete;

	bool Load ()
	{
		loaded = ACAPI_Environment (APIEnv_GetRenderingSetsID, &original, reinterpret_cast<void*> ((GS::IntPtr) APIRendSet_ImageID), nullptr) == NoError;
		return loaded;
	}

	void Resize (std::optional<Int32> width, std::optional<Int32> height)
	{
		if (!loaded)
			Fail ("Cannot read the rendering image size settings.", APIERR_GENERAL);
		Int32 w = width.value_or (0), h = height.value_or (0);
		if (w <= 0 && h > 0 && original.vSize > 0)	w = (Int32) std::lround ((double) h * original.hSize / original.vSize);
		if (h <= 0 && w > 0 && original.hSize > 0)	h = (Int32) std::lround ((double) w * original.vSize / original.hSize);
		if (w < 16 || w > 16000 || h < 16 || h > 16000)
			Fail ("width/height must be 16..16000 pixels.");
		API_RendImage img = original;
		img.hSize = (short) GS::Min (w, 32000);
		img.vSize = (short) GS::Min (h, 32000);
		CallApi ("Change the rendering size", [&] () {
			API_RendImage copy = img;
			return ACAPI_Environment (APIEnv_ChangeRenderingSetsID, &copy, reinterpret_cast<void*> ((GS::IntPtr) APIRendSet_ImageID));
		});
		changed = true;
	}

	OS SizeJson () const
	{
		API_RendImage img;
		BNZeroMemory (&img, sizeof (img));
		OS out;
		if (ACAPI_Environment (APIEnv_GetRenderingSetsID, &img, reinterpret_cast<void*> ((GS::IntPtr) APIRendSet_ImageID), nullptr) == NoError) {
			out.Add ("width", (Int32) img.hSize);
			out.Add ("height", (Int32) img.vSize);
			delete img.bkgPictFile;
		}
		return out;
	}

private:
	bool			loaded = false;
	bool			changed = false;
	API_RendImage	original;
};


OS RenderViewCmd (const OS& params)
{
	const bool jpeg = IsJpeg (params);
	IO::Location loc = OutputLocation (params);

	GS::UniString sceneUsed;
	if (auto scene = OptString (params, "scene")) {
		GS::Array<GS::UniString> scenes;
		ACAPI_Environment (APIEnv_GetRenderingSceneNamesID, &scenes);
		for (const GS::UniString& s : scenes) {
			if (s == *scene || (sceneUsed.IsEmpty () && EqualsIgnoreCase (s, *scene)))
				sceneUsed = s;
		}
		if (sceneUsed.IsEmpty ()) {
			GS::UniString list;
			for (const GS::UniString& s : scenes) {
				if (!list.IsEmpty ()) list += ", ";
				list += "'" + s + "'";
			}
			Fail ("Rendering scene '" + *scene + "' not found. Available: " + (list.IsEmpty () ? GS::UniString ("(none)") : list) + ".", APIERR_BADNAME);
		}
		CallApi ("Set the rendering scene", [&] () {
			GS::UniString s = sceneUsed;
			return ACAPI_Environment (APIEnv_SetCurrentRenderingSceneID, &s);
		});
	}

	RenderSizeGuard sizeGuard;
	sizeGuard.Load ();
	const std::optional<Int32> width = OptInt (params, "width");
	const std::optional<Int32> height = OptInt (params, "height");
	if (width.has_value () || height.has_value ())
		sizeGuard.Resize (width, height);
	const OS imageSize = sizeGuard.SizeJson ();

	auto render = [&] () -> GSErrCode {
		API_PhotoRenderPars pars = {};
		pars.fileTypeID = jpeg ? APIFType_JPEGFile : APIFType_PNGFile;
		pars.file = &loc;
		pars.colorDepth = kTrueColor;
		pars.dithered = false;
		return ACAPI_Automate (APIDo_PhotoRenderID, &pars, nullptr);
	};
	GSErrCode err = render ();
	if (err == APIERR_NEEDSUNDOSCOPE)
		Undoable ("Photo render (Claude)", [&] () { err = render (); });
	if (err == APIERR_BADWINDOW || err == APIERR_REFUSEDCMD) {
		API_WindowInfo current;
		if (!TryGetCurrentWindow (current) || current.typeID != APIWind_3DModelID) {
			API_WindowInfo w;
			BNZeroMemory (&w, sizeof (w));
			w.typeID = APIWind_3DModelID;
			OpenWindow (w, "the 3D window");
			err = render ();
		}
	}
	if (err == APIERR_MODULNOTINSTALLED || err == APIERR_MISSINGCODE)
		Fail ("Photo rendering is not available in this Archicad (rendering engine missing): " + ErrorName (err) + ". Use capture_view of the 3D window instead.", err);
	Check (err, "Photo rendering failed");

	OS out;
	out.Add ("path", GetString (params, "path"));
	out.Add ("format", GS::UniString (jpeg ? "jpeg" : "png"));
	out.Add ("fileWritten", FileExists (loc));
	if (!sceneUsed.IsEmpty ())
		out.Add ("scene", sceneUsed);
	out.Add ("imageSize", imageSize);
	out.Add ("projection", ProjectionJson (GetProjection ()));
	return out;
}

} // namespace


void RegisterView3DCommands ()
{
	RegisterCommand ("Get3DModelStats",
		"Counts the bodies / polygons / vertices of the model currently generated for the 3D window. Archicad builds the 3D model "
		"asynchronously in its idle loop; capture_view polls this until the counts are stable before saving a 3D picture. Output: {bodies, polygons, vertices}.",
		[] (const OS&) -> OS {
			void* windowSight = nullptr;
			void* previous = nullptr;
			bool selected = false;
			if (ACAPI_3D_GetCurrentWindowSight (&windowSight) == NoError && windowSight != nullptr)
				selected = ACAPI_3D_SelectSight (windowSight, &previous) == NoError;
			Int32 bodies = 0, pgons = 0, verts = 0;
			ACAPI_3D_GetNum (API_BodyID, &bodies);
			ACAPI_3D_GetNum (API_PgonID, &pgons);
			ACAPI_3D_GetNum (API_VertID, &verts);
			if (selected) {
				void* dummy = nullptr;
				ACAPI_3D_SelectSight (previous, &dummy);
			}
			return OS ("bodies", bodies, "polygons", pgons, "vertices", verts);
		});

	RegisterCommand ("Get3DView",
		"Returns the 3D view state: projection (perspective: camera/target {x,y,z} m, viewCone, roll, distance, azimuth, viewDirection; "
		"axonometric: projection preset, azimuth, tranmat), sun, 3D style (current + available), 3D window size, 3D filter (stories, mode, "
		"element types), cutting planes, rendering scenes/image size and the model extent box.",
		Get3DViewCmd);

	RegisterCommand ("Set3DView",
		"Changes the 3D view. Perspective: {mode:'perspective', camera?:{x,y,z}, target?:{x,y,z}} or orbit {azimuth (deg, from target to camera, "
		"CCW from +X), altitude (deg), distance? (m, default: fit model), target? (default: model center)}, viewCone?, roll?, twoPointPerspective?. "
		"Axonometric: {mode:'axonometric', projection?: Isometric|Dimetric|Monometric|Frontal|TopView|FrontView|SideView|BottomView|...|Parallel, "
		"azimuth?, altitude? (free parallel view), tranmat?}. Also sun {azimuth, altitude}, style (3D style name), styleSettings {model: "
		"Block|Wireframe|HiddenLine|Shading, transparency, monochrome, contours, sunShadows, vectorHatching, castShadowPercent, shadingPercent, "
		"backgroundAsInRendering, skyColor/groundColor {r,g,b 0..1}} (edits the current 3D style), windowSize {width,height}, "
		"stories {all?, from?, to?, trim?}, elementTypes [types]|['all'], cutPlanes {enabled?, shapes?}, open3D (default true).",
		Set3DViewCmd);

	RegisterCommand ("CaptureView",
		"Saves the ACTIVE window (floor plan, section, elevation, detail, worksheet, layout, 3D) as a PNG/JPEG picture. Input: {path (absolute), "
		"format?: png|jpeg, width?/height? (3D window pixel size during the capture), keepSelectionHighlight? (false), cropToWindow? (true = visible "
		"area)}. Output: {path, format, window}.",
		CaptureViewCmd);

	RegisterCommand ("RenderView",
		"Photo-renders the current 3D view (camera/projection of the 3D window) with the current or given rendering scene into a PNG/JPEG file. "
		"Input: {path (absolute), format?, width?, height? (pixels, restored afterwards), scene?}. Can take minutes. Output: {path, imageSize, scene}.",
		RenderViewCmd);
}

} // namespace views
} // namespace cc
