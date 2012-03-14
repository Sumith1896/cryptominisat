/*
 * CryptoMiniSat
 *
 * Copyright (c) 2009-2011, Mate Soos and collaborators. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3.0 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301  USA
 */

#include "GateFinder.h"
#include "time_mem.h"
#include "ThreadControl.h"
#include "Subsumer.h"

#ifdef USE_VTK
#include "vtkGraphLayoutView.h"
#include "vtkRenderWindow.h"
#include "vtkRenderWindowInteractor.h"
#include "vtkMutableDirectedGraph.h"
#include "vtkMutableUndirectedGraph.h"
#include "vtkMutableGraphHelper.h"
#endif //USE_VTK
using std::cout;
using std::endl;

GateFinder::GateFinder(Subsumer *_subsumer, ThreadControl *_control) :
    numERVars(0)
    , numDotPrinted(0)
    , totalTime(0)
    , totalLitsRemoved(0)
    , totalClausesShortened(0)
    , totalClausesRemoved(0)
    , totalVarsAdded(0)
    , totalVarsReplaced(0)
    , subsumer(_subsumer)
    , control(_control)
    , seen(_subsumer->seen)
    , seen2(_subsumer->seen2)
{}

uint32_t GateFinder::createNewVars()
{
    double myTime = cpuTime();
    vector<NewGateData> newGates;
    vector<Lit> tmp;
    vector<ClauseIndex> subs;
    uint64_t numOp = 0;
    subsumer->toDecrease = &numMaxCreateNewVars;

    const uint32_t size = (uint32_t)control->getNumUnsetVars()-1;

    uint32_t tries = 0;
    for (; tries < std::min(100000U, size*size/2); tries++) {
        if (*subsumer->toDecrease < 50L*1000L*1000L)
            break;

        //Take some variables randomly
        const Var var1 = control->mtrand.randInt(size);
        const Var var2 = control->mtrand.randInt(size);

        //Check that var1 & var2 are sane choices (not equivalent, not elimed, etc.)
        if (var1 == var2)
            continue;

        if (control->value(var1) != l_Undef
            || !control->decision_var[var1]
            || control->varData[var1].elimed != ELIMED_NONE
            ) continue;

        if (control->value(var2) != l_Undef
            || !control->decision_var[var2]
            || control->varData[var2].elimed != ELIMED_NONE
            ) continue;

        //Pick sign randomly
        Lit lit1 = Lit(var1, control->mtrand.randInt(1));
        Lit lit2 = Lit(var2, control->mtrand.randInt(1));

        //Make sure they are in the right order
        if (lit1 > lit2)
            std::swap(lit1, lit2);

        //See how many clauses this binary gate would shorten
        tmp.clear();
        tmp.push_back(lit1);
        tmp.push_back(lit2);
        subs.clear();
        subsumer->findSubsumed0(std::numeric_limits< uint32_t >::max(), tmp, calcAbstraction(tmp), subs);

        //See how many clauses this binary gate would allow us to remove
        uint32_t potential = 0;
        if (numOp < 100*1000*1000) {
            vector<Lit> lits;
            lits.push_back(lit1);
            lits.push_back(lit2);
            OrGate gate(lits, Lit(0,false), false);
            treatAndGate(gate, false, potential, numOp);
        }

        //If we find the above to be adequate, then this should be a new gate
        if (potential > 5 || subs.size() > 100
            || (potential > 1 && subs.size() > 50)) {
            newGates.push_back(NewGateData(lit1, lit2, subs.size(), potential));
        }
    }

    //Rank the potentially new gates
    std::sort(newGates.begin(), newGates.end());
    newGates.erase(std::unique(newGates.begin(), newGates.end() ), newGates.end() );

    //Add the new gates
    uint32_t addedNum = 0;
    for (uint32_t i = 0; i < newGates.size(); i++) {
        const NewGateData& n = newGates[i];
        if ((i > 50 && n.numLitRem < 1000 && n.numClRem < 25)
            || i > ((double)control->getNumUnsetVars()*0.01)
            || i > 100) break;

        const Var newVar = control->newVar();
        dontElim[newVar] = true;
        const Lit newLit = Lit(newVar, false);
        vector<Lit> lits;
        lits.push_back(n.lit1);
        lits.push_back(n.lit2);
        OrGate gate(lits, newLit, false);
        orGates.push_back(gate);
        gateOccEq[gate.eqLit.toInt()].push_back(orGates.size()-1);
        for (uint32_t i = 0; i < gate.lits.size(); i++) {
            gateOcc[gate.lits[i].toInt()].push_back(orGates.size()-1);
        }

        tmp.clear();
        tmp.push_back(newLit);
        tmp.push_back(~n.lit1);
        Clause* cl = control->addClauseInt(tmp);
        assert(cl == NULL);
        assert(control->ok);

        tmp.clear();
        tmp.push_back(newLit);
        tmp.push_back(~n.lit2);
        cl = control->addClauseInt(tmp);
        assert(cl == NULL);
        assert(control->ok);

        tmp.clear();
        tmp.push_back(~newLit);
        tmp.push_back(n.lit1);
        tmp.push_back(n.lit2);
        cl = control->addClauseInt(tmp, false, ClauseStats(), false);
        assert(cl != NULL);
        assert(control->ok);
        cl->stats.conflictNumIntroduced = control->sumConflicts;
        ClauseIndex c = subsumer->linkInClause(*cl);
        subsumer->clauseData[c.index].defOfOrGate = true;

        addedNum++;
        numERVars++;
    }

    if (control->conf.verbosity >= 1) {
        cout << "c Added " << addedNum << " vars "
        << " tried: " << tries
        << " time: " << (cpuTime() - myTime) << endl;
    }
    totalTime += cpuTime() - myTime;
    totalVarsAdded += addedNum;

    //cout << "c Added " << addedNum << " vars "
    //<< " time: " << (cpuTime() - myTime) << " numThread: " << control->threadNum << endl;

    return addedNum;
}

