/*========================== begin_copyright_notice ============================

Copyright (C) 2023 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

//
/// GenXBuiltinFunctions
/// -----------------
///
/// GenXMathFunction is a module pass that implements floating point math
/// functions
///
//===----------------------------------------------------------------------===//

#include "GenXSubtarget.h"
#include "GenXTargetMachine.h"

#include "vc/Utils/GenX/IntrinsicsWrapper.h"
#include "vc/Utils/GenX/KernelInfo.h"
#include "vc/Utils/General/BiF.h"

#include <llvm/CodeGen/TargetPassConfig.h>
#include <llvm/IR/InstVisitor.h>
#include <llvm/IR/Module.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Pass.h>

#include <string>

#define DEBUG_TYPE "genx-builtin-functions"

using namespace llvm;

class GenXBuiltinFunctions : public ModulePass,
                             public InstVisitor<GenXBuiltinFunctions, Value *> {
public:
  static char ID;

  GenXBuiltinFunctions() : ModulePass(ID) {}
  StringRef getPassName() const override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  bool runOnModule(Module &M) override;
  void runOnFunction(Function &F);

  Value *visitInstruction(Instruction &I) { return nullptr; }

  Value *visitFPToSI(CastInst &I);
  Value *visitFPToUI(CastInst &I);
  Value *visitSIToFP(CastInst &I);
  Value *visitUIToFP(CastInst &I);

  Value *visitFDiv(BinaryOperator &I);
  Value *visitSDiv(BinaryOperator &I);
  Value *visitSRem(BinaryOperator &I);
  Value *visitUDiv(BinaryOperator &I);
  Value *visitURem(BinaryOperator &I);

  Value *visitCallInst(CallInst &II);

private:
  Function *getBuiltinDeclaration(Module &M, StringRef Name, bool IsFast,
                                  ArrayRef<Type *> Types,
                                  StringRef Suffix = "");

  std::unique_ptr<Module> loadBuiltinLib(LLVMContext &Ctx, const DataLayout &DL,
                                         const std::string &Triple);

  Value *createLibraryCall(Instruction &I, Function *Func,
                           ArrayRef<Value *> Args);

  const GenXSubtarget *ST = nullptr;
};

char GenXBuiltinFunctions::ID = 0;

StringRef GenXBuiltinFunctions::getPassName() const {
  return "GenX floating-point math functions";
}

void GenXBuiltinFunctions::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TargetPassConfig>();
  AU.addRequired<GenXBackendConfig>();
}

namespace llvm {
void initializeGenXBuiltinFunctionsPass(PassRegistry &);
} // namespace llvm

INITIALIZE_PASS_BEGIN(GenXBuiltinFunctions, "GenXBuiltinFunctions",
                      "GenXBuiltinFunctions", false, false)
INITIALIZE_PASS_END(GenXBuiltinFunctions, "GenXBuiltinFunctions",
                    "GenXBuiltinFunctions", false, false)

ModulePass *llvm::createGenXBuiltinFunctionsPass() {
  initializeGenXBuiltinFunctionsPass(*PassRegistry::getPassRegistry());
  return new GenXBuiltinFunctions;
}

bool GenXBuiltinFunctions::runOnModule(Module &M) {
  ST = &getAnalysis<TargetPassConfig>()
            .getTM<GenXTargetMachine>()
            .getGenXSubtarget();

  auto Lib =
      loadBuiltinLib(M.getContext(), M.getDataLayout(), M.getTargetTriple());
  if (Lib && Linker::linkModules(M, std::move(Lib)))
    report_fatal_error("Error linking built-in functions");

  for (auto &F : M.getFunctionList())
    runOnFunction(F);

  // Remove unused built-in functions, mark used as internal
  std::vector<Function *> ToErase;
  for (auto &F : M.getFunctionList())
    if (vc::isBuiltinFunction(F)) {
      if (F.use_empty())
        ToErase.push_back(&F);
      else
        F.setLinkage(GlobalValue::InternalLinkage);
    }

  for (auto *F : ToErase)
    F->eraseFromParent();

  return true;
}

void GenXBuiltinFunctions::runOnFunction(Function &F) {
  std::vector<Instruction *> ToErase;
  for (auto &BB : F.getBasicBlockList())
    for (auto &Inst : BB)
      if (auto *NewVal = visit(Inst)) {
        Inst.replaceAllUsesWith(NewVal);
        ToErase.push_back(&Inst);
      }

  for (auto *Inst : ToErase)
    Inst->eraseFromParent();
}

Value *GenXBuiltinFunctions::createLibraryCall(Instruction &I, Function *Func,
                                               ArrayRef<Value *> Args) {
  if (!Func)
    return nullptr;

  IRBuilder<> Builder(&I);
  LLVM_DEBUG(dbgs() << "Replace instruction: " << I << "\n");

  auto *Call = Builder.CreateCall(Func, Args);
  Call->takeName(&I);

  LLVM_DEBUG(dbgs() << "Replaced with: " << *Call << "\n");

  return Call;
}

Value *GenXBuiltinFunctions::visitFPToSI(CastInst &I) {
  auto &M = *I.getModule();
  auto *Arg = I.getOperand(0);
  auto *STy = Arg->getType();
  auto *DTy = I.getType();

  if (!ST->emulateLongLong() || !DTy->getScalarType()->isIntegerTy(64))
    return nullptr;

  auto *Func = getBuiltinDeclaration(M, "fptosi", false, {STy});
  return createLibraryCall(I, Func, {Arg});
}

Value *GenXBuiltinFunctions::visitFPToUI(CastInst &I) {
  auto &M = *I.getModule();
  auto *Arg = I.getOperand(0);
  auto *STy = Arg->getType();
  auto *DTy = I.getType();

  if (!ST->emulateLongLong() || !DTy->getScalarType()->isIntegerTy(64))
    return nullptr;

  auto *Func = getBuiltinDeclaration(M, "fptoui", false, {STy});
  return createLibraryCall(I, Func, {Arg});
}

Value *GenXBuiltinFunctions::visitSIToFP(CastInst &I) {
  auto &M = *I.getModule();
  auto *Arg = I.getOperand(0);
  auto *STy = Arg->getType();
  auto *DTy = I.getType();

  if (!ST->emulateLongLong() || !STy->getScalarType()->isIntegerTy(64))
    return nullptr;

  auto *Func = getBuiltinDeclaration(M, "sitofp", false, {DTy});
  return createLibraryCall(I, Func, {Arg});
}

Value *GenXBuiltinFunctions::visitUIToFP(CastInst &I) {
  auto &M = *I.getModule();
  auto *Arg = I.getOperand(0);
  auto *STy = Arg->getType();
  auto *DTy = I.getType();

  if (!ST->emulateLongLong() || !STy->getScalarType()->isIntegerTy(64))
    return nullptr;

  auto *Func = getBuiltinDeclaration(M, "uitofp", false, {DTy});
  return createLibraryCall(I, Func, {Arg});
}

Value *GenXBuiltinFunctions::visitFDiv(BinaryOperator &I) {
  auto &M = *I.getModule();
  auto *Ty = I.getType();

  auto *Func = getBuiltinDeclaration(M, "fdiv", I.hasAllowReciprocal(), {Ty});
  return createLibraryCall(I, Func, {I.getOperand(0), I.getOperand(1)});
}

Value *GenXBuiltinFunctions::visitSDiv(BinaryOperator &I) {
  auto &M = *I.getModule();
  auto *Ty = I.getType();
  auto *STy = Ty->getScalarType();

  if (ST->hasIntDivRem32() && !STy->isIntegerTy(64))
    return nullptr;

  StringRef Suffix = STy->isIntegerTy(32) ? "__rtz_" : "";

  auto *Func = getBuiltinDeclaration(M, "sdiv", false, {Ty}, Suffix);
  return createLibraryCall(I, Func, {I.getOperand(0), I.getOperand(1)});
}

Value *GenXBuiltinFunctions::visitSRem(BinaryOperator &I) {
  auto &M = *I.getModule();
  auto *Ty = I.getType();
  auto *STy = Ty->getScalarType();

  if (ST->hasIntDivRem32() && !STy->isIntegerTy(64))
    return nullptr;

  StringRef Suffix = STy->isIntegerTy(32) ? "__rtz_" : "";

  auto *Func = getBuiltinDeclaration(M, "srem", false, {Ty}, Suffix);
  return createLibraryCall(I, Func, {I.getOperand(0), I.getOperand(1)});
}

Value *GenXBuiltinFunctions::visitUDiv(BinaryOperator &I) {
  auto &M = *I.getModule();
  auto *Ty = I.getType();
  auto *STy = Ty->getScalarType();

  if (ST->hasIntDivRem32() && !STy->isIntegerTy(64))
    return nullptr;

  StringRef Suffix = STy->isIntegerTy(32) ? "__rtz_" : "";

  auto *Func = getBuiltinDeclaration(M, "udiv", false, {Ty}, Suffix);
  return createLibraryCall(I, Func, {I.getOperand(0), I.getOperand(1)});
}

Value *GenXBuiltinFunctions::visitURem(BinaryOperator &I) {
  auto &M = *I.getModule();
  auto *Ty = I.getType();
  auto *STy = Ty->getScalarType();

  if (ST->hasIntDivRem32() && !STy->isIntegerTy(64))
    return nullptr;

  StringRef Suffix = STy->isIntegerTy(32) ? "__rtz_" : "";

  auto *Func = getBuiltinDeclaration(M, "urem", false, {Ty}, Suffix);
  return createLibraryCall(I, Func, {I.getOperand(0), I.getOperand(1)});
}

Value *GenXBuiltinFunctions::visitCallInst(CallInst &II) {
  auto IID = vc::getAnyIntrinsicID(&II);
  auto *Ty = II.getType();
  auto &M = *II.getModule();
  Function *Func = nullptr;

  switch (IID) {
  case Intrinsic::sqrt:
    Func = getBuiltinDeclaration(M, "fsqrt", II.hasApproxFunc(), {Ty});
    break;

  case GenXIntrinsic::genx_sqrt:
    Func = getBuiltinDeclaration(M, "fsqrt", true, {Ty});
    break;
  case GenXIntrinsic::genx_ieee_sqrt:
    Func = getBuiltinDeclaration(M, "fsqrt", false, {Ty});
    break;
  case GenXIntrinsic::genx_ieee_div:
    Func = getBuiltinDeclaration(M, "fdiv", false, {Ty});
    break;

  case GenXIntrinsic::genx_fptosi_sat: {
    auto *Arg = II.getArgOperand(0);
    auto *STy = Arg->getType();
    if (!ST->emulateLongLong() || !Ty->getScalarType()->isIntegerTy(64))
      return nullptr;
    Func = getBuiltinDeclaration(M, "fptosi", false, {STy});
  } break;
  case GenXIntrinsic::genx_fptoui_sat: {
    auto *Arg = II.getArgOperand(0);
    auto *STy = Arg->getType();
    if (!ST->emulateLongLong() || !Ty->getScalarType()->isIntegerTy(64))
      return nullptr;
    Func = getBuiltinDeclaration(M, "fptoui", false, {STy});
  } break;

  case vc::InternalIntrinsic::lsc_atomic_slm: {
    IRBuilder<> Builder(&II);
    auto *Opcode = cast<ConstantInt>(II.getArgOperand(1));
    if (Opcode->getZExtValue() == LSC_ATOMIC_ICAS)
      return nullptr;
    auto *VTy = cast<IGCLLVM::FixedVectorType>(Ty);
    auto *ETy = VTy->getElementType();
    if (!ST->hasLocalIntegerCas64() || !ETy->isIntegerTy(64))
      return nullptr;
    Func = getBuiltinDeclaration(M, "atomic_slm", false, {VTy});

    auto *MaskVTy = IGCLLVM::FixedVectorType::get(Builder.getInt8Ty(),
                                                  VTy->getNumElements());
    auto *Mask = Builder.CreateZExt(II.getArgOperand(0), MaskVTy);
    SmallVector<Value *, 10> Args = {Mask, Opcode};
    std::copy(II.arg_begin() + 4, II.arg_end() - 2, std::back_inserter(Args));
    Args.push_back(II.getArgOperand(12));

    return createLibraryCall(II, Func, Args);
  }

  default:
    break;
  }

  SmallVector<Value *, 2> Args(II.args());
  return createLibraryCall(II, Func, Args);
}

static std::string getMangledTypeStr(Type *Ty) {
  std::string Result;
  if (auto *VTy = dyn_cast<IGCLLVM::FixedVectorType>(Ty))
    Result += "v" + utostr(VTy->getNumElements()) +
              getMangledTypeStr(VTy->getElementType());
  else if (Ty)
    Result += EVT::getEVT(Ty).getEVTString();
  return Result;
}

Function *GenXBuiltinFunctions::getBuiltinDeclaration(Module &M, StringRef Name,
                                                      bool IsFast,
                                                      ArrayRef<Type *> Types,
                                                      StringRef Suffix) {
  std::string FuncName = vc::LibraryFunctionPrefix;
  FuncName += Name;
  if (IsFast)
    FuncName += "_fast";

  for (auto *Ty : Types)
    FuncName += "_" + getMangledTypeStr(Ty);

  FuncName += Suffix;

  auto *Func = M.getFunction(FuncName);
  return Func;
}

std::unique_ptr<Module>
GenXBuiltinFunctions::loadBuiltinLib(LLVMContext &Ctx, const DataLayout &DL,
                                     const std::string &Triple) {
  MemoryBufferRef BiFBuffer =
      getAnalysis<GenXBackendConfig>().getBiFModule(BiFKind::VCBuiltins);

  // NOTE: to simplify LIT testing it is legal to have an empty buffer
  if (BiFBuffer.getBufferSize() == 0)
    return nullptr;

  auto BiFModule = vc::getBiFModuleOrReportError(BiFBuffer, Ctx);

  BiFModule->setDataLayout(DL);
  BiFModule->setTargetTriple(Triple);

  return BiFModule;
}
