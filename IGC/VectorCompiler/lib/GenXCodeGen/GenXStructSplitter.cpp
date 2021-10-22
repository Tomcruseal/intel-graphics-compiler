/*========================== begin_copyright_notice ============================

Copyright (C) 2021 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

//
/// GenXStructSplitter
/// ------------------
/// It is a module pass whose purpose is to split all complicate structs into
/// plain substructs for further optimizations.
/// eg. {vec3f, vec3f, f, vec5i} will become {vec3f, vec3f, f} {vec5i}.
///
/// It does in 2 main steps:
/// 1. Resolves which structs can be splitted and splits it.
///   a. Collects all structs.
///   b. Creates DependencyGraph of struct usage.
///       Which structs contain which structs.
///   c. Splits structs.
/// 2. Replaces all structures if it is possible.
///   a. Replaces allocas.
///   b. Replaces all uses of allocas (GEP and PTI).
///     I. Replace all uses of GEP and PTI.
///
/// Ex. (C-like):
///   struct A = {int, float};
///   A a;
///   int i = a.int;
/// Will become:
///   struct Ai = {int};
///   struct Af = {float};
///   Ai ai;
///   Af af;
///   int i = ai.int;
///
/// Limitations:
///   1. Structure contains array of complex structs.
///   2. Structure is allocated as an array.
///   3. Structure contains prohibitted structure.
///   4. Structure using instruction is not GEP, PTI, alloca.
///   5. Users of the PTI not add, insertelement, shufflevector, read/write.
///   6. Pointer of the structure goes in function (except read/write).
///   7. Pointer offset from the begging of the structure covers different
///      types.
///   8. Pointer offset from the begging of the structure covers unsequential
///      splitted structs.
///
//===----------------------------------------------------------------------===//

#include "GenX.h"
#include "GenXSubtarget.h"
#include "GenXTargetMachine.h"

#include "vc/Support/GenXDiagnostic.h"

#include <llvm/CodeGen/TargetPassConfig.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstVisitor.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Pass.h>
#include <llvm/PassRegistry.h>
#include <llvm/Support/Debug.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvmWrapper/Support/Alignment.h>

#include "Probe/Assertion.h"

#include <unordered_map>
#include <unordered_set>

using namespace llvm;

#define DEBUG_TYPE "GENX_STRUCT_SPLITTER"

static cl::opt<bool> PerformStructSplitting(
    "vc-struct-splitting", cl::init(false), cl::Hidden,
    cl::desc(
        "Performs splitting complicate-constucted structs to plain structs."));

namespace {

class GenXStructSplitter : public ModulePass {
public:
  static char ID;

  explicit GenXStructSplitter() : ModulePass(ID) {}
  ~GenXStructSplitter() = default;

  StringRef getPassName() const override { return "GenX struct splitter"; }
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  bool runOnModule(Module &M) override;
};

} // namespace

namespace llvm {
void initializeGenXStructSplitterPass(PassRegistry &);
}

char GenXStructSplitter::ID = 0;
INITIALIZE_PASS_BEGIN(GenXStructSplitter, "GenXStructSplitter",
                      "GenXStructSplitter", false, true /*analysis*/)
INITIALIZE_PASS_DEPENDENCY(GenXBackendConfig)
INITIALIZE_PASS_END(GenXStructSplitter, "GenXStructSplitter",
                    "GenXStructSplitter", false, true /*analysis*/)

ModulePass *llvm::createGenXStructSplitterPass() {
  initializeGenXStructSplitterPass(*PassRegistry::getPassRegistry());
  return new GenXStructSplitter;
}

void GenXStructSplitter::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<GenXBackendConfig>();
  AU.setPreservesAll();
}

static Type *getArrayFreeTy(Type *Ty);
static Type *getBaseTy(Type *Ty);

// Class to do first analysis and ban all structures, which cannot be splitted
// at advance. It bans structures containing array of complex structs. It bans
// structures containing banned structs. It bans structures, which are allocated
// as an array.
class StructFilter : public InstVisitor<StructFilter> {
  std::unordered_set<StructType *> BannedStructs;

  bool checkForArrayOfComplicatedStructs(StructType &STy) const;
  bool checkForElementOfBannedStruct(StructType &STy) const;

public:
  StructFilter(Module &M);
  bool isStructBanned(StructType &STy) const;
  void visitAllocaInst(AllocaInst &AI);

  void print(raw_ostream &os = llvm::errs()) const;
};

// Class to handle all information about Structs which are used in Module.
// It contains initial structs info, performs struct splitting,
// contains transformation of structs and final structs info in Module.
// Example of work of this class is showed with dependencies:
// C : { [5 x A], i, f, [5 x B], D }
// A : { f, [5 x f] }
// B : { i, [5 x i] }
// D : { A, B }
// Where A,B,C,D are structs; f,i - base unsplitted types.
class DependencyGraph {
  static char const *getTypePrefix(Type &Ty);

public:
  //***************************************
  // Part responsible for types definition
  //***************************************

  // Helped struct contains splitted struct and position of data.
  // Just a replacement of std::pair for more informative access to Ty and
  // Index.
  struct SElement {
    StructType *Ty{nullptr};
    unsigned Index{0};

    SElement() = default;
    SElement(StructType *const &InTy, unsigned InIndex) noexcept;
    SElement &operator=(SElement const &Other) noexcept;
    void print(raw_ostream &os = llvm::errs()) const;
  };

  // Helped class contains array of Types and Indices on which Type is placed.
  // It is used for keeping elements of structure within the same subtype.
  class SElementsOfType {
    std::vector<Type *> Types;
    // vector of Indices correspondence to vector of Types
    std::vector<unsigned> IndicesOfTypes;

  public:
    SElementsOfType(unsigned Size);
    SElementsOfType(SElementsOfType const &Other);
    SElementsOfType(SElementsOfType &&Other) noexcept;
    SElementsOfType(std::vector<Type *> const &InTypes);

    void emplaceBack(Type &Ty, unsigned Index);
    unsigned size() const;

    Type *getTyAt(unsigned Index) const;
    unsigned getIdxAt(unsigned Index) const;
    std::pair<Type *&, unsigned &> at(unsigned Index);
    std::pair<Type *const &, unsigned const &> at(unsigned Index) const;

    std::vector<Type *> const &getTypesArray() const { return Types; }

    using TypeIt = std::vector<Type *>::iterator;
    using TypeIt_const = std::vector<Type *>::const_iterator;
    using IdxIt = std::vector<unsigned>::iterator;
    using IdxIt_const = std::vector<unsigned>::const_iterator;
    TypeIt begin_ty() { return Types.begin(); }
    TypeIt_const begin_ty() const { return Types.begin(); }
    TypeIt end_ty() { return Types.end(); }
    TypeIt_const end_ty() const { return Types.end(); }
    IdxIt begin() { return IndicesOfTypes.begin(); }
    IdxIt_const begin() const { return IndicesOfTypes.begin(); }
    IdxIt end() { return IndicesOfTypes.end(); }
    IdxIt_const end() const { return IndicesOfTypes.end(); }

    void print(raw_ostream &Os = llvm::errs()) const;
  };

  // The SMap is a full collection of Structs in Module within the
  // complete information about types and elements which are used in structure.
  // The STypes is a full information about elements used in Struct
  // The Idea is to separate all elements by their baseType.
  // SMap looks like:
  // StructType : (BaseTy: <ElementType, IndexOfElement> <,> ...)
  // C : (i: <i, 1> <[5xB], 3>)
  //     (f: <[5xA], 0> <f, 2>)
  //     (D: <D, 4>)
  // A : (f: <f, 0> <[5xf], 1>)
  // B : (i: <i, 0> <[5xi], 1>)
  // D : (f: <A, 0>)
  //     (i: <B, 0>)
  using STypes = std::unordered_map<Type *, SElementsOfType>;
  using SMap = std::unordered_map<StructType *, STypes>;