void GateFinder::findOrGates()
{
    assert(control->ok);

    double myTime = cpuTime();
    clearIndexes();
    numMaxGateFinder = 100L*1000L*1000L;
    subsumer->toDecrease = &numMaxGateFinder;

    findOrGates(true);

    uint32_t learntGatesSize = 0;
    uint32_t numLearnt = 0;
    uint32_t nonLearntGatesSize = 0;
    uint32_t numNonLearnt = 0;
    for(vector<OrGate>::const_iterator it = orGates.begin(), end = orGates.end(); it != end; it++) {
        if (it->learnt) {
            learntGatesSize += it->lits.size();
            numLearnt++;
        } else  {
            nonLearntGatesSize += it->lits.size();
            numNonLearnt++;
        }
    }

    if (control->conf.verbosity >= 1) {
        cout << "c ORs "
        << " nlearnt:" << std::setw(6) << numNonLearnt
        << " avg-s: " << std::fixed << std::setw(4) << std::setprecision(1)
        << ((double)nonLearntGatesSize/(double)numNonLearnt)
        << " learnt: " <<std::setw(6) << numLearnt
        << " avg-s: " << std::fixed << std::setw(4) << std::setprecision(1)
        << ((double)learntGatesSize/(double)numLearnt)
        << " T: " << std::fixed << std::setw(7) << std::setprecision(2) <<  (cpuTime() - myTime)
        << endl;
    }
    totalTime += cpuTime() - myTime;
}

void GateFinder::printGateStats() const
{
    uint32_t gateOccNum = 0;
    for (vector<vector<uint32_t> >::const_iterator it = gateOcc.begin(), end = gateOcc.end(); it != end; it++) {
        gateOccNum += it->size();
    }

    uint32_t gateOccEqNum = 0;
    for (vector<vector<uint32_t> >::const_iterator it = gateOccEq.begin(), end = gateOccEq.end(); it != end; it++) {
        gateOccEqNum += it->size();
    }

    uint32_t gateNum = 0;
    for (vector<OrGate>::const_iterator it = orGates.begin(), end = orGates.end(); it != end; it++) {
        gateNum += !it->removed;
    }

    cout << "c gateOcc num: " << gateOccNum
    << " gateOccEq num: " << gateOccEqNum
    << " gates size: " << gateNum << endl;
}

bool GateFinder::treatOrGates()
{
    assert(control->ok);
    gateLitsRemoved = 0;
    numOrGateReplaced = 0;

    doAllOptimisationWithGates();

    return control->ok;
}

void GateFinder::clearIndexes()
{
    //Clear gate definitions -- this will let us do more, because essentially
    //the other gates are not fully forgotten, so they don't bother us at all
    for (uint32_t i = 0; i < subsumer->clauseData.size(); i++) {
        subsumer->clauseData[i].defOfOrGate = false;
    }

    //Clear gate statistics
    orGates.clear();
    for (size_t i = 0; i < gateOcc.size(); i++)
        gateOcc[i].clear();
    for (size_t i = 0; i < gateOccEq.size(); i++)
        gateOccEq[i].clear();
}

bool GateFinder::extendedResolution()
{
    assert(control->ok);

    double myTime = cpuTime();
    uint32_t oldNumVarToReplace = control->getNewToReplaceVars();
    uint32_t oldNumBins = control->numBins;

    //Clear stuff
    clearIndexes();

    createNewVars();

    if (control->conf.verbosity >= 1) {
        cout << "c ORs : " << std::setw(6) << orGates.size()
        << " cl-sh: " << std::setw(5) << numOrGateReplaced
        << " l-rem: " << std::setw(6) << gateLitsRemoved
        << " b-add: " << std::setw(6) << (control->numBins - oldNumBins)
        << " v-rep: " << std::setw(3) << (control->getNewToReplaceVars() - oldNumVarToReplace)
        << " cl-rem: " << andGateNumFound
        << " avg s: " << ((double)andGateTotalSize/(double)andGateNumFound)
        << " T: " << std::fixed << std::setw(7) << std::setprecision(2) <<  (cpuTime() - myTime) << endl;
    }

    return control->ok;
}

