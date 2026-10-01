#pragma once

// *****************************************************************************
// IMPLEMENTATION FILE — included exactly once, at the end of Project.cpp.
// Kept as a header so that the CMake source glob (evaluated only when the build
// directory is configured) does not need to be re-run for this family.
//
// ProjectEditMenu — Undo / Redo.
//
// The Archicad 26 C++ API has NO undo/redo function (only ACAPI_CallUndoableCommand
// to create undo steps, and element notifications about undo/redo). This file therefore
// triggers Archicad's own Edit > Undo / Edit > Redo menu commands on macOS, in-process,
// through the Objective-C runtime (resolved with dlsym, so no extra link dependency):
//
//   1. find the item in [NSApp mainMenu] by its key equivalent (Cmd+Z = Undo,
//      Shift+Cmd+Z = Redo; works with any UI language), falling back to the first
//      items of the Edit menu when their title starts with a known Undo/Redo word;
//   2. let the menu validate itself (menuNeedsUpdate: + update) so the title shows
//      what will be undone ("Undo Create walls (Claude)") and the enabled state is fresh;
//   3. perform the item ASYNCHRONOUSLY on the main queue, i.e. after this JSON command
//      returned (Archicad's undo must not run nested inside an add-on command).
//
// Commands:
//   Undo {steps?: 1..50, dryRun?: bool, force?: bool, onlyIfTitleContains?: string}
//   Redo {steps?: 1..50, dryRun?: bool, force?: bool, onlyIfTitleContains?: string}
//   onlyIfTitleContains guards against undoing someone else's work: every step is performed only
//   while the menu title contains the text (checked when scheduling AND right before each step).
// Output: {scheduled, runId, steps, menuItem: {title, enabled, ...}, lastRun}
//   dryRun only inspects the menu item and reports the result of the last run
//   (lastRun: {runId, action, requested, performed, titles, stoppedReason, finished}).
// *****************************************************************************

#include "Commands/ProjectShared.hpp"
#include "Core/Command.hpp"

#include <chrono>
#include <string>

#if defined (macintosh) || defined (__APPLE__)
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#define CC_HAS_COCOA_MENU 1
#endif

namespace cc {
namespace project {

namespace {

enum class EditAction { Undo, Redo };

const char* ActionName (EditAction a)
{
	return a == EditAction::Undo ? "Undo" : "Redo";
}


// Result of the most recent asynchronous run (read by dryRun calls).
struct LastRun {
	Int32						runId = 0;
	EditAction					action = EditAction::Undo;
	Int32						requested = 0;
	Int32						performed = 0;
	GS::Array<GS::UniString>	titles;			// menu titles right before each performed step
	GS::UniString				stoppedReason;
	bool						finished = true;
	std::chrono::steady_clock::time_point scheduledAt;
};

LastRun		gLastRun;
Int32		gNextRunId = 1;


OS LastRunJson ()
{
	if (gLastRun.runId == 0)
		return OS ("runId", (Int32) 0, "note", GS::UniString ("No undo/redo has been run by the connector yet."));
	OS out;
	out.Add ("runId", gLastRun.runId);
	out.Add ("action", GS::UniString (ActionName (gLastRun.action)));
	out.Add ("requested", gLastRun.requested);
	out.Add ("performed", gLastRun.performed);
	out.Add ("titles", gLastRun.titles);
	out.Add ("finished", gLastRun.finished);
	if (!gLastRun.stoppedReason.IsEmpty ())
		out.Add ("stoppedReason", gLastRun.stoppedReason);
	return out;
}


#if defined (CC_HAS_COCOA_MENU)

// --- Minimal Objective-C runtime access ------------------------------------------------

using ObjPtr = void*;
using SelPtr = void*;

struct ObjcRuntime {
	ObjPtr	(*getClass) (const char*) = nullptr;
	SelPtr	(*registerName) (const char*) = nullptr;
	void*	msgSend = nullptr;

