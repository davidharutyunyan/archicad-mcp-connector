// *****************************************************************************
// Stories — story (floor) settings: list, insert, rename, change level/height,
// "show on sections", delete, and go to a story.
//
// Backed by APIEnv_GetStorySettingsID / APIEnv_ChangeStorySettingsID (API_StoryCmdType).
//
// Story JSON (all lengths in meters):
//   {index, displayNumber (number shown in the Navigator: index + 1 for index >= 0 when the
//    project skips story number 0, e.g. the Russian template), name,
//    level (elevation above Project Zero), height (to the next story; for the
//    top story when Archicad reports it), floorId (stable id, survives index shifts),
//    showOnSections, isCurrent, reservedByOtherUser (only when true, Teamwork),
//    elementCount / elementsByType (GetStories with includeElementCounts)}
//
// Story references accepted everywhere: index (integer), name (string, exact match first,
// then case-insensitive, then a numeric string as index), {index} | {name} | {floorId} | {displayNumber}.
//
// Commands: GetStories, CreateStories, ModifyStories, DeleteStories, SetCurrentStory.
//
// How Archicad 26 moves stories (measured live, and consistent with Tapir's notes on AC29):
//   * The current ("active") story is the anchor of the story ladder.
//   * APIStory_SetHeight (i, h) makes the gap between story i and i+1 equal to h by moving
//     the side of that boundary that is further from the anchor (as a rigid block).
//   * APIStory_InsAbove (ref, h) sets the height of *ref* to h; the new story gets ref's old
//     height. APIStory_InsBelow (X, h) (X above the ground floor) puts the new story at X's
//     level with height h and pushes X and the stories above up.
//   * APIStory_SetElevation silently does nothing for the top story.
// Because of that, every level change here is expressed as target levels for ALL stories and
// applied by ApplyLevels (anchor-aware SetHeight sweep, then verified).
//
// Undo: story settings commands are "non-undoable data structure modifiers" in the AC26 API
// and APIStory_Delete is a "complete operation" that is refused (APIERR_REFUSEDCMD) inside
// ACAPI_CallUndoableCommand. So every story command runs standalone (falling back to an undo
// scope only if Archicad asks for one), and a failure after the first change of an item is
// compensated explicitly (new story removed, previous levels/name restored).
// *****************************************************************************

#include "Commands/Commands.hpp"
#include "Core/Command.hpp"
#include "Core/Elements.hpp"
#include "Core/Enums.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

namespace cc {

namespace {

constexpr double	kLevelEps			= 1e-6;
constexpr double	kLevelTol			= 1e-5;		// two levels closer than this are equal
constexpr double	kVerifyTol			= 1e-4;		// accepted deviation after applying levels
constexpr double	kMinStoryHeight		= 0.001;	// smallest story height accepted
constexpr double	kDefaultStoryHeight	= 3.0;
constexpr double	kMaxPlausibleHeight	= 10000.0;
constexpr USize		kMaxBatch			= 200;

// --- Story snapshot ----------------------------------------------------------------

struct StoryRec {
	short					index = 0;
	short					floorId = 0;
	bool					notMine = false;
	bool					dispOnSections = false;
	double					level = 0.0;
	std::optional<double>	height;			// height to the next story (top story: only when Archicad reports it)
	GS::UniString			name;
};


struct StoryList {
	short					firstStory = 0;
	short					lastStory = 0;
	short					actStory = 0;
	bool					skipNullFloor = false;
	GS::Array<StoryRec>		stories;		// bottom -> top (index ascending)

	const StoryRec* FindIndex (Int32 index) const
	{
		for (const StoryRec& s : stories) {
			if (s.index == index)
				return &s;
		}
		return nullptr;
	}

	const StoryRec* FindFloorId (Int32 floorId) const
	{
		for (const StoryRec& s : stories) {
			if (s.floorId == floorId)
				return &s;
		}
		return nullptr;
	}

	// Position in 'stories' (bottom = 0), or -1.
	Int32 PosOfFloorId (Int32 floorId) const
	{
		for (UIndex i = 0; i < stories.GetSize (); ++i) {
			if (stories[i].floorId == floorId)
				return (Int32) i;
		}
		return -1;
	}

	const StoryRec* FindName (const GS::UniString& name) const
	{
		for (const StoryRec& s : stories) {
			if (s.name == name)
				return &s;
		}
		for (const StoryRec& s : stories) {
			if (EqualsIgnoreCase (s.name, name))
				return &s;
		}
		return nullptr;
	}

	const StoryRec* Below (const StoryRec& s) const	{ return FindIndex (s.index - 1); }
	const StoryRec* Above (const StoryRec& s) const	{ return FindIndex (s.index + 1); }
	const StoryRec& Top () const					{ return stories[stories.GetSize () - 1]; }
	const StoryRec& Bottom () const					{ return stories[0]; }

