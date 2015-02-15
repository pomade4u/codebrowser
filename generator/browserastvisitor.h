/****************************************************************************
 * Copyright (C) 2012-2014 Woboq GmbH
 * Olivier Goffart <contact at woboq.com>
 * http://woboq.com/codebrowser.html
 *
 * This file is part of the Woboq Code Browser.
 *
 * Commercial License Usage:
 * Licensees holding valid commercial licenses provided by Woboq may use
 * this file in accordance with the terms contained in a written agreement
 * between the licensee and Woboq.
 * For further information see http://woboq.com/codebrowser.html
 *
 * Alternatively, this work may be used under a Creative Commons
 * Attribution-NonCommercial-ShareAlike 3.0 (CC-BY-NC-SA 3.0) License.
 * http://creativecommons.org/licenses/by-nc-sa/3.0/deed.en_US
 * This license does not allow you to use the code browser to assist the
 * development of your commercial software. If you intent to do so, consider
 * purchasing a commercial licence.
 ****************************************************************************/

#pragma once

#include "annotator.h"
#include "qtsupport.h"
#include <clang/AST/ASTConsumer.h>
#include <clang/AST/DeclGroup.h>
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Mangle.h>
#include <clang/Lex/Lexer.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Basic/Version.h>


#include <iostream>

struct BrowserASTVisitor : clang::RecursiveASTVisitor<BrowserASTVisitor> {
    typedef clang::RecursiveASTVisitor<BrowserASTVisitor> Base;
    Annotator &annotator;
    clang::NamedDecl *currentContext = nullptr;
    std::deque<clang::Expr *> expr_stack;
    BrowserASTVisitor(Annotator &R) : annotator(R) {}

    bool VisitTypedefNameDecl(clang::TypedefNameDecl *d) {
        annotator.registerReference(d, d->getLocation(), Annotator::Typedef, Annotator::Declaration,
                                    annotator.getTypeRef(d->getUnderlyingType()));
        return true;
    }

    bool VisitTagDecl(clang::TagDecl *d) {
        if(!shouldProcess(d)) return false;
        if (d->isThisDeclarationADefinition()) {
            if (clang::CXXRecordDecl* cxx = llvm::dyn_cast<clang::CXXRecordDecl>(d)) {
                for (auto it = cxx->bases_begin(); it != cxx->bases_end(); ++it) {
                    if (! it->getType()->getAsCXXRecordDecl()) {
//                        std::cerr << " INHERITING but not from a CXXRecrod " << std::endl;
//                        it->getType().dump();
  // probably template type...  FIXME
                        continue;
                    }
                    annotator.registerOverride(d, it->getType()->getAsCXXRecordDecl(), d->getLocation());
                }
            }
        }
        annotator.registerReference(d, d->getLocation(), Annotator::Type,
                             d->isThisDeclarationADefinition() ? Annotator::Definition : Annotator::Declaration);
        return true;
    }

