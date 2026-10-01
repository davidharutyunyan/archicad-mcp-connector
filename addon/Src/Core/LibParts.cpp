#include "Core/LibParts.hpp"

#include <cmath>

namespace cc {

// --- Library parts -----------------------------------------------------------------

GS::UniString LibTypeName (API_LibTypeID typeID)
{
	switch (typeID) {
		case APILib_SpecID:				return "Spec";
		case APILib_WindowID:			return "Window";
		case APILib_DoorID:				return "Door";
		case APILib_ObjectID:			return "Object";
		case APILib_LampID:				return "Lamp";
		case APILib_RoomID:				return "Zone";
		case APILib_PropertyID:			return "Property";
		case APILib_PlanSignID:			return "PlanSign";
		case APILib_LabelID:			return "Label";
		case APILib_MacroID:			return "Macro";
		case APILib_PictID:				return "Picture";
		case APILib_ListSchemeID:		return "ListScheme";
		case APILib_SkylightID:			return "Skylight";
		case APILib_OpeningSymbolID:	return "OpeningSymbol";
		default:						return "Unknown";
	}
}


API_LibPart GetLibPartByIndex (Int32 libInd)
{
	API_LibPart lp;
	BNZeroMemory (&lp, sizeof (lp));
	lp.index = libInd;
	GSErrCode err = ACAPI_LibPart_Get (&lp);
	if (lp.location != nullptr) {
		delete lp.location;
		lp.location = nullptr;
	}
	Check (err, "Library part index " + GS::ValueToUniString (libInd) + " not found");
	return lp;
}


OS LibPartToJson (const API_LibPart& lp)
{
	OS out;
	out.Add ("index", (Int32) lp.index);
	out.Add ("name", GS::UniString (lp.docu_UName));
	out.Add ("fileName", GS::UniString (lp.file_UName));
	out.Add ("type", LibTypeName (lp.typeID));
	out.Add ("guid", GS::UniString (lp.ownUnID));
	out.Add ("parentGuid", GS::UniString (lp.parentUnID));
	out.Add ("isPlaceable", lp.isPlaceable);
	out.Add ("isTemplate", lp.isTemplate);
	out.Add ("missingDef", lp.missingDef);
	return out;
}


bool IsSubtypeOf (const API_LibPart& lp, API_LibTypeID typeID)
{
	return lp.typeID == typeID;
}


static std::optional<API_LibPart> SearchByName (const GS::UniString& name)
{
	API_LibPart lp;
	BNZeroMemory (&lp, sizeof (lp));
	GS::ucsncpy (lp.docu_UName, name.ToUStr ().Get (), API_UniLongNameLen - 1);
	GSErrCode err = ACAPI_LibPart_Search (&lp, false, false);
	if (lp.location != nullptr) {
		delete lp.location;
		lp.location = nullptr;
	}
	if (err == NoError && lp.index > 0)
		return lp;
	return std::nullopt;
}


static std::optional<API_LibPart> SearchByUnId (const GS::UniString& unId)
{
	API_LibPart lp;
	BNZeroMemory (&lp, sizeof (lp));
	GS::String s = ToStr (unId);
	if (!s.IsEmpty () && s[0] != '{') {
		// Accept "MAIN-GUID" without braces: search by main GUID prefix below.
	}
	CHTruncate (s.ToCStr (), lp.ownUnID, sizeof (lp.ownUnID));
	GSErrCode err = ACAPI_LibPart_Search (&lp, false, false);
	if (lp.location != nullptr) {
		delete lp.location;
		lp.location = nullptr;
	}
	if (err == NoError && lp.index > 0)
		return lp;
	return std::nullopt;
}


static GS::Array<API_LibPart> ScanLibParts (const std::function<bool (const API_LibPart&)>& pred, UIndex maxResults)
{
	GS::Array<API_LibPart> result;
	Int32 count = 0;
	if (ACAPI_LibPart_GetNum (&count) != NoError)
		return result;
	for (Int32 i = 1; i <= count && result.GetSize () < maxResults; ++i) {
		API_LibPart lp;
		BNZeroMemory (&lp, sizeof (lp));
		lp.index = i;
		GSErrCode err = ACAPI_LibPart_Get (&lp);
		if (lp.location != nullptr) {
			delete lp.location;
			lp.location = nullptr;
		}
		if (err == NoError && pred (lp))
			result.Push (lp);
	}
	return result;
}


API_LibPart FindLibPart (const OS& os, const char* key, std::optional<API_LibTypeID> requiredType)
{
	if (!os.Contains (key))
		Fail ("Missing required library part field '" + GS::UniString (key) + "'.");

	std::optional<API_LibPart> found;
	GS::UniString name;

	auto checkType = [&] (const API_LibPart& lp) {
		if (requiredType.has_value () && lp.typeID != *requiredType) {
			Fail ("Library part '" + GS::UniString (lp.docu_UName) + "' is a " + LibTypeName (lp.typeID) +
				  ", but a " + LibTypeName (*requiredType) + " is required.", APIERR_NOTSUBTYPEOF);
		}
	};

	if (IsNumber (os, key)) {
		API_LibPart lp = GetLibPartByIndex (GetInt (os, key));
		checkType (lp);
		return lp;
	}
	if (os.IsString (key)) {
		name = GetString (os, key);
	} else if (os.IsObject (key)) {
		OS ref = GetObject (os, key);
		if (IsNumber (ref, "index")) {
			API_LibPart lp = GetLibPartByIndex (GetInt (ref, "index"));
			checkType (lp);
			return lp;
		}
		if (ref.Contains ("guid")) {
			GS::UniString unId = GetString (ref, "guid");
			found = SearchByUnId (unId);
			if (!found.has_value ()) {
				// Match by main GUID only (revision may differ).
				GS::UniString mainGuid = unId;
				mainGuid.Trim ('{');
				Int32 close = (Int32) mainGuid.FindFirst ('}');
				if (close > 0)
					mainGuid = mainGuid.GetSubstring (0, close);
				auto scan = ScanLibParts ([&] (const API_LibPart& lp) {
					return GS::UniString (lp.ownUnID).Contains (mainGuid, GS::UniString::CaseInsensitive);
				}, 1);
				if (!scan.IsEmpty ())
					found = scan[0];
			}
			if (!found.has_value ())
				Fail ("Library part with GUID " + unId + " not found in the loaded libraries.", APIERR_BADNAME);
			checkType (*found);
			return *found;
		}
		name = GetString (ref, "name");
	} else {
		Fail ("Field '" + GS::UniString (key) + "' must be a library part name, index or {name|guid|index}.");
	}

	// 1) exact name (ACAPI search), 2) case-insensitive exact / file name, 3) substring
	found = SearchByName (name);
	if (found.has_value () && (!requiredType.has_value () || found->typeID == *requiredType))
		return *found;

	auto matches = ScanLibParts ([&] (const API_LibPart& lp) {
		if (requiredType.has_value () && lp.typeID != *requiredType)
			return false;
		GS::UniString docu (lp.docu_UName);
		GS::UniString file (lp.file_UName);
		return EqualsIgnoreCase (docu, name) || EqualsIgnoreCase (file, name) || EqualsIgnoreCase (file, name + ".gsm");
	}, 1);
	if (!matches.IsEmpty ())
		return matches[0];

	auto partial = ScanLibParts ([&] (const API_LibPart& lp) {
		if (requiredType.has_value () && lp.typeID != *requiredType)
			return false;
		if (!lp.isPlaceable)
			return false;
		return GS::UniString (lp.docu_UName).Contains (name, GS::UniString::CaseInsensitive);
	}, 8);
	if (partial.GetSize () == 1)
		return partial[0];

	GS::UniString msg = "Library part '" + name + "' not found";
	if (requiredType.has_value ())
		msg += " (type " + LibTypeName (*requiredType) + ")";
	if (!partial.IsEmpty ()) {
		msg += ". Did you mean one of: ";
		for (UIndex i = 0; i < partial.GetSize (); ++i) {
			if (i > 0) msg += ", ";
			msg += "'" + GS::UniString (partial[i].docu_UName) + "'";
		}
	}
	msg += ". Use search_library_parts to find exact names (they are localized).";
	Fail (msg, APIERR_BADNAME);
}

// --- Params ------------------------------------------------------------------------

ParamsHandle::~ParamsHandle ()
{
	if (params != nullptr)
		ACAPI_DisposeAddParHdl (&params);
}


API_AddParType** ParamsHandle::Release ()
{
	API_AddParType** p = params;
	params = nullptr;
	return p;
}


void ParamsHandle::Reset (API_AddParType** p)
{
	if (params != nullptr)
		ACAPI_DisposeAddParHdl (&params);
	params = p;
}


void LoadDefaultParams (Int32 libInd, ParamsHandle& out)
{
	double a = 0, b = 0;
	Int32 addParNum = 0;
	API_AddParType** addPars = nullptr;
	Check (ACAPI_LibPart_GetParams (libInd, &a, &b, &addParNum, &addPars), "Cannot read parameters of library part " + GS::ValueToUniString (libInd));
	out.Reset (addPars);
	out.a = a;
	out.b = b;
}


GS::UniString ParamTypeName (API_AddParID typeID)
{
	switch (typeID) {
		case APIParT_Integer:			return "Integer";
		case APIParT_Length:			return "Length";
		case APIParT_Angle:				return "Angle";
		case APIParT_RealNum:			return "RealNum";
		case APIParT_LightSw:			return "LightSwitch";
		case APIParT_ColRGB:			return "ColorRGB";
		case APIParT_Intens:			return "Intensity";
		case APIParT_LineTyp:			return "LineType";
		case APIParT_Mater:				return "Surface";
		case APIParT_FillPat:			return "FillPattern";
		case APIParT_PenCol:			return "Pen";
		case APIParT_CString:			return "String";
		case APIParT_Boolean:			return "Boolean";
		case APIParT_Separator:			return "Separator";
		case APIParT_Title:				return "Title";
		case APIParT_BuildingMaterial:	return "BuildingMaterial";
		case APIParT_Profile:			return "Profile";
		case APIParT_Dictionary:		return "Dictionary";
		default:						return "Unknown";
	}
}


static bool IsNumericParam (API_AddParID t)
{
	return t != APIParT_CString && t != APIParT_Separator && t != APIParT_Title && t != APIParT_Dictionary;
}


static void AddScalarValue (OS& out, const char* key, const API_AddParType& p, double real)
{
	switch (p.typeID) {
		case APIParT_Angle:		out.Add (key, RadToDeg (real)); break;
		case APIParT_Boolean:
		case APIParT_LightSw:	out.Add (key, real != 0.0); break;
		case APIParT_Integer:
		case APIParT_LineTyp:
		case APIParT_Mater:
		case APIParT_FillPat:
		case APIParT_PenCol:
		case APIParT_BuildingMaterial:
		case APIParT_Profile:	out.Add (key, (Int32) std::lround (real)); break;
		default:				out.Add (key, real); break;
	}
}


GS::Array<OS> ParamsToJson (API_AddParType** params, bool includeHidden, const GS::Array<GS::UniString>& names)
{
	GS::Array<OS> out;
	if (params == nullptr)
		return out;
	const Int32 n = BMhGetSize (reinterpret_cast<GSHandle> (params)) / sizeof (API_AddParType);
	for (Int32 i = 0; i < n; ++i) {
		const API_AddParType& p = (*params)[i];
		GS::UniString pname (p.name, CC_UTF8);
		if (!names.IsEmpty ()) {
			bool wanted = false;
			for (const GS::UniString& nm : names)
				if (EqualsIgnoreCase (nm, pname)) { wanted = true; break; }
			if (!wanted)
				continue;
		}
		if (p.typeID == APIParT_Separator || p.typeID == APIParT_Title)
			continue;
		const bool hidden = (p.flags & API_ParFlg_Hidden) != 0;
		if (hidden && !includeHidden && names.IsEmpty ())
			continue;

		OS item;
		item.Add ("name", pname);
		item.Add ("type", ParamTypeName (p.typeID));
		item.Add ("description", GS::UniString (p.uDescname));
		if (hidden)
			item.Add ("hidden", true);
		if ((p.flags & API_ParFlg_Disabled) != 0)
			item.Add ("disabled", true);

		if (p.typeMod == API_ParArray) {
			item.Add ("arrayDims", OS ("dim1", p.dim1, "dim2", p.dim2));
			GS::Array<OS> rows;
			if (p.value.array != nullptr) {
				if (IsNumericParam (p.typeID)) {
					const double* vals = reinterpret_cast<const double*> (*p.value.array);
					for (Int32 r = 0; r < p.dim1; ++r) {
						GS::Array<double> row;
						for (Int32 c = 0; c < std::max<Int32> (p.dim2, 1); ++c)
							row.Push (vals[r * std::max<Int32> (p.dim2, 1) + c]);
						rows.Push (OS ("values", row));
					}
				} else {
					const GS::uchar_t* s = reinterpret_cast<const GS::uchar_t*> (*p.value.array);
					for (Int32 r = 0; r < p.dim1; ++r) {
						GS::Array<GS::UniString> row;
						for (Int32 c = 0; c < std::max<Int32> (p.dim2, 1); ++c) {
							GS::UniString str (s);
							row.Push (str);
							s += GS::ucslen32 (s) + 1;
						}
						rows.Push (OS ("values", row));
					}
				}
			}
			item.Add ("value", rows);
		} else if (p.typeID == APIParT_CString) {
			item.Add ("value", GS::UniString (p.value.uStr));
		} else if (p.typeID == APIParT_Dictionary) {
			item.Add ("value", GS::UniString ("<dictionary>"));
		} else {
			AddScalarValue (item, "value", p, p.value.real);
			GS::UniString valueDesc (p.valueDescription);
			if (!valueDesc.IsEmpty ())
				item.Add ("valueDescription", valueDesc);
		}
		out.Push (item);
	}
	return out;
}


static API_AddParType* FindParam (API_AddParType** params, const GS::UniString& name)
{
	if (params == nullptr)
		return nullptr;
	const Int32 n = BMhGetSize (reinterpret_cast<GSHandle> (params)) / sizeof (API_AddParType);
	for (Int32 i = 0; i < n; ++i) {
		GS::UniString pname ((*params)[i].name, CC_UTF8);
		if (EqualsIgnoreCase (pname, name))
			return &(*params)[i];
	}
	return nullptr;
}


static double JsonToParamReal (const API_AddParType& p, const OS& values, const char* key)
{
	if (values.IsBool (key))
		return GetBool (values, key) ? 1.0 : 0.0;
	double v = GetDouble (values, key);
	if (p.typeID == APIParT_Angle)
		v = DegToRad (v);
	return v;
}


void ApplyParamValues (API_AddParType** params, const OS& values)
{
	GS::HashSet<GS::String> fieldNames = values.GetFieldNames ();
	for (const GS::String& field : fieldNames) {
		GS::UniString uname (field.ToCStr (), CC_UTF8);
		API_AddParType* p = FindParam (params, uname);
		if (p == nullptr)
			Fail ("GDL parameter '" + uname + "' does not exist in this library part. Use get_library_part_parameters / get_gdl_parameters to list names.", APIERR_BADNAME);
		const char* key = field.ToCStr ();
		if (p->typeMod == API_ParArray) {
			Fail ("Array parameter '" + uname + "' can only be changed with the script-based setter (set_gdl_parameters).", APIERR_NOTSUPPORTED);
		}
		if (p->typeID == APIParT_CString) {
			GS::UniString s = GetString (values, key);
			GS::ucsncpy (p->value.uStr, s.ToUStr ().Get (), API_UAddParStrLen - 1);
		} else if (IsNumericParam (p->typeID)) {
			p->value.real = JsonToParamReal (*p, values, key);
		} else {
			Fail ("Parameter '" + uname + "' of type " + ParamTypeName (p->typeID) + " cannot be set.", APIERR_NOTSUPPORTED);
		}
	}
}


namespace {
	struct ParamSession {
		bool open = false;
		~ParamSession () { if (open) ACAPI_Goodies (APIAny_CloseParametersID); }
	};

