module geseq_adapter_wrapper

go 1.23

require (
	github.com/geseq/orderbook v0.0.0
	github.com/geseq/udecimal v0.0.0-20211126195629-659dde5a92d8
)

require golang.org/x/sys v0.26.0 // indirect

// build.sh rewrites this line to point at the pinned third_party checkout.
replace github.com/geseq/orderbook => ../../../third_party/geseq_orderbook
