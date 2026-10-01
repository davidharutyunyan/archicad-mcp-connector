// *****************************************************************************
// DraftingText — Text (API_TextID) and Label (API_LabelID) adapters.
//
// Text content is written as UTF-16 into memo.textContent together with one paragraph per line
// (split at "\n") and one run per style segment — the structure of the DevKit's multistyle text
// example. Every run carries its own pen / font / size / face / effects: once paragraphs exist,
// Archicad ignores the element-level style fields, so a style-only change patches the runs of the
// existing content in place (no re-encoding) and a content change rebuilds all runs.
//
// Text fields (create / modify; * = required on create):
//   position* {x,y}, text* (string, "\n" = new line) | runs* [{text, pen, font, size, bold, italic,
//   underline, strikeout, superscript, subscript}], style: pen, font (Font attribute name/index),
//   size (mm), bold, italic, underline, strikeout, superscript, subscript, justification
//   "Left"|"Center"|"Right"|"Full", lineSpacing, anchor "TopLeft".."BottomRight", angle (deg),
//   widthFactor, charSpacing, wrapWidth (mm, 0 = no wrapping), fixedSize, frame, framePen,
//   frameOffset, background, backgroundPen, alwaysReadable
//
// Label fields:
//   parent (associative label: GUID of the labelled element), labelClass "Text"|"Symbol",
//   begin (leader start / arrow point; default: a reference point of the parent), middle, end
//   (text end of the leader; default: Archicad's default position for associative labels),
//   leader {show, shape "Segmented"|"Spline"|"SquareRoot", pen, lineType, arrows {...}, anchor
//   "Middle"|"Top"|"Bottom"|"Underlined", squareRootAngle}, frame, frameOffset, textOrientation
//   "Parallel"|"Horizontal"|"Vertical"|"General", alwaysReadable, angle,
//   text labels: text | runs + all text style fields above (except position/anchor),
//   symbol labels: libraryPart (Label library part), gdlParameters {name: value}, pen, font, size,
//   bold/italic/underline/strikeout/superscript/subscript, background, backgroundPen, wrapText
// + common: layer, storyIndex, renovationStatus, elementId, drawIndex
// *****************************************************************************

