module oceanbook_adapter_wrapper

go 1.23

// build.sh rewrites this line to point at the pinned oceanbook checkout.
replace github.com/draveness/oceanbook => ../../../third_party/oceanbook

require (
	github.com/draveness/oceanbook v0.0.0-00010101000000-000000000000
	github.com/shopspring/decimal v0.0.0-20180709203117-cd690d0c9e24
)

require (
	github.com/emirpasic/gods v1.12.0 // indirect
	github.com/golang/protobuf v1.4.2 // indirect
	github.com/konsorten/go-windows-terminal-sequences v1.0.3 // indirect
	github.com/logrusorgru/aurora v0.0.0-20181002194514-a7b3b318ed4e // indirect
	github.com/sirupsen/logrus v1.6.0 // indirect
	golang.org/x/net v0.0.0-20190311183353-d8887717615a // indirect
	golang.org/x/sys v0.0.0-20190422165155-953cdadca894 // indirect
	golang.org/x/text v0.3.0 // indirect
	google.golang.org/genproto v0.0.0-20190819201941-24fa4b261c55 // indirect
	google.golang.org/grpc v1.29.1 // indirect
	google.golang.org/protobuf v1.23.0 // indirect
)
