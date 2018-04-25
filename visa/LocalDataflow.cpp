#include "FlowGraph.h"
#include "BitSet.h"
#include "BuildIR.h"
#include <algorithm>
#include <unordered_map>
#include <vector>

using namespace vISA;
using namespace std;

namespace {

// Given an operand, turn its bitvec representation into bitmask one.
//
void getOpndFootprint(G4_Operand* opnd, BitSet& footprint)
{
    unsigned N = NUM_BITS_PER_ELT;
    unsigned lb = opnd->getLeftBound();
    unsigned rb = opnd->getRightBound();
    if (lb % N == 0 && rb - lb + 1 <= 64) {
        unsigned idx = lb / N;
        footprint.setElt(idx, opnd->getBitVecL());
        if (rb - lb + 1 > 32)
            footprint.setElt(idx + 1, opnd->getBitVecH());
    } else if (lb % N == 0 && (rb + 1) % N == 0) {
        unsigned idx = lb / N;
        unsigned endIdx = rb / N;
        unsigned bitvec = opnd->getBitVecL();
        footprint.setElt(idx, bitvec);

        idx++;
        if (idx <= endIdx) {
            bitvec = opnd->getBitVecH();
            footprint.setElt(idx, bitvec);
        }

        idx++;
        while (idx <= endIdx) {
            footprint.setElt(idx, 0xFFFFFFFF);
            idx++;
        }
    } else {
        uint64_t mask = opnd->getBitVecL() + (uint64_t(opnd->getBitVecH()) << 32);
        unsigned j = lb;
        for (unsigned i = 0; i <= std::min(rb, 63U); ++i) {
            if (mask & ((uint64_t)1 << i))
                footprint.set(j, true);
            j++;
        }
        while (j++ <= rb)
            footprint.set(j, true);
    }
}

// Combine footprint.
//
// Use DefOnpd's foot print to kill the footprint being defined.
//
void combineFootprint(G4_Operand* DefOpnd, BitSet& footprint)
{
    unsigned N = NUM_BITS_PER_ELT;
    unsigned lb = DefOpnd->getLeftBound();
    unsigned rb = DefOpnd->getRightBound();
    if (lb % N == 0 && rb - lb + 1 <= 64) {
        unsigned idx = lb / N;
        footprint.resetElt(idx, DefOpnd->getBitVecL());
        if (rb - lb + 1 > 32)
            footprint.resetElt(idx + 1, DefOpnd->getBitVecH());
    } else if (lb % N == 0 && (rb + 1) % N == 0) {
        unsigned idx = lb / N;
        unsigned endIdx = rb / N;
        unsigned bitvec = DefOpnd->getBitVecL();
        footprint.resetElt(idx, bitvec);

        idx++;
        if (idx <= endIdx) {
            bitvec = DefOpnd->getBitVecH();
            footprint.resetElt(idx, bitvec);
        }

        idx++;
        while (idx <= endIdx) {
            footprint.resetElt(idx, 0xFFFFFFFF);
            idx++;
        }
    } else {
        uint64_t mask = DefOpnd->getBitVecL() + (uint64_t(DefOpnd->getBitVecH()) << 32);
        unsigned j = lb;
        for (unsigned i = 0; i <= std::min(rb, 63U); ++i) {
            if (mask & ((uint64_t)1 << i))
                footprint.set(j, false);
            j++;
        }
        while (j++ <= rb)
            footprint.set(j, false);
    }
}

struct LocalLivenessInfo;

// This is the basic unit used to track live operands (not fully defined yet).
struct LiveNode {
    LiveNode(G4_INST* Inst, Gen4_Operand_Number OpNum)
        : Inst(Inst)
        , OpNum(OpNum)
        , mask(getMaskSize(Inst, OpNum), 0)
    {
        auto opnd = getOperand();
        if(opnd)
            getOpndFootprint(opnd, mask);
    }

    // The instruction being tracked.
    G4_INST* Inst;

    // This indicates which operand being tracked.
    Gen4_Operand_Number OpNum;

    // This tracks byte/bits level of opnd being partially defined.
    BitSet mask;

    // The list of definition nodes seen, sorted in a reversed order.
    //
    // This data structure models the following du/ud chain, and it proves
    // <I3, Src1> is a locally fully-defined operand.
    //
    // I1 : mov (8,  M1) V33(0,0)<1>:d V32(0,0)<1;1,0>:q
    // I2 : mov (8,  M1) V33(1,0)<1>:d V32(2,0)<1;1,0>:q
    // I3 : add (16, M1) V35<1>:d V34<1;1,0>:d V33(0,0)<1;1,0>:d
    //
    // LiveNode <I3, V33> will be partially defined by I2 or I1, and
    // I1 together with I2 fully defines this live node.
    //
    vector<std::pair<G4_INST *, Gen4_Operand_Number>> DefNodes;

