// *****************************************************************************
// AttributesProfiles — complex profile geometry (ProfileVectorImage) for the
// attribute commands: building a profile from JSON shapes and describing an
// existing profile's size. Based on the DevKit Attribute_Test example
// (BuildProfileDescription / Do_CreateProfile).
//
// Shape JSON (lengths in meters, profile plane: x = horizontal, y = vertical):
//   {polygon: [{x,y},...] | {points, holes?},   straight edges only
//    buildingMaterial: ref,                     fill, pens and edge surface come from it
//    core?: true, finish?: false,
//    contourVisible?: true, contourPen?: 1..255 (default: the material's cut fill pen),
//    contourLineType?: ref (default 1 = solid), cutEndLinePen?, cutEndLineType?}
// *****************************************************************************

// The VectorImage headers pull in GSRoot/Algorithms.hpp, which still uses std::random_shuffle
// (removed from libc++ in C++17); re-enable it for this translation unit only.
#ifndef _LIBCPP_ENABLE_CXX17_REMOVED_RANDOM_SHUFFLE
#define _LIBCPP_ENABLE_CXX17_REMOVED_RANDOM_SHUFFLE
#endif

#include "Commands/AttributesCommon.hpp"
#include "Core/Polygon.hpp"

#include "Box2DData.h"
#include "ExtendedPen.hpp"
#include "HatchTran.hpp"
#include "OverriddenAttributes.hpp"
#include "ProfileAdditionalInfo.hpp"
#include "ProfileVectorImage.hpp"

#include <memory>
#include <new>

