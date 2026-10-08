#pragma once
#include <cstdint>

namespace esper {

// Condition-function parameter kinds, to the resolution CTDA FormID remapping
// needs: the only load-bearing distinction is Form vs. not-Form. Int/Float/Str
// are lumped as "not a FormID". (A richer editor-facing kind set — actor value
// enums, axis, named enums — can layer on top later; this is the remap floor.)
enum class CtdaParam : uint8_t { None, Int, Float, Form, Str };

// One condition function's signature (the two CTDA parameter slots).
struct CtdaFuncInfo {
    const char* name = "";
    CtdaParam   p1   = CtdaParam::None;
    CtdaParam   p2   = CtdaParam::None;
};

// Look up a condition function by its CTDA function index; fills `out` and
// returns true when known. Returns false for an unknown index — the caller
// MUST treat unknown functions as un-remappable (their param FormID-ness is
// unknown, so a blind remap could corrupt an integer or miss a real reference).
bool LookupCtdaFunc(uint16_t index, CtdaFuncInfo& out);

} // namespace esper
