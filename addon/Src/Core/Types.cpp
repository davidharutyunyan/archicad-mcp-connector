#include "Core/Types.hpp"

namespace cc {

namespace {

struct ElemTypeEntry {
	API_ElemTypeID	id;
	const char*		name;
};

const ElemTypeEntry kElemTypes[] = {
	{ API_WallID,						"Wall" },
	{ API_ColumnID,						"Column" },
	{ API_BeamID,						"Beam" },
	{ API_WindowID,						"Window" },
	{ API_DoorID,						"Door" },
	{ API_ObjectID,						"Object" },
	{ API_LampID,						"Lamp" },
	{ API_SlabID,						"Slab" },
	{ API_RoofID,						"Roof" },
	{ API_MeshID,						"Mesh" },
	{ API_DimensionID,					"Dimension" },
	{ API_RadialDimensionID,			"RadialDimension" },
	{ API_LevelDimensionID,				"LevelDimension" },
	{ API_AngleDimensionID,				"AngleDimension" },
	{ API_TextID,						"Text" },
	{ API_LabelID,						"Label" },
	{ API_ZoneID,						"Zone" },
	{ API_HatchID,						"Hatch" },
	{ API_LineID,						"Line" },
	{ API_PolyLineID,					"PolyLine" },
	{ API_ArcID,						"Arc" },
	{ API_CircleID,						"Circle" },
	{ API_SplineID,						"Spline" },
	{ API_HotspotID,					"Hotspot" },
	{ API_CutPlaneID,					"CutPlane" },
	{ API_CameraID,						"Camera" },
	{ API_CamSetID,						"CamSet" },
	{ API_GroupID,						"Group" },
	{ API_SectElemID,					"SectElem" },
	{ API_DrawingID,					"Drawing" },
	{ API_PictureID,					"Picture" },
	{ API_DetailID,						"Detail" },
	{ API_ElevationID,					"Elevation" },
	{ API_InteriorElevationID,			"InteriorElevation" },
	{ API_WorksheetID,					"Worksheet" },
	{ API_HotlinkID,					"Hotlink" },
	{ API_CurtainWallID,				"CurtainWall" },
	{ API_CurtainWallSegmentID,			"CurtainWallSegment" },
	{ API_CurtainWallFrameID,			"CurtainWallFrame" },
	{ API_CurtainWallPanelID,			"CurtainWallPanel" },
	{ API_CurtainWallJunctionID,		"CurtainWallJunction" },
	{ API_CurtainWallAccessoryID,		"CurtainWallAccessory" },
	{ API_ShellID,						"Shell" },
	{ API_SkylightID,					"Skylight" },
	{ API_MorphID,						"Morph" },
	{ API_ChangeMarkerID,				"ChangeMarker" },
	{ API_StairID,						"Stair" },
	{ API_RiserID,						"Riser" },
	{ API_TreadID,						"Tread" },
	{ API_StairStructureID,				"StairStructure" },
	{ API_RailingID,					"Railing" },
	{ API_RailingToprailID,				"RailingToprail" },
	{ API_RailingHandrailID,			"RailingHandrail" },
	{ API_RailingRailID,				"RailingRail" },
	{ API_RailingPostID,				"RailingPost" },
	{ API_RailingInnerPostID,			"RailingInnerPost" },
	{ API_RailingBalusterID,			"RailingBaluster" },
	{ API_RailingPanelID,				"RailingPanel" },
	{ API_RailingSegmentID,				"RailingSegment" },
	{ API_RailingNodeID,				"RailingNode" },
	{ API_RailingBalusterSetID,			"RailingBalusterSet" },
	{ API_RailingPatternID,				"RailingPattern" },
	{ API_RailingToprailEndID,			"RailingToprailEnd" },
	{ API_RailingHandrailEndID,			"RailingHandrailEnd" },
	{ API_RailingRailEndID,				"RailingRailEnd" },
	{ API_RailingToprailConnectionID,	"RailingToprailConnection" },
	{ API_RailingHandrailConnectionID,	"RailingHandrailConnection" },
	{ API_RailingRailConnectionID,		"RailingRailConnection" },
	{ API_RailingEndFinishID,			"RailingEndFinish" },
	{ API_BeamSegmentID,				"BeamSegment" },
	{ API_ColumnSegmentID,				"ColumnSegment" },
	{ API_OpeningID,					"Opening" },
};

struct AttrTypeEntry {
	API_AttrTypeID	id;
	const char*		name;
};

const AttrTypeEntry kAttrTypes[] = {
	{ API_PenID,				"Pen" },
	{ API_LayerID,				"Layer" },
	{ API_LinetypeID,			"Line" },
	{ API_FilltypeID,			"Fill" },
	{ API_CompWallID,			"Composite" },
	{ API_MaterialID,			"Surface" },
	{ API_LayerCombID,			"LayerCombination" },
	{ API_ZoneCatID,			"ZoneCategory" },
	{ API_FontID,				"Font" },
	{ API_ProfileID,			"Profile" },
	{ API_PenTableID,			"PenTable" },
	{ API_DimStandID,			"DimensionStandard" },
	{ API_ModelViewOptionsID,	"ModelViewOption" },
	{ API_MEPSystemID,			"MEPSystem" },
	{ API_OperationProfileID,	"OperationProfile" },
	{ API_BuildingMaterialID,	"BuildingMaterial" },
};

const AttrTypeEntry kAttrAliases[] = {
	{ API_LinetypeID,			"LineType" },
	{ API_LinetypeID,			"Linetype" },
	{ API_FilltypeID,			"FillType" },
	{ API_FilltypeID,			"Filltype" },
	{ API_FilltypeID,			"Hatch" },
	{ API_CompWallID,			"CompositeStructure" },
	{ API_MaterialID,			"Material" },
	{ API_LayerCombID,			"LayerComb" },
	{ API_ZoneCatID,			"ZoneCat" },
	{ API_ProfileID,			"ComplexProfile" },
	{ API_ModelViewOptionsID,	"ModelViewOptions" },
	{ API_DimStandID,			"DimStandard" },
};

} // namespace


GS::UniString ElemTypeName (API_ElemTypeID typeID)
{
	for (const auto& e : kElemTypes) {
		if (e.id == typeID)
			return e.name;
	}
	if (typeID == API_ExternalElemID)
		return "External";
	GS::UniString s;
	s.Printf ("Unknown(%d)", (int) typeID);
	return s;
}


GS::UniString ElemTypeName (const API_ElemType& type)
{
	return ElemTypeName (type.typeID);
}


std::optional<API_ElemTypeID> ParseElemType (const GS::UniString& name)
{
	for (const auto& e : kElemTypes) {
		if (EqualsIgnoreCase (name, e.name))
			return e.id;
	}
	if (EqualsIgnoreCase (name, "Polyline"))	return API_PolyLineID;
	if (EqualsIgnoreCase (name, "Fill"))		return API_HatchID;
	if (EqualsIgnoreCase (name, "Section"))		return API_CutPlaneID;
	return std::nullopt;
}


API_ElemTypeID GetElemType (const OS& os, const char* key)
{
	GS::UniString name = GetString (os, key);
	auto t = ParseElemType (name);
	if (!t.has_value ())
		Fail ("Unknown element type '" + name + "'.");
	return *t;
}


const GS::Array<API_ElemTypeID>& AllElemTypes ()
{
	static GS::Array<API_ElemTypeID> all = [] {
		GS::Array<API_ElemTypeID> a;
		for (const auto& e : kElemTypes)
			a.Push (e.id);
		return a;
	} ();
	return all;
}


GS::UniString AttrTypeName (API_AttrTypeID typeID)
{
	for (const auto& e : kAttrTypes) {
		if (e.id == typeID)
			return e.name;
	}
	return "Unknown";
}


std::optional<API_AttrTypeID> ParseAttrType (const GS::UniString& name)
{
	for (const auto& e : kAttrTypes) {
		if (EqualsIgnoreCase (name, e.name))
			return e.id;
	}
	for (const auto& e : kAttrAliases) {
		if (EqualsIgnoreCase (name, e.name))
			return e.id;
	}
	return std::nullopt;
}


API_AttrTypeID GetAttrType (const OS& os, const char* key)
{
	GS::UniString name = GetString (os, key);
	auto t = ParseAttrType (name);
	if (!t.has_value ())
		Fail ("Unknown attribute type '" + name + "'. Use one of: Pen, Layer, Line, Fill, Composite, Surface, LayerCombination, ZoneCategory, Font, Profile, PenTable, DimensionStandard, ModelViewOption, MEPSystem, OperationProfile, BuildingMaterial.");
	return *t;
}


const GS::Array<API_AttrTypeID>& AllAttrTypes ()
{
	static GS::Array<API_AttrTypeID> all = [] {
		GS::Array<API_AttrTypeID> a;
		for (const auto& e : kAttrTypes)
			a.Push (e.id);
		return a;
	} ();
	return all;
}


// Frees the data ACAPI_Attribute_Get allocates inside some attribute types.
static void ReleaseAttributeData (API_AttrTypeID typeID, API_Attribute& attr)
{
	if (typeID == API_MaterialID && attr.material.texture.fileLoc != nullptr) {
		delete attr.material.texture.fileLoc;
		attr.material.texture.fileLoc = nullptr;
	}
	if (typeID == API_ModelViewOptionsID && attr.modelViewOpt.modelViewOpt.gdlOptions != nullptr)
		ACAPI_FreeGDLModelViewOptionsPtr (&attr.modelViewOpt.modelViewOpt.gdlOptions);
}


GS::UniString AttrName (API_AttrTypeID typeID, API_AttributeIndex index)
{
	API_Attribute attr = {};
	GS::UniString name;
	attr.header.typeID = typeID;
	attr.header.index = index;
	attr.header.uniStringNamePtr = &name;
	if (ACAPI_Attribute_Get (&attr) != NoError)
		return GS::UniString ();
	ReleaseAttributeData (typeID, attr);
	return name;
}


bool AttrExists (API_AttrTypeID typeID, API_AttributeIndex index)
{
	API_Attr_Head head = {};
	head.typeID = typeID;
	head.index = index;
	API_Attribute attr = {};
	attr.header = head;
	GSErrCode err = ACAPI_Attribute_Get (&attr);
	if (err == NoError)
		ReleaseAttributeData (typeID, attr);
	return err == NoError;
}


std::optional<API_AttributeIndex> FindAttrByName (API_AttrTypeID typeID, const GS::UniString& name)
{
	{
		API_Attr_Head head = {};
		GS::UniString searchName = name;
		head.typeID = typeID;
		head.uniStringNamePtr = &searchName;
		if (ACAPI_Attribute_Search (&head) == NoError)
			return head.index;
	}

	// Fallback: case-insensitive scan.
	API_AttributeIndex count = 0;
	if (ACAPI_Attribute_GetNum (typeID, &count) != NoError)
		return std::nullopt;
	for (API_AttributeIndex i = 1; i <= count; ++i) {
		GS::UniString n = AttrName (typeID, i);
		if (!n.IsEmpty () && EqualsIgnoreCase (n, name))
			return i;
	}
	return std::nullopt;
}


static std::optional<API_AttributeIndex> FindAttrByGuid (API_AttrTypeID typeID, const API_Guid& guid)
{
	API_AttributeIndex count = 0;
	if (ACAPI_Attribute_GetNum (typeID, &count) != NoError)
		return std::nullopt;
	for (API_AttributeIndex i = 1; i <= count; ++i) {
		API_Attribute attr = {};
		attr.header.typeID = typeID;
		attr.header.index = i;
		if (ACAPI_Attribute_Get (&attr) == NoError) {
			ReleaseAttributeData (typeID, attr);
			if (attr.header.guid == guid)
				return i;
		}
	}
	return std::nullopt;
}


std::optional<API_AttributeIndex> OptAttr (API_AttrTypeID typeID, const OS& os, const char* key)
{
	if (!os.Contains (key))
		return std::nullopt;

	const GS::UniString typeName = AttrTypeName (typeID);

	if (IsNumber (os, key)) {
		Int32 idx = GetInt (os, key);
		if (typeID != API_PenID && !AttrExists (typeID, idx))
			Fail (typeName + " attribute with index " + GS::ValueToUniString (idx) + " does not exist.", APIERR_BADINDEX);
		return idx;
	}
	if (os.IsString (key)) {
		GS::UniString name = GetString (os, key);
		auto idx = FindAttrByName (typeID, name);
		if (!idx.has_value ())
			Fail (typeName + " attribute named '" + name + "' not found. Use get_attributes to list available names.", APIERR_BADNAME);
		return idx;
	}
	if (os.IsObject (key)) {
		OS ref = GetObject (os, key);
		if (IsNumber (ref, "index"))
			return OptAttr (typeID, ref, "index");
		if (ref.Contains ("name"))
			return OptAttr (typeID, ref, "name");
		std::optional<API_Guid> guid;
		if (ref.Contains ("attributeId"))
			guid = GuidFromItem (GetObject (ref, "attributeId"));
		else if (ref.Contains ("guid"))
			guid = GetGuid (ref, "guid");
		if (guid.has_value ()) {
			auto idx = FindAttrByGuid (typeID, *guid);
			if (!idx.has_value ())
				Fail (typeName + " attribute with GUID " + GuidStr (*guid) + " not found.", APIERR_BADID);
			return idx;
		}
	}
	Fail ("Field '" + GS::UniString (key) + "' must be a " + typeName + " index, name, or {\"index\"|\"name\"|\"guid\"}.");
}


API_AttributeIndex GetAttr (API_AttrTypeID typeID, const OS& os, const char* key)
{
	auto idx = OptAttr (typeID, os, key);
	if (!idx.has_value ())
		Fail ("Missing required " + AttrTypeName (typeID) + " field '" + GS::UniString (key) + "'.");
	return *idx;
}


OS AttrRef (API_AttrTypeID typeID, API_AttributeIndex index)
{
	OS ref ("index", (Int32) index);
	if (typeID == API_PenID)
		return ref;
	API_Attribute attr = {};
	GS::UniString name;
	attr.header.typeID = typeID;
	attr.header.index = index;
	attr.header.uniStringNamePtr = &name;
	if (ACAPI_Attribute_Get (&attr) == NoError) {
		ReleaseAttributeData (typeID, attr);
		ref.Add ("name", name);
		ref.Add ("guid", GuidStr (attr.header.guid));
	}
	return ref;
}

// --- Stories -----------------------------------------------------------------

namespace {
	struct StoryData {
		short	firstStory = 0;
		short	lastStory = 0;
		short	actStory = 0;
		bool	skipNullFloor = false;
		GS::Array<API_StoryType> stories;
	};