  // The typedefs bellow are a full information about struct's transformation.
  // The idea is to generate map between Old struct's elements and new splitted
  // struct's elements Index of the element in InitSTy is index of
  // VecOfNewIndiciesDefinition and Index of the element of SplittedSTy will be
  // put according the InitSTy's element index. VecOfStructInfo looks like:
  // InitSTy : (SplittedSTy, Index) (,) <- at position=0
  //           (,)                      <- at position=1
  // FE. splitting of structs D and C:
  // D : (Df, 0)
  //     (Di, 0)
  // C : (Cf, 0)
  //     (Ci, 0)
  //     (Cf, 1)
  //     (Ci, 1)
  //     (Cf, 2) (Ci, 2)
  // And SMap in that case also contains
  // Ci : (i: <i, 0> <[5xB], 1> <Di, 2)
  // Cf : (f: <[5xA], 0> <f, 1> <Df, 2)
  // Df : (f: <A, 0>)
  // Di : (i: <B, 0>)
  // List of new structs which are on place of old unsplitted struct
  using ListOfSplittedElements = std::list<SElement>;
  // Vector of new structs elements. Position of element is corresponsible with
  // the index of this element in unsplitted structure
  using VecOfNewIndiciesDefinition = std::vector<ListOfSplittedElements>;
  // All collection of new elements
  using InfoAboutSplittedStruct =
      std::pair<StructType *, VecOfNewIndiciesDefinition>;
  // Info about all structs to be splitted.
  // Vector has been chosen to save the chronology of transformation.
  using VecOfStructInfo = std::vector<InfoAboutSplittedStruct>;

private:
  LLVMContext &Ctx;

  SMap AllStructs;
  VecOfStructInfo SplittedStructs;

  // A helped map for fast access to necessary structure transformation.
  std::unordered_map<StructType *, VecOfStructInfo::const_reverse_iterator>
      InfoToMerge;

  // Node represents a aggregative StructType with Nodes(another Structs) on
  // which it depends.
  class Node {
    StructType *STy{nullptr};

    // During the Graph transformation unsplitted stucts will be generated.
    // FE struct C_BS will be generated:
    // C_BS : { [5 x A], i, f, [5 x B], Df, Di }
    // But C_BS is the same C struct in terms of dependencies,
    // so PreviousNames set contains all previouse Node representaions.
    std::unordered_set<StructType *> PreviousNames;
    // FE C has childrens A, B, D
    std::unordered_set<Node *> ChildSTys;
    // FE A has parents D, C
    std::unordered_set<Node *> ParentSTys;

  public:
    Node(StructType &InSTy) : STy{&InSTy} {}

    bool hasParent() const { return !ParentSTys.empty(); }
    bool hasChild() const { return !ChildSTys.empty(); }
    void insertParent(Node &ParentNode);
    void insertChild(Node &ChildNode);
    void eraseChild(Node &ChildNode);
    bool isContainsStruct(StructType &InSTy) const;
    void substitute(StructType &InSTy);

    StructType *getType() const { return STy; }

    using NodeIt = std::unordered_set<Node *>::iterator;
    using NodeIt_const = std::unordered_set<Node *>::const_iterator;
    NodeIt parent_begin() { return ParentSTys.begin(); }
    NodeIt_const parent_begin() const { return ParentSTys.begin(); }
    NodeIt parent_end() { return ParentSTys.end(); }
    NodeIt_const parent_end() const { return ParentSTys.end(); }

    NodeIt child_begin() { return ChildSTys.begin(); }
    NodeIt_const child_begin() const { return ChildSTys.begin(); }
    NodeIt child_end() { return ChildSTys.end(); }
    NodeIt_const child_end() const { return ChildSTys.end(); }

    void dump(int tab, raw_ostream &os = llvm::errs()) const;
  };

  // Class responsible for allocating and releasing memory occupied by Nodes.
  class NodeMemoryManager {
    std::vector<std::unique_ptr<Node>> Nodes;

  public:
    NodeMemoryManager(Module &M);
    Node *create(StructType &STy);
  };
  NodeMemoryManager NodeMM;

  //***************************************
  // Part responsible for graph handling
  //***************************************

  // Heads contains all Nodes that have no parents.
  std::vector<Node *> Heads;
  void generateGraph();

  // Helped type is used to track if Node already placed in Graph
  using NodeTracker = std::unordered_map<StructType *, Node *>;
  Node *createNode(StructType &STy, NodeTracker &Inserted);
  void processNode(Node &SNode);
  void remakeParent(Node &SNode, Node &SNodeToChange,
                    ArrayRef<StructType *> NewReplaceStructs);
  void recreateGraph();

  //***************************************
  // Part responsible for records collection and handling
  //***************************************

  void setInfoAboutStructure(StructType &STy);
  void mergeStructGenerationInfo();
  StructType *
  checkAbilityToMerge(VecOfNewIndiciesDefinition const &NewSTypes) const;

public:
  DependencyGraph(Module &M, StructFilter const &Filter);
  void run();

  //***************************************
  // Part responsible for info accessing
  //***************************************
  bool isPlain(StructType &STy) const;
  bool isStructProcessed(StructType &STy) const;
  STypes const &getStructComponens(StructType &STy) const;
  Type *getPlainSubTy(StructType &STy) const;
  VecOfNewIndiciesDefinition const &
  getVecOfStructIdxMapping(StructType &STy) const;
  ListOfSplittedElements const &getElementsListOfSTyAtIdx(StructType &STy,
                                                          unsigned Idx) const;
  std::unordered_set<StructType *>
  getUniqueSplittedStructs(StructType &STy) const;
  //***************************************
  // Part responsible for dumping
  //***************************************
  void printData(raw_ostream &os = llvm::errs()) const;
  void print(raw_ostream &os = llvm::errs()) const;
  void graphDump(raw_ostream &os = llvm::errs()) const;
  void printGeneration(raw_ostream &os) const;
};

// Class to handle all instructions that are use splitted structs.
class Substituter : public InstVisitor<Substituter> {
  LLVMContext &Ctx;
  DataLayout const &DL;
  StructFilter Filter;
  DependencyGraph Graph;
  std::unordered_map<StructType *, std::vector<AllocaInst *>> Allocas;
  // Contains all instruction to substitute.
  using VecOfInstructionSubstitution =
      std::vector<std::pair<Instruction *, Instruction *>>;

  bool processAllocasOfOneSTy(std::vector<AllocaInst *> const Allocas,
                              VecOfInstructionSubstitution /*OUT*/ &InstToInst);

  std::unordered_map<Type *, Instruction *>
  generateNewAllocas(AllocaInst &OldInst) const;
  GetElementPtrInst *
  generateNewGEPs(GetElementPtrInst &GEPI, Type &DestSTy,
                  DependencyGraph::SElementsOfType LocalIdxPath /*copy*/,
                  std::unordered_map<Type *, Instruction *> const &NewInstr,
                  unsigned PlainTyIdx, unsigned Size) const;

  static Optional<
      std::tuple<DependencyGraph::SElementsOfType, std::vector<Type *>>>
  getIndicesPath(GetElementPtrInst &GEPI);
  static Optional<
      std::tuple<std::vector<GetElementPtrInst *>, std::vector<PtrToIntInst *>>>
  getInstUses(Instruction &I);
  static Optional<uint64_t> processAddInst(Instruction &I, BinaryOperator &BO);

  bool processGEP(GetElementPtrInst &GEPI,
                  std::unordered_map<Type *, Instruction *> const &NewInstr,
                  VecOfInstructionSubstitution /*OUT*/ &InstToInst);
  bool processPTI(PtrToIntInst &PTI,
                  std::unordered_map<Type *, Instruction *> const &NewInstr,
                  VecOfInstructionSubstitution /*OUT*/ &InstToInst);
  static bool processPTIsUses(Instruction &I, uint64_t /*OUT*/ &MaxPtrOffset);

public:
  Substituter(Module &M);

  void visitAllocaInst(AllocaInst &AI);
  bool processAllocas();

  void printAllAllocas(raw_ostream &Os = llvm::errs());
};

bool GenXStructSplitter::runOnModule(Module &M) {
  const auto &BC = getAnalysis<GenXBackendConfig>();
  if (PerformStructSplitting && BC.doStructSplitting())
    return Substituter{M}.processAllocas();

  return false;
}

//__________________________________________________________________
//          Block of StructFilter definition
//__________________________________________________________________

