#pragma once

namespace oa3 {

// The OpenADR 3 specification version this library implements; also the version segment
// of a VTN's base URL and of its MQTT topics ("OpenADR/3.1.0/...").
const char* spec_version();

const char* library_version();

}  // namespace oa3
