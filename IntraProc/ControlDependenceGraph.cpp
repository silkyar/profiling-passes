
//
//                      Static Program Analysis for LLVM
//
// This file is distributed under a Modified BSD License (see LICENSE.TXT).
//
//===----------------------------------------------------------------------===//
//
// This file defines the ControlDependenceGraph class, which allows fast and 
// efficient control dependence queries. It is based on Ferrante et al's "The 
// Program Dependence Graph and Its Use in Optimization."
//
//===----------------------------------------------------------------------===//

// DISCLAIMER
// The code for Control Dependency Graph been taken from Scott Moore's git repository
// https://github.com/thinkmoore/


#include "IntraProc/ControlDependenceGraph.h"

#include "llvm/Analysis/DOTGraphTraitsPass.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/CFG.h"

#include <iostream>
#include <fstream>

#include <deque>
#include <set>

using namespace llvm;
using namespace std;

namespace llvm {

	void ControlDependenceNode::addTrue(ControlDependenceNode *Child) {
		TrueChildren.insert(Child);
	}

	void ControlDependenceNode::addFalse(ControlDependenceNode *Child) {
		FalseChildren.insert(Child);
	}

	void ControlDependenceNode::addOther(ControlDependenceNode *Child) {
		OtherChildren.insert(Child);
	}

	void ControlDependenceNode::addParent(ControlDependenceNode *Parent) {
		assert(std::find(Parent->begin(), Parent->end(), this) != Parent->end()
				&& "Must be a child before adding the parent!");
		Parents.insert(Parent);
	}

	void ControlDependenceNode::removeTrue(ControlDependenceNode *Child) {
		node_iterator CN = TrueChildren.find(Child);
		if (CN != TrueChildren.end())
			TrueChildren.erase(CN);
	}

	void ControlDependenceNode::removeFalse(ControlDependenceNode *Child) {
		node_iterator CN = FalseChildren.find(Child);
		if (CN != FalseChildren.end())
			FalseChildren.erase(CN);
	}

	void ControlDependenceNode::removeOther(ControlDependenceNode *Child) {
		node_iterator CN = OtherChildren.find(Child);
		if (CN != OtherChildren.end())
			OtherChildren.erase(CN);
	}

	void ControlDependenceNode::removeParent(ControlDependenceNode *Parent) {
		node_iterator PN = Parents.find(Parent);
		if (PN != Parents.end())
			Parents.erase(PN);
	}

	const ControlDependenceNode *ControlDependenceNode::enclosingRegion() const {
		if (this->isRegion()) {
			return this;
		} else {
			assert(this->Parents.size() == 1);
			const ControlDependenceNode *region = *this->Parents.begin();
			assert(region->isRegion());
			return region;
		}
	}

	ControlDependenceNode::EdgeType
		ControlDependenceGraphBase::getEdgeType(const BasicBlock *A, const BasicBlock *B) {
			if (const BranchInst *b = dyn_cast<BranchInst>(A->getTerminator())) {
				if (b->isConditional()) {
					if (b->getSuccessor(0) == B) {
						return ControlDependenceNode::TRUE;
					} else if (b->getSuccessor(1) == B) {
						return ControlDependenceNode::FALSE;
					} else {
						assert(false && "Asking for edge type between unconnected basic blocks!");
					}
				}
			}
			return ControlDependenceNode::OTHER;
		}

