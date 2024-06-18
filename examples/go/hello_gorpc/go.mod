module github.com/spdk/spdk/examples/go-rpc

go 1.21

require (
	"github.com/spdk/spdk/go/rpc" v0.0.0
)
replace "github.com/spdk/spdk/go/rpc" v0.0.0 => "./../../../go/rpc"
