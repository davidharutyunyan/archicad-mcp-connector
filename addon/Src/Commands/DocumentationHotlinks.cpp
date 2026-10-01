// *****************************************************************************
// DocumentationHotlinks — hotlinked modules / XRefs and file merging
// (family "documentation").
//
//   GetHotlinks      {types?: ["Module"|"XRef"], includeInstances?, includeElementCounts?}
//   PlaceHotlinks    {hotlinks: [{source (file path) | node (hotlink node guid), name?, storyRange?, sourceStory?,
//                     reuseNode?, position?: {x, y, z?}, angle?, mirrored?, storyIndex?, floorDifference?, skipNested?,
//                     ignoreTopFloorLinks?, relinkWallOpenings?, adjustLevelDiffs?, suspendFixAngle?, layer?}], undoName?}
//   UpdateHotlinks   {nodes?: [node guid] | all?: true, relink?: [{node, source?, name?, storyRange?, sourceStory?}]}
//   DeleteHotlinks   {nodes: [node guid], keepElements?: bool (true = break: keep the elements as own elements)}
//   MergeFile        {path (.pln|.pla|.mod), position?, angle?, mirrored?, storyIndex?, storyRange?, sourceStory?,
//                     skipNested?, keepHotlink?, maxGuids?}
//
// Hotlink instances live in the floor plan database; every command switches to it
// temporarily. Positions are in meters (project coordinates), angles in degrees.
// *****************************************************************************

#include "Commands/DocumentationShared.hpp"

#include "GSTime.hpp"

#include <cmath>
#include <functional>
#include <map>