	// "Existing stories (index: 'name' @ level): -1: 'Basement' @ -3.000 m, 0: ..."
	GS::UniString Summary () const
	{
		GS::UniString s = "Existing stories (index: 'name' @ level): ";
		const USize maxListed = 40;
		for (UIndex i = 0; i < stories.GetSize () && i < maxListed; ++i) {
			if (i > 0)
				s += ", ";
			const int index = (int) stories[i].index;
			if (skipNullFloor && index >= 0)
				s += GS::UniString::Printf ("%d (shown as %d.): '", index, index + 1);
			else
				s += GS::UniString::Printf ("%d: '", index);
			s += stories[i].name + GS::UniString::Printf ("' @ %.3f m", stories[i].level);
		}
		if (stories.GetSize () > maxListed)
			s += GS::UniString::Printf (", ... (%u stories in total)", (unsigned) stories.GetSize ());
		s += ".";
		return s;
	}
};


GS::UniString NameFromBuffer (const GS::uchar_t (&buffer)[API_UniLongNameLen])
{
	GS::uchar_t copy[API_UniLongNameLen];
	std::memcpy (copy, buffer, sizeof (copy));
	copy[API_UniLongNameLen - 1] = 0;
	return GS::UniString (copy);
}


StoryList LoadStoryList ()
{
	API_StoryInfo info;
	BNZeroMemory (&info, sizeof (info));
	const GSErrCode err = ACAPI_Environment (APIEnv_GetStorySettingsID, &info, nullptr);

	struct HandleGuard {
		API_StoryType**& handle;
		~HandleGuard () { if (handle != nullptr) BMKillHandle (reinterpret_cast<GSHandle*> (&handle)); }
	} guard { info.data };

	Check (err, "Cannot read the story settings");
	if (info.data == nullptr)
		Fail ("Archicad returned no story data (is a project open?).", APIERR_GENERAL);

	const Int32 count = (Int32) info.lastStory - (Int32) info.firstStory + 1;
	const Int32 available = (Int32) (BMGetHandleSize (reinterpret_cast<GSConstHandle> (info.data)) / (GSSize) sizeof (API_StoryType));
	if (count <= 0 || available < count)
		Fail ("Archicad returned inconsistent story data.", APIERR_GENERAL);

	StoryList list;
	list.firstStory = info.firstStory;
	list.lastStory = info.lastStory;
	list.actStory = info.actStory;
	list.skipNullFloor = info.skipNullFloor;

	for (Int32 i = 0; i < count; ++i) {
		const API_StoryType& s = (*info.data)[i];
		StoryRec rec;
		rec.index = s.index;
		rec.floorId = s.floorId;
		rec.notMine = s.notMine;
		rec.dispOnSections = s.dispOnSections;
		rec.level = s.level;
		rec.name = NameFromBuffer (s.uName);
		// The handle normally holds one extra record above the top story whose level is
		// "top level + top story height"; use it only when it is plausible.
		if (i + 1 < available) {
			const double next = (*info.data)[i + 1].level;
			if (next > s.level + kLevelEps && next - s.level < kMaxPlausibleHeight)
				rec.height = next - s.level;
		}
		list.stories.Push (rec);
	}
	return list;
}


// Story number shown in the Navigator / Story Settings ("1." for index 0 when the project skips 0).
Int32 DisplayNumber (const StoryRec& s, const StoryList& list)
{
	return (list.skipNullFloor && s.index >= 0) ? (Int32) s.index + 1 : (Int32) s.index;
}


OS StoryToJson (const StoryRec& s, const StoryList& list)
{
	OS o;
	o.Add ("index", (Int32) s.index);
	o.Add ("displayNumber", DisplayNumber (s, list));
	o.Add ("name", s.name);
	o.Add ("level", s.level);
	if (s.height.has_value ())
		o.Add ("height", *s.height);
	o.Add ("floorId", (Int32) s.floorId);
	o.Add ("showOnSections", s.dispOnSections);
	o.Add ("isCurrent", s.index == list.actStory);
	if (s.notMine)
		o.Add ("reservedByOtherUser", true);
	return o;
}


GS::Array<OS> StoriesToJson (const StoryList& list)
{
	GS::Array<OS> arr;
	for (const StoryRec& s : list.stories)
		arr.Push (StoryToJson (s, list));
	return arr;
}


GS::UniString StoryLabel (const StoryRec& s)
{
	return GS::UniString::Printf ("story %d '", (int) s.index) + s.name + "'";
}


// Pre-existing stories whose level changed between two snapshots:
// [{floorId, index, name, levelBefore, level}] (stories missing in 'after' are skipped).
GS::Array<OS> LevelChanges (const StoryList& before, const StoryList& after)
{
	GS::Array<OS> changes;
	for (const StoryRec& b : before.stories) {
		const StoryRec* a = after.FindFloorId (b.floorId);
		if (a == nullptr || std::fabs (a->level - b.level) <= kVerifyTol)
			continue;
		OS o;
		o.Add ("floorId", (Int32) a->floorId);
		o.Add ("index", (Int32) a->index);
		o.Add ("name", a->name);
		o.Add ("levelBefore", b.level);
		o.Add ("level", a->level);
		changes.Push (o);
	}
	return changes;
}


void AddLevelChanges (OS& out, const StoryList& before, const StoryList& after)
{
	const GS::Array<OS> moved = LevelChanges (before, after);
	if (!moved.IsEmpty ())
		out.Add ("movedStories", moved);
}


// --- Story references ----------------------------------------------------------------

bool ParseIntString (const GS::UniString& str, Int32& value)
{
	GS::UniString trimmed = str;
	trimmed.Trim ();
	if (trimmed.IsEmpty ())
		return false;
	const GS::String s = ToStr (trimmed);
	const char* begin = s.ToCStr ();
	char* end = nullptr;
	const long v = std::strtol (begin, &end, 10);
	if (end == begin || *end != '\0')
		return false;
	value = (Int32) v;
	return true;
}


[[noreturn]] void FailIndex (const StoryList& list, Int32 index)
{
	Fail (GS::UniString::Printf ("Story index %d does not exist (valid range %d..%d). ", (int) index, (int) list.firstStory, (int) list.lastStory) +
		  list.Summary () + " Call get_stories to see the current stories.", APIERR_BADINDEX);
}


// Resolves os[key] (index | name | {index}|{name}|{floorId}|{displayNumber}) against the snapshot. Returns a copy.
StoryRec ResolveStory (const StoryList& list, const OS& os, const char* key)
{
	const GS::UniString keyName (key);
	if (!os.Contains (key))
		Fail ("Missing story reference '" + keyName + "' (story index, story name, or {floorId}). " + list.Summary ());

	if (IsNumber (os, key)) {
		const Int32 idx = GetInt (os, key);
		if (const StoryRec* s = list.FindIndex (idx))
			return *s;
		FailIndex (list, idx);
	}

	if (os.IsString (key)) {
		const GS::UniString name = GetString (os, key);
		if (const StoryRec* s = list.FindName (name))
			return *s;
		Int32 idx = 0;
		if (ParseIntString (name, idx)) {
			if (const StoryRec* s = list.FindIndex (idx))
				return *s;
			FailIndex (list, idx);
		}
		Fail ("Story named '" + name + "' not found. " + list.Summary () + " Story names are localized; call get_stories.", APIERR_BADNAME);
	}

	if (os.IsObject (key)) {
		const OS ref = GetObject (os, key);
		if (ref.Contains ("floorId")) {
			const Int32 floorId = GetInt (ref, "floorId");
			if (const StoryRec* s = list.FindFloorId (floorId))
				return *s;
			Fail (GS::UniString::Printf ("No story with floorId %d (it may have been deleted). ", (int) floorId) + list.Summary (), APIERR_BADID);
		}
		if (ref.Contains ("displayNumber")) {
			const Int32 number = GetInt (ref, "displayNumber");
			for (const StoryRec& s : list.stories) {
				if (DisplayNumber (s, list) == number)
					return s;
			}
			Fail (GS::UniString::Printf ("No story is shown with number %d. ", (int) number) + list.Summary (), APIERR_BADINDEX);
		}
		if (ref.Contains ("index"))
			return ResolveStory (list, ref, "index");
		if (ref.Contains ("name"))
			return ResolveStory (list, ref, "name");
		if (ref.Contains ("story"))
			return ResolveStory (list, ref, "story");
	}

	Fail ("Field '" + keyName + "' must be a story index (integer), a story name (string), or {\"index\"}|{\"name\"}|{\"floorId\"}|{\"displayNumber\"}.");
}


// Reads the target story of a command/patch from "story", falling back to top-level
// "index" / "floorId" (and "name" when allowNameKey, i.e. when "name" is not a field to change).
StoryRec ResolveTargetStory (const StoryList& list, const OS& spec, bool allowNameKey)
{
	if (spec.Contains ("story"))
		return ResolveStory (list, spec, "story");
	if (spec.Contains ("floorId") || spec.Contains ("index") || (allowNameKey && spec.Contains ("name"))) {
		OS ref;
		if (spec.Contains ("floorId"))
			ref.Add ("floorId", GetInt (spec, "floorId"));
		else if (spec.Contains ("index"))
			ref.Add ("index", GetInt (spec, "index"));
		else
			ref.Add ("name", GetString (spec, "name"));
		return ResolveStory (list, OS ("story", ref), "story");
	}
	Fail ("Missing 'story' (story index, story name, or {\"floorId\": n}). " + list.Summary ());
}


// --- Story commands ------------------------------------------------------------------

void SetCmdName (API_StoryCmdType& cmd, const GS::UniString& name)
{
	if (name.GetLength () >= API_UniLongNameLen)
		Fail (GS::UniString::Printf ("Story name is too long (%u characters, maximum %d).", (unsigned) name.GetLength (), (int) API_UniLongNameLen - 1));
	GS::ucsncpy (cmd.uName, name.ToUStr ().Get (), API_UniLongNameLen - 1);
	cmd.uName[API_UniLongNameLen - 1] = 0;
}


API_StoryCmdType NewCmd (API_StoryCmdID action, short index)
{
	API_StoryCmdType cmd;
	BNZeroMemory (&cmd, sizeof (cmd));
	cmd.action = action;
	cmd.index = index;
	return cmd;
}


void CheckStoryCmd (GSErrCode err, const GS::UniString& what)
{
	if (err == NoError)
		return;
	switch (err) {
		case APIERR_NOTMINE:
			Fail (what + ": the story settings are not reserved by you (Teamwork). Reserve 'Story Settings' in the Teamwork palette, then retry.", err);
		case APIERR_NEEDSUNDOSCOPE:
			Fail (what + ": Archicad requires an undo scope for this operation (internal add-on error).", err);
		case APIERR_BADINDEX:
			Fail (what + ": the story index is not valid any more. Call get_stories and retry.", err);
		case APIERR_BADNAME:
			Fail (what + ": Archicad rejected the story name (empty or invalid). Give a non-empty name.", err);
		default:
			Check (err, what);
	}
}


// Runs a story command standalone; only if Archicad insists on an undo scope, runs it in one.
GSErrCode ExecStoryCmd (API_StoryCmdType& cmd)
{
	GSErrCode err = ACAPI_Environment (APIEnv_ChangeStorySettingsID, &cmd, nullptr);
	if (err == APIERR_NEEDSUNDOSCOPE) {
		err = ACAPI_CallUndoableCommand ("Story settings (Claude)", [&] () -> GSErrCode {
			return ACAPI_Environment (APIEnv_ChangeStorySettingsID, &cmd, nullptr);
		});
	}
	return err;
}


void RunStoryCmd (API_StoryCmdType& cmd, const GS::UniString& what)
{
	CheckStoryCmd (ExecStoryCmd (cmd), what);
}


void SetStoryHeight (short index, double height, const GS::UniString& what)
{
	API_StoryCmdType cmd = NewCmd (APIStory_SetHeight, index);
	cmd.height = height;
	RunStoryCmd (cmd, what);
}


// Makes another story the current one (= the anchor that never moves) for the lifetime of
// the object and restores the previous current story (by floorId) afterwards.
class AnchorScope {
public:
	AnchorScope () = default;
	AnchorScope (const AnchorScope&) = delete;
	AnchorScope& operator= (const AnchorScope&) = delete;

