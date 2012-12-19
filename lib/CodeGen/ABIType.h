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
  class Qualifiers;

  class PointerType;
  class RecordType;

  template <class T>
  struct map_clang_to_abi_type;
  
  class ABIType {
  public:

    struct Pointer;
    struct CXXRecordDecl;
    struct RecordDecl;
    struct CXXDestructorDecl;
    struct Complex
    {};
    
    struct Type
    {
      bool isIncompleteType() const;
      bool isPointerType() const;
      bool isVariableArrayType() const;
      bool isConstantSizeType() const;
      
      // Not sure how much this is used.  I put it in to make an assert compile.
      bool isArrayType() const;
      
      template <class T>
      typename map_clang_to_abi_type<T>::type* getAs() const;

      template <class T>
      typename map_clang_to_abi_type<T>::type* castAs() const;

      CXXRecordDecl* getAsCXXRecordDecl() const;

      bool isBlockPointerType() const;
      bool isObjCRetainableType() const;
      bool isObjCObjectPointerType() const;
    };

    struct Pointer
    {
      ABIType getPointeeType() const;
    };

    struct FieldDecl
    {
      bool isBitField() const;
      ABIType getType() const;
      const RecordDecl *getParent() const;
    };
    
    struct RecordDecl
    {
      typedef FieldDecl** field_iterator;

      field_iterator field_begin() const;
      field_iterator field_end() const;
      
      bool isUnion() const;
    };
    
    struct Record
    {
      RecordDecl* getDecl() const;
    };

    struct Builtin
    {
      enum Kind_val {
#define BUILTIN_TYPE(Id, SingletonId) Id,
#define LAST_BUILTIN_TYPE(Id) LastKind = Id
#include "clang/AST/BuiltinTypes.def"
      };
      
      struct Kind {
        Kind(unsigned);
        Kind(Kind_val);
        operator Kind_val() const;
      };
    };

    struct CXXRecordDecl : RecordDecl
    {
      CXXDestructorDecl* getDestructor() const;
      bool hasTrivialDestructor() const;
    };

    struct CXXDestructorDecl
    {
      // Used in an assert.  Not sure this should be carried into ABI
      bool isUsed() const;
    };
    
    enum DestructionKind_val {
      DK_none,
      DK_cxx_destructor,
      DK_objc_strong_lifetime,
      DK_objc_weak_lifetime
    };

    struct DestructionKind {
      DestructionKind(unsigned);
      DestructionKind(DestructionKind_val);
      operator DestructionKind_val() const;
    };
    
    Type const* operator->() const;
    Type const& operator*() const;
    
    struct Qualifiers
    {
      enum TQ { // NOTE: These flags must be kept in sync with DeclSpec::TQ.
        Const    = 0x1,
        Restrict = 0x2,
        Volatile = 0x4,
        CVRMask = Const | Volatile | Restrict
      };

      enum GC_val {
        GCNone = 0,
        Weak,
        Strong
      };
      
      struct GC {
        GC(unsigned);
        GC(GC_val);
        operator GC_val() const;
      };

      enum ObjCLifetime_val {
        /// There is no lifetime qualification on this type.
        OCL_None,

        /// This object can be modified without requiring retains or
        /// releases.
        OCL_ExplicitNone,

        /// Assigning into this object requires the old value to be
        /// released and the new value to be retained.  The timing of the
        /// release of the old value is inexact: it may be moved to
        /// immediately after the last known point where the value is
        /// live.
        OCL_Strong,

        /// Reading or writing from this object requires a barrier call.
        OCL_Weak,

        /// Assigning into this object requires a lifetime extension.
        OCL_Autoreleasing
      };

      struct ObjCLifetime {
        ObjCLifetime(unsigned);
        ObjCLifetime(ObjCLifetime_val);
        operator ObjCLifetime_val() const;
        // Need to be able to convert back into clang's enum
        template <class T> operator T() const;
      };

      ObjCLifetime getObjCLifetime() const;
      
      bool hasVolatile() const;
      bool hasRestrict() const;
      unsigned getCVRQualifiers() const;
      GC getObjCGCAttr() const;
      unsigned getAddressSpace() const;
      void setObjCGCAttr(GC type);
      void addCVRQualifiers(unsigned mask);
      
      Qualifiers();
      Qualifiers(clang::Qualifiers);

      bool hasStrongOrWeakObjCLifetime() const;
    };
    Qualifiers getQualifiers() const;
    bool isObjCGCWeak() const;
    bool isDestructedType() const;
    struct Context {};
    bool isPODType(Context) const;
    
    ABIType getNonReferenceType() const;
  };

  template <>
  struct map_clang_to_abi_type<PointerType>
  {
    typedef ABIType::Pointer type;
  };
  
  template <>
  struct map_clang_to_abi_type<RecordType>
  {
    typedef ABIType::Record type;
  };

  template <>
  struct map_clang_to_abi_type<ComplexType>
  {
    typedef ABIType::Complex type;
  };
}

namespace llvm {
  template<class> struct DenseMapInfo;

  template<> struct DenseMapInfo<clang::ABIType> {
    static inline clang::ABIType getEmptyKey();
    static inline clang::ABIType getTombstoneKey();
    static unsigned getHashValue(clang::ABIType Val);
    static bool isEqual(clang::ABIType LHS, clang::ABIType RHS);
  };
}

#endif // CLANG_CODEGEN_ABITYPE_H
