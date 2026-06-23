module fmstephe_adapter_wrapper

go 1.21

// build.sh rewrites these two replace lines to point at the pinned third_party
// checkouts (the engine clone, and the local flib copy with arm64 ports).
replace github.com/fmstephe/matching_engine => ../../../third_party/fmstephe_matching_engine

// flib (2017) ships LazyStore / CacheLineBytes / the ftime asm for amd64 only;
// the local copy adds identical arm64 ports so the module compiles on aarch64.
// See README "Source patch" and third_party/fmstephe_flib_local.
replace github.com/fmstephe/flib => ../../../third_party/fmstephe_flib_local

require github.com/fmstephe/matching_engine v0.0.0-00010101000000-000000000000

require github.com/fmstephe/flib v0.0.0-20170802081819-76e5765dde32 // indirect
