// Compatibility shim -- the canonical Connection struct / contract header
// is include/connection.hpp. This file exists at the path the Core Server
// side includes ("shared/connection.hpp") so that side needs no changes.
//
// Deliberately an explicit relative path rather than a bare
// #include "connection.hpp": a bare quote-include would resolve to this
// very file first (same filename, same directory checked first), causing
// infinite self-inclusion.
#include "../../include/connection.hpp"
