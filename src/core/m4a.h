#pragma once

// poryaaaa's engine and voicegroup-loader headers for C++ translation
// units. Include this, never m4a_engine.h or voicegroup_loader.h directly:
// the engine's structs have a field named `slots` (M4ASeqEstimate), which
// Qt's `slots` keyword macro would expand to nothing wherever a Qt header
// came first; the first inclusion goes through here with the macro parked.
#pragma push_macro("slots")
#undef slots
extern "C" {
#include "m4a_engine.h"
#include "voicegroup_loader.h"
}
#pragma pop_macro("slots")