bool GateFinder::doAllOptimisationWithGates()
{
    assert(control->ok);

    //OR gate treatment
    if (control->conf.doShortenWithOrGates) {
        //Setup
        double myTime = cpuTime();
        gateLitsRemoved = 0;
        numOrGateReplaced = 0;
        numMaxShortenWithGates = 100L*1000L*1000L;
        subsumer->toDecrease = &numMaxShortenWithGates;

        //Do shortening
        for (vector<OrGate>::const_iterator it = orGates.begin(), end = orGates.end(); it != end; it++) {
            if (it->removed)
                continue;

            if (*subsumer->toDecrease < 0) {
                cout << "c No more time left for shortening with gates" << endl;
                break;
            }

            if (!shortenWithOrGate(*it))
                return false;
        }

        //Handle results
        if (control->conf.verbosity >= 1) {
            cout << "c OR-based"
            << " cl-sh: " << std::setw(5) << numOrGateReplaced
            << " l-rem: " << std::setw(6) << gateLitsRemoved
            << " T: " << std::fixed << std::setw(7) << std::setprecision(2) <<  (cpuTime() - myTime)
            << endl;
        }
        totalTime += cpuTime() - myTime;
        totalClausesShortened += numOrGateReplaced;
        totalLitsRemoved += gateLitsRemoved;
    }

    //AND gate treatment
    if (control->conf.doRemClWithAndGates) {
        //Setup
        numMaxClRemWithGates = 100L*1000L*1000L;
        subsumer->toDecrease = &numMaxClRemWithGates;
        double myTime = cpuTime();
        andGateNumFound = 0;
        andGateTotalSize = 0;

        //Do clause removal
        uint32_t foundPotential;
        uint64_t numOp = 0;
        for (vector<OrGate>::const_iterator it = orGates.begin(), end = orGates.end(); it != end; it++) {
            const OrGate& gate = *it;
            if (gate.removed || gate.lits.size() >2)
                continue;

            if (*subsumer->toDecrease < 0) {
                cout << "c No more time left for cl-removal with gates" << endl;
                break;
            }

            if (!treatAndGate(gate, true, foundPotential, numOp))
                return false;
        }


        //Handle results
        if (control->conf.verbosity >= 1) {
            cout << "c OR-based"
            << " cl-rem: " << andGateNumFound
            << " avg s: " << ((double)andGateTotalSize/(double)andGateNumFound)
            << " T: " << std::fixed << std::setw(7) << std::setprecision(2) <<  (cpuTime() - myTime)
            << endl;
        }
        totalTime += cpuTime() - myTime;
        totalClausesRemoved += andGateNumFound;
    }

    //EQ gate treatment
    if (control->conf.doFindEqLitsWithGates) {
        //Setup
        double myTime = cpuTime();
        uint32_t oldNumVarToReplace = control->getNewToReplaceVars();

        //Do equivalence checking
        if (!findEqOrGates())
            return false;

        //Handle results
        if (control->conf.verbosity >= 1) {
            cout << "c OR-based"
            << " v-rep: " << std::setw(3) << (control->getNewToReplaceVars() - oldNumVarToReplace)
            << " T: " << std::fixed << std::setw(7) << std::setprecision(2) <<  (cpuTime() - myTime)
            << endl;
        }
        totalTime += cpuTime() - myTime;
        totalVarsReplaced += control->getNewToReplaceVars() - oldNumVarToReplace;
    }

    return true;
}

bool GateFinder::findEqOrGates()
{
    assert(control->ok);
    vector<OrGate> gates = orGates;
    std::sort(gates.begin(), gates.end(), OrGateSorter2());

    vector<Lit> tmp(2);
    for (uint32_t i = 1; i < gates.size(); i++) {
        const OrGate& gate1 = gates[i-1];
        const OrGate& gate2 = gates[i];
        if (gate1.removed || gate2.removed) continue;

        if (gate1.lits == gate2.lits
            && gate1.eqLit.var() != gate2.eqLit.var()
           ) {
            tmp[0] = gate1.eqLit.unsign();
            tmp[1] = gate2.eqLit.unsign();
            const bool RHS = gate1.eqLit.sign() ^ gate2.eqLit.sign();
            if (!control->addXorClauseInt(tmp, RHS))
                return false;
            tmp.resize(2);
        }
    }

    return true;
}