//
//  Performs checking of module for banned structs.
//
StructFilter::StructFilter(Module &M) {
  std::list<StructType *> NotBannedYet;
  // Looks for an element as an array.
  for (auto &&STy : M.getIdentifiedStructTypes())
    if (checkForArrayOfComplicatedStructs(*STy))
      NotBannedYet.push_front(STy);
    else
      BannedStructs.emplace(STy);

  // Looks for an element as banned struct.
  for (auto It = NotBannedYet.begin(); It != NotBannedYet.end(); /*none*/)
    if (!checkForElementOfBannedStruct(**It)) {
      BannedStructs.emplace(*It);
      NotBannedYet.erase(It);
      It = NotBannedYet.begin();
    } else
      ++It;

  // Looks for an allocation an array.
  visit(M);
}

//
//  Returns true if STy is banned and false - if not.
//
bool StructFilter::isStructBanned(StructType &STy) const {
  return BannedStructs.find(&STy) != BannedStructs.end();
}

//
//  Visits all allocas and checks if it allocates an array of structure.
//
void StructFilter::visitAllocaInst(AllocaInst &AI) {
  Type *AllocaTy = AI.getAllocatedType();
  Type *AllocaBTy = getArrayFreeTy(AllocaTy);
  bool const IsSeq = AllocaBTy != AllocaTy;
  if (StructType *STy = dyn_cast<StructType>(AllocaBTy))
    if (IsSeq) {
      // If allocating an array of structs -> ban splitting
      BannedStructs.emplace(STy);
      return;
    }
}

//
//  Checks if structure has array of complex type.
//  Returns true if has not got.
//
bool StructFilter::checkForArrayOfComplicatedStructs(StructType &STy) const {
  auto IsSequential = [](Type &Ty) {
    return Ty.isVectorTy() || Ty.isArrayTy();
  };

  return !std::any_of(
      STy.elements().begin(), STy.elements().end(), [IsSequential](Type *Elm) {
        Type *BaseTy = getArrayFreeTy(Elm);
        if (StructType *SBTy = dyn_cast<StructType>(BaseTy))
          return IsSequential(*Elm) && BaseTy == getBaseTy(SBTy);
        return false;
      });
}

//
//  Checks if structure has element of banned struct.
//  Returns true if has not got.
//
bool StructFilter::checkForElementOfBannedStruct(StructType &STy) const {
  return !std::any_of(STy.elements().begin(), STy.elements().end(),
                      [this](Type *Elm) {
                        Type *BaseTy = getArrayFreeTy(Elm);
                        if (StructType *SBTy = dyn_cast<StructType>(BaseTy))
                          return isStructBanned(*SBTy);
                        return false;
                      });
}

//__________________________________________________________________
//          Block of DependencyGraph definition
//__________________________________________________________________

//
//  Tries to get a base type of structure if structure is plain.
//  If STy is not plain then tries to use getBaseTy().
//
Type *DependencyGraph::getPlainSubTy(StructType &STy) const {
  return isPlain(STy) ? AllStructs.find(&STy)->second.begin()->first
                      : getBaseTy(&STy);
}

//
//  * Determines if structure STy is plain:
//     contains in AllStruct
//     and there is only one baseTy
//  * Works unobvious for structs like: C1: { C2 }
//
bool DependencyGraph::isPlain(StructType &STy) const {
  auto FindIt = AllStructs.find(&STy);
  return FindIt != AllStructs.end() && FindIt->second.size() < 2;
}

//
//  Checks if Struct has been processed, so info about it exists in InfoToMerge.
//  Returns true if record about struct exists, false - conversely.
//
bool DependencyGraph::isStructProcessed(StructType &STy) const {
  return InfoToMerge.find(&STy) != InfoToMerge.end();
}

//
//  Gets the element's information of the struct.
//  Assertion if struct's info has not been collected.
//
DependencyGraph::STypes const &
DependencyGraph::getStructComponens(StructType &STy) const {
  auto FindIt = AllStructs.find(&STy);
  IGC_ASSERT_MESSAGE(
      FindIt != AllStructs.end(),
      "Info about struct has to be collected before getting components.\n");
  return FindIt->second;
}

//
//  Gets vector of elements substitution of old struct with new substructs'
//  elements.
//
DependencyGraph::VecOfNewIndiciesDefinition const &
DependencyGraph::getVecOfStructIdxMapping(StructType &STy) const {
  auto FindIt = InfoToMerge.find(&STy);
  IGC_ASSERT_MESSAGE(
      FindIt != InfoToMerge.end(),
      "Struct has to be processed before getting indices mapping.\n");
  return FindIt->second->second;
}

//
//  Gets element's list which substitutes splitted struct's(STy) element at
//  index(Idx).
//
DependencyGraph::ListOfSplittedElements const &
DependencyGraph::getElementsListOfSTyAtIdx(StructType &STy,
                                           unsigned Idx) const {
  VecOfNewIndiciesDefinition const &VecOfSTy = getVecOfStructIdxMapping(STy);
  IGC_ASSERT_MESSAGE(Idx < VecOfSTy.size(),
                     "Attempt to get element out of borders.");
  return VecOfSTy.at(Idx);
}

//
// Gets unique structures into which the structure STy is split.
//
std::unordered_set<StructType *>
DependencyGraph::getUniqueSplittedStructs(StructType &STy) const {
  std::unordered_set<StructType *> UniqueSplittedStructs;
  // Gets unique substructs.
  for (auto &&ListOfBaseTys : getVecOfStructIdxMapping(STy))
    for (auto &&BaseTy : ListOfBaseTys)
      UniqueSplittedStructs.emplace(BaseTy.Ty);
  return UniqueSplittedStructs;
}

//
//  * By AllStructs info generates dependency graph of structs.
//  * FE generates smth like this:
//    C -----> A
//     \     /
//      \-> D
//       \   \
//        \-> B
//
void DependencyGraph::generateGraph() {
  LLVM_DEBUG(dbgs() << "Graph generating begin.\n");
  NodeTracker Inserted;
  Heads.reserve(AllStructs.size());
  for (auto &&[STy, ChildrensBaseTys] : AllStructs) {
    if (Inserted.find(STy) != Inserted.end())
      // If already in graph -> skip
      continue;
    Heads.push_back(createNode(*STy, Inserted));
  }

  // During Graph creation where can be similar case: (C and D are heads)
  // C -> D ..
  // D -> A
  // So we want to remove D as it has parent=C
  // Cleanup Heads. Erase all entities with parent
  Heads.erase(
      std::remove_if(Heads.begin(), Heads.end(),
                     [](Node *HeadNode) { return HeadNode->hasParent(); }),
      Heads.end());
}

//
//  Creates the Node and places dependencies according to the Struct.
//
DependencyGraph::Node *DependencyGraph::createNode(StructType &STy,
                                                   NodeTracker &Inserted) {
  LLVM_DEBUG(dbgs() << "Creating node for struct: " << STy.getName() << "\n");

  {
    auto FindIt = Inserted.find(&STy);
    if (FindIt != Inserted.end()) {
      // This can occure when Struct has an processed child element.
      // Parent will be automatically set right after this function.
      // Later clean-up heads. This node will be erased as it has parents.
      Node *node = FindIt->second;
      return node;
    }
  }
  Node *ThisNode = NodeMM.create(STy);
  auto [It, IsInserted] = Inserted.emplace(&STy, ThisNode);

  if (!IsInserted) {
    vc::diagnose(Ctx, "StructSplitter",
                 "Processing Node which already has been processed. Struct: " +
                     ThisNode->getType()->getName(),
                 DS_Warning);
  }

  for (auto &&[BaseTy, Children] : getStructComponens(STy))
    std::for_each(Children.begin_ty(), Children.end_ty(), [&](Type *Child) {
      if (StructType *ChildSTy = dyn_cast<StructType>(getArrayFreeTy(Child))) {
        Node *ChildNode = createNode(*ChildSTy, Inserted);
        ChildNode->insertParent(*ThisNode);
        ThisNode->insertChild(*ChildNode);
      }
    });
  return ThisNode;
}