	void MoveTo (short index, short restoreFloorId)
	{
		API_StoryCmdType cmd = NewCmd (APIStory_GoTo, index);
		cmd.dontRebuild = true;
		RunStoryCmd (cmd, "Cannot temporarily switch the current story");
		restore = restoreFloorId;
	}

	~AnchorScope ()
	{
		if (!restore.has_value ())
			return;
		try {
			const StoryList list = LoadStoryList ();
			if (const StoryRec* s = list.FindFloorId (*restore)) {
				API_StoryCmdType cmd = NewCmd (APIStory_GoTo, s->index);
				ExecStoryCmd (cmd);
			}
		} catch (...) {
			// never throw from a destructor
		}
	}

private:
	std::optional<short> restore;
};


// --- Level solver ----------------------------------------------------------------------

// Target levels by floorId; stories not listed keep their level.
using LevelTargets = std::map<short, double>;


GS::UniString LevelOrderProblem (const StoryList& list, const std::vector<double>& t)
{
	for (UIndex i = 1; i < list.stories.GetSize (); ++i) {
		if (t[i] - t[i - 1] < kMinStoryHeight) {
			return "the result would put " + StoryLabel (list.stories[i]) + GS::UniString::Printf (" at %.3f m, not above ", t[i]) +
				   StoryLabel (list.stories[i - 1]) + GS::UniString::Printf (" at %.3f m", t[i - 1]);
		}
	}
	return GS::UniString ();
}


std::vector<double> TargetVector (const StoryList& list, const LevelTargets& targets)
{
	std::vector<double> t;
	for (const StoryRec& s : list.stories) {
		auto it = targets.find (s.floorId);
		t.push_back (it != targets.end () ? it->second : s.level);
	}
	return t;
}


// Checks targets before anything is changed; throws a normal (per-item) error.
void ValidateTargets (const StoryList& list, const LevelTargets& targets, std::optional<double> topHeight)
{
	const GS::UniString problem = LevelOrderProblem (list, TargetVector (list, targets));
	if (!problem.IsEmpty ())
		Fail ("Invalid story levels: " + problem + ". Stories must stay in order with a height > 0. " + list.Summary ());
	if (topHeight.has_value () && *topHeight < kMinStoryHeight)
		Fail (GS::UniString::Printf ("height must be > 0 (got %.4f m).", *topHeight));
}


// Moves the stories to the target levels (and sets the top story's height) with SetHeight
// sweeps outward from the anchor (current story), then verifies the result. Throws cc::Error.
void ApplyLevels (const LevelTargets& targets, std::optional<double> topHeight)
{
	StoryList cur = LoadStoryList ();
	const UIndex n = cur.stories.GetSize ();
	const std::vector<double> t = TargetVector (cur, targets);
	{
		const GS::UniString problem = LevelOrderProblem (cur, t);
		if (!problem.IsEmpty ())
			Fail ("Cannot set the story levels: " + problem + ".");
	}

	auto differs = [&] (UIndex i) { return std::fabs (cur.stories[i].level - t[i]) > kLevelTol; };
	bool anyDiffers = false;
	for (UIndex i = 0; i < n; ++i)
		anyDiffers = anyDiffers || differs (i);

	if (anyDiffers) {
		AnchorScope anchorScope;
		Int32 anchorPos = 0;
		Int32 lowestDiff = -1;
		for (UIndex i = 0; i < n; ++i) {
			if (cur.stories[i].index == cur.actStory)
				anchorPos = (Int32) i;
			if (lowestDiff < 0 && differs (i))
				lowestDiff = (Int32) i;
		}

		// The anchor (current story) never moves. Best anchor: a story that stays where it is
		// and lies below every story that moves, so the sweep only has to move stories upwards.
		// All stories below the lowest moving one stay, so the one right below it qualifies.
		const bool anchorFixed = !differs ((UIndex) anchorPos);
		Int32 wanted = anchorPos;
		if (!(anchorFixed && anchorPos < lowestDiff)) {
			if (lowestDiff > 0) {
				wanted = lowestDiff - 1;
			} else if (!anchorFixed) {
				wanted = -1;		// nearest story that stays, if any
				for (Int32 d = 1; d < (Int32) n && wanted < 0; ++d) {
					if (anchorPos - d >= 0 && !differs ((UIndex) (anchorPos - d)))
						wanted = anchorPos - d;
					else if (anchorPos + d < (Int32) n && !differs ((UIndex) (anchorPos + d)))
						wanted = anchorPos + d;
				}
			}
		}
		// Every story moves: anchor on the lowest story and move it by elevation (SetElevation
		// has no effect on the top story), the upward sweep does the rest.
		const bool elevateAnchor = wanted < 0;
		if (elevateAnchor)
			wanted = 0;
		if (wanted != anchorPos) {
			anchorScope.MoveTo (cur.stories[wanted].index, cur.stories[anchorPos].floorId);
			anchorPos = wanted;
			cur = LoadStoryList ();
		}
		if (elevateAnchor) {
			API_StoryCmdType cmd = NewCmd (APIStory_SetElevation, cur.stories[anchorPos].index);
			cmd.elevation = t[anchorPos];
			RunStoryCmd (cmd, "Cannot change the level of " + StoryLabel (cur.stories[anchorPos]));
			cur = LoadStoryList ();
		}

		for (UIndex p = (UIndex) anchorPos; p + 1 < n; ++p) {
			const double gap = t[p + 1] - t[p];
			if (std::fabs ((cur.stories[p + 1].level - cur.stories[p].level) - gap) > kLevelTol) {
				SetStoryHeight (cur.stories[p].index, gap, "Cannot change the height of " + StoryLabel (cur.stories[p]));
				cur = LoadStoryList ();
			}
		}
		for (UIndex p = (UIndex) anchorPos; p > 0; --p) {
			const double gap = t[p] - t[p - 1];
			if (std::fabs ((cur.stories[p].level - cur.stories[p - 1].level) - gap) > kLevelTol) {
				SetStoryHeight (cur.stories[p - 1].index, gap, "Cannot change the height of " + StoryLabel (cur.stories[p - 1]));
				cur = LoadStoryList ();
			}
		}
	}

	if (topHeight.has_value ()) {
		const StoryRec& top = cur.Top ();
		if (!top.height.has_value () || std::fabs (*top.height - *topHeight) > kLevelTol) {
			SetStoryHeight (top.index, *topHeight, "Cannot change the height of the top " + StoryLabel (top));
			cur = LoadStoryList ();
		}
	}

	for (UIndex i = 0; i < n && i < cur.stories.GetSize (); ++i) {
		if (std::fabs (cur.stories[i].level - t[i]) > kVerifyTol) {
			Fail ("Archicad placed " + StoryLabel (cur.stories[i]) + GS::UniString::Printf (" at %.4f m instead of %.4f m.", cur.stories[i].level, t[i]),
				  APIERR_GENERAL);
		}
	}
	if (topHeight.has_value () && cur.Top ().height.has_value () && std::fabs (*cur.Top ().height - *topHeight) > kVerifyTol) {
		Fail ("Archicad set the height of the top " + StoryLabel (cur.Top ()) +
			  GS::UniString::Printf (" to %.4f m instead of %.4f m.", *cur.Top ().height, *topHeight), APIERR_GENERAL);
	}
}


// --- Element counts per story --------------------------------------------------------

// Temporarily makes the floor plan the current database (element lists are per database).
class FloorPlanDatabaseScope {
public:
	FloorPlanDatabaseScope ()
	{
		BNZeroMemory (&previous, sizeof (previous));
		if (ACAPI_Database (APIDb_GetCurrentDatabaseID, &previous, nullptr) != NoError)
			return;
		if (previous.typeID == APIWind_FloorPlanID)
			return;
		API_DatabaseInfo plan;
		BNZeroMemory (&plan, sizeof (plan));
		plan.typeID = APIWind_FloorPlanID;
		switched = ACAPI_Database (APIDb_ChangeCurrentDatabaseID, &plan, nullptr) == NoError;
	}