void GateFinder::findOrGates(const bool learntGatesToo)
{
    uint32_t num = 0;
    for (vector<Clause*>::iterator it = subsumer->clauses.begin(), end = subsumer->clauses.end(); it != end; it++, num++) {
        //Clause removed
        if (*it == NULL)
            continue;

        //Ran out of time
        if (*subsumer->toDecrease < 0) {
            if (control->conf.verbosity >= 1) {
                cout << "c Finishing gate-finding: ran out of time" << endl;
            }
            break;
        }

        const Clause& cl = **it;
        //If clause is larger than the cap on gate size, skip. Only for speed reasons.
        if (cl.size() > control->conf.maxGateSize)
            continue;

        //if no learnt gates are allowed and this is learnt, skip
        if (!learntGatesToo && cl.learnt())
            continue;

        const bool wasLearnt = cl.learnt();

        //Check how many literals have zero cache&binary clause
        //If too many, it cannot possibly be an OR gate
        uint8_t numSizeZero = 0;
        for (const Lit *l = cl.begin(), *end2 = cl.end(); l != end2; l++) {
            Lit lit = *l;
            const vector<LitExtra>& cache = control->implCache[(~lit).toInt()].lits;
            const vec<Watched>& ws = control->watches[lit.toInt()];

            if (cache.size() == 0 && ws.size() == 0) {
                numSizeZero++;
                if (numSizeZero > 1)
                    break;
            }
        }
        if (numSizeZero > 1)
            continue;

        //Try to find a gate with eqlit (~*l)
        for (const Lit *l = cl.begin(), *end2 = cl.end(); l != end2; l++)
            findOrGate(~*l, ClauseIndex(num), learntGatesToo, wasLearnt);
    }
}

void GateFinder::findOrGate(const Lit eqLit, const ClauseIndex& c, const bool learntGatesToo, bool wasLearnt)
{
    const Clause& cl = *subsumer->clauses[c.index];
    bool isEqual = true;
    for (const Lit *l2 = cl.begin(), *end3 = cl.end(); l2 != end3; l2++) {
        //We are NOT looking for the literal that is on the RHS
        if (*l2 == ~eqLit)
            continue;

        //This is the other lineral in the binary clause
        //We are looking for a binary clause '~otherlit V eqLit'
        const Lit otherLit = *l2;
        bool OK = false;

        //Try to find corresponding binary clause in cache
        const vector<LitExtra>& cache = control->implCache[(~otherLit).toInt()].lits;
        *subsumer->toDecrease -= cache.size();
        for (vector<LitExtra>::const_iterator
            cacheLit = cache.begin(), endCache = cache.end()
            ; cacheLit != endCache && !OK
            ; cacheLit++
        ) {
            if ((learntGatesToo || cacheLit->getOnlyNLBin())
                 && cacheLit->getLit() == eqLit
            ) {
                wasLearnt |= !cacheLit->getOnlyNLBin();
                OK = true;
            }
        }

        //Try to find corresponding binary clause in watchlist
        const vec<Watched>& ws = control->watches[otherLit.toInt()];
        *subsumer->toDecrease -= ws.size();
        for (vec<Watched>::const_iterator
            wsIt = ws.begin(), endWS = ws.end()
            ; wsIt != endWS && !OK
            ; wsIt++
        ) {
            //Only binary clauses are of importance
            if (!wsIt->isBinary())
                continue;

            if ((learntGatesToo || !wsIt->getLearnt())
                 && wsIt->getOtherLit() == eqLit
            ) {
                wasLearnt |= wsIt->getLearnt();
                OK = true;
            }
        }

        //We have to find the binary clause. If not, this is not a gate
        if (!OK) {
            isEqual = false;
            break;
        }
    }

    //Does not make a gate, return
    if (!isEqual)
        return;

    //Create gate
    vector<Lit> lits;
    for (const Lit *l2 = cl.begin(), *end3 = cl.end(); l2 != end3; l2++) {
        if (*l2 == ~eqLit) continue;
        lits.push_back(*l2);
    }
    OrGate gate(lits, eqLit, wasLearnt);

    //Find if there are any gates that are the same
    const vector<uint32_t>& similar = gateOccEq[gate.eqLit.toInt()];
    for (vector<uint32_t>::const_iterator it = similar.begin(), end = similar.end(); it != end; it++) {
        //The same gate? Then froget about this
        if (orGates[*it] == gate)
            return;
    }

    //Add gate
    *subsumer->toDecrease -= gate.lits.size()*2;
    orGates.push_back(gate);
    subsumer->clauseData[c.index].defOfOrGate = true;
    gateOccEq[gate.eqLit.toInt()].push_back(orGates.size()-1);
    if (!wasLearnt) {
        for (uint32_t i = 0; i < gate.lits.size(); i++) {
            Lit lit = gate.lits[i];
            gateOcc[lit.toInt()].push_back(orGates.size()-1);
        }
    }

    #ifdef VERBOSE_ORGATE_REPLACE
    cout << "Found gate : " << gate << endl;
    #endif
}