	void ControlDependenceGraphBase::computeDependencies(Function &F, PostDominatorTree &pdt) {
		root = new ControlDependenceNode();
		nodes.insert(root);

		for (Function::iterator BB = F.begin(), E = F.end(); BB != E; ++BB) {
			ControlDependenceNode *bn = new ControlDependenceNode(BB);
			nodes.insert(bn);
			bbMap[BB] = bn;
		}

		for (Function::iterator BB = F.begin(), E = F.end(); BB != E; ++BB) {
			BasicBlock *A = BB;
			ControlDependenceNode *AN = bbMap[A];

			for (succ_iterator succ = succ_begin(A), end = succ_end(A); succ != end; ++succ) {
				BasicBlock *B = *succ;
				assert(A && B);
				if (A == B || !pdt.dominates(B,A)) {
					BasicBlock *L = pdt.findNearestCommonDominator(A,B);
					ControlDependenceNode::EdgeType type = ControlDependenceGraphBase::getEdgeType(A,B);
					if (A == L) {
						switch (type) {
							case ControlDependenceNode::TRUE:
								AN->addTrue(AN); break;
							case ControlDependenceNode::FALSE:
								AN->addFalse(AN); break;
							case ControlDependenceNode::OTHER:
								AN->addOther(AN); break;
						}
						AN->addParent(AN);
					}
					for (DomTreeNode *cur = pdt[B]; cur && cur != pdt[L]; cur = cur->getIDom()) {
						ControlDependenceNode *CN = bbMap[cur->getBlock()];
						switch (type) {
							case ControlDependenceNode::TRUE:
								AN->addTrue(CN); break;
							case ControlDependenceNode::FALSE:
								AN->addFalse(CN); break;
							case ControlDependenceNode::OTHER:
								AN->addOther(CN); break;
						}
						assert(CN);
						CN->addParent(AN);
					}
				}
			}
		}

		// ENTRY -> START
		for (DomTreeNode *cur = pdt[&F.getEntryBlock()]; cur; cur = cur->getIDom()) {
			if (cur->getBlock()) {
				ControlDependenceNode *CN = bbMap[cur->getBlock()];
				assert(CN);
				root->addOther(CN); CN->addParent(root);
			}
		}
	}

	void ControlDependenceGraphBase::insertRegions(PostDominatorTree &pdt) {
		typedef po_iterator<PostDominatorTree*> po_pdt_iterator;  
		typedef std::pair<ControlDependenceNode::EdgeType, ControlDependenceNode *> cd_type;
		typedef std::set<cd_type> cd_set_type;
		typedef std::map<cd_set_type, ControlDependenceNode *> cd_map_type;

		cd_map_type cdMap;
		cd_set_type initCDs;
		initCDs.insert(std::make_pair(ControlDependenceNode::OTHER, root));
		cdMap.insert(std::make_pair(initCDs,root));

		for (po_pdt_iterator DTN = po_pdt_iterator::begin(&pdt), END = po_pdt_iterator::end(&pdt);
				DTN != END; ++DTN) {
			if (!DTN->getBlock())
				continue;

			ControlDependenceNode *node = bbMap[DTN->getBlock()];
			assert(node);

			cd_set_type cds;
			for (ControlDependenceNode::node_iterator P = node->Parents.begin(), E = node->Parents.end(); P != E; ++P) {
				ControlDependenceNode *parent = *P;
				if (parent->TrueChildren.find(node) != parent->TrueChildren.end())
					cds.insert(std::make_pair(ControlDependenceNode::TRUE, parent));
				if (parent->FalseChildren.find(node) != parent->FalseChildren.end())
					cds.insert(std::make_pair(ControlDependenceNode::FALSE, parent));
				if (parent->OtherChildren.find(node) != parent->OtherChildren.end())
					cds.insert(std::make_pair(ControlDependenceNode::OTHER, parent));
			}

			cd_map_type::iterator CDEntry = cdMap.find(cds);
			ControlDependenceNode *region;
			if (CDEntry == cdMap.end()) {
				region = new ControlDependenceNode();
				nodes.insert(region);
				cdMap.insert(std::make_pair(cds,region));
				for (cd_set_type::iterator CD = cds.begin(), CDEnd = cds.end(); CD != CDEnd; ++CD) {
					switch (CD->first) {
						case ControlDependenceNode::TRUE:
							CD->second->addTrue(region);
							break;
						case ControlDependenceNode::FALSE:
							CD->second->addFalse(region);
							break;
						case ControlDependenceNode::OTHER:
							CD->second->addOther(region); 
							break;
					}
					region->addParent(CD->second);
				}
			} else {
				region = CDEntry->second;
			}
			for (cd_set_type::iterator CD = cds.begin(), CDEnd = cds.end(); CD != CDEnd; ++CD) {
				switch (CD->first) {
					case ControlDependenceNode::TRUE:
						CD->second->removeTrue(node);
						break;
					case ControlDependenceNode::FALSE:
						CD->second->removeFalse(node);
						break;
					case ControlDependenceNode::OTHER:
						CD->second->removeOther(node);
						break;
				}
				region->addOther(node);
				node->addParent(region);
				node->removeParent(CD->second);
			}
		}

		// Make sure that each node has at most one true or false edge
		for (std::set<ControlDependenceNode *>::iterator N = nodes.begin(), E = nodes.end();
				N != E; ++N) {
			ControlDependenceNode *node = *N;
			assert(node);
			if (node->isRegion())
				continue;

			// Fix too many true nodes
			if (node->TrueChildren.size() > 1) {
				ControlDependenceNode *region = new ControlDependenceNode();
				nodes.insert(region);
				for (ControlDependenceNode::node_iterator C = node->true_begin(), CE = node->true_end();
						C != CE; ++C) {
					ControlDependenceNode *child = *C;
					assert(node);
					assert(child);
					assert(region);
					region->addOther(child);
					child->addParent(region);
					child->removeParent(node);
					node->removeTrue(child);
				}
				node->addTrue(region);
			}

			// Fix too many false nodes
			if (node->FalseChildren.size() > 1) {
				ControlDependenceNode *region = new ControlDependenceNode();
				nodes.insert(region);
				for (ControlDependenceNode::node_iterator C = node->false_begin(), CE = node->false_end();
						C != CE; ++C) {
					ControlDependenceNode *child = *C;
					region->addOther(child);
					child->addParent(region);
					child->removeParent(node);
					node->removeFalse(child);
				}
				node->addFalse(region);
			}
		}
	}

