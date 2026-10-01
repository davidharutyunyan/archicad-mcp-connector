// *****************************************************************************
// ElementQuery — finding and analysing elements (family "element-query").
//
// Commands (namespace ClaudeConnector):
//   FindElements          filtered + paginated element listing
//   GetElementCounts      element counts per type / story / layer / renovation status
//   GetSelection          current selection (elements + marquee)
//   SetSelection          set / add / remove / clear the selection
//   GetElementQuantities  (ElementQueryQuantities.cpp)
//   GetConnectedElements, GetElementRelations, GetSubelements (ElementQueryRelations.cpp)
//   GetElement2DGeometry, GetElement3DGeometry (ElementQueryGeometry.cpp)
//
// Units: meters, square meters, cubic meters, degrees. All commands are read-only
// except SetSelection (selection changes are not undoable and need no undo scope).
// *****************************************************************************

#include "Commands/Commands.hpp"
#include "Commands/ElementQueryShared.hpp"
#include "Core/Command.hpp"
#include "Core/Elements.hpp"
#include "Core/Enums.hpp"
#include "Core/Polygon.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace cc {
namespace eq {

// =============================================================================
// Shared helpers (declared in ElementQueryShared.hpp)
// =============================================================================

namespace {

class ItemCollector : public OS::Processor {
public:
	GS::Array<OS>	items;
	Int32			depth = 0;

	bool Accept () const { return depth <= 1; }

