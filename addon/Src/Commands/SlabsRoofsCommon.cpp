// SlabsRoofsCommon.cpp — shared helpers of the slabs-roofs family (see SlabsRoofsCommon.hpp).

#include "Commands/SlabsRoofsCommon.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cc {
namespace slabroof {

namespace {

GS::UniString Key (const char* key)
{
	return GS::UniString (key);
}


template <class T>
T** NewHandle (Int32 count)
{
	GSHandle h = BMAllocateHandle ((GSSize) (count > 0 ? count : 0) * (GSSize) sizeof (T), ALLOCATE_CLEAR, 0);
	if (h == nullptr)
		Fail ("Out of memory.", APIERR_MEMFULL);
	return reinterpret_cast<T**> (h);
}


template <class T>
T* NewPtr (Int32 count)
{
	GSPtr p = BMAllocatePtr ((GSSize) (count > 0 ? count : 0) * (GSSize) sizeof (T), ALLOCATE_CLEAR, 0);
	if (p == nullptr)
		Fail ("Out of memory.", APIERR_MEMFULL);
	return reinterpret_cast<T*> (p);
}


template <class T>
void KillHandleOf (T**& h)
{
	if (h != nullptr) {
		GSHandle gh = reinterpret_cast<GSHandle> (h);
		BMKillHandle (&gh);
		h = nullptr;
	}
}


template <class T>
void KillPtrOf (T*& p)
{
	if (p != nullptr) {
		GSPtr gp = reinterpret_cast<GSPtr> (p);
		BMKillPtr (&gp);
		p = nullptr;
	}
}


// Copies a handle into a freshly allocated one (nullptr stays nullptr).
template <class T>
T** CopyHandle (T** src)
{
	if (src == nullptr)
		return nullptr;
	const GSSize bytes = BMGetHandleSize (reinterpret_cast<GSConstHandle> (src));
	GSHandle h = BMAllocateHandle (bytes, ALLOCATE_CLEAR, 0);
	if (h == nullptr)
		Fail ("Out of memory.", APIERR_MEMFULL);
	if (bytes > 0)
		std::memcpy (*h, *reinterpret_cast<GSHandle> (src), (size_t) bytes);
	return reinterpret_cast<T**> (h);
}


API_Coord3D Normalized (const API_Coord3D& v, const GS::UniString& what)
{
	const double len = std::sqrt (v.x * v.x + v.y * v.y + v.z * v.z);
	if (len < 1e-12)
		Fail (what + " must not be a zero vector.");
	return API_Coord3D { v.x / len, v.y / len, v.z / len };
}


API_Coord3D Cross (const API_Coord3D& a, const API_Coord3D& b)
{
	return API_Coord3D { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}


// RAII owner of the result of APIAny_RegularizePolygonID.
class RegularizedPolys {
public:
	~RegularizedPolys ()
	{
		if (polys != nullptr) {
			for (Int32 i = 0; i < count; ++i)
				ACAPI_Goodies (APIAny_DisposeRegularizedPolyID, &(*polys)[i]);
			GSHandle h = reinterpret_cast<GSHandle> (polys);
			BMKillHandle (&h);
		}
	}
	Int32					count = 0;
	API_RegularizedPoly**	polys = nullptr;
};

} // namespace

// --- Mask helpers ------------------------------------------------------------------------

void FieldMask::SetBytes (const void* field, size_t size) const
{
	if (mask == nullptr)
		return;
	const char* base = reinterpret_cast<const char*> (&element);
	const char* f = reinterpret_cast<const char*> (field);
	const std::ptrdiff_t offset = f - base;
	if (offset < 0 || size == 0 || offset + (std::ptrdiff_t) size > (std::ptrdiff_t) sizeof (API_Element))
		Fail ("Internal error: mask field outside of the element structure.", APIERR_GENERAL);
	std::memset (reinterpret_cast<char*> (mask) + offset, 0xFF, size);
}


bool AnyMasked (const API_Element& mask)
{
	const char* bytes = reinterpret_cast<const char*> (&mask);
	for (size_t i = 0; i < sizeof (mask); ++i) {
		if (bytes[i] != 0)
			return true;
	}
	return false;
}


void CommitStructChange (API_Element& element, API_Element& mask, const GS::UniString& what)
{
	if (!AnyMasked (mask))
		return;
	Check (ACAPI_Element_Change (&element, &mask, nullptr, 0, true), what);
	ACAPI_ELEMENT_MASK_CLEAR (mask);
}

// --- Small field helpers -------------------------------------------------------------------

bool ApplyPen (const OS& spec, const char* key, short& pen)
{
	auto v = OptInt (spec, key);
	if (!v.has_value ())
		return false;
	if (*v < 1 || *v > 255)
		Fail ("Field '" + Key (key) + "' must be a pen index 1-255.");
	pen = (short) *v;
	return true;
}


bool ApplyAttr (const OS& spec, const char* key, API_AttrTypeID type, API_AttributeIndex& index)
{
	auto v = OptAttr (type, spec, key);
	if (!v.has_value ())
		return false;
	index = *v;
	return true;
}


bool ApplyFlag (const OS& spec, const char* key, bool& value)
{
	auto v = OptBool (spec, key);
	if (!v.has_value ())
		return false;
	value = *v;
	return true;
}


bool ApplyLength (const OS& spec, const char* key, double& value, bool positive)
{
	auto v = OptDouble (spec, key);
	if (!v.has_value ())
		return false;
	if (positive && *v <= 0.0)
		Fail ("Field '" + Key (key) + "' must be greater than 0 (meters).");
	value = *v;
	return true;
}


bool ApplyAngleDeg (const OS& spec, const char* key, double& radians)
{
	auto v = OptAngle (spec, key);
	if (!v.has_value ())
		return false;
	radians = *v;
	return true;
}


void CheckCompositeUsage (const OS& spec, API_AttributeIndex composite, short usageFlag, const char* elementPlural)
{
	if (!Has (spec, "composite"))
		return;
	API_Attribute attr;
	BNZeroMemory (&attr, sizeof (attr));
	attr.header.typeID = API_CompWallID;
	attr.header.index = composite;
	GS::UniString name;
	attr.header.uniStringNamePtr = &name;
	if (ACAPI_Attribute_Get (&attr) != NoError)
		return;		// existence was already checked when the reference was resolved
	if ((attr.header.flags & usageFlag) != 0)
		return;
	Fail ("Composite '" + name + "' is not enabled for " + GS::UniString (elementPlural) +
		  " (see 'usage' in get_attributes type 'Composite'). Pick a composite whose usage includes " + GS::UniString (elementPlural) +
		  ", or enable it first with modify_attributes (usage).", APIERR_BADPARS);
}


bool ApplyPenOverride (const OS& spec, API_PenOverrideType& po)
{
	bool changed = false;
	auto one = [&] (const char* key, short& pen, bool& flag) {
		if (!spec.Contains (key))
			return;
		changed = true;
		if (spec.IsBool (key)) {
			if (GetBool (spec, key))
				Fail ("Field '" + Key (key) + "' must be a pen index 1-255, or false to remove the override.");
			flag = false;
			return;
		}
		ApplyPen (spec, key, pen);
		flag = true;
	};
	one ("cutFillPen", po.cutFillPen, po.overrideCutFillPen);
	one ("cutFillBackgroundPen", po.cutFillBackgroundPen, po.overrideCutFillBackgroundPen);
	return changed;
}


void AddPenOverrideJson (OS& out, const API_PenOverrideType& po)
{
	if (po.overrideCutFillPen)
		out.Add ("cutFillPen", (Int32) po.cutFillPen);
	if (po.overrideCutFillBackgroundPen)
		out.Add ("cutFillBackgroundPen", (Int32) po.cutFillBackgroundPen);
}


bool ApplyStoryVisibility (const OS& spec, API_StoryVisibility& contour, API_StoryVisibility& fill)
{
	OS v;
	if (!TryGetObject (spec, "storyVisibility", v))
		return false;
	auto count = [&] (const char* key) -> std::optional<short> {
		auto n = OptInt (v, key);
		if (!n.has_value ())
			return std::nullopt;
		if (*n < 0 || *n > 999)
			Fail ("storyVisibility." + Key (key) + " must be 0..999.");
		return (short) *n;
	};
	const auto home = OptBool (v, "onHomeStory");
	const auto allAbove = OptBool (v, "allAbove");
	const auto allBelow = OptBool (v, "allBelow");
	const auto above = count ("storiesAbove");
	const auto below = count ("storiesBelow");
	for (API_StoryVisibility* s : { &contour, &fill }) {
		if (home.has_value ())		s->showOnHome = *home;
		if (allAbove.has_value ())	s->showAllAbove = *allAbove;
		if (allBelow.has_value ())	s->showAllBelow = *allBelow;
		if (above.has_value ())		s->showRelAbove = *above;
		if (below.has_value ())		s->showRelBelow = *below;
	}
	return true;
}


OS StoryVisibilityJson (const API_StoryVisibility& v)
{
	return OS ("onHomeStory", v.showOnHome, "allAbove", v.showAllAbove, "allBelow", v.showAllBelow,
			   "storiesAbove", (Int32) v.showRelAbove, "storiesBelow", (Int32) v.showRelBelow);
}


bool ApplyEdgeTrim (const OS& spec, API_EdgeTrim& trim, const char* trimKey, const char* angleKey)
{
	bool changed = false;
	if (Has (spec, trimKey)) {
		trim.sideType = (API_EdgeTrimID) ParseNamed (kEdgeTrims, spec, trimKey);
		changed = true;
	}
	if (auto a = OptAngle (spec, angleKey)) {
		if (*a <= 0.0 || *a >= kPi)
			Fail ("Field '" + Key (angleKey) + "' must be between 0 and 180 degrees (90 = vertical edge).");
		trim.sideAngle = *a;
		if (!Has (spec, trimKey))
			trim.sideType = APIEdgeTrim_CustomAngle;
		changed = true;
	}
	return changed;
}


void AddEdgeTrimJson (OS& out, const API_EdgeTrim& trim, const char* trimKey, const char* angleKey)
{
	out.Add (trimKey, NameOf (kEdgeTrims, trim.sideType));
	if (trim.sideType == APIEdgeTrim_CustomAngle)
		out.Add (angleKey, RadToDeg (trim.sideAngle));
}


bool SameTrim (const API_EdgeTrim& a, const API_EdgeTrim& b)
{
	if (a.sideType != b.sideType)
		return false;
	return a.sideType != APIEdgeTrim_CustomAngle || std::fabs (a.sideAngle - b.sideAngle) < 1e-9;
}


bool SameOverride (const API_OverriddenAttribute& a, const API_OverriddenAttribute& b)
{
	if (a.overridden != b.overridden)
		return false;
	return !a.overridden || a.attributeIndex == b.attributeIndex;
}

// --- Polygon helpers ---------------------------------------------------------------------------

Int32 HandleCount (GSConstHandle h, size_t itemSize)
{
	if (h == nullptr || itemSize == 0)
		return 0;
	return (Int32) (BMGetHandleSize (h) / (GSSize) itemSize);
}


Int32 PtrCount (GSConstPtr p, size_t itemSize)
{
	if (p == nullptr || itemSize == 0)
		return 0;
	return (Int32) (BMGetPtrSize (p) / (GSSize) itemSize);
}


GS::Array<ContourRange> ContoursOf (Int32** pends)
{
	GS::Array<ContourRange> out;
	const Int32 nSub = HandleCount (reinterpret_cast<GSConstHandle> (pends), sizeof (Int32)) - 1;
	for (Int32 k = 1; k <= nSub; ++k) {
		ContourRange r;
		r.first = (*pends)[k - 1] + 1;
		r.count = (*pends)[k] - r.first;
		if (r.count > 0)
			out.Push (r);
	}
	return out;
}


API_Polygon PolygonCounts (API_Coord** coords, Int32** pends, API_PolyArc** parcs)
{
	API_Polygon poly;
	BNZeroMemory (&poly, sizeof (poly));
	const Int32 nSub = HandleCount (reinterpret_cast<GSConstHandle> (pends), sizeof (Int32)) - 1;
	const Int32 nCoordsHandle = HandleCount (reinterpret_cast<GSConstHandle> (coords), sizeof (API_Coord)) - 1;
	if (nSub < 1 || nCoordsHandle < 1)
		return poly;
	poly.nSubPolys = nSub;
	poly.nCoords = std::min ((*pends)[nSub], nCoordsHandle);
	poly.nArcs = HandleCount (reinterpret_cast<GSConstHandle> (parcs), sizeof (API_PolyArc));
	return poly;
}


GS::Array<bool> ReversedContours (const PolygonData& data)
{
	GS::Array<bool> out;
	out.Push (SignedArea (data.outline.points) < 0.0);
	for (const Contour& h : data.holes)
		out.Push (SignedArea (h.points) > 0.0);
	return out;
}


Int32 MapEdgeIndex (Int32 index, Int32 count, bool reversed)
{
	if (!reversed || count <= 0)
		return index;
	return ((count - 2 - index) % count + count) % count;
}


void ReshapePolygonInPlace (API_ElementMemo& memo, const PolygonData& input)
{
	PolygonData data = input;
	NormalizeOrientation (data);

	if (memo.coords == nullptr || memo.pends == nullptr)
		Fail ("The element has no polygon data that could be modified.", APIERR_BADPOLY);

	const Int32 existingCoords = HandleCount (reinterpret_cast<GSConstHandle> (memo.coords), sizeof (API_Coord)) - 1;
	// The polygon primitives require initialized vertexIDs / parcs handles; GetMemo may leave them null.
	if (memo.vertexIDs == nullptr)
		memo.vertexIDs = NewHandle<UInt32> (existingCoords + 1);
	if (memo.parcs == nullptr)
		memo.parcs = NewHandle<API_PolyArc> (0);

	// 1. remove every hole (highest index first)
	const Int32 nSub = HandleCount (reinterpret_cast<GSConstHandle> (memo.pends), sizeof (Int32)) - 1;
	for (Int32 k = nSub; k >= 2; --k) {
		Int32 idx = k;
		Check (ACAPI_Goodies (APIAny_DeleteSubPolyID, &memo, &idx), "Cannot remove a hole of the existing polygon");
	}

	// 2. resize the outline to the requested vertex count
	const Int32 desired = (Int32) data.outline.points.GetSize ();
	Int32 count = (*memo.pends)[1] - 1;
	while (count > desired) {
		Int32 idx = 1;
		Check (ACAPI_Goodies (APIAny_DeletePolyNodeID, &memo, &idx), "Cannot adjust the vertex count of the polygon");
		--count;
	}
	while (count < desired) {
		Int32 idx = 2;		// position the new node will occupy (between nodes 1 and 2)
		API_Coord mid;
		mid.x = ((*memo.coords)[1].x + (*memo.coords)[2].x) / 2.0;
		mid.y = ((*memo.coords)[1].y + (*memo.coords)[2].y) / 2.0;
		Check (ACAPI_Goodies (APIAny_InsertPolyNodeID, &memo, &idx, &mid), "Cannot adjust the vertex count of the polygon");
		++count;
	}
	if ((*memo.pends)[1] != desired + 1 ||
		HandleCount (reinterpret_cast<GSConstHandle> (memo.coords), sizeof (API_Coord)) < desired + 2)
		Fail ("Internal error: unexpected polygon layout after resizing the outline.", APIERR_GENERAL);

	// 3. outline coordinates
	for (Int32 i = 0; i < desired; ++i)
		(*memo.coords)[i + 1] = data.outline.points[i];
	(*memo.coords)[desired + 1] = data.outline.points[0];

	// 4. outline arcs (exact size; hole arcs are appended by InsertSubPoly)
	KillHandleOf (memo.parcs);
	const Int32 nArcs = (Int32) data.outline.arcs.GetSize ();
	memo.parcs = NewHandle<API_PolyArc> (nArcs);
	for (Int32 a = 0; a < nArcs; ++a) {
		(*memo.parcs)[a].begIndex = 1 + data.outline.arcs[a].first;
		(*memo.parcs)[a].endIndex = 2 + data.outline.arcs[a].first;
		(*memo.parcs)[a].arcAngle = data.outline.arcs[a].second;
	}

	// 5. holes
	for (const Contour& hole : data.holes) {
		const Int32 n = (Int32) hole.points.GetSize ();
		Memo ins;
		ins->coords = NewHandle<API_Coord> (n + 2);
		for (Int32 i = 0; i < n; ++i)
			(*ins->coords)[i + 1] = hole.points[i];
		(*ins->coords)[n + 1] = hole.points[0];
		ins->pends = NewHandle<Int32> (2);
		(*ins->pends)[0] = 0;
		(*ins->pends)[1] = n + 1;
		if (!hole.arcs.IsEmpty ()) {
			ins->parcs = NewHandle<API_PolyArc> ((Int32) hole.arcs.GetSize ());
			for (UIndex a = 0; a < hole.arcs.GetSize (); ++a) {
				(*ins->parcs)[a].begIndex = 1 + hole.arcs[a].first;
				(*ins->parcs)[a].endIndex = 2 + hole.arcs[a].first;
				(*ins->parcs)[a].arcAngle = hole.arcs[a].second;
			}
		}
		Check (ACAPI_Goodies (APIAny_InsertSubPolyID, &memo, ins.Ptr ()), "Cannot add a hole to the polygon");
	}
}


bool RegularizeMemoPolygon (API_ElementMemo& memo, API_Polygon& poly, const char* elementName)
{
	if (memo.coords == nullptr || memo.pends == nullptr)
		return false;

	API_RegularizedPoly src;
	BNZeroMemory (&src, sizeof (src));
	src.coords = memo.coords;
	src.pends = memo.pends;
	src.parcs = memo.parcs;
	src.vertexIDs = memo.vertexIDs;
	src.needVertexAncestry = true;
	src.needEdgeAncestry = true;

	RegularizedPolys result;
	GSErrCode err = ACAPI_Goodies (APIAny_RegularizePolygonID, &src, &result.count, &result.polys);
	if (err != NoError || result.polys == nullptr)
		return false;
	if (result.count < 1)
		Fail (GS::UniString (elementName) + " polygon is degenerate (zero area after removing self-intersections / duplicate points). Check the points.", APIERR_IRREGULARPOLY);
	if (result.count > 1) {
		GS::UniString msg;
		msg = GS::UniString::Printf (" polygon is self-intersecting: it splits into %d separate regular polygons. Pass each part as its own element (or fix the point order).", (int) result.count);
		Fail (GS::UniString (elementName) + msg, APIERR_IRREGULARPOLY);
	}

	const API_RegularizedPoly& r = (*result.polys)[0];
	const Int32 oldCoords = HandleCount (reinterpret_cast<GSConstHandle> (memo.coords), sizeof (API_Coord)) - 1;
	const API_Polygon counts = PolygonCounts (r.coords, r.pends, r.parcs);
	const Int32 n = counts.nCoords;
	if (n < 4)
		return false;

	auto ancestor = [&] (Int32** anc, Int32 j) -> Int32 {
		if (anc == nullptr || j >= HandleCount (reinterpret_cast<GSConstHandle> (anc), sizeof (Int32)))
			return 0;
		const Int32 a = (*anc)[j];
		return (a >= 1 && a <= oldCoords) ? a : 0;
	};

	// Vertex data (mesh heights)
	double** newZ = nullptr;
	if (memo.meshPolyZ != nullptr) {
		newZ = NewHandle<double> (n + 1);
		const Int32 oldZ = HandleCount (reinterpret_cast<GSConstHandle> (memo.meshPolyZ), sizeof (double));
		for (Int32 j = 1; j <= n; ++j) {
			const Int32 a = ancestor (r.vertexAncestry, j);
			(*newZ)[j] = (a > 0 && a < oldZ) ? (*memo.meshPolyZ)[a] : (j > 1 ? (*newZ)[j - 1] : 0.0);
		}
	}
	// Edge data (slab / roof edges)
	API_EdgeTrim** newTrims = nullptr;
	API_OverriddenAttribute* newSides = nullptr;
	API_RoofEdgeTypeID* newTypes = nullptr;
	const Int32 oldTrims = HandleCount (reinterpret_cast<GSConstHandle> (memo.edgeTrims), sizeof (API_EdgeTrim));
	const Int32 oldSides = PtrCount (reinterpret_cast<GSConstPtr> (memo.sideMaterials), sizeof (API_OverriddenAttribute));
	const Int32 oldTypes = PtrCount (reinterpret_cast<GSConstPtr> (memo.roofEdgeTypes), sizeof (API_RoofEdgeTypeID));
	if (memo.edgeTrims != nullptr)
		newTrims = NewHandle<API_EdgeTrim> (n + 1);
	if (memo.sideMaterials != nullptr)
		newSides = NewPtr<API_OverriddenAttribute> (n + 1);
	if (memo.roofEdgeTypes != nullptr)
		newTypes = NewPtr<API_RoofEdgeTypeID> (n + 1);
	for (Int32 j = 1; j <= n; ++j) {
		const Int32 a = ancestor (r.edgeAncestry, j);
		const Int32 src1 = a > 0 ? a : 1;
		if (newTrims != nullptr && src1 < oldTrims)	(*newTrims)[j] = (*memo.edgeTrims)[src1];
		if (newSides != nullptr && src1 < oldSides)	newSides[j] = memo.sideMaterials[src1];
		if (newTypes != nullptr && a > 0 && a < oldTypes) newTypes[j] = memo.roofEdgeTypes[a];
	}

	// Replace the memo polygon with copies of the regularized handles.
	API_Coord** coords = CopyHandle (r.coords);
	Int32** pends = CopyHandle (r.pends);
	API_PolyArc** parcs = r.parcs != nullptr ? CopyHandle (r.parcs) : NewHandle<API_PolyArc> (0);
	UInt32** vertexIDs = CopyHandle (r.vertexIDs);

	KillHandleOf (memo.coords);		memo.coords = coords;
	KillHandleOf (memo.pends);		memo.pends = pends;
	KillHandleOf (memo.parcs);		memo.parcs = parcs;
	KillHandleOf (memo.vertexIDs);	memo.vertexIDs = vertexIDs;
	KillHandleOf (memo.edgeIDs);
	KillHandleOf (memo.contourIDs);
	if (newZ != nullptr)		{ KillHandleOf (memo.meshPolyZ);	memo.meshPolyZ = newZ; }
	if (newTrims != nullptr)	{ KillHandleOf (memo.edgeTrims);	memo.edgeTrims = newTrims; }
	if (newSides != nullptr)	{ KillPtrOf (memo.sideMaterials);	memo.sideMaterials = newSides; }
	if (newTypes != nullptr)	{ KillPtrOf (memo.roofEdgeTypes);	memo.roofEdgeTypes = newTypes; }
	SyncClosingEdgeData (memo);

	poly.nCoords = counts.nCoords;
	poly.nSubPolys = counts.nSubPolys;
	poly.nArcs = counts.nArcs;
	return true;
}


void DisposePivotEdges (API_ElementMemo& memo)
{
	if (memo.pivotPolyEdges == nullptr)
		return;
	API_ElementMemo scratch;
	BNZeroMemory (&scratch, sizeof (scratch));
	scratch.pivotPolyEdges = memo.pivotPolyEdges;
	memo.pivotPolyEdges = nullptr;
	ACAPI_DisposeElemMemoHdls (&scratch);
}

// --- Edge data ---------------------------------------------------------------------------------

void AllocEdgeData (API_ElementMemo& memo, Int32 nCoords, const API_EdgeTrim& trim, const API_OverriddenAttribute& sideMat)
{
	KillHandleOf (memo.edgeTrims);
	KillPtrOf (memo.sideMaterials);
	memo.edgeTrims = NewHandle<API_EdgeTrim> (nCoords + 1);
	memo.sideMaterials = NewPtr<API_OverriddenAttribute> (nCoords + 1);
	for (Int32 i = 1; i <= nCoords; ++i) {
		(*memo.edgeTrims)[i] = trim;
		memo.sideMaterials[i] = sideMat;
	}
}


void EnsureRoofEdgeTypes (API_ElementMemo& memo, Int32 nCoords)
{
	if (memo.roofEdgeTypes != nullptr &&
		PtrCount (reinterpret_cast<GSConstPtr> (memo.roofEdgeTypes), sizeof (API_RoofEdgeTypeID)) >= nCoords + 1)
		return;
	KillPtrOf (memo.roofEdgeTypes);
	memo.roofEdgeTypes = NewPtr<API_RoofEdgeTypeID> (nCoords + 1);		// zero = APIRoofEdgeType_Undefined
}


bool HasEdgeData (const API_ElementMemo& memo, Int32 nCoords)
{
	return memo.edgeTrims != nullptr && memo.sideMaterials != nullptr &&
		HandleCount (reinterpret_cast<GSConstHandle> (memo.edgeTrims), sizeof (API_EdgeTrim)) >= nCoords + 1 &&
		PtrCount (reinterpret_cast<GSConstPtr> (memo.sideMaterials), sizeof (API_OverriddenAttribute)) >= nCoords + 1;
}


void SyncClosingEdgeData (API_ElementMemo& memo)
{
	if (memo.pends == nullptr)
		return;
	const Int32 nTrims = HandleCount (reinterpret_cast<GSConstHandle> (memo.edgeTrims), sizeof (API_EdgeTrim));
	const Int32 nSides = PtrCount (reinterpret_cast<GSConstPtr> (memo.sideMaterials), sizeof (API_OverriddenAttribute));
	const Int32 nTypes = PtrCount (reinterpret_cast<GSConstPtr> (memo.roofEdgeTypes), sizeof (API_RoofEdgeTypeID));
	for (const ContourRange& c : ContoursOf (memo.pends)) {
		const Int32 closing = c.first + c.count;
		if (closing < nTrims)	(*memo.edgeTrims)[closing] = (*memo.edgeTrims)[c.first];
		if (closing < nSides)	memo.sideMaterials[closing] = memo.sideMaterials[c.first];
		if (closing < nTypes)	memo.roofEdgeTypes[closing] = memo.roofEdgeTypes[c.first];
	}
}


bool ApplyEdgeOverrides (const OS& spec, API_ElementMemo& memo, const GS::Array<bool>* reversed, bool allowEdgeTypes)
{
	GS::Array<OS> items = GetObjectArray (spec, "edges", false);
	if (items.IsEmpty ())
		return false;
	const GS::Array<ContourRange> contours = ContoursOf (memo.pends);
	if (contours.IsEmpty ())
		Fail ("The element has no polygon, 'edges' cannot be applied.", APIERR_BADPOLY);
	const Int32 nCoords = contours[contours.GetSize () - 1].first + contours[contours.GetSize () - 1].count;
	if (!HasEdgeData (memo, nCoords))
		Fail ("Internal error: edge data is not allocated.", APIERR_GENERAL);

	for (UIndex k = 0; k < items.GetSize (); ++k) {
		const OS& e = items[k];
		GS::UniString where;
		where = GS::UniString::Printf ("edges[%u]", (unsigned) k);
		const Int32 c = GetInt (e, "contour", 0);
		if (c < 0 || c >= (Int32) contours.GetSize ()) {
			GS::UniString msg;
			msg = GS::UniString::Printf (".contour %d is out of range: 0 = outline, 1..%d = holes.", (int) c, (int) contours.GetSize () - 1);
			Fail (where + msg);
		}
		const ContourRange& range = contours[c];
		const Int32 idx = GetInt (e, "index");
		if (idx < 0 || idx >= range.count) {
			GS::UniString msg;
			msg = GS::UniString::Printf (".index %d is out of range: contour %d has %d edges (0..%d; edge i runs from point i to point i+1).",
						(int) idx, (int) c, (int) range.count, (int) range.count - 1);
			Fail (where + msg);
		}
		const bool rev = reversed != nullptr && c < (Int32) reversed->GetSize () && (*reversed)[c];
		const Int32 j = range.first + MapEdgeIndex (idx, range.count, rev);

		ApplyEdgeTrim (e, (*memo.edgeTrims)[j], "trim", "angle");
		if (e.Contains ("surface"))
			ApplyOverriddenSurface (e, "surface", memo.sideMaterials[j]);
		if (e.Contains ("edgeType")) {
			if (!allowEdgeTypes)
				Fail (where + ".edgeType is only supported for single-plane roof edges.");
			EnsureRoofEdgeTypes (memo, nCoords);
			memo.roofEdgeTypes[j] = (API_RoofEdgeTypeID) ParseNamed (kEdgeTypes, e, "edgeType");
		}
	}
	SyncClosingEdgeData (memo);
	return true;
}


void AddEdgesJson (OS& out, const API_ElementMemo& memo, const API_EdgeTrim& defTrim,
				   const API_OverriddenAttribute& defSide, bool withEdgeTypes)
{
	if (memo.pends == nullptr)
		return;
	const Int32 nTrims = HandleCount (reinterpret_cast<GSConstHandle> (memo.edgeTrims), sizeof (API_EdgeTrim));
	const Int32 nSides = PtrCount (reinterpret_cast<GSConstPtr> (memo.sideMaterials), sizeof (API_OverriddenAttribute));
	const Int32 nTypes = withEdgeTypes ? PtrCount (reinterpret_cast<GSConstPtr> (memo.roofEdgeTypes), sizeof (API_RoofEdgeTypeID)) : 0;
	GS::Array<OS> list;
	const GS::Array<ContourRange> contours = ContoursOf (memo.pends);
	for (UIndex c = 0; c < contours.GetSize (); ++c) {
		for (Int32 i = 0; i < contours[c].count; ++i) {
			const Int32 j = contours[c].first + i;
			const bool trimDiff = j < nTrims && !SameTrim ((*memo.edgeTrims)[j], defTrim);
			const bool sideDiff = j < nSides && !SameOverride (memo.sideMaterials[j], defSide);
			const bool typeDiff = j < nTypes && memo.roofEdgeTypes[j] != APIRoofEdgeType_Undefined;
			if (!trimDiff && !sideDiff && !typeDiff)
				continue;
			OS e ("contour", (Int32) c, "index", i);
			if (j < nTrims)
				AddEdgeTrimJson (e, (*memo.edgeTrims)[j], "trim", "angle");
			if (sideDiff) {
				if (memo.sideMaterials[j].overridden)
					e.Add ("surface", AttrRef (API_MaterialID, memo.sideMaterials[j].attributeIndex));
				else
					e.Add ("surface", false);
			}
			if (typeDiff)
				e.Add ("edgeType", NameOf (kEdgeTypes, memo.roofEdgeTypes[j]));
			list.Push (e);
		}
	}
	if (!list.IsEmpty ())
		out.Add ("edges", list);
}

// --- Shell base ----------------------------------------------------------------------------------

void ApplyShellBaseFields (API_ShellBaseType& base, const FieldMask& m, const OS& spec)
{
	if (ApplyLength (spec, "level", base.level))					m.Set (&base.level);
	if (ApplyLength (spec, "thickness", base.thickness, true))		m.Set (&base.thickness);
	// Structure (not via cc::ApplyStructure: for shells "profile" is the profile polyline, not a complex profile).
	bool structureChanged = false;
	if (auto bm = OptAttr (API_BuildingMaterialID, spec, "buildingMaterial")) {
		base.modelElemStructureType = API_BasicStructure;
		base.buildingMaterial = *bm;
		structureChanged = true;
	} else if (auto comp = OptAttr (API_CompWallID, spec, "composite")) {
		base.modelElemStructureType = API_CompositeStructure;
		base.composite = *comp;
		structureChanged = true;
	}
	if (structureChanged) {
		m.Set (&base.modelElemStructureType);
		m.Set (&base.buildingMaterial);
		m.Set (&base.composite);
	}
	if (ApplyOverriddenSurface (spec, "topSurface", base.topMat))		m.Set (&base.topMat);
	if (ApplyOverriddenSurface (spec, "bottomSurface", base.botMat))	m.Set (&base.botMat);
	if (ApplyOverriddenSurface (spec, "sideSurface", base.sidMat))		m.Set (&base.sidMat);
	if (ApplyFlag (spec, "surfacesChained", base.materialsChained))		m.Set (&base.materialsChained);
	if (ApplyEdgeTrim (spec, base.edgeTrim))							m.Set (&base.edgeTrim);
	if (Has (spec, "connectionBody")) {
		base.cutBodyType = (API_ShellBaseCutBodyTypeID) ParseNamed (kConnectionBodies, spec, "connectionBody");
		m.Set (&base.cutBodyType);
	}
	if (Has (spec, "floorPlanDisplay")) {
		base.displayOption = (API_ElemDisplayOptionsID) ParseNamed (kDisplayOptions, spec, "floorPlanDisplay");
		m.Set (&base.displayOption);
	}
	if (Has (spec, "viewDepth")) {
		base.viewDepthLimitation = (API_ElemViewDepthLimitationsID) ParseNamed (kViewDepths, spec, "viewDepth");
		m.Set (&base.viewDepthLimitation);
	}
	if (ApplyPen (spec, "contourPen", base.pen))								m.Set (&base.pen);
	if (ApplyAttr (spec, "contourLineType", API_LinetypeID, base.ltypeInd))	m.Set (&base.ltypeInd);
	if (ApplyPen (spec, "cutContourPen", base.sectContPen))					m.Set (&base.sectContPen);
	if (ApplyAttr (spec, "cutContourLineType", API_LinetypeID, base.sectContLtype))	m.Set (&base.sectContLtype);
	if (ApplyPenOverride (spec, base.penOverride))							m.Set (&base.penOverride);
	if (ApplyFlag (spec, "showCoverFill", base.useFloorFill))				m.Set (&base.useFloorFill);
	if (ApplyAttr (spec, "coverFill", API_FilltypeID, base.floorFillInd))	m.Set (&base.floorFillInd);
	if (ApplyPen (spec, "coverFillPen", base.floorFillPen))					m.Set (&base.floorFillPen);
	if (ApplyPen (spec, "coverFillBackgroundPen", base.floorFillBGPen))		m.Set (&base.floorFillBGPen);
	if (ApplyFlag (spec, "coverFillFromSurface", base.use3DHatching))		m.Set (&base.use3DHatching);
	if (ApplyFlag (spec, "coverFillAlignedToPivot", base.useFillLocBaseLine))	m.Set (&base.useFillLocBaseLine);
	if (ApplyFlag (spec, "coverFillDistorted", base.useSlantedFill))		m.Set (&base.useSlantedFill);
	if (ApplyPen (spec, "overheadLinePen", base.aboveViewLinePen))			m.Set (&base.aboveViewLinePen);
	if (ApplyAttr (spec, "overheadLineType", API_LinetypeID, base.aboveViewLineType))	m.Set (&base.aboveViewLineType);
	if (ApplyStoryVisibility (spec, base.visibilityCont, base.visibilityFill)) {
		m.Set (&base.visibilityCont);
		m.Set (&base.visibilityFill);
		if (!Has (spec, "autoStoryVisibility")) {
			base.isAutoOnStoryVisibility = false;
			m.Set (&base.isAutoOnStoryVisibility);
		}
	}
	if (ApplyFlag (spec, "autoStoryVisibility", base.isAutoOnStoryVisibility))	m.Set (&base.isAutoOnStoryVisibility);
}


void AddShellBaseJson (OS& out, const API_ShellBaseType& base)
{
	out.Add ("level", base.level);
	out.Add ("thickness", base.thickness);
	AddStructureJson (out, base.modelElemStructureType, base.buildingMaterial, base.composite, 0);
	AddOverriddenSurfaceJson (out, "topSurface", base.topMat);
	AddOverriddenSurfaceJson (out, "bottomSurface", base.botMat);
	AddOverriddenSurfaceJson (out, "sideSurface", base.sidMat);
	out.Add ("surfacesChained", base.materialsChained);
	AddEdgeTrimJson (out, base.edgeTrim);
	out.Add ("connectionBody", NameOf (kConnectionBodies, base.cutBodyType));
	out.Add ("floorPlanDisplay", NameOf (kDisplayOptions, base.displayOption));
	out.Add ("viewDepth", NameOf (kViewDepths, base.viewDepthLimitation));
	out.Add ("contourPen", (Int32) base.pen);
	out.Add ("contourLineType", AttrRef (API_LinetypeID, base.ltypeInd));
	out.Add ("cutContourPen", (Int32) base.sectContPen);
	out.Add ("cutContourLineType", AttrRef (API_LinetypeID, base.sectContLtype));
	AddPenOverrideJson (out, base.penOverride);
	out.Add ("showCoverFill", base.useFloorFill);
	out.Add ("coverFill", AttrRef (API_FilltypeID, base.floorFillInd));
	out.Add ("coverFillPen", (Int32) base.floorFillPen);
	out.Add ("coverFillBackgroundPen", (Int32) base.floorFillBGPen);
	out.Add ("coverFillFromSurface", base.use3DHatching);
	out.Add ("coverFillAlignedToPivot", base.useFillLocBaseLine);
	out.Add ("coverFillDistorted", base.useSlantedFill);
	out.Add ("overheadLinePen", (Int32) base.aboveViewLinePen);
	out.Add ("overheadLineType", AttrRef (API_LinetypeID, base.aboveViewLineType));
	out.Add ("autoStoryVisibility", base.isAutoOnStoryVisibility);
	out.Add ("storyVisibility", StoryVisibilityJson (base.visibilityCont));
}

// --- Frames ------------------------------------------------------------------------------------

API_Tranmat IdentityTranmat ()
{
	API_Tranmat t;
	BNZeroMemory (&t, sizeof (t));
	t.tmx[0] = t.tmx[5] = t.tmx[10] = 1.0;
	return t;
}


static API_Tranmat TranmatFromAxes (const API_Coord3D& origin, const API_Coord3D& x, const API_Coord3D& y, const API_Coord3D& z)
{
	API_Tranmat t;
	BNZeroMemory (&t, sizeof (t));
	t.tmx[0] = x.x;	t.tmx[1] = y.x;	t.tmx[2]  = z.x;	t.tmx[3]  = origin.x;
	t.tmx[4] = x.y;	t.tmx[5] = y.y;	t.tmx[6]  = z.y;	t.tmx[7]  = origin.y;
	t.tmx[8] = x.z;	t.tmx[9] = y.z;	t.tmx[10] = z.z;	t.tmx[11] = origin.z;
	return t;
}


API_Coord3D Point3DFrom (const OS& obj, const char* key, double defZ)
{
	OS p = GetObject (obj, key);
	API_Coord3D c;
	c.x = GetDouble (p, "x");
	c.y = GetDouble (p, "y");
	c.z = GetDouble (p, "z", defZ);
	return c;
}


std::optional<API_Coord3D> OptPoint3D (const OS& spec, const char* key, double defZ)
{
	if (!spec.Contains (key))
		return std::nullopt;
	return Point3DFrom (spec, key, defZ);
}


API_Tranmat FrameFrom (const OS& spec, const char* key)
{
	OS f = GetObject (spec, key);
	const GS::UniString name (key);
	if (f.Contains ("matrix")) {
		GS::Array<double> m = GetNumberArray (f, "matrix", true);
		if (m.GetSize () != 12)
			Fail (name + ".matrix must have exactly 12 numbers (row-major 3x4, translation at indices 3, 7, 11).");
		API_Tranmat t;
		for (UIndex i = 0; i < 12; ++i)
			t.tmx[i] = m[i];
		return t;
	}
	const API_Coord3D origin = f.Contains ("origin") ? Point3DFrom (f, "origin", 0.0) : API_Coord3D { 0.0, 0.0, 0.0 };
	const API_Coord3D x = Normalized (f.Contains ("xAxis") ? Point3DFrom (f, "xAxis", 0.0) : API_Coord3D { 1.0, 0.0, 0.0 }, name + ".xAxis");
	const API_Coord3D yIn = f.Contains ("yAxis") ? Point3DFrom (f, "yAxis", 0.0) : API_Coord3D { 0.0, 1.0, 0.0 };
	const API_Coord3D zRaw = Cross (x, yIn);
	if (std::sqrt (zRaw.x * zRaw.x + zRaw.y * zRaw.y + zRaw.z * zRaw.z) < 1e-9)
		Fail (name + ": xAxis and yAxis must not be parallel.");
	const API_Coord3D z = Normalized (zRaw, name + ".zAxis");
	const API_Coord3D y = Cross (z, x);
	return TranmatFromAxes (origin, x, y, z);
}


OS FrameJson (const API_Tranmat& tm)
{
	const double* t = tm.tmx;
	return OS ("origin", Coord3DObj (t[3], t[7], t[11]),
			   "xAxis", Coord3DObj (t[0], t[4], t[8]),
			   "yAxis", Coord3DObj (t[1], t[5], t[9]),
			   "zAxis", Coord3DObj (t[2], t[6], t[10]));
}


API_Tranmat VerticalFrame (const API_Coord3D& origin, double rotationRadians)
{
	const API_Coord3D x { std::cos (rotationRadians), std::sin (rotationRadians), 0.0 };
	const API_Coord3D y { 0.0, 0.0, 1.0 };
	return TranmatFromAxes (origin, x, y, Cross (x, y));
}

} // namespace slabroof
} // namespace cc