//
//  * Processes the bottom node. Assume that this node has no children
//    so all elements in this struct are plain.
//  * Splits this struct into plain substructs and recreates parents node.
//  * Eventually deletes this node from graph and releases memory as
//    after processing struct will be splitted to plain substructs
//    and parents node will no longer needed to track it.
//  * While processing nodes graph will self destruct.
//  * Info about all structs in Module (AllStructs) will be updated.
//  * Info about structs transformation (SplittedStructs) will be updated.
//
void DependencyGraph::processNode(Node &SNode) {
  // Go to the bottom of the graph.
  while (SNode.hasChild())
    processNode(**SNode.child_begin());

  LLVM_DEBUG(dbgs() << "Processing node for struct: "
                    << SNode.getType()->getName() << "\n");
  // Splitting always gets a plain type, so graph will be changed any way
  if (StructType *OldSTy = SNode.getType(); !isPlain(*OldSTy)) {
    // Splitting
    STypes const &Types = getStructComponens(*OldSTy);
    // Indices of unsplitted struct will be matched with indices of elemetnts of
    // new splitted structs.
    VecOfNewIndiciesDefinition IndicesMap(OldSTy->getNumElements());

    std::vector<StructType *> GeneratedStructs;
    GeneratedStructs.reserve(Types.size());

    StringRef OldSTyName = OldSTy->getName();

    for (auto &&[BaseTy, Elements] : Types) {
      StructType *NewPlainStruct = StructType::create(
          Ctx, Elements.getTypesArray(),
          Twine(OldSTyName + "." + getTypePrefix(*BaseTy) + ".splitted").str());
      GeneratedStructs.push_back(NewPlainStruct);

      // Match old elements with new elements.
      for (auto ElmIndex : enumerate(Elements))
        IndicesMap[ElmIndex.value()].emplace_front(NewPlainStruct,
                                                   ElmIndex.index());

      // Update AllStructs
      setInfoAboutStructure(*NewPlainStruct);
    }

    // Update SplittedStructs
    SplittedStructs.emplace_back(std::make_pair(OldSTy, std::move(IndicesMap)));

    // Remake parent Node.
    // As D will be splitted to Di,Df so C(parent) has to be splitted to Ci,Cf.
    // It will be done in 3 steps:
    // 1st: Create new struct before splitting: C_BS : {Ci, Cf}
    // 2nd: Substitute struct C in Node represented this struct to C_BS
    // 3rd: When D processing will be changed, C(C_BS) will be automatically
    //      splitted to Ci,Cf as Node responsible for C(C_BS) will no longer
    //      have childrens
    // In this case there will be a record in transformation info:
    //  D     -> Di, Df
    //  C     -> C_BS
    //  C_BS  -> Ci, Cf
    // so transformation C->C_BS->Ci,Cf can be merged to C->Ci,Cf
    std::for_each(SNode.parent_begin(), SNode.parent_end(),
                  [&](Node *ParentNode) {
                    remakeParent(*ParentNode, SNode, GeneratedStructs);
                  });
  }

  // Remove dependencies.
  std::for_each(SNode.parent_begin(), SNode.parent_end(),
                [&SNode](Node *ParentNode) { ParentNode->eraseChild(SNode); });
}

//
//  * Creates unsplitted struct with new element's types generated from child
//    Node.
//  * As D splitted to Di,Df, structure C has to change element D to Di,Df and
//    splits later. So after this function in Node responsible for C, will
//    be placed new structure C_BS and later C_BS will be processed.
//  * SNode - current parent node to be changed
//  * SNodeToChange - child node that already has been changed
//
void DependencyGraph::remakeParent(Node &SNode, Node &SNodeToChange,
                                   ArrayRef<StructType *> NewReplaceStructs) {
  LLVM_DEBUG(dbgs() << "Recreating parent node: " << SNode.getType()->getName()
                    << "\n\tChild node: " << SNodeToChange.getType()->getName()
                    << "\n");
  StructType *CurrentS = SNode.getType();
  StringRef CurrentSName = CurrentS->getName();
  unsigned const NumElements = CurrentS->getNumElements();
  unsigned const NewMaxSize = NumElements + NewReplaceStructs.size() - 1;
  std::vector<Type *> NewElements;
  NewElements.reserve(NewMaxSize);
  // First create an empty struct
  // Later setBody with elements. It is for completing VecOfStructInfo
  StructType *BeforeSplitingS = StructType::create(
      CurrentS->getContext(), Twine(CurrentSName + "_BS").str());
  VecOfNewIndiciesDefinition NewIndices(NumElements);
  unsigned Index{0};
  unsigned ExpandIndicies{0};
  for (auto &&Elm : CurrentS->elements()) {
    if (StructType *SElm = dyn_cast<StructType>(Elm);
        SElm && SNodeToChange.isContainsStruct(*SElm)) {
      // If element of structure is splitted element, then we need to replace
      // this element with new.
      for (auto &&NewSTy : NewReplaceStructs) {
        NewElements.emplace_back(NewSTy);
        NewIndices[Index].emplace_front(BeforeSplitingS,
                                        Index + ExpandIndicies++);
      }
      // The Index will be inc, so there is no need of extra offset
      --ExpandIndicies;
    } else {
      // If element of structure is not changed, then just copies info about it
      // and places right indices.
      NewElements.emplace_back(Elm);
      NewIndices[Index].emplace_front(BeforeSplitingS, Index + ExpandIndicies);
    }
    ++Index;
  }

  BeforeSplitingS->setBody(NewElements);

  // Updates AllStructs and SplittedStructs info.
  setInfoAboutStructure(*BeforeSplitingS);
  SplittedStructs.emplace_back(std::make_pair(CurrentS, std::move(NewIndices)));

  // Substitutes structure in Node
  SNode.substitute(*BeforeSplitingS);
}

//
//  For each Node in head launches Graph processing.
//  After processing as node is deleted we remove it from Heads.
//
void DependencyGraph::recreateGraph() {
  LLVM_DEBUG(dbgs() << "Graph recreating begin.\n");
  for (auto *Node : Heads)
    processNode(*Node);
}

//
//  Records information about structure into AllStructs.
//
void DependencyGraph::setInfoAboutStructure(StructType &STy) {
  LLVM_DEBUG(dbgs() << "Collecting infornation about struct: " << STy.getName()
                    << "\n");
  STypes BaseTypes;
  unsigned Index{0};
  unsigned const NumElements = STy.getNumElements();
  // SElementsOfType reservs memory to avoid reallocations and easy access
  // Will be more memory overhead
  for (auto &&Elm : STy.elements()) {
    Type *BaseTy = getBaseTy(Elm);
    // BaseTy can be structure in AllStructs, so we get info from AllStructs
    if (StructType *SBTy = dyn_cast<StructType>(BaseTy))
      BaseTy = getPlainSubTy(*SBTy);

    auto FindIt = BaseTypes.find(BaseTy);
    if (FindIt == BaseTypes.end()) {
      // If there is no entity with baseTy, creates it with preallocated array
      auto [It, IsInserted] =
          BaseTypes.emplace(BaseTy, SElementsOfType{NumElements});
      It->second.emplaceBack(*Elm, Index++);
    } else
      // If there is an entity with baseTy, inserts it into array
      FindIt->second.emplaceBack(*Elm, Index++);
  }

  auto [It, IsInserted] = AllStructs.emplace(&STy, std::move(BaseTypes));
  if (!IsInserted) {
    vc::diagnose(
        Ctx, "StructSplitter",
        "Processing Struct which already has been processed. Struct: " +
            STy.getName(),
        DS_Warning);
  }
}

//
//  * As BeforeSplitting struct is temporary it can be removed from
//    transformation info.
//  * Also only here the InfoToMerge is filling.
//
void DependencyGraph::mergeStructGenerationInfo() {
  LLVM_DEBUG(dbgs() << "Merging structs.\n");
  for (auto It = SplittedStructs.rbegin(), End = SplittedStructs.rend();
       It != End; ++It) {
    if (StructType *SToMerge = checkAbilityToMerge(It->second)) {
      LLVM_DEBUG(dbgs() << "Able to merge: " << *It->first << "\n\tWith "
                        << *SToMerge << "\n");

      VecOfNewIndiciesDefinition const &InfoAboutTemporaryS =
          getVecOfStructIdxMapping(*SToMerge);

      for (ListOfSplittedElements &ElementsList : It->second) {
        for (SElement &Element : ElementsList) {
          IGC_ASSERT_MESSAGE(Element.Index < InfoAboutTemporaryS.size(),
                             "Attempt to get element out of borders.");
          ListOfSplittedElements const &NewElement =
              InfoAboutTemporaryS.at(Element.Index);

          ListOfSplittedElements::const_iterator EIt = NewElement.begin();
          // Changes current element and if on this 'Element.Index' lots of new
          // elements are to be placed, then extend list from begining not to
          // invalidate iterations.
          Element = *EIt;
          while (++EIt != NewElement.end())
            ElementsList.push_front(*EIt);
        }
      }
    }

    InfoToMerge.emplace(It->first, It);
  }
}

