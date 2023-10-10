/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2023 Intel Corporation.
 *   Copyright (c) 2023 Dell Inc, or its subsidiaries.
 *   All rights reserved.
 */

package client

import (
	"encoding/json"
	"fmt"
	"net"
	"reflect"
	"sync/atomic"
)

const (
	// jsonRPCVersion specifies the version of the JSON-RPC protocol.
	jsonRPCVersion = "2.0"
	// Unix specifies network type for socket connection.
	Unix = "unix"
	// TCP specifies network type for tcp connection.
	TCP = "tcp"
)

// Client interface mostly for mockery auto-generation
type IClient interface {
	Call(method string, params any) (*Response, error)
}

// Client represents JSON-RPC 2.0 client.
type Client struct {
	codec     *jsonCodec
	requestId atomic.Uint64
}

// build time check that struct implements interface
var _ IClient = (*Client)(nil)

// Call method sends a JSON-RPC 2.0 request to a specified address (provided during client creation).
func (c *Client) Call(method string, params any) (*Response, error) {
	id := c.requestId.Add(1)

	request, reqErr := createRequest(method, id, params)
	if reqErr != nil {
		return nil, fmt.Errorf("error during client call for %s method, err: %w",
			method, reqErr)
	}

	encErr := c.codec.encoder.Encode(request)
	if encErr != nil {
		return nil, fmt.Errorf("error during request encode for %s method, err: %w",
			method, encErr)
	}

	response := &Response{}
	decErr := c.codec.decoder.Decode(response)
	if decErr != nil {
		return nil, fmt.Errorf("error during response decode for %s method, err: %w",
			method, decErr)
	}

	if request.ID != uint64(response.ID) {
		return nil, fmt.Errorf("error mismatch request and response IDs for %s method",
			method)
	}

	if response.Error != nil {
		return nil, fmt.Errorf("error received for %s method, err: %w",
			method, response.Error)
	}

	return response, nil
}

// Close closes connection with underlying stream.
func (c *Client) Close() error {
	return c.codec.close()
}

type jsonCodec struct {
	encoder *json.Encoder
	decoder *json.Decoder
	conn    net.Conn
}

func (j *jsonCodec) close() error {
	return j.conn.Close()
}

func createJsonCodec(conn net.Conn) *jsonCodec {
	return &jsonCodec{
		encoder: json.NewEncoder(conn),
		decoder: json.NewDecoder(conn),
		conn:    conn,
	}
}

func createRequest(method string, requestId uint64, params any) (*Request, error) {
	paramErr := verifyRequestParamsType(params)
	if paramErr != nil {
		return nil, fmt.Errorf("error during request creation for %s method, err: %w",
			method, paramErr)
	}

	return &Request{
		Version: jsonRPCVersion,
		Method:  method,
		Params:  params,
		ID:      requestId,
	}, nil
}

func createConnectionToSocket(socketAddress string) (net.Conn, error) {
	address, err := net.ResolveUnixAddr(Unix, socketAddress)
	if err != nil {
		return nil, err
	}

	conn, err := net.DialUnix(Unix, nil, address)
	if err != nil {
		return nil, fmt.Errorf("could not connect to a Unix socket on address %s, err: %w",
			address.String(), err)
	}

	return conn, nil
}

func createConnectionToTcp(tcpAddress string) (net.Conn, error) {
	address, err := net.ResolveTCPAddr(TCP, tcpAddress)
	if err != nil {
		return nil, err
	}

	conn, err := net.DialTCP(TCP, nil, address)
	if err != nil {
		return nil, fmt.Errorf("could not connect to a TCP socket on address %s, err: %w",
			address.String(), err)
	}

	return conn, nil
}

func verifyRequestParamsType(params any) error {
	// Nil is allowed value for params field.
	if params == nil {
		return nil
	}

	paramType := reflect.TypeOf(params).Kind()
	if paramType == reflect.Pointer {
		paramType = reflect.TypeOf(params).Elem().Kind()
	}

	switch paramType {
	case reflect.Array, reflect.Map, reflect.Slice, reflect.Struct:
		return nil
	default:
		return fmt.Errorf("param type %s is not supported", paramType.String())
	}
}

// CreateClientWithJsonCodec creates a new JSON-RPC client.
// Both Unix and TCP sockets are supported
func CreateClientWithJsonCodec(network, address string) (*Client, error) {
	switch network {
	case "unix", "unixgram", "unixpacket":
		conn, err := createConnectionToSocket(address)
		if err != nil {
			return nil, fmt.Errorf("error during client creation for Unix socket, " +
				"err: %w", err)
		}

		return &Client{codec: createJsonCodec(conn), requestId: atomic.Uint64{}}, nil
	case "tcp", "tcp4", "tcp6":
		conn, err := createConnectionToTcp(address)
		if err != nil {
			return nil, fmt.Errorf("error during client creation for TCP socket, " +
				"err: %w", err)
		}

		return &Client{codec: createJsonCodec(conn), requestId: atomic.Uint64{}}, nil
	default:
		return nil, fmt.Errorf("unsupported network type")
	}
}

// Request represents JSON-RPC request.
// For more information visit https://www.jsonrpc.org/specification#request_object
type Request struct {
	Version string `json:"jsonrpc"`
	Method  string `json:"method"`
	Params  any    `json:"params,omitempty"`
	ID      uint64 `json:"id,omitempty"`
}

func (req *Request) ToString() (string, error) {
	jsonReq, err := json.Marshal(req)
	if err != nil {
		return "", fmt.Errorf("error when creating json string representation " +
			"of Request, err: %w", err)
	}

	return string(jsonReq), nil
}

// Response represents JSON-RPC response.
// For more information visit http://www.jsonrpc.org/specification#response_object
type Response struct {
	Version string `json:"jsonrpc"`
	Error   *Error `json:"error,omitempty"`
	Result  any    `json:"result,omitempty"`
	ID      int    `json:"id,omitempty"`
}

func (resp *Response) ToString() (string, error) {
	jsonResp, err := json.Marshal(resp)
	if err != nil {
		return "", fmt.Errorf("error when creating json string representation " +
			"of Response, err: %w", err)
	}

	return string(jsonResp), nil
}

// Error represents JSON-RPC error.
// For more information visit https://www.jsonrpc.org/specification#error_object
type Error struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
	Data    any    `json:"data,omitempty"`
}

// Error returns formatted string of JSON-RPC error.
func (err *Error) Error() string {
	return fmt.Sprintf("Code=%d Msg=%s", err.Code, err.Message)
}