    G4_Operand *getOperand() const
    {
        return Inst->getOperand(OpNum);
    }

    static unsigned getMaskSize(G4_INST *Inst, Gen4_Operand_Number OpNum)
    {
        G4_Operand *Opnd = Inst->getOperand(OpNum);
        assert(Opnd && "null opnd");

        if (Opnd)
        {
            G4_Declare *Dcl = Opnd->getTopDcl();
            if (Dcl == nullptr) {
                // There is no top declaration for this operand, so this is ARF.
                return 32;
            }
            return Dcl->getRegVar()->isFlag() ? Dcl->getNumberFlagElements()
                : Dcl->getByteSize();
        }

        return 0;
    }

    // Propocess a defintion to this live node. If this definition
    // together with existing definitions fully kill this use, return true
    // and add def-use links; return false otherwise.
    bool addDefinition(G4_INST* Inst, Gen4_Operand_Number opndNum,
                       LocalLivenessInfo &LLI);

    // Check if an operand depends on channel mask.
    static bool dependsOnChannelMask(bool IsInSimdFlow, G4_INST* Inst,
                                     Gen4_Operand_Number OpNum);

    // Check if def-use is 'aligned' with channel mask.
    static bool alignedWithChannelMask(G4_INST* DefInst, Gen4_Operand_Number DefOpNum,
                                       G4_INST *UseInst, Gen4_Operand_Number UseOpNum);

    friend void swap(LiveNode& a, LiveNode& b)
    {
        a.DefNodes.swap(b.DefNodes);
        std::swap(a.Inst, b.Inst);
        std::swap(a.OpNum, b.OpNum);
        a.mask.swap(b.mask);
    }
};

struct LocalLivenessInfo {
    // Keep live nodes while scanning the block.
    // Each declare is associated with a list of live nodes.
    std::unordered_map<const G4_Declare*, vector<LiveNode>> LiveNodes;

    // This indicates if this block is in simd-cf or not.
    bool IsInSimdFlow;

    // Default constructor.
    explicit LocalLivenessInfo(bool IsInSimdFlow)
        : IsInSimdFlow(IsInSimdFlow)
    {
    }

    // Populate global operands.
    void populateGlobals(GlobalOpndHashTable &globalOpndHT)
    {
        for (auto& Nodes : LiveNodes) {
            for (auto &LN : Nodes.second) {
                G4_Operand* Opnd = LN.Inst->getOperand(LN.OpNum);
                assert(Opnd && "null operand");

                // This is a temporal solution to allow optimizations
                // on partially defined local variables.
                //
                // TODO: use pseudo-kill to mark such variables.
                for (auto I = LN.DefNodes.rbegin(), E = LN.DefNodes.rend();
                     I != E; ++I)
                    I->first->addDefUse(LN.Inst, LN.OpNum);

                globalOpndHT.addGlobalOpnd(Opnd);
            }
            Nodes.second.clear();
        }
    }
};

} // namespace

static G4_CmpRelation compOpnd(G4_Operand* Opnd1, G4_Operand* Opnd2)
{
    if (Opnd1->isDstRegRegion())
        return Opnd1->asDstRegRegion()->compareOperand(Opnd2);
    else if (Opnd1->isCondMod())
        return Opnd1->asCondMod()->compareOperand(Opnd2);
    else if (Opnd1->isSrcRegRegion())
        return Opnd1->asSrcRegRegion()->compareOperand(Opnd2);
    else if (Opnd1->isPredicate())
        return Opnd1->asPredicate()->compareOperand(Opnd2);

    // all other cases, virtual call.
    return Opnd1->compareOperand(Opnd2);
}