	void BoolFound (const GS::String&, bool v) override						{ if (Accept ()) items.Push (OS ("v", v)); }
	void IntFound (const GS::String&, Int64 v) override						{ if (Accept ()) items.Push (OS ("v", v)); }
	void UIntFound (const GS::String&, UInt64 v) override					{ if (Accept ()) items.Push (OS ("v", v)); }
	void RealFound (const GS::String&, double v) override					{ if (Accept ()) items.Push (OS ("v", v)); }
	void StringFound (const GS::String&, const GS::UniString& v) override	{ if (Accept ()) items.Push (OS ("v", v)); }
	bool ObjectFound (const GS::String&, const OS& v) override				{ if (Accept ()) items.Push (OS ("v", v)); return false; }
	void ListEntered (const GS::String&) override							{ ++depth; }
	void ListExited (const GS::String&) override							{ --depth; }
};

} // namespace


GS::Array<OS> ArrayItems (const OS& os, const char* key)
{
	if (!os.Contains (key))
		return {};
	ItemCollector collector;
	os.Enumerate (key, collector);
	return collector.items;
}


GS::UniString AllElemTypeNames ()
{
	GS::UniString names;
	for (API_ElemTypeID t : AllElemTypes ()) {
		if (!names.IsEmpty ())
			names += ", ";
		names += ElemTypeName (t);
	}
	return names;
}


GS::Array<API_ElemTypeID> GetElemTypeArray (const OS& os, const char* key)
{
	GS::Array<API_ElemTypeID> result;
	for (const OS& item : ArrayItems (os, key)) {
		if (!item.IsString ("v"))
			Fail ("'" + GS::UniString (key) + "' must be an array of element type names such as \"Wall\", \"Slab\", \"Door\".");
		GS::UniString name = GetString (item, "v");
		if (EqualsIgnoreCase (name, "All") || EqualsIgnoreCase (name, "*"))
			continue;
		auto t = ParseElemType (name);
		if (!t.has_value ())
			Fail ("Unknown element type '" + name + "' in '" + GS::UniString (key) + "'. Valid types: " + AllElemTypeNames () + ".");
		if (!result.Contains (*t))
			result.Push (*t);
	}
	return result;
}


GS::Array<API_Guid> OptGuidArray (const OS& os, const char* key)
{
	if (!os.Contains (key))
		return {};
	return GetGuidArray (os, key, false);
}


bool IsSubelementType (API_ElemTypeID typeID)
{
	switch (typeID) {
		case API_CurtainWallSegmentID:
		case API_CurtainWallFrameID:
		case API_CurtainWallPanelID:
		case API_CurtainWallJunctionID:
		case API_CurtainWallAccessoryID:
		case API_RiserID:
		case API_TreadID:
		case API_StairStructureID:
		case API_RailingToprailID:
		case API_RailingHandrailID:
		case API_RailingRailID:
		case API_RailingPostID:
		case API_RailingInnerPostID:
		case API_RailingBalusterID:
		case API_RailingPanelID:
		case API_RailingSegmentID:
		case API_RailingNodeID:
		case API_RailingBalusterSetID:
		case API_RailingPatternID:
		case API_RailingToprailEndID:
		case API_RailingHandrailEndID:
		case API_RailingRailEndID:
		case API_RailingToprailConnectionID:
		case API_RailingHandrailConnectionID:
		case API_RailingRailConnectionID:
		case API_RailingEndFinishID:
		case API_BeamSegmentID:
		case API_ColumnSegmentID:
			return true;
		default:
			return false;
	}
}


bool IsHierarchicalType (API_ElemTypeID typeID)
{
	return typeID == API_CurtainWallID || typeID == API_StairID || typeID == API_RailingID ||
		   typeID == API_BeamID || typeID == API_ColumnID;
}

// --- NameCache ---------------------------------------------------------------------

GS::UniString NameCache::LayerName (API_AttributeIndex index)
{
	if (const GS::UniString* cached = layers.GetPtr ((Int32) index))
		return *cached;
	GS::UniString name = AttrName (API_LayerID, index);
	layers.Put ((Int32) index, name);
	return name;
}


OS NameCache::Layer (API_AttributeIndex index)
{
	return OS ("index", (Int32) index, "name", LayerName (index));
}


GS::UniString NameCache::LibPartName (Int32 libInd)
{
	if (libInd <= 0)
		return GS::UniString ();
	if (const GS::UniString* cached = libParts.GetPtr (libInd))
		return *cached;
	API_LibPart lp;
	BNZeroMemory (&lp, sizeof (lp));
	lp.index = libInd;
	GSErrCode err = ACAPI_LibPart_Get (&lp);
	if (lp.location != nullptr) {
		delete lp.location;
		lp.location = nullptr;
	}
	GS::UniString name = (err == NoError) ? GS::UniString (lp.docu_UName) : GS::UniString ();
	libParts.Put (libInd, name);
	return name;
}


GS::UniString NameCache::StoryNameOf (short floorInd)
{
	if (const GS::UniString* cached = stories.GetPtr ((Int32) floorInd))
		return *cached;
	GS::UniString name = StoryName (floorInd);
	stories.Put ((Int32) floorInd, name);
	return name;
}


OS ElementBrief (const API_Elem_Head& head, NameCache& names, bool withElementId)
{
	OS out;
	out.Add ("guid", GuidStr (head.guid));
	out.Add ("type", ElemTypeName (head.type));
	out.Add ("storyIndex", (Int32) head.floorInd);
	out.Add ("layer", names.Layer (head.layer));
	if (withElementId)
		out.Add ("elementId", GetElementInfoString (head.guid));
	return out;
}


Int32 LibIndOf (const API_Element& element)
{
	switch (element.header.type.typeID) {
		case API_ObjectID:		return element.object.libInd;
		case API_LampID:		return element.lamp.libInd;
		case API_WindowID:		return element.window.openingBase.libInd;
		case API_DoorID:		return element.door.openingBase.libInd;
		case API_SkylightID:	return element.skylight.openingBase.libInd;
		case API_ZoneID:		return element.zone.libInd;
		default:				return 0;
	}
}


API_Guid OwnerOf (const API_Element& e)
{
	switch (e.header.type.typeID) {
		case API_WindowID:						return e.window.owner;
		case API_DoorID:						return e.door.owner;
		case API_SkylightID:					return e.skylight.owner;
		case API_LabelID:						return e.label.parent;
		case API_ObjectID:						return e.object.owner;
		case API_LampID:						return e.lamp.owner;
		case API_CurtainWallSegmentID:			return e.cwSegment.owner;
		case API_CurtainWallFrameID:			return e.cwFrame.owner;
		case API_CurtainWallPanelID:			return e.cwPanel.owner;
		case API_CurtainWallJunctionID:			return e.cwJunction.owner;
		case API_CurtainWallAccessoryID:		return e.cwAccessory.owner;
		case API_RiserID:						return e.stairRiser.owner;
		case API_TreadID:						return e.stairTread.owner;
		case API_StairStructureID:				return e.stairStructure.owner;
		case API_RailingToprailID:				return e.railingToprail.owner;
		case API_RailingHandrailID:				return e.railingHandrail.owner;
		case API_RailingRailID:					return e.railingRail.owner;
		case API_RailingPostID:					return e.railingPost.owner;
		case API_RailingInnerPostID:			return e.railingInnerPost.owner;
		case API_RailingBalusterID:				return e.railingBaluster.owner;
		case API_RailingPanelID:				return e.railingPanel.owner;
		case API_RailingSegmentID:				return e.railingSegment.owner;
		case API_RailingNodeID:					return e.railingNode.owner;
		case API_RailingBalusterSetID:			return e.railingBalusterSet.owner;
		case API_RailingPatternID:				return e.railingPattern.owner;
		case API_RailingToprailEndID:			return e.railingToprailEnd.owner;
		case API_RailingHandrailEndID:			return e.railingHandrailEnd.owner;
		case API_RailingRailEndID:				return e.railingRailEnd.owner;
		case API_RailingToprailConnectionID:	return e.railingToprailConnection.owner;
		case API_RailingHandrailConnectionID:	return e.railingHandrailConnection.owner;
		case API_RailingRailConnectionID:		return e.railingRailConnection.owner;
		case API_RailingEndFinishID:			return e.railingEndFinish.owner;
		case API_BeamSegmentID:					return e.beamSegment.owner;
		case API_ColumnSegmentID:				return e.columnSegment.owner;
		default:								return APINULLGuid;
	}
}

// --- Sub-elements ------------------------------------------------------------------

namespace {

template <typename T>
void CollectArray (const T* arr, API_ElemTypeID typeID, bool withExtra, GS::Array<SubElemInfo>& out,
				   const std::function<void (const T&, OS&)>& extra = nullptr)
{
	if (arr == nullptr)
		return;
	const GSSize n = BMGetPtrSize (reinterpret_cast<GSConstPtr> (arr)) / (GSSize) sizeof (T);
	for (GSSize i = 0; i < n; ++i) {
		SubElemInfo info;
		info.head = arr[i].head;
		info.typeID = typeID;
		info.index = (UInt32) i;
		if (withExtra && extra)
			extra (arr[i], info.extra);
		out.Push (info);
	}
}


void AddVisible (OS& out, bool visible)
{
	if (!visible)
		out.Add ("visible", false);
}

} // namespace


GS::Array<SubElemInfo> CollectSubelements (const API_Guid& guid, API_ElemTypeID typeID, bool withExtra)
{
	GS::Array<SubElemInfo> out;
	UInt64 mask = 0;
	switch (typeID) {
		case API_CurtainWallID:
			mask = APIMemoMask_CWallSegments | APIMemoMask_CWallFrames | APIMemoMask_CWallPanels |
				   APIMemoMask_CWallJunctions | APIMemoMask_CWallAccessories;
			break;
		case API_StairID:
			mask = APIMemoMask_StairRiser | APIMemoMask_StairTread | APIMemoMask_StairStructure;
			break;
		case API_RailingID:
			mask = APIMemoMask_RailingNode | APIMemoMask_RailingSegment | APIMemoMask_RailingPost |
				   APIMemoMask_RailingInnerPost | APIMemoMask_RailingRail | APIMemoMask_RailingHandrail |
				   APIMemoMask_RailingToprail | APIMemoMask_RailingPanel | APIMemoMask_RailingBaluster |
				   APIMemoMask_RailingPattern | APIMemoMask_RailingBalusterSet | APIMemoMask_RailingRailEnd |
				   APIMemoMask_RailingHandrailEnd | APIMemoMask_RailingToprailEnd | APIMemoMask_RailingRailConnection |
				   APIMemoMask_RailingHandrailConnection | APIMemoMask_RailingToprailConnection;
			break;
		case API_BeamID:
			mask = APIMemoMask_BeamSegment;
			break;
		case API_ColumnID:
			mask = APIMemoMask_ColumnSegment;
			break;
		default:
			return out;
	}

	Memo memo;
	LoadMemo (guid, *memo, mask);
	const API_ElementMemo& m = *memo;

	switch (typeID) {
		case API_CurtainWallID:
			CollectArray<API_CWSegmentType> (m.cWallSegments, API_CurtainWallSegmentID, withExtra, out,
				[] (const API_CWSegmentType& s, OS& x) {
					x.Add ("begin", Coord3DObj (s.begC));
					x.Add ("end", Coord3DObj (s.endC));
				});
			CollectArray<API_CWFrameType> (m.cWallFrames, API_CurtainWallFrameID, withExtra, out,
				[] (const API_CWFrameType& f, OS& x) {
					x.Add ("className", GS::UniString (f.className));
					x.Add ("begin", Coord3DObj (f.begC));
					x.Add ("end", Coord3DObj (f.endC));
					if (f.contourID >= 0)
						x.Add ("isContourFrame", true);
				});
			CollectArray<API_CWPanelType> (m.cWallPanels, API_CurtainWallPanelID, withExtra, out,
				[] (const API_CWPanelType& p, OS& x) {
					x.Add ("className", GS::UniString (p.className));
					x.Add ("centroid", Coord3DObj (p.centroid));
					x.Add ("thickness", p.thickness);
					if (p.hidden)
						x.Add ("hidden", true);
					bool degenerate = false;
					API_Guid panelGuid = p.head.guid;
					if (ACAPI_Database (APIDb_IsCWPanelDegenerateID, &panelGuid, &degenerate) == NoError && degenerate)
						x.Add ("degenerate", true);
				});
			CollectArray<API_CWJunctionType> (m.cWallJunctions, API_CurtainWallJunctionID, withExtra, out);
			CollectArray<API_CWAccessoryType> (m.cWallAccessories, API_CurtainWallAccessoryID, withExtra, out);
			break;

		case API_StairID:
			CollectArray<API_StairRiserType> (m.stairRisers, API_RiserID, withExtra, out,
				[] (const API_StairRiserType& r, OS& x) {
					x.Add ("sequenceNumber", (Int32) r.sequenceNumber);
					AddVisible (x, r.visible);
				});
			CollectArray<API_StairTreadType> (m.stairTreads, API_TreadID, withExtra, out,
				[] (const API_StairTreadType& t, OS& x) {
					x.Add ("sequenceNumber", (Int32) t.sequenceNumber);
					AddVisible (x, t.visible);
				});
			CollectArray<API_StairStructureType> (m.stairStructures, API_StairStructureID, withExtra, out,
				[] (const API_StairStructureType& s, OS& x) {
					x.Add ("sequenceNumber", (Int32) s.sequenceNumber);
					if (s.isLanding)
						x.Add ("isLanding", true);
					AddVisible (x, s.visible);
				});
			break;

		case API_RailingID:
			CollectArray<API_RailingSegmentType> (m.railingSegments, API_RailingSegmentID, withExtra, out,
				[] (const API_RailingSegmentType& s, OS& x) { x.Add ("height", s.height); AddVisible (x, s.visible); });
			CollectArray<API_RailingNodeType> (m.railingNodes, API_RailingNodeID, withExtra, out,
				[] (const API_RailingNodeType& n, OS& x) { AddVisible (x, n.visible); });
			CollectArray<API_RailingPostType> (m.railingPosts, API_RailingPostID, withExtra, out);
			CollectArray<API_RailingInnerPostType> (m.railingInnerPosts, API_RailingInnerPostID, withExtra, out);
			CollectArray<API_RailingToprailType> (m.railingToprails, API_RailingToprailID, withExtra, out,
				[] (const API_RailingToprailType& t, OS& x) { AddVisible (x, t.visible); });
			CollectArray<API_RailingHandrailType> (m.railingHandrails, API_RailingHandrailID, withExtra, out,
				[] (const API_RailingHandrailType& h, OS& x) { x.Add ("height", h.height); AddVisible (x, h.visible); });
			CollectArray<API_RailingRailType> (m.railingRails, API_RailingRailID, withExtra, out,
				[] (const API_RailingRailType& r, OS& x) { x.Add ("height", r.height); AddVisible (x, r.visible); });
			CollectArray<API_RailingPanelType> (m.railingPanels, API_RailingPanelID, withExtra, out,
				[] (const API_RailingPanelType& p, OS& x) { AddVisible (x, p.visible); });
			CollectArray<API_RailingBalusterSetType> (m.railingBalusterSets, API_RailingBalusterSetID, withExtra, out,
				[] (const API_RailingBalusterSetType& b, OS& x) { x.Add ("balusterCount", (Int32) b.nBalusters); });
			CollectArray<API_RailingBalusterType> (m.railingBalusters, API_RailingBalusterID, withExtra, out);
			CollectArray<API_RailingPatternType> (m.railingPatterns, API_RailingPatternID, withExtra, out);
			CollectArray<API_RailingRailEndType> (m.railingToprailEnds, API_RailingToprailEndID, withExtra, out);
			CollectArray<API_RailingRailEndType> (m.railingHandrailEnds, API_RailingHandrailEndID, withExtra, out);
			CollectArray<API_RailingRailEndType> (m.railingRailEnds, API_RailingRailEndID, withExtra, out);
			CollectArray<API_RailingRailConnectionType> (m.railingToprailConnections, API_RailingToprailConnectionID, withExtra, out);
			CollectArray<API_RailingRailConnectionType> (m.railingHandrailConnections, API_RailingHandrailConnectionID, withExtra, out);
			CollectArray<API_RailingRailConnectionType> (m.railingRailConnections, API_RailingRailConnectionID, withExtra, out);
			break;

		case API_BeamID:
			CollectArray<API_BeamSegmentType> (m.beamSegments, API_BeamSegmentID, withExtra, out);
			break;

		case API_ColumnID:
			CollectArray<API_ColumnSegmentType> (m.columnSegments, API_ColumnSegmentID, withExtra, out);
			break;

		default:
			break;
	}
	return out;
}

// --- Polygons in handles -------------------------------------------------------------

OS HandlesPolygonToJson (API_Coord** coords, Int32** pends, API_PolyArc** parcs, const API_Polygon* poly)
{
	OS empty ("points", GS::Array<OS> ());
	if (coords == nullptr || pends == nullptr || *coords == nullptr || *pends == nullptr)
		return empty;

	Int32 nCoords = (Int32) (BMGetHandleSize (reinterpret_cast<GSConstHandle> (coords)) / (GSSize) sizeof (API_Coord)) - 1;
	Int32 nSubPolys = (Int32) (BMGetHandleSize (reinterpret_cast<GSConstHandle> (pends)) / (GSSize) sizeof (Int32)) - 1;
	Int32 nArcs = (parcs != nullptr && *parcs != nullptr)
		? (Int32) (BMGetHandleSize (reinterpret_cast<GSConstHandle> (parcs)) / (GSSize) sizeof (API_PolyArc)) : 0;
	if (poly != nullptr) {
		if (poly->nCoords > 0) nCoords = std::min (nCoords, poly->nCoords);
		if (poly->nSubPolys > 0) nSubPolys = std::min (nSubPolys, poly->nSubPolys);
		nArcs = std::min (nArcs, std::max (poly->nArcs, (Int32) 0));
	}
	if (nCoords < 3 || nSubPolys < 1)
		return empty;

	// Validate contour ends: strictly increasing, within the coordinate range.
	Int32 prev = 0;
	for (Int32 k = 1; k <= nSubPolys; ++k) {
		const Int32 end = (*pends)[k];
		if (end <= prev || end > nCoords)
			return empty;
		prev = end;
	}

	API_Polygon p;
	BNZeroMemory (&p, sizeof (p));
	p.nCoords = nCoords;
	p.nSubPolys = nSubPolys;
	p.nArcs = nArcs;

	API_ElementMemo memo;
	BNZeroMemory (&memo, sizeof (memo));
	memo.coords = coords;
	memo.pends = pends;
	memo.parcs = nArcs > 0 ? parcs : nullptr;
	OS out = PolygonToJson (p, memo);
	// memo only borrows the handles — nothing to dispose here.
	return out;
}

// =============================================================================
// Element query (FindElements / GetElementCounts)
// =============================================================================

namespace {

const NamedValue kFilterFlags[] = {
	{ "OnVisibleLayer",			APIFilt_OnVisLayer },
	{ "Editable",				APIFilt_IsEditable },
	{ "OnActiveStory",			APIFilt_OnActFloor },
	{ "In3D",					APIFilt_In3D },
	{ "InMyWorkspace",			APIFilt_InMyWorkspace },
	{ "Independent",			APIFilt_IsIndependent },
	{ "OnActiveLayout",			APIFilt_OnActLayout },
	{ "InCroppedView",			APIFilt_InCroppedView },
	{ "HasAccessRight",			APIFilt_HasAccessRight },
	{ "VisibleByRenovation",	APIFilt_IsVisibleByRenovation },
	{ "Overridden",				APIFilt_IsOverridden },
	{ "InStructureDisplay",		APIFilt_IsInStructureDisplay },
	{ "FromFloorPlan",			APIFilt_FromFloorplan },
};

const NamedValue kRenovation[] = {
	{ "Existing",	API_ExistingStatus },
	{ "New",		API_NewStatus },
	{ "Demolished",	API_DemolishedStatus },
	{ "Default",	API_DefaultStatus },
};


GS::UniString Lower (const GS::UniString& s)
{
	return s.ToLowerCase ();
}


// Case-sensitive glob match on already lower-cased strings: '*' = any run, '?' = one char.
bool WildcardMatch (const GS::UniString& pattern, const GS::UniString& text)
{
	const USize pn = pattern.GetLength ();
	const USize tn = text.GetLength ();
	USize p = 0, t = 0, mark = 0;
	bool haveStar = false;
	USize star = 0;
	const GS::UniChar starCh ('*');
	const GS::UniChar qCh ('?');
	while (t < tn) {
		if (p < pn && (pattern[p] == qCh || pattern[p] == text[t])) {
			++p; ++t;
		} else if (p < pn && pattern[p] == starCh) {
			haveStar = true; star = p++; mark = t;
		} else if (haveStar) {
			p = star + 1; t = ++mark;
		} else {
			return false;
		}
	}
	while (p < pn && pattern[p] == starCh)
		++p;
	return p == pn;
}


struct Region {
	std::optional<double>	xMin, yMin, xMax, yMax, zMin, zMax;
	bool					inside = false;

