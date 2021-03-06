//===- GCDAsyncSemaphoreChecker.cpp -----------------------------*- C++ -*-==//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines GCDAsyncSemaphoreChecker which checks against a common
// antipattern when synchronous API is emulated from asynchronous callbacks
// using a semaphor:
//
//   dispatch_semapshore_t sema = dispatch_semaphore_create(0);

//   AnyCFunctionCall(^{
//     // code…
//     dispatch_semapshore_signal(sema);
//   })
//   dispatch_semapshore_wait(sema, *)
//
// Such code is a common performance problem, due to inability of GCD to
// properly handle QoS when a combination of queues and semaphors is used.
// Good code would either use asynchronous API (when available), or perform
// the necessary action in asynchronous callback.
//
// Currently, the check is performed using a simple heuristical AST pattern
// matching.
//
//===----------------------------------------------------------------------===//

#include "ClangSACheckers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/AnalysisManager.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "gcdasyncsemaphorechecker"

using namespace clang;
using namespace ento;
using namespace ast_matchers;

namespace {

const char *WarningBinding = "semaphore_wait";

class GCDAsyncSemaphoreChecker : public Checker<check::ASTCodeBody> {
public:
  void checkASTCodeBody(const Decl *D,
                        AnalysisManager &AM,
                        BugReporter &BR) const;
};

class Callback : public MatchFinder::MatchCallback {
  BugReporter &BR;
  const GCDAsyncSemaphoreChecker *C;
  AnalysisDeclContext *ADC;

public:
  Callback(BugReporter &BR,
           AnalysisDeclContext *ADC,
           const GCDAsyncSemaphoreChecker *C) : BR(BR), C(C), ADC(ADC) {}

  virtual void run(const MatchFinder::MatchResult &Result) override;
};

auto callsName(const char *FunctionName)
    -> decltype(callee(functionDecl())) {
  return callee(functionDecl(hasName(FunctionName)));
}

auto equalsBoundArgDecl(int ArgIdx, const char *DeclName)
    -> decltype(hasArgument(0, expr())) {
  return hasArgument(ArgIdx, ignoringParenCasts(declRefExpr(
                                 to(varDecl(equalsBoundNode(DeclName))))));
}

auto bindAssignmentToDecl(const char *DeclName) -> decltype(hasLHS(expr())) {
  return hasLHS(ignoringParenImpCasts(
                         declRefExpr(to(varDecl().bind(DeclName)))));
}

void GCDAsyncSemaphoreChecker::checkASTCodeBody(const Decl *D,
                                               AnalysisManager &AM,
                                               BugReporter &BR) const {

  // The pattern is very common in tests, and it is OK to use it there.
  if (const auto* ND = dyn_cast<NamedDecl>(D)) {
    std::string DeclName = ND->getNameAsString();
    if (StringRef(DeclName).startswith("test"))
      return;
  }

  const char *SemaphoreBinding = "semaphore_name";
  auto SemaphoreCreateM = callExpr(callsName("dispatch_semaphore_create"));

  auto SemaphoreBindingM = anyOf(
      forEachDescendant(
          varDecl(hasDescendant(SemaphoreCreateM)).bind(SemaphoreBinding)),
      forEachDescendant(binaryOperator(bindAssignmentToDecl(SemaphoreBinding),
                     hasRHS(SemaphoreCreateM))));

  auto SemaphoreWaitM = forEachDescendant(
    callExpr(
      allOf(
        callsName("dispatch_semaphore_wait"),
        equalsBoundArgDecl(0, SemaphoreBinding)
      )
    ).bind(WarningBinding));

  auto HasBlockArgumentM = hasAnyArgument(hasType(
            hasCanonicalType(blockPointerType())
            ));

  auto ArgCallsSignalM = hasArgument(0, hasDescendant(callExpr(
          allOf(
              callsName("dispatch_semaphore_signal"),
              equalsBoundArgDecl(0, SemaphoreBinding)
              ))));

  auto HasBlockAndCallsSignalM = allOf(HasBlockArgumentM, ArgCallsSignalM);

  auto AcceptsBlockM =
    forEachDescendant(
      stmt(anyOf(
        callExpr(HasBlockAndCallsSignalM),
        objcMessageExpr(HasBlockAndCallsSignalM)
           )));

  auto FinalM = compoundStmt(SemaphoreBindingM, SemaphoreWaitM, AcceptsBlockM);

  MatchFinder F;
  Callback CB(BR, AM.getAnalysisDeclContext(D), this);

  F.addMatcher(FinalM, &CB);
  F.match(*D->getBody(), AM.getASTContext());
}

void Callback::run(const MatchFinder::MatchResult &Result) {
  const auto *SW = Result.Nodes.getNodeAs<CallExpr>(WarningBinding);
  assert(SW);
  BR.EmitBasicReport(
      ADC->getDecl(), C,
      /*Name=*/"Semaphore performance anti-pattern",
      /*Category=*/"Performance",
      "Possible semaphore performance anti-pattern: wait on a semaphore "
      "signalled to in a callback",
      PathDiagnosticLocation::createBegin(SW, BR.getSourceManager(), ADC),
      SW->getSourceRange());
}

}

void ento::registerGCDAsyncSemaphoreChecker(CheckerManager &Mgr) {
  Mgr.registerChecker<GCDAsyncSemaphoreChecker>();
}