bool LiveNode::addDefinition(G4_INST* DefInst, Gen4_Operand_Number DefOpNum,
                             LocalLivenessInfo &LLI)
{
    // This definition does not overlap with this live node.
    G4_Operand* DefOpnd = DefInst->getOperand(DefOpNum);
    G4_Operand* UseOpnd = getOperand();
    G4_CmpRelation Rel = compOpnd(DefOpnd, UseOpnd);
    if (Rel == G4_CmpRelation::Rel_disjoint)
        return false;

    // Check if this definition will be fully convered by a previous definition.
    // This checks a single definition only. It is not optimal, but should cover
    // common cases.
    for (auto &Node : DefNodes) {
        if (Node.second != DefOpNum)
            continue;
        G4_INST *PrevDefInst = Node.first;
        if (PrevDefInst->getPredicate() || DefInst->getPredicate())
            continue;
        if (PrevDefInst->getMaskOffset() != DefInst->getMaskOffset())
            continue;
        if (!PrevDefInst->isWriteEnableInst() && DefInst->isWriteEnableInst())
            continue;
        G4_Operand* PrevDef = PrevDefInst->getOperand(Node.second);
        G4_Operand* CurrDef = DefInst->getOperand(DefOpNum);
        assert((PrevDef != nullptr) && (CurrDef != nullptr) );
        G4_CmpRelation DefRel = compOpnd(PrevDef, CurrDef);
        if (DefRel == G4_CmpRelation::Rel_eq || DefRel == G4_CmpRelation::Rel_gt)
            return false;
    }

    // Determine to count this definition's footprint or not.
    auto CombineBitV = [=, &LLI]() {
        // If definition is predicated and it is not sel, then
        // this definition cannot count, unless use is predicated
        // by the same predicate value.
        if (G4_Predicate *DefPred = DefInst->getPredicate()) {
            if (DefInst->opcode() != G4_opcode::G4_sel) {
                // The following case is a full definition, when predicates
                // have the same value.
                // (+P1) mov (8, M1) V33(0,0)<1>:d 1
                // (+P1) add (8, M1) V34(0,0)<1>:d V33(0,0)<1;1,0>:d V32(0,0)<1;1,0>:d
                G4_Predicate *UsePred = this->Inst->getPredicate();
                if (UsePred == nullptr || !DefPred->samePredicate(*UsePred))
                    return false;

                // If UsePred is alive and has no definition, then UsePred should
                // have the same value as DefPred.
                G4_Declare *Dcl = UsePred->getTopDcl();
                auto Iter = LLI.LiveNodes.find(Dcl);
                if (Iter == LLI.LiveNodes.end())
                    return false;

                // Find this live node..
                auto NI = std::find_if(Iter->second.begin(), Iter->second.end(),
                                       [=](const LiveNode &LN) {
                                           return LN.Inst == this->Inst &&
                                                  LN.OpNum == this->OpNum;
                                       });

                // Not alive or alive but partially defined.
                if (NI == Iter->second.end() || !NI->DefNodes.empty())
                    return false;
            }
        }

        // If def does not depend on channel mask, then combine its footprint.
        if (!dependsOnChannelMask(LLI.IsInSimdFlow, DefInst, DefOpNum))
            return true;

        // Otherwise, if this use does not depend on channel mask, then do not combine.
        if (!dependsOnChannelMask(LLI.IsInSimdFlow, this->Inst, this->OpNum))
            return false;

        return alignedWithChannelMask(DefInst, DefOpNum, this->Inst, this->OpNum);
    };

    if (CombineBitV()) {
        G4_Operand* DefOpnd = DefInst->getOperand(DefOpNum);
        combineFootprint(DefOpnd, mask);
    }

    if (UseOpnd && mask.isEmpty(UseOpnd->getLeftBound(), UseOpnd->getRightBound())) {
        // Use reverse_iterator as analysis is bottom-up. This makes
        // early defs come first in the def-use lists.
        if (!DefInst->isPseudoKill())
            DefInst->addDefUse(this->Inst, this->OpNum);
        for (auto I = DefNodes.rbegin(), E = DefNodes.rend(); I != E; ++I)
            I->first->addDefUse(this->Inst, this->OpNum);
        return true;
    }

    // This live node is not yet fully killed.
    DefNodes.emplace_back(DefInst, DefOpNum);
    return false;
}

bool LiveNode::dependsOnChannelMask(bool IsInSimdFlow, G4_INST *Inst,
                                    Gen4_Operand_Number OpNum)
{
    if (!IsInSimdFlow || Inst->isWriteEnableInst())
        return false;

    // Otherwise.
    return true;
}