//
//  * We are able to merge two struct's records only if new elements of struct
//    are the same.
//  * C : (C_BS, 0)
//        (C_BS, 1)
//        (C_BS, 2)
//        (C_BS, 3)
//        (C_BS, 4) (C_BS, 5)
//  * C_BS :  (Cf, 0)
//            (Ci, 0)
//            (Cf, 1)
//            (Ci, 1)
//            (Cf, 2)
//            (Ci, 2)
//
StructType *DependencyGraph::checkAbilityToMerge(
    VecOfNewIndiciesDefinition const &NewSTypes) const {
  StructType *STyToCheck{nullptr};
  for (auto &&SplittedElements : NewSTypes)
    for (auto &&Element : SplittedElements) {
      if (!STyToCheck)
        STyToCheck = Element.Ty;
      else if (STyToCheck != Element.Ty)
        return nullptr;
    }

  // If somehow there is no struct to merge, then do not merge.
  // Not obviouse if it can occure.
  if (!STyToCheck) {
    vc::diagnose(Ctx, "StructSplitter", "Merging with empty structs.",
                 DS_Warning);
  }
  return isStructProcessed(*STyToCheck) ? STyToCheck : nullptr;
}

//
//  Constructor gets all initial information about structures in Module.
//
DependencyGraph::DependencyGraph(Module &M, StructFilter const &Filter)
    : Ctx{M.getContext()}, NodeMM{M} {
  for (auto &&STy : M.getIdentifiedStructTypes())
    if (!Filter.isStructBanned(*STy))
      setInfoAboutStructure(*STy);
}

//
//  Launches structure dependencies processing.
//
void DependencyGraph::run() {
  generateGraph();
  recreateGraph();
  mergeStructGenerationInfo();
}

//__________________________________________________________________
//          Block of Substituter definition
//__________________________________________________________________

//
//  Collects all information of structs, allocas and launches struct splittting,
//    based on this information.
//
Substituter::Substituter(Module &M)
    : Ctx(M.getContext()), DL(M.getDataLayout()), Filter(M), Graph(M, Filter) {
  Graph.run();

  // visit should be after graph processing
  visit(M);
}

//
//  Collects all allocas that allocate memory for structure to split.
//
void Substituter::visitAllocaInst(AllocaInst &AI) {
  if (StructType *STy = dyn_cast<StructType>(AI.getAllocatedType()))
    if (Graph.isStructProcessed(*STy)) {
      LLVM_DEBUG(dbgs() << "Collecting alloca to replace: " << AI.getName()
                        << "\n");
      // In case if struct S marked to be splitted, but there is
      // alloca [5 x %struct.C] then skip.
      // Gets only allocas which will be splitted
      // InfoToMerge contains this info
      Allocas[STy].emplace_back(&AI);
    }
}

//
//  By VecOfIndices into which substructures to split the structure.
//  Returns Instruction set within substructures for easy access.
//
std::unordered_map<Type *, Instruction *>
Substituter::generateNewAllocas(AllocaInst &OldInst) const {
  LLVM_DEBUG(dbgs() << "Generating allocas to replace: " << OldInst.getName()
                    << "\n");

  StructType *STy = dyn_cast<StructType>(OldInst.getAllocatedType());
  IGC_ASSERT_MESSAGE(STy, "Alloca to replace produces non-struct type.");

  std::unordered_set<StructType *> UniqueSplittedStructs =
      Graph.getUniqueSplittedStructs(*STy);

  std::unordered_map<Type *, Instruction *> NewInstructions;
  NewInstructions.reserve(UniqueSplittedStructs.size());

  IRBuilder<> IRB(&OldInst);
  for (StructType *NewSTy : UniqueSplittedStructs) {
    AllocaInst *NewAlloca = IRB.CreateAlloca(
        NewSTy, 0, OldInst.getName() + "." + NewSTy->getName());
    NewAlloca->setAlignment(IGCLLVM::getAlign(OldInst));
    auto [It, IsInserted] = NewInstructions.emplace(NewSTy, NewAlloca);
    if (!IsInserted) {
      vc::diagnose(Ctx, "StructSplitter",
                   "Alloca instruction responsible for structure has already "
                   "been created.\n\tVariable name: " +
                       It->second->getName(),
                   DS_Warning);
    }
  }
  return NewInstructions;
}

//
//  Creating new GEPI instruction.
//  GEPI - instruction to replace.
//  PlainType - the result type of new gep working
//  LocalIdxPath - the sequence of indices to recive needed type
//  NewInstr - instruction map to set proper uses
//  PlainTyIdx - index of the first plain type
//  size - count of the indices of gep
//
GetElementPtrInst *Substituter::generateNewGEPs(
    GetElementPtrInst &GEPI, Type &PlainType,
    DependencyGraph::SElementsOfType LocalIdxPath /*copy*/,
    std::unordered_map<Type *, Instruction *> const &NewInstr,
    unsigned PlainTyIdx, unsigned Size) const {
  LLVM_DEBUG(dbgs() << "Generating GEP to replace: " << GEPI.getName() << "\n");

  for (unsigned i = 0; i != PlainTyIdx; ++i) {
    auto &&[Ty, Idx] = LocalIdxPath.at(i);
    StructType *STy = dyn_cast<StructType>(Ty);
    DependencyGraph::ListOfSplittedElements const &ListOfPossibleTypes =
        Graph.getElementsListOfSTyAtIdx(*STy, Idx);
    // Struct C is splitted to Ci and Cf, so we have to choose
    // are we be indexed via Ci or Cf.
    // %di = gep Ci, 0, 2
    // %df = gep Cf, 0, 2
    for (auto &&PossibleElement : ListOfPossibleTypes) {
      // Kind of getting SubType
      // We choose right "branch" by PlainType.
      if (&PlainType == Graph.getPlainSubTy(*PossibleElement.Ty)) {
        Ty = PossibleElement.Ty;
        Idx = PossibleElement.Index;
        break;
      }
    }
  }

  // Generates new IdxList for instruction.
  std::vector<Value *> IdxList;
  IdxList.reserve(Size + 1);
  IdxList.emplace_back(*GEPI.idx_begin());
  for (unsigned i = 0; i != Size; ++i)
    // TODO how to chose i32 or i64 for indices value?
    IdxList.emplace_back(
        ConstantInt::get(Ctx, APInt(32, LocalIdxPath.getIdxAt(i))));

  // Find proper instruction generated before.
  // The necessary splitted struct placed in the first position.
  Type *Inserted = LocalIdxPath.getTyAt(0);
  auto FindInstrIt = NewInstr.find(Inserted);
  IGC_ASSERT_MESSAGE(
      FindInstrIt != NewInstr.end(),
      "Cannot find instruction according to splitted structure type.");
  Instruction *ToInsert = FindInstrIt->second;

  IRBuilder<> IRB(&GEPI);
  GetElementPtrInst *NewGEP = cast<GetElementPtrInst>(
      IRB.CreateGEP(Inserted, ToInsert, IdxList, GEPI.getName() + ".splitted"));

  return NewGEP;
}
//
//  An entry point of replacement instructions.
//  First replaces allocas, then replaces GEP and so one.
//
bool Substituter::processAllocas() {
  bool Changed{false};
  for (auto &&[STy, VecOfAllocas] : Allocas) {
    VecOfInstructionSubstitution InstToInst;
    if (processAllocasOfOneSTy(VecOfAllocas, InstToInst)) {
      Changed = true;
      for (auto [InstToReplace, ToInst] : InstToInst)
        InstToReplace->replaceAllUsesWith(ToInst);
    }
  }
  return Changed;
}
//
//  Processes allocas which allocates memory for certain structure type.
//  Returns true, if all usage of structure is appropriate and outs vector of
//  substutited intructions. Returns false, if some instruction is prohibited
//  to split.
//
bool Substituter::processAllocasOfOneSTy(
    std::vector<AllocaInst *> const Allocas,
    VecOfInstructionSubstitution /*OUT*/ &InstToInst) {
  for (AllocaInst *Alloca : Allocas) {
    LLVM_DEBUG(dbgs() << "Processing alloca: " << Alloca->getName() << "\n");
    auto InstUses = getInstUses(*Alloca);
    if (!InstUses)
      return false;
    auto [UsesGEP, UsesPTI] = std::move(InstUses.getValue());

    std::unordered_map<Type *, Instruction *> NewInstructions =
        generateNewAllocas(*Alloca);

    for (GetElementPtrInst *GEP : UsesGEP)
      if (!processGEP(*GEP, NewInstructions, InstToInst))
        return false;
    for (PtrToIntInst *PTI : UsesPTI)
      if (!processPTI(*PTI, NewInstructions, InstToInst))
        return false;
  }
  return true;
}

