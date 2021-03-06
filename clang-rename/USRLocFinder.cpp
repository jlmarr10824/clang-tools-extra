//===--- tools/extra/clang-rename/USRLocFinder.cpp - Clang rename tool ----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Mehtods for finding all instances of a USR. Our strategy is very
/// simple; we just compare the USR at every relevant AST node with the one
/// provided.
///
//===----------------------------------------------------------------------===//

#include "USRLocFinder.h"
#include "USRFinder.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Index/USRGeneration.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/SmallVector.h"

using namespace llvm;

namespace clang {
namespace rename {

namespace {
// \brief This visitor recursively searches for all instances of a USR in a
// translation unit and stores them for later usage.
class USRLocFindingASTVisitor
    : public clang::RecursiveASTVisitor<USRLocFindingASTVisitor> {
public:
  explicit USRLocFindingASTVisitor(StringRef USR, StringRef PrevName)
      : USR(USR), PrevName(PrevName) {}

  // Declaration visitors:

  bool VisitNamedDecl(const NamedDecl *Decl) {
    if (getUSRForDecl(Decl) == USR) {
      LocationsFound.push_back(Decl->getLocation());
    }
    return true;
  }

  bool VisitVarDecl(clang::VarDecl *Decl) {
    clang::QualType Type = Decl->getType();
    const clang::RecordDecl *RecordDecl = Type->getPointeeCXXRecordDecl();
    if (RecordDecl) {
      if (getUSRForDecl(RecordDecl) == USR) {
        // The declaration refers to a type that is to be renamed.
        LocationsFound.push_back(Decl->getTypeSpecStartLoc());
      }
    }
    return true;
  }

  bool VisitCXXConstructorDecl(clang::CXXConstructorDecl *ConstructorDecl) {
    const ASTContext &Context = ConstructorDecl->getASTContext();
    for (auto &Initializer : ConstructorDecl->inits()) {
      if (Initializer->getSourceOrder() == -1) {
        // Ignore implicit initializers.
        continue;
      }

      if (const clang::FieldDecl *FieldDecl = Initializer->getAnyMember()) {
        if (getUSRForDecl(FieldDecl) == USR) {
          // The initializer refers to a field that is to be renamed.
          SourceLocation Location = Initializer->getSourceLocation();
          StringRef TokenName = Lexer::getSourceText(
              CharSourceRange::getTokenRange(Location),
              Context.getSourceManager(), Context.getLangOpts());
          if (TokenName == PrevName) {
            // The token of the source location we find actually has the old
            // name.
            LocationsFound.push_back(Initializer->getSourceLocation());
          }
        }
      }
    }

    if (getUSRForDecl(ConstructorDecl) == USR) {
      // This takes care of the class name part of a non-inline ctor definition.
      LocationsFound.push_back(ConstructorDecl->getLocStart());
    }
    return true;
  }

  bool VisitCXXDestructorDecl(clang::CXXDestructorDecl *DestructorDecl) {
    if (getUSRForDecl(DestructorDecl->getParent()) == USR) {
      // Handles "~Foo" from "Foo::~Foo".
      SourceLocation Location = DestructorDecl->getLocation();
      const ASTContext &Context = DestructorDecl->getASTContext();
      StringRef LLVM_ATTRIBUTE_UNUSED TokenName = Lexer::getSourceText(
          CharSourceRange::getTokenRange(Location), Context.getSourceManager(),
          Context.getLangOpts());
      // 1 is the length of the "~" string that is not to be touched by the
      // rename.
      assert(TokenName.startswith("~"));
      LocationsFound.push_back(Location.getLocWithOffset(1));

      if (DestructorDecl->isThisDeclarationADefinition()) {
        // Handles "Foo" from "Foo::~Foo".
        LocationsFound.push_back(DestructorDecl->getLocStart());
      }
    }

    return true;
  }

  // Expression visitors:

  bool VisitDeclRefExpr(const DeclRefExpr *Expr) {
    const auto *Decl = Expr->getFoundDecl();

    checkNestedNameSpecifierLoc(Expr->getQualifierLoc());
    if (getUSRForDecl(Decl) == USR) {
      const SourceManager &Manager = Decl->getASTContext().getSourceManager();
      SourceLocation Location = Manager.getSpellingLoc(Expr->getLocation());
      LocationsFound.push_back(Location);
    }

    return true;
  }

  bool VisitMemberExpr(const MemberExpr *Expr) {
    const auto *Decl = Expr->getFoundDecl().getDecl();
    if (getUSRForDecl(Decl) == USR) {
      const SourceManager &Manager = Decl->getASTContext().getSourceManager();
      SourceLocation Location = Manager.getSpellingLoc(Expr->getMemberLoc());
      LocationsFound.push_back(Location);
    }
    return true;
  }

  bool VisitCXXConstructExpr(const CXXConstructExpr *Expr) {
    CXXConstructorDecl *Decl = Expr->getConstructor();

    if (getUSRForDecl(Decl) == USR) {
      // This takes care of 'new <name>' expressions.
      LocationsFound.push_back(Expr->getLocation());
    }

    return true;
  }

  bool VisitCXXStaticCastExpr(clang::CXXStaticCastExpr *Expr) {
    return handleCXXNamedCastExpr(Expr);
  }

  bool VisitCXXDynamicCastExpr(clang::CXXDynamicCastExpr *Expr) {
    return handleCXXNamedCastExpr(Expr);
  }

  bool VisitCXXReinterpretCastExpr(clang::CXXReinterpretCastExpr *Expr) {
    return handleCXXNamedCastExpr(Expr);
  }

  bool VisitCXXConstCastExpr(clang::CXXConstCastExpr *Expr) {
    return handleCXXNamedCastExpr(Expr);
  }

  // Non-visitors:

  // \brief Returns a list of unique locations. Duplicate or overlapping
  // locations are erroneous and should be reported!
  const std::vector<clang::SourceLocation> &getLocationsFound() const {
    return LocationsFound;
  }

private:
  // Namespace traversal:
  void checkNestedNameSpecifierLoc(NestedNameSpecifierLoc NameLoc) {
    while (NameLoc) {
      const auto *Decl = NameLoc.getNestedNameSpecifier()->getAsNamespace();
      if (Decl && getUSRForDecl(Decl) == USR)
        LocationsFound.push_back(NameLoc.getLocalBeginLoc());
      NameLoc = NameLoc.getPrefix();
    }
  }

  bool handleCXXNamedCastExpr(clang::CXXNamedCastExpr *Expr) {
    clang::QualType Type = Expr->getType();
    // See if this a cast of a pointer.
    const RecordDecl *Decl = Type->getPointeeCXXRecordDecl();
    if (!Decl) {
      // See if this is a cast of a reference.
      Decl = Type->getAsCXXRecordDecl();
    }

    if (Decl && getUSRForDecl(Decl) == USR) {
      SourceLocation Location =
          Expr->getTypeInfoAsWritten()->getTypeLoc().getBeginLoc();
      LocationsFound.push_back(Location);
    }

    return true;
  }

  // All the locations of the USR were found.
  const std::string USR;
  // Old name that is renamed.
  const std::string PrevName;
  std::vector<clang::SourceLocation> LocationsFound;
};
} // namespace

std::vector<SourceLocation> getLocationsOfUSR(StringRef USR, StringRef PrevName,
                                              Decl *Decl) {
  USRLocFindingASTVisitor Visitor(USR, PrevName);

  Visitor.TraverseDecl(Decl);
  return Visitor.getLocationsFound();
}

} // namespace rename
} // namespace clang
