//- llvm/Transform/Ocelot/StructuralAnalysis.h - Structural Analysis - *C++ -*-// 
// 
//                     The LLVM Compiler Infrastructure 
// 
// This file is distributed under the University of Illinois Open Source 
// License. See LICENSE.TXT for details. 
// 
//===----------------------------------------------------------------------===// 
// 
// This file defines the class of Structural Analysis which will return the 
// control tree and unstructured branches of a function 
// 
//===----------------------------------------------------------------------===// 
 
#ifndef LLVM_ANALYSIS_ASSIGNFALLTHROUGHEDGE_H 
#define LLVM_ANALYSIS_ASSIGNFALLTHROUGHEDGE_H

#include <ocelot/analysis/interface/Pass.h>
#include <ocelot/ir/interface/ControlFlowGraph.h>
#include <ocelot/ir/interface/PTXKernel.h>

#include <hydrazine/implementation/debug.h>

#include <set>
#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace ir {
  class PTXKernel;
}

namespace analysis {
  typedef struct NodeCFG {
    ir::ControlFlowGraph::iterator BB;  // Map to the corresponding ir::BasicBlock * if it is original
    std::set<struct NodeCFG *> predNode; // Predecessor of the node
    std::set<struct NodeCFG *> succNode; // Successor of the node
  } NodeCFGTy;

  typedef std::unordered_map<ir::ControlFlowGraph::iterator, NodeCFGTy *> BB2NodeCFGMapTy;
  typedef std::set<NodeCFGTy *> NodeCFGSetTy;
  typedef std::vector<NodeCFGTy *> NodeCFGVecTy;
  typedef std::pair<NodeCFGTy *, NodeCFGTy *> EdgeCFGTy;
  typedef std::vector<EdgeCFGTy> EdgeCFGVecTy;
  typedef std::map<NodeCFGTy *, bool> VisitMapCFGTy;

  class AssignFallThroughEdge {
  public:
    AssignFallThroughEdge(ir::PTXKernel *k) {_kernel = k;}

    void replaceWithDummyEdge();

    void assignEdges();

  private:
    ir::PTXKernel* _kernel;

    BB2NodeCFGMapTy BB2NodeMap;    

    NodeCFGSetTy N;

    int postMax, preMax, sortedMax;

    VisitMapCFGTy visit;
    
    std::unordered_set<ir::ControlFlowGraph::iterator> hasIncomingFallThroughNode;

    std::map<NodeCFGTy *, int> preTree, postTree, sortedVal;

    EdgeCFGVecTy backEdgeVec;

    NodeCFGVecTy sortedNodes;
 
    void buildSimpleCFG();

    void DFSPostorder(NodeCFGTy *x);

    void assignBackEdge();

    void topoSort();

    void findEntryNode(NodeCFGSetTy &S);

    NodeCFGTy *pickOneNode(NodeCFGSetTy &S);

    void adjustBraInst();
  };
}

#endif