    bool VisitNamespaceDecl(clang::NamespaceDecl *d) {
        annotator.registerReference(d, d->getLocation(), Annotator::Namespace, Annotator::Declaration);
        return true;
    }
    bool VisitNamespaceAliasDecl(clang::NamespaceAliasDecl *d) {
        annotator.registerReference(d, d->getLocation(), Annotator::Namespace, Annotator::Declaration);
        return true;
    }
    bool VisitFunctionDecl(clang::FunctionDecl *d) {
        if(!shouldProcess(d)) return false;
        std::string typeText;
        {
            llvm::raw_string_ostream typeTextStream(typeText);

            bool isConst = false;
            if (clang::CXXMethodDecl *cxx = llvm::dyn_cast<clang::CXXMethodDecl>(d)) {
                if (cxx->isStatic())
                    typeTextStream << "static ";
#if CLANG_VERSION_MAJOR!=3 || CLANG_VERSION_MINOR>1
                isConst =  cxx->isConst();
#endif
                if (cxx->isThisDeclarationADefinition()) {
                    for (auto it = cxx->begin_overridden_methods(); it != cxx->end_overridden_methods(); ++it) {
                        const clang::CXXMethodDecl *ovr = (*it)->getCanonicalDecl();
                        annotator.registerOverride(d, const_cast<clang::CXXMethodDecl*>(ovr), d->getNameInfo().getBeginLoc());
                    }
                }
            }
            typeTextStream << annotator.getTypeRef(getResultType(d)) << " " << d->getQualifiedNameAsString() << "(";
            for (uint i = 0; i < d->getNumParams(); i++) {
                if (i!=0) typeTextStream << ", ";
                clang::ParmVarDecl* PVD = d->getParamDecl(i);
                typeTextStream << annotator.getTypeRef(PVD->getType()) << " " << PVD->getName();
                if (PVD->hasDefaultArg() && !PVD->hasUninstantiatedDefaultArg()) {
                    typeTextStream << " = ";
                    PVD->getDefaultArg()->printPretty(typeTextStream, 0, annotator.getLangOpts());
                }
            }
            typeTextStream << ")";
            if (isConst) {
                typeTextStream << " const";
            }
        }

        annotator.registerReference(d, d->getNameInfo().getSourceRange(), Annotator::Decl,
                             d->isThisDeclarationADefinition() ? Annotator::Definition : Annotator::Declaration,
                             typeText);
        return true;
    }
    bool VisitEnumConstantDecl(clang::EnumConstantDecl *d) {
        annotator.registerReference(d, d->getLocation(), Annotator::EnumDecl, Annotator::Declaration, d->getInitVal().toString(10));
        return true;
    }
    bool VisitVarDecl(clang::VarDecl *d) {
        if(!shouldProcess(d)) return false;
        annotator.registerReference(d, d->getLocation(), Annotator::Decl,
                             d->isThisDeclarationADefinition() ? Annotator::Definition : Annotator::Declaration,
                             annotator.getTypeRef(d->getType()));
        return true;
    }
    bool VisitFieldDecl(clang::FieldDecl *d) {
        annotator.registerReference(d, d->getLocation(), Annotator::Decl, Annotator::Declaration,
                                    annotator.getTypeRef(d->getType()));
        return true;
    }

    bool VisitMemberExpr(clang::MemberExpr *e) {
       annotator.registerUse(e->getMemberDecl(), e->getMemberNameInfo().getSourceRange() ,
                             isMember(e->getMemberDecl()) ? Annotator::Member : Annotator::Ref,
                             currentContext, classify());
       return true;
    }
    bool VisitDeclRefExpr(clang::DeclRefExpr *e) {
        clang::ValueDecl* decl = e->getDecl();
        annotator.registerUse(decl, e->getNameInfo().getSourceRange(),
                            llvm::isa<clang::EnumConstantDecl>(decl) ? Annotator::EnumDecl :
                            isMember(decl) ? Annotator::Member : Annotator::Ref,
                            currentContext, classify());
       return true;
   }

   bool VisitTypedefTypeLoc(clang::TypedefTypeLoc TL) {
       clang::SourceRange range = TL.getSourceRange();
       annotator.registerReference(TL.getTypedefNameDecl(), range,  Annotator::Typedef, Annotator::Use,
                                   annotator.getTypeRef(TL.getTypedefNameDecl()->getUnderlyingType()),
                                   currentContext);
       return true;
   }

   bool VisitTagTypeLoc(clang::TagTypeLoc TL) {
       clang::SourceRange range = TL.getSourceRange();
       annotator.registerUse(TL.getDecl(), range.getBegin(), Annotator::Type, currentContext);
       return true;
   }

    bool VisitTemplateSpecializationTypeLoc(clang::TemplateSpecializationTypeLoc TL) {

        clang::TemplateDecl* decl = TL.getTypePtr()->getTemplateName().getAsTemplateDecl();
        if (decl) {
            auto loc = TL.getTemplateNameLoc();
            annotator.registerUse(decl, loc, Annotator::Type, currentContext);
        } else {
            std::cerr << "VisitTemplateSpecializationTypeLoc " << " "<< TL.getType().getAsString();
        }
        return true;
    }

