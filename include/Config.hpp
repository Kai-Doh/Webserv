// Compatibility shim -- the real header moved to srcs/config/Config.hpp as
// part of organizing srcs/ into per-module subfolders. Kept here so any
// code (this project's or a teammate's branch) that still does a bare
// #include "Config.hpp" against -Iinclude keeps compiling unchanged.
#include "config/Config.hpp"
