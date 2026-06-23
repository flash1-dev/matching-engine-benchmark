module i25959341_adapter_wrapper

go 1.22.1

// build.sh rewrites this line to point at the pinned third_party checkout.
replace orderbook => ../../../third_party/i25959341_src

require (
	github.com/shopspring/decimal v1.4.0
	orderbook v0.0.0
)

require github.com/emirpasic/gods v1.18.1 // indirect