#include "Commands/DraftingCommon.hpp"
#include "Core/Command.hpp"
#include "Core/Elements.hpp"
#include "Core/Enums.hpp"
#include "Core/LibParts.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace cc {
namespace drafting {

namespace {

constexpr double kEps = 1e-9;

const NamedValue kAnchors[] = {
	{ "TopLeft",		APIAnc_LT },
	{ "TopCenter",		APIAnc_MT },
	{ "TopRight",		APIAnc_RT },
	{ "MiddleLeft",		APIAnc_LM },
	{ "Center",			APIAnc_MM },
	{ "MiddleRight",	APIAnc_RM },
	{ "BottomLeft",		APIAnc_LB },
	{ "BottomCenter",	APIAnc_MB },
	{ "BottomRight",	APIAnc_RB },
};

const NamedValue kJustifications[] = {
	{ "Left",		APIJust_Left },
	{ "Center",		APIJust_Center },
	{ "Right",		APIJust_Right },
	{ "Full",		APIJust_Full },
	{ "Justified",	APIJust_Full },
};

const NamedValue kLabelClasses[] = {
	{ "Text",		APILblClass_Text },
	{ "Symbol",		APILblClass_Symbol },
};

const NamedValue kLeaderShapes[] = {
	{ "Segmented",	API_Segmented },
	{ "Spline",		API_Splinear },
	{ "SquareRoot",	API_SquareRoot },
	{ "Straight",	API_Segmented },
};

const NamedValue kLabelAnchors[] = {
	{ "Middle",		APILbl_MiddleAnchor },
	{ "Top",		APILbl_TopAnchor },
	{ "Bottom",		APILbl_BottomAnchor },
	{ "Underlined",	APILbl_Underlined },
};

const NamedValue kTextWays[] = {
	{ "Parallel",		APIDir_Parallel },
	{ "Horizontal",		APIDir_Horizontal },
	{ "Vertical",		APIDir_Vertical },
	{ "General",		APIDir_General },
	{ "Custom",			APIDir_General },
};

// --- Styled runs ----------------------------------------------------------------------

struct StyledRun {
	std::vector<GS::uchar_t>	text;		// UTF-16 units, "\n" separates paragraphs
	short						pen = 1;
	short						font = 1;
	unsigned short				faceBits = 0;
	unsigned short				effectBits = 0;
	double						size = 2.5;
};


std::vector<GS::uchar_t> ToUnits (const GS::UniString& s)
{
	std::vector<GS::uchar_t> units;
	const USize n = s.GetLength ();
	const GS::uchar_t* p = s.ToUStr ().Get ();
	units.reserve (n);
	for (USize i = 0; i < n; ++i) {
		const GS::uchar_t c = p[i];
		if (c == '\r') {
			if (i + 1 < n && p[i + 1] == '\n')
				continue;					// "\r\n" -> "\n"
			units.push_back ('\n');			// lone "\r" -> "\n"
		} else if (c != 0) {
			units.push_back (c);
		}
	}
	return units;
}


GS::UniString FromUnits (const GS::uchar_t* p, size_t count)
{
	if (count == 0)
		return GS::UniString ();
	return GS::UniString (reinterpret_cast<const GS::UniChar::Layout*> (p), (USize) count);
}


void SetBit (unsigned short& bits, unsigned short bit, bool on)
{
	if (on) bits = (unsigned short) (bits | bit); else bits = (unsigned short) (bits & ~bit);
}


// Applies the run-level style fields present in spec to a run.
void ApplyRunStyle (StyledRun& r, const OS& spec)
{
	if (auto p = OptPen (spec, "pen"))					r.pen = *p;
	if (auto f = OptAttr (API_FontID, spec, "font"))	r.font = (short) *f;
	if (auto s = OptPositive (spec, "size"))			r.size = *s;
	if (auto b = OptBool (spec, "bold"))				SetBit (r.faceBits, APIFace_Bold, *b);
	if (auto b = OptBool (spec, "italic"))				SetBit (r.faceBits, APIFace_Italic, *b);
	if (auto b = OptBool (spec, "underline"))			SetBit (r.faceBits, APIFace_Underline, *b);
	if (auto b = OptBool (spec, "strikeout"))			SetBit (r.effectBits, APIEffect_StrikeOut, *b);
	if (auto b = OptBool (spec, "superscript"))		SetBit (r.effectBits, APIEffect_SuperScript, *b);
	if (auto b = OptBool (spec, "subscript"))			SetBit (r.effectBits, APIEffect_SubScript, *b);
}


void ApplyRunStyleToApiRun (API_RunType& run, const OS& spec)
{
	StyledRun r;
	r.pen = run.pen; r.font = run.font; r.faceBits = run.faceBits; r.effectBits = run.effectBits; r.size = run.size;
	ApplyRunStyle (r, spec);
	run.pen = r.pen; run.font = r.font; run.faceBits = r.faceBits; run.effectBits = r.effectBits; run.size = r.size;
}


StyledRun BaseRun (const API_TextType& t)
{
	StyledRun r;
	r.pen = t.pen;
	r.font = t.font;
	r.faceBits = t.faceBits;
	r.effectBits = (unsigned short) t.effectsBits;
	r.size = t.size;
	return r;
}


void AddRunStyle (OS& out, short pen, short font, unsigned short faceBits, unsigned short effectBits, double size, bool fontRef)
{
	out.Add ("pen", (Int32) pen);
	if (fontRef)
		out.Add ("font", AttrRef (API_FontID, font));
	else
		out.Add ("font", (Int32) font);
	out.Add ("size", size);
	out.Add ("bold", (faceBits & APIFace_Bold) != 0);
	out.Add ("italic", (faceBits & APIFace_Italic) != 0);
	out.Add ("underline", (faceBits & APIFace_Underline) != 0);
	out.Add ("strikeout", (effectBits & APIEffect_StrikeOut) != 0);
	out.Add ("superscript", (effectBits & APIEffect_SuperScript) != 0);
	out.Add ("subscript", (effectBits & APIEffect_SubScript) != 0);
}

// --- Memo text helpers -------------------------------------------------------------------

void KillTextMemo (API_ElementMemo& memo)
{
	if (memo.textContent != nullptr)
		BMKillHandle (reinterpret_cast<GSHandle*> (&memo.textContent));
	if (memo.paragraphs != nullptr)
		ACAPI_DisposeParagraphsHdl (&memo.paragraphs);
	if (memo.textLineStarts != nullptr)
		BMKillHandle (reinterpret_cast<GSHandle*> (&memo.textLineStarts));
	memo.textContent = nullptr;
	memo.paragraphs = nullptr;
	memo.textLineStarts = nullptr;
}


Int32 ParagraphCount (const API_ElementMemo& memo)
{
	if (memo.paragraphs == nullptr)
		return 0;
	return (Int32) (BMGetHandleSize (reinterpret_cast<GSConstHandle> (memo.paragraphs)) / (GSSize) sizeof (API_ParagraphType));
}


Int32 RunCount (const API_ParagraphType& par)
{
	if (par.run == nullptr)
		return 0;
	return (Int32) (BMGetPtrSize (reinterpret_cast<GSConstPtr> (par.run)) / (GSSize) sizeof (API_RunType));
}


// True when the memo holds no text (encoding-agnostic: works for UTF-8 and UTF-16 content).
bool MemoTextIsEmpty (const API_ElementMemo& memo)
{
	if (memo.textContent == nullptr)
		return true;
	const GSSize size = BMGetHandleSize (reinterpret_cast<GSConstHandle> (memo.textContent));
	if (size <= 0)
		return true;
	const unsigned char* b = reinterpret_cast<const unsigned char*> (*memo.textContent);
	return b[0] == 0 && (size < 2 || b[1] == 0);
}


// Writes runs (UTF-16) + paragraphs (one per line) into memo and updates the text struct.
void WriteTextContent (API_ElementMemo& memo, API_TextType& t, const std::vector<StyledRun>& runs, API_JustID just, double spacing)
{
	struct Segment { Int32 from; Int32 len; const StyledRun* style; };
	struct Paragraph { Int32 start; Int32 range; std::vector<Segment> segs; };

	std::vector<GS::uchar_t> full;
	std::vector<Paragraph> pars (1);
	pars[0].start = 0;
	const StyledRun* current = runs.empty () ? nullptr : &runs[0];
	for (const StyledRun& r : runs) {
		current = &r;
		Int32 segStart = (Int32) full.size ();
		for (GS::uchar_t c : r.text) {
			if (c == '\n') {
				const Int32 pos = (Int32) full.size ();
				if (pos > segStart)
					pars.back ().segs.push_back ({ segStart - pars.back ().start, pos - segStart, &r });
				if (pars.back ().segs.empty ())
					pars.back ().segs.push_back ({ 0, 0, &r });
				pars.back ().range = pos - pars.back ().start;
				full.push_back ('\n');
				Paragraph next;
				next.start = (Int32) full.size ();
				next.range = 0;
				pars.push_back (next);
				segStart = next.start;
			} else {
				full.push_back (c);
			}
		}
		const Int32 pos = (Int32) full.size ();
		if (pos > segStart)
			pars.back ().segs.push_back ({ segStart - pars.back ().start, pos - segStart, &r });
	}
	pars.back ().range = (Int32) full.size () - pars.back ().start;
	if (pars.back ().segs.empty ()) {
		if (current == nullptr)
			Fail ("Text content is empty.");
		pars.back ().segs.push_back ({ 0, 0, current });
	}
	if (full.empty ())
		Fail ("Text content must not be empty.");

	KillTextMemo (memo);

	memo.textContent = BMAllocateHandle ((GSSize) ((full.size () + 1) * sizeof (GS::uchar_t)), ALLOCATE_CLEAR, 0);
	if (memo.textContent == nullptr)
		Fail ("Out of memory.", APIERR_MEMFULL);
	std::memcpy (*memo.textContent, full.data (), full.size () * sizeof (GS::uchar_t));

	const Int32 nPar = (Int32) pars.size ();
	memo.paragraphs = reinterpret_cast<API_ParagraphType**> (BMAllocateHandle ((GSSize) (nPar * sizeof (API_ParagraphType)), ALLOCATE_CLEAR, 0));
	if (memo.paragraphs == nullptr)
		Fail ("Out of memory.", APIERR_MEMFULL);
	for (Int32 p = 0; p < nPar; ++p) {
		API_ParagraphType& par = (*memo.paragraphs)[p];
		const Paragraph& src = pars[p];
		par.from = src.start;
		par.range = src.range;
		par.just = just;
		par.firstIndent = 0.0;
		par.indent = 0.0;
		par.rightIndent = 0.0;
		par.spacing = spacing;
		par.tab = reinterpret_cast<API_TabType*> (BMAllocatePtr (sizeof (API_TabType), ALLOCATE_CLEAR, 0));
		par.run = reinterpret_cast<API_RunType*> (BMAllocatePtr ((GSSize) (src.segs.size () * sizeof (API_RunType)), ALLOCATE_CLEAR, 0));
		if (par.tab == nullptr || par.run == nullptr)
			Fail ("Out of memory.", APIERR_MEMFULL);	// partial allocations are released with the memo
		par.tab[0].type = APITab_Left;
		par.tab[0].pos = 0.0;
		for (size_t s = 0; s < src.segs.size (); ++s) {
			const Segment& seg = src.segs[s];
			API_RunType& run = par.run[s];
			run.from = seg.from;
			run.range = seg.len;
			run.pen = seg.style->pen;
			run.faceBits = seg.style->faceBits;
			run.font = seg.style->font;
			run.effectBits = seg.style->effectBits;
			run.size = seg.style->size;
		}
		if (src.range > 0) {
			par.eolPos = reinterpret_cast<Int32*> (BMAllocatePtr (sizeof (Int32), ALLOCATE_CLEAR, 0));
			if (par.eolPos == nullptr)
				Fail ("Out of memory.", APIERR_MEMFULL);
			par.eolPos[0] = src.range - 1;
		}
	}

	// Element-level fields mirror the first run (used by readers that ignore the runs).
	const StyledRun& first = *pars[0].segs[0].style;
	t.pen = first.pen;
	t.font = first.font;
	t.faceBits = first.faceBits;
	t.effectsBits = first.effectBits;
	t.size = first.size;
	t.just = just;
	t.spacing = spacing;
	t.charCode = CC_UniCode;
	t.multiStyle = true;
	t.useEolPos = false;			// let Archicad compute the line breaks
	t.nLine = nPar;
}


// Decoded text content with its runs.
struct TextContent {
	GS::UniString				text;
	std::vector<StyledRun>		runs;
	API_JustID					just = APIJust_Left;
	double						spacing = 0.0;
	bool						hasParagraphs = false;
};


bool LooksLikeUtf16 (const unsigned char* b, GSSize size)
{
	// Mostly-ASCII UTF-16LE text has a zero high byte in its first unit; UTF-8 never has an
	// embedded zero before its terminator.
	return size >= 4 && b[0] != 0 && b[1] == 0 && b[2] != 0;
}


// Reads text + runs from a memo filled by ACAPI_Element_GetMemo.
TextContent DecodeTextMemo (const API_ElementMemo& memo, const API_TextType& t, bool preferUtf16)
{
	TextContent out;
	out.just = t.just;
	out.spacing = t.spacing;

	std::vector<GS::uchar_t> units;		// UTF-16 units when utf16, else bytes widened
	std::vector<char> bytes;
	bool utf16 = preferUtf16;
	if (memo.textContent != nullptr) {
		const GSSize size = BMGetHandleSize (reinterpret_cast<GSConstHandle> (memo.textContent));
		const unsigned char* b = reinterpret_cast<const unsigned char*> (*memo.textContent);
		if (!utf16 && LooksLikeUtf16 (b, size))
			utf16 = true;
		if (utf16) {
			const GSSize count = size / (GSSize) sizeof (GS::uchar_t);
			const GS::uchar_t* p = reinterpret_cast<const GS::uchar_t*> (*memo.textContent);
			for (GSSize i = 0; i < count && p[i] != 0; ++i)
				units.push_back (p[i]);
		} else {
			for (GSSize i = 0; i < size && b[i] != 0; ++i)
				bytes.push_back ((char) b[i]);
		}
	}
	const Int32 unitCount = utf16 ? (Int32) units.size () : (Int32) bytes.size ();
	auto substring = [&] (Int32 from, Int32 to) -> GS::UniString {
		from = std::max<Int32> (0, std::min (from, unitCount));
		to = std::max<Int32> (from, std::min (to, unitCount));
		if (to <= from)
			return GS::UniString ();
		if (utf16)
			return FromUnits (units.data () + from, (size_t) (to - from));
		std::string s (bytes.data () + from, (size_t) (to - from));
		return GS::UniString (s.c_str (), CC_UTF8);
	};
	out.text = substring (0, unitCount);

	struct Span { Int32 start; API_RunType run; };
	std::vector<Span> spans;
	const Int32 nPar = ParagraphCount (memo);
	for (Int32 p = 0; p < nPar; ++p) {
		const API_ParagraphType& par = (*memo.paragraphs)[p];
		if (p == 0) {
			out.just = par.just;
			out.spacing = par.spacing;
		}
		const Int32 nRun = RunCount (par);
		for (Int32 r = 0; r < nRun; ++r)
			spans.push_back ({ par.from + par.run[r].from, par.run[r] });
	}
	out.hasParagraphs = !spans.empty ();
	std::stable_sort (spans.begin (), spans.end (), [] (const Span& a, const Span& b) { return a.start < b.start; });

	if (spans.empty ()) {
		StyledRun r = BaseRun (t);
		r.text = ToUnits (out.text);
		out.runs.push_back (r);
	} else {
		// Each run spans to the start of the next one, so the concatenation gives back the whole
		// content including the line breaks between paragraphs.
		for (size_t i = 0; i < spans.size (); ++i) {
			const Int32 start = (i == 0) ? 0 : spans[i].start;
			const Int32 end = (i + 1 < spans.size ()) ? spans[i + 1].start : unitCount;
			StyledRun r;
			r.pen = spans[i].run.pen;
			r.font = spans[i].run.font;
			r.faceBits = spans[i].run.faceBits;
			r.effectBits = spans[i].run.effectBits;
			r.size = spans[i].run.size;
			r.text = ToUnits (substring (start, end));
			out.runs.push_back (r);
		}
	}
	// Normalize line breaks in the full text as well.
	std::vector<GS::uchar_t> norm = ToUnits (out.text);
	out.text = FromUnits (norm.data (), norm.size ());
	return out;
}


bool ReadTextContent (const API_Guid& guid, const API_TextType& t, TextContent& out)
{
	Memo memo;
	GSErrCode err = ACAPI_Element_GetMemo (guid, memo.Ptr (), APIMemoMask_TextContentUni | APIMemoMask_ParagraphUni);
	bool utf16 = true;
	if (err != NoError || memo->textContent == nullptr) {
		ACAPI_DisposeElemMemoHdls (memo.Ptr ());
		BNZeroMemory (memo.Ptr (), sizeof (API_ElementMemo));
		err = ACAPI_Element_GetMemo (guid, memo.Ptr (), APIMemoMask_TextContent | APIMemoMask_Paragraph);
		utf16 = false;
		if (err != NoError)
			return false;
	}
	out = DecodeTextMemo (*memo, t, utf16);
	return true;
}

// --- Text fields (shared by Text elements and text labels) ------------------------------

// Element-level style (returns true when a run-level field was given).
bool ApplyElementTextStyle (API_Element& e, API_Element* mask, API_TextType& t, const OS& spec)
{
	StyledRun r = BaseRun (t);
	ApplyRunStyle (r, spec);
	bool any = false;
	if (Has (spec, "pen"))		{ t.pen = r.pen; MaskField (mask, e, t.pen); any = true; }
	if (Has (spec, "font"))		{ t.font = r.font; MaskField (mask, e, t.font); any = true; }
	if (Has (spec, "size"))		{ t.size = r.size; MaskField (mask, e, t.size); any = true; }
	if (Has (spec, "bold") || Has (spec, "italic") || Has (spec, "underline")) {
		t.faceBits = r.faceBits; MaskField (mask, e, t.faceBits); any = true;
	}
	if (Has (spec, "strikeout") || Has (spec, "superscript") || Has (spec, "subscript")) {
		t.effectsBits = r.effectBits; MaskField (mask, e, t.effectsBits); any = true;
	}
	return any;
}


// Non-run text fields. Text elements: + position / anchor / frame / frameOffset;
// labels place their text by the leader and keep frame / frameOffset on the label.
void ApplyTextLayout (API_Element& e, API_Element* mask, API_TextType& t, const OS& spec, bool isLabel)
{
	if (!isLabel) {
		if (auto c = OptCoord (spec, "position"))	{ t.loc = *c; MaskField (mask, e, t.loc); }
		if (Has (spec, "anchor")) {
			t.anchor = (API_AnchorID) ParseNamed (kAnchors, spec, "anchor");
			MaskField (mask, e, t.anchor);
		}
	} else if (Has (spec, "position") || Has (spec, "anchor")) {
		Fail ("Labels are placed by their leader: use 'begin' / 'middle' / 'end' instead of 'position' / 'anchor'.");
	}
	if (auto a = OptAngle (spec, "angle"))				{ t.angle = *a; MaskField (mask, e, t.angle); }
	if (auto v = OptPositive (spec, "widthFactor"))	{ t.widthFactor = *v; MaskField (mask, e, t.widthFactor); }
	if (auto v = OptPositive (spec, "charSpacing"))	{ t.charSpaceFactor = *v; MaskField (mask, e, t.charSpaceFactor); }
	if (auto v = OptNonNegative (spec, "wrapWidth")) {
		t.nonBreaking = *v <= kEps;
		t.width = *v;
		if (t.nonBreaking) t.height = 0.0;
		MaskField (mask, e, t.nonBreaking);
		MaskField (mask, e, t.width);
		MaskField (mask, e, t.height);
	}
	if (auto b = OptBool (spec, "fixedSize"))			{ t.fixedSize = *b; MaskField (mask, e, t.fixedSize); }
	if (auto p = OptPen (spec, "framePen"))			{ t.contourPen = *p; MaskField (mask, e, t.contourPen); }
	if (auto b = OptBool (spec, "background"))			{ t.usedFill = *b; MaskField (mask, e, t.usedFill); }
	if (auto p = OptPen (spec, "backgroundPen"))		{ t.fillPen = *p; MaskField (mask, e, t.fillPen); }
	if (auto b = OptBool (spec, "alwaysReadable"))		{ t.flipEnabled = *b; MaskField (mask, e, t.flipEnabled); }
	if (!isLabel) {
		if (auto b = OptBool (spec, "frame"))				{ t.usedContour = *b; MaskField (mask, e, t.usedContour); }
		if (auto v = OptNonNegative (spec, "frameOffset"))	{ t.contourOffset = *v; MaskField (mask, e, t.contourOffset); }
	}
}


void MaskContentFields (API_Element& e, API_Element* mask, API_TextType& t)
{
	MaskField (mask, e, t.pen);
	MaskField (mask, e, t.font);
	MaskField (mask, e, t.faceBits);
	MaskField (mask, e, t.effectsBits);
	MaskField (mask, e, t.size);
	MaskField (mask, e, t.just);
	MaskField (mask, e, t.spacing);
	MaskField (mask, e, t.charCode);
	MaskField (mask, e, t.multiStyle);
	MaskField (mask, e, t.useEolPos);
	MaskField (mask, e, t.nLine);
	MaskField (mask, e, t.nonBreaking);
	MaskField (mask, e, t.width);
	MaskField (mask, e, t.height);
}


// Content + style. existing: the element being modified (nullptr on create).
// memo: on create the memo passed to ACAPI_Element_Create (may hold tool-default content),
// on modify the blank memo of the core's ACAPI_Element_Change.
void ApplyTextContent (API_Element& e, API_Element* mask, API_TextType& t, const OS& spec, API_ElementMemo& memo,
					   UInt64& memoMask, const API_Guid* existing, bool contentRequired)
{
	if (Has (spec, "text") && Has (spec, "runs"))
		Fail ("Give either 'text' or 'runs', not both.");
	const bool contentGiven = Has (spec, "text") || Has (spec, "runs");
	const bool styleGiven = ApplyElementTextStyle (e, mask, t, spec);

	std::optional<API_JustID> just;
	if (Has (spec, "justification"))
		just = (API_JustID) ParseNamed (kJustifications, spec, "justification");
	std::optional<double> spacing = OptDouble (spec, "lineSpacing");
	if (just.has_value ())		{ t.just = *just; MaskField (mask, e, t.just); }
	if (spacing.has_value ())	{ t.spacing = *spacing; MaskField (mask, e, t.spacing); }

	if (contentGiven) {
		std::vector<StyledRun> runs;
		const StyledRun base = BaseRun (t);
		if (Has (spec, "text")) {
			StyledRun r = base;
			r.text = ToUnits (GetString (spec, "text"));
			runs.push_back (r);
		} else {
			GS::Array<OS> items = GetObjectArray (spec, "runs");
			if (items.IsEmpty ())
				Fail ("'runs' must contain at least one {text, ...style} item.");
			for (const OS& item : items) {
				StyledRun r = base;
				ApplyRunStyle (r, item);
				r.text = ToUnits (GetString (item, "text"));
				runs.push_back (r);
			}
		}
		const bool nonBreaking = t.nonBreaking;
		const double width = t.width;
		WriteTextContent (memo, t, runs, t.just, t.spacing);
		t.nonBreaking = nonBreaking;
		t.width = nonBreaking ? 0.0 : width;
		if (nonBreaking) t.height = 0.0;
		MaskContentFields (e, mask, t);
		// WriteTextContent writes UTF-16 (runs/paragraphs in UTF-16 units): ACAPI_Element_Change needs the
		// *Uni masks. The plain masks mean UTF-8 (verified live: APIERR_PAROVERFLOW on texts, the first
		// characters of Cyrillic labels garbled).
		memoMask |= APIMemoMask_TextContentUni | APIMemoMask_ParagraphUni;
		return;
	}

	if (!styleGiven && !just.has_value () && !spacing.has_value ()) {
		if (existing == nullptr && contentRequired && MemoTextIsEmpty (memo))
			Fail ("Text requires 'text' (use \"\\n\" for new lines) or 'runs' [{text, ...style}].");
		return;
	}

	// Style-only change: patch the runs of the existing content in place (keeps the encoding).
	if (existing != nullptr) {
		KillTextMemo (memo);
		Check (ACAPI_Element_GetMemo (*existing, &memo, APIMemoMask_TextContent | APIMemoMask_Paragraph),
			   "Cannot read the text content to restyle it (pass 'text' to replace the content)");
	} else if (contentRequired && MemoTextIsEmpty (memo)) {
		Fail ("Text requires 'text' (use \"\\n\" for new lines) or 'runs' [{text, ...style}].");
	}
	const Int32 nPar = ParagraphCount (memo);
	for (Int32 p = 0; p < nPar; ++p) {
		API_ParagraphType& par = (*memo.paragraphs)[p];
		if (just.has_value ())		par.just = *just;
		if (spacing.has_value ())	par.spacing = *spacing;
		const Int32 nRun = RunCount (par);
		for (Int32 r = 0; r < nRun; ++r)
			ApplyRunStyleToApiRun (par.run[r], spec);
	}
	if (existing != nullptr && memo.textContent != nullptr) {
		// Archicad rebuilds the line starts; a stale array makes the change silently ignored.
		if (memo.textLineStarts != nullptr)
			BMKillHandle (reinterpret_cast<GSHandle*> (&memo.textLineStarts));
		memo.textLineStarts = nullptr;
		memoMask |= APIMemoMask_TextContent | APIMemoMask_Paragraph;
	}
}


void AddTextJson (OS& out, const API_Guid& guid, const API_TextType& t, bool isLabel)
{
	TextContent content;
	const bool haveContent = ReadTextContent (guid, t, content);
	if (haveContent)
		out.Add ("text", content.text);

	// Style: the first run is authoritative for multistyle texts.
	if (haveContent && !content.runs.empty ()) {
		const StyledRun& f = content.runs[0];
		AddRunStyle (out, f.pen, f.font, f.faceBits, f.effectBits, f.size, true);
	} else {
		AddRunStyle (out, t.pen, t.font, t.faceBits, (unsigned short) t.effectsBits, t.size, true);
	}
	if (haveContent && content.runs.size () > 1) {
		GS::Array<OS> runs;
		for (const StyledRun& r : content.runs) {
			OS rj;
			rj.Add ("text", FromUnits (r.text.data (), r.text.size ()));
			AddRunStyle (rj, r.pen, r.font, r.faceBits, r.effectBits, r.size, false);
			runs.Push (rj);
		}
		out.Add ("runs", runs);
	}
	out.Add ("justification", NameOf (kJustifications, haveContent ? content.just : t.just));
	out.Add ("lineSpacing", haveContent ? content.spacing : t.spacing);
	if (!isLabel) {
		out.Add ("position", CoordObj (t.loc));
		out.Add ("anchor", NameOf (kAnchors, t.anchor));
	}
	AddAngle (out, "angle", t.angle);
	out.Add ("widthFactor", t.widthFactor);
	out.Add ("charSpacing", t.charSpaceFactor);
	out.Add ("wrapWidth", t.nonBreaking ? 0.0 : t.width);
	out.Add ("boxWidth", t.width);
	out.Add ("boxHeight", t.height);
	out.Add ("lineCount", (Int32) t.nLine);
	out.Add ("fixedSize", t.fixedSize);
	out.Add ("framePen", (Int32) t.contourPen);
	out.Add ("background", t.usedFill);
	out.Add ("backgroundPen", (Int32) t.fillPen);
	if (!isLabel) {
		out.Add ("frame", t.usedContour);
		out.Add ("frameOffset", t.contourOffset);
		out.Add ("alwaysReadable", t.flipEnabled);
		if (t.owner != APINULLGuid)
			out.Add ("owner", GuidStr (t.owner));
	}
}


// =============================================================================
// Text
// =============================================================================

API_Guid CreateText (const OS& spec)
{
	if (!Has (spec, "position"))
		Fail ("Text requires 'position' {x, y} (m) and 'text' (or 'runs').");
	if (!Has (spec, "text") && !Has (spec, "runs"))
		Fail ("Text requires 'text' (use \"\\n\" for new lines) or 'runs' [{text, ...style}].");
	API_Element e = NewElement (API_TextID);
	GetDefaults (e, nullptr);
	ApplyCommonFields (e, nullptr, spec);
	ApplyTextLayout (e, nullptr, e.text, spec, false);
	Memo memo;
	UInt64 memoMask = 0;
	ApplyTextContent (e, nullptr, e.text, spec, *memo, memoMask, nullptr, true);
	CheckDrafting (ACAPI_Element_Create (&e, memo.Ptr ()), "Cannot create text");
	return e.header.guid;
}


void SerializeText (const API_Element& e, OS& out)
{
	AddTextJson (out, e.header.guid, e.text, false);
}


void ModifyText (API_Element& e, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask, const OS& patch)
{
	ApplyTextLayout (e, &mask, e.text, patch, false);
	const API_Guid guid = e.header.guid;
	ApplyTextContent (e, &mask, e.text, patch, memo, memoMask, &guid, true);
}

// =============================================================================
// Label
// =============================================================================

// Temporarily switches off autotext resolution so tool-default label content keeps its
// autotext keys (as the DevKit's label example does).
class AutoTextOff {
public:
	AutoTextOff ()
	{
		bool on = false;
		if (ACAPI_Goodies (APIAny_GetAutoTextFlagID, &on) == NoError && on) {
			bool off = false;
			changed = ACAPI_Goodies (APIAny_ChangeAutoTextFlagID, &off) == NoError;
		}
	}
	~AutoTextOff ()
	{
		if (changed) {
			bool on = true;
			ACAPI_Goodies (APIAny_ChangeAutoTextFlagID, &on);
		}
	}
	AutoTextOff (const AutoTextOff&) = delete;
	AutoTextOff& operator= (const AutoTextOff&) = delete;
private:
	bool changed = false;
};


API_Coord Mid (const API_Coord& a, const API_Coord& b)
{
	return { (a.x + b.x) / 2.0, (a.y + b.y) / 2.0 };
}


// A point on / inside the labelled element used as the leader start when 'begin' is omitted.
std::optional<API_Coord> ParentReferencePoint (const API_Guid& guid)
{
	API_Element p = GetElement (guid);
	switch (p.header.type.typeID) {
		case API_WallID:		return Mid (p.wall.begC, p.wall.endC);
		case API_BeamID:		return Mid (p.beam.begC, p.beam.endC);
		case API_ColumnID:		return p.column.origoPos;
		case API_ObjectID:		return p.object.pos;
		case API_LampID:		return p.lamp.pos;
		case API_ZoneID:		return p.zone.pos;
		case API_LineID:		return Mid (p.line.begC, p.line.endC);
		case API_ArcID:			return p.arc.origC;
		case API_CircleID:		return p.circle.origC;
		case API_HotspotID:		return p.hotspot.pos;
		case API_TextID:		return p.text.loc;
		case API_WindowID:
		case API_DoorID: {
			const API_WindowType& w = p.header.type.typeID == API_WindowID ? p.window : p.door;
			if (w.owner == APINULLGuid || !ElementExists (w.owner))
				return std::nullopt;
			API_Element wall = GetElement (w.owner);
			if (wall.header.type.typeID != API_WallID)
				return std::nullopt;
			const double len = std::hypot (wall.wall.endC.x - wall.wall.begC.x, wall.wall.endC.y - wall.wall.begC.y);
			if (len < kEps)
				return wall.wall.begC;
			const double t = w.objLoc / len;		// approximate for curved walls (chord)
			return API_Coord { wall.wall.begC.x + (wall.wall.endC.x - wall.wall.begC.x) * t,
							   wall.wall.begC.y + (wall.wall.endC.y - wall.wall.begC.y) * t };
		}
		default:
			break;
	}
	// Polygon-based elements (slabs, roofs, meshes, hatches, polylines ...): centre of the bounding box.
	Memo memo;
	if (ACAPI_Element_GetMemo (guid, memo.Ptr (), APIMemoMask_Polygon) == NoError && memo->coords != nullptr) {
		const Int32 n = (Int32) (BMGetHandleSize (reinterpret_cast<GSConstHandle> (memo->coords)) / (GSSize) sizeof (API_Coord));
		double xMin = 0, xMax = 0, yMin = 0, yMax = 0;
		bool first = true;
		for (Int32 i = 1; i < n; ++i) {
			const API_Coord& c = (*memo->coords)[i];
			if (first || c.x < xMin) xMin = c.x;
			if (first || c.x > xMax) xMax = c.x;
			if (first || c.y < yMin) yMin = c.y;
			if (first || c.y > yMax) yMax = c.y;
			first = false;
		}
		if (!first)
			return API_Coord { (xMin + xMax) / 2.0, (yMin + yMax) / 2.0 };
	}
	return std::nullopt;
}


void ApplyLabelParams (API_Element& e, API_Element* mask, const OS& spec, API_ElementMemo& memo, UInt64& memoMask, const API_Guid* existing)
{
	API_ObjectType& sym = e.label.u.symbol;
	const bool lpGiven = Has (spec, "libraryPart");
	const bool paramsGiven = Has (spec, "gdlParameters");
	if (!lpGiven && !paramsGiven)
		return;
	OS values;
	if (paramsGiven)
		values = GetObject (spec, "gdlParameters");
	API_AddParType** params = nullptr;
	if (lpGiven) {
		API_LibPart lp = FindLibPart (spec, "libraryPart", APILib_LabelID);
		sym.libInd = lp.index;
		MaskField (mask, e, sym.libInd);
		double a = 0, b = 0;
		params = ChangeParamsWithScriptForLibPart (lp.index, e.header.type, values, &a, &b);
		if (existing == nullptr && a > 0 && b > 0 && (sym.xRatio <= 0 || sym.yRatio <= 0)) {
			sym.xRatio = a;
			sym.yRatio = b;
		}
	} else if (existing != nullptr) {
		params = ChangeParamsWithScript (*existing, e.header.type, sym.libInd, values);
	} else if (memo.params != nullptr) {
		// New label from the tool defaults: keep the tool's parameter values, change only the given ones.
		ApplyParamValues (memo.params, values);
		memoMask |= APIMemoMask_AddPars;
		return;
	} else {
		params = ChangeParamsWithScriptForLibPart (sym.libInd, e.header.type, values);
	}
	if (params == nullptr)
		Fail ("Cannot compute the label library part parameters.");
	if (memo.params != nullptr)
		ACAPI_DisposeAddParHdl (&memo.params);
	memo.params = params;
	memoMask |= APIMemoMask_AddPars;
}


// Symbol-label text style (stored in the label, not in the runs).
void ApplySymbolLabelStyle (API_Element& e, API_Element* mask, const OS& spec)
{
	API_LabelType& l = e.label;
	if (Has (spec, "runs") || Has (spec, "text"))
		Fail ("Symbol labels take their text from the library part: remove 'text' / 'runs' (or use labelClass 'Text').");
	if (auto p = OptPen (spec, "pen"))					{ l.u.symbol.pen = *p; MaskField (mask, e, l.u.symbol.pen); }
	if (auto f = OptAttr (API_FontID, spec, "font"))	{ l.font = (short) *f; MaskField (mask, e, l.font); }
	if (auto s = OptPositive (spec, "size"))			{ l.textSize = *s; MaskField (mask, e, l.textSize); }
	unsigned short effects = (unsigned short) l.effectsBits;
	bool faceChanged = false, effectsChanged = false;
	if (auto b = OptBool (spec, "bold"))		{ SetBit (l.faceBits, APIFace_Bold, *b); faceChanged = true; }
	if (auto b = OptBool (spec, "italic"))		{ SetBit (l.faceBits, APIFace_Italic, *b); faceChanged = true; }
	if (auto b = OptBool (spec, "underline"))	{ SetBit (l.faceBits, APIFace_Underline, *b); faceChanged = true; }
	if (auto b = OptBool (spec, "strikeout"))	{ SetBit (effects, APIEffect_StrikeOut, *b); effectsChanged = true; }
	if (auto b = OptBool (spec, "superscript"))	{ SetBit (effects, APIEffect_SuperScript, *b); effectsChanged = true; }
	if (auto b = OptBool (spec, "subscript"))	{ SetBit (effects, APIEffect_SubScript, *b); effectsChanged = true; }
	if (faceChanged)	MaskField (mask, e, l.faceBits);
	if (effectsChanged)	{ l.effectsBits = effects; MaskField (mask, e, l.effectsBits); }
	if (auto b = OptBool (spec, "background"))		{ l.useBgFill = *b; MaskField (mask, e, l.useBgFill); }
	if (auto p = OptPen (spec, "backgroundPen", 0))	{ l.fillBgPen = *p; MaskField (mask, e, l.fillBgPen); }
	if (auto b = OptBool (spec, "wrapText"))		{ l.nonBreaking = !*b; MaskField (mask, e, l.nonBreaking); }
	if (auto a = OptAngle (spec, "angle"))			{ l.u.symbol.angle = *a; MaskField (mask, e, l.u.symbol.angle); }
	for (const char* key : { "justification", "lineSpacing", "widthFactor", "charSpacing", "wrapWidth", "framePen", "fixedSize" }) {
		if (Has (spec, key))
			Fail ("'" + GS::UniString (key) + "' only applies to text labels (this is a symbol label; its library part draws the text).");
	}
}


void ApplyLeader (API_Element& e, API_Element* mask, const OS& leader)
{
	API_LabelType& l = e.label;
	if (auto b = OptBool (leader, "show"))			{ l.hasLeaderLine = *b; MaskField (mask, e, l.hasLeaderLine); }
	if (Has (leader, "shape")) {
		l.leaderShape = (API_LeaderLineShapeID) ParseNamed (kLeaderShapes, leader, "shape");
		MaskField (mask, e, l.leaderShape);
	}
	if (auto p = OptPen (leader, "pen"))			{ l.pen = *p; MaskField (mask, e, l.pen); }
	if (auto lt = OptAttr (API_LinetypeID, leader, "lineType")) { l.ltypeInd = *lt; MaskField (mask, e, l.ltypeInd); }
	if (Has (leader, "anchor")) {
		l.anchorPoint = (API_LblAnchorID) ParseNamed (kLabelAnchors, leader, "anchor");
		MaskField (mask, e, l.anchorPoint);
	}
	if (auto a = OptAngle (leader, "squareRootAngle"))	{ l.squareRootAngle = *a; MaskField (mask, e, l.squareRootAngle); }
	if (Has (leader, "arrows"))
		ApplyArrows (e, mask, GetObject (leader, "arrows"), l.arrowData);
}


// Common label fields (both classes) except class / parent.
void ApplyLabelFields (API_Element& e, API_Element* mask, const OS& spec, API_ElementMemo& memo, UInt64& memoMask, const API_Guid* existing)
{
	API_LabelType& l = e.label;
	if (existing != nullptr) {
		if (auto c = OptCoord (spec, "begin"))	{ l.begC = *c; MaskField (mask, e, l.begC); }
		if (auto c = OptCoord (spec, "middle"))	{ l.midC = *c; MaskField (mask, e, l.midC); }
		if (auto c = OptCoord (spec, "end"))	{ l.endC = *c; MaskField (mask, e, l.endC); }
	}
	OS leader;
	if (TryGetObject (spec, "leader", leader))
		ApplyLeader (e, mask, leader);
	if (auto b = OptBool (spec, "frame"))				{ l.framed = *b; MaskField (mask, e, l.framed); }
	if (auto v = OptNonNegative (spec, "frameOffset"))	{ l.contourOffset = *v; MaskField (mask, e, l.contourOffset); }
	if (Has (spec, "textOrientation")) {
		l.textWay = (API_DirID) ParseNamed (kTextWays, spec, "textOrientation");
		MaskField (mask, e, l.textWay);
	}
	if (auto b = OptBool (spec, "alwaysReadable")) {
		l.flipEnabled = *b;
		MaskField (mask, e, l.flipEnabled);
	}

	if (l.labelClass == APILblClass_Text) {
		for (const char* key : { "libraryPart", "gdlParameters", "wrapText" }) {
			if (Has (spec, key))
				Fail ("'" + GS::UniString (key) + "' only applies to symbol labels (labelClass 'Symbol').");
		}
		ApplyTextLayout (e, mask, l.u.text, spec, true);		// also mirrors alwaysReadable into the text
		if (Has (spec, "wrapWidth")) {
			// The label's own flag decides about wrapping ("Wrap Text"), not only the text's.
			l.nonBreaking = l.u.text.nonBreaking;
			MaskField (mask, e, l.nonBreaking);
		}
		ApplyTextContent (e, mask, l.u.text, spec, memo, memoMask, existing, true);
	} else {
		ApplySymbolLabelStyle (e, mask, spec);
		ApplyLabelParams (e, mask, spec, memo, memoMask, existing);
	}
}


// Switches a freshly defaulted label to the other class (the struct is a union).
void ConvertLabelClass (API_Element& e, API_LblClassID cls, API_ElementMemo& memo, const OS& spec)
{
	API_LabelType& l = e.label;
	const API_Elem_Head head = e.header;
	const short leaderPen = l.pen > 0 ? l.pen : 1;
	BNZeroMemory (&l.u, sizeof (l.u));
	l.u.head = head;
	l.labelClass = cls;
	if (cls == APILblClass_Symbol) {
		if (!Has (spec, "libraryPart"))
			Fail ("The Label tool default is a text label: a symbol label needs 'libraryPart' (a Label library part — use search_library_parts).");
		KillTextMemo (memo);
		l.u.symbol.pen = leaderPen;
		l.u.symbol.xRatio = 0.0;
		l.u.symbol.yRatio = 0.0;
	} else {
		if (memo.params != nullptr)
			ACAPI_DisposeAddParHdl (&memo.params);
		API_TextType& t = l.u.text;
		// Start the text style from the Text tool defaults (font, size, spacing, factors ...).
		API_Element textDefaults = NewElement (API_TextID);
		if (ACAPI_Element_GetDefaults (&textDefaults, nullptr) == NoError) {
			t = textDefaults.text;
			t.head = head;
			t.owner = APINULLGuid;
		} else {
			t.pen = leaderPen;
			t.font = l.font > 0 ? l.font : 1;
			t.size = l.textSize > 0 ? l.textSize : 2.5;
			t.faceBits = l.faceBits;
			t.effectsBits = l.effectsBits;
			t.just = APIJust_Left;
			t.spacing = -1.0;
			t.widthFactor = 1.0;
			t.charSpaceFactor = 1.0;
		}
		t.anchor = APIAnc_LB;
		t.nonBreaking = true;
		t.width = 0.0;
		t.height = 0.0;
		t.flipEnabled = l.flipEnabled;
	}
}


API_Guid CreateLabel (const OS& spec)
{
	API_Element e = NewElement (API_LabelID);
	std::optional<API_Guid> parent = OptGuid (spec, "parent");
	API_Elem_Head parentHead;
	BNZeroMemory (&parentHead, sizeof (parentHead));
	if (parent.has_value ()) {
		parentHead = GetHeader (*parent);
		e.label.parentType = parentHead.type;		// label defaults depend on the labelled element type
	}

	Memo memo;
	{
		AutoTextOff autoTextOff;
		GetDefaults (e, memo.Ptr ());
	}
	if (parent.has_value ()) {
		e.label.parent = *parent;
		e.label.parentType = parentHead.type;
		e.header.floorInd = parentHead.floorInd;
	} else {
		e.label.parent = APINULLGuid;
		e.label.parentType = API_ElemType (API_ZombieElemID);
	}
	ApplyCommonFields (e, nullptr, spec);

	if (Has (spec, "labelClass")) {
		const API_LblClassID cls = (API_LblClassID) ParseNamed (kLabelClasses, spec, "labelClass");
		if (cls != e.label.labelClass)
			ConvertLabelClass (e, cls, *memo, spec);
	}
	if (e.label.labelClass == APILblClass_Symbol && e.label.u.symbol.libInd <= 0 && !Has (spec, "libraryPart"))
		Fail ("Symbol label needs 'libraryPart' (a Label library part — use search_library_parts).");

	// Leader geometry.
	API_Coord begin;
	if (auto c = OptCoord (spec, "begin")) {
		begin = *c;
	} else if (parent.has_value ()) {
		auto ref = ParentReferencePoint (*parent);
		if (!ref.has_value ())
			Fail ("Cannot derive a leader start point from the parent element: give 'begin' {x, y}.");
		begin = *ref;
	} else {
		Fail ("Label requires 'begin' {x, y} (the leader start / arrow point), or 'parent' to label an element.");
	}
	e.label.begC = begin;
	const bool hasMiddle = Has (spec, "middle"), hasEnd = Has (spec, "end");
	if (hasMiddle && !hasEnd)
		Fail ("Label 'middle' needs 'end' too.");
	if (hasEnd) {
		e.label.endC = GetCoord (spec, "end");
		e.label.midC = hasMiddle ? GetCoord (spec, "middle") : Mid (begin, e.label.endC);
		e.label.createAtDefaultPosition = false;
	} else if (parent.has_value ()) {
		e.label.midC = { begin.x + 1.0, begin.y + 1.0 };
		e.label.endC = { begin.x + 2.5, begin.y + 1.0 };
		e.label.createAtDefaultPosition = true;
	} else {
		e.label.midC = { begin.x + 1.0, begin.y + 0.5 };
		e.label.endC = { begin.x + 3.0, begin.y + 0.5 };
		e.label.createAtDefaultPosition = false;
	}

	if (e.label.labelClass == APILblClass_Text) {
		// Label tool defaults can wrap the text into a zero-width box: one character per line, the text
		// effectively invisible (verified live). New text labels do not wrap unless wrapWidth is given, and
		// are horizontal unless textOrientation is given ('General' put them along the leader).
		e.label.nonBreaking = true;
		e.label.u.text.nonBreaking = true;
		e.label.u.text.width = 0.0;
		e.label.u.text.height = 0.0;
		if (!Has (spec, "textOrientation"))
			e.label.textWay = APIDir_Horizontal;
	}
	UInt64 memoMask = 0;
	ApplyLabelFields (e, nullptr, spec, *memo, memoMask, nullptr);
	CheckDrafting (ACAPI_Element_Create (&e, memo.Ptr ()), parent.has_value () ? "Cannot create associative label" : "Cannot create label");
	return e.header.guid;
}


void SerializeLabel (const API_Element& e, OS& out)
{
	const API_LabelType& l = e.label;
	out.Add ("labelClass", NameOf (kLabelClasses, l.labelClass));
	const bool associative = l.parent != APINULLGuid;
	out.Add ("associative", associative);
	if (associative) {
		out.Add ("parent", GuidStr (l.parent));
		out.Add ("parentType", ElemTypeName (l.parentType));
	}
	out.Add ("begin", CoordObj (l.begC));
	out.Add ("middle", CoordObj (l.midC));
	out.Add ("end", CoordObj (l.endC));
	OS leader;
	leader.Add ("show", l.hasLeaderLine);
	leader.Add ("shape", NameOf (kLeaderShapes, l.leaderShape));
	leader.Add ("pen", (Int32) l.pen);
	leader.Add ("lineType", AttrRef (API_LinetypeID, l.ltypeInd));
	leader.Add ("anchor", NameOf (kLabelAnchors, l.anchorPoint));
	AddAngle (leader, "squareRootAngle", l.squareRootAngle);
	leader.Add ("arrows", ArrowsJson (l.arrowData));
	out.Add ("leader", leader);
	out.Add ("frame", l.framed);
	out.Add ("frameOffset", l.contourOffset);
	out.Add ("textOrientation", NameOf (kTextWays, l.textWay));
	out.Add ("alwaysReadable", l.flipEnabled);
	out.Add ("hideWithBaseElement", l.hideWithBaseElem);

	if (l.labelClass == APILblClass_Text) {
		AddTextJson (out, e.header.guid, l.u.text, true);
	} else {
		const API_ObjectType& sym = l.u.symbol;
		OS lpJson = Try ([&] () -> OS { return LibPartToJson (GetLibPartByIndex (sym.libInd)); });
		out.Add ("libraryPart", lpJson);
		out.Add ("pen", (Int32) sym.pen);
		out.Add ("font", AttrRef (API_FontID, l.font));
		out.Add ("size", l.textSize);
		out.Add ("bold", (l.faceBits & APIFace_Bold) != 0);
		out.Add ("italic", (l.faceBits & APIFace_Italic) != 0);
		out.Add ("underline", (l.faceBits & APIFace_Underline) != 0);
		out.Add ("strikeout", (l.effectsBits & APIEffect_StrikeOut) != 0);
		out.Add ("superscript", (l.effectsBits & APIEffect_SuperScript) != 0);
		out.Add ("subscript", (l.effectsBits & APIEffect_SubScript) != 0);
		out.Add ("background", l.useBgFill);
		out.Add ("backgroundPen", (Int32) l.fillBgPen);
		out.Add ("wrapText", !l.nonBreaking);
		AddAngle (out, "angle", sym.angle);
	}
}


void ModifyLabel (API_Element& e, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask, const OS& patch)
{
	if (Has (patch, "parent"))
		Fail ("A label cannot be re-attached to another element: delete it and create a new label with create_labels.", APIERR_NOTSUPPORTED);
	if (Has (patch, "labelClass") && (API_LblClassID) ParseNamed (kLabelClasses, patch, "labelClass") != e.label.labelClass)
		Fail ("The class of an existing label (Text/Symbol) cannot be changed: delete it and create a new label.", APIERR_NOTSUPPORTED);
	const API_Guid guid = e.header.guid;
	ApplyLabelFields (e, &mask, patch, memo, memoMask, &guid);
}

} // namespace


void RegisterTextLabelAdapters ()
{
	RegisterAdapter ({ API_TextID,	CreateText,		SerializeText,	ModifyText });
	RegisterAdapter ({ API_LabelID,	CreateLabel,	SerializeLabel,	ModifyLabel });
}

} // namespace drafting
} // namespace cc