//
// Retrieves information of Type gotten within each index access.
// FE:
//  %a = gep C, 0, 4, 0
//  (C, 4) -> D
//  (D, 0) -> A
//
Optional<std::tuple<DependencyGraph::SElementsOfType, std::vector<Type *>>>
Substituter::getIndicesPath(GetElementPtrInst &GEPI) {
  unsigned const Size = GEPI.getNumIndices() - 1;
  DependencyGraph::SElementsOfType IdxPath(Size);
  std::vector<Type *> GottenTypeArr;
  GottenTypeArr.reserve(Size);

  // Skip first operator as it always 0 to rename poiterTy and get to structTy
  Type *CurrentType = GEPI.getSourceElementType();
  for (auto It = GEPI.idx_begin() + 1, End = GEPI.idx_end(); It != End; ++It) {
    Value *VIdx = *It;
    if (Constant *CIdx = dyn_cast<Constant>(VIdx)) {
      APInt const &Int = CIdx->getUniqueInteger();
      // Naive assumption that all indices are unsigned greater then zero and
      // scalar
      uint64_t Idx = Int.getZExtValue();

      // This approach can fail in case of dynamic indices.
      // To use table in that case.
      Type *GottenType{nullptr};
      if (CurrentType->isVectorTy() || CurrentType->isArrayTy())
        GottenType = CurrentType->getContainedType(0);
      else
        GottenType = CurrentType->getContainedType(Idx);

      IdxPath.emplaceBack(*CurrentType, Idx);
      GottenTypeArr.emplace_back(GottenType);
      CurrentType = GottenType;
    } else {
      LLVM_DEBUG(dbgs() << "WARN:: Non constant indices do not supported!\n");
      return None;
    }
  }
  return std::make_tuple(std::move(IdxPath), std::move(GottenTypeArr));
}

//
// Gets GEP and PTI users of instruction I.
//
Optional<
    std::tuple<std::vector<GetElementPtrInst *>, std::vector<PtrToIntInst *>>>
Substituter::getInstUses(Instruction &I) {
  // Checks That users of GEP are apropreate.
  std::vector<GetElementPtrInst *> UsesGEP;
  std::vector<PtrToIntInst *> UsesPTI;
  UsesGEP.reserve(I.getNumUses());
  UsesPTI.reserve(I.getNumUses());
  for (auto const &U : I.uses())
    if (GetElementPtrInst *I = dyn_cast<GetElementPtrInst>(U.getUser()))
      UsesGEP.push_back(I);
    else if (PtrToIntInst *PTI = dyn_cast<PtrToIntInst>(U.getUser()))
      UsesPTI.push_back(PTI);
    else {
      LLVM_DEBUG(
          dbgs()
          << "WARN:: Struct uses where it cannot be used!\n\tInstruction: "
          << *U.getUser() << "\n");
      return None;
    }
  return std::make_tuple(std::move(UsesGEP), std::move(UsesPTI));
}

//
//  * Generates new instructions that use splitted struct.
//  * In case if result of old instruction is a struct to be splitted, then
//    generates new instructions and results of them are splitted structs.
//  * In case if result of old instruction is unsplitted struct or data,
//    when just generate new instruction with proper access indecies.
//  * FE1: %d = gep C, 0, 4
//     -> %di = gep Ci, 0, 2
//     -> %df = gep Cf, 0, 1
//  * FE2: %a = gep C, 0, 4, 0
//     -> %a = gep Cf, 0, 2, 0
//
bool Substituter::processGEP(
    GetElementPtrInst &GEPI,
    std::unordered_map<Type *, Instruction *> const &NewInstr,
    VecOfInstructionSubstitution /*OUT*/ &InstToInst) {
  LLVM_DEBUG(dbgs() << "Processing uses of instruction: " << GEPI.getName()
                    << "\n");
  auto IndicesPath = getIndicesPath(GEPI);
  if (!IndicesPath)
    return false;
  auto [IdxPath, GottenTypeArr] = std::move(IndicesPath.getValue());
  unsigned const Size = GottenTypeArr.size();
  Type *CurrentType = GottenTypeArr.back();

  // Find the first index of plain type.
  // All indices after PlaintTyIdx can be just copied.
  auto FindIt = std::find_if(GottenTypeArr.begin(), GottenTypeArr.end(),
                             [this](Type *Ty) {
                               StructType *STy = dyn_cast<StructType>(Ty);
                               return !STy || !Graph.isStructProcessed(*STy);
                             });
  unsigned PlainTyIdx = FindIt - GottenTypeArr.begin();

  if (PlainTyIdx == Size) {
    // Case of FE1
    auto InstUses = getInstUses(GEPI);
    if (!InstUses)
      return false;
    auto [UsesGEP, UsesPTI] = std::move(InstUses.getValue());

    // That means that we getting splitted struct so we need to create GEPs.
    // STyToBeSplitted is the result of instruction.
    StructType *STyToBeSplitted = dyn_cast<StructType>(CurrentType);
    std::unordered_set<StructType *> UniqueSplittedStructs =
        Graph.getUniqueSplittedStructs(*STyToBeSplitted);

    std::unordered_map<Type *, Instruction *> NewInstructions;
    NewInstructions.reserve(UniqueSplittedStructs.size());

    // For each substruct we have to generate it's own IdxPath and GEP
    for (StructType *DestSTy : UniqueSplittedStructs) {
      Type *PlainType = Graph.getPlainSubTy(*DestSTy);
      GetElementPtrInst *NewGEP = generateNewGEPs(GEPI, *PlainType, IdxPath,
                                                  NewInstr, PlainTyIdx, Size);
      NewInstructions.emplace(DestSTy, NewGEP);
    }

    // Runs user processing on GEP and PTI users.
    // All uses has to be changed.
    for (GetElementPtrInst *GEP : UsesGEP)
      if (!processGEP(*GEP, NewInstructions, InstToInst))
        return false;
    for (PtrToIntInst *PTI : UsesPTI)
      if (!processPTI(*PTI, NewInstructions, InstToInst))
        return false;
  } else {
    Type *PlainType = getBaseTy(GottenTypeArr[PlainTyIdx]);
    GetElementPtrInst *NewGEP = generateNewGEPs(GEPI, *PlainType, IdxPath,
                                                NewInstr, PlainTyIdx + 1, Size);
    LLVM_DEBUG(dbgs() << "New Instruction has been created: " << *NewGEP
                      << "\n");
    InstToInst.emplace_back(cast<Instruction>(&GEPI),
                            cast<Instruction>(NewGEP));
  }
  return true;
}