	StoryData LoadStories ()
	{
		StoryData data;
		API_StoryInfo info = {};
		if (ACAPI_Environment (APIEnv_GetStorySettingsID, &info, nullptr) == NoError) {
			data.firstStory = info.firstStory;
			data.lastStory = info.lastStory;
			data.actStory = info.actStory;
			data.skipNullFloor = info.skipNullFloor;
			if (info.data != nullptr) {
				Int32 n = info.lastStory - info.firstStory + 1;
				for (Int32 i = 0; i < n; ++i)
					data.stories.Push ((*info.data)[i]);
				BMKillHandle (reinterpret_cast<GSHandle*> (&info.data));
			}
		}
		return data;
	}
}


short CurrentStoryIndex ()
{
	return LoadStories ().actStory;
}


static GSErrCode GoToStoryIndex (short index)
{
	API_StoryCmdType cmd;
	BNZeroMemory (&cmd, sizeof (cmd));
	cmd.action = APIStory_GoTo;
	cmd.index = index;
	GSErrCode err = ACAPI_Environment (APIEnv_ChangeStorySettingsID, &cmd, nullptr);
	if (err == APIERR_NEEDSUNDOSCOPE) {
		err = ACAPI_CallUndoableCommand ("Go to story (Claude)", [&] () -> GSErrCode {
			API_StoryCmdType c = cmd;
			return ACAPI_Environment (APIEnv_ChangeStorySettingsID, &c, nullptr);
		});
	}
	return err;
}


CurrentStoryScope::CurrentStoryScope (short floorInd)
{
	const short current = CurrentStoryIndex ();
	if (current == floorInd)
		return;
	Check (GoToStoryIndex (floorInd), GS::UniString::Printf ("Cannot make story %d the current story", (int) floorInd));
	previous = current;
}


CurrentStoryScope::~CurrentStoryScope ()
{
	if (previous.has_value ())
		GoToStoryIndex (*previous);		// best effort: never throw from a destructor
}


double StoryLevel (short floorInd)
{
	StoryData data = LoadStories ();
	for (const API_StoryType& s : data.stories) {
		if (s.index == floorInd)
			return s.level;
	}
	return 0.0;
}


GS::UniString StoryName (short floorInd)
{
	StoryData data = LoadStories ();
	for (const API_StoryType& s : data.stories) {
		if (s.index == floorInd)
			return GS::UniString (s.uName);
	}
	return GS::UniString ();
}


std::optional<short> OptStory (const OS& os, const char* key)
{
	if (!os.Contains (key))
		return std::nullopt;

	StoryData data = LoadStories ();
	auto checkIndex = [&] (Int32 idx) -> short {
		if (idx < data.firstStory || idx > data.lastStory) {
			GS::UniString msg;
			msg.Printf ("Story index %d is out of range [%d..%d]. Use get_stories / create_stories first.", (int) idx, (int) data.firstStory, (int) data.lastStory);
			Fail (msg, APIERR_BADINDEX);
		}
		return (short) idx;
	};

	if (IsNumber (os, key))
		return checkIndex (GetInt (os, key));

	GS::UniString name;
	if (os.IsString (key)) {
		name = GetString (os, key);
	} else if (os.IsObject (key)) {
		OS ref = GetObject (os, key);
		if (IsNumber (ref, "index"))
			return checkIndex (GetInt (ref, "index"));
		if (IsNumber (ref, "floorId")) {
			const Int32 floorId = GetInt (ref, "floorId");
			for (const API_StoryType& s : data.stories) {
				if (s.floorId == floorId)
					return s.index;
			}
			Fail (GS::UniString::Printf ("No story with floorId %d. Use get_stories to list the stories.", (int) floorId), APIERR_BADID);
		}
		if (IsNumber (ref, "displayNumber")) {
			// Number shown in the Navigator: index + 1 for index >= 0 when "skip null floor" is on.
			const Int32 number = GetInt (ref, "displayNumber");
			for (const API_StoryType& s : data.stories) {
				const Int32 shown = (data.skipNullFloor && s.index >= 0) ? (Int32) s.index + 1 : (Int32) s.index;
				if (shown == number)
					return s.index;
			}
			Fail (GS::UniString::Printf ("No story with displayNumber %d. Use get_stories to list the stories.", (int) number), APIERR_BADINDEX);
		}
		name = GetString (ref, "name");
	} else {
		Fail ("Field '" + GS::UniString (key) + "' must be a story index, a story name, or {\"index\"|\"name\"|\"floorId\"|\"displayNumber\"}.");
	}
	for (const API_StoryType& s : data.stories) {
		if (EqualsIgnoreCase (GS::UniString (s.uName), name))
			return s.index;
	}
	Fail ("Story named '" + name + "' not found. Use get_stories to list the stories.", APIERR_BADNAME);
}

} // namespace cc
