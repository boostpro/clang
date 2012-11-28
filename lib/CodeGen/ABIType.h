//===----- ABIType.h - The Type in ---*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef CLANG_CODEGEN_ABITYPE_H
#define CLANG_CODEGEN_ABITYPE_H

namespace clang {
  class QualType;

  class ABIType {
  public:

  enum DestructionKind {
    DK_none,
    DK_cxx_destructor,
    DK_objc_strong_lifetime,
    DK_objc_weak_lifetime
  };
    
    ABIType(QualType const&);
  };
}
#endif // CLANG_CODEGEN_ABITYPE_H
