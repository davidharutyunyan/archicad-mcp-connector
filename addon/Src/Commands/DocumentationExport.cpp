// *****************************************************************************
// DocumentationExport — publishing and Archicad-native exports
// (family "documentation").
//
//   GetPublisherSets      {includeItems?}
//   PublishPublisherSet   {name | index, outputPath?, createFolders?, items?: [publisher item guid]}
//   ListIfcTranslators    {}
//   ExportIfc             {path, translator?, scope?, elements?, format?, storyIndex?, includeBoundingBoxGeometry?, overwrite?, createFolders?}
//   ExportPdf             {path, view? | layout? | database? | storyIndex?, paper?, restoreWindow?, overwrite?, createFolders?}
//                         or {exports: [{path, view? | layout? | ...}], ...common options}
//   ExportModule          {path, elements? | useSelection?, overwrite?, createFolders?}
// *****************************************************************************

#include "Commands/DocumentationShared.hpp"

#include <chrono>
#include <cmath>

namespace cc {
namespace doc {

namespace {

// =============================================================================
// Publisher sets
// =============================================================================

struct PublisherSet {
	Int32			index = 0;
	GS::UniString	name;			// Unicode name (root navigator item) or decoded set name
	GS::UniString	rawName;		// decoded char name of the set
	API_Guid		rootGuid = APINULLGuid;
	GS::UniString	path;			// LAN output folder (may be empty)
	GS::UniString	host;
	GS::UniString	directory;
};


GS::Array<PublisherSet> LoadPublisherSets ()
{
	GS::Array<PublisherSet> sets;
	Int32 n = 0;
	if (ACAPI_Navigator (APINavigator_GetNavigatorSetNumID, &n, nullptr) != NoError)
		return sets;
	for (Int32 i = 0; i < n; ++i) {
		API_NavigatorSet set;
		BNZeroMemory (&set, sizeof (set));
		set.mapId = API_PublisherSets;
		set.wantsExtraInfo = true;
		Int32 idx = i;
		const GSErrCode err = ACAPI_Navigator (APINavigator_GetNavigatorSetID, &set, &idx);
		PublisherSet ps;
		ps.index = i;
		if (err == NoError) {
			ps.rawName = ApiText (set.name);
			ps.rootGuid = set.rootGuid;
			if (set.path != nullptr)
				ps.path = LocationPath (*set.path);
			if (set.host != nullptr)
				ps.host = *set.host;
			if (set.dirName != nullptr)
				ps.directory = *set.dirName;
			API_NavigatorItem root;
			if (set.rootGuid != APINULLGuid && GetNavigatorItem (set.rootGuid, root))
				ps.name = GS::UniString (root.uName);
			if (ps.name.IsEmpty ())
				ps.name = ps.rawName;
		}
		delete set.host;
		delete set.dirName;
		delete set.path;
		delete set.twFileServerPath;
		if (err == NoError)
			sets.Push (ps);
	}
	return sets;
}


GS::Array<OS> PublisherItemsJson (const API_Guid& parent, Int32 depth)
{
	GS::Array<OS> list;
	if (depth > 32)
		return list;
	for (const API_NavigatorItem& item : NavigatorChildren (parent, API_PublisherSets)) {
		OS j = NavItemJson (item, false);
		GS::Array<OS> children = PublisherItemsJson (item.guid, depth + 1);
		if (!children.IsEmpty ())
			j.Add ("children", children);
		list.Push (j);
	}
	return list;
}


OS PublisherSetJson (const PublisherSet& ps, bool withItems)
{
	OS out;
	out.Add ("name", ps.name);
	if (ps.rawName != ps.name && !ps.rawName.IsEmpty ())
		out.Add ("setName", ps.rawName);
	out.Add ("index", ps.index);
	out.Add ("rootNavigatorItemGuid", GuidStr (ps.rootGuid));
	if (!ps.path.IsEmpty ())
		out.Add ("outputPath", ps.path);
	if (!ps.host.IsEmpty ())
		out.Add ("host", ps.host);
	if (!ps.directory.IsEmpty ())
		out.Add ("directory", ps.directory);
	if (withItems)
		out.Add ("items", PublisherItemsJson (ps.rootGuid, 0));
	return out;
}


const PublisherSet& FindPublisherSet (const GS::Array<PublisherSet>& sets, const OS& params)
{
	if (sets.IsEmpty ())
		Fail ("The project has no publisher sets. Create one in Archicad (Navigator > Publisher Sets).", APIERR_BADNAME);
	GS::UniString names;
	for (const PublisherSet& s : sets) {
		if (!names.IsEmpty ())
			names += "; ";
		names += "'" + s.name + "'";
	}
	if (auto idx = OptInt (params, "index")) {
		for (const PublisherSet& s : sets) {
			if (s.index == *idx)
				return s;
		}
		Fail (GS::UniString::Printf ("No publisher set with index %d. Sets: ", (int) *idx) + names + ".");
	}
	const GS::UniString name = GetString (params, "name");
	for (int pass = 0; pass < 2; ++pass) {
		for (const PublisherSet& s : sets) {
			const GS::UniString alt (s.rawName);
			const bool hit = pass == 0 ? (s.name == name || alt == name) : (EqualsIgnoreCase (s.name, name) || EqualsIgnoreCase (alt, name));
			if (hit)
				return s;
		}
	}
	Fail ("No publisher set named '" + name + "'. Available: " + names + " (names are localized; call get_publisher_sets).", APIERR_BADNAME);
}


OS GetPublisherSets (const OS& params)
{
	const bool withItems = GetBool (params, "includeItems", false);
	GS::Array<OS> list;
	for (const PublisherSet& s : LoadPublisherSets ())
		list.Push (PublisherSetJson (s, withItems));
	return OS ("publisherSets", list, "count", (Int32) list.GetSize ());
}


OS PublishPublisherSet (const OS& params)
{
	const GS::Array<PublisherSet> sets = LoadPublisherSets ();
	const PublisherSet& set = FindPublisherSet (sets, params);

	GS::UniString outputFolder = set.path;
	std::unique_ptr<IO::Location> outLoc;
	if (auto p = OptString (params, "outputPath")) {
		outputFolder = PrepareOutputFolder (*p, GetBool (params, "createFolders", false), "outputPath");
		outLoc.reset (new IO::Location (ToLocation (outputFolder)));
	}

	GS::Array<API_Guid> items = GetGuidArray (params, "items", false);

	API_PublishPars pars;
	BNZeroMemory (&pars, sizeof (pars));
	pars.guid = set.rootGuid;
	pars.path = outLoc.get ();

	const Int64 start = NowEpochSeconds ();
	const auto t0 = std::chrono::steady_clock::now ();
	const GSErrCode err = ACAPI_Automate (APIDo_PublishID, &pars, items.IsEmpty () ? nullptr : &items);
	pars.path = nullptr;
	if (err != NoError) {
		GS::UniString hint;
		if (err == APIERR_REFUSEDCMD)
			hint = " Archicad refused (a dialog may be open, another operation running, or publishing is not allowed in this license/Teamwork role).";
		else if (outputFolder.IsEmpty ())
			hint = " The set has no local output folder (it may publish to BIMcloud/FTP); pass outputPath with an absolute folder.";
		else
			hint = " Check that the output folder '" + outputFolder + "' is writable.";
		Fail ("Publishing '" + set.name + "' failed: " + ErrorName (err) + "." + hint, err);
	}
	const double seconds = std::chrono::duration<double> (std::chrono::steady_clock::now () - t0).count ();

	OS out;
	out.Add ("published", true);
	out.Add ("publisherSet", PublisherSetJson (set, false));
	out.Add ("durationSeconds", seconds);
	if (!items.IsEmpty ())
		out.Add ("itemCount", (Int32) items.GetSize ());
	if (!outputFolder.IsEmpty ()) {
		out.Add ("outputFolder", outputFolder);
		GS::Array<OS> files = RecentFiles (outputFolder, start - 2, 500);
		out.Add ("files", files);
		out.Add ("fileCount", (Int32) files.GetSize ());
		if (files.IsEmpty ())
			out.Add ("warning", GS::UniString ("No new files were found in the output folder. The set may be empty, publish elsewhere "
											   "(BIMcloud/FTP, or a sub folder structure elsewhere), or Archicad may still be writing."));
	}
	return out;
}

// =============================================================================
// IFC
// =============================================================================

const NamedValue kIfcScopes[] = {
	{ "EntireProject",			API_EntireProject },
	{ "VisibleOnAllStories",	API_VisibleElementsOnAllStories },
	{ "AllOnCurrentStory",		API_AllElementsOnCurrentStorey },
	{ "VisibleOnCurrentStory",	API_VisibleElementsOnCurrentStorey },
	{ "Selection",				API_SelectedElementsOnly },
	{ "Elements",				API_FilteredElements },
};


OS ListIfcTranslators (const OS&)
{
	GS::Array<API_IFCTranslatorIdentifier> list;
	Check (ACAPI_IFC_GetIFCExportTranslatorsList (list), "Cannot list IFC export translators");
	GS::Array<OS> out;
	for (UIndex i = 0; i < list.GetSize (); ++i)
		out.Push (OS ("name", list[i].name, "index", (Int32) i, "isDefault", i == 0));
	return OS ("translators", out);
}


OS ExportIfc (const OS& params)
{
	GS::Array<API_IFCTranslatorIdentifier> translators;
	Check (ACAPI_IFC_GetIFCExportTranslatorsList (translators), "Cannot list IFC export translators");
	if (translators.IsEmpty ())
		Fail ("Archicad has no IFC export translator (File > Interoperability > IFC > IFC Translators).");

	UIndex chosen = 0;
	if (auto want = OptString (params, "translator")) {
		chosen = MaxUIndex;
		for (int pass = 0; pass < 3 && chosen == MaxUIndex; ++pass) {
			for (UIndex i = 0; i < translators.GetSize (); ++i) {
				const GS::UniString& n = translators[i].name;
				const bool hit = pass == 0 ? n == *want : pass == 1 ? EqualsIgnoreCase (n, *want) : n.Contains (*want, GS::UniString::CaseInsensitive);
				if (hit) {
					chosen = i;
					break;
				}
			}
		}
		if (chosen == MaxUIndex) {
			GS::UniString names;
			for (const API_IFCTranslatorIdentifier& t : translators)
				names += (names.IsEmpty () ? "'" : ", '") + t.name + "'";
			Fail ("No IFC translator named '" + *want + "'. Available: " + names + ".", APIERR_BADNAME);
		}
	}

	GS::UniString format = GetString (params, "format", GS::UniString ());
	GS::Array<GS::UniString> exts;
	exts.Push ("ifc");
	exts.Push ("ifcxml");
	const GS::UniString path = PrepareOutputPath (params, "path", exts, format.IsEmpty () ? GS::UniString ("ifc") : format.ToLowerCase ());
	if (format.IsEmpty ())
		format = FileExtension (path);
	if (!EqualsIgnoreCase (format, "ifc") && !EqualsIgnoreCase (format, "ifcxml"))
		Fail ("'format' must be ifc or ifcxml.");
	if (!EqualsIgnoreCase (format, FileExtension (path)))
		Fail ("The file extension does not match format '" + format + "'.");

	GS::Array<API_Guid> elements = GetGuidArray (params, "elements", false);
	API_ElementsToIfcExportID scope = elements.IsEmpty () ? API_EntireProject : API_FilteredElements;
	if (Has (params, "scope"))
		scope = (API_ElementsToIfcExportID) ParseNamed (kIfcScopes, params, "scope");
	if (scope == API_FilteredElements && elements.IsEmpty ())
		Fail ("scope 'Elements' needs 'elements' (element guids).");

	WindowRestorer restorer;
	restorer.Arm (true);
	const API_WindowInfo win = CurrentWindow ();
	if (auto story = OptStory (params, "storyIndex")) {
		OS t ("storyIndex", (Int32) *story);
		OpenTargetWindow (t);
	} else if (!IsModelWindow (win.typeID)) {
		API_WindowInfo plan;
		BNZeroMemory (&plan, sizeof (plan));
		plan.typeID = APIWind_FloorPlanID;
		SwitchToWindow (plan);
	}

	API_SavePars_Ifc pars {};
	pars.subType = EqualsIgnoreCase (format, "ifcxml") ? API_IFCXML : API_IFC;
	pars.translatorIdentifier = translators[chosen];
	pars.elementsToIfcExport = scope;
	pars.elementsSet = scope == API_FilteredElements ? &elements : nullptr;
	pars.includeBoundingBoxGeometry = GetBool (params, "includeBoundingBoxGeometry", false);

	IO::Location loc = ToLocation (path);
	API_FileSavePars fsp;
	BNZeroMemory (&fsp, sizeof (fsp));
	fsp.fileTypeID = APIFType_IfcFile;
	fsp.file = &loc;

	const auto t0 = std::chrono::steady_clock::now ();
	const GSErrCode err = ACAPI_Automate (APIDo_SaveID, &fsp, &pars);
	fsp.file = nullptr;
	if (err != NoError) {
		GS::UniString hint;
		if (err == APIERR_REFUSEDCMD)
			hint = " Archicad refused (demo/educational license, an open dialog, or another running operation).";
		else if (scope == API_SelectedElementsOnly)
			hint = " Nothing may be selected; select elements first or use scope EntireProject / Elements.";
		Fail ("IFC export to '" + path + "' failed: " + ErrorName (err) + "." + hint, err);
	}
	restorer.Finish ();

	OS out;
	out.Add ("file", FileJson (path));
	out.Add ("translator", translators[chosen].name);
	out.Add ("scope", NameOf (kIfcScopes, scope));
	out.Add ("format", format.ToLowerCase ());
	if (scope == API_FilteredElements)
		out.Add ("elementCount", (Int32) elements.GetSize ());
	out.Add ("durationSeconds", std::chrono::duration<double> (std::chrono::steady_clock::now () - t0).count ());
	return out;
}

// =============================================================================
// PDF
// =============================================================================

OS ExportOnePdf (const OS& item, const OS& common)
{
	const bool overwrite = GetBool (item, "overwrite", GetBool (common, "overwrite", false));
	const bool createFolders = GetBool (item, "createFolders", GetBool (common, "createFolders", false));
	const bool restore = GetBool (item, "restoreWindow", GetBool (common, "restoreWindow", true));
	OS pathSpec ("path", GetString (item, "path"), "overwrite", overwrite, "createFolders", createFolders);
	GS::Array<GS::UniString> exts;
	exts.Push ("pdf");
	const GS::UniString path = PrepareOutputPath (pathSpec, "path", exts, "pdf");

	WindowRestorer restorer;
	restorer.Arm (restore);
	TargetWindow tw = OpenTargetWindow (item);

	OS paper;
	const bool hasPaper = TryGetObject (item, "paper", paper) || TryGetObject (common, "paper", paper);
	API_SavePars_Pdf pdf;
	BNZeroMemory (&pdf, sizeof (pdf));
	bool fromLayout = false;
	if (tw.window.typeID == APIWind_LayoutID || tw.window.typeID == APIWind_MasterLayoutID) {
		API_LayoutInfo li;
		BNZeroMemory (&li, sizeof (li));
		API_DatabaseUnId unId = tw.window.databaseUnId;
		if (ACAPI_Environment (APIEnv_GetLayoutSetsID, &li, &unId, nullptr) == NoError && li.sizeX > 1.0 && li.sizeY > 1.0) {
			pdf.sizeX = (float) li.sizeX;
			pdf.sizeY = (float) li.sizeY;
			fromLayout = true;
		}
		delete li.customData;
	}
	if (!fromLayout) {
		pdf.sizeX = 420.0f;		// A3 landscape
		pdf.sizeY = 297.0f;
		pdf.leftMargin = pdf.rightMargin = pdf.topMargin = pdf.bottomMargin = 10.0f;
	}
	if (hasPaper) {
		if (auto v = OptDouble (paper, "width"))	pdf.sizeX = (float) (*v * 1000.0);
		if (auto v = OptDouble (paper, "height"))	pdf.sizeY = (float) (*v * 1000.0);
		OS m;
		if (TryGetObject (paper, "margins", m)) {
			if (auto v = OptDouble (m, "left"))		pdf.leftMargin = (float) (*v * 1000.0);
			if (auto v = OptDouble (m, "top"))		pdf.topMargin = (float) (*v * 1000.0);
			if (auto v = OptDouble (m, "right"))	pdf.rightMargin = (float) (*v * 1000.0);
			if (auto v = OptDouble (m, "bottom"))	pdf.bottomMargin = (float) (*v * 1000.0);
		} else if (auto v = OptDouble (paper, "margin")) {
			pdf.leftMargin = pdf.topMargin = pdf.rightMargin = pdf.bottomMargin = (float) (*v * 1000.0);
		}
	}
	if (pdf.sizeX < 10.0f || pdf.sizeY < 10.0f)
		Fail ("paper width/height must be at least 0.01 m.");

	IO::Location loc = ToLocation (path);
	API_FileSavePars fsp;
	BNZeroMemory (&fsp, sizeof (fsp));
	fsp.fileTypeID = APIFType_PdfFile;
	fsp.file = &loc;
	const GSErrCode err = ACAPI_Automate (APIDo_SaveID, &fsp, &pdf);
	fsp.file = nullptr;
	if (err != NoError) {
		GS::UniString hint;
		if (err == APIERR_REFUSEDCMD)
			hint = " Archicad refused (demo license, an open dialog, or another running operation).";
		else if (!IsModelWindow (tw.window.typeID) && tw.window.typeID != APIWind_LayoutID && tw.window.typeID != APIWind_MasterLayoutID)
			hint = " The front window (" + DbTypeName (tw.window.typeID) + ") cannot be saved as PDF; pass view, layout or database.";
		Fail ("PDF export of " + tw.description + " failed: " + ErrorName (err) + "." + hint, err);
	}
	restorer.Finish ();

	OS out;
	out.Add ("file", FileJson (path));
	out.Add ("exported", tw.description);
	out.Add ("target", tw.json);
	OS p;
	p.Add ("width", pdf.sizeX / 1000.0);
	p.Add ("height", pdf.sizeY / 1000.0);
	OS m;
	m.Add ("left", pdf.leftMargin / 1000.0);
	m.Add ("top", pdf.topMargin / 1000.0);
	m.Add ("right", pdf.rightMargin / 1000.0);
	m.Add ("bottom", pdf.bottomMargin / 1000.0);
	p.Add ("margins", m);
	p.Add ("fromLayout", fromLayout && !hasPaper);
	out.Add ("paper", p);
	return out;
}


OS ExportPdf (const OS& params)
{
	if (!Has (params, "exports"))
		return ExportOnePdf (params, params);
	const GS::Array<OS> items = GetObjectArray (params, "exports");
	if (items.IsEmpty ())
		Fail ("'exports' must contain at least one {path, view|layout|database|storyIndex} item.");
	GS::Array<OS> results;
	for (const OS& item : items)
		results.Push (Try ([&] () -> OS { return ExportOnePdf (item, params); }));
	return OS ("results", results);
}

// =============================================================================
// Module (.mod)
// =============================================================================

OS ExportModule (const OS& params)
{
	GS::Array<GS::UniString> exts;
	exts.Push ("mod");
	const GS::Array<API_Guid> elements = GetGuidArray (params, "elements", false);
	const bool useSelection = GetBool (params, "useSelection", false);
	if (elements.IsEmpty () && !useSelection)
		Fail ("Pass 'elements' (guids) or useSelection: true (saves the current selection).");
	if (!elements.IsEmpty () && useSelection)
		Fail ("Pass either 'elements' or useSelection: true, not both.");
	const GS::UniString path = PrepareOutputPath (params, "path", exts, "mod");

	std::unique_ptr<DatabaseScope> scope;
	API_Elem_Head** heads = nullptr;
	if (!elements.IsEmpty ()) {
		API_Guid first = elements[0];
		API_DatabaseInfo db;
		BNZeroMemory (&db, sizeof (db));
		if (ACAPI_Database (APIDb_GetContainingDatabaseID, &first, &db) != NoError)
			Fail ("Element " + GuidStr (first) + " not found.", APIERR_BADID);
		if (db.typeID != APIWind_FloorPlanID && db.typeID != APIWind_SectionID && db.typeID != APIWind_DetailID &&
			db.typeID != APIWind_ElevationID && db.typeID != APIWind_InteriorElevationID && db.typeID != APIWind_WorksheetID)
			Fail ("Modules can only be saved from floor plan, section/elevation, detail or worksheet elements.");
		scope.reset (new DatabaseScope (db));
		heads = reinterpret_cast<API_Elem_Head**> (BMAllocateHandle ((GSSize) (elements.GetSize () * sizeof (API_Elem_Head)), ALLOCATE_CLEAR, 0));
		if (heads == nullptr)
			Fail ("Out of memory.", APIERR_MEMFULL);
		for (UIndex i = 0; i < elements.GetSize (); ++i) {
			API_Elem_Head h;
			BNZeroMemory (&h, sizeof (h));
			h.guid = elements[i];
			if (ACAPI_Element_GetHeader (&h) != NoError) {
				BMKillHandle (reinterpret_cast<GSHandle*> (&heads));
				Fail ("Element " + GuidStr (elements[i]) + " not found in the database of the first element (all elements must be in one database).", APIERR_BADID);
			}
			(*heads)[i] = h;
		}
	}

	IO::Location loc = ToLocation (path);
	const GSErrCode err = ACAPI_Automate (APIDo_SaveAsModuleFileID, &loc, heads);
	if (heads != nullptr)
		BMKillHandle (reinterpret_cast<GSHandle*> (&heads));
	if (err != NoError) {
		GS::UniString hint;
		if (err == APIERR_BADDATABASE)
			hint = " Open a floor plan / section / detail window first.";
		else if (err == APIERR_BADINDEX)
			hint = " None of the elements can be saved (or nothing is selected).";
		Fail ("Saving the module failed: " + ErrorName (err) + "." + hint, err);
	}
	OS out;
	out.Add ("file", FileJson (path));
	out.Add ("elementCount", (Int32) elements.GetSize ());
	out.Add ("source", GS::UniString (elements.IsEmpty () ? "selection" : "elements"));
	return out;
}

} // namespace


void RegisterDocumentationExportCommands ()
{
	RegisterCommand ("GetPublisherSets",
		"Lists publisher sets: name (localized), index, root navigator item guid, local output folder. Input: {includeItems?: bool "
		"(publisher item tree with navigator item guids, for publishing only some items)}.",
		[] (const OS& params) -> OS { return GetPublisherSets (params); });

	RegisterCommand ("PublishPublisherSet",
		"Publishes a publisher set (File > Publish). Input: {name | index, outputPath?: absolute folder (default: the set's own path), "
		"createFolders?: bool, items?: [publisher item navigator guid] (only these items)}. Output: {published, outputFolder, files "
		"[{path, sizeBytes}] written during the publish, durationSeconds}.",
		[] (const OS& params) -> OS { return PublishPublisherSet (params); });

	RegisterCommand ("ListIfcTranslators",
		"Lists the IFC export translators (localized names; the first one is Archicad's default).",
		[] (const OS& params) -> OS { return ListIfcTranslators (params); });

	RegisterCommand ("ExportIfc",
		"Saves the model as IFC. Input: {path (.ifc|.ifcxml), translator?: name (default: first), scope?: EntireProject|"
		"VisibleOnAllStories|AllOnCurrentStory|VisibleOnCurrentStory|Selection|Elements (default EntireProject, or Elements when "
		"'elements' is given), elements?: [guid], storyIndex? (for the current-story scopes), format?: ifc|ifcxml, "
		"includeBoundingBoxGeometry?, overwrite?, createFolders?}. Output: {file {path, sizeBytes}, translator, scope}.",
		[] (const OS& params) -> OS { return ExportIfc (params); });

	RegisterCommand ("ExportPdf",
		"Saves a window as PDF (File > Save As PDF). Input: {path, view? (navigator view guid) | layout? | database? | storyIndex? "
		"(default: front window), paper?: {width, height, margins?: {left, top, right, bottom} | margin} in meters (default: layout "
		"sheet size, else A3 landscape), restoreWindow?: bool (default true), overwrite?, createFolders?} or {exports: [items], "
		"...common options}.",
		[] (const OS& params) -> OS { return ExportPdf (params); });

	RegisterCommand ("ExportModule",
		"Saves elements as an Archicad module file (.mod, for hotlinks). Input: {path, elements?: [guid] | useSelection?: true, "
		"overwrite?, createFolders?}.",
		[] (const OS& params) -> OS { return ExportModule (params); });
}

} // namespace doc
} // namespace cc
