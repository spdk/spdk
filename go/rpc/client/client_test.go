/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023 Dell Inc, or its subsidiaries.
 *   All rights reserved.
 */

package client

import (
	"reflect"
	"testing"
)

func Test_createRequest(t *testing.T) {
	tests := map[string]struct {
		params  any
		wantErr bool
	}{
		"nil params": {
			params:  nil,
			wantErr: false,
		},
		"reflect.Pointer": {
			params:  &struct{}{},
			wantErr: false,
		},
		"reflect.Array": {
			params:  [3]string{"a", "b", "c"},
			wantErr: false,
		},
		"reflect.Map": {
			params:  map[string]int{"a": 1, "b": 2, "c": 3},
			wantErr: false,
		},
		"reflect.Slice": {
			params:  []string{"a", "b", "c"},
			wantErr: false,
		},
		"reflect.Struct": {
			params:  struct{}{},
			wantErr: false,
		},
		"invalid param": {
			params:  "invalidParam",
			wantErr: true,
		},
	}

	for testName, tt := range tests {
		t.Run(testName, func(t *testing.T) {
			const method = "some method"
			const requestId = 17
			gotRequest, err := createRequest(method, requestId, tt.params)
			wantRequest := &Request{
				Version: jsonRPCVersion,
				Method:  method,
				Params:  tt.params,
				ID:      requestId,
			}
			if tt.wantErr {
				wantRequest = nil
			}
			if (err != nil) != tt.wantErr {
				t.Errorf("Expect error: %v, received: %v", nil, err)
			}
			if !reflect.DeepEqual(gotRequest, wantRequest) {
				t.Errorf("Expect %v request, received %v", wantRequest, gotRequest)
			}
		})
	}
}