bool GateFinder::shortenWithOrGate(const OrGate& gate)
{
    assert(control->ok);

    //Find clauses that potentially could be shortened
    vector<ClauseIndex> subs;
    subsumer->findSubsumed0(std::numeric_limits< uint32_t >::max(), gate.lits, calcAbstraction(gate.lits), subs);

    for (uint32_t i = 0; i < subs.size(); i++) {
        ClauseIndex c = subs[i];
        //Don't shorten definitions of OR gates -- we could be manipulating the definition of the gate itself
        //Don't shorten non-learnt clauses with learnt gates --- potential loss if e.g. learnt clause is removed later
        if (subsumer->clauseData[c.index].defOfOrGate
            || (!subsumer->clauses[c.index]->learnt() && gate.learnt))
            continue;

        #ifdef VERBOSE_ORGATE_REPLACE
        cout << "OR gate-based cl-shortening" << endl;
        cout << "Gate used: " << gate << endl;
        cout << "orig Clause: " << *clauses[c.index]<< endl;
        #endif

        numOrGateReplaced++;

        //Go through clause, check if RHS (eqLit) is inside the clause
        //If it is, we have two possibilities:
        //1) a = b V c , clause: a V b V c V d
        //2) a = b V c , clause: -a V b V c V d --> clause can be safely removed
        bool removedClause = false;
        bool eqLitInside = false;
        Clause *cl = subsumer->clauses[c.index];
        for (Lit *l = cl->begin(), *end = cl->end(); l != end; l++) {
            if (gate.eqLit.var() == l->var()) {
                if (gate.eqLit == *l) {
                    eqLitInside = true;
                    break;
                } else {
                    assert(gate.eqLit == ~*l);
                    subsumer->unlinkClause(c);
                    removedClause = true;
                    break;
                }
            }
        }

        //This clause got removed. Moving on to next clause containing all the gates' LHS literals
        if (removedClause)
            continue;

        //Set up future clause's lits
        vector<Lit> lits;
        for (uint32_t i = 0; i < cl->size(); i++) {
            const Lit lit = (*cl)[i];
            bool inGate = false;
            for (vector<Lit>::const_iterator it = gate.lits.begin(), end = gate.lits.end(); it != end; it++) {
                if (*it == lit) {
                    inGate = true;
                    gateLitsRemoved++;
                    break;
                }
            }

            if (!inGate)
                lits.push_back(lit);
        }
        if (!eqLitInside) {
            lits.push_back(gate.eqLit);
            gateLitsRemoved--;
        }

        //Future clause's stat
        const bool learnt = cl->learnt();
        ClauseStats stats = cl->stats;

        //Free the old clause and allocate new one
        subsumer->unlinkClause(c.index);
        cl = control->addClauseInt(lits, learnt, stats, false);
        if (!control->ok)
            return false;

        //If this clause is NULL, then just ignore
        if (cl == NULL)
            continue;

        subsumer->linkInClause(*cl);

        #ifdef VERBOSE_ORGATE_REPLACE
        cout << "new  Clause : " << cl << endl;
        cout << "-----------" << endl;
        #endif
    }

    return true;
}

CL_ABST_TYPE GateFinder::calculateSortedOcc(const OrGate& gate, uint16_t& maxSize, vector<size_t>& seen2Set, uint64_t& numOp)
{
    CL_ABST_TYPE abstraction = 0;

    //Initialise sizeSortedOcc, which s a temporary to save memory frees&requests
    for (uint32_t i = 0; i < sizeSortedOcc.size(); i++)
        sizeSortedOcc[i].clear();

    const Occur& csOther = subsumer->occur[(~(gate.lits[1])).toInt()];
    //cout << "csother: " << csOther.size() << endl;
    *subsumer->toDecrease -= csOther.size()*3;
    for (Occur::const_iterator it = csOther.begin(), end = csOther.end(); it != end; it++) {
        const Clause& cl = *subsumer->clauses[it->index];

        if (subsumer->clauseData[it->index].defOfOrGate //We might be removing the definition. Info loss
            || (!cl.learnt() && gate.learnt)) //We might be contracting 2 non-learnt clauses based on a learnt gate. Info loss
            continue;

        numOp += cl.size();

        //Make sure sizeSortedOcc is enough, and add this clause to it
        maxSize = std::max(maxSize, cl.size());
        if (sizeSortedOcc.size() < (uint32_t)maxSize+1)
            sizeSortedOcc.resize(maxSize+1);

        sizeSortedOcc[cl.size()].push_back(*it);

        //Set seen2 & abstraction, which are optimisations to speed up and-gate-based-contraction
        for (uint32_t i = 0; i < cl.size(); i++) {
            if (!seen2[cl[i].toInt()]) {
                seen2[cl[i].toInt()] = true;
                seen2Set.push_back(cl[i].toInt());
            }
            abstraction |= 1UL << (cl[i].var() % CLAUSE_ABST_SIZE);
        }
    }
    abstraction |= 1UL << (gate.lits[0].var() % CLAUSE_ABST_SIZE);

    return abstraction;
}