// Check if def-use is 'aligned' with channel mask.
//
// CM = 11110000 (M8)
//      00001111 (M0)
//
// mov (8, M0) V33(0,0)<2>:d 1   // defines V33.{0,2,4,6}
// mov (8, M8) V33(0,1)<2>:d 2   // defines V33.{9,11,13,15}
// add (16, M0) V34.0<1>:d V33.0<1;1,0>:d 3 // uses V33.{0,1,2,3,12,13,14,15}
//
// This is not aligned, and it is not a full kill.
//
// mov (8, M0) V33(0,0)<1>:d 1   // defines V33.{0,1,2,3}
// mov (8, M8) V33(1,0)<1>:d 2   // defines V33.{12,13,14,15}
// add (16, M0) V34.0<1>:d V33.0<1;1,0>:d 3 // uses V33.{0,1,2,3,12,13,14,15}
//
// This is aligned, and it is a full kill.
//
bool LiveNode::alignedWithChannelMask(
    G4_INST* DefInst, Gen4_Operand_Number DefOpNum,
    G4_INST* UseInst, Gen4_Operand_Number UseOpNum)
{
    G4_Operand* DefOpnd = DefInst->getOperand(DefOpNum);
    G4_Operand* UseOpnd = UseInst->getOperand(UseOpNum);
    unsigned DefLB = DefOpnd->getLeftBound();
    unsigned UseLB = UseOpnd->getLeftBound();
    unsigned DefMaskOffset = DefInst->getMaskOffset();
    unsigned UseMaskOffset = UseInst->getMaskOffset();

    // Flag is tracking at bit level.
    G4_Declare *Dcl = DefOpnd->getTopDcl();
    if (Dcl && Dcl->getRegFile() == G4_RegFileKind::G4_FLAG) {
        int DefOffset = int(DefLB - DefMaskOffset);
        int UseOffset = int(UseLB - UseMaskOffset);
        return DefOffset == UseOffset;
    }

    // Do not analyze sends.
    if (DefInst->isSend() || UseInst->isSend())
        return true;

    // Check that all uses are defined under the righ emask, if defined.
    unsigned DefStride = 1;
    if (DefOpnd->isDstRegRegion()) {
        DefStride = DefOpnd->asDstRegRegion()->getHorzStride();
    }

    bool IsContinguous = false;
    if (UseOpnd->isSrcRegRegion()) {
        G4_SrcRegRegion* UseReg = UseOpnd->asSrcRegRegion();
        RegionDesc* UseRegDesc = UseReg->getRegion();
        IsContinguous = UseRegDesc->isContiguous(UseInst->getExecSize());
    }

    unsigned DefTySz = G4_Type_Table[DefOpnd->getType()].byteSize;
    unsigned UseTySz = G4_Type_Table[UseOpnd->getType()].byteSize;

    // Common cases.
    if (DefStride == 1 && IsContinguous && DefTySz == UseTySz) {
        int DefOffset = int(DefLB - DefMaskOffset * DefTySz);
        int UseOffset = int(UseLB - UseMaskOffset * UseTySz);
        return DefOffset == UseOffset;
    }

    // Other cases.
    //
    // Initial value -1 means byte not defined, for [DefLB, DefRB].
    std::array<int, 64> DefByteMask;
    DefByteMask.fill(-1);

    // Set channel value for each defining byte.
    int Channel = DefInst->getMaskOffset();
    for (unsigned i = 0, n = DefInst->getExecSize(); i < n; ++i) {
        for (unsigned j = 0; j < DefTySz; ++j)
            DefByteMask[i * DefTySz * DefStride + j] = Channel;
        ++Channel;
    }

    // In general, enumerate elements of use region and for each byte
    // check if this byte is defined by a correct emask. Note that
    // it is ok that some bytes are not defined.
    //
    Channel = UseInst->getMaskOffset();
    if (UseOpnd->isSrcRegRegion()) {
        unsigned DefRB = DefOpnd->getRightBound();
        auto Desc = UseOpnd->asSrcRegRegion()->getRegion();
        int hs = Desc->isScalar() ? 1 : Desc->horzStride;
        int vs = Desc->isScalar() ? 0 : Desc->vertStride;
        int numRows = UseInst->getExecSize() / Desc->width;

        for (int i = 0; i < numRows; ++i) {
            for (int j = 0; j < Desc->width; ++j) {
                // Check this element's emask at byte level when defined.
                int eltOffset = i * vs * UseTySz + j * hs * UseTySz;
                if (UseLB + eltOffset >= DefLB &&
                    UseLB + eltOffset + UseTySz - 1 <= DefRB) {
                    int Offset = UseLB + eltOffset - DefLB;
                    for (unsigned k = 0; k < UseTySz; ++k) {
                        int Mask = DefByteMask[Offset + k];
                        if (Mask != -1 && Mask != Channel)
                            return false;
                    }
                }
                ++Channel;
            }
        }
    }

    return true;
}

// Remove one element from vector and return the iterator to the next element.
template <typename T, typename AllocatorTy>
typename std::vector<T, AllocatorTy>::iterator
kill(std::vector<T, AllocatorTy>& Elts,
     typename std::vector<T, AllocatorTy>::iterator Iter)
{
    assert(Iter != Elts.end());
    assert(!Elts.empty());
    if (&*Iter == &Elts.back()) {
        // This is the last element so the next one is none.
        Elts.pop_back();
        return Elts.end();
    }

    // Not the last one, swap with the tail and keep the iterator unchanged.
    std::swap(*Iter, Elts.back());
    Elts.pop_back();
    return Iter;
}