	API_AddParType** RunParamChanges (API_ParamOwnerType& owner, API_AddParType** currentParams, const OS& values, double* outA, double* outB)
	{
		ParamSession session;
		Check (ACAPI_Goodies (APIAny_OpenParametersID, &owner), "Cannot open GDL parameters");
		session.open = true;

		GS::HashSet<GS::String> fieldNames = values.GetFieldNames ();
		for (const GS::String& field : fieldNames) {
			GS::UniString uname (field.ToCStr (), CC_UTF8);
			const char* key = field.ToCStr ();
			API_AddParType* p = FindParam (currentParams, uname);
			if (p == nullptr && !EqualsIgnoreCase (uname, "A") && !EqualsIgnoreCase (uname, "B"))
				Fail ("GDL parameter '" + uname + "' does not exist. Use get_gdl_parameters to list names.", APIERR_BADNAME);

			API_ChangeParamType chg;
			BNZeroMemory (&chg, sizeof (chg));
			CHTruncate (field.ToCStr (), chg.name, sizeof (chg.name));

			GS::UniString strValue;
			if (p != nullptr && p->typeMod == API_ParArray) {
				// Array: accept [[...],[...]] as {"values": [...]} rows or flat numeric array.
				GS::Array<double> flat = GetNumberArray (values, key, false);
				if (flat.IsEmpty ()) {
					GS::Array<OS> rows = GetObjectArray (values, key, true);
					for (const OS& row : rows)
						for (double v : GetNumberArray (row, "values", true))
							flat.Push (v);
				}
				const Int32 dim2 = std::max<Int32> (p->dim2, 1);
				for (UIndex i = 0; i < flat.GetSize (); ++i) {
					API_ChangeParamType c = chg;
					c.ind1 = (Int32) (i / dim2) + 1;
					c.ind2 = (Int32) (i % dim2) + 1;
					c.realValue = p->typeID == APIParT_Angle ? DegToRad (flat[i]) : flat[i];
					Check (ACAPI_Goodies (APIAny_ChangeAParameterID, &c), "Cannot change array parameter '" + uname + "'");
				}
				continue;
			}

			if (values.IsString (key)) {
				strValue = GetString (values, key);
				chg.uStrValue = const_cast<GS::uchar_t*> (strValue.ToUStr ().Get ());
				Check (ACAPI_Goodies (APIAny_ChangeAParameterID, &chg), "Cannot change parameter '" + uname + "'");
			} else {
				if (p != nullptr)
					chg.realValue = JsonToParamReal (*p, values, key);
				else
					chg.realValue = GetDouble (values, key);
				Check (ACAPI_Goodies (APIAny_ChangeAParameterID, &chg), "Cannot change parameter '" + uname + "'");
			}
		}

		API_GetParamsType getParams;
		BNZeroMemory (&getParams, sizeof (getParams));
		Check (ACAPI_Goodies (APIAny_GetActParametersID, &getParams), "Cannot read back GDL parameters");
		if (outA) *outA = getParams.a;
		if (outB) *outB = getParams.b;
		return getParams.params;
	}
}


API_AddParType** ChangeParamsWithScript (const API_Guid& elemGuid, const API_ElemType& type, Int32 libInd, const OS& values)
{
	API_ElementMemo memo;
	BNZeroMemory (&memo, sizeof (memo));
	Check (ACAPI_Element_GetMemo (elemGuid, &memo, APIMemoMask_AddPars), "Cannot read GDL parameters of element");

	API_ParamOwnerType owner;
	BNZeroMemory (&owner, sizeof (owner));
	owner.guid = elemGuid;
	owner.libInd = 0;
	owner.type = type;

	API_AddParType** result = nullptr;
	try {
		result = RunParamChanges (owner, memo.params, values, nullptr, nullptr);
	} catch (...) {
		ACAPI_DisposeElemMemoHdls (&memo);
		throw;
	}
	ACAPI_DisposeElemMemoHdls (&memo);
	return result;
}


API_AddParType** ChangeParamsWithScriptForLibPart (Int32 libInd, const API_ElemType& type, const OS& values, double* outA, double* outB)
{
	ParamsHandle defaults;
	LoadDefaultParams (libInd, defaults);

	API_ParamOwnerType owner;
	BNZeroMemory (&owner, sizeof (owner));
	owner.guid = APINULLGuid;
	owner.libInd = libInd;
	owner.type = type;
	return RunParamChanges (owner, defaults.Get (), values, outA, outB);
}

} // namespace cc