bool GateFinder::treatAndGate(const OrGate& gate, const bool reallyRemove, uint32_t& foundPotential, uint64_t& numOp)
{
    assert(gate.lits.size() == 2);

    //If there are no clauses that contain the opposite of the literals on the LHS, there is nothing we can do
    if (subsumer->occur[(~(gate.lits[0])).toInt()].empty()
        || subsumer->occur[(~(gate.lits[1])).toInt()].empty())
        return true;

    //Set up sorted occurrance list of the other lit (lits[1]) in the gate
    uint16_t maxSize = 0; //Maximum clause size in this occur
    vector<size_t> seen2Set; //Bits that have been set in seen2, and later need to be cleared
    CL_ABST_TYPE abstraction = calculateSortedOcc(gate, maxSize, seen2Set, numOp);

    //Setup
    set<ClauseIndex> clToUnlink;
    ClauseIndex other;
    foundPotential = 0;

    //Now go through lits[0] and see if anything matches
    Occur& cs = subsumer->occur[(~(gate.lits[0])).toInt()];
    subsumer->toDecrease -= cs.size()*3;
    for (Occur::const_iterator it2 = cs.begin(), end2 = cs.end(); it2 != end2; it2++) {
        if (subsumer->clauseData[it2->index].defOfOrGate //Don't remove definition by accident
            || (subsumer->clauseData[it2->index].abst | abstraction) != abstraction //Abstraction must be OK
            || subsumer->clauseData[it2->index].size > maxSize //Size must be less than maxSize
            || sizeSortedOcc[subsumer->clauseData[it2->index].size].empty()) //this bracket for sizeSortedOcc must be non-empty
            continue;

        const Clause& cl = *subsumer->clauses[it2->index];
        numOp += cl.size();

        //Check that we are not removing non-learnt info based on learnt gate
        if (!cl.learnt() && gate.learnt)
            continue;

        //Check that ~lits[1] is not inside this clause, and that eqLit is not inside, either
        //Also check that all literals inside have at least been set by seen2 (otherwise, no chance of exact match)
        bool OK = true;
        for (uint32_t i = 0; i < cl.size(); i++) {
            if (cl[i] == ~(gate.lits[0])) continue;
            if (   cl[i].var() == ~(gate.lits[1].var())
                || cl[i].var() == gate.eqLit.var()
                || !seen2[cl[i].toInt()]
                ) {
                OK = false;
                break;
            }
        }
        if (!OK) continue;

        //Calculate abstraction and set 'seen'
        CL_ABST_TYPE abst2 = 0;
        for (uint32_t i = 0; i < cl.size(); i++) {
            //Lit0 doesn't count into abstraction
            if (cl[i] == ~(gate.lits[0]))
                continue;

            seen[cl[i].toInt()] = true;
            abst2 |= 1UL << (cl[i].var() % CLAUSE_ABST_SIZE);
        }
        abst2 |= 1UL << ((~(gate.lits[1])).var() % CLAUSE_ABST_SIZE);

        //Find matching pair
        numOp += sizeSortedOcc[cl.size()].size()*5;
        const bool foundOther = findAndGateOtherCl(sizeSortedOcc[cl.size()], ~(gate.lits[1]), abst2, other);
        foundPotential += foundOther;
        if (reallyRemove && foundOther) {
            assert(other.index != it2->index);
            clToUnlink.insert(other.index);
            clToUnlink.insert(it2->index);
            //Add new clause that is shorter and represents both of the clauses above
            if (!treatAndGateClause(other, gate, cl))
                return false;
        }

        //Clear 'seen' from bits set
        for (uint32_t i = 0; i < cl.size(); i++) {
            seen[cl[i].toInt()] = false;
        }
    }

    //Clear from seen2 bits that have been set
    for(vector<size_t>::const_iterator it = seen2Set.begin(), end = seen2Set.end(); it != end; it++) {
        seen2[*it] = false;
    }

    //Now that all is computed, remove those that need removal
    for(std::set<ClauseIndex>::const_iterator it2 = clToUnlink.begin(), end2 = clToUnlink.end(); it2 != end2; it2++) {
        subsumer->unlinkClause(*it2);
    }
    clToUnlink.clear();

    return true;
}