	std::string ControlDependenceGraphBase::getBBName(BasicBlock* BB) {
		return (BB != NULL && BB->hasName()) ? BB->getName() : "";
	}

	std::string ControlDependenceGraphBase::getFName(Function* F) {
		return (F!= NULL && F->hasName()) ? F->getName() : "";
	}

	std::set<ControlDependenceNode*> ControlDependenceGraphBase::whatsgoingon(Function &F) {
	
		ofstream log;
		log.open("log.txt", ios::app);
			
		static unsigned edge_id = 0;
		std::set<ControlDependenceNode* >visitedBBs;
	
		log << "In function " << getFName(&F) << "\n";
		
		std::list<ControlDependenceNode*> nodeList;
		nodeList.push_back(root);
		while(nodeList.size() != 0) {
			ControlDependenceNode* node = nodeList.front();
			nodeList.pop_front();
			visitedBBs.insert(node);
			for (ControlDependenceNode::edge_iterator C = node->begin(), CE = node->end();
				C != CE; ++C) {
				if (visitedBBs.find(*C) != visitedBBs.end()) {
					backEdges.insert(std::make_pair(make_pair(node, *C), edge_id++));
					continue;
				}
				else {
					edges.insert(std::make_pair(make_pair(node, *C), edge_id++));
					nodeList.push_back(*C);
				}
			}
		}
		log.close();
		return visitedBBs;
	}

	/*
	 * This creates a reachable set for all ControlDependenceNodes
	 */
	void ControlDependenceGraphBase::getReachableSet(Function &F, NodeListTy nodeList, RMapType &reachabilityMap) {
		
		if (getFName(&F).compare("add_pair_to_block") != 0) return;
		ofstream log;
		log.open("log.txt", ios::app);
			
		for(NodeListTy::iterator NItr = nodeList.begin(); NItr != nodeList.end(); NItr ++) {
			ControlDependenceNode* root = *NItr;
			std::queue<ControlDependenceNode*> CDNQueue;
			CDNQueue.push(root);
			log << "Pushing root " << getNodeName(root).c_str() << "\n";

			std::set<ControlDependenceNode*> reachSet;
			while(CDNQueue.size() != 0) {
				ControlDependenceNode* cdn = CDNQueue.front();
				CDNQueue.pop();
				
				// Add the popped out node to set and add its children to the queue
				reachSet.insert(cdn);
				log << "Pushing node" << getNodeName(cdn).c_str() << "\n";
				
				for (ControlDependenceNode::edge_iterator C = cdn->begin(), CE = cdn->end();
						C != CE; ++C) {
					std::pair<ControlDependenceNode*, ControlDependenceNode*> edge = make_pair(cdn, *C);
					if (backEdges.find(edge) != backEdges.end()) {
						log << "Backedge " << getNodeName(cdn) <<  " " << *C << " " << getNodeName(*C) << "\n";
						continue;
					}
					CDNQueue.push(*C);
					log << "Child node " << getNodeName(*C) << "\n";
				}
			}
			// Add the reachability set to the map
			reachabilityMap.insert(make_pair(root, reachSet));
		}
		log.close();
	}

