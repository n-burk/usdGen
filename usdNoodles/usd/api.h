// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef NOODLES_USD_API_H
#define NOODLES_USD_API_H

// Keep the optional USD adapter ABI independent from the core noodles DLL.
// Consumers of a shared adapter import these entry points; the adapter itself
// defines NOODLES_USD_EXPORTS.  Static builds deliberately need no decoration.
#if defined(_WIN32) && !defined(NOODLES_USD_STATIC)
#  if defined(NOODLES_USD_EXPORTS)
#    define NOODLES_USD_API __declspec(dllexport)
#  else
#    define NOODLES_USD_API __declspec(dllimport)
#  endif
#else
#  define NOODLES_USD_API
#endif

#endif // NOODLES_USD_API_H
