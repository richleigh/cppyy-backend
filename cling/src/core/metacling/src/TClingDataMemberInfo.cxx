// @(#)root/core/meta:$Id$
// Author: Paul Russo   30/07/2012

/*************************************************************************
 * Copyright (C) 1995-2000, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

/** \class TClingDataMemberInfo

Emulation of the CINT DataMemberInfo class.

The CINT C++ interpreter provides an interface to metadata about
the data members of a class through the DataMemberInfo class.  This
class provides the same functionality, using an interface as close
as possible to DataMemberInfo but the data member metadata comes
from the Clang C++ compiler, not CINT.
*/

#include "TClingDataMemberInfo.h"

#include "TDictionary.h"
#include "TClingTypeInfo.h"
#include "TClingUtils.h"
#include "TClassEdit.h"
#include "TError.h"

#include "clang/AST/Attr.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/GlobalDecl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/Type.h"

#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/APFloat.h"


using namespace clang;

namespace CppyyLegacy {

TClingDataMemberInfo::TClingDataMemberInfo(cling::Interpreter *interp,
                                           TClingClassInfo *ci)
: TClingDeclInfo(nullptr), fInterp(interp), fClassInfo(0), fFirstTime(true), fTitle(""), fContextIdx(0U), fIoType(""), fIoName("")
{
   if (!ci) {
      // We are meant to iterate over the global namespace (well at least CINT did).
      fClassInfo = new TClingClassInfo(interp);
   } else {
      fClassInfo = new TClingClassInfo(*ci);
   }
   if (fClassInfo->IsValid()) {
      Decl *D = const_cast<Decl*>(fClassInfo->GetDecl());

      clang::DeclContext *dc = llvm::cast<clang::DeclContext>(D);
      dc->collectAllContexts(fContexts);

      // Could trigger deserialization of decls.
      cling::Interpreter::PushTransactionRAII RAII(interp);
      fIter = llvm::cast<clang::DeclContext>(D)->decls_begin();
      const TagDecl *TD = CppyyLegacy::TMetaUtils::GetAnnotatedRedeclarable(llvm::dyn_cast<TagDecl>(D));
      if (TD)
         fIter = TD->decls_begin();

      // Move to first data member.
      InternalNext();
      fFirstTime = true;
   }

}

TClingDataMemberInfo::TClingDataMemberInfo(cling::Interpreter *interp,
                                           const clang::ValueDecl *ValD,
                                           TClingClassInfo *ci)
: TClingDeclInfo(ValD), fInterp(interp), fClassInfo(ci ? new TClingClassInfo(*ci) : new TClingClassInfo(interp, ValD)), fFirstTime(true),
  fTitle(""), fContextIdx(0U), fIoType(""), fIoName(""){

   using namespace llvm;
   const auto DC = ValD->getDeclContext();
   (void)DC;
   assert((ci || isa<TranslationUnitDecl>(DC) ||
          ((DC->isTransparentContext() || DC->isInlineNamespace()) && isa<TranslationUnitDecl>(DC->getParent()) ) ||
           isa<EnumConstantDecl>(ValD)) && "Not TU?");
   assert((isa<VarDecl>(ValD) ||
           isa<FieldDecl>(ValD) ||
           isa<EnumConstantDecl>(ValD) ||
           isa<IndirectFieldDecl>(ValD)) &&
          "The decl should be either VarDecl or FieldDecl or EnumConstDecl");

}

void TClingDataMemberInfo::CheckForIoTypeAndName() const
{
   // Three cases:
   // 1) 00: none to be checked
   // 2) 01: type to be checked
   // 3) 10: none to be checked
   // 4) 11: both to be checked
   unsigned int code = fIoType.empty() + (int(fIoName.empty()) << 1);

   if (code == 0) return;

   const Decl* decl = GetDecl();

   if (code == 3 || code == 2) CppyyLegacy::TMetaUtils::ExtractAttrPropertyFromName(*decl,"ioname",fIoName);
   if (code == 3 || code == 1) CppyyLegacy::TMetaUtils::ExtractAttrPropertyFromName(*decl,"iotype",fIoType);

}

const clang::Decl* TClingDataMemberInfo::GetDecl() const {
   if (const clang::Decl* SingleDecl = TClingDeclInfo::GetDecl())
      return SingleDecl;
   if (clang::UsingShadowDecl* shadow_decl = llvm::dyn_cast<clang::UsingShadowDecl>(*fIter))
      return shadow_decl->getTargetDecl();
   return *fIter;
}

TDictionary::DeclId_t TClingDataMemberInfo::GetDeclId() const
{
   if (!IsValid()) {
      return TDictionary::DeclId_t();
   }
   return (const clang::Decl*)(GetDecl()->getCanonicalDecl());
}

TDictionary::DeclId_t TClingDataMemberInfo::GetTagDeclId() const
{
   if (!IsValid()) {
      return TDictionary::DeclId_t();
   }

   const clang::Decl* decl = (const clang::Decl*)(GetDecl()->getCanonicalDecl());
   const clang::ValueDecl* vd = llvm::dyn_cast<clang::ValueDecl>(decl);
   if (vd) {
      clang::QualType qt = vd->getType();
      const clang::TagType* tt = qt->getAs<clang::TagType>();
      if (tt)
         return tt->getDecl();
   }

   return TDictionary::DeclId_t();
}

const clang::ValueDecl *TClingDataMemberInfo::GetTargetValueDecl() const
{
   const Decl *D = GetDecl();
   do {
      if (auto VD = dyn_cast<ValueDecl>(D))
         return VD;
   } while ((D = dyn_cast<UsingShadowDecl>(D)->getTargetDecl()));
   return nullptr;
}

int TClingDataMemberInfo::ArrayDim() const
{
   if (!IsValid()) {
      return -1;
   }
   // Sanity check the current data member.
   clang::Decl::Kind DK = GetDecl()->getKind();
   if (
       (DK != clang::Decl::Field) &&
       (DK != clang::Decl::Var) &&
       (DK != clang::Decl::EnumConstant)
       ) {
      // Error, was not a data member, variable, or enumerator.
      return -1;
   }
   if (DK == clang::Decl::EnumConstant) {
      // We know that an enumerator value does not have array type.
      return 0;
   }
   // To get this information we must count the number
   // of array type nodes in the canonical type chain.
   const clang::ValueDecl *VD = llvm::dyn_cast<clang::ValueDecl>(GetDecl());
   clang::QualType QT = VD->getType().getCanonicalType();
   int cnt = 0;
   while (1) {
      if (QT->isArrayType()) {
         ++cnt;
         QT = llvm::cast<clang::ArrayType>(QT)->getElementType();
         continue;
      }
      else if (QT->isReferenceType()) {
         QT = llvm::cast<clang::ReferenceType>(QT)->getPointeeType();
         continue;
      }
      else if (QT->isPointerType()) {
         QT = llvm::cast<clang::PointerType>(QT)->getPointeeType();
         continue;
      }
      else if (QT->isMemberPointerType()) {
         QT = llvm::cast<clang::MemberPointerType>(QT)->getPointeeType();
         continue;
      }
      break;
   }
   return cnt;
}

int TClingDataMemberInfo::MaxIndex(int dim) const
{
   if (!IsValid()) {
      return -1;
   }
   // Sanity check the current data member.
   clang::Decl::Kind DK = GetDecl()->getKind();
   if (
       (DK != clang::Decl::Field) &&
       (DK != clang::Decl::Var) &&
       (DK != clang::Decl::EnumConstant)
       ) {
      // Error, was not a data member, variable, or enumerator.
      return -1;
   }
   if (DK == clang::Decl::EnumConstant) {
      // We know that an enumerator value does not have array type.
      return 0;
   }
   // To get this information we must count the number
   // of array type nodes in the canonical type chain.
   const clang::ValueDecl *VD = llvm::dyn_cast<clang::ValueDecl>(GetDecl());
   clang::QualType QT = VD->getType().getCanonicalType();
   int paran = ArrayDim();
   if ((dim < 0) || (dim >= paran)) {
      // Passed dimension is out of bounds.
      return -1;
   }
   int cnt = dim;
   int max = 0;
   while (1) {
      if (QT->isArrayType()) {
         if (cnt == 0) {
            if (const clang::ConstantArrayType *CAT =
                llvm::dyn_cast<clang::ConstantArrayType>(QT)
                ) {
               max = static_cast<int>(CAT->getSize().getZExtValue());
            }
            else if (llvm::dyn_cast<clang::IncompleteArrayType>(QT)) {
               max = INT_MAX;
            }
            else {
               max = -1;
            }
            break;
         }
         --cnt;
         QT = llvm::cast<clang::ArrayType>(QT)->getElementType();
         continue;
      }
      else if (QT->isReferenceType()) {
         QT = llvm::cast<clang::ReferenceType>(QT)->getPointeeType();
         continue;
      }
      else if (QT->isPointerType()) {
         QT = llvm::cast<clang::PointerType>(QT)->getPointeeType();
         continue;
      }
      else if (QT->isMemberPointerType()) {
         QT = llvm::cast<clang::MemberPointerType>(QT)->getPointeeType();
         continue;
      }
      break;
   }
   return max;
}

int TClingDataMemberInfo::InternalNext()
{
   assert(!fDecl
          && "This is a single decl, not an iterator!");

   fNameCache.clear(); // invalidate the cache.
   bool increment = true;
   // Move to next acceptable data member.
   while (fFirstTime || *fIter) {
      // Move to next decl in context.
      if (fFirstTime) {
         fFirstTime = false;
      }
      else if (increment) {
         ++fIter;
      } else {
         increment = true;
      }

      // Handle reaching end of current decl context.
      if (!*fIter) {
         if (fIterStack.size()) {
            // End of current decl context, and we have more to go.
            fIter = fIterStack.back();
            fIterStack.pop_back();
            continue;
         }
         while (!*fIter) {
            // Check the next decl context (of namespace)
            ++fContextIdx;
            if (fContextIdx >= fContexts.size()) {
               // Iterator is now invalid.
               return 0;
            }
            clang::DeclContext *dc = fContexts[fContextIdx];
            // Could trigger deserialization of decls.
            cling::Interpreter::PushTransactionRAII RAII(fInterp);
            fIter = dc->decls_begin();
            if (*fIter) {
               // Good, a non-empty context.
               break;
            }
         }
      }

      // Valid decl, recurse into it, accept it, or reject it.
      clang::Decl::Kind DK = fIter->getKind();
      if (DK == clang::Decl::Enum) {
         EnumDecl* ed = dyn_cast<EnumDecl>(*fIter);
         if (ed && !ed->isScoped()) {
            // We have an enum, expand transparent/non-scoped ones.
            fIterStack.push_back(fIter);
            cling::Interpreter::PushTransactionRAII RAII(fInterp);
            fIter = llvm::dyn_cast<clang::DeclContext>(*fIter)->decls_begin();
            increment = false; // avoid the next incrementation
         }
         continue;
      }
      if (DK == clang::Decl::Field) {
         FieldDecl* ff = dyn_cast<FieldDecl>(*fIter);
         if (ff->isAnonymousStructOrUnion()) {
            cling::Interpreter::PushTransactionRAII RAII(fInterp);
            const auto* rec = ff->getType()->getAs<RecordType>()->getDecl();
            if (rec) {
               fIterStack.push_back(fIter);
               fIter = rec->decls_begin();
               increment = false; // avoid the next incrementation
               continue;
            }
         }
      }
      if ((DK == clang::Decl::Field) || (DK == clang::Decl::EnumConstant) ||
          (DK == clang::Decl::Var)) {
         // Stop on class data members, enumerator values,
         // and namespace variable members.
         return 1;
      }
      if (clang::UsingShadowDecl* shadow_decl = llvm::dyn_cast<clang::UsingShadowDecl>(*fIter)) {
          clang::Decl::Kind target_DK = shadow_decl->getTargetDecl()->getKind();
          if ((target_DK == clang::Decl::Field) || (target_DK == clang::Decl::EnumConstant) ||
              (target_DK == clang::Decl::Var)) {
             return 1;       // as above
          }
      }
      // Collect internal `__cling_N5xxx' inline namespaces; they will be traversed later
      if (auto NS = dyn_cast<NamespaceDecl>(*fIter)) {
         if (NS->getDeclContext()->isTranslationUnit() && NS->isInlineNamespace())
            fContexts.push_back(NS);
      }
   }
   return 0;
}

intptr_t TClingDataMemberInfo::Offset()
{
   using namespace clang;

   if (!IsValid()) {
      return (intptr_t)-1;
   }

   const Decl *D = GetDecl();
   ASTContext& C = D->getASTContext();
   if (const FieldDecl *FldD = dyn_cast<FieldDecl>(D)) {
      // The current member is a non-static data member; add a transaction as
      // the lookup could trigger deserialization
      cling::Interpreter::PushTransactionRAII RAII(fInterp);

      const clang::RecordDecl *RD = FldD->getParent();
      const clang::ASTRecordLayout &Layout = C.getASTRecordLayout(RD);
      uint64_t bits = Layout.getFieldOffset(FldD->getFieldIndex());
      intptr_t offset = C.toCharUnitsFromBits(bits).getQuantity();
      while (RD->isAnonymousStructOrUnion()) {
         // calculate the offset of the union in its parent (TODO: this seems silly)
         const clang::RecordDecl* anon = RD;
         RD = llvm::dyn_cast<RecordDecl>(anon->getParent());
         for (auto f = RD->field_begin(); f != RD->field_end(); ++f) {
            auto* rt = f->getType()->getAs<RecordType>();
            if (!rt) continue;
            if (anon == rt->getDecl()) {
               FldD = *f;
               break;
            }
         }
         const auto& ll = C.getASTRecordLayout(RD);
         uint64_t boff = ll.getFieldOffset(FldD->getFieldIndex());
         offset += C.toCharUnitsFromBits(boff).getQuantity();
      }
      return offset;
   }
   else if (const VarDecl *VD = dyn_cast<VarDecl>(D)) {
      // Could trigger deserialization of decls, in particular in case
      // of constexpr, like:
      //   static constexpr Long64_t something = std::numeric_limits<Long64_t>::max();
      cling::Interpreter::PushTransactionRAII RAII(fInterp);

      // We can't reassign constexpr or const variables. We can compute the
      // initializer.
      if (VD->hasInit() && (VD->isConstexpr() || VD->getType().isConstQualified())) {
         if (const APValue* val = VD->evaluateValue()) {
            if (VD->getType()->isIntegralType(C)) {
               return (intptr_t)val->getInt().getRawData();
            } else {
               // The VD stores the init value; its lifetime should the lifetime of
               // this offset.
               switch (val->getKind()) {
               case APValue::Int: {
                  if (val->getInt().isSigned())
                     fConstInitVal.fLong = (long)val->getInt().getSExtValue();
                  else
                     fConstInitVal.fLong = (long)val->getInt().getZExtValue();
                  return (intptr_t) &fConstInitVal.fLong;
               }
               case APValue::Float:
                  if (&val->getFloat().getSemantics()
                      == (const llvm::fltSemantics*)&llvm::APFloat::IEEEsingle()) {
                     fConstInitVal.fFloat = val->getFloat().convertToFloat();
                     return (intptr_t)&fConstInitVal.fFloat;
                  } else if (&val->getFloat().getSemantics()
                             == (const llvm::fltSemantics*) &llvm::APFloat::IEEEdouble()) {
                     fConstInitVal.fDouble = val->getFloat().convertToDouble();
                     return (intptr_t)&fConstInitVal.fDouble;
                  }
                  // else fall-through
               default:
                  ;// fall-through
               };
               // fall-through
            } // not integral type
         } // have an APValue
      } // have an initializing value

      // Try the slow operation.
      if (intptr_t addr = reinterpret_cast<intptr_t>(fInterp->getAddressOfGlobal(GlobalDecl(VD))))
         return addr;
   }
   // FIXME: We have to explicitly check for not enum constant because the
   // implementation of getAddressOfGlobal relies on mangling the name and in
   // clang there is misbehaviour in MangleContext::shouldMangleDeclName.
   // enum constants are essentially numbers and don't get addresses. However
   // ROOT expects the address to the enum constant initializer to be returned.
   else if (const EnumConstantDecl *ECD = dyn_cast<EnumConstantDecl>(D))
      // The raw data is stored as a long long, so we need to find the 'long'
      // part.
#ifdef R__BYTESWAP
      // In this case at the beginning.
      return (intptr_t)ECD->getInitVal().getRawData();
#else
      // In this case in the second part.
      return reinterpret_cast<intptr_t>(((char*)ECD->getInitVal().getRawData())+sizeof(intptr_t) );
#endif
   return (intptr_t)-1;
}

long TClingDataMemberInfo::Property() const
{
   if (!IsValid()) {
      return 0L;
   }
   long property = 0L;
   const clang::Decl *decl = GetDecl();
   if (decl->getDeclContext()->isTransparentContext()) {
      decl = llvm::dyn_cast<clang::Decl>(decl->getDeclContext());
      if (!decl) decl = GetDecl();
   }

   const clang::Decl *declaccess = decl;
   if (!TClingDeclInfo::GetDecl() && llvm::dyn_cast<clang::UsingShadowDecl>(*fIter))
      declaccess = *fIter;

   switch (declaccess->getAccess()) {
      case clang::AS_public:
         property |= kIsPublic;
         break;
      case clang::AS_protected:
         property |= kIsProtected;
         break;
      case clang::AS_private:
         property |= kIsPrivate;
         break;
      case clang::AS_none:
         if (declaccess->getDeclContext()->isNamespace()) {
            property |= kIsPublic;
         } else {
            // IMPOSSIBLE
         }
         break;
      default:
         // IMPOSSIBLE
         break;
   }
   if (const clang::VarDecl *vard = llvm::dyn_cast<clang::VarDecl>(GetDecl())) {
      if (vard->isConstexpr())
         property |= kIsConstexpr;
      if (vard->getStorageClass() == clang::SC_Static) {
         property |= kIsStatic;
      } else if (decl->getDeclContext()->isNamespace()) {
         // Data members of a namespace are global variable which were
         // considered to be 'static' in the CINT (and thus ROOT) scheme.
         property |= kIsStatic;
      }
   }
   if (llvm::isa<clang::EnumConstantDecl>(GetDecl())) {
      // Enumeration constant are considered to be 'static' data member in
      // the CINT (and thus ROOT) scheme.
      property |= kIsStatic;
   }
   const clang::ValueDecl *vd = llvm::dyn_cast<clang::ValueDecl>(GetDecl());
   clang::QualType qt = vd->getType();
   if (llvm::isa<clang::TypedefType>(qt)) {
      property |= kIsTypedef;
   }
   qt = qt.getCanonicalType();
   if (qt.isConstQualified()) {
      property |= kIsConstant;
   }
   while (1) {
      if (qt->isArrayType()) {
         property |= kIsArray;
         qt = llvm::cast<clang::ArrayType>(qt)->getElementType();
         continue;
      }
      else if (qt->isReferenceType()) {
         property |= kIsReference;
         qt = llvm::cast<clang::ReferenceType>(qt)->getPointeeType();
         continue;
      }
      else if (qt->isPointerType()) {
         property |= kIsPointer;
         if (qt.isConstQualified()) {
            property |= kIsConstPointer;
         }
         qt = llvm::cast<clang::PointerType>(qt)->getPointeeType();
         continue;
      }
      else if (qt->isMemberPointerType()) {
         qt = llvm::cast<clang::MemberPointerType>(qt)->getPointeeType();
         continue;
      }
      break;
   }
   if (qt->isBuiltinType()) {
      property |= kIsFundamental;
   }
   if (qt.isConstQualified()) {
      property |= kIsConstant;
   }
   const clang::TagType *tt = qt->getAs<clang::TagType>();
   if (tt) {
      const clang::TagDecl *td = tt->getDecl();
      if (td->isClass()) {
         property |= kIsClass;
      }
      else if (td->isStruct()) {
         property |= kIsStruct;
      }
      else if (td->isUnion()) {
         property |= kIsUnion;
      }
      else if (td->isEnum()) {
         property |= kIsEnum;
      }
   }
   // We can't be a namespace, can we?
   //   if (dc->isNamespace() && !dc->isTranslationUnit()) {
   //      property |= kIsNamespace;
   //   }
   return property;
}

long TClingDataMemberInfo::TypeProperty() const
{
   if (!IsValid()) {
      return 0L;
   }
   const clang::ValueDecl *vd = llvm::dyn_cast<clang::ValueDecl>(GetDecl());
   clang::QualType qt = vd->getType();
   return TClingTypeInfo(fInterp, qt).Property();
}

int TClingDataMemberInfo::TypeSize() const
{
   if (!IsValid()) {
      return -1;
   }

   // Sanity check the current data member.
   clang::Decl::Kind dk = GetDecl()->getKind();
   if ((dk != clang::Decl::Field) && (dk != clang::Decl::Var) &&
       (dk != clang::Decl::EnumConstant)) {
      // Error, was not a data member, variable, or enumerator.
      return -1;
   }
   const clang::ValueDecl *vd = llvm::dyn_cast<clang::ValueDecl>(GetDecl());
   clang::QualType qt = vd->getType();
   if (qt->isIncompleteType()) {
      // We cannot determine the size of forward-declared types.
      return -1;
   }
   clang::ASTContext &context = GetDecl()->getASTContext();
   // Truncate cast to fit to cint interface.
   return static_cast<int>(context.getTypeSizeInChars(qt).getQuantity());
}

const char *TClingDataMemberInfo::TypeName() const
{
   if (!IsValid()) {
      return 0;
   }

   CheckForIoTypeAndName();
   if (!fIoType.empty()) return fIoType.c_str();

   // Note: This must be static because we return a pointer inside it!
   static std::string buf;
   buf.clear();
   if (const clang::ValueDecl *vd = llvm::dyn_cast<clang::ValueDecl>(GetDecl())) {
      clang::QualType vdType = vd->getType();
      // In CINT's version, the type name returns did *not* include any array
      // information, ROOT's existing code depends on it.
      while (vdType->isArrayType()) {
         vdType = GetDecl()->getASTContext().getQualifiedType(vdType->getBaseElementTypeUnsafe(),vdType.getQualifiers());
      }

      // if (we_need_to_do_the_subst_because_the_class_is_a_template_instance_of_double32_t)
      vdType = CppyyLegacy::TMetaUtils::ReSubstTemplateArg(vdType, fClassInfo->GetType() );

      CppyyLegacy::TMetaUtils::GetFullyQualifiedTypeName(buf, vdType, *fInterp);
      if (buf.find("(lambda)") != std::string::npos) {
          // TODO: this is a special case that works in conjunction with the support
          // for lambda variables. As of LLVM16, template instantiations of scopes with
          // decltype of a lambda functions will print "(lambda)" as the template arg,
          // instead of an implicit function ptr type. This makes the scope name unusable.
          // For the relevant use case, however, the scope isn't relevant: only the actual
          // type of the data member represented here is. Hence, taking the canonical type
          // is fine (the by-passed typedef isn't used). This need not be generally true,
          // but it's a niche case to begin with and the scoped type isn't going to be
          // usuable as-is. Further, as of C++17, classes can be implicitly instantiated
          // meaning that an `std::function` can be used as a workaround to get at a
          // workable type of a lambda.
          buf.clear();
          CppyyLegacy::TMetaUtils::GetFullyQualifiedTypeName(buf, vdType.getCanonicalType(), *fInterp);
      }

      return buf.c_str();
   }
   return 0;
}

const char *TClingDataMemberInfo::TypeTrueName(const CppyyLegacy::TMetaUtils::TNormalizedCtxt &normCtxt) const
{
   if (!IsValid()) {
      return 0;
   }

   CheckForIoTypeAndName();
   if (!fIoType.empty()) return fIoType.c_str();

   // Note: This must be static because we return a pointer inside it!
   static std::string buf;
   buf.clear();
   if (const clang::ValueDecl *vd = llvm::dyn_cast<clang::ValueDecl>(GetDecl())) {
      // if (we_need_to_do_the_subst_because_the_class_is_a_template_instance_of_double32_t)
      clang::QualType vdType = CppyyLegacy::TMetaUtils::ReSubstTemplateArg(vd->getType(), fClassInfo->GetType());

      CppyyLegacy::TMetaUtils::GetNormalizedName(buf, vdType, *fInterp, normCtxt);

      // In CINT's version, the type name returns did *not* include any array
      // information, ROOT's existing code depends on it.
      // This might become part of the implementation of GetNormalizedName.
      while (buf.length() && buf[buf.length()-1] == ']') {
         size_t last = buf.rfind('['); // if this is not the bracket we are looking, the type is malformed.
         if (last != std::string::npos) {
            buf.erase(last);
         }
      }
      return buf.c_str();
   }
   return 0;
}

const char *TClingDataMemberInfo::Name() const
{
   if (!IsValid()) {
      return 0;
   }

   CheckForIoTypeAndName();
   if (!fIoName.empty()) return fIoName.c_str();

   return TClingDeclInfo::Name();
}

const char *TClingDataMemberInfo::Title()
{
   if (!IsValid()) {
      return 0;
   }

   //NOTE: We can't use it as a cache due to the "thoughtful" self iterator
   //if (fTitle.size())
   //   return fTitle.c_str();

   bool titleFound=false;
   // Try to get the comment either from the annotation or the header file if present
   std::string attribute_s;
   const Decl* decl = GetDecl();
   for (Decl::attr_iterator attrIt = decl->attr_begin();
        attrIt!=decl->attr_end() && !titleFound ;++attrIt){
      if (0 == CppyyLegacy::TMetaUtils::extractAttrString(*attrIt, attribute_s) &&
          attribute_s.find(CppyyLegacy::TMetaUtils::propNames::separator) == std::string::npos){
         fTitle = attribute_s;
         titleFound=true;
      }
   }

   if (!titleFound && !GetDecl()->isFromASTFile()) {
      // Try to get the comment from the header file if present
      // but not for decls from AST file, where rootcling would have
      // created an annotation
      fTitle = CppyyLegacy::TMetaUtils::GetComment(*GetDecl()).str();
   }

   return fTitle.c_str();
}

// ValidArrayIndex return a static string (so use it or copy it immediately, do not
// call GrabIndex twice in the same expression) containing the size of the
// array data member.
llvm::StringRef TClingDataMemberInfo::ValidArrayIndex() const
{
   if (!IsValid()) {
      return llvm::StringRef();
   }
   const clang::DeclaratorDecl *FD = llvm::dyn_cast<clang::DeclaratorDecl>(GetTargetValueDecl());
   if (FD)
      return CppyyLegacy::TMetaUtils::DataMemberInfo__ValidArrayIndex(*fInterp, *FD);
   return {};
}

} // namespace CppyyLegacy