    bool TraverseNestedNameSpecifierLoc(clang::NestedNameSpecifierLoc NNS) {
        if (!NNS)
            return true;

        switch (NNS.getNestedNameSpecifier()->getKind()) {
            case clang::NestedNameSpecifier::Namespace:
                if (NNS.getNestedNameSpecifier()->getAsNamespace()->isAnonymousNamespace())
                    break;
                annotator.registerReference(NNS.getNestedNameSpecifier()->getAsNamespace(),
                                     NNS.getSourceRange(), Annotator::Namespace);
                return true; // skip prefixes
            case clang::NestedNameSpecifier::NamespaceAlias:
                annotator.registerReference(NNS.getNestedNameSpecifier()->getAsNamespaceAlias()->getAliasedNamespace(),
                                     NNS.getSourceRange(), Annotator::Namespace);
                return true; //skip prefixes
            default: break;
        }
        return Base::TraverseNestedNameSpecifierLoc(NNS);
    }
    bool TraverseUsingDirectiveDecl(clang::UsingDirectiveDecl *d) {
        annotator.registerReference(d->getNominatedNamespace(),
                                    { d->getQualifierLoc().getBeginLoc(), d->getIdentLocation() }
                                    , Annotator::Namespace);
        // don't call Base::TraverseUsingDirectiveDecl in order to skip prefix
        return true;
    }

    // the initializers would not be highlighted otherwise
    bool TraverseConstructorInitializer(clang::CXXCtorInitializer *Init) {
        if (Init->isAnyMemberInitializer() && Init->isWritten()) {
            annotator.registerUse(Init->getAnyMember(), Init->getMemberLocation(),
                                  Init->isMemberInitializer() ? Annotator::Member : Annotator::Ref,
                                  currentContext);
        }
        return Base::TraverseConstructorInitializer(Init);
    }

    // try to put a link to the right constructor
    bool VisitCXXConstructExpr(clang::CXXConstructExpr *ctr) {
        if (clang::CXXConstructorDecl *decl = ctr->getConstructor()) {
#if CLANG_VERSION_MAJOR==3 && CLANG_VERSION_MINOR<4
            clang::SourceLocation parenLoc = ctr->getParenRange().getBegin();
#else
            clang::SourceLocation parenLoc = ctr->getParenOrBraceRange().getBegin();
#endif

            // Highlight the opening parenthese
            annotator.registerUse(decl, parenLoc, Annotator::Ref, currentContext);
        }
        QtSupport qt{annotator, currentContext};
        qt.visitCXXConstructExpr(ctr);
        return true;
    }

    bool TraverseDecl(clang::Decl *d) {
        if (!d) return true;
        auto saved = currentContext;
        if (clang::FunctionDecl::classof(d) || clang::RecordDecl::classof(d) ||
            clang::NamespaceDecl::classof(d) || clang::TemplateDecl::classof(d)) {
            currentContext = llvm::dyn_cast<clang::NamedDecl>(d);
        }
        Base::TraverseDecl(d);
        currentContext = saved;
        return true;
    }

    // Since we cannot find up the parent of a node, we keep a stack of parents
    bool TraverseStmt(clang::Stmt *s) {
        auto e = llvm::dyn_cast_or_null<clang::Expr>(s);
        decltype(expr_stack) old_stack;
        if (e) {
            expr_stack.push_front(e);
        } else {
            std::swap(old_stack, expr_stack);
        }
        auto r = Base::TraverseStmt(s);
        if (e) {
            expr_stack.pop_front();
        } else {
            std::swap(old_stack, expr_stack);
        }
        return r;
    }