//
//  Checks if accessing by ptr covers one unsplitted block and substitutes
//  struct. Tracks max offset of ptr until ptr goes to function. If function is
//  read/write, then check if max offset lies within unsplited block. If it
//  does, then substitutes struct. Overwise we cannot split struct.
//
bool Substituter::processPTI(
    PtrToIntInst &PTI,
    std::unordered_map<Type *, Instruction *> const &NewInstr,
    VecOfInstructionSubstitution /*OUT*/ &InstToInst) {

  StructType *STy = dyn_cast<StructType>(
      PTI.getPointerOperand()->getType()->getPointerElementType());
  IGC_ASSERT_MESSAGE(STy, "Operand of PTI has to be StructType.");

  uint64_t MaxPtrOffset{0};
  if (!processPTIsUses(PTI, MaxPtrOffset))
    return false;

  // If MaxPtrOffset covers elements, which will be laid sequitially within one
  // new struct, then we can substiture PTI with new PTI;
  unsigned IdxOfOldElm{0};
  StructType *SplittedSTy{nullptr};
  unsigned IdxOfSplittedStructElm{0};
  DependencyGraph::VecOfNewIndiciesDefinition const &IdxMapping =
      Graph.getVecOfStructIdxMapping(*STy);
  for (auto &&Elm : STy->elements()) {
    IGC_ASSERT_MESSAGE(IdxOfOldElm < IdxMapping.size(),
                       "Attempt to get element out of borders.");
    DependencyGraph::ListOfSplittedElements const &ListOfElements =
        IdxMapping.at(IdxOfOldElm++);
    for (auto &&NewElm : ListOfElements) {
      if (!SplittedSTy) {
        // The head of sequential check
        SplittedSTy = NewElm.Ty;
        IdxOfSplittedStructElm = NewElm.Index;
        if (IdxOfSplittedStructElm) {
          // A {i32, float}
          // Af {float} <- prohibited
          // Ai {i32}   <- allowed
          LLVM_DEBUG(dbgs() << "WARN:: Struct (" << *STy
                            << ") cannot be splitted as the first element of "
                               "the splitted struct has to be the first "
                               "element of the original struct!\n");
          return false;
        }
      } else {
        if (NewElm.Ty != SplittedSTy) {
          // A {i32, i32, float}; Offset = 8byte
          // Prohibited as offset covers i32 and float
          LLVM_DEBUG(dbgs() << "WARN:: Struct (" << *STy
                            << ") cannot be splitted as pointer offset covers "
                               "different splitted types.\n");
          return false;
        } else if (NewElm.Index != ++IdxOfSplittedStructElm) {
          LLVM_DEBUG(dbgs() << "WARN:: Struct (" << *STy
                            << ") cannot be splitted as pointer offset covers "
                               "unsequential types.\n");
          return false;
        }
      }
    }
    if (!MaxPtrOffset)
      break;
    uint64_t const SizeOfElm = DL.getTypeAllocSizeInBits(Elm) / genx::ByteBits;
    MaxPtrOffset = SizeOfElm > MaxPtrOffset ? 0 : MaxPtrOffset - SizeOfElm;
  }

  auto FindInstrIt = NewInstr.find(SplittedSTy);
  IGC_ASSERT_MESSAGE(
      FindInstrIt != NewInstr.end(),
      "Cannot find instruction according to splitted structure type.");
  Instruction *ToInsert = FindInstrIt->second;

  IRBuilder<> IRB(&PTI);
  Value *NewPTI =
      IRB.CreatePtrToInt(ToInsert, PTI.getType(), PTI.getName() + ".splitted");

  LLVM_DEBUG(dbgs() << "New Instruction has been created: " << *NewPTI << "\n");
  InstToInst.emplace_back(cast<Instruction>(&PTI), cast<Instruction>(NewPTI));
  return true;
}

//
// Callculates offset after add instruction.
//
Optional<uint64_t> Substituter::processAddInst(Instruction &User,
                                               BinaryOperator &BO) {
  // Do Ptr Offset calculation.
  uint64_t LocalPtrOffset{0};
  Value *V0 = BO.getOperand(0);
  // If the one of operands is the Instruction then the other is ptr offset.
  // It can be vector or scalar.
  // "add V 5" or "add 5 V"
  Value *ToCalculateOffset =
      dyn_cast<Instruction>(V0) != &User ? V0 : BO.getOperand(1);
  Constant *ConstantOffsets = dyn_cast<Constant>(ToCalculateOffset);
  if (!ConstantOffsets) {
    LLVM_DEBUG(dbgs() << "WARN:: Calculation of the pointer offset has to "
                         "be staticly known\n. Bad instruction: "
                      << BO << "\n");
    return None;
  }
  Type *OffsetTy = ToCalculateOffset->getType();
  if (OffsetTy->isVectorTy()) {
    unsigned const Width =
        cast<IGCLLVM::FixedVectorType>(OffsetTy)->getNumElements();
    for (unsigned i = 0; i != Width; ++i) {
      Value *OffsetValue = ConstantOffsets->getAggregateElement(i);
      Constant *COffsetValue = cast<Constant>(OffsetValue);
      uint64_t Offset = COffsetValue->getUniqueInteger().getZExtValue();
      LocalPtrOffset = std::max(LocalPtrOffset, Offset);
    }
  } else if (OffsetTy->isIntegerTy()) {
    uint64_t Offset = ConstantOffsets->getUniqueInteger().getZExtValue();
    LocalPtrOffset = std::max(LocalPtrOffset, Offset);
  } else {
    LLVM_DEBUG(
        dbgs()
        << "Offset is unsupported type. Has to be Integer or Vector, but: "
        << *OffsetTy << "\n");
    return None;
  }
  return LocalPtrOffset;
}

//
//  Checks for appropreate operations on ptr and calculates max offset of ptr.
//  Calculation has to be done staticly.
//  ptr may only go to read/write funcitons.
//
bool Substituter::processPTIsUses(Instruction &I,
                                  uint64_t /*OUT*/ &MaxPtrOffset) {
  uint64_t LocalPtrOffset{0};
  for (auto const &U : I.uses()) {
    Instruction *User = dyn_cast<Instruction>(U.getUser());
    if (User->getOpcode() == Instruction::FAdd ||
        User->getOpcode() == Instruction::Add) {
      BinaryOperator *BO = dyn_cast<BinaryOperator>(User);
      auto Offset = processAddInst(I, *BO);
      if (!Offset)
        return false;
      LocalPtrOffset = std::max(LocalPtrOffset, Offset.getValue());
    } else if (GenXIntrinsic::isGenXIntrinsic(User) &&
               User->mayReadOrWriteMemory()) {
      // We can read/write from/to unsplitted block.
      continue;
    } else if (User->getOpcode() != Instruction::ShuffleVector &&
               User->getOpcode() != Instruction::InsertElement) {
      // There extensions are to fit the pattern of using ptrtoint:
      // %pti = ptrtoint %StructTy* %ray to i64
      // %base = insertelement <16 x i64> undef, i64 %pti, i32 0
      // %shuffle = shufflevector <16 x i64> %base, <16 x i64> undef, <16 x i32>
      // zeroinitializer %offset = add nuw nsw <16 x i64> %shuffle, <i64 0, i64
      // 4, ...>

      // Anything else is prohibited.
      return false;
    }

    // Do next processings
    if (!processPTIsUses(*User, LocalPtrOffset))
      return false;
  }
  MaxPtrOffset += LocalPtrOffset;
  return true;
}

//__________________________________________________________________
//          Block of SElement definition
//__________________________________________________________________
DependencyGraph::SElement::SElement(StructType *const &InTy,
                                    unsigned InIndex) noexcept
    : Ty{InTy}, Index{InIndex} {}
DependencyGraph::SElement &DependencyGraph::SElement::
operator=(SElement const &Other) noexcept {
  Ty = Other.Ty;
  Index = Other.Index;
  return *this;
}

//__________________________________________________________________
//          Block of SElementsOfType definition
//__________________________________________________________________
DependencyGraph::SElementsOfType::SElementsOfType(unsigned Size) {
  Types.reserve(Size);
  IndicesOfTypes.reserve(Size);
};
DependencyGraph::SElementsOfType::SElementsOfType(SElementsOfType const &Other)
    : Types{Other.Types}, IndicesOfTypes{Other.IndicesOfTypes} {}
DependencyGraph::SElementsOfType::SElementsOfType(
    SElementsOfType &&Other) noexcept
    : Types{std::move(Other.Types)}, IndicesOfTypes{
                                         std::move(Other.IndicesOfTypes)} {}