	bool Matches (const API_Box3D& b) const
	{
		if (inside) {
			if (xMin && b.xMin < *xMin) return false;
			if (yMin && b.yMin < *yMin) return false;
			if (zMin && b.zMin < *zMin) return false;
			if (xMax && b.xMax > *xMax) return false;
			if (yMax && b.yMax > *yMax) return false;
			if (zMax && b.zMax > *zMax) return false;
			return true;
		}
		if (xMin && b.xMax < *xMin) return false;
		if (yMin && b.yMax < *yMin) return false;
		if (zMin && b.zMax < *zMin) return false;
		if (xMax && b.xMin > *xMax) return false;
		if (yMax && b.yMin > *yMax) return false;
		if (zMax && b.zMin > *zMax) return false;
		return true;
	}
};


struct QueryFilter {
	GS::Array<API_ElemTypeID>			types;				// explicit types (empty = all)
	GS::HashSet<Int32>					typeSet;
	GS::HashSet<Int32>					excludeTypes;
	bool								includeSubelements = false;
	API_ElemFilterFlags					flags = APIFilt_None;
	bool								selectedOnly = false;
	std::optional<GS::Array<API_Guid>>	within;
	std::optional<GS::HashSet<Int32>>	stories;
	std::optional<GS::HashSet<Int32>>	layers;
	std::optional<GS::HashSet<Int32>>	renovation;
	std::optional<GS::UniString>		elementIdPattern;	// lower-cased
	std::optional<GS::UniString>		libraryPart;		// lower-cased substring
	std::optional<GS::HashSet<API_Guid>> groupMembers;
	std::optional<bool>					grouped;
	std::optional<API_Guid>				hotlinkGuid;
	std::optional<bool>					inHotlink;
	std::optional<bool>					locked;
	std::optional<Region>				region;
	bool								needElement = false;	// libraryPart filter / output needs ACAPI_Element_Get
};


// OptStory with a readable message for out-of-range indices (the Core message can come back empty).
std::optional<short> OptStoryChecked (const OS& os, const char* key)
{
	try {
		return OptStory (os, key);
	} catch (const Error& e) {
		if (!e.message.IsEmpty ())
			throw;
		GS::UniString what = "The story";
		if (IsNumber (os, key))
			what = GS::UniString::Printf ("Story index %d", (int) GetInt (os, key));
		Fail (what + " does not exist in this project. Use get_stories to list the stories (index 0 = ground floor, "
			  "negative = basements), or pass the story name.", e.code);
	}
}


QueryFilter ParseFilter (const OS& params)
{
	QueryFilter f;

	f.types = GetElemTypeArray (params, "types");
	for (API_ElemTypeID t : f.types)
		f.typeSet.Add ((Int32) t);
	for (API_ElemTypeID t : GetElemTypeArray (params, "excludeTypes"))
		f.excludeTypes.Add ((Int32) t);
	f.includeSubelements = GetBool (params, "includeSubelements", false);

	for (const OS& item : ArrayItems (params, "filters")) {
		if (!item.IsString ("v"))
			Fail ("'filters' must be an array of strings such as \"OnVisibleLayer\", \"Editable\", \"In3D\".");
		f.flags |= (API_ElemFilterFlags) ParseNamedString (kFilterFlags, GetString (item, "v"), "filters");
	}

	f.selectedOnly = GetBool (params, "selectedOnly", false);
	if (params.Contains ("withinElements"))
		f.within = GetGuidArray (params, "withinElements", false);

	// Stories: storyIndex (single) and/or stories (array); indices or names.
	GS::Array<OS> storyItems = ArrayItems (params, "stories");
	if (auto single = OptStoryChecked (params, "storyIndex"))
		storyItems.Push (OS ("v", (Int32) *single));
	if (!storyItems.IsEmpty ()) {
		GS::HashSet<Int32> set;
		for (const OS& item : storyItems) {
			if (auto s = OptStoryChecked (item, "v"))
				set.Add ((Int32) *s);
		}
		f.stories = set;
	}

	GS::Array<OS> layerItems = ArrayItems (params, "layers");
	if (!layerItems.IsEmpty ()) {
		GS::HashSet<Int32> set;
		for (const OS& item : layerItems) {
			if (auto l = OptAttr (API_LayerID, item, "v"))
				set.Add ((Int32) *l);
		}
		f.layers = set;
	}

	GS::Array<OS> renoItems = ArrayItems (params, "renovationStatus");
	if (!renoItems.IsEmpty ()) {
		GS::HashSet<Int32> set;
		for (const OS& item : renoItems)
			set.Add (ParseNamed (kRenovation, item, "v"));
		f.renovation = set;
	}

	if (auto pattern = OptString (params, "elementId")) {
		if (!pattern->IsEmpty ())
			f.elementIdPattern = Lower (*pattern);
	}
	if (auto lp = OptString (params, "libraryPart")) {
		if (!lp->IsEmpty ()) {
			f.libraryPart = Lower (*lp);
			f.needElement = true;
		}
	}

	if (auto group = OptGuid (params, "groupGuid")) {
		GS::Array<API_Guid> members;
		GSErrCode err = ACAPI_ElementGroup_GetAllGroupedElems (*group, &members);
		if (err != NoError)
			Fail ("Group " + GuidStr (*group) + " not found (" + ErrorName (err) + "). Take groupGuid from find_elements / get_element_details output.", err);
		GS::HashSet<API_Guid> set;
		for (const API_Guid& g : members)
			set.Add (g);
		f.groupMembers = set;
	}
	f.grouped = OptBool (params, "grouped");
	f.hotlinkGuid = OptGuid (params, "hotlinkGuid");
	f.inHotlink = OptBool (params, "inHotlink");
	f.locked = OptBool (params, "locked");

	OS region;
	if (TryGetObject (params, "region", region)) {
		Region r;
		r.xMin = OptDouble (region, "xMin");
		r.yMin = OptDouble (region, "yMin");
		r.xMax = OptDouble (region, "xMax");
		r.yMax = OptDouble (region, "yMax");
		r.zMin = OptDouble (region, "zMin");
		r.zMax = OptDouble (region, "zMax");
		if (auto mode = OptString (region, "mode")) {
			if (EqualsIgnoreCase (*mode, "Inside"))
				r.inside = true;
			else if (!EqualsIgnoreCase (*mode, "Intersects"))
				Fail ("region.mode must be \"Intersects\" (default) or \"Inside\".");
		}
		if (!r.xMin && !r.yMin && !r.xMax && !r.yMax && !r.zMin && !r.zMax)
			Fail ("region needs at least one of xMin, yMin, xMax, yMax, zMin, zMax (meters; z is absolute, relative to project zero).");
		f.region = r;
	}
	return f;
}


struct Match {
	API_Elem_Head	head;
	bool			hasBox = false;
	API_Box3D		box;
	Int32			libInd = 0;
};


struct QueryStats {
	Int32	scanned = 0;
	Int32	missing = 0;
	Int32	noBounds = 0;
	Int32	ownedObjectsSkipped = 0;	// GDL part objects of curtain walls / railings / stairs (includeSubelements shows them)
};


bool CalcBounds (const API_Elem_Head& head, API_Box3D& box)
{
	BNZeroMemory (&box, sizeof (box));
	API_Elem_Head h = head;
	return ACAPI_Database (APIDb_CalcBoundsID, &h, &box) == NoError;
}


GS::Array<API_Guid> SelectedGuids ()
{
	API_SelectionInfo info;
	BNZeroMemory (&info, sizeof (info));
	GS::Array<API_Neig> neigs;
	GSErrCode err = ACAPI_Selection_Get (&info, &neigs, false, true);
	BMKillHandle (reinterpret_cast<GSHandle*> (&info.marquee.coords));
	GS::Array<API_Guid> result;
	if (err != NoError && err != APIERR_NOSEL)
		Check (err, "Cannot read the selection");
	for (const API_Neig& n : neigs)
		result.Push (n.guid);
	return result;
}


GS::Array<Match> RunQuery (const QueryFilter& f, QueryStats& stats, NameCache& names)
{
	// 1. Candidate GUIDs.
	GS::Array<API_Guid> candidates;
	bool listedWithFlags = false;
	if (f.selectedOnly || f.within.has_value ()) {
		if (f.selectedOnly) {
			candidates = SelectedGuids ();
			if (f.within.has_value ()) {
				GS::HashSet<API_Guid> allowed;
				for (const API_Guid& g : *f.within)
					allowed.Add (g);
				GS::Array<API_Guid> both;
				for (const API_Guid& g : candidates) {
					if (allowed.Contains (g))
						both.Push (g);
				}
				candidates = both;
			}
		} else {
			candidates = *f.within;
		}
	} else {
		const GS::Array<API_ElemTypeID>& types = f.types.IsEmpty () ? AllElemTypes () : f.types;
		for (API_ElemTypeID t : types) {
			if (f.types.IsEmpty () && !f.includeSubelements && IsSubelementType (t))
				continue;
			if (f.excludeTypes.Contains ((Int32) t))
				continue;
			GS::Array<API_Guid> list;
			if (ACAPI_Element_GetElemList (API_ElemType (t), &list, f.flags) == NoError)
				candidates.Append (list);
		}
		listedWithFlags = true;
	}

	// 2. Filter.
	GS::Array<Match> matches;
	GS::HashSet<API_Guid> seen;
	for (const API_Guid& guid : candidates) {
		if (seen.Contains (guid))
			continue;
		seen.Add (guid);
		++stats.scanned;

		API_Elem_Head head;
		BNZeroMemory (&head, sizeof (head));
		head.guid = guid;
		if (ACAPI_Element_GetHeader (&head) != NoError) {
			++stats.missing;
			continue;
		}
		// Explicit element lists (selection / withinElements) keep sub-elements unless excluded.
		const API_ElemTypeID typeID = head.type.typeID;
		if (!f.typeSet.IsEmpty () && !f.typeSet.Contains ((Int32) typeID))
			continue;
		if (f.excludeTypes.Contains ((Int32) typeID))
			continue;
		if (!listedWithFlags && f.flags != APIFilt_None && !ACAPI_Element_Filter (guid, f.flags))
			continue;
		if (f.stories.has_value () && !f.stories->Contains ((Int32) head.floorInd))
			continue;
		if (f.layers.has_value () && !f.layers->Contains ((Int32) head.layer))
			continue;
		if (f.renovation.has_value () && !f.renovation->Contains ((Int32) head.renovationStatus))
			continue;
		if (f.grouped.has_value () && (*f.grouped != (head.groupGuid != APINULLGuid)))
			continue;
		if (f.groupMembers.has_value () && !f.groupMembers->Contains (guid))
			continue;
		if (f.inHotlink.has_value () && (*f.inHotlink != (head.hotlinkGuid != APINULLGuid)))
			continue;
		if (f.hotlinkGuid.has_value () && head.hotlinkGuid != *f.hotlinkGuid)
			continue;
		if (f.locked.has_value () && (*f.locked != (head.lockId != 0)))
			continue;
		if (f.elementIdPattern.has_value ()) {
			if (!WildcardMatch (*f.elementIdPattern, Lower (GetElementInfoString (guid))))
				continue;
		}

		// Objects/lamps owned by another element (the GDL parts Archicad keeps for curtain wall frames/panels,
		// railing and stair components, and the marker head objects of elevations/sections/details, whose
		// owner GUID is set without an owner type) are sub-elements: skip them unless includeSubelements.
		if (listedWithFlags && !f.includeSubelements && (typeID == API_ObjectID || typeID == API_LampID)) {
			API_Element owned;
			BNZeroMemory (&owned, sizeof (owned));
			owned.header.guid = guid;
			if (ACAPI_Element_Get (&owned) == NoError &&
				(owned.object.ownerType.typeID != API_ZombieElemID || owned.object.owner != APINULLGuid)) {
				++stats.ownedObjectsSkipped;
				continue;
			}
		}

		Match m;
		m.head = head;
		if (f.needElement) {
			API_Element element;
			BNZeroMemory (&element, sizeof (element));
			element.header.guid = guid;
			if (ACAPI_Element_Get (&element) != NoError) {
				++stats.missing;
				continue;
			}
			m.libInd = LibIndOf (element);
			if (f.libraryPart.has_value ()) {
				if (m.libInd <= 0)
					continue;
				if (!Lower (names.LibPartName (m.libInd)).Contains (*f.libraryPart))
					continue;
			}
		}
		if (f.region.has_value ()) {
			if (!CalcBounds (head, m.box)) {
				++stats.noBounds;
				continue;
			}
			m.hasBox = true;
			if (!f.region->Matches (m.box))
				continue;
		}
		matches.Push (m);
	}
	return matches;
}


OS StatsJson (const QueryStats& stats)
{
	OS out ("scanned", stats.scanned);
	if (stats.missing > 0)
		out.Add ("unreadable", stats.missing);
	if (stats.noBounds > 0)
		out.Add ("withoutBounds", stats.noBounds);
	if (stats.ownedObjectsSkipped > 0)
		out.Add ("ownedPartObjectsSkipped", stats.ownedObjectsSkipped);
	return out;
}


template <typename K>
void Increment (std::map<K, Int32>& m, const K& key)
{
	auto it = m.find (key);
	if (it == m.end ())
		m.emplace (key, 1);
	else
		++it->second;
}


OS CountsToJson (const std::map<std::string, Int32>& counts)
{
	OS out;
	for (const auto& kv : counts)
		out.Add (GS::String (kv.first.c_str ()), kv.second);
	return out;
}


std::string TypeKey (const API_ElemType& type)
{
	return std::string (ToStr (ElemTypeName (type)).ToCStr ());
}


// Element lists come from the CURRENT database: with a layout / section / detail in front, floor plan
// elements are not searched (verified live: 0 elevation markers while a layout was open).
void AddNonPlanDatabaseHint (OS& out, const OS& params)
{
	API_DatabaseInfo db;
	BNZeroMemory (&db, sizeof (db));
	if (ACAPI_Database (APIDb_GetCurrentDatabaseID, &db, nullptr) != NoError || db.typeID == APIWind_FloorPlanID || db.typeID == APIWind_3DModelID)
		return;
	for (const GS::UniString& name : GetStringArray (params, "filters")) {
		if (EqualsIgnoreCase (name, "FromFloorPlan"))
			return;
	}
	out.Add ("hint", GS::UniString ("The active window is not the floor plan, so only the elements of its own database were searched "
									"(e.g. the drawings and texts of a layout). Pass filters: ['FromFloorPlan'] to search the floor plan."));
}

} // namespace


void RegisterFindCommands ()
{
	RegisterCommand ("FindElements",
		"Finds elements with combinable filters and returns a paginated list [{guid, type, storyIndex, layer:{index,name}, elementId, "
		"(boundingBox), (libraryPart)}]. Input (all optional): types[], excludeTypes[], includeSubelements, filters[] "
		"(OnVisibleLayer|Editable|OnActiveStory|In3D|InMyWorkspace|Independent|OnActiveLayout|InCroppedView|HasAccessRight|"
		"VisibleByRenovation|Overridden|InStructureDisplay|FromFloorPlan), storyIndex, stories[], layers[], renovationStatus[], "
		"selectedOnly, withinElements[], elementId (wildcards * ?), libraryPart (substring), groupGuid, grouped, hotlinkGuid, inHotlink, "
		"locked, region {xMin,yMin,xMax,yMax,zMin,zMax,mode:Intersects|Inside}, includeBoundingBox, includeLibraryPart, "
		"includeElementId (default true), offset, limit (default 500).",
		[] (const OS& params) -> OS {
			QueryFilter f = ParseFilter (params);
			const bool withBox = GetBool (params, "includeBoundingBox", false);
			const bool withLib = GetBool (params, "includeLibraryPart", false);
			const bool withId = GetBool (params, "includeElementId", true);
			if (withLib)
				f.needElement = true;
			const Int32 offset = std::max (GetInt (params, "offset", 0), (Int32) 0);
			const Int32 limit = std::max (std::min (GetInt (params, "limit", 500), (Int32) 10000), (Int32) 1);

			NameCache names;
			QueryStats stats;
			GS::Array<Match> matches = RunQuery (f, stats, names);

			const Int32 total = (Int32) matches.GetSize ();
			GS::Array<OS> list;
			for (Int32 i = offset; i < total && i < offset + limit; ++i) {
				Match& m = matches[i];
				OS item = ElementBrief (m.head, names, withId);
				if (withBox) {
					if (!m.hasBox)
						m.hasBox = CalcBounds (m.head, m.box);
					if (m.hasBox)
						item.Add ("boundingBox", Box3DObj (m.box));
				}
				if (withLib && m.libInd > 0)
					item.Add ("libraryPart", names.LibPartName (m.libInd));
				list.Push (item);
			}
			OS out;
			out.Add ("total", total);
			out.Add ("offset", offset);
			out.Add ("returned", (Int32) list.GetSize ());
			out.Add ("hasMore", offset + (Int32) list.GetSize () < total);
			out.Add ("elements", list);
			out.Add ("stats", StatsJson (stats));
			AddNonPlanDatabaseHint (out, params);
			return out;
		});

	RegisterCommand ("GetElementCounts",
		"Counts elements per type (and optionally per story / layer / renovation status). Accepts the same filters as FindElements "
		"plus groupBy[]: \"story\" | \"layer\" | \"renovationStatus\". Output: {total, byType: {Wall: n, ...}, byStory?: [{storyIndex, "
		"storyName, total, byType}], byLayer?: [{layer, total, byType}], byRenovationStatus?: {...}}.",
		[] (const OS& params) -> OS {
			QueryFilter f = ParseFilter (params);
			bool byStory = false, byLayer = false, byReno = false;
			for (const OS& item : ArrayItems (params, "groupBy")) {
				GS::UniString g = GetString (item, "v");
				if (EqualsIgnoreCase (g, "story") || EqualsIgnoreCase (g, "stories"))			byStory = true;
				else if (EqualsIgnoreCase (g, "layer") || EqualsIgnoreCase (g, "layers"))		byLayer = true;
				else if (EqualsIgnoreCase (g, "renovationStatus") || EqualsIgnoreCase (g, "renovation")) byReno = true;
				else if (EqualsIgnoreCase (g, "type")) {}
				else Fail ("groupBy values must be \"story\", \"layer\" or \"renovationStatus\" (per-type counts are always returned).");
			}

			NameCache names;
			QueryStats stats;
			GS::Array<Match> matches = RunQuery (f, stats, names);

			std::map<std::string, Int32> byType;
			std::map<Int32, std::map<std::string, Int32>> storyTypes;
			std::map<Int32, std::map<std::string, Int32>> layerTypes;
			std::map<std::string, Int32> renoCounts;
			for (const Match& m : matches) {
				const std::string key = TypeKey (m.head.type);
				Increment (byType, key);
				if (byStory) Increment (storyTypes[(Int32) m.head.floorInd], key);
				if (byLayer) Increment (layerTypes[(Int32) m.head.layer], key);
				if (byReno)  Increment (renoCounts, std::string (ToStr (RenovationStatusName (m.head.renovationStatus)).ToCStr ()));
			}

			auto sumOf = [] (const std::map<std::string, Int32>& m) {
				Int32 s = 0;
				for (const auto& kv : m) s += kv.second;
				return s;
			};

			OS out;
			out.Add ("total", (Int32) matches.GetSize ());
			out.Add ("byType", CountsToJson (byType));
			if (byStory) {
				GS::Array<OS> list;
				for (const auto& kv : storyTypes) {
					list.Push (OS ("storyIndex", kv.first, "storyName", names.StoryNameOf ((short) kv.first),
								   "total", sumOf (kv.second), "byType", CountsToJson (kv.second)));
				}
				out.Add ("byStory", list);
			}
			if (byLayer) {
				std::vector<std::pair<Int32, Int32>> order;		// (layer, total)
				for (const auto& kv : layerTypes)
					order.push_back ({ kv.first, sumOf (kv.second) });
				std::sort (order.begin (), order.end (), [] (const auto& a, const auto& b) { return a.second > b.second; });
				GS::Array<OS> list;
				for (const auto& lt : order) {
					list.Push (OS ("layer", names.Layer (lt.first), "total", lt.second,
								   "byType", CountsToJson (layerTypes[lt.first])));
				}
				out.Add ("byLayer", list);
			}
			if (byReno)
				out.Add ("byRenovationStatus", CountsToJson (renoCounts));
			out.Add ("stats", StatsJson (stats));
			return out;
		});
}

// =============================================================================
// Selection
// =============================================================================

namespace {

const NamedValue kMarqueeRelations[] = {
	{ "InsidePartially",	API_InsidePartially },
	{ "InsideEntirely",		API_InsideEntirely },
	{ "OutsidePartially",	API_OutsidePartially },
	{ "OutsideEntirely",	API_OutsideEntirely },
};


GS::UniString SelectionTypeName (API_SelTypeID t)
{
	switch (t) {
		case API_SelEmpty:		return "None";
		case API_SelElems:		return "Elements";
		case API_MarqueePoly:	return "MarqueePolygon";
		case API_MarqueeHorBox:	return "MarqueeBox";
		case API_MarqueeRotBox:	return "MarqueeRotatedBox";
		default:				return "Unknown";
	}
}


OS MarqueeJson (const API_SelectionInfo& info)
{
	OS out;
	out.Add ("box", BoxObj (info.marquee.box));
	if (std::fabs (info.marquee.boxRotAngle) > 1e-12)
		AddAngle (out, "boxRotationAngle", info.marquee.boxRotAngle);
	out.Add ("multiStory", info.multiStory);
	GS::Array<OS> pts;
	if (info.marquee.coords != nullptr && *info.marquee.coords != nullptr && info.marquee.nCoords > 0) {
		const Int32 inHandle = (Int32) (BMGetHandleSize (reinterpret_cast<GSConstHandle> (info.marquee.coords)) / (GSSize) sizeof (API_Coord));
		Int32 n = std::min (info.marquee.nCoords, inHandle);
		const API_Coord* c = *info.marquee.coords;
		if (n > 1 && std::fabs (c[0].x - c[n - 1].x) < 1e-9 && std::fabs (c[0].y - c[n - 1].y) < 1e-9)
			--n;	// closing point
		for (Int32 i = 0; i < n; ++i)
			pts.Push (CoordObj (c[i]));
	}
	if (!pts.IsEmpty ())
		out.Add ("polygon", pts);
	return out;
}


GS::UniString SelectError (GSErrCode err)
{
	GS::UniString msg = ErrorName (err);
	if (err == APIERR_NOSEL || err == APIERR_BADID || err == APIERR_REFUSEDCMD)
		msg += ". The element may be on a hidden or locked layer, on another story/database than the active window, or not selectable here.";
	return msg;
}

} // namespace


void RegisterSelectionCommands ()
{
	RegisterCommand ("GetSelection",
		"Returns the current selection: {selectionType: None|Elements|MarqueePolygon|MarqueeBox|MarqueeRotatedBox, total, editableCount, "
		"elements: [{guid, type, storyIndex, layer, elementId, (partial)}], marquee?: {box, polygon, boxRotationAngle, multiStory}}. "
		"Input (optional): onlyEditable, includePartial (partially selected elements, default false), marqueeRelation "
		"(InsidePartially|InsideEntirely|OutsidePartially|OutsideEntirely — which elements count when a marquee is active), "
		"includeElementId (default true), offset, limit (default 1000).",
		[] (const OS& params) -> OS {
			const bool onlyEditable = GetBool (params, "onlyEditable", false);
			const bool includePartial = GetBool (params, "includePartial", false);
			const bool withId = GetBool (params, "includeElementId", true);
			API_SelRelativePosID rel = API_InsidePartially;
			if (params.Contains ("marqueeRelation"))
				rel = (API_SelRelativePosID) ParseNamed (kMarqueeRelations, params, "marqueeRelation");
			const Int32 offset = std::max (GetInt (params, "offset", 0), (Int32) 0);
			const Int32 limit = std::max (std::min (GetInt (params, "limit", 1000), (Int32) 10000), (Int32) 1);

			API_SelectionInfo info;
			BNZeroMemory (&info, sizeof (info));
			GS::Array<API_Neig> neigs;
			GSErrCode err = ACAPI_Selection_Get (&info, &neigs, onlyEditable, !includePartial, rel);
			OS marquee;
			const bool isMarquee = (err == NoError || err == APIERR_NOSEL) &&
								   (info.typeID == API_MarqueePoly || info.typeID == API_MarqueeHorBox || info.typeID == API_MarqueeRotBox);
			if (isMarquee)
				marquee = MarqueeJson (info);
			BMKillHandle (reinterpret_cast<GSHandle*> (&info.marquee.coords));
			if (err != NoError && err != APIERR_NOSEL)
				Check (err, "Cannot read the selection");

			// Unique elements in selection order.
			GS::Array<const API_Neig*> unique;
			GS::HashSet<API_Guid> seen;
			for (const API_Neig& n : neigs) {
				if (n.guid == APINULLGuid || seen.Contains (n.guid))
					continue;
				seen.Add (n.guid);
				unique.Push (&n);
			}

			NameCache names;
			GS::Array<OS> list;
			const Int32 total = (Int32) unique.GetSize ();
			for (Int32 i = offset; i < total && i < offset + limit; ++i) {
				const API_Neig& n = *unique[i];
				API_Elem_Head head;
				BNZeroMemory (&head, sizeof (head));
				head.guid = n.guid;
				if (ACAPI_Element_GetHeader (&head) != NoError) {
					list.Push (OS ("guid", GuidStr (n.guid), "error", GS::UniString ("element header not readable")));
					continue;
				}
				OS item = ElementBrief (head, names, withId);
				if (n.elemPartType != APINeigElemPart_None)
					item.Add ("partial", true);
				list.Push (item);
			}

			OS out;
			out.Add ("selectionType", (err == APIERR_NOSEL && !isMarquee) ? GS::UniString ("None") : SelectionTypeName (info.typeID));
			out.Add ("total", total);
			out.Add ("editableCount", (Int32) info.sel_nElemEdit);
			out.Add ("offset", offset);
			out.Add ("hasMore", offset + (Int32) list.GetSize () < total);
			out.Add ("elements", list);
			if (isMarquee)
				out.Add ("marquee", marquee);
			return out;
		});

	RegisterCommand ("SetSelection",
		"Changes the selection. Input: {mode: \"set\" (replace, default) | \"add\" | \"remove\" | \"clear\", elements: [guid]}. "
		"Output: {mode, requested, applied, failed: [{guid, error}], selectionCount}. Selection is not undoable.",
		[] (const OS& params) -> OS {
			GS::UniString mode = GetString (params, "mode", "set");
			const bool isSet = EqualsIgnoreCase (mode, "set") || EqualsIgnoreCase (mode, "replace");
			const bool isAdd = EqualsIgnoreCase (mode, "add");
			const bool isRemove = EqualsIgnoreCase (mode, "remove") || EqualsIgnoreCase (mode, "deselect");
			const bool isClear = EqualsIgnoreCase (mode, "clear") || EqualsIgnoreCase (mode, "none");
			if (!isSet && !isAdd && !isRemove && !isClear)
				Fail ("mode must be \"set\", \"add\", \"remove\" or \"clear\".");

			GS::Array<API_Guid> guids = OptGuidArray (params, "elements");
			if (!isClear && guids.IsEmpty () && !isSet)
				Fail ("Pass 'elements' (GUIDs from find_elements) for mode '" + mode + "'.");

			GS::Array<OS> failed;
			GS::Array<API_Neig> neigs;
			GS::HashSet<API_Guid> seen;
			for (const API_Guid& g : guids) {
				if (seen.Contains (g))
					continue;
				seen.Add (g);
				if (!ElementExists (g)) {
					failed.Push (OS ("guid", GuidStr (g), "error", GS::UniString ("Element not found (deleted, or in another database).")));
					continue;
				}
				neigs.PushNew (g);
			}

			if (isSet || isClear) {
				GSErrCode err = ACAPI_Element_DeselectAll ();
				if (err != NoError && err != APIERR_NOSEL)
					Check (err, "Cannot clear the selection");
			}

			Int32 applied = 0;
			if (!isClear && !neigs.IsEmpty ()) {
				const bool add = !isRemove;
				GSErrCode err = ACAPI_Element_Select (neigs, add);
				if (err == NoError) {
					applied = (Int32) neigs.GetSize ();
				} else {
					// Retry one by one to find the elements that cannot be (de)selected.
					for (const API_Neig& n : neigs) {
						GSErrCode e = ACAPI_Element_Select ({ n }, add);
						if (e == NoError)
							++applied;
						else
							failed.Push (OS ("guid", GuidStr (n.guid), "error", SelectError (e)));
					}
				}
			}

			Int32 selectionCount = 0;
			{
				API_SelectionInfo info;
				BNZeroMemory (&info, sizeof (info));
				GS::Array<API_Neig> sel;
				if (ACAPI_Selection_Get (&info, &sel, false, true) == NoError) {
					GS::HashSet<API_Guid> uniq;
					for (const API_Neig& n : sel)
						uniq.Add (n.guid);
					selectionCount = (Int32) uniq.GetSize ();
				}
				BMKillHandle (reinterpret_cast<GSHandle*> (&info.marquee.coords));
			}

			OS out;
			out.Add ("mode", isSet ? GS::UniString ("set") : isAdd ? GS::UniString ("add") : isRemove ? GS::UniString ("remove") : GS::UniString ("clear"));
			out.Add ("requested", (Int32) guids.GetSize ());
			out.Add ("applied", applied);
			out.Add ("failed", failed);
			out.Add ("selectionCount", selectionCount);
			return out;
		});
}

} // namespace eq


void RegisterElementQueryCommands ()
{
	eq::RegisterFindCommands ();
	eq::RegisterSelectionCommands ();
	eq::RegisterQuantityCommands ();
	eq::RegisterRelationCommands ();
	eq::RegisterGeometryCommands ();
}

} // namespace cc