namespace cc {
namespace attr {

namespace {

void AddContour (const Contour& c, GS::Array<Coord>& coords, GS::Array<UInt32>& boends, const GS::UniString& what)
{
	if (!c.arcs.IsEmpty ())
		Fail (what + ": curved edges are not supported in profile shapes yet; approximate the arc with points.");
	if (c.points.GetSize () < 3)
		Fail (what + " needs at least 3 points.");
	for (const API_Coord& p : c.points)
		coords.Push (Coord (p.x, p.y));
	coords.Push (Coord (c.points[0].x, c.points[0].y));		// close the contour
	boends.Push (coords.GetSize () - 1);
}


void AddShape (ProfileVectorImage& image, const OS& shape, UIndex shapeIndex)
{
	const GS::UniString what = "shapes[" + GS::ValueToUniString ((Int32) shapeIndex) + "]";
	if (!shape.Contains ("polygon"))
		Fail (what + " needs 'polygon' (points in meters, x horizontal, y vertical).");

	PolygonData poly = GetPolygon (shape, "polygon");
	NormalizeOrientation (poly);

	const API_AttributeIndex bmIndex = ResolveRef (API_BuildingMaterialID, shape, "buildingMaterial");
	AttrData bm;
	Check (bm.Load (API_BuildingMaterialID, bmIndex), what + ": cannot read the building material");
	const API_BuildingMaterialType& b = bm.attr.buildingMaterial;

	const short contourPen = OptPen (shape, "contourPen").value_or (b.cutFillPen >= 1 ? b.cutFillPen : (short) 1);
	const API_AttributeIndex contourLine = OptRef (API_LinetypeID, shape, "contourLineType").value_or (1);
	const short cutEndPen = OptPen (shape, "cutEndLinePen").value_or (contourPen);
	const API_AttributeIndex cutEndLine = OptRef (API_LinetypeID, shape, "cutEndLineType").value_or (contourLine);
	const bool contourVisible = GetBool (shape, "contourVisible", true);
	const bool core = GetBool (shape, "core", true);
	const bool finish = GetBool (shape, "finish", false);

	// 1-based coordinates (index 0 unused), every contour closed; boends[k] = closing vertex of contour k.
	GS::Array<Coord> coords;
	GS::Array<UInt32> boends;
	coords.Push (Coord (0.0, 0.0));
	boends.Push (0);
	AddContour (poly.outline, coords, boends, what);
	for (UIndex h = 0; h < poly.holes.GetSize (); ++h)
		AddContour (poly.holes[h], coords, boends, what + ".holes[" + GS::ValueToUniString ((Int32) h) + "]");
	const Int32 nCoords = (Int32) coords.GetSize () - 1;

	// Profile additional info: one ProfileItem followed by (nCoords + 1) ProfileEdgeData.
	const GSSize size = (GSSize) sizeof (ProfileItem) + (GSSize) (nCoords + 1) * (GSSize) sizeof (ProfileEdgeData);
	GSHandle addInfo = BMAllocateHandle (size, ALLOCATE_CLEAR, 0);
	if (addInfo == nullptr)
		Fail ("Out of memory.", APIERR_MEMFULL);

	ProfileItem* item = new (*addInfo) ProfileItem ();
	item->SetCutEndLinePen (cutEndPen);
	item->SetCutEndLineType (cutEndLine);
	item->SetVisibleCutEndLines (true);
	item->SetCore (core);
	item->SetFinish (finish);

	ProfileEdgeData* edges = reinterpret_cast<ProfileEdgeData*> (*addInfo + sizeof (ProfileItem));
	new (&edges[0]) ProfileEdgeData (0, 0, 0, 0);
	for (Int32 i = 1; i <= nCoords; ++i) {
		new (&edges[i]) ProfileEdgeData (b.cutMaterial, contourPen, contourLine,
										 ProfileEdgeData::IsVisibleLineFlag | ProfileEdgeData::SurfaceFromBuildMatFlag);
	}

	GX::Pattern::HatchTran hatchTrafo;
	hatchTrafo.SetGlobal ();

	GS::UniString failure;
	try {
		image.AddHatchObsolete (contourVisible,
								VBAttr::ExtendedPen (contourPen),
								contourLine,
								bmIndex,
								b.cutFill,
								VBAttr::OverriddenExtendedPen (VBAttr::DoNotUseThisAttribute, VBAttr::ExtendedPen (b.cutFillPen)),
								VBAttr::OverriddenPen (VBAttr::DoNotUseThisAttribute, b.cutFillBackgroundPen),
								hatchTrafo,
								(Int32) boends.GetSize () - 1,
								boends.GetContent (),
								nCoords,
								coords.GetContent (),
								nullptr,
								1,				// fill category: cut fill
								addInfo,
								nullptr);
	} catch (const GS::Exception& e) {
		failure = e.GetMessage ();
	} catch (...) {
		failure = "unknown error";
	}
	BMKillHandle (&addInfo);
	if (!failure.IsEmpty ())
		Fail (what + ": cannot build the profile geometry (" + failure + "). Check that the polygon is simple (not self-intersecting).");
}

} // namespace


ProfileVectorImage* NewProfileImage (const GS::Array<OS>& shapes)
{
	if (shapes.IsEmpty ())
		Fail ("A profile needs at least one shape: shapes: [{polygon, buildingMaterial}].");
	std::unique_ptr<ProfileVectorImage> image (new ProfileVectorImage ());
	for (UIndex i = 0; i < shapes.GetSize (); ++i)
		AddShape (*image, shapes[i], i);
	return image.release ();
}


void SetProfileImage (API_AttributeDefExt& defs, ProfileVectorImage* image)
{
	if (defs.profile_vectorImageItems != nullptr && defs.profile_vectorImageItems != image)
		delete defs.profile_vectorImageItems;
	defs.profile_vectorImageItems = image;
	if (defs.profile_vectorImageParameterNames != nullptr) {		// stretch parameter names of the old geometry
		delete defs.profile_vectorImageParameterNames;
		defs.profile_vectorImageParameterNames = nullptr;
	}
}


void AddProfileGeometryJson (const ProfileVectorImage& image, OS& out)
{
	Box2DData box;
	if (image.GetBounds (&box, true) == NoError && box.xMax >= box.xMin && box.yMax >= box.yMin) {
		out.Add ("width", box.xMax - box.xMin);
		out.Add ("height", box.yMax - box.yMin);
		OS bounds ("xMin", box.xMin, "yMin", box.yMin, "xMax", box.xMax);
		bounds.Add ("yMax", box.yMax);
		out.Add ("bounds", bounds);
	}
	out.Add ("hatchCount", (Int32) image.GetItemCountOfType (SyHatch));
}

} // namespace attr
} // namespace cc
