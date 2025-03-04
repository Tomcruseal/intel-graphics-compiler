/*========================== begin_copyright_notice ============================

Copyright (C) 2017-2022 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

#ifndef __GRAPHCOLOR_H__
#define __GRAPHCOLOR_H__

#include "Assertions.h"
#include "BitSet.h"
#include "G4_IR.hpp"
#include "RPE.h"
#include "SpillManagerGMRF.h"
#include "VarSplit.h"

// clang-format off
#include "common/LLVMWarningsPush.hpp"
#include "llvm/Support/Allocator.h"
#include "common/LLVMWarningsPop.hpp"
// clang-format on

#include <limits>
#include <list>
#include <map>
#include <memory>
#include <queue>
#include <unordered_set>
#include <vector>

#define BITS_DWORD 32
#define ROUND(x, y) ((x) + ((y - x % y) % y))

namespace vISA {
const float MAXSPILLCOST = (std::numeric_limits<float>::max());
const float MINSPILLCOST = -(std::numeric_limits<float>::max());

enum BankConflict {
  BANK_CONFLICT_NONE,
  BANK_CONFLICT_FIRST_HALF_EVEN,
  BANK_CONFLICT_FIRST_HALF_ODD,
  BANK_CONFLICT_SECOND_HALF_EVEN,
  BANK_CONFLICT_SECOND_HALF_ODD
};

class BankConflictPass {
private:
  GlobalRA &gra;
  bool forGlobal;

  BankConflict setupBankAccordingToSiblingOperand(BankConflict assignedBank,
                                                  unsigned offset,
                                                  bool oneGRFBank);
  void setupEvenOddBankConflictsForDecls(G4_Declare *dcl_1, G4_Declare *dcl_2,
                                         unsigned offset1, unsigned offset2,
                                         BankConflict &srcBC1,
                                         BankConflict &srcBC2);
  void setupBankConflictsOneGRFOld(G4_INST *inst, int &bank1RegNum,
                                   int &bank2RegNum, float GRFRatio,
                                   unsigned &internalConflict);
  bool isOddOffset(unsigned offset) const;
  void setupBankConflictsforDPAS(G4_INST *inst);
  bool hasDpasInst = false;

  void setupBankConflictsforTwoGRFs(G4_INST *inst);
  void setupBankConflictsforMad(G4_INST *inst);
  void setupBankConflictsForBB(G4_BB *bb, unsigned &threeSourceInstNum,
                               unsigned &sendInstNum, unsigned numRegLRA,
                               unsigned &internalConflict);
  void setupBankConflictsForBBTGL(G4_BB *bb, unsigned &threeSourceInstNum,
                                  unsigned &sendInstNum, unsigned numRegLRA,
                                  unsigned &internalConflict);
  bool hasInternalConflict3Srcs(BankConflict *srcBC);
  void setupBankForSrc0(G4_INST *inst, G4_INST *prevInst);
  void getBanks(G4_INST *inst, BankConflict *srcBC, G4_Declare **dcls,
                G4_Declare **opndDcls, unsigned *offset);
  void getPrevBanks(G4_INST *inst, BankConflict *srcBC, G4_Declare **dcls,
                    G4_Declare **opndDcls, unsigned *offset);

public:
  bool setupBankConflictsForKernel(bool doLocalRR, bool &threeSourceCandidate,
                                   unsigned numRegLRA,
                                   bool &highInternalConflict);

  BankConflictPass(GlobalRA &g, bool global) : gra(g), forGlobal(global) {}
};

// The forbidden kind for the forbidden bit of each register files.
// Note that:
// a) There is no forbidden regsiter for address and flag regsiters.
// We keep them just in case.
// b) All the forbidden kinds from EOT to RESERVEGRF are for GRF
enum class forbiddenKind {
  FBD_ADDR = 0,
  FBD_FLAG = 1,
  FBD_RESERVEDGRF,
  FBD_EOT,
  FBD_LASTGRF,
  FBD_EOTLASTGRF,
  FBD_CALLERSAVE,
  FBD_CALLEESAVE,
  FBD_NUM,
};

enum class AugmentationMasks {
  Undetermined = 0,
  Default16Bit = 1,
  Default32Bit = 2,
  Default64Bit = 3,
  DefaultPredicateMask = 4,
  NonDefault = 5,
};

class LiveRange final {
  G4_RegVar *const var;
  G4_Declare *const dcl;
  const G4_RegFileKind regKind;
  forbiddenKind forbiddenType;
  BitSet *forbidden = nullptr;
  bool spilled = false;
  bool isUnconstrained = false;

  GlobalRA &gra;
  unsigned numRegNeeded;
  unsigned degree = 0;
  unsigned refCount = 0;
  unsigned parentLRID;
  AssignedReg reg;
  float spillCost;
  BankConflict bc = BANK_CONFLICT_NONE;
  const static unsigned UndefHint = 0xffffffff;
  unsigned allocHint = UndefHint;

  union {
    uint16_t bunch = 0;
    struct {
      uint16_t calleeSaveBias : 1; // indicates if the var is biased to get a
                                   // callee-save assignment or not
      uint16_t callerSaveBias : 1; // indicates if the var is biased to get a
                                   // caller-save assignment or not
      uint16_t isEOTSrc : 1; // Gen7 only, Whether the liveRange is the message
                             // source of an EOT send
      uint16_t retIp : 1; // variable is the return ip and should not be spilled

      uint16_t active : 1;
      uint16_t isInfiniteCost : 1;
      uint16_t isCandidate : 1;
      uint16_t isPseudoNode : 1;
      uint16_t isPartialDeclare : 1;
      uint16_t isSplittedDeclare : 1;
    };
  };

  LiveRange(G4_RegVar *v, GlobalRA &);

public:
  static LiveRange *createNewLiveRange(G4_Declare *dcl, GlobalRA &gra);

  void initialize();
  void initializeForbidden();

  void *operator new(size_t sz, llvm::SpecificBumpPtrAllocator<LiveRange> &m) {
    return m.Allocate();
  }

  void setBitFieldUnionValue(uint16_t v) { bunch = v; }

  void setDegree(unsigned d) { degree = d; }
  unsigned getDegree() const { return degree; }

  void setUnconstrained(bool d) { isUnconstrained = d; }
  bool getIsUnconstrained() const { return isUnconstrained; }

  unsigned getNumRegNeeded() const { return numRegNeeded; }

  void subtractDegree(unsigned d) {
    vISA_ASSERT(d <= degree, ERROR_INTERNAL_ARGUMENT);
    degree -= d;
  }

  void setActive(bool v) { active = v; }
  bool getActive() const { return active; }

  void emit(std::ostream &output) const {
    output << getVar()->getDeclare()->getName();
    if (reg.phyReg != NULL) {
      output << "(";
      reg.phyReg->emit(output);
      output << '.' << reg.subRegOff << ':';
      output << TypeSymbol(getVar()->getDeclare()->getElemType()) << ")";
    }
    output << "(size = " << getDcl()->getByteSize()
           << ", spill cost = " << getSpillCost()
           << ", degree = " << getDegree() << ")";
  }

  unsigned getRefCount() const { return refCount; }
  void setRefCount(unsigned count) { refCount = count; }

  float getSpillCost() const { return spillCost; }
  void setSpillCost(float cost) { spillCost = cost; }

  bool getIsInfiniteSpillCost() const { return isInfiniteCost; }
  void checkForInfiniteSpillCost(G4_BB *bb,
                                 std::list<G4_INST *>::reverse_iterator &it);

  G4_VarBase *getPhyReg() const { return reg.phyReg; }

  unsigned getPhyRegOff() const { return reg.subRegOff; }

  void setPhyReg(G4_VarBase *pr, unsigned off) {
    vISA_ASSERT(pr->isPhyReg(), ERROR_UNKNOWN);
    reg.phyReg = pr;
    reg.subRegOff = off;
  }

  void resetPhyReg() {
    reg.phyReg = nullptr;
    reg.subRegOff = 0;
  }

  bool getIsPseudoNode() const { return isPseudoNode; }
  void setIsPseudoNode() { isPseudoNode = true; }
  bool getIsPartialDcl() const { return isPartialDeclare; }
  void setIsPartialDcl() { isPartialDeclare = true; }
  bool getIsSplittedDcl() const { return isSplittedDeclare; }
  void setIsSplittedDcl(bool v) { isSplittedDeclare = v; }
  BankConflict getBC() const { return bc; }
  void setBC(BankConflict c) { bc = c; }
  void setParentLRID(int id) { parentLRID = id; }
  unsigned getParentLRID() const { return parentLRID; }

  unsigned getAllocHint() const { return allocHint; }
  bool hasAllocHint() const { return allocHint != UndefHint; }
  void setAllocHint(unsigned h);
  void resetAllocHint() { allocHint = UndefHint; }

  // From VarBasis
public:
  void setForbidden(forbiddenKind f);
  void markForbidden(vISA::Mem_Manager &GCMem, int reg, int numReg);
  BitSet *getForbidden();
  int getNumForbidden();
  G4_RegVar *getVar() const { return var; }
  G4_Declare *getDcl() const { return dcl; }
  G4_RegFileKind getRegKind() const { return regKind; }
  void dump() const;

  void setCalleeSaveBias(bool v) { calleeSaveBias = v; }
  bool getCalleeSaveBias() const { return calleeSaveBias; }

  void setCallerSaveBias(bool v) { callerSaveBias = v; }
  bool getCallerSaveBias() const { return callerSaveBias; }

  void setEOTSrc() { isEOTSrc = true; }
  bool getEOTSrc() const { return isEOTSrc; }

  void setRetIp() { retIp = true; }
  bool isRetIp() const { return retIp; }

  bool isSpilled() const { return spilled; }
  void setSpilled(bool v) { spilled = v; }

  void setCandidate(bool v) { isCandidate = v; }
  bool getCandidate() const { return isCandidate; }

  void resetForbidden() {
    forbidden = nullptr;
    forbiddenType = forbiddenKind::FBD_NUM;
  }

private:
  // const Options *m_options;
  unsigned getForbiddenVectorSize() const;
}; // class LiveRange
} // namespace vISA
using LIVERANGE_LIST = std::list<vISA::LiveRange *>;
using LIVERANGE_LIST_ITER = LIVERANGE_LIST::iterator;
using LiveRangeVec = std::vector<vISA::LiveRange *>;

// A mapping from the pseudo decl created for caller save/restore, to the ret
// val This is used in augmentIntfGraph to prune interference edges for fcall
// ret val
typedef std::map<vISA::G4_Declare *, vISA::G4_Declare *> FCALL_RET_MAP;
typedef std::map<vISA::G4_Declare *, std::pair<vISA::G4_INST *, unsigned>>
    CALL_DECL_MAP;

namespace vISA {
struct criticalCmpForEndInterval {
  GlobalRA &gra;
  criticalCmpForEndInterval(GlobalRA &g);
  bool operator()(G4_Declare *A, G4_Declare *B) const;
};
struct AugmentPriorityQueue
    : std::priority_queue<G4_Declare *, std::vector<G4_Declare *>,
                          criticalCmpForEndInterval> {
  AugmentPriorityQueue(criticalCmpForEndInterval cmp);
  auto begin() const { return c.begin(); }
  auto end() const { return c.end(); }
};
} // namespace vISA
//
// A bit array records all interference information.
// (2D matrix is flatten to 1D array)
// Since the interference information is symmetric, we can use only
// half of the size. To simplify the implementation, we use the full
// size of the bit array.
//
namespace vISA {
class Augmentation {
private:
  // pair of default mask, non-default mask
  using MaskDeclares = std::pair<llvm_SBitVector, llvm_SBitVector>;
  G4_Kernel &kernel;
  Interference &intf;
  GlobalRA &gra;
  const LivenessAnalysis &liveAnalysis;
  const LiveRangeVec &lrs;
  FCALL_RET_MAP &fcallRetMap;
  CALL_DECL_MAP callDclMap;
  std::unordered_map<FuncInfo *, PhyRegSummary> localSummaryOfCallee;
  std::vector<G4_Declare *> sortedIntervals;
  AugmentPriorityQueue defaultMaskQueue{criticalCmpForEndInterval(gra)};
  AugmentPriorityQueue nonDefaultMaskQueue{criticalCmpForEndInterval(gra)};
  // overlapDclsWithFunc holds default and non-default range live across
  // all call sites of func.
  std::unordered_map<FuncInfo *, MaskDeclares> overlapDclsWithFunc;
  std::unordered_map<G4_Declare *, MaskDeclares> retDeclares;

  bool updateDstMaskForGather(G4_INST *inst, std::vector<unsigned char> &mask);
  bool updateDstMaskForGatherRaw(G4_INST *inst,
                                 std::vector<unsigned char> &mask,
                                 const G4_SendDescRaw *rawDesc);
  void updateDstMask(G4_INST *inst, bool checkCmodOnly);
  static unsigned getByteSizeFromMask(AugmentationMasks type);
  bool isDefaultMaskDcl(G4_Declare *dcl, unsigned simdSize,
                        AugmentationMasks type);
  bool isDefaultMaskSubDeclare(unsigned char *mask, unsigned lb, unsigned rb,
                               G4_Declare *dcl, unsigned simdSize);
  bool verifyMaskIfInit(G4_Declare *dcl, AugmentationMasks mask);
  bool checkGRFPattern3(G4_Declare *dcl, G4_DstRegRegion *dst, unsigned maskOff,
                        unsigned lb, unsigned rb, unsigned execSize);
  bool checkGRFPattern2(G4_Declare *dcl, G4_DstRegRegion *dst, unsigned maskOff,
                        unsigned lb, unsigned rb, unsigned execSize);
  bool checkGRFPattern1(G4_Declare *dcl, G4_DstRegRegion *dst, unsigned maskOff,
                        unsigned lb, unsigned rb, unsigned execSize);
  void markNonDefaultDstRgn(G4_INST *inst, G4_Operand *opnd);
  bool markNonDefaultMaskDef();
  void updateStartIntervalForSubDcl(G4_Declare *dcl, G4_INST *curInst,
                                    G4_Operand *opnd);
  void updateEndIntervalForSubDcl(G4_Declare *dcl, G4_INST *curInst,
                                  G4_Operand *opnd);
  void updateStartInterval(const G4_Declare *dcl, G4_INST *curInst);
  void updateEndInterval(const G4_Declare *dcl, G4_INST *curInst);
  void updateStartIntervalForLocal(G4_Declare *dcl, G4_INST *curInst,
                                   G4_Operand *opnd);
  void updateEndIntervalForLocal(G4_Declare *dcl, G4_INST *curInst,
                                 G4_Operand *opnd);
  void buildLiveIntervals();
  void sortLiveIntervals();
  unsigned getEnd(const G4_Declare *dcl) const;
  bool isNoMask(const G4_Declare *dcl, unsigned size) const;
  bool isConsecutiveBits(const G4_Declare *dcl, unsigned size) const;
  bool isCompatible(const G4_Declare *testDcl,
                    const G4_Declare *biggerDcl) const;
  void buildInterferenceIncompatibleMask();
  void buildInteferenceForCallSiteOrRetDeclare(G4_Declare *newDcl,
                                               MaskDeclares *mask);
  void buildInteferenceForCallsite(FuncInfo *func);
  void buildInteferenceForRetDeclares();
  void buildSummaryForCallees();
  void expireIntervals(unsigned startIdx);
  void buildSIMDIntfDcl(G4_Declare *newDcl, bool isCall);
  void buildSIMDIntfAll(G4_Declare *newDcl);
  void handleSIMDIntf(G4_Declare *firstDcl, G4_Declare *secondDcl, bool isCall);
  bool weakEdgeNeeded(AugmentationMasks, AugmentationMasks);

  void addSIMDIntfDclForCallSite(G4_BB *callBB);

  void addSIMDIntfForRetDclares(G4_Declare *newDcl);

public:
  Augmentation(G4_Kernel &k, Interference &i, const LivenessAnalysis &l,
               const LiveRangeVec &ranges, GlobalRA &g);
  ~Augmentation();

  void augmentIntfGraph();

  const std::vector<G4_Declare *> &getSortedLiveIntervals() const {
    return sortedIntervals;
  }
};

// This class contains implementation of various methods to implement
// incremental intf computation. Instance of this class is created
// once and stored in GlobalRA. This class should therefore not
// hold pointer to GraphColor/Interference or such other short-living
// instances.
class IncrementalRA {
private:
  GlobalRA &gra;
  G4_Kernel &kernel;
  LiveRangeVec lrs;
  G4_RegFileKind selectedRF = G4_RegFileKind::G4_UndefinedRF;
  unsigned int level = 0;
  std::unordered_set<G4_Declare *> needIntfUpdate;
  unsigned int maxDclId = 0;
  // Map of root G4_Declare* -> id assigned to its G4_RegVar
  // This allows us to reuse ids from previous iteration.
  std::unordered_map<G4_Declare *, unsigned int> varIdx;
  unsigned int maxVarIdx = 0;

  // Reset state to mark start of new type of GRA (eg, from flag to GRF)
  void reset();

public:
  llvm::SpecificBumpPtrAllocator<LiveRange> mem;

  IncrementalRA(GlobalRA &g);

  bool isEnabled() { return level > 0; }
  bool isEnabledWithVerification() { return level == 2; }

  static bool isEnabled(G4_Kernel &kernel) {
    // 0 - disabled
    // 1 - enabled
    // 2 - enabled with verification
    return kernel.getOptions()->getuInt32Option(vISA_IncrementalRA) >= 1;
  }

  static bool isEnabledWithVerification(G4_Kernel &kernel) {
    return kernel.getOptions()->getuInt32Option(vISA_IncrementalRA) == 2;
  }

  void registerNextIter(G4_RegFileKind rf,
                        const LivenessAnalysis *liveness = nullptr);
  // After computing interference incrementally, GraphColor needs to clear
  // candidate list to prepare for new incremental RA temps.
  void clearCandidates() { needIntfUpdate.clear(); }

  LiveRangeVec &getLRs() { return lrs; }

  G4_RegFileKind getSelectedRF() const { return selectedRF; }

  // This method is invoked when a new G4_Declare is created and a
  // LiveRange instance needs to be added for it.
  void addNewRAVariable(G4_Declare *dcl);
  // This method is invoked when an existing RA variable is either
  // removed from the program or a change is expected in liveness
  // of a variable due to optimization.
  void markForIntfUpdate(G4_Declare *dcl);

  void skipIncrementalRANextIter();

  void moveFromHybridToGlobalGRF() {
    varIdx.clear();
    maxVarIdx = 0;
    reset();
  }

  // Return idx of a G4_RegVar if it was given an id in previous
  // iteration. If dcl was present in previous id assign phase
  // return pair <true, id>. When dcl is seen for first time,
  // return <false, X>. Second field of pair contains legal value
  // only if first field is true.
  std::pair<bool, unsigned int> getIdFromPrevIter(G4_Declare *dcl);

  // Record new dcl and id assigned to its G4_RegVar. Update
  // maxVarIdx so we know first free id in next RA iteration.
  void recordVarId(G4_Declare *dcl, unsigned int id);

  // Return next id that can be assigned to a new variable. In 1st
  // RA iteration, this returns 0 because no variables exist in
  // incremental RA. In 2nd RA iteration, this method returns
  // first available index that can be assigned to new variable.
  unsigned int getNextVarId(unsigned char RF) {
    if ((RF & selectedRF) == 0) {
      varIdx.clear();
      maxVarIdx = 0;
    }
    if (varIdx.size() == 0)
      return 0;
    return maxVarIdx + 1;
  }

  // Handle local split here. reduceBy argument tells us how many
  // G4_Declares were removed by resetGlobalRAStates().
  // TODO: Deprecate this method once we stop erasing old partial
  // dcls from kernel.Declares
  void reduceMaxDclId(unsigned int reduceBy) {
    if (!level)
      return;
    maxDclId -= reduceBy;
  }

private:
  // For verification only
  std::vector<llvm_SBitVector> def_in;
  std::vector<llvm_SBitVector> def_out;
  std::vector<llvm_SBitVector> use_in;
  std::vector<llvm_SBitVector> use_out;
  std::vector<llvm_SBitVector> use_gen;
  std::vector<llvm_SBitVector> use_kill;

  std::unique_ptr<VarReferences> prevIterRefs;

  // Return true if verification passes, false otherwise
  bool verify(const LivenessAnalysis *curLiveness) const;

  // Copy over liveness sets from current iteration's liveness
  void copyLiveness(const LivenessAnalysis *liveness);

public:
  std::unordered_set<G4_Declare *> unassignedVars;

  // Compute variables that are left over in sorted list when
  // computing color order. This is to aid debugging only.
  void computeLeftOverUnassigned(const LiveRangeVec &sorted,
                                 const LivenessAnalysis &liveAnalysis);
};

class Interference {
  friend class Augmentation;

  // This stores compatible ranges for each variable. Such
  // compatible ranges will not be present in sparseIntf set.
  // We store G4_Declare* instead of id is because variables
  // allocated by LRA will not have a valid id.
  std::map<G4_Declare *, std::vector<G4_Declare *>> compatibleSparseIntf;

  GlobalRA &gra;
  G4_Kernel &kernel;
  const LiveRangeVec &lrs;
  IR_Builder &builder;
  const unsigned maxId;
  const unsigned rowSize;
  const unsigned splitStartId;
  const unsigned splitNum;
  unsigned *matrix = nullptr;
  const LivenessAnalysis *const liveAnalysis;
  Augmentation aug;
  IncrementalRA &incRA;

  std::vector<std::vector<unsigned>> sparseIntf;

  // sparse interference matrix.
  // we don't directly update sparseIntf to ensure uniqueness
  // like dense matrix, interference is not symmetric (that is, if v1 and v2
  // interfere and v1 < v2, we insert (v1, v2) but not (v2, v1)) for better
  // cache behavior
  std::vector<llvm_SBitVector> sparseMatrix;

  unsigned int denseMatrixLimit = 0;

  static void updateLiveness(llvm_SBitVector &live, uint32_t id, bool val) {
    if (val) {
      live.set(id);
    } else {
      live.reset(id);
    }
  }

  G4_Declare *getGRFDclForHRA(int GRFNum) const;

  bool useDenseMatrix() const {
    // The size check is added to prevent offset overflow in
    // generateSparseIntfGraph() and help avoid out-of-memory
    // issue in dense matrix allocation.
    unsigned long long size = static_cast<unsigned long long>(rowSize) *
                              static_cast<unsigned long long>(maxId);
    unsigned long long max = std::numeric_limits<unsigned int>::max();
    return (maxId < denseMatrixLimit) && (size < max);
  }

  // Only upper-half matrix is now used in intf graph.
  inline void safeSetInterference(unsigned v1, unsigned v2) {
    // Assume v1 < v2
    if (useDenseMatrix()) {
      unsigned col = v2 / BITS_DWORD;
      matrix[v1 * rowSize + col] |= 1 << (v2 % BITS_DWORD);
    } else {
      sparseMatrix[v1].set(v2);
    }
  }

  inline void safeClearInterference(unsigned v1, unsigned v2) {
    // Assume v1 < v2
    if (useDenseMatrix()) {
      unsigned col = v2 / BITS_DWORD;
      matrix[v1 * rowSize + col] &= ~(1 << (v2 % BITS_DWORD));
    } else {
      sparseMatrix[v1].reset(v2);
    }
  }

  inline void setBlockInterferencesOneWay(unsigned v1, unsigned col,
                                          unsigned block) {
    if (useDenseMatrix()) {
#ifdef _DEBUG
      vISA_ASSERT(
          sparseIntf.size() == 0,
          "Updating intf graph matrix after populating sparse intf graph");
#endif

      matrix[v1 * rowSize + col] |= block;
    } else {
      auto &&intfSet = sparseMatrix[v1];
      for (int i = 0; i < BITS_DWORD; ++i) {
        if (block & (1 << i)) {
          uint32_t v2 = col * BITS_DWORD + i;
          intfSet.set(v2);
        }
      }
    }
  }

  unsigned getInterferenceBlk(unsigned idx) const {
    vISA_ASSERT(useDenseMatrix(), "matrix is not initialized");
    return matrix[idx];
  }

  void addCalleeSaveBias(const llvm_SBitVector &live);

  void buildInterferenceAtBBExit(const G4_BB *bb, llvm_SBitVector &live);
  void buildInterferenceWithinBB(G4_BB *bb, llvm_SBitVector &live);
  void buildInterferenceForDst(G4_BB *bb, llvm_SBitVector &live, G4_INST *inst,
                               std::list<G4_INST *>::reverse_iterator i,
                               G4_DstRegRegion *dst);
  void buildInterferenceForFcall(G4_BB *bb, llvm_SBitVector &live,
                                 G4_INST *inst,
                                 std::list<G4_INST *>::reverse_iterator i,
                                 const G4_VarBase *regVar);

  inline void filterSplitDclares(unsigned startIdx, unsigned endIdx, unsigned n,
                                 unsigned col, unsigned &elt, bool is_split);
  void buildInterferenceWithLive(const llvm_SBitVector &live, unsigned i);
  void buildInterferenceWithSubDcl(unsigned lr_id, G4_Operand *opnd,
                                   llvm_SBitVector &live, bool setLive,
                                   bool setIntf);
  void buildInterferenceWithAllSubDcl(unsigned v1, unsigned v2);

  void markInterferenceForSend(G4_BB *bb, G4_INST *inst, G4_DstRegRegion *dst);

  void buildInterferenceWithLocalRA(G4_BB *bb);

  void buildInterferenceAmongLiveOuts();
  void buildInterferenceAmongLiveIns();

  void markInterferenceToAvoidDstSrcOverlap(G4_BB *bb, G4_INST *inst);

  void generateSparseIntfGraph();
  void countNeighbors();

  void setupLRs(G4_BB *bb);

public:
  Interference(const LivenessAnalysis *l, const LiveRangeVec &lr, unsigned n,
               unsigned ns, unsigned nm, GlobalRA &g);

  ~Interference() {
    if (useDenseMatrix()) {
      delete[] matrix;
    }
  }

  const std::vector<G4_Declare *> *
  getCompatibleSparseIntf(G4_Declare *d) const {
    if (compatibleSparseIntf.size() > 0) {
      auto it = compatibleSparseIntf.find(d);
      if (it == compatibleSparseIntf.end()) {
        return nullptr;
      }
      return &it->second;
    }
    return nullptr;
  }

  void init() {
    if (useDenseMatrix()) {
      auto N = (size_t)rowSize * (size_t)maxId;
      matrix = new uint32_t[N](); // zero-initialize
    } else {
      sparseMatrix.resize(maxId);
    }
  }

  void computeInterference();
  void getNormIntfNum();
  void applyPartitionBias();
  bool interfereBetween(unsigned v1, unsigned v2) const;
  const std::vector<unsigned> &getSparseIntfForVar(unsigned id) const {
    return sparseIntf[id];
  }

  inline bool varSplitCheckBeforeIntf(unsigned v1, unsigned v2) const;

  void checkAndSetIntf(unsigned v1, unsigned v2) {
    if (v1 < v2) {
      safeSetInterference(v1, v2);
    } else if (v1 > v2) {
      safeSetInterference(v2, v1);
    }
  }

  void dumpInterference() const;
  void dumpVarInterference() const;
  bool dumpIntf(const char *) const;
  void interferenceVerificationForSplit() const;

  bool linearScanVerify() const;

  bool isStrongEdgeBetween(const G4_Declare *, const G4_Declare *) const;

  const Augmentation &getAugmentation() const { return aug; }
};

// Class to compute reg chart dump and dump it to ostream.
// Used only when -dumpregchart is passed.
class RegChartDump {
  const GlobalRA &gra;
  std::vector<G4_Declare *> sortedLiveIntervals;
  std::unordered_map<G4_Declare *, std::pair<G4_INST *, G4_INST *>> startEnd;

public:
  void recordLiveIntervals(const std::vector<G4_Declare *> &dcls);
  void dumpRegChart(std::ostream &, const LiveRangeVec &lrs, unsigned numLRs);

  RegChartDump(const GlobalRA &g) : gra(g) {}
};

class GraphColor {
  GlobalRA &gra;

  // This is not necessarily the same as the number of available physical GRFs,
  // as failSafeRA will reserve some GRF.
  // FIXME: failSafeRA should use RegisterClass/forbidden GRF to model this
  // instead of directly changing the number of GRFs.
  unsigned totalGRFRegCount;
  const unsigned numVar;
  // The original code has no comments whatsoever (sigh), but best as I can tell
  // this vector is used to track the active values held by each of A0's
  // phyiscal subreg. The values themselves correspond to a word in the
  // GRF home location of a spilled address variable. Each GRF home location is
  // represented by its allocation order and assumed to be 16-word wide
  // regardless of its actual size; in other words,
  // AddrSpillLoc0 has [0-15],
  // AddrSpillLoc1 has [16-31],
  // and so on.
  // When the clean up code sees a address fill of the form
  //    mov (N) a0.i AddrSpillLoc(K).S<1;1,0>:uw
  // it updates spAddrRegSig[i, i+N) = [K*16+S, K*16+S+N)
  // When it sees a write to AddrSpillLoc, i.e., a spill of the form
  //    mov (N) AddrSpillLoc(k) a0.i<1;1,0>:uw
  // it clears spAddrRegSig's entries that hold AddrSpillLoc(k).
  // If it encounters a non-fill write to A0 (e.g., send message descriptor
  // write), it also clears the corresponding bits in spAddrRegSig.
  //
  // FIXME: This code is very likely to be buggy, since its initial value is 0
  // and this conflicts with AddrSpillLoc0's first word.
  std::vector<unsigned> spAddrRegSig;
  Interference intf;
  PhyRegPool &regPool;
  IR_Builder &builder;
  LiveRangeVec &lrs;
  bool isHybrid;
  LIVERANGE_LIST spilledLRs;
  bool forceSpill;
  vISA::Mem_Manager GCMem;
  const Options *m_options;

  unsigned evenTotalDegree = 1;
  unsigned oddTotalDegree = 1;
  unsigned evenTotalRegNum = 1;
  unsigned oddTotalRegNum = 1;
  unsigned evenMaxRegNum = 1;
  unsigned oddMaxRegNum = 1;

  G4_Kernel &kernel;
  LivenessAnalysis &liveAnalysis;

  LiveRangeVec colorOrder;
  LIVERANGE_LIST unconstrainedWorklist;
  LIVERANGE_LIST constrainedWorklist;
  unsigned numColor = 0;

  bool failSafeIter = false;

  unsigned edgeWeightGRF(const LiveRange *lr1, const LiveRange *lr2);
  unsigned edgeWeightARF(const LiveRange *lr1, const LiveRange *lr2);

  void computeDegreeForGRF();
  void computeDegreeForARF();
  void computeSpillCosts(bool useSplitLLRHeuristic, const RPE *rpe);
  void determineColorOrdering();
  void removeConstrained();
  void relaxNeighborDegreeGRF(LiveRange *lr);
  void relaxNeighborDegreeARF(LiveRange *lr);
  bool assignColors(ColorHeuristic heuristicGRF, bool doBankConflict,
                    bool highInternalConflict, bool doBundleConflict = false,
                    bool doCoalescing = true);
  bool assignColors(ColorHeuristic h) {
    // Do graph coloring without bank conflict reduction.
    return assignColors(h, false, false);
  }

  void clearSpillAddrLocSignature() {
    std::fill(spAddrRegSig.begin(), spAddrRegSig.end(), 0);
  }
  void pruneActiveSpillAddrLocs(G4_DstRegRegion *, unsigned, G4_Type);
  void updateActiveSpillAddrLocs(G4_DstRegRegion *, G4_SrcRegRegion *,
                                 unsigned execSize);
  bool redundantAddrFill(G4_DstRegRegion *, G4_SrcRegRegion *,
                         unsigned execSize);

  void gatherScatterForbiddenWA();

public:
  void getExtraInterferenceInfo();
  GraphColor(LivenessAnalysis &live, bool hybrid, bool forceSpill_);

  const Options *getOptions() const { return m_options; }

  bool regAlloc(bool doBankConflictReduction, bool highInternalConflict,
                const RPE *rpe);
  bool requireSpillCode() const { return !spilledLRs.empty(); }
  const Interference *getIntf() const { return &intf; }
  void createLiveRanges();
  const LiveRangeVec &getLiveRanges() const { return lrs; }
  const LIVERANGE_LIST &getSpilledLiveRanges() const { return spilledLRs; }
  void confirmRegisterAssignments();
  void resetTemporaryRegisterAssignments();
  void cleanupRedundantARFFillCode();
  void getCalleeSaveRegisters();
  void addA0SaveRestoreCode();
  void addFlagSaveRestoreCode();
  void getSaveRestoreRegister();
  void getCallerSaveRegisters();
  void dumpRegisterPressure();
  GlobalRA &getGRA() { return gra; }
  G4_SrcRegRegion *getScratchSurface() const;
  unsigned int getNumVars() const { return numVar; }
  float getSpillRatio() const { return (float)spilledLRs.size() / numVar; }
  void markFailSafeIter(bool f) { failSafeIter = f; }
  void setTotalGRFRegCount(unsigned c) { totalGRFRegCount = c; }
  unsigned getTotalGRFRegCount() { return totalGRFRegCount; }
};

struct BundleConflict {
  const G4_Declare *const dcl;
  const int offset;
  BundleConflict(const G4_Declare *dcl, int offset)
      : dcl(dcl), offset(offset) {}
};

struct RAVarInfo {
  unsigned numSplit = 0;
  unsigned bb_id = UINT_MAX; // block local variable's block id.
  G4_Declare *splittedDCL = nullptr;
  LocalLiveRange *localLR = nullptr;
  LSLiveRange *LSLR = nullptr;
  unsigned numRefs = 0;
  BankConflict conflict =
      BANK_CONFLICT_NONE; // used to indicate bank that should be assigned to
                          // dcl if possible
  G4_INST *startInterval = nullptr;
  G4_INST *endInterval = nullptr;
  std::vector<unsigned char> mask;
  std::vector<const G4_Declare *> subDclList;
  unsigned subOff = 0;
  std::vector<BundleConflict> bundleConflicts;
  G4_SubReg_Align subAlign = G4_SubReg_Align::Any;
  bool isEvenAlign = false;
};

class VerifyAugmentation {
private:
  G4_Kernel *kernel = nullptr;
  GlobalRA *gra = nullptr;
  std::vector<G4_Declare *> sortedLiveRanges;
  std::unordered_map<
      const G4_Declare *,
      std::tuple<LiveRange *, AugmentationMasks, G4_INST *, G4_INST *>>
      masks;
  LiveRangeVec lrs;
  unsigned numVars = 0;
  const Interference *intf = nullptr;
  std::unordered_map<G4_Declare *, LiveRange *> DclLRMap;
  std::unordered_map<G4_BB *, std::string> bbLabels;
  std::vector<std::tuple<G4_BB *, unsigned, unsigned>> BBLexId;

  static const char *getStr(AugmentationMasks a) {
    if (a == AugmentationMasks::Default16Bit)
      return "Default16Bit";
    else if (a == AugmentationMasks::Default32Bit)
      return "Default32Bit";
    else if (a == AugmentationMasks::Default64Bit)
      return "Default64Bit";
    else if (a == AugmentationMasks::NonDefault)
      return "NonDefault";
    else if (a == AugmentationMasks::Undetermined)
      return "Undetermined";

    return "-----";
  };
  void labelBBs();
  void populateBBLexId();
  bool interfereBetween(G4_Declare *, G4_Declare *);
  void verifyAlign(G4_Declare *dcl);
  unsigned getGRFBaseOffset(const G4_Declare *dcl) const;

public:
  void verify();
  void reset() {
    sortedLiveRanges.clear();
    masks.clear();
    kernel = nullptr;
    gra = nullptr;
    numVars = 0;
    intf = nullptr;
    DclLRMap.clear();
    bbLabels.clear();
    BBLexId.clear();
  }
  void loadAugData(std::vector<G4_Declare *> &s, const LiveRangeVec &l,
                   unsigned n, const Interference *i, GlobalRA &g);
  void dump(const char *dclName);
  bool isClobbered(LiveRange *lr, std::string &msg);
};

class PointsToAnalysis;

class ForbiddenRegs {
  IR_Builder &builder;
  std::vector<BitSet> forbiddenVec;

public:
  ForbiddenRegs(IR_Builder &b) : builder(b) {
    // Initialize forbidden bits
    forbiddenVec.resize((size_t)forbiddenKind::FBD_NUM);
    forbiddenVec[(size_t)forbiddenKind::FBD_ADDR].resize(
        getForbiddenVectorSize(G4_ADDRESS));
    forbiddenVec[(size_t)forbiddenKind::FBD_FLAG].resize(
        getForbiddenVectorSize(G4_FLAG));
  };
  ~ForbiddenRegs(){};

  unsigned getForbiddenVectorSize(G4_RegFileKind regKind) const;
  void generateReservedGRFForbidden(unsigned reserveSpillSize);
  void generateLastGRFForbidden();
  void generateEOTGRFForbidden();
  void generateEOTLastGRFForbidden();
  void generateCallerSaveGRFForbidden();
  void generateCalleeSaveGRFForbidden();

  BitSet *getForbiddenRegs(forbiddenKind type) {
    return &forbiddenVec[(size_t)type];
  }
};

class GlobalRA {

private:
  std::unordered_set<G4_INST *> EUFusionCallWAInsts;
  bool m_EUFusionCallWANeeded;
  std::unordered_set<G4_INST *> EUFusionNoMaskWAInsts;

public:
  bool EUFusionCallWANeeded() const { return m_EUFusionCallWANeeded; }
  void addEUFusionCallWAInst(G4_INST *inst);
  void removeEUFusionCallWAInst(G4_INST *inst) {
    EUFusionCallWAInsts.erase(inst);
  }
  const std::unordered_set<G4_INST *> &getEUFusionCallWAInsts() {
    return EUFusionCallWAInsts;
  }
  bool EUFusionNoMaskWANeeded() const { return builder.hasFusedEUNoMaskWA(); }
  void addEUFusionNoMaskWAInst(G4_BB *BB, G4_INST *Inst);
  void removeEUFusionNoMaskWAInst(G4_INST *Inst);
  const std::unordered_set<G4_INST *> &getEUFusionNoMaskWAInsts() {
    return EUFusionNoMaskWAInsts;
  }

public:
  std::unique_ptr<VerifyAugmentation> verifyAugmentation;
  std::unique_ptr<RegChartDump> regChart;
  std::unique_ptr<SpillAnalysis> spillAnalysis;
  static bool useGenericAugAlign(PlatformGen gen) {
    if (gen == PlatformGen::GEN9 || gen == PlatformGen::GEN8)
      return false;
    return true;
  }
  static const char StackCallStr[];
  // The pre assigned forbidden register bits for different kinds
  ForbiddenRegs fbdRegs;

private:
  template <class REGION_TYPE>
  static unsigned getRegionDisp(REGION_TYPE *region, const IR_Builder &irb);
  unsigned getRegionByteSize(G4_DstRegRegion *region, unsigned execSize);
  static bool owordAligned(unsigned offset) { return offset % 16 == 0; }
  template <class REGION_TYPE>
  bool isUnalignedRegion(REGION_TYPE *region, unsigned execSize);
  bool shouldPreloadDst(G4_INST *instContext, G4_BB *curBB);
  bool livenessCandidate(const G4_Declare *decl) const;
  void updateDefSet(std::set<G4_Declare *> &defs, G4_Declare *referencedDcl);
  void detectUndefinedUses(LivenessAnalysis &liveAnalysis, G4_Kernel &kernel);
  void markBlockLocalVar(G4_RegVar *var, unsigned bbId);
  void markBlockLocalVars();
  void computePhyReg();
  void fixAlignment();
  // Updates `slot1SetR0` and `slot1ResetR0` with hword spill/fill instructions
  // that need to update r0.5 to address slot 1 scratch space.
  void markSlot1HwordSpillFill(G4_BB *);
  void expandSpillIntrinsic(G4_BB *);
  void expandFillIntrinsic(G4_BB *);
  void expandSpillFillIntrinsics(unsigned);
  void saveRestoreA0(G4_BB *);
  static const RAVarInfo defaultValues;
  std::vector<RAVarInfo> vars;
  std::vector<AugmentationMasks> varMasks;
  std::vector<G4_Declare *> UndeclaredVars;

  // fake declares for each GRF reg, used by HRA
  // note only GRFs that are used by LRA get a declare
  std::vector<G4_Declare *> GRFDclsForHRA;

  // Store all LocalLiveRange instances created so they're
  // appropriately destroyed alongwith instance of GlobalRA.
  // This needs to be a list because we'll take address of
  // its elements and std::vector cannot be used due to its
  // reallocation policy.
  std::list<LocalLiveRange> localLiveRanges;

  std::unordered_map<const G4_BB *, unsigned> subretloc;
  // map ret location to declare for call/ret
  std::map<uint32_t, G4_Declare *> retDecls;

  // store instructions that shouldnt be rematerialized.
  std::unordered_set<G4_INST *> dontRemat;


  // map each BB to its local RA GRF usage summary, populated in local RA.
  std::map<G4_BB *, PhyRegSummary *> bbLocalRAMap;
  llvm::SpecificBumpPtrAllocator<PhyRegSummary> PRSAlloc;

  RAVarInfo &allocVar(const G4_Declare *dcl) {
    auto dclid = dcl->getDeclId();
    if (dclid >= vars.size())
      vars.resize(dclid + 1);
    return vars[dclid];
  }

  const RAVarInfo &getVar(const G4_Declare *dcl) const {
    // It's assumed that dcl has already been added to vars vector. To add newly
    // created RA variables to the vector pre-RA, addVarToRA() can be used.
    auto dclid = dcl->getDeclId();
    return vars[dclid];
  }

  // temp variable storing the FP dcl's old value
  // created in addStoreRestoreForFP
  G4_Declare *oldFPDcl = nullptr;

  // instruction to save/restore vISA FP, only present in functions
  G4_INST *saveBE_FPInst = nullptr;
  G4_INST *restoreBE_FPInst = nullptr;

  // instruction go update BE_FP, BE_SP, only present in functions
  G4_INST *setupBE_FP = nullptr;
  G4_INST *setupBE_SP = nullptr;

  // new temps for each reference of spilled address/flag decls
  std::unordered_set<G4_Declare *> addrFlagSpillDcls;

  // track spill/fill code in basic blocks
  std::unordered_set<G4_BB *> BBsWithSpillCode;

  // store iteration number for GRA loop
  unsigned iterNo = 0;

  uint32_t numGRFSpill = 0;
  uint32_t numGRFFill = 0;

  unsigned int numReservedGRFsFailSafe = BoundedRA::NOT_FOUND;

  // For hword scratch messages, when using separate scratch space for spills,
  // r0.5 needs to be updated before spill/fill to point to slot 1 space.
  // These maps mark which spills/fills need to set/reset r0.5.
  std::unordered_set<G4_INST *> slot1SetR0;
  std::unordered_set<G4_INST *> slot1ResetR0;

  void insertSlot1HwordR0Set(G4_BB *bb, INST_LIST_ITER &instIt);
  void insertSlot1HwordR0Reset(G4_BB *bb, INST_LIST_ITER &instIt);

  bool spillFillIntrinUsesLSC(G4_INST *spillFillIntrin);
  void expandFillLSC(G4_BB *bb, INST_LIST_ITER &instIt);
  void expandSpillLSC(G4_BB *bb, INST_LIST_ITER &instIt);
  void expandScatterSpillLSC(G4_BB *bb, INST_LIST_ITER &instIt);
  void expandFillNonStackcall(uint32_t numRows, uint32_t offset,
                              short rowOffset, G4_SrcRegRegion *header,
                              G4_DstRegRegion *resultRgn, G4_BB *bb,
                              INST_LIST_ITER &instIt);
  void expandSpillNonStackcall(uint32_t numRows, uint32_t offset,
                               short rowOffset, G4_SrcRegRegion *header,
                               G4_SrcRegRegion *payload, G4_BB *bb,
                               INST_LIST_ITER &instIt);
  void expandFillStackcall(uint32_t numRows, uint32_t offset, short rowOffset,
                           G4_SrcRegRegion *header, G4_DstRegRegion *resultRgn,
                           G4_BB *bb, INST_LIST_ITER &instIt);
  void expandSpillStackcall(uint32_t numRows, uint32_t offset, short rowOffset,
                            G4_SrcRegRegion *payload, G4_BB *bb,
                            INST_LIST_ITER &instIt);
  bool stopAfter(const char *subpass) const {
    auto passName = builder.getOptions()->getOptionCstr(vISA_StopAfterPass);
    return passName && strcmp(passName, subpass) == 0;
  }

public:
  static unsigned sendBlockSizeCode(unsigned owordSize);

  // For current program, store caller/callee save/restore instructions
  std::unordered_set<G4_INST *> calleeSaveInsts;
  std::unordered_set<G4_INST *> calleeRestoreInsts;
  std::unordered_map<G4_INST *, std::unordered_set<G4_INST *>> callerSaveInsts;
  std::unordered_map<G4_INST *, std::unordered_set<G4_INST *>>
      callerRestoreInsts;
  std::unordered_map<G4_BB *, std::vector<bool>> callerSaveRegsMap;
  std::unordered_map<G4_BB *, unsigned> callerSaveRegCountMap;
  std::unordered_map<G4_BB *, std::vector<bool>> retRegsMap;
  std::vector<bool> calleeSaveRegs;
  unsigned calleeSaveRegCount = 0;

  std::unordered_map<G4_Declare *, SplitResults> splitResults;

  G4_Kernel &kernel;
  IR_Builder &builder;
  PhyRegPool &regPool;
  PointsToAnalysis &pointsToAnalysis;
  FCALL_RET_MAP fcallRetMap;

  bool useLscForSpillFill = false;
  bool useLscForScatterSpill = false;
  bool useLscForNonStackCallSpillFill = false;
  bool useFastRA = false;
  bool useHybridRAwithSpill = false;
  bool useLocalRA = false;

  IncrementalRA incRA;


  bool avoidBundleConflict = false;
  VarSplitPass *getVarSplitPass() const { return kernel.getVarSplitPass(); }

  unsigned getSubRetLoc(const G4_BB *bb) {
    auto it = subretloc.find(bb);
    if (it == subretloc.end())
      return UNDEFINED_VAL;
    return it->second;
  }

  void setSubRetLoc(const G4_BB *bb, unsigned s) { subretloc[bb] = s; }

  bool isSubRetLocConflict(G4_BB *bb, std::vector<unsigned> &usedLoc,
                           unsigned stackTop);
  void assignLocForReturnAddr();
  unsigned determineReturnAddrLoc(unsigned entryId,
                                  std::vector<unsigned> &retLoc, G4_BB *bb);
  void insertCallReturnVar();
  void insertSaveAddr(G4_BB *);
  void insertRestoreAddr(G4_BB *);
  void setIterNo(unsigned i) { iterNo = i; }
  unsigned getIterNo() const { return iterNo; }
  void fixSrc0IndirFcall();

  G4_Declare *getRetDecl(uint32_t retLoc) {
    auto result = retDecls.find(retLoc);
    if (result != retDecls.end()) {
      return result->second;
    }

    const char *name = builder.getNameString(24, "RET__loc%d", retLoc);
    G4_Declare *dcl = builder.createDeclare(name, G4_GRF, 2, 1, Type_UD);

    // call destination must still be QWord aligned
    dcl->setSubRegAlign(Four_Word);
    setSubRegAlign(dcl, Four_Word);

    retDecls[retLoc] = dcl;
    return dcl;
  }

  G4_INST *getSaveBE_FPInst() const { return saveBE_FPInst; };
  G4_INST *getRestoreBE_FPInst() const { return restoreBE_FPInst; };

  static unsigned owordToGRFSize(unsigned numOwords, const IR_Builder &builder);
  static unsigned hwordToGRFSize(unsigned numHwords, const IR_Builder &builder);
  static unsigned GRFToHwordSize(unsigned numGRFs, const IR_Builder &builder);
  static unsigned GRFSizeToOwords(unsigned numGRFs, const IR_Builder &builder);
  static unsigned getHWordByteSize();

  // RA specific fields
  G4_Declare *getGRFDclForHRA(int GRFNum) const {
    return GRFDclsForHRA[GRFNum];
  }

  G4_Declare *getOldFPDcl() const { return oldFPDcl; }

  bool isAddrFlagSpillDcl(G4_Declare *dcl) const {
    return addrFlagSpillDcls.count(dcl) != 0;
  }

  void addAddrFlagSpillDcl(G4_Declare *dcl) { addrFlagSpillDcls.insert(dcl); }

  bool hasSpillCodeInBB(G4_BB *bb) const {
    return BBsWithSpillCode.find(bb) != BBsWithSpillCode.end();
  }

  void addSpillCodeInBB(G4_BB *bb) { BBsWithSpillCode.insert(bb); }

  void addUndefinedDcl(G4_Declare *dcl) { UndeclaredVars.push_back(dcl); }

  bool isUndefinedDcl(const G4_Declare *dcl) const {
    return std::find(UndeclaredVars.begin(), UndeclaredVars.end(), dcl) !=
           UndeclaredVars.end();
  }

  RAVarInfo &addVarToRA(const G4_Declare *dcl) { return allocVar(dcl); }

  unsigned getSplitVarNum(const G4_Declare *dcl) const {
    return getVar(dcl).numSplit;
  }

  void setSplitVarNum(const G4_Declare *dcl, unsigned val) {
    allocVar(dcl).numSplit = val;
  }

  unsigned getBBId(const G4_Declare *dcl) const { return getVar(dcl).bb_id; }

  void setBBId(const G4_Declare *dcl, unsigned id) { allocVar(dcl).bb_id = id; }

  bool isBlockLocal(const G4_Declare *dcl) const {
    return getBBId(dcl) < (UINT_MAX - 1);
  }

  G4_Declare *getSplittedDeclare(const G4_Declare *dcl) const {
    return getVar(dcl).splittedDCL;
  }

  void setSplittedDeclare(const G4_Declare *dcl, G4_Declare *sd) {
    allocVar(dcl).splittedDCL = sd;
  }

  LocalLiveRange *getLocalLR(const G4_Declare *dcl) const {
    return getVar(dcl).localLR;
  }

  void setLocalLR(G4_Declare *dcl, LocalLiveRange *lr) {
    RAVarInfo &var = allocVar(dcl);
    vISA_ASSERT(var.localLR == NULL,
                "Local live range already allocated for declaration");
    var.localLR = lr;
    lr->setTopDcl(dcl);
  }

  LSLiveRange *getSafeLSLR(const G4_Declare *dcl) const {
    auto dclid = dcl->getDeclId();
    if (dclid < vars.size()) {
      return vars[dclid].LSLR;
    } else {
      return nullptr;
    }
  }

  LSLiveRange *getLSLR(const G4_Declare *dcl) const { return getVar(dcl).LSLR; }

  void setLSLR(G4_Declare *dcl, LSLiveRange *lr) {
    RAVarInfo &var = allocVar(dcl);
    vISA_ASSERT(var.LSLR == NULL,
                "Local live range already allocated for declaration");
    var.LSLR = lr;
    lr->setTopDcl(dcl);
  }

  void resetLSLR(const G4_Declare *dcl) { allocVar(dcl).LSLR = nullptr; }

  void resetLocalLR(const G4_Declare *dcl) { allocVar(dcl).localLR = nullptr; }

  void clearStaleLiveRanges() {
    for (auto dcl : kernel.Declares) {
      setBBId(dcl, UINT_MAX);
      resetLocalLR(dcl);
    }
  }

  void clearLocalLiveRanges() {
    for (auto dcl : kernel.Declares) {
      resetLocalLR(dcl);
    }
  }

  void recordRef(const G4_Declare *dcl) { allocVar(dcl).numRefs++; }

  unsigned getNumRefs(const G4_Declare *dcl) const {
    return getVar(dcl).numRefs;
  }

  void setNumRefs(const G4_Declare *dcl, unsigned refs) {
    allocVar(dcl).numRefs = refs;
  }

  BankConflict getBankConflict(const G4_Declare *dcl) const {
    return getVar(dcl).conflict;
  }

  void setBankConflict(const G4_Declare *dcl, BankConflict c) {
    allocVar(dcl).conflict = c;
  }

  G4_INST *getStartInterval(const G4_Declare *dcl) const {
    return getVar(dcl).startInterval;
  }

  void setStartInterval(const G4_Declare *dcl, G4_INST *inst) {
    allocVar(dcl).startInterval = inst;
  }

  G4_INST *getEndInterval(const G4_Declare *dcl) const {
    return getVar(dcl).endInterval;
  }

  void setEndInterval(const G4_Declare *dcl, G4_INST *inst) {
    allocVar(dcl).endInterval = inst;
  }

  const std::vector<unsigned char> &getMask(const G4_Declare *dcl) const {
    return getVar(dcl).mask;
  }

  void setMask(const G4_Declare *dcl, std::vector<unsigned char> m) {
    allocVar(dcl).mask = m;
  }

  AugmentationMasks getAugmentationMask(const G4_Declare *dcl) const {
    auto dclid = dcl->getDeclId();
    if (dclid >= varMasks.size()) {
      return AugmentationMasks::Undetermined;
    }
    return varMasks[dclid];
  }

  void setAugmentationMask(const G4_Declare *dcl, AugmentationMasks m) {
    auto dclid = dcl->getDeclId();
    if (dclid >= varMasks.size())
      varMasks.resize(dclid + 1);
    varMasks[dclid] = m;
    if (dcl->getIsSplittedDcl()) {
      for (const G4_Declare *subDcl : getSubDclList(dcl)) {
        setAugmentationMask(subDcl, m);
      }
    }
  }

  bool getHasNonDefaultMaskDef(const G4_Declare *dcl) const {
    return (getAugmentationMask(dcl) == AugmentationMasks::NonDefault);
  }

  void addBundleConflictDcl(const G4_Declare *dcl, const G4_Declare *subDcl,
                            int offset) {
    allocVar(dcl).bundleConflicts.emplace_back(subDcl, offset);
  }

  void clearBundleConflictDcl(const G4_Declare *dcl) {
    allocVar(dcl).bundleConflicts.clear();
  }

  const std::vector<BundleConflict> &
  getBundleConflicts(const G4_Declare *dcl) const {
    return getVar(dcl).bundleConflicts;
  }

  unsigned get_bundle(unsigned baseReg, int offset) const {
    if (builder.hasPartialInt64Support()) {
      return (((baseReg + offset) % 32) / 2);
    }
    return (((baseReg + offset) % 64) / 4);
  }

  unsigned get_bank(unsigned baseReg, int offset) {
    int bankID = (baseReg + offset) % 2;

    if (builder.hasTwoGRFBank16Bundles()) {
      bankID = ((baseReg + offset) % 4) / 2;
    }


    if (builder.hasOneGRFBank16Bundles()) {
      bankID = (baseReg + offset) % 2;
    }

    return bankID;
  }

  void addSubDcl(const G4_Declare *dcl, G4_Declare *subDcl) {
    allocVar(dcl).subDclList.push_back(subDcl);
  }

  void clearSubDcl(const G4_Declare *dcl) { allocVar(dcl).subDclList.clear(); }

  const std::vector<const G4_Declare *> &
  getSubDclList(const G4_Declare *dcl) const {
    return getVar(dcl).subDclList;
  }

  unsigned getSubOffset(const G4_Declare *dcl) const {
    return getVar(dcl).subOff;
  }

  void setSubOffset(const G4_Declare *dcl, unsigned offset) {
    allocVar(dcl).subOff = offset;
  }

  G4_SubReg_Align getSubRegAlign(const G4_Declare *dcl) const {
    return getVar(dcl).subAlign;
  }

  void setSubRegAlign(const G4_Declare *dcl, G4_SubReg_Align subAlg) {
    auto &subAlign = allocVar(dcl).subAlign;
    // sub reg alignment can only be more restricted than prior setting
    vISA_ASSERT(subAlign == Any || subAlign == subAlg || subAlign % 2 == 0,
                ERROR_UNKNOWN);
    if (subAlign > subAlg) {
      vISA_ASSERT(subAlign % subAlg == 0, "Sub reg alignment conflict");
      // do nothing; keep the original alignment (more restricted)
    } else {
      vISA_ASSERT(subAlg % subAlign == 0, "Sub reg alignment conflict");
      subAlign = subAlg;
    }
  }

  bool hasAlignSetup(const G4_Declare *dcl) const {
    if (getVar(dcl).subAlign == G4_SubReg_Align::Any &&
        dcl->getSubRegAlign() != G4_SubReg_Align::Any)
      return false;
    return true;
  }

  bool isEvenAligned(const G4_Declare *dcl) const {
    return getVar(dcl).isEvenAlign;
  }

  void setEvenAligned(const G4_Declare *dcl, bool e) {
    allocVar(dcl).isEvenAlign = e;
  }

  BankAlign getBankAlign(const G4_Declare *) const;
  bool areAllDefsNoMask(G4_Declare *);
  void removeUnreferencedDcls();
  LocalLiveRange *GetOrCreateLocalLiveRange(G4_Declare *topdcl);

  GlobalRA(G4_Kernel &k, PhyRegPool &r, PointsToAnalysis &p2a)
      : kernel(k), builder(*k.fg.builder), regPool(r), pointsToAnalysis(p2a),
        fbdRegs(*k.fg.builder), incRA(*this) {
    vars.resize(k.Declares.size());
    varMasks.resize(k.Declares.size());

    if (kernel.getOptions()->getOption(vISA_VerifyAugmentation)) {
      verifyAugmentation = std::make_unique<VerifyAugmentation>();
    }

    // Set callWA condition.
    //    Call return ip and mask need wa only for non-entry functions. As call
    //    WA also needs a temp, we conservatively add WA for
    //    caller-save/callee-save code too, which applies to all functions,
    //    including the entry function.
    m_EUFusionCallWANeeded =
        builder.hasFusedEU() &&
        builder.getuint32Option(vISA_fusedCallWA) == 1 &&
        (kernel.fg.getHasStackCalls() || kernel.hasIndirectCall());
  }

  void emitFGWithLiveness(const LivenessAnalysis &liveAnalysis) const;
  void reportSpillInfo(const LivenessAnalysis &liveness,
                       const GraphColor &coloring) const;
  static uint32_t getRefCount(int loopNestLevel);
  void updateSubRegAlignment(G4_SubReg_Align subAlign);
  bool isChannelSliced();
  void evenAlign();
  bool evenAlignNeeded(G4_Declare *);
  void getBankAlignment(LiveRange *lr, BankAlign &align);
  void printLiveIntervals();
  void reportUndefinedUses(LivenessAnalysis &liveAnalysis, G4_BB *bb,
                           G4_INST *inst, G4_Declare *referencedDcl,
                           std::set<G4_Declare *> &defs,
                           Gen4_Operand_Number opndNum);
  void detectNeverDefinedUses();

  void determineSpillRegSize(unsigned &spillRegSize,
                             unsigned &indrSpillRegSize);
  G4_Imm *createMsgDesc(unsigned owordSize, bool writeType, bool isSplitSend);
  void stackCallProlog();
  void saveRegs(unsigned startReg, unsigned owordSize,
                G4_Declare *scratchRegDcl, G4_Declare *framePtr,
                unsigned frameOwordOffset, G4_BB *bb, INST_LIST_ITER insertIt,
                std::unordered_set<G4_INST *> &group);
  void saveActiveRegs(std::vector<bool> &saveRegs, unsigned startReg,
                      unsigned frameOffset, G4_BB *bb, INST_LIST_ITER insertIt,
                      std::unordered_set<G4_INST *> &group);
  void addrRegAlloc();
  void flagRegAlloc();
  void fastRADecision();
  bool tryHybridRA();
  bool hybridRA(LocalRA &lra);
  void assignRegForAliasDcl();
  void removeSplitDecl();
  std::pair<unsigned, unsigned> reserveGRFSpillReg(GraphColor &coloring);
  void generateForbiddenTemplates(unsigned reserveSpillSize);

  BitSet *getForbiddenRegs(forbiddenKind type) {
    return fbdRegs.getForbiddenRegs(type);
  }

  unsigned getForbiddenVectorSize(G4_RegFileKind regKind) {
    return fbdRegs.getForbiddenVectorSize(regKind);
  }

  int coloringRegAlloc();
  void restoreRegs(unsigned startReg, unsigned owordSize,
                   G4_Declare *scratchRegDcl, G4_Declare *framePtr,
                   unsigned frameOwordOffset, G4_BB *bb,
                   INST_LIST_ITER insertIt,
                   std::unordered_set<G4_INST *> &group, bool caller);
  void restoreActiveRegs(std::vector<bool> &restoreRegs, unsigned startReg,
                         unsigned frameOffset, G4_BB *bb,
                         INST_LIST_ITER insertIt,
                         std::unordered_set<G4_INST *> &group, bool caller);
  void OptimizeActiveRegsFootprint(std::vector<bool> &saveRegs);
  void OptimizeActiveRegsFootprint(std::vector<bool> &saveRegs,
                                   std::vector<bool> &retRegs);
  void addCallerSaveRestoreCode();
  void addCalleeSaveRestoreCode();
  void addGenxMainStackSetupCode();
  void addCalleeStackSetupCode();
  void addSaveRestoreCode(unsigned localSpillAreaOwordSize);
  void addCallerSavePseudoCode();
  void addCalleeSavePseudoCode();
  void addStoreRestoreToReturn();
  void markGraphBlockLocalVars();
  void verifyRA(LivenessAnalysis &liveAnalysis);
  void verifySpillFill();
  void resetGlobalRAStates();

  void insertPhyRegDecls();

  void copyMissingAlignment() {
    // Insert alignment for vars created in RA
    for (auto dcl : kernel.Declares) {
      if (dcl->getAliasDeclare())
        continue;

      if (dcl->getDeclId() >= vars.size()) {
        allocVar(dcl);
      }
      if (!hasAlignSetup(dcl)) {
        // Var may be temp created in RA
        setSubRegAlign(dcl, dcl->getSubRegAlign());
        setEvenAligned(dcl, dcl->isEvenAlign());
      }
    }
  }

  void copyAlignment(G4_Declare *dst, G4_Declare *src) {
    setEvenAligned(dst, isEvenAligned(src));
    setSubRegAlign(dst, getSubRegAlign(src));
  }

  void copyAlignment() {
    for (auto dcl : kernel.Declares) {
      if (dcl->getAliasDeclare())
        continue;

      setSubRegAlign(dcl, dcl->getSubRegAlign());
      setEvenAligned(dcl, dcl->isEvenAlign());
    }
  }

  bool isNoRemat(G4_INST *inst) {
    return dontRemat.find(inst) != dontRemat.end();
  }

  void addNoRemat(G4_INST *inst) { dontRemat.insert(inst); }

  unsigned int getNumReservedGRFs() {
    // Return # GRFs reserved for new fail safe mechanism
    // 1. If fail safe mechanism is invoked before coloring then
    //    # reserved GRFs is updated explicitly before this method
    //    is invoked.
    // 2. If a regular (ie, non-fail safe) RA iteration spill
    //    very little then we may convert it to fail safe but with
    //    0 reserved GRFs as it it too late to reserve a GRF after
    //    coloring.
    if (numReservedGRFsFailSafe == BoundedRA::NOT_FOUND)
      numReservedGRFsFailSafe =
          kernel.getSimdSize() == kernel.numEltPerGRF<Type_UD>() ? 1 : 2;

    return numReservedGRFsFailSafe;
  }

  void setNumReservedGRFsFailSafe(unsigned int num) {
    numReservedGRFsFailSafe = num;
  }

  PhyRegSummary *createPhyRegSummary() {
    auto PRSMem = PRSAlloc.Allocate();
    return new (PRSMem) PhyRegSummary(&builder, kernel.getNumRegTotal());
  }

  void addBBLRASummary(G4_BB *bb, PhyRegSummary *summary) {
    bbLocalRAMap.insert(std::make_pair(bb, summary));
  }

  void clearBBLRASummaries() { bbLocalRAMap.clear(); }

  PhyRegSummary *getBBLRASummary(G4_BB *bb) const {
    auto &&iter = bbLocalRAMap.find(bb);
    return iter != bbLocalRAMap.end() ? iter->second : nullptr;
  }

public:
  // Store new variables created when inserting scalar imm
  // spill/fill code. Such variables are not infinite spill
  // cost. So if the variable spills again, we shouldn't
  // get in an infinite loop by retrying same spill/fill.
  std::unordered_set<G4_Declare *> scalarSpills;
};

inline G4_Declare *Interference::getGRFDclForHRA(int GRFNum) const {
  return gra.getGRFDclForHRA(GRFNum);
}

class VarSplit {
private:
  G4_Kernel &kernel;
  GlobalRA &gra;

  VarRange *splitVarRange(VarRange *src1, VarRange *src2,
                          std::stack<VarRange *> *toDelete);
  void rangeListSpliting(VAR_RANGE_LIST *rangeList, G4_Operand *opnd,
                         std::stack<VarRange *> *toDelete);
  void getHeightWidth(G4_Type type, unsigned numberElements,
                      unsigned short &dclWidth, unsigned short &dclHeight,
                      int &totalByteSize) const;
  void createSubDcls(G4_Kernel &kernel, G4_Declare *oldDcl,
                     std::vector<G4_Declare *> &splitDclList);
  void insertMovesToTemp(IR_Builder &builder, G4_Declare *oldDcl,
                         G4_Operand *dstOpnd, G4_BB *bb,
                         INST_LIST_ITER instIter,
                         std::vector<G4_Declare *> &splitDclList);
  void insertMovesFromTemp(G4_Kernel &kernel, G4_Declare *oldDcl, int index,
                           G4_Operand *srcOpnd, int pos, G4_BB *bb,
                           INST_LIST_ITER instIter,
                           std::vector<G4_Declare *> &splitDclList);

public:
  bool didLocalSplit = false;
  bool didGlobalSplit = false;

  void localSplit(IR_Builder &builder, G4_BB *bb);
  void globalSplit(IR_Builder &builder, G4_Kernel &kernel);
  bool canDoGlobalSplit(IR_Builder &builder, G4_Kernel &kernel,
                        uint32_t sendSpillRefCount);

  VarSplit(GlobalRA &g) : kernel(g.kernel), gra(g) {}
};

class DynPerfModel {
private:
  std::string Buffer;

public:
  G4_Kernel &Kernel;
  unsigned int NumSpills = 0;
  unsigned int NumFills = 0;
  unsigned int NumRAIters = 0;
  unsigned long long TotalDynInst = 0;
  unsigned long long FillDynInst = 0;
  unsigned long long SpillDynInst = 0;
  // vector item at index i corresponds to nesting level i
  // #Loops at this nesting level, #Spills, #Fills
  std::vector<std::tuple<unsigned int, unsigned int, unsigned int>>
      SpillFillPerNestingLevel;

  DynPerfModel(G4_Kernel &K) : Kernel(K) {}

  void run();
  void dump();
};
} // namespace vISA

#endif // __GRAPHCOLOR_H__
