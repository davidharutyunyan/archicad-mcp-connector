// *****************************************************************************
// Enums.hpp — string <-> enum tables for JSON fields.
//
//   static const cc::NamedValue kRefLines[] = { {"Outside", APIWallRefLine_Outside}, ... };
//   wall.referenceLineLocation = (API_WallReferenceLineLocationID) cc::ParseNamed (kRefLines, spec, "referenceLine");
//   out.Add ("referenceLine", cc::NameOf (kRefLines, wall.referenceLineLocation));
// *****************************************************************************

#pragma once

#include "Core/Json.hpp"

namespace cc {

struct NamedValue {
	const char*	name;
	Int32		value;
};

template <size_t N>
Int32 ParseNamedString (const NamedValue (&table)[N], const GS::UniString& s, const char* key)
{
	for (const NamedValue& nv : table) {
		if (EqualsIgnoreCase (s, nv.name))
			return nv.value;
	}
	GS::UniString allowed;
	for (const NamedValue& nv : table) {
		if (!allowed.IsEmpty ()) allowed += ", ";
		allowed += nv.name;
	}
	Fail ("Invalid value '" + s + "' for '" + GS::UniString (key) + "'. Allowed: " + allowed + ".");
}

// Reads os[key] (string, case-insensitive) and maps it through the table. Throws listing allowed values.
template <size_t N>
Int32 ParseNamed (const NamedValue (&table)[N], const OS& os, const char* key)
{
	return ParseNamedString (table, GetString (os, key), key);
}

template <size_t N>
std::optional<Int32> OptNamed (const NamedValue (&table)[N], const OS& os, const char* key)
{
	if (!os.Contains (key))
		return std::nullopt;
	return ParseNamed (table, os, key);
}

template <size_t N>
GS::UniString NameOf (const NamedValue (&table)[N], Int32 value)
{
	for (const NamedValue& nv : table) {
		if (nv.value == value)
			return nv.name;
	}
	return GS::ValueToUniString (value);
}

} // namespace cc