	~FloorPlanDatabaseScope ()
	{
		if (switched)
			ACAPI_Database (APIDb_ChangeCurrentDatabaseID, &previous, nullptr);
	}

	FloorPlanDatabaseScope (const FloorPlanDatabaseScope&) = delete;
	FloorPlanDatabaseScope& operator= (const FloorPlanDatabaseScope&) = delete;

private:
	API_DatabaseInfo	previous;
	bool				switched = false;
};


// Parts of hierarchical elements (counted through their main element instead).
bool IsSubElementType (API_ElemTypeID t)
{
	switch (t) {
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


struct StoryElementCounts {
	Int32					total = 0;
	std::map<int, Int32>	byType;		// API_ElemTypeID -> count
};

using ElementCountMap = std::map<short, StoryElementCounts>;	// floor index -> counts


ElementCountMap CountElementsPerStory ()
{
	ElementCountMap counts;
	FloorPlanDatabaseScope scope;

	GS::Array<API_Guid> guids;
	Check (ACAPI_Element_GetElemList (API_ElemType (API_ZombieElemID), &guids), "Cannot list the floor plan elements");
	for (const API_Guid& guid : guids) {
		API_Elem_Head head;
		BNZeroMemory (&head, sizeof (head));
		head.guid = guid;
		if (ACAPI_Element_GetHeader (&head) != NoError)
			continue;
		if (IsSubElementType (head.type.typeID))
			continue;
		// Hidden GDL parts of curtain walls / stairs / railings are listed as objects too.
		if (head.type.typeID == API_ObjectID) {
			API_Element elem;
			BNZeroMemory (&elem, sizeof (elem));
			elem.header.guid = guid;
			if (ACAPI_Element_Get (&elem) == NoError &&
				(elem.object.ownerType.typeID != API_ZombieElemID || elem.object.owner != APINULLGuid))
				continue;
		}
		StoryElementCounts& c = counts[head.floorInd];
		c.total++;
		c.byType[(int) head.type.typeID]++;
	}
	return counts;
}


void AddElementCounts (OS& storyJson, const ElementCountMap& counts, short floorInd)
{
	auto it = counts.find (floorInd);
	OS byType;
	Int32 total = 0;
	if (it != counts.end ()) {
		total = it->second.total;
		for (const auto& entry : it->second.byType)
			byType.Add (ToStr (ElemTypeName ((API_ElemTypeID) entry.first)), entry.second);
	}
	storyJson.Add ("elementCount", total);
	storyJson.Add ("elementsByType", byType);
}


// --- Level lookup --------------------------------------------------------------------

OS LevelLookup (const StoryList& list, double z)
{
	const StoryRec* found = &list.Bottom ();
	for (const StoryRec& s : list.stories) {
		if (z + kLevelEps >= s.level)
			found = &s;
	}
	OS o;
	o.Add ("level", z);
	o.Add ("storyIndex", (Int32) found->index);
	o.Add ("storyName", found->name);
	o.Add ("storyLevel", found->level);
	o.Add ("offsetFromStory", z - found->level);
	if (z + kLevelEps < list.Bottom ().level)
		o.Add ("note", GS::UniString ("Below the lowest story: the offset is negative."));
	return o;
}


// --- Compensation after a failed item -----------------------------------------------

// Puts the stories that exist in 'before' back to their levels (and the top story height).
// Returns an empty string on success, otherwise the problem.
GS::UniString RestoreLevels (const StoryList& before)
{
	try {
		const StoryList cur = LoadStoryList ();
		LevelTargets levels;
		for (const StoryRec& s : cur.stories) {
			if (const StoryRec* b = before.FindFloorId (s.floorId))
				levels[s.floorId] = b->level;
		}
		std::optional<double> topHeight;
		if (const StoryRec* b = before.FindFloorId (cur.Top ().floorId))
			topHeight = b->height;
		ApplyLevels (levels, topHeight);
		return GS::UniString ();
	} catch (const Error& e) {
		return e.message;
	}
}


GS::UniString RestoreAfterFailedCreate (const StoryList& before, short createdFloorId)
{
	try {
		const StoryList cur = LoadStoryList ();
		if (const StoryRec* s = cur.FindFloorId (createdFloorId)) {
			API_StoryCmdType cmd = NewCmd (APIStory_Delete, s->index);
			RunStoryCmd (cmd, "Cannot remove the inserted " + StoryLabel (*s));
		}
	} catch (const Error& e) {
		return e.message;
	}
	return RestoreLevels (before);
}


GS::UniString RestoreAfterFailedModify (const StoryList& before, const StoryRec& target)
{
	GS::UniString problems = RestoreLevels (before);
	try {
		const StoryList cur = LoadStoryList ();
		if (const StoryRec* s = cur.FindFloorId (target.floorId)) {
			if (s->name != target.name) {
				API_StoryCmdType cmd = NewCmd (APIStory_Rename, s->index);
				SetCmdName (cmd, target.name);
				RunStoryCmd (cmd, "Cannot restore the name of " + StoryLabel (*s));
			}
			if (s->dispOnSections != target.dispOnSections) {
				API_StoryCmdType cmd = NewCmd (APIStory_SetDispOnSections, s->index);
				cmd.dispOnSections = target.dispOnSections;
				RunStoryCmd (cmd, "Cannot restore 'showOnSections' of " + StoryLabel (*s));
			}
		}
	} catch (const Error& e) {
		problems += (problems.IsEmpty () ? "" : "; ") + e.message;
	}
	return problems;
}


// --- Create --------------------------------------------------------------------------

enum class InsertPosition : Int32 { Top, Bottom, Above, Below };

const NamedValue kPositions[] = {
	{ "Top",	(Int32) InsertPosition::Top },
	{ "Bottom",	(Int32) InsertPosition::Bottom },
	{ "Above",	(Int32) InsertPosition::Above },
	{ "Below",	(Int32) InsertPosition::Below },
};


std::optional<double> OptLevel (const OS& spec)
{
	if (auto v = OptDouble (spec, "level"))
		return v;
	return OptDouble (spec, "elevation");
}


std::optional<bool> OptShowOnSections (const OS& spec)
{
	if (auto v = OptBool (spec, "showOnSections"))
		return v;
	return OptBool (spec, "dispOnSections");
}


StoryRec FindByFloorIdOrFail (short floorId, const StoryList& list)
{
	const StoryRec* s = list.FindFloorId (floorId);
	if (s == nullptr)
		Fail (GS::UniString::Printf ("The story with floorId %d disappeared during the operation. ", (int) floorId) + list.Summary (), APIERR_GENERAL);
	return *s;
}


void CheckHeightValue (const std::optional<double>& height)
{
	if (height.has_value () && *height < kMinStoryHeight)
		Fail (GS::UniString::Printf ("height must be > 0 (got %.4f m).", *height));
}


// The new story goes between 'lo' (the story below it, nullptr at the bottom) and 'hi' (the
// story above it, nullptr at the top). Only one side moves (as a rigid block) when the new
// story needs room: above the ground floor the stories above move up; when 'hi' is the ground
// floor or a basement (index <= 0) the stories below move down instead.
struct InsertPlan {
	double	level = 0.0;		// level of the new story
	double	height = 0.0;		// height of the new story (to the story above it)
	double	upperShift = 0.0;	// added to the levels of 'hi' and the stories above it
	double	lowerShift = 0.0;	// added to the levels of 'lo' and the stories below it
};


InsertPlan PlanInsert (const StoryRec* lo, const StoryRec* hi, const StoryRec& ref, std::optional<double> level, std::optional<double> height)
{
	InsertPlan plan;
	const double refHeight = ref.height.value_or (kDefaultStoryHeight);

	if (hi == nullptr) {							// new top story: nothing moves
		plan.level = level.value_or (lo->level + lo->height.value_or (kDefaultStoryHeight));
		plan.height = height.value_or (refHeight);
		if (plan.level - lo->level < kMinStoryHeight)
			Fail (GS::UniString::Printf ("level %.3f m must be above the current top story (", plan.level) + StoryLabel (*lo) +
				  GS::UniString::Printf (" at %.3f m).", lo->level));
		return plan;
	}

	if (lo == nullptr) {							// new bottom story: nothing moves
		if (level.has_value () && height.has_value () && std::fabs (*level + *height - hi->level) > kLevelTol)
			Fail ("A new lowest story ends at the current lowest story (" + StoryLabel (*hi) +
				  GS::UniString::Printf (" at %.3f m): give either level or height, not both (level + height = %.3f m).", hi->level, *level + *height));
		if (level.has_value ()) {
			plan.level = *level;
			plan.height = hi->level - *level;
		} else {
			plan.height = height.value_or (refHeight);
			plan.level = hi->level - plan.height;
		}
		if (plan.height < kMinStoryHeight)
			Fail (GS::UniString::Printf ("level %.3f m must be below the current lowest story (", plan.level) + StoryLabel (*hi) +
				  GS::UniString::Printf (" at %.3f m).", hi->level));
		return plan;
	}

	const bool upperMoves = hi->index > 0;

	if (level.has_value () && !height.has_value ()) {	// fill the gap: nothing moves
		if (*level - lo->level < kMinStoryHeight || hi->level - *level < kMinStoryHeight)
			Fail (GS::UniString::Printf ("level %.3f m must be between ", *level) + StoryLabel (*lo) + GS::UniString::Printf (" (%.3f m) and ", lo->level) +
				  StoryLabel (*hi) + GS::UniString::Printf (" (%.3f m). To make room instead, give height as well (the stories %s move).", hi->level,
				  upperMoves ? "above" : "below"));
		plan.level = *level;
		plan.height = hi->level - *level;
		return plan;
	}

	plan.height = height.value_or (refHeight);
	if (upperMoves) {
		plan.level = level.value_or (hi->level);
		if (plan.level - lo->level < kMinStoryHeight)
			Fail (GS::UniString::Printf ("level %.3f m must be above ", plan.level) + StoryLabel (*lo) + GS::UniString::Printf (" at %.3f m.", lo->level));
		plan.upperShift = plan.level + plan.height - hi->level;
	} else {
		if (level.has_value () && std::fabs (*level + plan.height - hi->level) > kLevelTol)
			Fail ("Below the ground floor the stories at and above " + StoryLabel (*hi) +
				  GS::UniString::Printf (" keep their level, so the new story ends at %.3f m: give either level or height, not both.", hi->level));
		plan.level = hi->level - plan.height;
		plan.lowerShift = plan.level - hi->level;	// 'lo' keeps its height: it now ends at the new story
	}
	return plan;
}


// Per-call state of the batch commands.
struct BatchContext {
	bool abortBatch = false;	// set when an item left the stories in an unknown state
};


OS CreateOneStory (const OS& spec, BatchContext& ctx)
{
	const StoryList before = LoadStoryList ();

	const InsertPosition position = spec.Contains ("position")
		? (InsertPosition) ParseNamed (kPositions, spec, "position")
		: InsertPosition::Top;

	StoryRec ref;
	switch (position) {
		case InsertPosition::Top:		ref = before.Top ();		break;
		case InsertPosition::Bottom:	ref = before.Bottom ();		break;
		case InsertPosition::Above:
		case InsertPosition::Below:
			if (!spec.Contains ("relativeTo"))
				Fail ("position 'Above'/'Below' needs 'relativeTo' (the story index or name to insert next to). " + before.Summary ());
			ref = ResolveStory (before, spec, "relativeTo");
			break;
	}
	const bool insertAbove = position == InsertPosition::Top || position == InsertPosition::Above;

	const GS::UniString			name = GetString (spec, "name", GS::UniString ());
	const std::optional<double>	height = OptDouble (spec, "height");
	const std::optional<double>	level = OptLevel (spec);
	const std::optional<bool>	showOnSections = OptShowOnSections (spec);
	CheckHeightValue (height);

	const StoryRec* lo = insertAbove ? before.FindFloorId (ref.floorId) : before.Below (ref);
	const StoryRec* hi = insertAbove ? before.Above (ref) : before.FindFloorId (ref.floorId);
	const InsertPlan plan = PlanInsert (lo, hi, ref, level, height);

	// Targets for every pre-existing story + the new one (added once its floorId is known).
	LevelTargets targets;
	const Int32 hiPos = hi != nullptr ? before.PosOfFloorId (hi->floorId) : (Int32) before.stories.GetSize ();
	for (UIndex i = 0; i < before.stories.GetSize (); ++i) {
		const StoryRec& s = before.stories[i];
		targets[s.floorId] = s.level + ((Int32) i >= hiPos ? plan.upperShift : plan.lowerShift);
	}
	// The top story keeps its height unless the new story becomes the top.
	std::optional<double> topHeight = hi == nullptr ? std::optional<double> (plan.height) : before.Top ().height;
	{
		LevelTargets check = targets;
		const short fakeId = -32000;	// validate the order with the new story included
		StoryList withNew = before;
		StoryRec probe;
		probe.floorId = fakeId;
		probe.index = (short) (hiPos < (Int32) before.stories.GetSize () ? before.stories[hiPos].index : before.lastStory + 1);
		probe.level = plan.level;
		probe.name = name.IsEmpty () ? GS::UniString ("(new story)") : name;
		withNew.stories.Insert ((UIndex) hiPos, probe);
		check[fakeId] = plan.level;
		ValidateTargets (withNew, check, topHeight);
	}

	// Insert. Its first change is still a normal per-item error (nothing changed yet).
	// InsAbove's height becomes the height of 'ref', InsBelow's the height of the new story;
	// ApplyLevels then puts every story exactly where it belongs.
	API_StoryCmdType cmd = NewCmd (insertAbove ? APIStory_InsAbove : APIStory_InsBelow, ref.index);
	cmd.height = insertAbove ? plan.level - ref.level : plan.height;
	if (cmd.height < kMinStoryHeight)
		cmd.height = plan.height;
	SetCmdName (cmd, name);
	RunStoryCmd (cmd, "Cannot insert a story " + GS::UniString (insertAbove ? "above " : "below ") + StoryLabel (ref));

	short floorId = 0;
	bool found = false;
	StoryList after = LoadStoryList ();
	for (const StoryRec& s : after.stories) {
		if (before.FindFloorId (s.floorId) == nullptr) {
			floorId = s.floorId;
			found = true;
			break;
		}
	}
	if (!found) {
		ctx.abortBatch = true;
		Fail ("Archicad accepted the insert but no new story appeared. Call get_stories to check the project.", APIERR_GENERAL);
	}

	try {
		targets[floorId] = plan.level;
		ApplyLevels (targets, topHeight);
		after = LoadStoryList ();

		if (showOnSections.has_value ()) {
			const StoryRec cur = FindByFloorIdOrFail (floorId, after);
			if (cur.dispOnSections != *showOnSections) {
				API_StoryCmdType disp = NewCmd (APIStory_SetDispOnSections, cur.index);
				disp.dispOnSections = *showOnSections;
				RunStoryCmd (disp, "Story created, but setting 'showOnSections' failed");
				after = LoadStoryList ();
			}
		}
	} catch (const Error& e) {
		const GS::UniString restoreProblem = RestoreAfterFailedCreate (before, floorId);
		if (!restoreProblem.IsEmpty ()) {
			ctx.abortBatch = true;
			Fail (e.message + " Undoing the insert failed as well (" + restoreProblem + "); call get_stories and check the stories.", e.code);
		}
		Fail (e.message + " The inserted story was removed again and the other stories are back at their previous levels.", e.code);
	}

	const StoryRec result = FindByFloorIdOrFail (floorId, after);
	OS out ("story", StoryToJson (result, after));
	AddLevelChanges (out, before, after);
	if (!name.IsEmpty ()) {
		Int32 sameName = 0;
		for (const StoryRec& s : after.stories) {
			if (EqualsIgnoreCase (s.name, name))
				sameName++;
		}
		if (sameName > 1)
			out.Add ("warning", GS::UniString ("Another story has the same name; refer to these stories by index or floorId."));
	}
	return out;
}


// --- Modify --------------------------------------------------------------------------

OS ModifyOneStory (const OS& spec, BatchContext& ctx)
{
	const StoryList before = LoadStoryList ();
	const StoryRec target = ResolveTargetStory (before, spec, false);
	const short floorId = target.floorId;
	const Int32 pos = before.PosOfFloorId (floorId);
	const UIndex n = before.stories.GetSize ();

	const std::optional<GS::UniString>	name = OptString (spec, "name");
	const std::optional<double>			level = OptLevel (spec);
	const std::optional<double>			height = OptDouble (spec, "height");
	const std::optional<bool>			showOnSections = OptShowOnSections (spec);
	const bool							moveAbove = GetBool (spec, "moveStoriesAbove", false);

	if (!name.has_value () && !level.has_value () && !height.has_value () && !showOnSections.has_value ())
		Fail ("Nothing to change for " + StoryLabel (target) + ": give at least one of name, level, height, showOnSections.");
	if (moveAbove && !level.has_value ())
		Fail ("moveStoriesAbove only applies together with level (to move the story together with all stories above it).");
	CheckHeightValue (height);

	// Target levels.
	std::vector<double> t;
	for (const StoryRec& s : before.stories)
		t.push_back (s.level);
	std::optional<double> topHeight = before.Top ().height;
	if (level.has_value ()) {
		if (moveAbove) {
			const double delta = *level - target.level;
			for (UIndex i = (UIndex) pos; i < n; ++i)
				t[i] += delta;
		} else {
			const StoryRec* below = before.Below (target);
			const StoryRec* above = before.Above (target);
			if (below != nullptr && *level - below->level < kMinStoryHeight)
				Fail (GS::UniString::Printf ("level %.3f m must be above the story below (", *level) + StoryLabel (*below) +
					  GS::UniString::Printf (" at %.3f m). Lower that story first, or pass moveStoriesAbove: true on that story.", below->level));
			if (above != nullptr && !height.has_value () && above->level - *level < kMinStoryHeight)
				Fail (GS::UniString::Printf ("level %.3f m must be below the story above (", *level) + StoryLabel (*above) +
					  GS::UniString::Printf (" at %.3f m). Only this story moves; pass moveStoriesAbove: true to move the stories above along with it, "
											 "or change the height of the story below instead.", above->level));
			t[(UIndex) pos] = *level;
		}
	}
	if (height.has_value ()) {
		if ((UIndex) pos + 1 == n) {
			topHeight = height;
		} else {
			const double delta = *height - (t[(UIndex) pos + 1] - t[(UIndex) pos]);
			for (UIndex i = (UIndex) pos + 1; i < n; ++i)
				t[i] += delta;
		}
	}
	LevelTargets targets;
	for (UIndex i = 0; i < n; ++i)
		targets[before.stories[i].floorId] = t[i];
	ValidateTargets (before, targets, topHeight);

	if (name.has_value ()) {
		API_StoryCmdType probe = NewCmd (APIStory_Rename, target.index);
		SetCmdName (probe, *name);		// validates the length before anything changes
	}

	try {
		if (name.has_value () && *name != target.name) {
			API_StoryCmdType cmd = NewCmd (APIStory_Rename, target.index);
			SetCmdName (cmd, *name);
			RunStoryCmd (cmd, "Cannot rename " + StoryLabel (target));
		}

		if (level.has_value () || height.has_value ())
			ApplyLevels (targets, topHeight);

		if (showOnSections.has_value () && *showOnSections != target.dispOnSections) {
			const StoryRec cur = FindByFloorIdOrFail (floorId, LoadStoryList ());
			API_StoryCmdType cmd = NewCmd (APIStory_SetDispOnSections, cur.index);
			cmd.dispOnSections = *showOnSections;
			RunStoryCmd (cmd, "Cannot change 'showOnSections' of " + StoryLabel (cur));
		}
	} catch (const Error& e) {
		const StoryList now = LoadStoryList ();
		if (LevelChanges (before, now).IsEmpty () && FindByFloorIdOrFail (floorId, now).name == target.name &&
			FindByFloorIdOrFail (floorId, now).dispOnSections == target.dispOnSections)
			throw;		// nothing was changed
		const GS::UniString restoreProblem = RestoreAfterFailedModify (before, target);
		if (!restoreProblem.IsEmpty ()) {
			ctx.abortBatch = true;
			Fail (e.message + " Restoring the previous state failed as well (" + restoreProblem + "); call get_stories and check the stories.", e.code);
		}
		Fail (e.message + " The story settings were restored to their previous state.", e.code);
	}

	const StoryList after = LoadStoryList ();
	OS out ("story", StoryToJson (FindByFloorIdOrFail (floorId, after), after));
	AddLevelChanges (out, before, after);
	return out;
}


// --- Delete --------------------------------------------------------------------------

// Accepts "stories": [ref...] where ref is an index, a name, or {index}|{name}|{floorId}|{story}.
GS::Array<StoryRec> ResolveStoryList (const StoryList& list, const OS& params, const char* key)
{
	if (!params.Contains (key) || !params.IsList (key))
		Fail ("'" + GS::UniString (key) + "' must be a non-empty array of story references (index, name, or {floorId}). " + list.Summary ());

	// The list must be homogeneous: all objects, all strings, or all numbers
	// (the MCP tool always sends objects).
	GS::Array<OS> wrapped;
	GS::Array<OS> objects;
	GS::Array<GS::UniString> names;
	if (params.Get (key, objects) && !objects.IsEmpty ()) {
		for (const OS& item : objects) {
			OS w;
			if (item.Contains ("story"))
				w = item;
			else
				w.Add ("story", item);
			wrapped.Push (w);
		}
	} else if (params.Get (key, names) && !names.IsEmpty ()) {
		for (const GS::UniString& n : names)
			wrapped.Push (OS ("story", n));
	} else {
		for (double n : GetNumberArray (params, key))
			wrapped.Push (OS ("story", (Int32) std::llround (n)));
	}
	if (wrapped.IsEmpty ())
		Fail ("'" + GS::UniString (key) + "' must be a non-empty array of story references: all integers, all names, or objects {index}|{name}|{floorId}.");

	GS::Array<StoryRec> result;
	for (const OS& w : wrapped) {
		const StoryRec s = ResolveStory (list, w, "story");
		bool duplicate = false;
		for (const StoryRec& r : result) {
			if (r.floorId == s.floorId)
				duplicate = true;
		}
		if (!duplicate)
			result.Push (s);
	}
	return result;
}

} // namespace


void RegisterStoryCommands ()
{
	RegisterCommand ("GetStories",
		"Lists all stories bottom->top: {stories: [{index, displayNumber (number shown in the Navigator), name, level (m above Project Zero), height (m, to the next story), floorId (stable id), "
		"showOnSections, isCurrent, reservedByOtherUser?}], firstIndex, lastIndex, currentIndex, count, skipNullFloor, ghostStory?}. "
		"Options: includeElementCounts (bool, adds elementCount + elementsByType per story, counted by home story), "
		"atLevels ([z in m] -> levelLookup: [{level, storyIndex, storyName, storyLevel, offsetFromStory}]).",
		[] (const OS& params) -> OS {
			const StoryList list = LoadStoryList ();
			const bool withCounts = GetBool (params, "includeElementCounts", false);

			ElementCountMap counts;
			if (withCounts)
				counts = CountElementsPerStory ();

			GS::Array<OS> stories;
			for (const StoryRec& s : list.stories) {
				OS js = StoryToJson (s, list);
				if (withCounts)
					AddElementCounts (js, counts, s.index);
				stories.Push (js);
			}

			OS out;
			out.Add ("stories", stories);
			out.Add ("firstIndex", (Int32) list.firstStory);
			out.Add ("lastIndex", (Int32) list.lastStory);
			out.Add ("currentIndex", (Int32) list.actStory);
			out.Add ("count", (Int32) list.stories.GetSize ());
			out.Add ("skipNullFloor", list.skipNullFloor);

			API_GhostStoryType ghost;
			BNZeroMemory (&ghost, sizeof (ghost));
			if (ACAPI_Environment (APIEnv_GetGhostStorySettingsID, &ghost, nullptr) == NoError) {
				OS g;
				g.Add ("on", ghost.on);
				g.Add ("storyIndex", (Int32) ghost.storyInd);
				g.Add ("showOne", ghost.showOne);
				g.Add ("showAbove", ghost.showAbove);
				g.Add ("showBelow", ghost.showBelow);
				out.Add ("ghostStory", g);
			}

			const GS::Array<double> levels = GetNumberArray (params, "atLevels");
			if (!levels.IsEmpty ()) {
				GS::Array<OS> lookup;
				for (double z : levels)
					lookup.Push (LevelLookup (list, z));
				out.Add ("levelLookup", lookup);
			}
			return out;
		});

	RegisterCommand ("CreateStories",
		"Inserts stories, items applied in order (each sees the result of the previous ones). "
		"Input: {stories: [{name?, height? (m, height of the NEW story; default: height of the reference story, or 3), level? (m above Project Zero), "
		"position?: 'Top' (default, above the highest story)|'Bottom' (below the lowest)|'Above'|'Below', relativeTo? (story ref, for Above/Below), "
		"showOnSections?}]}. level only = the new story fills the gap, nothing moves; height (with or without level) = room is made: "
		"above the ground floor the stories above move up, next to the ground floor/basements the stories below move down. "
		"Output: {results: [{story, movedStories?} | {error}], stories: [all stories after the change]}. "
		"Inserting shifts the indexes of other stories; floorId stays stable. Story settings are not undoable in the API: "
		"a failed item is reverted by the add-on itself.",
		[] (const OS& params) -> OS {
			const GS::Array<OS> items = GetObjectArray (params, "stories");
			if (items.IsEmpty ())
				Fail ("'stories' must contain at least one story spec, e.g. {\"stories\": [{\"name\": \"Roof\", \"height\": 3}]}.");
			if (items.GetSize () > kMaxBatch)
				Fail (GS::UniString::Printf ("Too many stories in one call (%u, maximum %u).", (unsigned) items.GetSize (), (unsigned) kMaxBatch));

			BatchContext ctx;
			GS::Array<OS> results;
			for (UIndex i = 0; i < items.GetSize (); ++i) {
				if (ctx.abortBatch) {
					results.Push (ErrorObject (APIERR_CANCEL, GS::UniString::Printf ("Not attempted: item %u left the stories in an unexpected state.", (unsigned) i - 1)));
					continue;
				}
				results.Push (Try ([&] () { return CreateOneStory (items[i], ctx); }));
			}
			const StoryList after = LoadStoryList ();
			return OS ("results", results, "stories", StoriesToJson (after));
		});

	RegisterCommand ("ModifyStories",
		"Changes stories, patches applied in order. Input: {stories: [{story (index | name | {floorId}), name?, level? (m above Project Zero; "
		"only this story moves, the heights of it and of the story below change; moveStoriesAbove: true moves the stories above along), "
		"height? (m, to the next story; the stories above move), showOnSections?}]}. "
		"Output: {results: [{story, movedStories?} | {error}], stories: [all stories after the change]}. A failed patch is reverted.",
		[] (const OS& params) -> OS {
			const GS::Array<OS> items = GetObjectArray (params, "stories");
			if (items.IsEmpty ())
				Fail ("'stories' must contain at least one patch, e.g. {\"stories\": [{\"story\": 1, \"height\": 3.2}]}.");
			if (items.GetSize () > kMaxBatch)
				Fail (GS::UniString::Printf ("Too many stories in one call (%u, maximum %u).", (unsigned) items.GetSize (), (unsigned) kMaxBatch));

			BatchContext ctx;
			GS::Array<OS> results;
			for (UIndex i = 0; i < items.GetSize (); ++i) {
				if (ctx.abortBatch) {
					results.Push (ErrorObject (APIERR_CANCEL, GS::UniString::Printf ("Not attempted: item %u left the stories in an unexpected state.", (unsigned) i - 1)));
					continue;
				}
				results.Push (Try ([&] () { return ModifyOneStory (items[i], ctx); }));
			}
			const StoryList after = LoadStoryList ();
			return OS ("results", results, "stories", StoriesToJson (after));
		});

	RegisterCommand ("DeleteStories",
		"Deletes stories AND ALL ELEMENTS whose home story they are. Input: {stories: [index | name | {index}|{name}|{floorId}], "
		"dryRun?: bool (only report what would be deleted), keepLevels?: bool (default false: Archicad closes the gap, e.g. the stories above move down; "
		"true: every remaining story keeps its level and the story below a deleted one grows)}. References are resolved before anything is deleted. "
		"At least one story must remain. Each deletion is its own Archicad undo step (Edit > Undo restores the story and its elements). "
		"Output: {results: [{deleted: story + elementCount} | {error}], movedStories?, stories: [remaining]} "
		"or, with dryRun, {dryRun: true, wouldDelete: [story + elementCount + elementsByType], remainingCount}.",
		[] (const OS& params) -> OS {
			const StoryList list = LoadStoryList ();
			const GS::Array<StoryRec> targets = ResolveStoryList (list, params, "stories");
			if (targets.GetSize () >= list.stories.GetSize ())
				Fail ("Cannot delete every story: a project needs at least one story. " + list.Summary ());

			const ElementCountMap counts = CountElementsPerStory ();

			if (GetBool (params, "dryRun", false)) {
				GS::Array<OS> would;
				for (const StoryRec& s : targets) {
					OS js = StoryToJson (s, list);
					AddElementCounts (js, counts, s.index);
					would.Push (js);
				}
				return OS ("dryRun", true, "wouldDelete", would, "remainingCount", (Int32) (list.stories.GetSize () - targets.GetSize ()));
			}

			// APIStory_Delete is a "complete operation": it must NOT run inside an undo scope
			// (APIERR_REFUSEDCMD); Archicad records it as its own undo step.
			const bool keepLevels = GetBool (params, "keepLevels", false);
			// Archicad switches the current story when a story is deleted: remember it (by floorId).
			std::optional<short> previousCurrentFloorId;
			for (const StoryRec& s : list.stories) {
				if (s.index == list.actStory)
					previousCurrentFloorId = s.floorId;
			}
			GS::Array<OS> results;
			bool anyDeleted = false;
			for (const StoryRec& target : targets) {
				results.Push (Try ([&] () -> OS {
					const StoryList cur = LoadStoryList ();
					const StoryRec s = FindByFloorIdOrFail (target.floorId, cur);
					if (cur.stories.GetSize () <= 1)
						Fail ("Cannot delete the last remaining story.");
					API_StoryCmdType cmd = NewCmd (APIStory_Delete, s.index);
					RunStoryCmd (cmd, "Cannot delete " + StoryLabel (s));
					anyDeleted = true;
					OS js = StoryToJson (target, list);
					AddElementCounts (js, counts, target.index);
					return OS ("deleted", js);
				}));
			}

			OS out;
			if (keepLevels && anyDeleted) {
				const GS::UniString problem = RestoreLevels (list);
				if (!problem.IsEmpty ())
					out.Add ("warning", GS::UniString (GS::UniString ("The stories were deleted, but keeping the levels of the remaining stories failed: ") + problem + " Call get_stories."));
			}
			if (anyDeleted && previousCurrentFloorId.has_value ()) {
				const StoryList now = LoadStoryList ();
				if (const StoryRec* prev = now.FindFloorId (*previousCurrentFloorId)) {
					if (prev->index != now.actStory) {
						API_StoryCmdType cmd = NewCmd (APIStory_GoTo, prev->index);
						ExecStoryCmd (cmd);
					}
				}
			}
			const StoryList after = LoadStoryList ();
			out.Add ("results", results);
			out.Add ("stories", StoriesToJson (after));
			AddLevelChanges (out, list, after);
			return out;
		});

	RegisterCommand ("SetCurrentStory",
		"Makes a story the current one (like double-clicking it in the Navigator). Input: {story: index | name | {floorId}, "
		"openFloorPlan?: bool (default true: switch to the floor plan window first)}. Output: {currentStory, windowChanged}.",
		[] (const OS& params) -> OS {
			const StoryList list = LoadStoryList ();
			const StoryRec target = ResolveTargetStory (list, params, true);

			bool windowChanged = false;
			if (GetBool (params, "openFloorPlan", true)) {
				API_WindowInfo current;
				BNZeroMemory (&current, sizeof (current));
				const GSErrCode werr = ACAPI_Database (APIDb_GetCurrentWindowID, &current, nullptr);
				if (werr != NoError || current.typeID != APIWind_FloorPlanID) {
					API_WindowInfo plan;
					BNZeroMemory (&plan, sizeof (plan));
					plan.typeID = APIWind_FloorPlanID;
					Check (ACAPI_Automate (APIDo_ChangeWindowID, &plan, nullptr), "Cannot switch to the floor plan window");
					windowChanged = true;
				}
			}

			API_StoryCmdType cmd = NewCmd (APIStory_GoTo, target.index);
			const GSErrCode err = ACAPI_Environment (APIEnv_ChangeStorySettingsID, &cmd, nullptr);
			if (err == APIERR_NEEDSUNDOSCOPE) {
				Undoable ("Go to story (Claude)", [&] () {
					API_StoryCmdType inner = NewCmd (APIStory_GoTo, target.index);
					RunStoryCmd (inner, "Cannot go to " + StoryLabel (target));
				});
			} else {
				CheckStoryCmd (err, "Cannot go to " + StoryLabel (target));
			}

			const StoryList after = LoadStoryList ();
			const StoryRec cur = FindByFloorIdOrFail (target.floorId, after);
			OS out ("currentStory", StoryToJson (cur, after), "windowChanged", windowChanged);
			if (after.actStory != cur.index)
				out.Add ("warning", GS::UniString::Printf ("Archicad reports story %d as current.", (int) after.actStory));
			return out;
		});
}

} // namespace cc
