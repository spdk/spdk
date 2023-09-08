/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2023 Intel Corporation.
 *   All rights reserved.
 */

package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"github.com/spdk/spdk/go/rpc/client"
	"log"
	"os"
)

const (
	socketAddress    = "/var/tmp/spdk.sock"
	bDevGetBDevs     = "bdev_get_bdevs"
	defaultBdevName = ""
	defaultTimeout  = 0
)

func main() {

	//create client
	rpcClient, err := client.CreateClientWithJsonCodec(client.Unix, socketAddress)
	if err != nil {
		log.Fatalf("error on client creation, err: %s", err.Error())
	}
	defer rpcClient.Close()

	//sends a JSON-RPC 2.0 request with "bdev_get_bdevs" method and provided params
	resp, err := rpcClient.Call(bDevGetBDevs, getBDevParams())
	if err != nil {
		log.Fatalf("error on JSON-RPC call, method: %s err: %s", bDevGetBDevs, err.Error())
	}

	result, err := json.Marshal(resp.Result)
	if err != nil {
		log.Print(fmt.Errorf("error when creating json string representation: %w", err).Error())
	}

	fmt.Printf("%s\n", string(result))
}

func getBDevParams() map[string]any {
	fs := flag.NewFlagSet("set", flag.ContinueOnError)
	fs.String("name", defaultBdevName, "Name of the Blockdev")
	fs.Int("timeout", defaultTimeout, "Time in ms to wait for the bdev to appear")

	err := fs.Parse(os.Args[1:])
	if err != nil {
		log.Fatalf("%s\n", err.Error())
	}

	paramsMap := make(map[string]any)
	fs.Visit(func(f *flag.Flag) {
		paramsMap[f.Name] = f.Value
	})

	return paramsMap
}