bool GateFinder::treatAndGateClause(const ClauseIndex& other, const OrGate& gate, const Clause& cl)
{
    #ifdef VERBOSE_ORGATE_REPLACE
    cout << "AND gate-based cl rem" << endl;
    cout << "clause 1: " << cl << endl;
    cout << "clause 2: " << *clauses[other.index] << endl;
    cout << "gate : " << gate << endl;
    #endif

    //Update stats
    andGateNumFound++;
    andGateTotalSize += cl.size();

    //Put into 'lits' the literals of the clause
    vector<Lit> lits;
    lits.clear();
    for (uint32_t i = 0; i < cl.size(); i++) {
        if (cl[i] != ~(gate.lits[0]))
            lits.push_back(cl[i]);

        assert(cl[i].var() != gate.eqLit.var());
    }
    lits.push_back(~(gate.eqLit));

    //Calculate learnt & glue
    Clause& otherCl = *subsumer->clauses[other.index];
    *subsumer->toDecrease -= otherCl.size()*2;
    bool learnt = otherCl.learnt() && cl.learnt();
    ClauseStats stats = ClauseStats::combineStats(cl.stats, otherCl.stats);

    #ifdef VERBOSE_ORGATE_REPLACE
    cout << "new clause:" << lits << endl;
    cout << "-----------" << endl;
    #endif

    //Create and link in new clause
    Clause* c = control->addClauseInt(lits, learnt, stats, false);
    if (c != NULL)
        subsumer->linkInClause(*c);
    if (!control->ok)
        return false;

    return true;
}

inline bool GateFinder::findAndGateOtherCl(const vector<ClauseIndex>& sizeSortedOcc, const Lit lit, const CL_ABST_TYPE abst2, ClauseIndex& other)
{
    *subsumer->toDecrease -= sizeSortedOcc.size();
    for (vector<ClauseIndex>::const_iterator it = sizeSortedOcc.begin(), end = sizeSortedOcc.end(); it != end; it++) {

        if (subsumer->clauseData[it->index].defOfOrGate //Don't potentially remove clause that is the definition itself
            || subsumer->clauseData[it->index].abst != abst2) continue; //abstraction must match

        const Clause& cl = *subsumer->clauses[it->index];
        for (uint32_t i = 0; i < cl.size(); i++) {
            //we skip the other lit in the gate
            if (cl[i] == lit)
                continue;

            //Seen is correct, so this one is not the one we are searching for
            if (!seen[cl[i].toInt()])
                goto next;

        }
        other = *it;
        return true;

        next:;
    }

    return false;
}

void GateFinder::printDot2()
{
    std::stringstream ss;
    ss << "Gates" << (numDotPrinted++) << ".dot";
    std::string filenename = ss.str();
    std::ofstream file(filenename.c_str(), std::ios::out);
    file << "digraph G {" << endl;
    uint32_t index = 0;
    vector<bool> gateUsed;
    gateUsed.resize(orGates.size(), false);
    index = 0;
    for (vector<OrGate>::const_iterator it = orGates.begin(), end = orGates.end(); it != end; it++, index++) {
        for (vector<Lit>::const_iterator it2 = it->lits.begin(), end2 = it->lits.end(); it2 != end2; it2++) {
            vector<uint32_t>& occ = gateOccEq[it2->toInt()];
            for (vector<uint32_t>::const_iterator it3 = occ.begin(), end3 = occ.end(); it3 != end3; it3++) {
                if (*it3 == index) continue;

                file << "Gate" << *it3;
                gateUsed[*it3] = true;
                file << " -> ";

                file << "Gate" << index;
                gateUsed[index] = true;

                file << "[arrowsize=\"0.4\"];" << endl;
            }

            /*vector<uint32_t>& occ2 = gateOccEq[(~*it2).toInt()];
            for (vector<uint32_t>::const_iterator it3 = occ2.begin(), end3 = occ2.end(); it3 != end3; it3++) {
                if (*it3 == index) continue;

                file << "Gate" << *it3;
                gateUsed[*it3] = true;
                file << " -> ";

                file << "Gate" << index;
                gateUsed[index] = true;

                file << "[style = \"dotted\", arrowsize=\"0.4\"];" << endl;
            }*/
        }
    }

    index = 0;
    for (vector<OrGate>::iterator it = orGates.begin(), end = orGates.end(); it != end; it++, index++) {

        if (gateUsed[index]) {
            file << "Gate" << index << " [ shape=\"point\"";
            file << ", size = 0.8";
            file << ", style=\"filled\"";
            if (it->learnt) file << ", color=\"darkseagreen4\"";
            else file << ", color=\"darkseagreen\"";
            file << "];" << endl;
        }
    }

    file  << "}" << endl;
    file.close();
    cout << "c Printed gate structure to file " << filenename << endl;
}

