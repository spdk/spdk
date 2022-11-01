/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

// spdk might one day provide Go bindings for the SPDK RPC API.
// At the moment all that it does is allowing a Go project
// to vendor in the SPDK source code via dep with this
// in Gopkg.toml:
//
// required = ["github.com/spdk/spdk/go"]
//
// [prune]
//  go-tests = true
//  unused-packages = true
//
//  [[prune.project]]
//    name = "github.com/spdk/spdk"
//    go-tests = false
//    unused-packages = false

package spdk
