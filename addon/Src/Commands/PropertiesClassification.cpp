// *****************************************************************************
// PropertiesClassification — classification system / item authoring.
// (Reading element classifications and classifying elements is done with the
// official JSON API commands, wrapped by the "official" tool family.)
//
//   CreateClassificationSystem   {name, editionVersion?, description?, source?, editionDate?, items?: [ItemSpec], reuseExisting?}
//   CreateClassificationItems    {system, parent?, items: [ItemSpec]}
//        ItemSpec = {id, name?, description?, parent?, before?, children?: [ItemSpec]}
//   ModifyClassificationSystem   {system, name?, editionVersion?, description?, source?, editionDate?}
//   ModifyClassificationItems    {system?, items: [{item, id?, name?, description?}]}
//   DeleteClassificationItems    {system?, items: [ref]}           (children are deleted too)
//   DeleteClassificationSystems  {systems: [ref]}
//   GetClassificationSystemItems {system, flat?}                   (tree with GUIDs, for authoring)
//   ImportClassificationsXml     {xml, systemConflictPolicy?, itemConflictPolicy?}
// *****************************************************************************

#include "Commands/PropertiesCommon.hpp"
#include "Core/Command.hpp"
#include "Core/Enums.hpp"

#include <functional>

namespace cc {
namespace props {

namespace {

OS ItemJson (const API_ClassificationItem& item)
{
	OS out ("guid", GuidStr (item.guid), "id", item.id);
	if (!item.name.IsEmpty ())
		out.Add ("name", item.name);
	if (!item.description.IsEmpty ())
		out.Add ("description", item.description);
	return out;
}


// "Root / Child / Item" path of an indexed item (IDs).
GS::UniString ItemPath (const ClassificationIndex& cls, const API_Guid& itemGuid)
{
	GS::UniString path;
	API_Guid current = itemGuid;
	for (Int32 guard = 0; guard < 128 && current != APINULLGuid; ++guard) {
		const ClassificationIndex::Item* it = cls.ItemByGuid (current);
		if (it == nullptr)
			break;
		path = path.IsEmpty () ? it->item.id : it->item.id + " / " + path;
		current = it->parent;
	}
	return path;
}


void CreateItemsTree (ClassificationIndex& cls, const API_ClassificationSystem& system, const API_Guid& parentGuid,
					  const GS::UniString& parentPath, const GS::Array<OS>& specs, GS::Array<OS>& results)
{
	for (const OS& spec : specs) {
		API_ClassificationItem created;
		GS::UniString path;
		OS r = Try ([&] () -> OS {
			API_ClassificationItem item;
			item.guid = APINULLGuid;
			item.id = Trimmed (GetString (spec, "id"));
			if (item.id.IsEmpty ())
				Fail ("Classification item IDs must not be empty.", APIERR_BADNAME);
			item.name = GetString (spec, "name", GS::UniString ());
			item.description = GetString (spec, "description", GS::UniString ());
			API_Guid parent = parentGuid;
			GS::UniString basePath = parentPath;
			if (spec.Contains ("parent")) {
				parent = cls.ResolveItem (ReadJScalar (spec, "parent"), &system).item.guid;
				basePath = ItemPath (cls, parent);
			}
			path = basePath.IsEmpty () ? item.id : basePath + " / " + item.id;
			API_Guid next = APINULLGuid;
			if (spec.Contains ("before")) {
				const ClassificationIndex::Item& before = cls.ResolveItem (ReadJScalar (spec, "before"), &system);
				if (before.parent != parent)
					Fail ("'before' item '" + before.item.id + "' is not a sibling (it has a different parent).");
				next = before.item.guid;
			}
			for (const ClassificationIndex::Item& existing : cls.Items ()) {
				if (existing.system == system.guid && EqualsIgnoreCase (Trimmed (existing.item.id), item.id))
					Fail ("Item ID '" + item.id + "' already exists in " + cls.SystemLabel (system.guid) + " (" + GuidStr (existing.item.guid) +
						  "). IDs must be unique within a system.", APIERR_NAMEALREADYUSED);
			}
			CheckWithHint (ACAPI_Classification_CreateClassificationItem (item, system.guid, parent, next),
						   "Cannot create classification item '" + item.id + "'");
			cls.AddItem (item, system.guid, parent);
			created = item;
			OS out = ItemJson (item);
			out.Add ("path", path);
			if (parent != APINULLGuid)
				out.Add ("parent", GuidStr (parent));
			return out;
		});
		if (r.Contains ("error")) {
			OS error;
			r.Get ("error", error);
			OS failed ("id", GetString (spec, "id", GS::UniString ()), "error", error);
			results.Push (failed);
			continue;	// children of a failed item are skipped
		}
		results.Push (r);
		if (spec.Contains ("children"))
			CreateItemsTree (cls, system, created.guid, path, GetObjectArray (spec, "children"), results);
	}
}


Int32 CountFailures (const GS::Array<OS>& results)
{
	Int32 n = 0;
	for (const OS& r : results)
		if (r.Contains ("error"))
			++n;
	return n;
}


void ApplySystemFields (API_ClassificationSystem& system, const OS& params)
{
	if (auto v = OptString (params, "name"))
		system.name = Trimmed (*v);
	if (auto v = OptString (params, "editionVersion"))
		system.editionVersion = Trimmed (*v);
	if (auto v = OptString (params, "description"))
		system.description = *v;
	if (auto v = OptString (params, "source"))
		system.source = *v;
	if (auto v = OptString (params, "editionDate"))
		system.editionDate = ParseDate (*v);
}


GS::Array<OS> BuildTree (const ClassificationIndex& cls, const API_Guid& systemGuid)
{
	GS::HashTable<API_Guid, GS::Array<UIndex>> children;	// parent GUID (APINULLGuid = root) -> item indices
	const GS::Array<ClassificationIndex::Item>& items = cls.Items ();
	for (UIndex i = 0; i < items.GetSize (); ++i) {
		if (items[i].system == systemGuid)
			children.Retrieve (items[i].parent).Push (i);
	}
	std::function<GS::Array<OS> (const API_Guid&, Int32)> build = [&] (const API_Guid& parent, Int32 depth) {
		GS::Array<OS> nodes;
		const GS::Array<UIndex>* list = children.GetPtr (parent);
		if (list == nullptr)
			return nodes;
		for (UIndex i : *list) {
			OS node = ItemJson (items[i].item);
			if (depth < 64) {
				GS::Array<OS> sub = build (items[i].item.guid, depth + 1);
				if (!sub.IsEmpty ())
					node.Add ("children", sub);
			}
			nodes.Push (node);
		}
		return nodes;
	};
	return build (APINULLGuid, 0);
}


const NamedValue kSystemPolicies[] = {
	{ "Merge",		API_MergeConflictingSystems },
	{ "Replace",	API_ReplaceConflictingSystems },
	{ "Skip",		API_SkipConflictingSystems },
};

const NamedValue kItemPolicies[] = {
	{ "Replace",	API_ReplaceConflictingItems },
	{ "Skip",		API_SkipConflicitingItems },
};

} // namespace


void RegisterClassificationAuthoringCommands ()
{
	RegisterCommand ("CreateClassificationSystem",
		"Creates a classification system (optionally with its item tree) in one undo step. Input: {name, editionVersion? (default "
		"'1.0'; name + version must be unique), description?, source?, editionDate? (YYYY-MM-DD, default today), items?: [{id, name?, "
		"description?, children?: [...]}], reuseExisting?: false}. Output: {system, items: [{guid, id, path} | {id, error}], created, failed}.",
		[] (const OS& params) -> OS {
			API_ClassificationSystem system;
			system.guid = APINULLGuid;
			system.name = Trimmed (GetString (params, "name"));
			system.editionVersion = Trimmed (GetString (params, "editionVersion", "1.0"));
			system.description = GetString (params, "description", GS::UniString ());
			system.source = GetString (params, "source", GS::UniString ());
			system.editionDate = params.Contains ("editionDate") ? ParseDate (GetString (params, "editionDate")) : Today ();
			if (system.name.IsEmpty () || system.editionVersion.IsEmpty ())
				Fail ("Classification systems need a non-empty name and editionVersion.", APIERR_BADNAME);
			const bool reuse = GetBool (params, "reuseExisting", false);
			const GS::Array<OS> itemSpecs = GetObjectArray (params, "items", false);

			bool existed = false;
			GS::Array<OS> itemResults;
			Undoable (GetString (params, "undoName", "Create classification system"), [&] () {
				ClassificationIndex cls;
				for (const API_ClassificationSystem& s : cls.Systems ()) {
					if (EqualsIgnoreCase (Trimmed (s.name), system.name) && EqualsIgnoreCase (Trimmed (s.editionVersion), system.editionVersion)) {
						if (!reuse)
							Fail ("Classification system '" + cls.SystemLabel (s.guid) + "' already exists (" + GuidStr (s.guid) +
								  "). Pass reuseExisting: true to add items to it, use create_classification_items, or change name/editionVersion.",
								  APIERR_NAMEALREADYUSED);
						system = s;
						existed = true;
					}
				}
				if (!existed) {
					CheckWithHint (ACAPI_Classification_CreateClassificationSystem (system),
								   "Cannot create classification system '" + system.name + "'");
					ACAPI_Classification_GetClassificationSystem (system);
					cls.Reload ();
				}
				if (!itemSpecs.IsEmpty ())
					CreateItemsTree (cls, system, APINULLGuid, GS::UniString (), itemSpecs, itemResults);
			});
			OS out ("system", ClassificationIndex::SystemJson (system));
			if (existed)
				out.Add ("alreadyExisted", true);
			const Int32 failed = CountFailures (itemResults);
			out.Add ("items", itemResults);
			out.Add ("created", (Int32) itemResults.GetSize () - failed);
			out.Add ("failed", failed);
			return out;
		});

	RegisterCommand ("CreateClassificationItems",
		"Adds classification items (a tree) to an existing system in one undo step. Input: {system: name|guid|{name, editionVersion}, "
		"parent?: item ref (default: root), items: [{id, name?, description?, parent?, before? (sibling to insert before), children?: "
		"[...]}]}. Item refs: GUID or item ID within the system. Output: {items: [{guid, id, path, parent?} | {id, error}], created, failed}.",
		[] (const OS& params) -> OS {
			const GS::Array<OS> itemSpecs = GetObjectArray (params, "items");
			GS::Array<OS> itemResults;
			API_ClassificationSystem system;
			Undoable (GetString (params, "undoName", "Create classification items"), [&] () {
				ClassificationIndex cls;
				system = cls.ResolveSystem (ReadJScalar (params, "system"));
				API_Guid parent = APINULLGuid;
				GS::UniString parentPath;
				if (params.Contains ("parent")) {
					const ClassificationIndex::Item& p = cls.ResolveItem (ReadJScalar (params, "parent"), &system);
					parent = p.item.guid;
					parentPath = ItemPath (cls, p.item.guid);
				}
				CreateItemsTree (cls, system, parent, parentPath, itemSpecs, itemResults);
			});
			const Int32 failed = CountFailures (itemResults);
			return OS ("system", ClassificationIndex::SystemJson (system), "items", itemResults,
					   "created", (Int32) itemResults.GetSize () - failed, "failed", failed);
		});

	RegisterCommand ("ModifyClassificationSystem",
		"Changes a classification system: {system, name?, editionVersion?, description?, source?, editionDate? (YYYY-MM-DD)}.",
		[] (const OS& params) -> OS {
			API_ClassificationSystem system;
			Undoable (GetString (params, "undoName", "Modify classification system"), [&] () {
				ClassificationIndex cls;
				system = cls.ResolveSystem (ReadJScalar (params, "system"));
				ApplySystemFields (system, params);
				if (system.name.IsEmpty () || system.editionVersion.IsEmpty ())
					Fail ("Classification systems need a non-empty name and editionVersion.", APIERR_BADNAME);
				CheckWithHint (ACAPI_Classification_ChangeClassificationSystem (system), "Cannot change classification system '" + system.name + "'");
				ACAPI_Classification_GetClassificationSystem (system);
			});
			return OS ("system", ClassificationIndex::SystemJson (system));
		});

	RegisterCommand ("ModifyClassificationItems",
		"Changes classification items in one undo step: {system? (scope for item IDs), items: [{item: GUID|ID|{system, id}, id?, name?, description?}]}.",
		[] (const OS& params) -> OS {
			const GS::Array<OS> specs = GetObjectArray (params, "items");
			GS::Array<OS> results;
			Undoable (GetString (params, "undoName", "Modify classification items"), [&] () {
				ClassificationIndex cls;
				const API_ClassificationSystem* scope = nullptr;
				API_ClassificationSystem scopeSystem;
				if (params.Contains ("system")) {
					scopeSystem = cls.ResolveSystem (ReadJScalar (params, "system"));
					scope = &scopeSystem;
				}
				for (const OS& spec : specs) {
					results.Push (Try ([&] () -> OS {
						API_ClassificationItem item = cls.ResolveItem (ReadJScalar (spec, "item"), scope).item;
						if (auto v = OptString (spec, "id")) {
							item.id = Trimmed (*v);
							if (item.id.IsEmpty ())
								Fail ("Classification item IDs must not be empty.", APIERR_BADNAME);
						}
						if (auto v = OptString (spec, "name"))
							item.name = *v;
						if (auto v = OptString (spec, "description"))
							item.description = *v;
						CheckWithHint (ACAPI_Classification_ChangeClassificationItem (item), "Cannot change classification item '" + item.id + "'");
						cls.UpdateItem (item);
						return ItemJson (item);
					}));
				}
			});
			return OS ("results", results);
		});

	RegisterCommand ("DeleteClassificationItems",
		"Deletes classification items (with all their children) in one undo step: {system? (scope for item IDs), items: [GUID|ID|{system, id}]}.",
		[] (const OS& params) -> OS {
			const JValue refs = ReadJValue (params, "items");
			if (!refs.present || refs.items.IsEmpty ())
				Fail ("Missing required array field 'items'.");
			GS::Array<OS> results;
			Undoable (GetString (params, "undoName", "Delete classification items"), [&] () {
				ClassificationIndex cls;
				const API_ClassificationSystem* scope = nullptr;
				API_ClassificationSystem scopeSystem;
				if (params.Contains ("system")) {
					scopeSystem = cls.ResolveSystem (ReadJScalar (params, "system"));
					scope = &scopeSystem;
				}
				GS::Array<API_Guid> deleted;
				for (const JScalar& ref : refs.items) {
					results.Push (Try ([&] () -> OS {
						const ClassificationIndex::Item it = cls.ResolveItem (ref, scope);
						if (deleted.Contains (it.item.guid))
							return OS ("guid", GuidStr (it.item.guid), "id", it.item.id, "deleted", true, "note", GS::UniString ("already deleted with its parent"));
						const GS::Array<API_Guid> descendants = cls.Descendants (it.item.guid);
						CheckWithHint (ACAPI_Classification_DeleteClassificationItem (it.item.guid), "Cannot delete classification item '" + it.item.id + "'");
						deleted.Push (it.item.guid);
						deleted.Append (descendants);
						return OS ("guid", GuidStr (it.item.guid), "id", it.item.id, "deleted", true, "deletedChildren", (Int32) descendants.GetSize ());
					}));
				}
			});
			return OS ("results", results);
		});

	RegisterCommand ("DeleteClassificationSystems",
		"Deletes classification systems (with all their items) in one undo step: {systems: [name|guid|{name, editionVersion}]}.",
		[] (const OS& params) -> OS {
			const JValue refs = ReadJValue (params, "systems");
			if (!refs.present || refs.items.IsEmpty ())
				Fail ("Missing required array field 'systems'.");
			GS::Array<OS> results;
			Undoable (GetString (params, "undoName", "Delete classification systems"), [&] () {
				ClassificationIndex cls;
				for (const JScalar& ref : refs.items) {
					results.Push (Try ([&] () -> OS {
						const API_ClassificationSystem system = cls.ResolveSystem (ref);
						const GS::UniString label = cls.SystemLabel (system.guid);
						CheckWithHint (ACAPI_Classification_DeleteClassificationSystem (system.guid), "Cannot delete classification system '" + label + "'");
						cls.Reload ();
						return OS ("guid", GuidStr (system.guid), "name", label, "deleted", true);
					}));
				}
			});
			return OS ("results", results);
		});

	RegisterCommand ("GetClassificationSystemItems",
		"Returns one classification system with its item tree (GUID, id, name, description, children) — or all systems without items "
		"when system is omitted. Input: {system?, flat?: false (true = flat list with path and depth)}.",
		[] (const OS& params) -> OS {
			ClassificationIndex cls;
			if (!params.Contains ("system")) {
				GS::Array<OS> systems;
				for (const API_ClassificationSystem& s : cls.Systems ()) {
					OS j = ClassificationIndex::SystemJson (s);
					j.Add ("itemCount", (Int32) cls.SystemItemGuids (s.guid).GetSize ());
					systems.Push (j);
				}
				return OS ("systems", systems);
			}
			const API_ClassificationSystem system = cls.ResolveSystem (ReadJScalar (params, "system"));
			OS out ("system", ClassificationIndex::SystemJson (system));
			if (GetBool (params, "flat", false)) {
				GS::Array<OS> items;
				for (const ClassificationIndex::Item& it : cls.Items ()) {
					if (it.system != system.guid)
						continue;
					OS j = ItemJson (it.item);
					j.Add ("depth", it.depth);
					if (it.parent != APINULLGuid)
						j.Add ("parent", GuidStr (it.parent));
					items.Push (j);
				}
				out.Add ("items", items);
			} else {
				out.Add ("items", BuildTree (cls, system.guid));
			}
			return out;
		});

	RegisterCommand ("ImportClassificationsXml",
		"Imports classification systems from Archicad's classification XML (Classification Manager export format). Input: {xml, "
		"systemConflictPolicy?: Merge|Replace|Skip (default Merge), itemConflictPolicy?: Replace|Skip (default Replace)}.",
		[] (const OS& params) -> OS {
			const GS::UniString xml = GetString (params, "xml");
			if (Trimmed (xml).IsEmpty ())
				Fail ("xml must not be empty.");
			const auto systemPolicy = params.Contains ("systemConflictPolicy")
				? (API_ClassificationSystemNameConflictResolutionPolicy) ParseNamed (kSystemPolicies, params, "systemConflictPolicy")
				: API_MergeConflictingSystems;
			const auto itemPolicy = params.Contains ("itemConflictPolicy")
				? (API_ClassificationItemNameConflictResolutionPolicy) ParseNamed (kItemPolicies, params, "itemConflictPolicy")
				: API_ReplaceConflictingItems;
			ClassificationIndex before;
			Undoable (GetString (params, "undoName", "Import classifications"), [&] () {
				CheckWithHint (ACAPI_Classification_Import (xml, systemPolicy, itemPolicy),
							   "Classification XML import failed (check that the XML is an Archicad classification export)");
			});
			ClassificationIndex after;
			GS::Array<OS> systems;
			for (const API_ClassificationSystem& s : after.Systems ()) {
				OS j = ClassificationIndex::SystemJson (s);
				j.Add ("new", before.SystemByGuid (s.guid) == nullptr);
				j.Add ("itemCount", (Int32) after.SystemItemGuids (s.guid).GetSize ());
				systems.Push (j);
			}
			return OS ("systems", systems);
		});
}

} // namespace props
} // namespace cc
