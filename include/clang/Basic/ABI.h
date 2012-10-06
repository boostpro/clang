//===----- ABI.h - ABI related declarations ---------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Enums/classes describing ABI related information about constructors,
/// destructors and thunks.
///
//===----------------------------------------------------------------------===//

#ifndef CLANG_BASIC_ABI_H
#define CLANG_BASIC_ABI_H

#include "llvm/ABI/ABI.h"

namespace clang {
  using llvm_abi::CXXCtorType;
  using llvm_abi::CXXDtorType;
  using llvm_abi::ReturnAdjustment;
  using llvm_abi::ThisAdjustment;
  using llvm_abi::ThunkInfo
} // end namespace clang

#endif // CLANG_BASIC_ABI_H
