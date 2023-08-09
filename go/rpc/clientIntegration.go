/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2023 Intel Corporation.
 *   All rights reserved.
 */

package main

//#cgo CFLAGS: -I../../include
//#include "spdk/stdinc.h"
import "C"
import (
	"encoding/json"
	"github.com/spdk/spdk/go/rpc/client"
	"log"
	"reflect"
	"strings"
	"unsafe"
)

const (
	InvalidParameterError = iota + 1
	ConnectionError
	JsonRpcCallError
	InvalidResponseError
)

func main() {
}

//export spdk_gorpc_call
func spdk_gorpc_call(jsonPtr *C.char, location *C.char) (*C.char, C.int) {
	var jsonMap map[string]any
	jsonStr := C.GoString(jsonPtr)
	jsonDecoder := json.NewDecoder(strings.NewReader(jsonStr))
	jsonDecoder.UseNumber()
	err := jsonDecoder.Decode(&jsonMap)
	if err != nil {
		log.Printf("error when decoding function arguments, err: %s", err.Error())
		return nil, InvalidParameterError
	}

	socketLocation := C.GoString(location)

	rpcClient, err := client.CreateClientWithJsonCodec(client.Unix, socketLocation)
	if err != nil {
		log.Printf("error on client creation, err: %s", err.Error())
		return nil, ConnectionError
	}
	defer rpcClient.Close()

	method := jsonMap["method"].(string)
	params := jsonMap["params"].(map[string]any)
	// Force Go client to skip 'params' parameter in JSON-RPC call.
	if len(params) == 0 {
		params = nil
	}

	resp, err := rpcClient.Call(method, params)
	if err != nil {
		log.Printf("error on JSON-RPC call, method: %s, params: %s, err: %s", method, params, err.Error())
		return nil, JsonRpcCallError
	}

	var respToEncode any
	// This is a special case where inside JSON-RPC response 'Result' field is null ("result": null).
	// Due to json tag 'omitempty' in *client.Response.Result this field is ignored during
	// serialization when its value is null. General idea is to create a map containing values
	// from response and serialize that map instead of a *client.Response we got from
	// a Client.Call() method.
	if resp.Result == nil && resp.Error == nil {
		jsonMap := make(map[string]any)
		// Get all *client.Response fields.
		types := reflect.TypeOf(resp).Elem()
		// Get all *client.Response values.
		values := reflect.ValueOf(resp).Elem()
		// Iterate over fields.
		for i := 0; i < types.NumField(); i++ {
			// Name of a field.
			fieldName := types.Field(i).Name
			// Name of *client.Response.Error field.
			errorFieldName := reflect.TypeOf(resp.Error).Elem().Name()
			// Add all fields with values to a map except *client.Response.Error
			if fieldName != errorFieldName {
				jsonMap[strings.ToLower(fieldName)] = values.Field(i).Interface()
			}
		}
		respToEncode = jsonMap
	} else {
		respToEncode = resp
	}

	encodedResp, err := json.Marshal(respToEncode)
	if err != nil {
		log.Printf("error on creating json representation of response, err: %s", err.Error())
		return nil, InvalidResponseError
	}

	return C.CString(string(encodedResp)), 0
}

//export spdk_gorpc_free_response
func spdk_gorpc_free_response(p *C.char) {
	C.free(unsafe.Pointer(p))
}