// Process reads. This simply creates a new live read node.
//
// Note that for an indirect operand, its top dcl is its address variable.
// This models def-use for the underlying address variable, not the
// variable being addressed. An indirect definition introduces a use too.
//
static void processReadOpnds(G4_BB *BB, G4_INST *Inst, LocalLivenessInfo &LLI)
{
    // (1) Indirect dst operand reads address.
    G4_DstRegRegion* Dst = Inst->getDst();
    if (Dst && Dst->isIndirect()) {
        G4_Declare* Dcl = Dst->getTopDcl();
        assert(Dcl && "out of sync");
        LLI.LiveNodes[Dcl].emplace_back(
            Inst, Gen4_Operand_Number::Opnd_dst);
    }

    // (2) Direct and indirect source operands.
    for (auto OpNum :
        { Gen4_Operand_Number::Opnd_src0, Gen4_Operand_Number::Opnd_src1,
          Gen4_Operand_Number::Opnd_src2, Gen4_Operand_Number::Opnd_src3,
          Gen4_Operand_Number::Opnd_pred, Gen4_Operand_Number::Opnd_implAccSrc }) {
        G4_Operand* opnd = Inst->getOperand(OpNum);
        if (opnd == nullptr || opnd->isImm() || opnd->isNullReg() || opnd->isLabel())
            continue;

        G4_Declare* Dcl = opnd->getTopDcl();
        LLI.LiveNodes[Dcl].emplace_back(Inst, OpNum);
    }
}

// Process writes. If this is a partial definition, then record this partial
// definition. When all partial definitions together define this live read node,
// it is killed and du/ud links are added.
//
static void processWriteOpnds(G4_BB *BB, G4_INST *Inst, LocalLivenessInfo &LLI)
{
    for (auto OpNum : { Gen4_Operand_Number::Opnd_dst,
                        Gen4_Operand_Number::Opnd_condMod,
                        Gen4_Operand_Number::Opnd_implAccDst }) {
        G4_Operand* opnd = Inst->getOperand(OpNum);
        if (opnd == nullptr || opnd->isNullReg())
            continue;

        // Do not try to kill uses with indirect definitions.
        if (opnd->isDstRegRegion() && opnd->asDstRegRegion()->isIndirect())
            continue;

        // Iterate all live nodes associated to the same declaration.
        auto& Nodes = LLI.LiveNodes[opnd->getTopDcl()];
        for (auto Iter = Nodes.begin(); Iter != Nodes.end(); /*empty*/) {
            LiveNode &liveNode = *Iter;
            if (liveNode.addDefinition(Inst, OpNum, LLI)) {
                Iter = kill(Nodes, Iter);
                continue;
            }
            ++Iter;
        }
    }
}

void FlowGraph::localDataFlowAnalysis()
{
    for (auto BB : BBs) {
        LocalLivenessInfo LLI(BB->isInSimdFlow());
        for (auto I = BB->rbegin(), E = BB->rend(); I != E; ++I) {
            G4_INST* Inst = *I;
            G4_opcode Op = Inst->opcode();
            if (Op == G4_opcode::G4_return || Op == G4_opcode::G4_label)
                continue;
            if (Inst->isOptBarrier()) {
                // Do not try to build def-use accross an optimization barrier,
                // and this effectively disables optimizations across it.
                LLI.populateGlobals(globalOpndHT);

                // A barrier does not kill, but may introduce uses.
                processReadOpnds(BB, Inst, LLI);
                continue;
            }
            processWriteOpnds(BB, Inst, LLI);
            processReadOpnds(BB, Inst, LLI);
        }

        // All left over live nodes are global.
        LLI.populateGlobals(globalOpndHT);

        // Sort use lists according to their local ids.
        // This matches the use list order produced by forward
        // reaching definition based analysis. It is better for
        // optimizations not to rely on this order.
        BB->resetLocalId();
        for (auto Inst : *BB) {
            if (Inst->use_size() > 1) {
                using Ty = std::pair<vISA::G4_INST *, Gen4_Operand_Number>;
                auto Cmp = [](const Ty &lhs, const Ty &rhs) -> bool {
                    int lhsID = lhs.first->getLocalId();
                    int rhsID = rhs.first->getLocalId();
                    if (lhsID < rhsID)
                        return true;
                    else if (lhsID > rhsID)
                        return false;
                    return lhs.second < rhs.second;
                };
                Inst->sortUses(Cmp);
            }
        }
    }
}