	bool Ok () const { return getClass != nullptr && registerName != nullptr && msgSend != nullptr; }
};


const ObjcRuntime& Runtime ()
{
	static const ObjcRuntime rt = [] {
		ObjcRuntime r;
		r.getClass		= reinterpret_cast<ObjPtr (*) (const char*)> (dlsym (RTLD_DEFAULT, "objc_getClass"));
		r.registerName	= reinterpret_cast<SelPtr (*) (const char*)> (dlsym (RTLD_DEFAULT, "sel_registerName"));
		r.msgSend		= dlsym (RTLD_DEFAULT, "objc_msgSend");
		return r;
	} ();
	return rt;
}


// objc_msgSend must be called through a correctly typed function pointer (integer/pointer returns only).
template <typename R, typename... Args>
R Msg (ObjPtr receiver, const char* selector, Args... args)
{
	if (receiver == nullptr)
		return R ();
	using Fn = R (*) (ObjPtr, SelPtr, Args...);
	return reinterpret_cast<Fn> (Runtime ().msgSend) (receiver, Runtime ().registerName (selector), args...);
}


template <typename... Args>
void MsgVoid (ObjPtr receiver, const char* selector, Args... args)
{
	if (receiver == nullptr)
		return;
	using Fn = void (*) (ObjPtr, SelPtr, Args...);
	reinterpret_cast<Fn> (Runtime ().msgSend) (receiver, Runtime ().registerName (selector), args...);
}


GS::UniString NSStringToUni (ObjPtr str)
{
	const char* utf8 = Msg<const char*> (str, "UTF8String");
	return utf8 != nullptr ? GS::UniString (utf8, CC_UTF8) : GS::UniString ();
}


constexpr unsigned long kShift		= 1ul << 17;
constexpr unsigned long kControl	= 1ul << 18;
constexpr unsigned long kOption		= 1ul << 19;
constexpr unsigned long kCommand	= 1ul << 20;


struct FoundItem {
	ObjPtr			menu = nullptr;		// the submenu containing the item
	long			index = -1;
	GS::UniString	menuTitle;
	GS::UniString	title;
	GS::UniString	keyEquivalent;
	bool			enabled = false;
	GS::UniString	matchedBy;			// "keyEquivalent" | "title"
};


bool MatchesKey (EditAction action, const std::string& key, unsigned long mods)
{
	mods &= (kShift | kControl | kOption | kCommand);
	if (action == EditAction::Undo)
		return key == "z" && mods == kCommand;
	return (key == "z" && mods == (kCommand | kShift)) || (key == "Z" && (mods == kCommand || mods == (kCommand | kShift)));
}


bool TitleLooksLike (EditAction action, const GS::UniString& title)
{
	static const char* const kUndoWords[] = { "Undo", "Отменить", "Отмена", "Rückgängig", "Widerrufen", "Annuler", "Deshacer", "Annulla", "Desfazer", "Cofnij", "Zpět", "Visszavonás", "取り消し", "撤销", "還原" };
	static const char* const kRedoWords[] = { "Redo", "Повторить", "Вернуть", "Wiederholen", "Wiederherstellen", "Rétablir", "Refaire", "Rehacer", "Ripeti", "Refazer", "Ponów", "Znovu", "Mégis", "やり直し", "重做" };
	if (action == EditAction::Undo) {
		for (const char* w : kUndoWords)
			if (title.BeginsWith (GS::UniString (w, CC_UTF8)))
				return true;
	} else {
		for (const char* w : kRedoWords)
			if (title.BeginsWith (GS::UniString (w, CC_UTF8)))
				return true;
	}
	return false;
}


void RefreshMenu (ObjPtr menu)
{
	// What AppKit does before a menu is shown or searched for key equivalents.
	ObjPtr delegate = Msg<ObjPtr> (menu, "delegate");
	if (delegate != nullptr) {
		SelPtr needsUpdate = Runtime ().registerName ("menuNeedsUpdate:");
		if (Msg<signed char> (delegate, "respondsToSelector:", needsUpdate) != 0)
			MsgVoid (delegate, "menuNeedsUpdate:", menu);
	}
	MsgVoid (menu, "update");
}


FoundItem Describe (ObjPtr menu, long index, ObjPtr topItem, const char* matchedBy)
{
	FoundItem f;
	ObjPtr item = Msg<ObjPtr> (menu, "itemAtIndex:", index);
	f.menu = menu;
	f.index = index;
	f.menuTitle = NSStringToUni (Msg<ObjPtr> (topItem, "title"));
	f.title = NSStringToUni (Msg<ObjPtr> (item, "title"));
	f.keyEquivalent = NSStringToUni (Msg<ObjPtr> (item, "keyEquivalent"));
	f.enabled = Msg<signed char> (item, "isEnabled") != 0;
	f.matchedBy = matchedBy;
	return f;
}


// Searches one submenu for the action by key equivalent; returns the item index or -1.
long FindByKeyIn (ObjPtr menu, EditAction action)
{
	const long n = Msg<long> (menu, "numberOfItems");
	for (long j = 0; j < n && j < 200; ++j) {
		ObjPtr item = Msg<ObjPtr> (menu, "itemAtIndex:", j);
		if (item == nullptr || Msg<signed char> (item, "isSeparatorItem") != 0)
			continue;
		const char* key = Msg<const char*> (Msg<ObjPtr> (item, "keyEquivalent"), "UTF8String");
		if (key == nullptr || key[0] == '\0')
			continue;
		if (MatchesKey (action, key, Msg<unsigned long> (item, "keyEquivalentModifierMask")))
			return j;
	}
	return -1;
}


// Finds the Undo/Redo item in the main menu (refreshing its menu). found.menu == nullptr when not found.
FoundItem FindEditItem (EditAction action, GS::UniString& problem)
{
	FoundItem none;
	if (!Runtime ().Ok ()) {
		problem = "The Objective-C runtime is not available in this process.";
		return none;
	}
	ObjPtr app = Msg<ObjPtr> (Runtime ().getClass ("NSApplication"), "sharedApplication");
	ObjPtr mainMenu = Msg<ObjPtr> (app, "mainMenu");
	if (mainMenu == nullptr) {
		problem = "Archicad has no main menu right now (a modal dialog may be open).";
		return none;
	}
	const long top = Msg<long> (mainMenu, "numberOfItems");

	// 1) key equivalent (language independent). Pass 0 searches the menus as they are; pass 1
	//    lets each top-level menu populate/validate itself first (menus may be filled lazily).
	for (int pass = 0; pass < 2; ++pass) {
		for (long i = 0; i < top && i < 16; ++i) {
			ObjPtr topItem = Msg<ObjPtr> (mainMenu, "itemAtIndex:", i);
			ObjPtr sub = Msg<ObjPtr> (topItem, "submenu");
			if (sub == nullptr)
				continue;
			if (pass == 0 && FindByKeyIn (sub, action) < 0)
				continue;
			RefreshMenu (sub);
			const long j = FindByKeyIn (sub, action);		// re-find: the delegate may rebuild the menu
			if (j >= 0)
				return Describe (sub, j, topItem, "keyEquivalent");
		}
	}

	// 2) fallback: first items of the Edit menu (normally the 3rd top-level menu), by title
	//    (menus were refreshed by pass 1 above)
	for (long i = 1; i < top && i < 4; ++i) {
		ObjPtr topItem = Msg<ObjPtr> (mainMenu, "itemAtIndex:", i);
		ObjPtr sub = Msg<ObjPtr> (topItem, "submenu");
		if (sub == nullptr)
			continue;
		const long n = Msg<long> (sub, "numberOfItems");
		for (long j = 0; j < n && j < 4; ++j) {
			ObjPtr item = Msg<ObjPtr> (sub, "itemAtIndex:", j);
			if (item != nullptr && TitleLooksLike (action, NSStringToUni (Msg<ObjPtr> (item, "title"))))
				return Describe (sub, j, topItem, "title");
		}
	}
	problem = GS::UniString ("Could not find the Edit > ") + ActionName (action) + " menu item (looked for its keyboard shortcut " +
			  (action == EditAction::Undo ? "Cmd+Z" : "Shift+Cmd+Z") + " and for its title).";
	return none;
}


OS FoundJson (const FoundItem& f)
{
	OS out;
	out.Add ("title", f.title);
	out.Add ("enabled", f.enabled);
	out.Add ("menu", f.menuTitle);
	out.Add ("keyEquivalent", f.keyEquivalent);
	out.Add ("matchedBy", f.matchedBy);
	return out;
}


struct RunContext {
	Int32			runId;
	EditAction		action;
	Int32			steps;
	bool			force;
	GS::UniString	titleFilter;		// empty = no filter
	Int32			staleWaits = 0;		// main-queue turns waited for the menu title to change after a step
};


bool TitleMatches (const GS::UniString& title, const GS::UniString& filter)
{
	return filter.IsEmpty () || title.ToLowerCase ().Contains (filter.ToLowerCase ());
}


// Performs one step; returns false when the run has to stop (stoppedReason set).
bool PerformStep (const RunContext& run)
{
	GS::UniString problem;
	const FoundItem f = FindEditItem (run.action, problem);
	if (f.menu == nullptr) {
		gLastRun.stoppedReason = problem;
		return false;
	}
	if (!f.enabled && !run.force) {
		gLastRun.stoppedReason = GS::UniString ("The ") + ActionName (run.action) + " menu item is disabled (nothing more to " +
								 (run.action == EditAction::Undo ? "undo" : "redo") + ", or Archicad is busy).";
		return false;
	}
	if (!TitleMatches (f.title, run.titleFilter)) {
		gLastRun.stoppedReason = "The next step '" + f.title + "' does not contain '" + run.titleFilter + "' (onlyIfTitleContains): stopped.";
		return false;
	}
	gLastRun.titles.Push (f.title);
	MsgVoid (f.menu, "performActionForItemAtIndex:", f.index);
	++gLastRun.performed;
	return true;
}


// Runs on the main queue after the JSON command has returned: ONE step per main-queue turn.
// Archicad renames the Undo/Redo item only when the event loop runs, so performing all steps in a
// single callback read the same (stale) title for every step (verified live).
void PerformRun (void* ctx)
{
	RunContext* run = static_cast<RunContext*> (ctx);
	bool more = false;
	if (run->runId == gLastRun.runId) {			// a newer run replaces this one
		// Archicad renames the item a few event-loop turns after a step; 50 ms was not always enough (verified live: the 3rd
		// of 3 undos read the 2nd step's title). Wait up to ~1 s while the title still equals the previous step's; equal
		// titles are legitimate (two steps with the same name), so go on after that.
		if (!gLastRun.titles.IsEmpty () && run->staleWaits < 20) {
			GS::UniString problem;
			const FoundItem f = FindEditItem (run->action, problem);
			if (f.menu != nullptr && f.title == gLastRun.titles.GetLast ()) {
				++run->staleWaits;
				dispatch_after_f (dispatch_time (DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC), dispatch_get_main_queue (), run, PerformRun);
				return;
			}
		}
		run->staleWaits = 0;
		try {
			more = PerformStep (*run) && gLastRun.performed < run->steps;
		} catch (...) {
			gLastRun.stoppedReason = "Unexpected exception while performing the menu command.";
		}
	}
	if (more) {
		dispatch_after_f (dispatch_time (DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC), dispatch_get_main_queue (), run, PerformRun);
		return;
	}
	if (run->runId == gLastRun.runId)
		gLastRun.finished = true;
	delete run;
}

#endif // CC_HAS_COCOA_MENU


OS RunEditAction (EditAction action, const OS& params)
{
#if defined (CC_HAS_COCOA_MENU)
	const Int32 steps = GetInt (params, "steps", 1);
	if (steps < 1 || steps > 50)
		Fail ("'steps' must be between 1 and 50.");
	const bool dryRun = GetBool (params, "dryRun", false);
	const bool force = GetBool (params, "force", false);
	const GS::UniString titleFilter = GetString (params, "onlyIfTitleContains", "");

	GS::UniString problem;
	const FoundItem found = FindEditItem (action, problem);

	OS out;
	if (dryRun) {
		out.Add ("found", found.menu != nullptr);
		if (found.menu != nullptr)
			out.Add ("menuItem", FoundJson (found));
		else
			out.Add ("problem", problem);
		out.Add ("lastRun", LastRunJson ());
		return out;
	}
	if (found.menu == nullptr)
		Fail (problem + " Undo/redo is not available through the Archicad 26 API itself.", APIERR_NOTSUPPORTED);
	if (!TitleMatches (found.title, titleFilter))
		Fail ("Nothing was done: the " + GS::UniString (ActionName (action)) + " menu item is '" + found.title + "', which does not contain '" + titleFilter +
			  "' (onlyIfTitleContains). The last step was probably made by someone else.", APIERR_REFUSEDCMD);
	if (!found.enabled && !force)
		Fail ("Nothing to " + GS::UniString (action == EditAction::Undo ? "undo" : "redo") + ": the Edit > " + ActionName (action) + " menu item ('" + found.title +
			  "') is disabled. Pass force: true only if you are sure the state is stale.", APIERR_REFUSEDCMD);
	if (!gLastRun.finished && std::chrono::steady_clock::now () - gLastRun.scheduledAt < std::chrono::seconds (30))
		Fail ("A previous undo/redo run is still pending. Wait a moment and retry.", APIERR_REFUSEDCMD);

	gLastRun = LastRun ();
	gLastRun.runId = gNextRunId++;
	gLastRun.action = action;
	gLastRun.requested = steps;
	gLastRun.finished = false;
	gLastRun.scheduledAt = std::chrono::steady_clock::now ();

	RunContext* ctx = new RunContext { gLastRun.runId, action, steps, force, titleFilter };
	dispatch_async_f (dispatch_get_main_queue (), ctx, PerformRun);

	out.Add ("scheduled", true);
	out.Add ("runId", gLastRun.runId);
	out.Add ("steps", steps);
	out.Add ("menuItem", FoundJson (found));
	out.Add ("note", GS::UniString ("The menu command runs right after this call returns; call again with dryRun: true to see lastRun."));
	return out;
#else
	(void) params;
	Fail (GS::UniString (ActionName (action)) + " is not available: the Archicad 26 API has no undo/redo function, and the Edit-menu workaround is implemented for macOS only.", APIERR_NOTSUPPORTED);
#endif
}

} // namespace


void RegisterEditMenuCommands ()
{
	RegisterCommand ("Undo",
		"Undoes the last Archicad operation(s) by triggering Edit > Undo (the AC26 API has no undo function; macOS only). "
		"Input: {steps?: 1-50, dryRun?: only report the menu item title/enabled state and the last run, force?: perform even if the item reports disabled, "
		"onlyIfTitleContains?: perform each step only while the menu title contains this text (e.g. '(Claude)')}. "
		"The command runs asynchronously right after the call returns.",
		[] (const OS& params) -> OS { return RunEditAction (EditAction::Undo, params); });

	RegisterCommand ("Redo",
		"Redoes the last undone operation(s) by triggering Edit > Redo (macOS only). Input: {steps?: 1-50, dryRun?, force?, onlyIfTitleContains?}. Asynchronous like Undo.",
		[] (const OS& params) -> OS { return RunEditAction (EditAction::Redo, params); });
}

} // namespace project
} // namespace cc