namespace cc {
namespace doc {

namespace {

// =============================================================================
// Enum tables / small helpers
// =============================================================================

const NamedValue kHotlinkTypes[] = {
	{ "Module",		APIHotlink_Module },
	{ "XRef",		APIHotlink_XRef },
};

const NamedValue kStoryRanges[] = {
	{ "SingleStory",	APIHotlink_SingleStory },
	{ "AllStories",		APIHotlink_AllStories },
};

const NamedValue kSourceTypes[] = {
	{ "LocalFile",			APIHotlink_LocalFile },
	{ "TeamworkFileServer",	APIHotlink_TWFS },
	{ "TeamworkProject",	APIHotlink_TWProject },
};

const NamedValue kSourceStatus[] = {
	{ "AuthenticationCanceled",	API_HotlinkSource_AuthCanceld },
	{ "Missing",				API_HotlinkSource_SourceMissing },
	{ "Available",				API_HotlinkSource_SourceAvailable },
	{ "NotAccessible",			API_HotlinkSource_SourceNotAccessible },
	{ "Incompatible",			API_HotlinkSource_SourceIncompatible },
	{ "Offline",				API_HotlinkSource_SourceOffline },
};

constexpr Int32 kMaxNodeDepth = 32;


// ISO 8601 UTC ("2024-05-01T12:30:00Z"); empty for 0.
GS::UniString TimeString (GSTime t)
{
	if (t == 0)
		return GS::UniString ();
	GSTimeRecord rec;
	if (TIGetTimeRecord (t, &rec, TI_UTC_TIME) != NoError)
		return GS::UniString ();
	return GS::UniString::Printf ("%04u-%02u-%02uT%02u:%02u:%02uZ",
								  (unsigned) rec.year, (unsigned) rec.month, (unsigned) rec.day,
								  (unsigned) rec.hour, (unsigned) rec.minute, (unsigned) rec.second);
}


GS::UniString FileNameWithoutExtension (const GS::UniString& path)
{
	UIndex start = 0;
	for (UIndex i = 0; i < path.GetLength (); ++i) {
		if (path[i] == '/' || path[i] == '\\')
			start = i + 1;
	}
	GS::UniString name = path.GetSubstring (start, path.GetLength () - start);
	const UIndex dot = name.FindLast (GS::UniChar ('.'));
	if (dot != MaxUIndex && dot > 0)
		name = name.GetSubstring (0, dot);
	return name;
}


API_DatabaseInfo FloorPlanDatabase ()
{
	return ListDatabases (APIWind_FloorPlanID)[0];
}


// Fills node (must be zero-initialized; its destructor frees the locations Archicad allocates).
bool LoadNode (const API_Guid& guid, API_HotlinkNode& node)
{
	node.guid = guid;
	bool enableUnplaced = true;
	return ACAPI_Database (APIDb_GetHotlinkNodeID, &node, &enableUnplaced) == NoError;
}


std::optional<API_HotlinkSourceStatus> SourceStatus (const API_HotlinkNode& node)
{
	if (node.sourceLocation == nullptr)
		return std::nullopt;
	API_HotlinkSourceStatus status = API_HotlinkSource_SourceMissing;
	if (ACAPI_Database (APIDb_GetHotlinkSourceStatusID, node.sourceLocation, &status) != NoError)
		return std::nullopt;
	return status;
}


GS::Array<API_Guid> NodeInstances (const API_Guid& nodeGuid)
{
	GS::Array<API_Guid> instances;
	API_Guid g = nodeGuid;
	ACAPI_Database (APIDb_GetHotlinkInstancesID, &g, &instances);
	return instances;
}


OS NodeJson (const API_HotlinkNode& n)
{
	OS out ("nodeGuid", GuidStr (n.guid));
	out.Add ("name", GS::UniString (n.name));
	out.Add ("type", NameOf (kHotlinkTypes, n.type));
	out.Add ("sourceType", NameOf (kSourceTypes, n.sourceType));
	if (n.sourceLocation != nullptr)
		out.Add ("source", LocationPath (*n.sourceLocation));
	if (n.serverSourceLocation != nullptr)
		out.Add ("serverSource", LocationPath (*n.serverSourceLocation));
	if (auto status = SourceStatus (n))
		out.Add ("sourceStatus", NameOf (kSourceStatus, *status));
	if (n.type == APIHotlink_Module) {
		out.Add ("storyRange", NameOf (kStoryRanges, n.storyRangeType));
		if (n.storyRangeType == APIHotlink_SingleStory) {
			OS s ("index", (Int32) n.refFloorInd);
			const GS::UniString storyName (n.refFloorName);
			if (!storyName.IsEmpty ())
				s.Add ("name", storyName);
			out.Add ("sourceStory", s);
		}
	}
	const GS::UniString updated = TimeString (n.updateTime);
	if (!updated.IsEmpty ())
		out.Add ("updateTime", updated);
	if (n.fileSize > 0)
		out.Add ("fileSizeBytes", (double) n.fileSize);
	if (n.sourceType != APIHotlink_LocalFile)
		out.Add ("versionNumber", (double) n.versionNumber);
	if (n.ownerId != 0)
		out.Add ("ownedByAddOn", true);
	return out;
}


OS InstanceJson (const API_Element& e, bool withElementCount)
{
	const API_HotlinkType& h = e.hotlink;
	const API_Tranmat& t = h.transformation;
	OS out ("guid", GuidStr (e.header.guid));
	out.Add ("nodeGuid", GuidStr (h.hotlinkNodeGuid));
	out.Add ("type", NameOf (kHotlinkTypes, h.type));
	out.Add ("storyIndex", (Int32) e.header.floorInd);
	out.Add ("layer", AttrRef (API_LayerID, e.header.layer));
	out.Add ("position", Coord3DObj (t.tmx[3], t.tmx[7], t.tmx[11]));
	const bool mirrored = t.tmx[0] * t.tmx[5] - t.tmx[1] * t.tmx[4] < 0.0;
	AddAngle (out, "angle", mirrored ? std::atan2 (-t.tmx[4], -t.tmx[0]) : std::atan2 (t.tmx[4], t.tmx[0]));
	if (mirrored)
		out.Add ("mirrored", true);
	out.Add ("floorDifference", (Int32) h.floorDifference);
	out.Add ("skipNested", h.skipNested);
	out.Add ("ignoreTopFloorLinks", h.ignoreTopFloorLinks);
	out.Add ("relinkWallOpenings", h.relinkWallOpenings);
	out.Add ("adjustLevelDiffs", h.adjustLevelDiffs);
	out.Add ("suspendFixAngle", h.suspendFixAngle);
	if (h.hotlinkGroupGuid != APINULLGuid)
		out.Add ("groupGuid", GuidStr (h.hotlinkGroupGuid));
	if (e.header.hotlinkGuid != APINULLGuid)
		out.Add ("containingHotlinkGuid", GuidStr (e.header.hotlinkGuid));
	if (withElementCount) {
		API_Guid g = e.header.guid;
		GS::HashTable<API_Guid, API_Guid> proxies;
		if (ACAPI_Database (APIDb_GetHotlinkProxyElementTableID, &g, &proxies) == NoError)
			out.Add ("elementCount", (Int32) proxies.GetSize ());
	}
	return out;
}


API_Element GetHotlinkInstance (const API_Guid& guid)
{
	API_Element e = GetElement (guid);
	if (e.header.type.typeID != API_HotlinkID)
		Fail ("Element " + GuidStr (guid) + " is a " + ElemTypeName (e.header.type) + ", not a hotlink instance. Use get_hotlinks.", APIERR_BADELEMENTTYPE);
	return e;
}


// Top-level nodes (direct children of the root) of the given hotlink types.
GS::Array<API_Guid> TopLevelNodes (const GS::Array<API_HotlinkTypeID>& types)
{
	GS::Array<API_Guid> nodes;
	for (API_HotlinkTypeID type : types) {
		API_HotlinkTypeID t = type;
		API_Guid root = APINULLGuid;
		if (ACAPI_Database (APIDb_GetHotlinkRootNodeGuidID, &t, &root) != NoError || root == APINULLGuid)
			continue;
		GS::HashTable<API_Guid, GS::Array<API_Guid>> tree;
		if (ACAPI_Database (APIDb_GetHotlinkNodeTreeID, &root, &tree) != NoError)
			continue;
		if (const GS::Array<API_Guid>* kids = tree.GetPtr (root)) {
			for (const API_Guid& k : *kids)
				nodes.Push (k);
		}
	}
	return nodes;
}


GS::Array<API_Guid> AllModuleNodes ()
{
	GS::Array<API_Guid> nodes;
	API_HotlinkTypeID type = APIHotlink_Module;
	bool enableUnplaced = true;
	ACAPI_Database (APIDb_GetHotlinkNodesID, &type, &nodes, &enableUnplaced);
	return nodes;
}


bool SamePath (const GS::UniString& a, const GS::UniString& b)
{
	return a == b || EqualsIgnoreCase (a, b);		// macOS file systems are case-insensitive by default
}


// Existing module node with this source file (and, when checkSettings, the same story range settings).
API_Guid FindNodeBySource (const GS::UniString& path, bool checkSettings, API_HotlinkStoryRangeID range, short sourceStory)
{
	for (const API_Guid& g : AllModuleNodes ()) {
		API_HotlinkNode node {};
		if (!LoadNode (g, node) || node.sourceLocation == nullptr)
			continue;
		if (!SamePath (LocationPath (*node.sourceLocation), path))
			continue;
		if (checkSettings) {
			if (node.storyRangeType != range)
				continue;
			if (range == APIHotlink_SingleStory && node.refFloorInd != sourceStory)
				continue;
		}
		return g;
	}
	return APINULLGuid;
}


const GS::Array<GS::UniString>& ModuleExtensions ()
{
	static const GS::Array<GS::UniString> exts = { "pln", "pla", "mod" };
	return exts;
}


GS::UniString RequireModuleSource (const OS& spec, const char* key)
{
	const GS::UniString path = RequireInputFile (spec, key);
	const GS::UniString ext = FileExtension (path);
	if (!ModuleExtensions ().Contains (ext)) {
		GS::UniString hint;
		if (ext == "ifc" || ext == "ifczip" || ext == "ifcxml")
			hint = " IFC files cannot be merged or hotlinked through the Archicad 26 API: open/merge them in Archicad (File > Interoperability > Merge), or ask for a .pln/.mod export of that model.";
		else if (ext == "dwg" || ext == "dxf")
			hint = " DWG/DXF files cannot be merged or attached as XRef through the Archicad 26 API: use File > Interoperability > Merge or File > External Content > Attach XRef in Archicad.";
		else
			hint = " Use an Archicad project (.pln), archive (.pla) or module (.mod, see export_module) file.";
		Fail ("Unsupported source file type '." + ext + "' for '" + GS::UniString (key) + "'." + hint, APIERR_BADPARS);
	}
	return path;
}


struct NodeSettings {
	API_HotlinkStoryRangeID	range = APIHotlink_AllStories;
	short					sourceStory = 0;
};


NodeSettings ReadNodeSettings (const OS& spec)
{
	NodeSettings s;
	if (Has (spec, "storyRange"))
		s.range = (API_HotlinkStoryRangeID) ParseNamed (kStoryRanges, spec, "storyRange");
	if (auto v = OptInt (spec, "sourceStory")) {
		s.sourceStory = (short) *v;
		if (!Has (spec, "storyRange"))
			s.range = APIHotlink_SingleStory;
	}
	return s;
}


API_Guid CreateModuleNode (const GS::UniString& path, const GS::UniString& name, const NodeSettings& settings)
{
	API_HotlinkNode node {};
	node.type = APIHotlink_Module;
	node.storyRangeType = settings.range;
	node.refFloorInd = settings.range == APIHotlink_SingleStory ? settings.sourceStory : 0;
	node.sourceType = APIHotlink_LocalFile;
	node.sourceLocation = new IO::Location (ToLocation (path));		// freed by ~API_HotlinkNode
	CopyUniText (name.IsEmpty () ? FileNameWithoutExtension (path) : name, node.name, API_UniLongNameLen);
	const GSErrCode err = ACAPI_Database (APIDb_CreateHotlinkNodeID, &node);
	if (err != NoError) {
		Fail ("Cannot create a hotlink to '" + path + "': " + ErrorName (err) + ". The file must be a readable Archicad project (.pln), "
			  "archive (.pla) or module (.mod), must not be the current project itself, and in Teamwork you need the 'Hotlink and "
			  "XRef Management' right. For a single-story hotlink check that sourceStory exists in the source file.", err);
	}
	if (node.guid == APINULLGuid)
		Fail ("Archicad created the hotlink node but returned no guid.", APIERR_GENERAL);
	return node.guid;
}


// Builds the instance transformation from position / angle / mirrored; missing parts come from 'base'
// (the current transformation when modifying, identity when placing).
API_Tranmat MakeTransform (const OS& spec, const API_Tranmat* base = nullptr)
{
	API_Coord3D pos = { 0.0, 0.0, 0.0 };
	double baseAngle = 0.0;
	bool baseMirrored = false;
	if (base != nullptr) {
		pos = { base->tmx[3], base->tmx[7], base->tmx[11] };
		baseAngle = std::atan2 (base->tmx[4], base->tmx[0]);
		baseMirrored = base->tmx[0] * base->tmx[5] - base->tmx[1] * base->tmx[4] < 0.0;
		if (baseMirrored)		// the first column is (-cos, -sin) for a mirrored module
			baseAngle = std::atan2 (-base->tmx[4], -base->tmx[0]);
	}
	OS p;
	if (TryGetObject (spec, "position", p)) {
		pos.x = GetDouble (p, "x");
		pos.y = GetDouble (p, "y");
		pos.z = GetDouble (p, "z", base != nullptr ? pos.z : 0.0);
	} else if (Has (spec, "position")) {
		Fail ("'position' must be an object {x, y, z?} in meters.");
	}
	const double angle = GetAngle (spec, "angle", baseAngle);
	const bool mirrored = GetBool (spec, "mirrored", baseMirrored);
	const double co = std::cos (angle);
	const double si = std::sin (angle);
	const double mx = mirrored ? -1.0 : 1.0;		// mirror about the local Y axis, then rotate

	API_Tranmat t;
	BNZeroMemory (&t, sizeof (t));
	t.tmx[0] = co * mx;		t.tmx[1] = -si;		t.tmx[2] = 0.0;		t.tmx[3] = pos.x;
	t.tmx[4] = si * mx;		t.tmx[5] = co;		t.tmx[6] = 0.0;		t.tmx[7] = pos.y;
	t.tmx[8] = 0.0;			t.tmx[9] = 0.0;		t.tmx[10] = 1.0;	t.tmx[11] = pos.z;
	return t;
}


void ApplyInstanceFlags (API_HotlinkType& h, const OS& spec)
{
	if (auto v = OptBool (spec, "skipNested"))			h.skipNested = *v;
	if (auto v = OptBool (spec, "ignoreTopFloorLinks"))	h.ignoreTopFloorLinks = *v;
	if (auto v = OptBool (spec, "relinkWallOpenings"))	h.relinkWallOpenings = *v;
	if (auto v = OptBool (spec, "adjustLevelDiffs"))	h.adjustLevelDiffs = *v;
	if (auto v = OptBool (spec, "suspendFixAngle"))		h.suspendFixAngle = *v;
	if (auto v = OptInt (spec, "floorDifference"))		h.floorDifference = (short) *v;
}


// Creates one instance of a module node. Must run inside an undo scope with the floor plan current.
API_Guid PlaceInstance (const API_Guid& nodeGuid, const OS& spec)
{
	API_Element e = NewElement (API_HotlinkID);
	if (ACAPI_Element_GetDefaults (&e, nullptr) != NoError) {
		e = NewElement (API_HotlinkID);
		e.hotlink.ignoreTopFloorLinks = true;
	}
	e.header.type = API_ElemType (API_HotlinkID);
	e.header.guid = APINULLGuid;
	e.header.floorInd = CurrentStoryIndex ();
	if (e.header.layer == 0)
		e.header.layer = 1;		// the "Archicad" layer always exists
	ApplyCommonFields (e, nullptr, spec);
	e.hotlink.type = APIHotlink_Module;
	e.hotlink.hotlinkNodeGuid = nodeGuid;
	e.hotlink.hotlinkGroupGuid = APINULLGuid;
	e.hotlink.transformation = MakeTransform (spec);
	ApplyInstanceFlags (e.hotlink, spec);

	const GSErrCode err = ACAPI_Element_Create (&e, nullptr);
	if (err != NoError) {
		Fail ("Cannot place the hotlink instance: " + ErrorName (err) + ". Check storyIndex / layer; in Teamwork your role needs the "
			  "'Hotlink and XRef Management' access right (see get_teamwork_status).", err);
	}
	return e.header.guid;
}


OS NodeDetails (const API_Guid& guid)
{
	API_HotlinkNode node {};
	if (!LoadNode (guid, node))
		return OS ("nodeGuid", GuidStr (guid));
	return NodeJson (node);
}


API_Guid RequireNode (const GS::UniString& ref)
{
	const API_Guid g = ParseGuid (ref);
	API_HotlinkNode node {};
	if (!LoadNode (g, node)) {
		// Maybe an instance guid was passed.
		API_Element e;
		BNZeroMemory (&e, sizeof (e));
		e.header.guid = g;
		if (ACAPI_Element_Get (&e) == NoError && e.header.type.typeID == API_HotlinkID)
			return e.hotlink.hotlinkNodeGuid;
		Fail ("No hotlink node with guid " + ref + ". Call get_hotlinks to list nodes (nodeGuid) and instances.", APIERR_BADID);
	}
	return g;
}

// =============================================================================
// GetHotlinks
// =============================================================================

OS GetHotlinks (const OS& params)
{
	GS::Array<API_HotlinkTypeID> types;
	for (const GS::UniString& t : GetRefArray (params, "types"))
		types.Push ((API_HotlinkTypeID) ParseNamedString (kHotlinkTypes, t, "types"));
	if (types.IsEmpty ()) {
		types.Push (APIHotlink_Module);
		types.Push (APIHotlink_XRef);
	}
	const bool withInstances = GetBool (params, "includeInstances", true);
	const bool withCounts = GetBool (params, "includeElementCounts", false);

	DatabaseScope scope (FloorPlanDatabase ());

	GS::Array<OS> nodes;
	Int32 instanceTotal = 0;
	for (API_HotlinkTypeID type : types) {
		API_HotlinkTypeID t = type;
		API_Guid root = APINULLGuid;
		if (ACAPI_Database (APIDb_GetHotlinkRootNodeGuidID, &t, &root) != NoError || root == APINULLGuid)
			continue;
		GS::HashTable<API_Guid, GS::Array<API_Guid>> tree;
		if (ACAPI_Database (APIDb_GetHotlinkNodeTreeID, &root, &tree) != NoError)
			continue;

		std::function<void (const API_Guid&, const API_Guid&, Int32)> walk = [&] (const API_Guid& guid, const API_Guid& parent, Int32 depth) {
			if (depth > kMaxNodeDepth)
				return;
			OS j = Try ([&] () -> OS {
				API_HotlinkNode node {};
				if (!LoadNode (guid, node))
					Fail ("Cannot read hotlink node " + GuidStr (guid) + ".", APIERR_BADID);
				return NodeJson (node);
			});
			if (j.Contains ("error"))
				j.Add ("nodeGuid", GuidStr (guid));
			j.Add ("depth", depth);
			if (parent != APINULLGuid)
				j.Add ("parentNodeGuid", GuidStr (parent));
			const GS::Array<API_Guid>* kids = tree.GetPtr (guid);
			if (kids != nullptr && !kids->IsEmpty ()) {
				GS::Array<GS::UniString> childGuids;
				for (const API_Guid& k : *kids)
					childGuids.Push (GuidStr (k));
				j.Add ("childNodeGuids", childGuids);
			}
			const GS::Array<API_Guid> instances = NodeInstances (guid);
			j.Add ("instanceCount", (Int32) instances.GetSize ());
			instanceTotal += (Int32) instances.GetSize ();
			if (withInstances) {
				GS::Array<OS> list;
				for (const API_Guid& g : instances) {
					OS r = Try ([&] () -> OS { return InstanceJson (GetHotlinkInstance (g), withCounts); });
					if (r.Contains ("error") && !r.Contains ("guid"))
						r.Add ("guid", GuidStr (g));
					list.Push (r);
				}
				j.Add ("instances", list);
			}
			nodes.Push (j);
			if (kids != nullptr) {
				for (const API_Guid& k : *kids)
					walk (k, guid, depth + 1);
			}
		};

		if (const GS::Array<API_Guid>* top = tree.GetPtr (root)) {
			const GS::Array<API_Guid> topNodes = *top;
			for (const API_Guid& k : topNodes)
				walk (k, APINULLGuid, 0);
		}
	}
	return OS ("nodes", nodes, "nodeCount", (Int32) nodes.GetSize (), "instanceCount", instanceTotal);
}

// =============================================================================
// PlaceHotlinks
// =============================================================================

OS PlaceHotlinks (const OS& params)
{
	const GS::Array<OS> specs = GetObjectArray (params, "hotlinks");
	if (specs.IsEmpty ())
		Fail ("Pass at least one item in 'hotlinks' ({source: '/path/file.pln'} or {node: nodeGuid}).");

	struct Job { OS spec; GS::UniString source; API_Guid node = APINULLGuid; NodeSettings settings; bool ready = false; OS result;
				 API_Guid instance = APINULLGuid; bool nodeCreated = false; };
	GS::Array<Job> jobs;
	for (const OS& spec : specs) {
		Job job;
		job.spec = spec;
		job.result = Try ([&] () -> OS {
			const bool hasSource = Has (spec, "source");
			const bool hasNode = Has (spec, "node");
			if (hasSource == hasNode)
				Fail ("Each hotlink needs exactly one of 'source' (absolute path of a .pln/.pla/.mod file) or 'node' (existing hotlink node guid from get_hotlinks).");
			job.settings = ReadNodeSettings (spec);
			if (hasSource) {
				job.source = RequireModuleSource (spec, "source");
			} else {
				job.node = RequireNode (GetString (spec, "node"));
				if (Has (spec, "storyRange") || Has (spec, "sourceStory") || Has (spec, "name"))
					Fail ("storyRange / sourceStory / name belong to the hotlink node; with 'node' they cannot be changed here (use update_hotlinks relink).");
			}
			job.ready = true;
			return OS ();
		});
		jobs.Push (job);
	}

	DatabaseScope scope (FloorPlanDatabase ());
	Undoable (GetString (params, "undoName", "Place hotlinks (Claude)"), [&] () {
		for (Job& job : jobs) {
			if (!job.ready)
				continue;
			job.result = Try ([&] () -> OS {
				API_Guid node = job.node;
				if (node == APINULLGuid) {
					if (GetBool (job.spec, "reuseNode", true))
						node = FindNodeBySource (job.source, true, job.settings.range, job.settings.sourceStory);
					if (node == APINULLGuid) {
						node = CreateModuleNode (job.source, GetString (job.spec, "name", GS::UniString ()), job.settings);
						job.nodeCreated = true;
					}
				}
				try {
					job.instance = PlaceInstance (node, job.spec);
				} catch (const Error&) {
					if (job.nodeCreated) {		// do not leave an unplaced node behind
						API_Guid g = node;
						ACAPI_Database (APIDb_DeleteHotlinkNodeID, &g);
						job.nodeCreated = false;
					}
					throw;
				}
				job.node = node;
				return OS ();
			});
		}
	});

	GS::Array<OS> results;
	for (Job& job : jobs) {
		if (job.instance == APINULLGuid) {
			results.Push (job.result);
			continue;
		}
		OS r ("guid", GuidStr (job.instance));
		r.Add ("nodeGuid", GuidStr (job.node));
		r.Add ("nodeCreated", job.nodeCreated);
		OS inst = Try ([&] () -> OS { return InstanceJson (GetHotlinkInstance (job.instance), true); });
		if (!inst.Contains ("error"))
			r.Add ("instance", inst);
		r.Add ("node", NodeDetails (job.node));
		results.Push (r);
	}
	return OS ("results", results);
}

// =============================================================================
// UpdateHotlinks
// =============================================================================

OS UpdateHotlinks (const OS& params)
{
	const GS::Array<OS> relinks = GetObjectArray (params, "relink", false);
	const GS::Array<GS::UniString> nodeRefs = GetRefArray (params, "nodes", { "nodeGuid", "guid" });
	const bool all = GetBool (params, "all", false);
	if (relinks.IsEmpty () && nodeRefs.IsEmpty () && !all)
		Fail ("Pass 'nodes' (hotlink node guids from get_hotlinks), all: true, or 'relink' items.");

	DatabaseScope scope (FloorPlanDatabase ());

	GS::Array<OS> results;
	GS::Array<API_Guid> toUpdate;

	// 1) Relink / modify nodes (one undo step).
	if (!relinks.IsEmpty ()) {
		Undoable ("Relink hotlinks (Claude)", [&] () {
			for (const OS& spec : relinks) {
				OS r = Try ([&] () -> OS {
					const API_Guid g = RequireNode (GetString (spec, "node"));
					API_HotlinkNode node {};
					if (!LoadNode (g, node))
						Fail ("Cannot read hotlink node " + GuidStr (g) + ".", APIERR_BADID);
					if (node.type != APIHotlink_Module && (Has (spec, "storyRange") || Has (spec, "sourceStory")))
						Fail ("storyRange / sourceStory apply to hotlinked modules only.");
					bool changed = false;
					if (Has (spec, "source")) {
						const GS::UniString path = RequireModuleSource (spec, "source");
						delete node.sourceLocation;
						node.sourceLocation = new IO::Location (ToLocation (path));
						node.sourceType = APIHotlink_LocalFile;
						changed = true;
					}
					if (auto name = OptString (spec, "name")) {
						CopyUniText (*name, node.name, API_UniLongNameLen);
						changed = true;
					}
					if (Has (spec, "storyRange") || Has (spec, "sourceStory")) {
						const NodeSettings s = ReadNodeSettings (spec);
						node.storyRangeType = s.range;
						node.refFloorInd = s.range == APIHotlink_SingleStory ? s.sourceStory : 0;
						changed = true;
					}
					if (!changed)
						Fail ("Relink item for node " + GuidStr (g) + " changes nothing: pass source, name, storyRange or sourceStory.");
					const GSErrCode err = ACAPI_Database (APIDb_ModifyHotlinkNodeID, &node);
					if (err != NoError)
						Fail ("Cannot modify hotlink node '" + GS::UniString (node.name) + "': " + ErrorName (err) + ".", err);
					if (!toUpdate.Contains (g))
						toUpdate.Push (g);
					return OS ("nodeGuid", GuidStr (g), "relinked", true);
				});
				results.Push (r);
			}
		});
	}

	// 2) Nodes to refresh.
	for (const GS::UniString& ref : nodeRefs) {
		OS r = Try ([&] () -> OS {
			const API_Guid g = RequireNode (ref);
			if (!toUpdate.Contains (g))
				toUpdate.Push (g);
			return OS ();
		});
		if (r.Contains ("error")) {
			r.Add ("node", ref);
			results.Push (r);
		}
	}
	if (all) {
		GS::Array<API_HotlinkTypeID> types;
		types.Push (APIHotlink_Module);
		types.Push (APIHotlink_XRef);
		for (const API_Guid& g : TopLevelNodes (types)) {
			if (!toUpdate.Contains (g))
				toUpdate.Push (g);
		}
	}

	// 3) Update the caches.
	Int32 updated = 0;
	for (const API_Guid& g : toUpdate) {
		OS r = Try ([&] () -> OS {
			API_HotlinkNode node {};
			if (!LoadNode (g, node))
				Fail ("Cannot read hotlink node " + GuidStr (g) + ".", APIERR_BADID);
			const GS::UniString name (node.name);
			if (auto status = SourceStatus (node)) {
				if (*status != API_HotlinkSource_SourceAvailable) {
					Fail ("Hotlink '" + name + "': the source is " + NameOf (kSourceStatus, *status) + (node.sourceLocation != nullptr ? " ('" +
						  LocationPath (*node.sourceLocation) + "')" : GS::UniString ()) + ". Relink it: update_hotlinks {relink: [{node: '" +
						  GuidStr (g) + "', source: '/new/path.pln'}]}.", APIERR_BADPARS);
				}
			}
			API_Guid ng = g;
			const GSErrCode err = CallMaybeUndoable ("Update hotlink (Claude)", [&] () -> GSErrCode {
				return ACAPI_Database (APIDb_UpdateHotlinkCacheID, &ng);
			});
			if (err != NoError)
				Fail ("Updating hotlink '" + name + "' failed: " + ErrorName (err) + ". In Teamwork reserve the hotlink cache first: reserve_elements {objectSets: [\"HotlinkCacheManagement\"]}.", err);
			++updated;
			OS item ("nodeGuid", GuidStr (g), "updated", true);
			item.Add ("node", NodeDetails (g));
			return item;
		});
		if (r.Contains ("error") && !r.Contains ("nodeGuid"))
			r.Add ("nodeGuid", GuidStr (g));
		results.Push (r);
	}
	return OS ("results", results, "updatedCount", updated);
}

// =============================================================================
// DeleteHotlinks
// =============================================================================

OS DeleteHotlinks (const OS& params)
{
	const GS::Array<GS::UniString> refs = GetRefArray (params, "nodes", { "nodeGuid", "guid" });
	if (refs.IsEmpty ())
		Fail ("Pass the hotlink node guids to remove in 'nodes' (get_hotlinks).");
	const bool keepElements = GetBool (params, "keepElements", false);

	DatabaseScope scope (FloorPlanDatabase ());
	GS::Array<OS> results;
	Undoable (keepElements ? "Break hotlinks (Claude)" : "Delete hotlinks (Claude)", [&] () {
		for (const GS::UniString& ref : refs) {
			OS r = Try ([&] () -> OS {
				const API_Guid g = RequireNode (ref);
				GS::UniString name;
				{
					API_HotlinkNode node {};
					if (LoadNode (g, node))
						name = GS::UniString (node.name);
				}
				const Int32 instances = (Int32) NodeInstances (g).GetSize ();
				API_Guid ng = g;
				const GSErrCode err = ACAPI_Database (keepElements ? APIDb_BreakHotlinkNodeID : APIDb_DeleteHotlinkNodeID, &ng);
				if (err != NoError)
					Fail ("Cannot " + GS::UniString (keepElements ? "break" : "delete") + " hotlink '" + name + "': " + ErrorName (err) +
						  ". Nested hotlinks can only be removed together with their parent; in Teamwork reserve 'Hotlink and XRef Management'.", err);
				OS item ("nodeGuid", GuidStr (g), "name", name);
				item.Add (keepElements ? "broken" : "deleted", true);
				item.Add ("instanceCount", instances);
				return item;
			});
			if (r.Contains ("error") && !r.Contains ("nodeGuid"))
				r.Add ("node", ref);
			results.Push (r);
		}
	});
	return OS ("results", results);
}

// =============================================================================
// MergeFile (hotlink + break)
// =============================================================================

GS::HashSet<API_Guid> AllElementGuids ()
{
	GS::HashSet<API_Guid> set;
	GS::Array<API_Guid> list;
	if (ACAPI_Element_GetElemList (API_ElemType (API_ZombieElemID), &list) == NoError) {
		for (const API_Guid& g : list)
			set.Add (g);
	}
	return set;
}


OS MergeFile (const OS& params)
{
	const GS::UniString path = RequireModuleSource (params, "path");
	const bool keepHotlink = GetBool (params, "keepHotlink", false);
	const Int32 maxGuids = std::max<Int32> (0, std::min<Int32> (GetInt (params, "maxGuids", 500), 20000));
	const NodeSettings settings = ReadNodeSettings (params);

	DatabaseScope scope (FloorPlanDatabase ());

	const API_Guid existing = FindNodeBySource (path, false, settings.range, settings.sourceStory);
	if (existing != APINULLGuid) {
		Fail ("'" + path + "' is already hotlinked in this project (node " + GuidStr (existing) + ", " +
			  GS::ValueToUniString ((Int32) NodeInstances (existing).GetSize ()) + " instance(s)). Merging breaks the hotlink node and "
			  "would convert those instances too. Use place_hotlink {node} to add another instance, or break that hotlink with "
			  "delete_hotlinks {nodes: ['" + GuidStr (existing) + "'], keepElements: true}.", APIERR_BADPARS);
	}

	const GS::HashSet<API_Guid> before = AllElementGuids ();

	API_Guid node = APINULLGuid;
	API_Guid instance = APINULLGuid;
	Undoable (GetString (params, "undoName", "Merge file (Claude)"), [&] () {
		node = CreateModuleNode (path, GetString (params, "name", "Merge - " + FileNameWithoutExtension (path)), settings);
		try {
			instance = PlaceInstance (node, params);
		} catch (const Error&) {
			API_Guid g = node;
			ACAPI_Database (APIDb_DeleteHotlinkNodeID, &g);
			node = APINULLGuid;
			throw;
		}
	});

	OS out;
	out.Add ("source", FileJson (path));
	out.Add ("nodeGuid", GuidStr (node));
	OS inst = Try ([&] () -> OS { return InstanceJson (GetHotlinkInstance (instance), true); });
	if (!inst.Contains ("error"))
		out.Add ("hotlinkInstance", inst);

	if (keepHotlink) {
		out.Add ("merged", false);
		out.Add ("keptAsHotlink", true);
		out.Add ("note", GS::UniString ("The file was placed as a hotlinked module (keepHotlink: true). Break it later with delete_hotlinks "
										"{nodes: [nodeGuid], keepElements: true} to turn it into own elements."));
		return out;
	}

	// Safety: only break a node that has exactly our instance.
	const GS::Array<API_Guid> instances = NodeInstances (node);
	if (instances.GetSize () != 1 || instances[0] != instance) {
		out.Add ("merged", false);
		out.Add ("warning", GS::UniString ("Archicad attached the file to a hotlink node that has other instances, so it was NOT broken "
										   "(that would convert the other instances as well). The file stays placed as a hotlink."));
		return out;
	}

	OS breakResult = Try ([&] () -> OS {
		Undoable ("Merge file: break hotlink (Claude)", [&] () {
			API_Guid g = node;
			Check (ACAPI_Database (APIDb_BreakHotlinkNodeID, &g), "Cannot break the temporary hotlink");
		});
		return OS ();
	});
	if (breakResult.Contains ("error")) {
		out.Add ("merged", false);
		out.Add ("breakError", breakResult);
		out.Add ("warning", GS::UniString ("The file was placed as a hotlink but could not be broken into own elements. Undo in Archicad, "
										   "or retry with delete_hotlinks {nodes: [nodeGuid], keepElements: true}."));
		return out;
	}

	// New own elements = after - before.
	GS::Array<API_Guid> added;
	GS::Array<API_Guid> after;
	if (ACAPI_Element_GetElemList (API_ElemType (API_ZombieElemID), &after) == NoError) {
		for (const API_Guid& g : after) {
			if (!before.Contains (g))
				added.Push (g);
		}
	}
	std::map<std::string, Int32> byType;
	GS::Array<GS::UniString> guids;
	for (const API_Guid& g : added) {
		API_Elem_Head h;
		BNZeroMemory (&h, sizeof (h));
		h.guid = g;
		if (ACAPI_Element_GetHeader (&h) != NoError)
			continue;
		++byType[Utf8 (ElemTypeName (h.type))];
		if ((Int32) guids.GetSize () < maxGuids)
			guids.Push (GuidStr (g));
	}
	OS counts;
	for (const auto& kv : byType)
		counts.Add (GS::String (kv.first.c_str ()), kv.second);

	out.Add ("merged", true);
	out.Add ("elementCount", (Int32) added.GetSize ());
	out.Add ("countsByType", counts);
	out.Add ("elements", guids);
	if ((Int32) added.GetSize () > maxGuids)
		out.Add ("elementsTruncated", true);
	out.Add ("note", GS::UniString ("Merged by placing the file as a hotlinked module and breaking the hotlink (Archicad 26 has no merge API). "
									"Attributes of the source (layers, composites, surfaces ...) were added where missing; stories are matched "
									"by the hotlink story settings."));
	return out;
}

} // namespace


void RegisterDocumentationHotlinkCommands ()
{
	// Hotlink instances through the generic element tools (get_element_details / modify_elements).
	RegisterAdapter ({
		API_HotlinkID,
		nullptr,
		[] (const API_Element& e, OS& out) {
			out = InstanceJson (e, false);
			out.Add ("node", NodeDetails (e.hotlink.hotlinkNodeGuid));
		},
		[] (API_Element& e, API_Element& mask, API_ElementMemo&, UInt64&, const OS& patch) {
			if (Has (patch, "position") || Has (patch, "angle") || Has (patch, "mirrored")) {
				const API_Tranmat current = e.hotlink.transformation;
				e.hotlink.transformation = MakeTransform (patch, &current);
				ACAPI_ELEMENT_MASK_SET (mask, API_HotlinkType, transformation);
			}
			const API_HotlinkType before = e.hotlink;
			ApplyInstanceFlags (e.hotlink, patch);
			if (e.hotlink.skipNested != before.skipNested)					ACAPI_ELEMENT_MASK_SET (mask, API_HotlinkType, skipNested);
			if (e.hotlink.ignoreTopFloorLinks != before.ignoreTopFloorLinks)	ACAPI_ELEMENT_MASK_SET (mask, API_HotlinkType, ignoreTopFloorLinks);
			if (e.hotlink.relinkWallOpenings != before.relinkWallOpenings)	ACAPI_ELEMENT_MASK_SET (mask, API_HotlinkType, relinkWallOpenings);
			if (e.hotlink.adjustLevelDiffs != before.adjustLevelDiffs)		ACAPI_ELEMENT_MASK_SET (mask, API_HotlinkType, adjustLevelDiffs);
			if (e.hotlink.suspendFixAngle != before.suspendFixAngle)			ACAPI_ELEMENT_MASK_SET (mask, API_HotlinkType, suspendFixAngle);
			if (e.hotlink.floorDifference != before.floorDifference)			ACAPI_ELEMENT_MASK_SET (mask, API_HotlinkType, floorDifference);
			if (Has (patch, "node")) {
				e.hotlink.hotlinkNodeGuid = RequireNode (GetString (patch, "node"));
				ACAPI_ELEMENT_MASK_SET (mask, API_HotlinkType, hotlinkNodeGuid);
			}
		}
	});

	RegisterCommand ("GetHotlinks",
		"Lists hotlink nodes (sources: hotlinked modules and XRefs) as a tree with source path, source status (Available/Missing/...), "
		"story range, update time, and their placed instances (guid, story, position {x,y,z} m, angle deg, mirrored, options). Input: "
		"{types?: [\"Module\"|\"XRef\"], includeInstances?: bool (default true), includeElementCounts?: bool (default false)}.",
		[] (const OS& params) -> OS { return GetHotlinks (params); });

	RegisterCommand ("PlaceHotlinks",
		"Places hotlinked modules (one undo step). Input: {hotlinks: [{source: absolute .pln/.pla/.mod path | node: hotlink node guid, "
		"name?, storyRange?: AllStories|SingleStory, sourceStory?: int, reuseNode?: bool (default true), position?: {x, y, z?} m, "
		"angle?: deg, mirrored?, storyIndex?, floorDifference?, skipNested?, ignoreTopFloorLinks?, relinkWallOpenings?, "
		"adjustLevelDiffs?, suspendFixAngle?, layer?}]}. Output: results [{guid, nodeGuid, nodeCreated, instance, node} | {error}].",
		[] (const OS& params) -> OS { return PlaceHotlinks (params); });

	RegisterCommand ("UpdateHotlinks",
		"Refreshes hotlinks from their source files and/or relinks them. Input: {nodes?: [node guid] | all?: true, relink?: [{node, "
		"source?: new file path, name?, storyRange?, sourceStory?}]}. Output: results per node, updatedCount.",
		[] (const OS& params) -> OS { return UpdateHotlinks (params); });

	RegisterCommand ("DeleteHotlinks",
		"Removes hotlink nodes with all their instances (one undo step). Input: {nodes: [node guid], keepElements?: bool (true = break "
		"the link and keep the elements as own, editable elements; default false = delete them)}.",
		[] (const OS& params) -> OS { return DeleteHotlinks (params); });

	RegisterCommand ("MergeFile",
		"Merges an Archicad project/module file (.pln, .pla, .mod) into the current project as own elements (hotlink + break). "
		"Input: {path, position?: {x, y, z?} m, angle?: deg, mirrored?, storyIndex?, storyRange?, sourceStory?, skipNested?, "
		"keepHotlink?: bool (keep it hotlinked instead of breaking), maxGuids? (default 500)}. Output: {merged, elementCount, "
		"countsByType, elements: [guid]}.",
		[] (const OS& params) -> OS { return MergeFile (params); });
}

} // namespace doc
} // namespace cc
