module matchingo_adapter_wrapper

go 1.23

// build.sh rewrites this line to point at the pinned third_party checkout.
replace github.com/gonevo/matchingo => ../../../third_party/matchingo_src

require (
	github.com/gonevo/matchingo v0.0.0-00010101000000-000000000000
	github.com/nikolaydubina/fpdecimal v0.16.0
)

require (
	github.com/gammazero/deque v0.2.1 // indirect
	github.com/hashicorp/go-set v0.1.13 // indirect
)