    bool TraverseDeclarationNameInfo(clang::DeclarationNameInfo NameInfo) {
        // Do not visit the TypeLoc of constructor or destructors
        return true;
    }


    // This is only to support QObject::connect SIGNAL and SLOT
    bool VisitCallExpr(clang::CallExpr *e) {
        QtSupport qt{annotator, currentContext};
        qt.visitCallExpr(e);
        return true;
    }

private:

    Annotator::DeclType classify() {
        bool first = true;
        clang::Expr *previous = nullptr;
        for (auto expr : expr_stack) {
            if (first) {
                previous = expr;
                first = false;
                continue; //skip the first element (ourself)
            }
            if (llvm::isa<clang::MemberExpr>(expr)) {
                return Annotator::Use_MemberAccess;
            }
            if (auto op = llvm::dyn_cast<clang::BinaryOperator>(expr)) {
                if (op->isAssignmentOp() && op->getLHS() == previous)
                    return Annotator::Use_Write;
                return Annotator::Use_Read;
            }
            if (auto op = llvm::dyn_cast<clang::UnaryOperator>(expr)) {
                if (op->isIncrementDecrementOp())
                    return Annotator::Use_Write;
                if (op->isArithmeticOp() || op->getOpcode() == clang::UO_Deref)
                    return Annotator::Use_Read;
                if (op->getOpcode() == clang::UO_AddrOf)
                    return Annotator::Use_Address;
                return Annotator::Use;
            }
            if (auto op = llvm::dyn_cast<clang::CXXOperatorCallExpr>(expr)) {
                // Special case for some of the CXXOperatorCallExpr to check if it is Use_Write
                // Anything else goes through normal CallExpr
                auto o = op->getOperator();
                if (o == clang::OO_Equal || (o >= clang::OO_PlusEqual && o <= clang::OO_PipeEqual)
                        || (o >= clang::OO_LessLessEqual && o <= clang::OO_GreaterGreaterEqual)
                        || (o >= clang::OO_PlusPlus && o <= clang::OO_MinusMinus) ) {
                    if (op->getNumArgs() >= 1 && op->getArg(0) == previous && op->getDirectCallee()
                            && op->getDirectCallee()->getNumParams() >= 1) {
                        auto t = op->getDirectCallee()->getParamDecl(0)->getType();
                        if (t->isReferenceType() && !t.getNonReferenceType().isConstQualified()) {
                            return Annotator::Use_Write;
                        }
                    }
                }
            }
            if (auto call = llvm::dyn_cast<clang::CallExpr>(expr)) {
                if (previous == call->getCallee())
                    return Annotator::Use_Call;
                auto decl = call->getDirectCallee();
                for (uint i = 0; i < call->getNumArgs(); ++i) {
                    if (!decl || decl->getNumParams() <= i)
                        break;
                    if (call->getArg(i) != previous)
                        continue;
                    auto t = decl->getParamDecl(i)->getType();
                    if (t->isReferenceType() && !t.getNonReferenceType().isConstQualified())
                        return Annotator::Use_Address; // non const reference
                    return Annotator::Use_Read; // anything else is considered as read;
                }
                return Annotator::Use;
            }


            previous = expr;
        }
        return Annotator::Use;
    }


    bool isMember(clang::NamedDecl *d) {
        if (!currentContext)
            return false;
        clang::CXXRecordDecl *ctx = llvm::dyn_cast<clang::CXXRecordDecl>(currentContext->getDeclContext());
        if (!ctx) return false;
        if (d->getDeclContext() == ctx)
            return true;

        // try to see if it is in a inhertited class
        clang::CXXRecordDecl *rec = llvm::dyn_cast<clang::CXXRecordDecl>(d->getDeclContext());
        return rec && ctx->isDerivedFrom(rec);
    }

    bool shouldProcess(clang::NamedDecl *d) {
        return annotator.shouldProcess(clang::FullSourceLoc(d->getLocation(),
                            annotator.getSourceMgr()).getExpansionLoc().getFileID());
    }
};