	std::string ControlDependenceGraphBase::getNodeName(ControlDependenceNode* node) {
		if (node->isRegion()) {
			return "Region";
		}
		BasicBlock* BB = node->getBlock();
		return getBBName(BB);
	}

	void ControlDependenceGraphBase::printReachabilityMap(RMapType reachabilityMap) {
		ofstream log;
		log.open("log.txt", ios::app);
		log << "In print \n";	
		RMapType::iterator RItr;
		for(RItr = reachabilityMap.begin(); RItr != reachabilityMap.end(); RItr++) {
			log << getNodeName(RItr->first).c_str() << " " ;
			for(std::set<ControlDependenceNode*>::iterator RSetItr = (RItr->second).begin();
				RSetItr != (RItr->second).end(); RSetItr++) {
				log << getNodeName(*RSetItr) << " " << " ";	
			}
			log << endl;
		}
		log.close();
	}

	void ControlDependenceGraphBase::graphForFunction(Function &F, PostDominatorTree &pdt) {
		computeDependencies(F,pdt);
		insertRegions(pdt);
	}

	bool ControlDependenceGraphBase::controls(BasicBlock *A, BasicBlock *B) const {
		const ControlDependenceNode *n = getNode(B);
		assert(n && "Basic block not in control dependence graph!");
		while (n->getNumParents() == 1) {
			n = *n->parent_begin();
			if (n->getBlock() == A)
				return true;
		}
		return false;
	}

	bool ControlDependenceGraphBase::influences(BasicBlock *A, BasicBlock *B) const {
		const ControlDependenceNode *n = getNode(B);
		assert(n && "Basic block not in control dependence graph!");

		std::deque<ControlDependenceNode *> worklist;
		worklist.insert(worklist.end(), n->parent_begin(), n->parent_end());

		while (!worklist.empty()) {
			n = worklist.front();
			worklist.pop_front();
			if (n->getBlock() == A) return true;
			worklist.insert(worklist.end(), n->parent_begin(), n->parent_end());
		}

		return false;
	}

	const ControlDependenceNode *ControlDependenceGraphBase::enclosingRegion(BasicBlock *BB) const {
		if (const ControlDependenceNode *node = this->getNode(BB)) {
			return node->enclosingRegion();
		} else {
			return NULL;
		}
	}

} // namespace llvm

namespace {

	struct ControlDependenceViewer
		: public DOTGraphTraitsViewer<ControlDependenceGraph, false> {
			static char ID;
			ControlDependenceViewer() : 
				DOTGraphTraitsViewer<ControlDependenceGraph, false>("control-deps", ID) {}
		};

	struct ControlDependencePrinter
		: public DOTGraphTraitsPrinter<ControlDependenceGraph, false> {
			static char ID;
			ControlDependencePrinter() :
				DOTGraphTraitsPrinter<ControlDependenceGraph, false>("control-deps", ID) {}
		};

} // end anonymous namespace

char ControlDependenceGraph::ID = 0;
static RegisterPass<ControlDependenceGraph> Graph("function-control-deps",
		"Compute control dependency graphs",
		true, true);

char ControlDependenceGraphs::ID = 0;
static RegisterPass<ControlDependenceGraphs> Graphs("module-control-deps",
		"Compute control dependency graphs for an entire module",
		true, true);

char ControlDependenceViewer::ID = 0;
static RegisterPass<ControlDependenceViewer> Viewer("view-control-deps",
		"View the control dependency graph",
		true, true);

char ControlDependencePrinter::ID = 0;
static RegisterPass<ControlDependencePrinter> Printer("print-control-deps",
		"Print the control dependency graph as a 'dot' file",
		true, true);