// Automaticaly matches Types with sequential Indices
DependencyGraph::SElementsOfType::SElementsOfType(
    std::vector<Type *> const &InTypes)
    : Types{InTypes}, IndicesOfTypes(Types.size()) {
  std::iota(IndicesOfTypes.begin(), IndicesOfTypes.end(), 0);
}
void DependencyGraph::SElementsOfType::emplaceBack(Type &Ty, unsigned Index) {
  Types.emplace_back(&Ty);
  IndicesOfTypes.emplace_back(Index);
}
unsigned DependencyGraph::SElementsOfType::size() const {
  unsigned const Size = Types.size();
  IGC_ASSERT_MESSAGE(Size == IndicesOfTypes.size(),
                     "Size of Types and Indices has to be the same.");
  return Size;
}
Type *DependencyGraph::SElementsOfType::getTyAt(unsigned Index) const {
  IGC_ASSERT_MESSAGE(Index < size(), "Attempt to get element out of borders.");
  return Types.at(Index);
}
unsigned DependencyGraph::SElementsOfType::getIdxAt(unsigned Index) const {
  IGC_ASSERT_MESSAGE(Index < size(), "Attempt to get element out of borders.");
  return IndicesOfTypes.at(Index);
}
std::pair<Type *&, unsigned &>
DependencyGraph::SElementsOfType::at(unsigned Index) {
  IGC_ASSERT_MESSAGE(Index < size(), "Attempt to get element out of borders.");
  return std::make_pair(std::ref(Types.at(Index)),
                        std::ref(IndicesOfTypes.at(Index)));
}
std::pair<Type *const &, unsigned const &>
DependencyGraph::SElementsOfType::at(unsigned Index) const {
  IGC_ASSERT_MESSAGE(Index < size(), "Attempt to get element out of borders.");
  return std::make_pair(std::ref(Types.at(Index)),
                        std::ref(IndicesOfTypes.at(Index)));
}
//__________________________________________________________________
//          Block of Node definition
//__________________________________________________________________
void DependencyGraph::Node::insertParent(Node &ParentNode) {
  auto &&[It, IsInserted] = ParentSTys.emplace(&ParentNode);
  // Insertion may not occur in simillar case like insertChild
}
void DependencyGraph::Node::insertChild(Node &ChildNode) {
  auto &&[It, IsInserted] = ChildSTys.emplace(&ChildNode);
  // Insertion may not occur if there is a dependency like : G {C, C};
}
// Checks of STy is previouse definition of the Node.
bool DependencyGraph::Node::isContainsStruct(StructType &InSTy) const {
  return (STy == &InSTy) ? true
                         : PreviousNames.find(&InSTy) != PreviousNames.end();
}
// Sets STy as new definition of the Node.
void DependencyGraph::Node::substitute(StructType &InSTy) {
  PreviousNames.emplace(STy);
  STy = &InSTy;
}
void DependencyGraph::Node::eraseChild(Node &ChildNode) {
  size_t ElCount = ChildSTys.erase(&ChildNode);
  IGC_ASSERT(ElCount);
}

//__________________________________________________________________
//          Block of NodeMemoryManager definition
//__________________________________________________________________
DependencyGraph::NodeMemoryManager::NodeMemoryManager(Module &M) {
  Nodes.reserve(M.getIdentifiedStructTypes().size());
}
// Allocates memory and holds pointer.
DependencyGraph::Node *
DependencyGraph::NodeMemoryManager::create(StructType &STy) {
  Nodes.emplace_back(std::make_unique<Node>(STy));
  return Nodes.back().get();
}

//
//  Retrieves base type. It tries to unwrap structures and arrays.
//
Type *getBaseTy(Type *Ty) {
  IGC_ASSERT(Ty);

  Type *BaseTy{getArrayFreeTy(Ty)};
  while (StructType *STy = dyn_cast<StructType>(BaseTy)) {
    // If empty struct
    if (!STy->getNumElements())
      return STy;

    BaseTy = getArrayFreeTy(*STy->element_begin());
    // Check that all elements in struct are the same type/subtype
    for (auto &&Elm : STy->elements())
      if (BaseTy != getArrayFreeTy(Elm))
        return STy;
  }
  return BaseTy;
}

//
//  Retrieves base type of the array
//
Type *getArrayFreeTy(Type *Ty) {
  IGC_ASSERT(Ty);
  while (isa<ArrayType>(Ty) || isa<VectorType>(Ty))
    Ty = Ty->getContainedType(0);
  return Ty;
}

//
//  Help function to get type-specific prefix for naming
//
char const *DependencyGraph::getTypePrefix(Type &Ty) {
  Type::TypeID ID = Ty.getTypeID();
  switch (ID) {
  case Type::VoidTyID:
    return "void";
  case Type::HalfTyID:
    return "h";
  case Type::FloatTyID:
    return "f";
  case Type::DoubleTyID:
    return "d";
  case Type::X86_FP80TyID:
    return "x86fp";
  case Type::FP128TyID:
    return "fp";
  case Type::PPC_FP128TyID:
    return "ppcfp";
  case Type::LabelTyID:
    return "l";
  case Type::MetadataTyID:
    return "m";
  case Type::X86_MMXTyID:
    return "mmx";
  case Type::TokenTyID:
    return "t";
  case Type::IntegerTyID:
    return "i";
  case Type::FunctionTyID:
    return "foo";
  case Type::StructTyID:
    return "s";
  case Type::ArrayTyID:
    return "a";
  case Type::PointerTyID:
    return "p";
  default:
    return "unnamed";
  }
}

//__________________________________________________________________
//          Block of data printing
//__________________________________________________________________
void StructFilter::print(raw_ostream &Os) const {
  Os << "Banned structs:\n";
  for (auto *STy : BannedStructs)
    Os << "\t" << *STy << "\n";
  Os << "\n";
}

void DependencyGraph::SElement::print(raw_ostream &Os) const {
  if (Ty)
    Os << "Ty: " << *Ty << "  Index: " << Index;
}

void DependencyGraph::SElementsOfType::print(raw_ostream &Os) const {
  for (unsigned i = 0, end = Types.size(); i != end; ++i)
    Os << "\t\tTy: " << *(Types[i]) << " at pos: " << IndicesOfTypes[i] << "\n";
}

void DependencyGraph::Node::dump(int Tab, raw_ostream &Os) const {
  if (!STy)
    return;
  for (int i = 0; i != Tab; ++i)
    Os << "    ";
  Tab++;
  Os << "Node: " << *STy << "\n";
  if (!ChildSTys.empty()) {
    for (int i = 0; i != Tab; ++i)
      Os << "    ";
    Os << "With childs\n";
  }
  for (auto Child : ChildSTys)
    Child->dump(Tab, Os);
}

void DependencyGraph::printData(raw_ostream &Os) const {
  for (auto &&[Struct, SubTypes] : AllStructs) {
    Os << "Struct " << *Struct << "consists of:\n";
    for (auto &&[SubType, Tys] : SubTypes) {
      Os << "\t"
         << "BaseTy: " << *SubType << "\n";
      Tys.print(Os);
    }
  }
}

void DependencyGraph::print(raw_ostream &Os) const {
  Os << "\n _________________________________";
  Os << "\n/                                 \\\n";
  Os << "Data:\n";
  printData(Os);
  Os << "\nGraph:\n";
  graphDump(Os);
  Os << "\nGenerations:\n";
  printGeneration(Os);
  Os << "\\_________________________________/\n";
}

void DependencyGraph::graphDump(raw_ostream &Os) const {
  for (auto Head : Heads) {
    Os << "Head:\n";
    Head->dump(1, Os);
  }
}

void DependencyGraph::printGeneration(raw_ostream &Os) const {
  for (auto &&SplittedStruct : SplittedStructs) {
    Os << "Splitted struct: " << *SplittedStruct.first << "to: \n";
    for (auto &&ChangedTo : SplittedStruct.second) {
      for (auto &&Elm : ChangedTo) {
        Os << "  ";
        Elm.print(Os);
        Os << ",  ";
      }
      Os << "\n";
    }
  }
}

void Substituter::printAllAllocas(raw_ostream &Os) {
  Os << "Allocas\n";
  for (auto &&[STy, VecOfAllocas] : Allocas) {
    Os << "  For struct: " << *STy << "\n";
    for (auto &&Alloca : VecOfAllocas)
      Os << "    " << *Alloca << "\n";
  }
  Os << "\n";
}