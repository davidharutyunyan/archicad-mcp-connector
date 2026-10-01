// *****************************************************************************
// ElementQueryRelations — GetConnectedElements, GetElementRelations and
// GetSubelements (family "element-query").
//
//   GetConnectedElements  ACAPI_Element_GetConnectedElements (openings, skylights,
//                         labels ... of an element) + owner/host, solid element
//                         operations, roof/shell trims
//   GetElementRelations   ACAPI_Element_GetRelations: zone <-> boundary elements,
//                         wall/beam connections, opening from/to zones, roof/shell zones
//   GetSubelements        parts of curtain walls, stairs, railings, beams, columns
// *****************************************************************************

#include "Commands/ElementQueryShared.hpp"
#include "Core/Command.hpp"
#include "Core/Enums.hpp"

#include <map>
#include <string>

namespace cc {
namespace eq {

namespace {

const NamedValue kSolidOps[] = {
	{ "Subtraction",				APISolid_Substract },
	{ "SubtractionUpwards",			APISolid_SubstUp },
	{ "SubtractionDownwards",		APISolid_SubstDown },
	{ "Intersection",				APISolid_Intersect },
	{ "Addition",					APISolid_Add },
};

const NamedValue kTrimTypes[] = {
	{ "None",			APITrim_No },
	{ "KeepInside",		APITrim_KeepInside },
	{ "KeepOutside",	APITrim_KeepOutside },
	{ "KeepAll",		APITrim_KeepAll },
};


GS::Array<GS::UniString> GuidStrings (const GS::Array<API_Guid>& guids)
{
	GS::Array<GS::UniString> out;
	for (const API_Guid& g : guids)
		out.Push (GuidStr (g));
	return out;
}


OS RefOf (const API_Guid& guid)
{
	OS ref ("guid", GuidStr (guid));
	API_Elem_Head head;
	BNZeroMemory (&head, sizeof (head));
	head.guid = guid;
	if (ACAPI_Element_GetHeader (&head) == NoError)
		ref.Add ("type", ElemTypeName (head.type));
	return ref;
}


OS WithGuid (OS result, const API_Guid& guid)
{
	if (result.Contains ("error") && !result.Contains ("guid"))
		result.Add ("guid", GuidStr (guid));
	return result;
}

// --- Relations ---------------------------------------------------------------------

GS::Array<OS> ConnectionsFromHandle (API_ConnectionGuidItem** items, Int32 count)
{
	GS::Array<OS> out;
	if (items == nullptr || *items == nullptr || count <= 0)
		return out;
	const Int32 inHandle = (Int32) (BMGetHandleSize (reinterpret_cast<GSConstHandle> (items)) / (GSSize) sizeof (API_ConnectionGuidItem));
	const Int32 n = std::min (count, inHandle);
	for (Int32 i = 0; i < n; ++i) {
		const API_ConnectionGuidItem& c = (*items)[i];
		out.Push (OS ("guid", GuidStr (c.guid), "connectedWithItsBegin", c.conWithBeg));
	}
	return out;
}


GS::Array<OS> ConnectionsFromArray (const GS::Array<API_ConnectionGuidItem>* items)
{
	GS::Array<OS> out;
	if (items == nullptr)
		return out;
	for (const API_ConnectionGuidItem& c : *items)
		out.Push (OS ("guid", GuidStr (c.guid), "connectedWithItsBegin", c.conWithBeg));
	return out;
}


void AddIfAny (OS& out, const char* key, const GS::Array<OS>& list)
{
	if (!list.IsEmpty ())
		out.Add (key, list);
}


OS ZoneRelations (const API_Guid& guid, bool withPolygons)
{
	API_RoomRelation info;		// has C++ members: default-construct, never BNZeroMemory
	GSErrCode err = ACAPI_Element_GetRelations (guid, API_ZombieElemID, &info);
	if (err != NoError) {
		ACAPI_DisposeRoomRelationHdls (&info);
		Check (err, "Cannot read the relations of zone " + GuidStr (guid));
	}

	OS out;
	out.Add ("relationKind", GS::UniString ("Zone"));
	// Merge by type name (the table is keyed by API_ElemType, i.e. type + variation).
	std::map<std::string, GS::Array<GS::UniString>> grouped;
	Int32 total = 0;
	for (const auto& pair : info.elementsGroupedByType) {
		const API_ElemType& type = *pair.key;
		const GS::Array<API_Guid>& list = *pair.value;
		total += (Int32) list.GetSize ();
		grouped[std::string (ToStr (ElemTypeName (type)).ToCStr ())].Append (GuidStrings (list));
	}
	OS byType;
	for (const auto& kv : grouped)
		byType.Add (GS::String (kv.first.c_str ()), kv.second);
	out.Add ("relatedElementCount", total);
	out.Add ("relatedElementsByType", byType);

	GS::Array<OS> wallParts;
	for (const API_WallPart& p : info.wallPart)
		wallParts.Push (OS ("wall", GuidStr (p.guid), "zoneEdgeIndex", (Int32) p.roomEdge, "tBegin", p.tBeg, "tEnd", p.tEnd));
	AddIfAny (out, "wallParts", wallParts);

	GS::Array<OS> beamParts;
	for (const API_BeamPart& p : info.beamPart)
		beamParts.Push (OS ("beam", GuidStr (p.guid), "tBegin", p.tBeg, "tEnd", p.tEnd));
	AddIfAny (out, "beamParts", beamParts);

	GS::Array<OS> cwParts;
	for (const API_CWSegmentPart& p : info.cwSegmentPart)
		cwParts.Push (OS ("curtainWallSegment", GuidStr (p.guid), "zoneEdgeIndex", (Int32) p.roomEdge, "tBegin", p.tBeg, "tEnd", p.tEnd));
	AddIfAny (out, "curtainWallSegmentParts", cwParts);

	GS::Array<OS> niches;
	for (const API_Niche& n : info.niches) {
		OS niche ("height", n.height);
		if (withPolygons)
			niche.Add ("polygon", HandlesPolygonToJson (n.coords, n.pends, n.parcs));
		niches.Push (niche);
	}
	AddIfAny (out, "niches", niches);

	ACAPI_DisposeRoomRelationHdls (&info);
	return out;
}


OS WallRelations (const API_Guid& guid, bool withPolygons)
{
	API_WallRelation info;
	BNZeroMemory (&info, sizeof (info));
	GSErrCode err = ACAPI_Element_GetRelations (guid, API_ZombieElemID, &info);
	if (err != NoError) {
		ACAPI_DisposeWallRelationHdls (&info);
		Check (err, "Cannot read the relations of wall " + GuidStr (guid));
	}
	OS out;
	out.Add ("relationKind", GS::UniString ("Wall"));
	if (withPolygons)
		out.Add ("connectionPolygon", HandlesPolygonToJson (info.coords, info.pends, info.parcs, &info.connPoly));
	out.Add ("connectedAtBegin", ConnectionsFromHandle (info.conBeg, info.nConBeg));
	out.Add ("connectedAtEnd", ConnectionsFromHandle (info.conEnd, info.nConEnd));
	out.Add ("connectedToReferenceLine", ConnectionsFromHandle (info.conRef, info.nConRef));
	out.Add ("connectedWithTheirEnds", ConnectionsFromHandle (info.con, info.nCon));
	out.Add ("crossing", ConnectionsFromHandle (info.conX, info.nConX));
	ACAPI_DisposeWallRelationHdls (&info);
	return out;
}


OS BeamConnectionsJson (const GS::Array<API_ConnectionGuidItem>* beg, const GS::Array<API_ConnectionGuidItem>* end,
						const GS::Array<API_ConnectionGuidItem>* ref, const GS::Array<API_ConnectionGuidItem>* con,
						const GS::Array<API_ConnectionGuidItem>* x)
{
	OS out;
	out.Add ("connectedAtBegin", ConnectionsFromArray (beg));
	out.Add ("connectedAtEnd", ConnectionsFromArray (end));
	out.Add ("connectedToReferenceLine", ConnectionsFromArray (ref));
	out.Add ("connectedWithTheirEnds", ConnectionsFromArray (con));
	out.Add ("crossing", ConnectionsFromArray (x));
	return out;
}


OS BeamRelations (const API_Guid& guid, bool withPolygons)
{
	API_BeamRelation info;
	BNZeroMemory (&info, sizeof (info));
	GSErrCode err = ACAPI_Element_GetRelations (guid, API_ZombieElemID, &info);
	if (err != NoError) {
		ACAPI_DisposeBeamRelationHdls (&info);
		Check (err, "Cannot read the relations of beam " + GuidStr (guid));
	}
	OS out = BeamConnectionsJson (info.conBeg, info.conEnd, info.conRef, info.con, info.conX);
	out.Add ("relationKind", GS::UniString ("Beam"));
	if (withPolygons)
		out.Add ("connectionPolygon", HandlesPolygonToJson (info.coords, info.pends, info.parcs, &info.connPoly));
	if (info.segmentRelations != nullptr) {
		GS::Array<OS> segments;
		Int32 index = 0;
		for (const API_BeamSegmentRelation& s : *info.segmentRelations) {
			OS seg = BeamConnectionsJson (s.conBeg, s.conEnd, s.conRef, s.con, s.conX);
			seg.Add ("segmentIndex", index++);
			if (withPolygons)
				seg.Add ("connectionPolygon", HandlesPolygonToJson (s.coords, s.pends, s.parcs, &s.connPoly));
			segments.Push (seg);
		}
		AddIfAny (out, "segments", segments);
	}
	ACAPI_DisposeBeamRelationHdls (&info);
	return out;
}


OS OpeningRelations (const API_Guid& guid, const GS::UniString& typeName)
{
	API_WindowRelation info;
	BNZeroMemory (&info, sizeof (info));
	Check (ACAPI_Element_GetRelations (guid, API_ZombieElemID, &info), "Cannot read the relations of " + typeName + " " + GuidStr (guid));
	OS out;
	out.Add ("relationKind", GS::UniString ("Opening"));
	if (info.fromRoom != APINULLGuid)
		out.Add ("fromZone", GuidStr (info.fromRoom));
	if (info.toRoom != APINULLGuid)
		out.Add ("toZone", GuidStr (info.toRoom));
	return out;
}


OS RoofShellRelations (const API_Guid& guid, const GS::UniString& typeName)
{
	GS::Array<API_Guid> rooms;
	API_RoofRelation info;
	BNZeroMemory (&info, sizeof (info));
	info.rooms = &rooms;
	Check (ACAPI_Element_GetRelations (guid, API_ZombieElemID, &info), "Cannot read the relations of " + typeName + " " + GuidStr (guid));
	OS out;
	out.Add ("relationKind", GS::UniString ("RoofOrShell"));
	out.Add ("zones", GuidStrings (rooms));
	return out;
}

} // namespace


void RegisterRelationCommands ()
{
	RegisterCommand ("GetConnectedElements",
		"Elements attached to / hosted by each element. Input: {elements: [guid], types?: [\"Window\",\"Door\",\"Skylight\",\"Opening\","
		"\"Label\", ...] (default: those five), includeOwner (default true: host wall/roof of an opening, labelled element of a label, "
		"parent of a sub-element), includeSolidOperations (default false), includeTrims (default false), includeTypes (default false: "
		"return {guid,type} instead of plain GUIDs)}. Output: {elements: [{guid, type, connected: {Window: [...], ...}, owner?, "
		"solidOperations?: {operators, targets}, trims?: {trimmedBy, trims}} | {guid, error}]}.",
		[] (const OS& params) -> OS {
			GS::Array<API_Guid> guids = GetGuidArray (params, "elements", true);
			if (guids.IsEmpty ())
				Fail ("Pass at least one element GUID in 'elements'.");
			GS::Array<API_ElemTypeID> types = GetElemTypeArray (params, "types");
			const bool explicitTypes = !types.IsEmpty ();
			if (!explicitTypes)
				types = { API_WindowID, API_DoorID, API_SkylightID, API_OpeningID, API_LabelID };
			const bool withOwner = GetBool (params, "includeOwner", true);
			const bool withSolid = GetBool (params, "includeSolidOperations", false);
			const bool withTrims = GetBool (params, "includeTrims", false);
			const bool withTypes = GetBool (params, "includeTypes", false);

			auto refList = [&] (const GS::Array<API_Guid>& list) -> GS::Array<OS> {
				GS::Array<OS> out;
				for (const API_Guid& g : list)
					out.Push (RefOf (g));
				return out;
			};

			GS::Array<OS> results;
			for (const API_Guid& guid : guids) {
				results.Push (WithGuid (Try ([&] () -> OS {
					API_Element element = GetElement (guid);
					OS item;
					item.Add ("guid", GuidStr (guid));
					item.Add ("type", ElemTypeName (element.header.type));

					OS connected;
					OS errors;
					bool anyError = false;
					Int32 total = 0;
					for (API_ElemTypeID t : types) {
						GS::Array<API_Guid> list;
						GSErrCode err = ACAPI_Element_GetConnectedElements (guid, API_ElemType (t), &list);
						if (err != NoError) {
							if (explicitTypes) {
								errors.Add (ToStr (ElemTypeName (t)), ErrorName (err));
								anyError = true;
							}
							continue;
						}
						if (list.IsEmpty ())
							continue;
						total += (Int32) list.GetSize ();
						if (withTypes)
							connected.Add (ToStr (ElemTypeName (t)), refList (list));
						else
							connected.Add (ToStr (ElemTypeName (t)), GuidStrings (list));
					}
					item.Add ("connectedCount", total);
					item.Add ("connected", connected);
					if (anyError)
						item.Add ("connectedErrors", errors);

					if (withOwner) {
						API_Guid owner = OwnerOf (element);
						if (owner != APINULLGuid)
							item.Add ("owner", RefOf (owner));
					}

					if (withSolid) {
						OS solid;
						GS::Array<API_Guid> operators, targets;
						if (ACAPI_Element_SolidLink_GetOperators (guid, &operators) == NoError && !operators.IsEmpty ()) {
							GS::Array<OS> list;
							for (const API_Guid& op : operators) {
								OS ref = RefOf (op);
								API_SolidOperationID opId;
								if (ACAPI_Element_SolidLink_GetOperation (guid, op, &opId) == NoError)
									ref.Add ("operation", NameOf (kSolidOps, (Int32) opId));
								list.Push (ref);
							}
							solid.Add ("operators", list);	// elements that cut / modify this element
						}
						if (ACAPI_Element_SolidLink_GetTargets (guid, &targets) == NoError && !targets.IsEmpty ()) {
							GS::Array<OS> list;
							for (const API_Guid& target : targets) {
								OS ref = RefOf (target);
								API_SolidOperationID opId;
								if (ACAPI_Element_SolidLink_GetOperation (target, guid, &opId) == NoError)
									ref.Add ("operation", NameOf (kSolidOps, (Int32) opId));
								list.Push (ref);
							}
							solid.Add ("targets", list);	// elements this element cuts / modifies
						}
						item.Add ("solidOperations", solid);
					}

					if (withTrims) {
						OS trims;
						GS::Array<API_Guid> trimming, trimmed;
						if (ACAPI_Element_Trim_GetTrimmingElements (guid, &trimming) == NoError && !trimming.IsEmpty ()) {
							GS::Array<OS> list;
							for (const API_Guid& g : trimming) {
								OS ref = RefOf (g);
								API_TrimTypeID tt;
								if (ACAPI_Element_Trim_GetTrimType (guid, g, &tt) == NoError)
									ref.Add ("trimType", NameOf (kTrimTypes, (Int32) tt));
								list.Push (ref);
							}
							trims.Add ("trimmedBy", list);
						}
						if (ACAPI_Element_Trim_GetTrimmedElements (guid, &trimmed) == NoError && !trimmed.IsEmpty ()) {
							GS::Array<OS> list;
							for (const API_Guid& g : trimmed) {
								OS ref = RefOf (g);
								API_TrimTypeID tt;
								if (ACAPI_Element_Trim_GetTrimType (g, guid, &tt) == NoError)
									ref.Add ("trimType", NameOf (kTrimTypes, (Int32) tt));
								list.Push (ref);
							}
							trims.Add ("trims", list);
						}
						item.Add ("trims", trims);
					}
					return item;
				}), guid));
			}
			return OS ("elements", results);
		});

	RegisterCommand ("GetElementRelations",
		"Topological relations (ACAPI_Element_GetRelations). Input: {elements: [guid], includePolygons (default true)}. Per type: "
		"Zone -> relatedElementsByType (bounding walls, columns, slabs, doors, windows...), wallParts/beamParts/curtainWallSegmentParts "
		"(boundary pieces with zoneEdgeIndex, tBegin, tEnd), niches; Wall -> connectionPolygon (plan outline after joins), connectedAtBegin/"
		"AtEnd/ToReferenceLine/WithTheirEnds/crossing walls; Beam -> the same for beams + per segment; Window/Door/Skylight/"
		"CurtainWallPanel -> fromZone/toZone; Roof/Shell -> zones. Other types return an error.",
		[] (const OS& params) -> OS {
			GS::Array<API_Guid> guids = GetGuidArray (params, "elements", true);
			if (guids.IsEmpty ())
				Fail ("Pass at least one element GUID in 'elements'.");
			const bool withPolygons = GetBool (params, "includePolygons", true);

			GS::Array<OS> results;
			for (const API_Guid& guid : guids) {
				results.Push (WithGuid (Try ([&] () -> OS {
					API_Elem_Head head = GetHeader (guid);
					const GS::UniString typeName = ElemTypeName (head.type);
					OS rel;
					switch (head.type.typeID) {
						case API_ZoneID:				rel = ZoneRelations (guid, withPolygons);	break;
						case API_WallID:				rel = WallRelations (guid, withPolygons);	break;
						case API_BeamID:				rel = BeamRelations (guid, withPolygons);	break;
						case API_WindowID:
						case API_DoorID:
						case API_SkylightID:
						case API_CurtainWallPanelID:	rel = OpeningRelations (guid, typeName);	break;
						case API_RoofID:
						case API_ShellID:				rel = RoofShellRelations (guid, typeName);	break;
						default:
							Fail ("Relations are available for Zone, Wall, Beam, Window, Door, Skylight, CurtainWallPanel, Roof and Shell, "
								  "not for " + typeName + ". Use get_connected_elements / get_subelements instead.", APIERR_BADELEMENTTYPE);
					}
					rel.Add ("guid", GuidStr (guid));
					rel.Add ("type", typeName);
					return rel;
				}), guid));
			}
			return OS ("elements", results);
		});

	RegisterCommand ("GetSubelements",
		"Parts of hierarchical elements: CurtainWall -> segments, frames, panels, junctions, accessories; Stair -> risers, treads, "
		"structures; Railing -> segments, nodes, posts, inner posts, top rails, handrails, rails, panels, baluster sets, balusters, "
		"patterns, rail ends and connections; Beam/Column -> segments. For a sub-element GUID its owner is returned. Input: "
		"{elements: [guid], types?: [sub-element type names], maxPerType (default 500), includeDetails (default true)}. Output: "
		"{elements: [{guid, type, total, counts: {Type: n}, subelements: {Type: [{guid, index, ...}]}, truncated?} | {guid, type, owner}]}.",
		[] (const OS& params) -> OS {
			GS::Array<API_Guid> guids = GetGuidArray (params, "elements", true);
			if (guids.IsEmpty ())
				Fail ("Pass at least one element GUID in 'elements'.");
			GS::Array<API_ElemTypeID> filterTypes = GetElemTypeArray (params, "types");
			const Int32 maxPerType = std::max (GetInt (params, "maxPerType", 500), (Int32) 0);
			const bool withDetails = GetBool (params, "includeDetails", true);

			GS::Array<OS> results;
			for (const API_Guid& guid : guids) {
				results.Push (WithGuid (Try ([&] () -> OS {
					API_Element element = GetElement (guid);
					const API_ElemTypeID typeID = element.header.type.typeID;
					OS item;
					item.Add ("guid", GuidStr (guid));
					item.Add ("type", ElemTypeName (element.header.type));

					if (IsSubelementType (typeID)) {
						API_Guid owner = OwnerOf (element);
						if (owner != APINULLGuid)
							item.Add ("owner", RefOf (owner));
						item.Add ("isSubelement", true);
						return item;
					}
					if (!IsHierarchicalType (typeID))
						Fail (ElemTypeName (element.header.type) + " has no sub-elements. Hierarchical types: CurtainWall, Stair, Railing, Beam, Column.",
							  APIERR_BADELEMENTTYPE);

					GS::Array<SubElemInfo> subs = CollectSubelements (guid, typeID, withDetails);
					std::map<std::string, Int32> counts;
					std::map<std::string, GS::Array<OS>> lists;
					bool truncated = false;
					Int32 total = 0;
					for (const SubElemInfo& s : subs) {
						if (!filterTypes.IsEmpty () && !filterTypes.Contains (s.typeID))
							continue;
						const std::string key (ToStr (ElemTypeName (s.typeID)).ToCStr ());
						++total;
						Int32& c = counts[key];
						++c;
						if (c > maxPerType) {
							truncated = true;
							continue;
						}
						OS sub = withDetails ? s.extra : OS ();
						sub.Add ("guid", GuidStr (s.head.guid));
						sub.Add ("index", (Int32) s.index);
						lists[key].Push (sub);
					}
					OS countsJson, listsJson;
					for (const auto& kv : counts)
						countsJson.Add (GS::String (kv.first.c_str ()), kv.second);
					for (const auto& kv : lists)
						listsJson.Add (GS::String (kv.first.c_str ()), kv.second);
					item.Add ("total", total);
					item.Add ("counts", countsJson);
					item.Add ("subelements", listsJson);
					if (truncated)
						item.Add ("truncated", GS::UniString ("Some types have more than maxPerType items; raise maxPerType or filter with 'types'."));
					return item;
				}), guid));
			}
			return OS ("elements", results);
		});
}

} // namespace eq
} // namespace cc
