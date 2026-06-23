module danielgatis_adapter_wrapper

go 1.20

// build.sh rewrites this line to point at the pinned third_party checkout.
replace github.com/danielgatis/go-orderbook => ../../../third_party/danielgatis_src

require (
	github.com/danielgatis/go-orderbook v0.0.0-00010101000000-000000000000
	github.com/shopspring/decimal v1.3.1
)

require (
	github.com/emirpasic/gods v1.18.1 // indirect
	github.com/google/uuid v1.3.0 // indirect
)