void GateFinder::printDot()
{
    printDot2();

    #ifdef USE_VTK
    vtkSmartPointer<vtkMutableDirectedGraph> g =
    vtkSmartPointer<vtkMutableDirectedGraph>::New();

    vtkSmartPointer<vtkMutableGraphHelper> graphHelper =
    vtkSmartPointer<vtkMutableGraphHelper>::New();
    graphHelper->SetGraph(g);
    /*vtkIdType v0 = graphHelper->AddVertex();
    vtkIdType v1 = graphHelper->AddVertex();
    vtkIdType v2 = graphHelper->AddVertex();

    vtkIdType v3 = graphHelper->AddVertex();
    vtkIdType v5 = graphHelper->AddVertex();

    graphHelper->AddEdge(v0, v1);
    graphHelper->AddEdge(v1, v2);
    graphHelper->AddEdge(v0, v2);

    graphHelper->AddEdge(v3, v5);*/



    vector<size_t> gateUsed;
    gateUsed.resize(orGates.size(), 0);
    vector<vtkIdType> vertexes(orGates.size());
    uint64_t edgesAdded = 0;

    //Go through each gate
    uint32_t index = 0;
    for (vector<OrGate>::const_iterator it = orGates.begin(), end = orGates.end(); it != end; it++, index++) {
        //Each literal in the LHS
        for (vector<Lit>::const_iterator it2 = it->lits.begin(), end2 = it->lits.end(); it2 != end2; it2++) {
            //See if it is connected as an output(RHS) to another gate
            const vector<uint32_t>& occ = gateOccEq[it2->toInt()];
            for (vector<uint32_t>::const_iterator it3 = occ.begin(), end3 = occ.end(); it3 != end3; it3++) {
                //It's this gate, ignore
                if (*it3 == index)
                    continue;

                //Add vertexes if not present
                if (!gateUsed[*it3])
                    vertexes[*it3] = graphHelper->AddVertex();
                gateUsed[*it3]++;;

                if (!gateUsed[index])
                    vertexes[index] = graphHelper->AddVertex();
                gateUsed[index]++;

                //Add edge
                graphHelper->AddEdge(vertexes[*it3], vertexes[index]);
                edgesAdded++;
            }

            /*
            //See if it is connected as an ~output(~RHS) to another gate
            const vector<uint32_t>& occ2 = gateOccEq[(~*it2).toInt()];
            for (vector<uint32_t>::const_iterator it3 = occ2.begin(), end3 = occ2.end(); it3 != end3; it3++) {
                //It's this gate, ignore
                if (*it3 == index)
                    continue;

                //Add vertexes if not present
                if (!gateUsed[*it3])
                    vertexes[*it3] = graphHelper->AddVertex();
                gateUsed[*it3]++;;

                if (!gateUsed[index])
                    vertexes[index] = graphHelper->AddVertex();
                gateUsed[index]++;

                //Add edge
                graphHelper->AddEdge(vertexes[*it3], vertexes[index]);
                edgesAdded++;
            }*/
        }
    }

    /*index = 0;
    for (vector<OrGate>::iterator it = orGates.begin(), end = orGates.end(); it != end; it++, index++) {

        if (gateUsed[index]) {
            file << "Gate" << index << " [ shape=\"point\"";
            file << ", size = 0.8";
            file << ", style=\"filled\"";
            if (it->learnt) file << ", color=\"darkseagreen4\"";
            else file << ", color=\"darkseagreen\"";
            file << "];" << endl;
        }
    }*/


  // Can also do this:
  //graphHelper->RemoveEdge(0);

    cout << "c Edges added: " << edgesAdded << endl;
    vtkSmartPointer<vtkGraphLayoutView> graphLayoutView =
    vtkSmartPointer<vtkGraphLayoutView>::New();
    graphLayoutView->AddRepresentationFromInput(graphHelper->GetGraph());
    //graphLayoutView->SetLayoutStrategyToForceDirected();
    //graphLayoutView->SetLayoutStrategyToClustering2D();
    graphLayoutView->SetLayoutStrategyToFast2D();
    graphLayoutView->ResetCamera();
    graphLayoutView->Render();
    graphLayoutView->GetInteractor()->Start();

    #endif //USE_VTK
}

void GateFinder::newVar()
{
    dontElim.push_back(0);
    gateOcc.push_back(vector<uint32_t>());
    gateOcc.push_back(vector<uint32_t>());
    gateOccEq.push_back(vector<uint32_t>());
    gateOccEq.push_back(vector<uint32_t>());
}

double GateFinder::getTotalTime() const
{
    return totalTime;
}

size_t GateFinder::getTotalLitsRemoved() const
{
    return totalLitsRemoved;
}

size_t GateFinder::getTotalClausesShortened() const
{
    return totalClausesShortened;
}

size_t GateFinder::getTotalClausesRemoved() const
{
    return totalClausesRemoved;
}

size_t GateFinder::getTotalVarsAdded() const
{
    return totalVarsAdded;
}

size_t GateFinder::getTotalVarsReplaced() const
{
    return totalVarsReplaced;
}
