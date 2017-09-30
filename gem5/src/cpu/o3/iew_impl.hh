/*
 * Copyright (c) 2010-2013 ARM Limited
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Kevin Lim
 */

#ifndef __CPU_O3_IEW_IMPL_IMPL_HH__
#define __CPU_O3_IEW_IMPL_IMPL_HH__

// @todo: Fix the instantaneous communication among all the stages within
// iew.  There's a clear delay between issue and execute, yet backwards
// communication happens simultaneously.

#include <queue>

#include "arch/utility.hh"
#include "config/the_isa.hh"
#include "cpu/checker/cpu.hh"
#include "cpu/o3/fu_pool.hh"
#include "cpu/o3/iew.hh"
#include "cpu/timebuf.hh"
#include "debug/Activity.hh"
#include "debug/Drain.hh"
#include "debug/IEW.hh"
#include "debug/O3PipeView.hh"
#include "params/DerivO3CPU.hh"

#define HISTORY_TABLE_SIZE 1000

using namespace std;

template<class Impl>
DefaultIEW<Impl>::DefaultIEW(O3CPU *_cpu, DerivO3CPUParams *params)
    : issueToExecQueue(params->backComSize, params->forwardComSize),
      cpu(_cpu),
      instQueue(_cpu, this, params),
      ldstQueue(_cpu, this, params),
      fuPool(params->fuPool),
      commitToIEWDelay(params->commitToIEWDelay),
      renameToIEWDelay(params->renameToIEWDelay),
      issueToExecuteDelay(params->issueToExecuteDelay),
      dispatchWidth(params->dispatchWidth),
      issueWidth(params->issueWidth),
	  isBCUsed(params->isBundleCommitUsed),
      wbWidth(params->wbWidth),
	  isIXUUsed(params->isIXUUsed),
	  ixuWidth(params->ixuWidth),
	  ixuDepth(params->ixuDepth),
      numThreads(params->numThreads)
{
    if (dispatchWidth > Impl::MaxWidth)
        fatal("dispatchWidth (%d) is larger than compiled limit (%d),\n"
             "\tincrease MaxWidth in src/cpu/o3/impl.hh\n",
             dispatchWidth, static_cast<int>(Impl::MaxWidth));
    if (issueWidth > Impl::MaxWidth)
        fatal("issueWidth (%d) is larger than compiled limit (%d),\n"
             "\tincrease MaxWidth in src/cpu/o3/impl.hh\n",
             issueWidth, static_cast<int>(Impl::MaxWidth));
    if (wbWidth > Impl::MaxWidth)
        fatal("wbWidth (%d) is larger than compiled limit (%d),\n"
             "\tincrease MaxWidth in src/cpu/o3/impl.hh\n",
             wbWidth, static_cast<int>(Impl::MaxWidth));

    _status = Active;
    exeStatus = Running;
    wbStatus = Idle;

	/* Allocate IXU buffers */
	buffer_of_ixu = new std::deque<DynInstPtr>[ixuDepth+1];

	/* Initialize IXU history table */
	IXU_history_table = new IXU_history_entries*[HISTORY_TABLE_SIZE];

	for(int i=0; i<HISTORY_TABLE_SIZE; i++)
	{
		IXU_history_table[i] = new IXU_history_entries;
		initializeIHT(IXU_history_table[i]);
	}

    // Setup wire to read instructions coming from issue.
    fromIssue = issueToExecQueue.getWire(-issueToExecuteDelay);

    // Instruction queue needs the queue between issue and execute.
    instQueue.setIssueToExecuteQueue(&issueToExecQueue);

    for (ThreadID tid = 0; tid < numThreads; tid++) {
        dispatchStatus[tid] = Running;
        fetchRedirect[tid] = false;
    }

    updateLSQNextCycle = false;

    skidBufferMax = (renameToIEWDelay + 1) * params->renameWidth;
}

template <class Impl>
std::string
DefaultIEW<Impl>::name() const
{
    return cpu->name() + ".iew";
}

template <class Impl>
void
DefaultIEW<Impl>::regProbePoints()
{
    ppDispatch = new ProbePointArg<DynInstPtr>(cpu->getProbeManager(), "Dispatch");
    ppMispredict = new ProbePointArg<DynInstPtr>(cpu->getProbeManager(), "Mispredict");
    /**
     * Probe point with dynamic instruction as the argument used to probe when
     * an instruction starts to execute.
     */
    ppExecute = new ProbePointArg<DynInstPtr>(cpu->getProbeManager(),
                                              "Execute");
    /**
     * Probe point with dynamic instruction as the argument used to probe when
     * an instruction execution completes and it is marked ready to commit.
     */
    ppToCommit = new ProbePointArg<DynInstPtr>(cpu->getProbeManager(),
                                               "ToCommit");
}

template <class Impl>
void
DefaultIEW<Impl>::regStats()
{
    using namespace Stats;

    instQueue.regStats();
    ldstQueue.regStats();

    iewIdleCycles
        .name(name() + ".iewIdleCycles")
        .desc("Number of cycles IEW is idle");

    iewSquashCycles
        .name(name() + ".iewSquashCycles")
        .desc("Number of cycles IEW is squashing");

    iewBlockCycles
        .name(name() + ".iewBlockCycles")
        .desc("Number of cycles IEW is blocking");

    iewUnblockCycles
        .name(name() + ".iewUnblockCycles")
        .desc("Number of cycles IEW is unblocking");

    iewDispatchedInsts
        .name(name() + ".iewDispatchedInsts")
        .desc("Number of instructions dispatched to IQ");

    iewDispSquashedInsts
        .name(name() + ".iewDispSquashedInsts")
        .desc("Number of squashed instructions skipped by dispatch");

    iewDispLoadInsts
        .name(name() + ".iewDispLoadInsts")
        .desc("Number of dispatched load instructions");

    iewDispStoreInsts
        .name(name() + ".iewDispStoreInsts")
        .desc("Number of dispatched store instructions");

    iewDispNonSpecInsts
        .name(name() + ".iewDispNonSpecInsts")
        .desc("Number of dispatched non-speculative instructions");

    iewIQFullEvents
        .name(name() + ".iewIQFullEvents")
        .desc("Number of times the IQ has become full, causing a stall");

    iewLSQFullEvents
        .name(name() + ".iewLSQFullEvents")
        .desc("Number of times the LSQ has become full, causing a stall");

    memOrderViolationEvents
        .name(name() + ".memOrderViolationEvents")
        .desc("Number of memory order violations");

    predictedTakenIncorrect
        .name(name() + ".predictedTakenIncorrect")
        .desc("Number of branches that were predicted taken incorrectly");

    predictedNotTakenIncorrect
        .name(name() + ".predictedNotTakenIncorrect")
        .desc("Number of branches that were predicted not taken incorrectly");

    branchMispredicts
        .name(name() + ".branchMispredicts")
        .desc("Number of branch mispredicts detected at execute");

    branchMispredicts = predictedTakenIncorrect + predictedNotTakenIncorrect;

    iewExecutedInsts
        .name(name() + ".iewExecutedInsts")
        .desc("Number of executed instructions");
	/****************** IXU Structure ***********************/

    ixuEnteredInsts
        .name(name() + ".ixuEnteredInsts")
        .desc("Number of instructions entered into IXU");

	ixuExecInsts
        .name(name() + ".ixuExecInsts")
        .desc("Number of instructions executed in IXU");

    ixuExecIn1st
        .name(name() + ".ixuExecIn1st")
        .desc("Number of instructions executed in 1st stage of IXU");

    ixuExecIn2nd
        .name(name() + ".ixuExecIn2nd")
        .desc("Number of instructions executed in 2nd stage of IXU");

    ixuExecIn3rd
        .name(name() + ".ixuExecIn3rd")
        .desc("Number of instructions executed in 3rd stage of IXU");

    ixuExecIn4th
        .name(name() + ".ixuExecIn4th")
        .desc("Number of instructions executed in 4th stage of IXU");

    numOfIQAccessFronIXU
        .name(name() + ".numOfIQAccessFronIXU")
        .desc("Number of instructions that access instruction queue to wakeup");

    numOfForwardingInIXU
        .name(name() + ".numOfForwardingInIXU")
        .desc("Number of forwarded counts that in IXU structure");

	/*******************************************************/
    iewExecLoadInsts
        .init(cpu->numThreads)
        .name(name() + ".iewExecLoadInsts")
        .desc("Number of load instructions executed")
        .flags(total);

    iewExecSquashedInsts
        .name(name() + ".iewExecSquashedInsts")
        .desc("Number of squashed instructions skipped in execute");

    iewExecutedSwp
        .init(cpu->numThreads)
        .name(name() + ".exec_swp")
        .desc("number of swp insts executed")
        .flags(total);

    iewExecutedNop
        .init(cpu->numThreads)
        .name(name() + ".exec_nop")
        .desc("number of nop insts executed")
        .flags(total);

    iewExecutedRefs
        .init(cpu->numThreads)
        .name(name() + ".exec_refs")
        .desc("number of memory reference insts executed")
        .flags(total);

    iewExecutedBranches
        .init(cpu->numThreads)
        .name(name() + ".exec_branches")
        .desc("Number of branches executed")
        .flags(total);

    iewExecStoreInsts
        .name(name() + ".exec_stores")
        .desc("Number of stores executed")
        .flags(total);
    iewExecStoreInsts = iewExecutedRefs - iewExecLoadInsts;

    iewExecRate
        .name(name() + ".exec_rate")
        .desc("Inst execution rate")
        .flags(total);

    iewExecRate = iewExecutedInsts / cpu->numCycles;

    iewInstsToCommit
        .init(cpu->numThreads)
        .name(name() + ".wb_sent")
        .desc("cumulative count of insts sent to commit")
        .flags(total);

    writebackCount
        .init(cpu->numThreads)
        .name(name() + ".wb_count")
        .desc("cumulative count of insts written-back")
        .flags(total);

    producerInst
        .init(cpu->numThreads)
        .name(name() + ".wb_producers")
        .desc("num instructions producing a value")
        .flags(total);

    consumerInst
        .init(cpu->numThreads)
        .name(name() + ".wb_consumers")
        .desc("num instructions consuming a value")
        .flags(total);

    wbFanout
        .name(name() + ".wb_fanout")
        .desc("average fanout of values written-back")
        .flags(total);

    wbFanout = producerInst / consumerInst;

    wbRate
        .name(name() + ".wb_rate")
        .desc("insts written-back per cycle")
        .flags(total);
    wbRate = writebackCount / cpu->numCycles;
}

template<class Impl>
void
DefaultIEW<Impl>::startupStage()
{
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        toRename->iewInfo[tid].usedIQ = true;
        toRename->iewInfo[tid].freeIQEntries =
            instQueue.numFreeEntries(tid);

        toRename->iewInfo[tid].usedLSQ = true;
        toRename->iewInfo[tid].freeLQEntries = ldstQueue.numFreeLoadEntries(tid);
        toRename->iewInfo[tid].freeSQEntries = ldstQueue.numFreeStoreEntries(tid);
    }

    // Initialize the checker's dcache port here
    if (cpu->checker) {
        cpu->checker->setDcachePort(&cpu->getDataPort());
    }

    cpu->activateStage(O3CPU::IEWIdx);
}

template<class Impl>
void
DefaultIEW<Impl>::setTimeBuffer(TimeBuffer<TimeStruct> *tb_ptr)
{
    timeBuffer = tb_ptr;

    // Setup wire to read information from time buffer, from commit.
    fromCommit = timeBuffer->getWire(-commitToIEWDelay);

    // Setup wire to write information back to previous stages.
    toRename = timeBuffer->getWire(0);

    toFetch = timeBuffer->getWire(0);

    // Instruction queue also needs main time buffer.
    instQueue.setTimeBuffer(tb_ptr);
}

template<class Impl>
void
DefaultIEW<Impl>::setRenameQueue(TimeBuffer<RenameStruct> *rq_ptr)
{
    renameQueue = rq_ptr;

    // Setup wire to read information from rename queue.
    fromRename = renameQueue->getWire(-renameToIEWDelay);
}

template<class Impl>
void
DefaultIEW<Impl>::setIEWQueue(TimeBuffer<IEWStruct> *iq_ptr)
{
    iewQueue = iq_ptr;

    // Setup wire to write instructions to commit.
    toCommit = iewQueue->getWire(0);
}

template<class Impl>
void
DefaultIEW<Impl>::setActiveThreads(list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;

    ldstQueue.setActiveThreads(at_ptr);
    instQueue.setActiveThreads(at_ptr);
}

template<class Impl>
void
DefaultIEW<Impl>::setScoreboard(Scoreboard *sb_ptr)
{
    scoreboard = sb_ptr;
}

template <typename Impl>
void DefaultIEW<Impl>::setLWModule(LWModule *_lwModule)
{
	lwModule = _lwModule;
}

template <class Impl>
bool
DefaultIEW<Impl>::isDrained() const
{
    bool drained = ldstQueue.isDrained() && instQueue.isDrained();

    for (ThreadID tid = 0; tid < numThreads; tid++) {
        if (!insts[tid].empty()) {
            DPRINTF(Drain, "%i: Insts not empty.\n", tid);
            drained = false;
        }
        if (!skidBuffer[tid].empty()) {
            DPRINTF(Drain, "%i: Skid buffer not empty.\n", tid);
            drained = false;
        }
        drained = drained && dispatchStatus[tid] == Running;
    }

    // Also check the FU pool as instructions are "stored" in FU
    // completion events until they are done and not accounted for
    // above
    if (drained && !fuPool->isDrained()) {
        DPRINTF(Drain, "FU pool still busy.\n");
        drained = false;
    }

    return drained;
}

template <class Impl>
void
DefaultIEW<Impl>::drainSanityCheck() const
{
    assert(isDrained());

    instQueue.drainSanityCheck();
    ldstQueue.drainSanityCheck();
}

template <class Impl>
void
DefaultIEW<Impl>::takeOverFrom()
{
    // Reset all state.
    _status = Active;
    exeStatus = Running;
    wbStatus = Idle;

    instQueue.takeOverFrom();
    ldstQueue.takeOverFrom();
    fuPool->takeOverFrom();

    startupStage();
    cpu->activityThisCycle();

    for (ThreadID tid = 0; tid < numThreads; tid++) {
        dispatchStatus[tid] = Running;
        fetchRedirect[tid] = false;
    }

    updateLSQNextCycle = false;

    for (int i = 0; i < issueToExecQueue.getSize(); ++i) {
        issueToExecQueue.advance();
    }
}

template<class Impl>
void
DefaultIEW<Impl>::squash(ThreadID tid)
{
    DPRINTF(IEW, "[tid:%i]: Squashing all instructions.\n", tid);

    // Tell the IQ to start squashing.
    instQueue.squash(tid);

    // Tell the LDSTQ to start squashing.
    ldstQueue.squash(fromCommit->commitInfo[tid].doneSeqNum, tid);
    updatedQueues = true;

    // Clear the skid buffer in case it has any data in it.
    DPRINTF(IEW, "[tid:%i]: Removing skidbuffer instructions until [sn:%i].\n",
            tid, fromCommit->commitInfo[tid].doneSeqNum);

    while (!skidBuffer[tid].empty()) {
        if (skidBuffer[tid].front()->isLoad()) {
            toRename->iewInfo[tid].dispatchedToLQ++;
        }
        if (skidBuffer[tid].front()->isStore()) {
            toRename->iewInfo[tid].dispatchedToSQ++;
        }

        toRename->iewInfo[tid].dispatched++;

        skidBuffer[tid].pop();
    }

    emptyRenameInsts(tid);
}

template<class Impl>
void
DefaultIEW<Impl>::squashDueToBranch(DynInstPtr &inst, ThreadID tid)
{
    DPRINTF(IEW, "[tid:%i]: Squashing from a specific instruction, PC: %s "
            "[sn:%i].\n", tid, inst->pcState(), inst->seqNum);

    if (!toCommit->squash[tid] ||
            inst->seqNum < toCommit->squashedSeqNum[tid]) {
        toCommit->squash[tid] = true;
        toCommit->squashedSeqNum[tid] = inst->seqNum;
        toCommit->branchTaken[tid] = inst->pcState().branching();

        TheISA::PCState pc = inst->pcState();
        TheISA::advancePC(pc, inst->staticInst);

        toCommit->pc[tid] = pc;
        toCommit->mispredictInst[tid] = inst;
        toCommit->includeSquashInst[tid] = false;

        wroteToTimeBuffer = true;
    }

}

template<class Impl>
void
DefaultIEW<Impl>::squashDueToMemOrder(DynInstPtr &inst, ThreadID tid)
{
    DPRINTF(IEW, "[tid:%i]: Memory violation, squashing violator and younger "
            "insts, PC: %s [sn:%i].\n", tid, inst->pcState(), inst->seqNum);
    // Need to include inst->seqNum in the following comparison to cover the
    // corner case when a branch misprediction and a memory violation for the
    // same instruction (e.g. load PC) are detected in the same cycle.  In this
    // case the memory violator should take precedence over the branch
    // misprediction because it requires the violator itself to be included in
    // the squash.
    if (!toCommit->squash[tid] ||
            inst->seqNum <= toCommit->squashedSeqNum[tid]) {
        toCommit->squash[tid] = true;

        toCommit->squashedSeqNum[tid] = inst->seqNum;
        toCommit->pc[tid] = inst->pcState();
        toCommit->mispredictInst[tid] = NULL;

        // Must include the memory violator in the squash.
        toCommit->includeSquashInst[tid] = true;

        wroteToTimeBuffer = true;
    }
}

template<class Impl>
void
DefaultIEW<Impl>::block(ThreadID tid)
{
    DPRINTF(IEW, "[tid:%u]: Blocking.\n", tid);

    if (dispatchStatus[tid] != Blocked &&
        dispatchStatus[tid] != Unblocking) {
        toRename->iewBlock[tid] = true;
        wroteToTimeBuffer = true;
    }

    // Add the current inputs to the skid buffer so they can be
    // reprocessed when this stage unblocks.
    skidInsert(tid);

    dispatchStatus[tid] = Blocked;
}

template<class Impl>
void
DefaultIEW<Impl>::unblock(ThreadID tid)
{
    DPRINTF(IEW, "[tid:%i]: Reading instructions out of the skid "
            "buffer %u.\n",tid, tid);

    // If the skid bufffer is empty, signal back to previous stages to unblock.
    // Also switch status to running.
    if (skidBuffer[tid].empty()) {
        toRename->iewUnblock[tid] = true;
        wroteToTimeBuffer = true;
        DPRINTF(IEW, "[tid:%i]: Done unblocking.\n",tid);
        dispatchStatus[tid] = Running;
    }
}

template<class Impl>
void
DefaultIEW<Impl>::wakeDependents(DynInstPtr &inst)
{
    instQueue.wakeDependents(inst);
}

template<class Impl>
void
DefaultIEW<Impl>::rescheduleMemInst(DynInstPtr &inst)
{
    instQueue.rescheduleMemInst(inst);
}

template<class Impl>
void
DefaultIEW<Impl>::replayMemInst(DynInstPtr &inst)
{
    instQueue.replayMemInst(inst);
}

template<class Impl>
void
DefaultIEW<Impl>::blockMemInst(DynInstPtr& inst)
{
    instQueue.blockMemInst(inst);
}

template<class Impl>
void
DefaultIEW<Impl>::cacheUnblocked()
{
    instQueue.cacheUnblocked();
}

template<class Impl>
void
DefaultIEW<Impl>::instToCommit(DynInstPtr &inst)
{
    // This function should not be called after writebackInsts in a
    // single cycle.  That will cause problems with an instruction
    // being added to the queue to commit without being processed by
    // writebackInsts prior to being sent to commit.

    // First check the time slot that this instruction will write
    // to.  If there are free write ports at the time, then go ahead
    // and write the instruction to that time.  If there are not,
    // keep looking back to see where's the first time there's a
    // free slot.
    while ((*iewQueue)[wbCycle].insts[wbNumInst]) {
        ++wbNumInst;
        if (wbNumInst == wbWidth) {
            ++wbCycle;
            wbNumInst = 0;
        }
    }

    DPRINTF(IEW, "Current wb cycle: %i, width: %i, numInst: %i, wbActual:%i\n",
            wbCycle, wbWidth, wbNumInst, wbCycle * wbWidth + wbNumInst);
    // Add finished instruction to queue to commit.
    (*iewQueue)[wbCycle].insts[wbNumInst] = inst;
    (*iewQueue)[wbCycle].size++;
}

template <class Impl>
unsigned
DefaultIEW<Impl>::validInstsFromRename()
{
    unsigned inst_count = 0;

    for (int i=0; i<fromRename->size; i++) {
        if (!fromRename->insts[i]->isSquashed())
            inst_count++;
    }

    return inst_count;
}

template<class Impl>
void
DefaultIEW<Impl>::skidInsert(ThreadID tid)
{
    DynInstPtr inst = NULL;

    while (!insts[tid].empty()) {
        inst = insts[tid].front();

        insts[tid].pop();

        DPRINTF(IEW,"[tid:%i]: Inserting [sn:%lli] PC:%s into "
                "dispatch skidBuffer %i\n",tid, inst->seqNum,
                inst->pcState(),tid);

        skidBuffer[tid].push(inst);
    }

    assert(skidBuffer[tid].size() <= skidBufferMax &&
           "Skidbuffer Exceeded Max Size");
}

template<class Impl>
int
DefaultIEW<Impl>::skidCount()
{
    int max=0;

    list<ThreadID>::iterator threads = activeThreads->begin();
    list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;
        unsigned thread_count = skidBuffer[tid].size();
        if (max < thread_count)
            max = thread_count;
    }

    return max;
}

template<class Impl>
bool
DefaultIEW<Impl>::skidsEmpty()
{
    list<ThreadID>::iterator threads = activeThreads->begin();
    list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (!skidBuffer[tid].empty())
            return false;
    }

    return true;
}

template <class Impl>
void
DefaultIEW<Impl>::updateStatus()
{
    bool any_unblocking = false;

    list<ThreadID>::iterator threads = activeThreads->begin();
    list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (dispatchStatus[tid] == Unblocking) {
            any_unblocking = true;
            break;
        }
    }

    // If there are no ready instructions waiting to be scheduled by the IQ,
    // and there's no stores waiting to write back, and dispatch is not
    // unblocking, then there is no internal activity for the IEW stage.
    instQueue.intInstQueueReads++;
    if (_status == Active && !instQueue.hasReadyInsts() &&
        !ldstQueue.willWB() && !any_unblocking) {
        DPRINTF(IEW, "IEW switching to idle\n");

        deactivateStage();

        _status = Inactive;
    } else if (_status == Inactive && (instQueue.hasReadyInsts() ||
                                       ldstQueue.willWB() ||
                                       any_unblocking)) {
        // Otherwise there is internal activity.  Set to active.
        DPRINTF(IEW, "IEW switching to active\n");

        activateStage();

        _status = Active;
    }
}

template <class Impl>
void
DefaultIEW<Impl>::resetEntries()
{
    instQueue.resetEntries();
    ldstQueue.resetEntries();
}

template <class Impl>
bool
DefaultIEW<Impl>::checkStall(ThreadID tid)
{
    bool ret_val(false);

    if (fromCommit->commitInfo[tid].robSquashing) {
        DPRINTF(IEW,"[tid:%i]: Stall from Commit stage detected.\n",tid);
        ret_val = true;
    } else if (instQueue.isFull(tid)) {
        DPRINTF(IEW,"[tid:%i]: Stall: IQ  is full.\n",tid);
        ret_val = true;
    }

    return ret_val;
}

template <class Impl>
void
DefaultIEW<Impl>::checkSignalsAndUpdate(ThreadID tid)
{
    // Check if there's a squash signal, squash if there is
    // Check stall signals, block if there is.
    // If status was Blocked
    //     if so then go to unblocking
    // If status was Squashing
    //     check if squashing is not high.  Switch to running this cycle.

    if (fromCommit->commitInfo[tid].squash) {
        squash(tid);

        if (dispatchStatus[tid] == Blocked ||
            dispatchStatus[tid] == Unblocking) {
            toRename->iewUnblock[tid] = true;
            wroteToTimeBuffer = true;
        }

        dispatchStatus[tid] = Squashing;
        fetchRedirect[tid] = false;
        return;
    }

    if (fromCommit->commitInfo[tid].robSquashing) {
        DPRINTF(IEW, "[tid:%i]: ROB is still squashing.\n", tid);

        dispatchStatus[tid] = Squashing;
        emptyRenameInsts(tid);
        wroteToTimeBuffer = true;
    }

    if (checkStall(tid)) {
        block(tid);
        dispatchStatus[tid] = Blocked;
        return;
    }

    if (dispatchStatus[tid] == Blocked) {
        // Status from previous cycle was blocked, but there are no more stall
        // conditions.  Switch over to unblocking.
        DPRINTF(IEW, "[tid:%i]: Done blocking, switching to unblocking.\n",
                tid);

        dispatchStatus[tid] = Unblocking;

        unblock(tid);

        return;
    }

    if (dispatchStatus[tid] == Squashing) {
        // Switch status to running if rename isn't being told to block or
        // squash this cycle.
        DPRINTF(IEW, "[tid:%i]: Done squashing, switching to running.\n",
                tid);

        dispatchStatus[tid] = Running;

        return;
    }
}

template <class Impl>
void
DefaultIEW<Impl>::sortInsts()
{
    int insts_from_rename = fromRename->size;
#ifdef DEBUG
    for (ThreadID tid = 0; tid < numThreads; tid++)
        assert(insts[tid].empty());
#endif
    for (int i = 0; i < insts_from_rename; ++i) {
        insts[fromRename->insts[i]->threadNumber].push(fromRename->insts[i]);
    }
}

template <class Impl>
void
DefaultIEW<Impl>::emptyRenameInsts(ThreadID tid)
{
    DPRINTF(IEW, "[tid:%i]: Removing incoming rename instructions\n", tid);

    while (!insts[tid].empty()) {

        if (insts[tid].front()->isLoad()) {
            toRename->iewInfo[tid].dispatchedToLQ++;
        }
        if (insts[tid].front()->isStore()) {
            toRename->iewInfo[tid].dispatchedToSQ++;
        }

        toRename->iewInfo[tid].dispatched++;

        insts[tid].pop();
    }
}

template <class Impl>
void
DefaultIEW<Impl>::wakeCPU()
{
    cpu->wakeCPU();
}

template <class Impl>
void
DefaultIEW<Impl>::activityThisCycle()
{
    DPRINTF(Activity, "Activity this cycle.\n");
    cpu->activityThisCycle();
}

template <class Impl>
inline void
DefaultIEW<Impl>::activateStage()
{
    DPRINTF(Activity, "Activating stage.\n");
    cpu->activateStage(O3CPU::IEWIdx);
}

template <class Impl>
inline void
DefaultIEW<Impl>::deactivateStage()
{
    DPRINTF(Activity, "Deactivating stage.\n");
    cpu->deactivateStage(O3CPU::IEWIdx);
}

template<class Impl>
void
DefaultIEW<Impl>::dispatch(ThreadID tid)
{
    // If status is Running or idle,
    //     call dispatchInsts()
    // If status is Unblocking,
    //     buffer any instructions coming from rename
    //     continue trying to empty skid buffer
    //     check if stall conditions have passed

    if (dispatchStatus[tid] == Blocked) {
        ++iewBlockCycles;

    } else if (dispatchStatus[tid] == Squashing) {
        ++iewSquashCycles;
    }

    // Dispatch should try to dispatch as many instructions as its bandwidth
    // will allow, as long as it is not currently blocked.
    if (dispatchStatus[tid] == Running ||
        dispatchStatus[tid] == Idle) {
        DPRINTF(IEW, "[tid:%i] Not blocked, so attempting to run "
                "dispatch.\n", tid);

        dispatchInsts(tid);
    } else if (dispatchStatus[tid] == Unblocking) {
        // Make sure that the skid buffer has something in it if the
        // status is unblocking.
        assert(!skidsEmpty());

        // If the status was unblocking, then instructions from the skid
        // buffer were used.  Remove those instructions and handle
        // the rest of unblocking.
        dispatchInsts(tid);

        ++iewUnblockCycles;

        if (validInstsFromRename()) {
            // Add the current inputs to the skid buffer so they can be
            // reprocessed when this stage unblocks.
            skidInsert(tid);
        }

        unblock(tid);
    }
}

template <class Impl>
void
DefaultIEW<Impl>::dispatchInsts(ThreadID tid)
{
    // Obtain instructions from skid buffer if unblocking, or queue from rename
    // otherwise.
    std::queue<DynInstPtr> &insts_to_dispatch =
        dispatchStatus[tid] == Unblocking ?
        skidBuffer[tid] : insts[tid];

    int insts_to_add = insts_to_dispatch.size();

	DynInstPtr inst;
    bool add_to_iq = false;
    int dis_num_inst = 0;

	/* Update IHT (IXU History Table) on every cycle */
	updateIHTBeforeDispatch();

    // Loop through the instructions, putting them in the instruction
    // queue.
    for ( ; dis_num_inst < insts_to_add &&
              dis_num_inst < dispatchWidth;
          ++dis_num_inst)
    {
        inst = insts_to_dispatch.front();

        if (dispatchStatus[tid] == Unblocking) {
            DPRINTF(IEW, "[tid:%i]: Issue: Examining instruction from skid "
                    "buffer\n", tid);
        }

        // Make sure there's a valid instruction there.
        assert(inst);

        DPRINTF(IEW, "[tid:%i]: Issue: Adding PC %s [sn:%lli] [tid:%i] to "
                "IQ.\n",
                tid, inst->pcState(), inst->seqNum, inst->threadNumber);

        // Be sure to mark these instructions as ready so that the
        // commit stage can go ahead and execute them, and mark
        // them as issued so the IQ doesn't reprocess them.

        // Check for squashed instructions.
        if (inst->isSquashed()) {
            DPRINTF(IEW, "[tid:%i]: Issue: Squashed instruction encountered, "
                    "not adding to IQ.\n", tid);

            ++iewDispSquashedInsts;

            insts_to_dispatch.pop();

            //Tell Rename That An Instruction has been processed
            if (inst->isLoad()) {
                toRename->iewInfo[tid].dispatchedToLQ++;
            }
            if (inst->isStore()) {
                toRename->iewInfo[tid].dispatchedToSQ++;
            }

            toRename->iewInfo[tid].dispatched++;

            continue;
        }

	/********************************************************* 
	 *
	 *  This is for Parallel IXU
	 * 
	 * Only none of memory instructions and all of instructions of 
	 * 1 execution latency have to be checked
	 *
	 * *******************************************************/

		if(isIXUUsed == true)
		{
			/* dmb instruction can't be handled in IXU */
		if (!inst->isMemBarrier() && !inst->isWriteBarrier() && !inst->isNonSpeculative())
		{
		/* check if an instruction is not memory instruction 
		 * or has one operation latency */
		if(!inst->isMemRef() && !inst->isLoad() 
				&& !inst->isStore() && getLatency(inst) == 1)
		{
			int ixu_buf_size = buffer_of_ixu[0].size();

			/* check an instruction can enter IXU */
			if(canInstEnterIXU(inst) && (ixu_buf_size < ixuWidth))
			{
				/* count number of forwarding count stats */
				countNumOfForwarding(inst);

				ixuEnteredInsts++;

				/* Updates dest regs of instruction in IHT */
				setDestRegInIHT(inst);

				DPRINTF(IEW, "[tid:%i]: IXU: instruction is moved to IXU. [sn:%i]\n"
						, tid, inst->seqNum);

				/* this is for committing an instruction */
				instQueue.insertFromIXU(inst);

				/* we need to update another scoreboard, placed in inst_queue */
				instQueue.addToDependentsFromIXU(inst);

				/* Insert instruction to buffer in IXU */
				buffer_of_ixu[0].push_back(inst);
				insts_to_dispatch.pop();

				/* signal to rename stage */
        		toRename->iewInfo[tid].dispatched++;

        		ppDispatch->notify(inst);

				continue;
			}
			else
			{
				DPRINTF(IEW, "[tid:%i]: IXU: instruction is moved to OXU. [sn:%i]\n",
						tid, inst->seqNum);
			}
			/* If an instruction can't enter IXU, just go to OXU */
		}
		else
		{
			DPRINTF(IEW, "[tid:%i]: IXU: instruction is moved to OXU. [sn:%i]\n",
					tid, inst->seqNum);
		}

		} // MemBarrier & WriteBarrier
		else
		{
			DPRINTF(IEW, "[tid:%i]: an instruction is MemBarrier or WriteBarrier. [sn:%i]\n",
					tid, inst->seqNum);
		}

		}
	/********************************************************/

        // Check for full conditions.
        if (instQueue.isFull(tid)) {
            DPRINTF(IEW, "[tid:%i]: Issue: IQ has become full.\n", tid);

            // Call function to start blocking.
            block(tid);

            // Set unblock to false. Special case where we are using
            // skidbuffer (unblocking) instructions but then we still
            // get full in the IQ.
            toRename->iewUnblock[tid] = false;

            ++iewIQFullEvents;
            break;
        }

        // Check LSQ if inst is LD/ST
        if ((inst->isLoad() && ldstQueue.lqFull(tid)) ||
            (inst->isStore() && ldstQueue.sqFull(tid))) {
            DPRINTF(IEW, "[tid:%i]: Issue: %s has become full.\n",tid,
                    inst->isLoad() ? "LQ" : "SQ");

            // Call function to start blocking.
            block(tid);

            // Set unblock to false. Special case where we are using
            // skidbuffer (unblocking) instructions but then we still
            // get full in the IQ.
            toRename->iewUnblock[tid] = false;

            ++iewLSQFullEvents;
            break;
        }

        // Otherwise issue the instruction just fine.
        if (inst->isLoad()) {
            DPRINTF(IEW, "[tid:%i]: Issue: Memory instruction "
                    "encountered, adding to LSQ.\n", tid);

            // Reserve a spot in the load store queue for this
            // memory access.
            ldstQueue.insertLoad(inst);

            ++iewDispLoadInsts;

            add_to_iq = true;

            toRename->iewInfo[tid].dispatchedToLQ++;
        } else if (inst->isStore()) {
            DPRINTF(IEW, "[tid:%i]: Issue: Memory instruction "
                    "encountered, adding to LSQ.\n", tid);

            ldstQueue.insertStore(inst);

            ++iewDispStoreInsts;

            if (inst->isStoreConditional()) {
                // Store conditionals need to be set as "canCommit()"
                // so that commit can process them when they reach the
                // head of commit.
                // @todo: This is somewhat specific to Alpha.
				DPRINTF(IEW, "Issue: Store instruction is conditional. [sn:%i]",
						inst->seqNum);
                inst->setCanCommit();
                instQueue.insertNonSpec(inst);
                add_to_iq = false;

			if(isBCUsed == true)
			{
				// if instruction is bundle mode
				if(inst->bundle_info != NULL)
				{
					setExceptionInBQ(inst);
				}
			}

                ++iewDispNonSpecInsts;
            } else {
                add_to_iq = true;
            }

            toRename->iewInfo[tid].dispatchedToSQ++;
        } else if (inst->isMemBarrier() || inst->isWriteBarrier()) {
            // Same as non-speculative stores.
            inst->setCanCommit();
            instQueue.insertBarrier(inst);
            add_to_iq = false;

			if(isBCUsed == true)
			{
				// if instruction is bundle mode
				if(inst->bundle_info != NULL)
				{
					setExceptionInBQ(inst);
				}
			}

        } else if (inst->isNop()) {
            DPRINTF(IEW, "[tid:%i]: Issue: Nop instruction encountered, "
                    "skipping.\n", tid);

            inst->setIssued();
            inst->setExecuted();
            inst->setCanCommit();

            instQueue.recordProducer(inst);

            iewExecutedNop[tid]++;

            add_to_iq = false;
        } else {
            assert(!inst->isExecuted());
            add_to_iq = true;
        }

        if (inst->isNonSpeculative()) {
            DPRINTF(IEW, "[tid:%i]: Issue: Nonspeculative instruction "
                    "encountered, skipping.\n", tid);

            // Same as non-speculative stores.
            inst->setCanCommit();

            // Specifically insert it as nonspeculative.
            instQueue.insertNonSpec(inst);

			if(isBCUsed == true)
			{
				// if instruction is bundle mode
				if(inst->bundle_info != NULL)
				{
					setExceptionInBQ(inst);
				}
			}

            ++iewDispNonSpecInsts;

            add_to_iq = false;
        }

        // If the instruction queue is not full, then add the
        // instruction.
        if (add_to_iq) {
            instQueue.insert(inst);
        }

        insts_to_dispatch.pop();

        toRename->iewInfo[tid].dispatched++;

        ++iewDispatchedInsts;

#if TRACING_ON
        inst->dispatchTick = curTick() - inst->fetchTick;
#endif
        ppDispatch->notify(inst);
    }

    if (!insts_to_dispatch.empty()) {
        DPRINTF(IEW,"[tid:%i]: Issue: Bandwidth Full. Blocking.\n", tid);
        block(tid);
        toRename->iewUnblock[tid] = false;
    }

    if (dispatchStatus[tid] == Idle && dis_num_inst) {
        dispatchStatus[tid] = Running;

        updatedQueues = true;
    }

    dis_num_inst = 0;
}

template <class Impl>
void
DefaultIEW<Impl>::printAvailableInsts()
{
    int inst = 0;

    std::cout << "Available Instructions: ";

    while (fromIssue->insts[inst]) {

        if (inst%3==0) std::cout << "\n\t";

        std::cout << "PC: " << fromIssue->insts[inst]->pcState()
             << " TN: " << fromIssue->insts[inst]->threadNumber
             << " SN: " << fromIssue->insts[inst]->seqNum << " | ";

        inst++;

    }

    std::cout << "\n";
}

template <class Impl>
void
DefaultIEW<Impl>::executeInsts()
{
    wbNumInst = 0;
    wbCycle = 0;

    list<ThreadID>::iterator threads = activeThreads->begin();
    list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;
        fetchRedirect[tid] = false;
    }

	/********************************************************
	 *
	 *  IXU is operated right here
	 *
	 *  buf_idx:3 -> Last stage
	 *  buf_idx:2 -> 2nd stage
	 *  buf_idx:1 -> 1st stage
	 *
	 * *****************************************************/

	if(isIXUUsed == true)
	{
	/* Through buffers in IXU, updates instruction's status */
	updateInstInIXUBuffer();

	/* 1. Data is fetched from buffer of 3rd stage */
	for(int buf_idx = ixuDepth; buf_idx >= 1; buf_idx--)
	{
		int num_of_insts_ixu_buffer = buffer_of_ixu[buf_idx].size();

		if(num_of_insts_ixu_buffer > ixuWidth)
		{
			DPRINTF(IEW, "IXU: Buffer size of %dth stage is exceeded.\n", ixuDepth);
			DPRINTF(IEW, "IXU: num of insts in %dth buffer: %d\n", 
					buf_idx, num_of_insts_ixu_buffer);

			assert(num_of_insts_ixu_buffer <= ixuWidth);
		}

		for(int idx = 0; idx < num_of_insts_ixu_buffer; idx++)
		{
			DynInstPtr inst = buffer_of_ixu[buf_idx].front();

			// Notify potential listeners that this instruction has started
			// executing
			ppExecute->notify(inst);

			/* Check if the instruction is squashed, so skip it */
			if(inst->isSquashed())
			{
				DPRINTF(IEW, "IXU: Instruction was squashed. PC: %s, [tid:%i]"
							 " [sn:%i]\n", inst->pcState(), inst->threadNumber,
							 inst->seqNum);

			/* Consider the instruction is executed so that commit can go ahead 
			 * and retire the instruction */
				inst->setExecuted();

            /* Not sure if I should set this here or just let commit try to
             	commit any squashed instructions.  I like the latter a bit more.*/
				inst->setCanCommit();

				buffer_of_ixu[buf_idx].pop_front();

				/* Reset IHT's left cycle to -1 */
				resetIHTatSquash(inst);
				continue;
			}

			/* Double check whether instruction is for memory operation 
			 * and is ready to issue */
			if(!inst->isMemRef() && inst->readyToIssue())
			{
				if(inst->getFault() == NoFault)
				{
					DPRINTF(IEW, "IXU: Instruction is executed in %dth stage. "
							" [sn:%i]\n", buf_idx, inst->seqNum);
					inst->execute();

					/* for measuring stats */
					ixuExecInsts++;

					if(buf_idx == 1)
						ixuExecIn1st++;
					else if(buf_idx == 2)
						ixuExecIn2nd++;
					else if(buf_idx == 3)
						ixuExecIn3rd++;
					else if(buf_idx == 4)
						ixuExecIn4th++;

					if(!inst->readPredicate())
						inst->forwardOldRegs();
				}

				inst->setExecuted();
				instToCommit(inst);
			}
			/* If an instruction can't be executed at last IXU satge, 
			 * it'll cause problems */
			else
			{
				DPRINTF(IEW, "IXU: There is still left instruction that is not "
						"executed in IXU. [sn:%i]\n", inst->seqNum);
			}

			updateExeInstStats(inst);

			ThreadID tid = inst->threadNumber;
			/* Check if branch instruction wasn't correct 
			 * we have to tell commit to squash in flight instructions */
			if(!fetchRedirect[tid] || !toCommit->squash[tid]
				|| toCommit->squashedSeqNum[tid] > inst->seqNum)
			{
            	bool loadNotExecuted_in_IXU = !inst->isExecuted() && inst->isLoad();

				if(inst->mispredicted() && !loadNotExecuted_in_IXU 
						&& inst->readyToIssue())
				{
					fetchRedirect[tid] = true;

					DPRINTF(IEW, "IXU: Branch mispredict detected. [sn:%i]\n", inst->seqNum);
					DPRINTF(IEW, "IXU: Predicted target was PC: %s.\n",
							inst->readPredTarg());
					DPRINTF(IEW, "IXU: Execute: Redirecting fetch to PC: %s.\n",
							inst->pcState());

					squashDueToBranch(inst, tid);

					ppMispredict->notify(inst);

					if(inst->readPredTaken())
						predictedTakenIncorrect++;
					else
						predictedNotTakenIncorrect++;

					/* Setting branch instruction's destination ready and OXU */
					setDestRegReady(inst, tid);
				}
			}

			/* Writeback instruction in advance, because some instructions such as
			 * ldr require writeback and require to make src reg as ready maybe...*/
			if(inst->isExecuted() && !inst->mispredicted())
			{
				writebackInstInIXU(inst);

				setDestRegReady(inst, tid);
			}

			buffer_of_ixu[buf_idx].pop_front();

			if(buf_idx == ixuDepth && !inst->isExecuted())
			{
				DPRINTF(IEW, "IXU: there is not executed in %dth stage. [sn:%i]\n", 
						ixuDepth, inst->seqNum);
				assert(buf_idx != ixuDepth || inst->isExecuted());
			}

			/* Not executed instructions is lifted to next IXU stage 
			 * except for 3rd IXU stage */
			if(buf_idx != ixuDepth && !inst->isExecuted())
			{
				DPRINTF(IEW, "IXU: Instruction can't be executed in %dth stage, so "
				"moved to %dth stage's buffer. [sn:%i]\n", buf_idx, buf_idx+1, inst->seqNum);
				buffer_of_ixu[buf_idx+1].push_back(inst);
			}

		}
		
	}

	/* Move instruction from temp buffer[0] to buffer[1] */
	moveInstsToBuffer();

	}
	/*********************************************************/

    // Uncomment this if you want to see all available instructions.
    // @todo This doesn't actually work anymore, we should fix it.
//    printAvailableInsts();

    // Execute/writeback any instructions that are available.
    int insts_to_execute = fromIssue->size;
    int inst_num = 0;
    for (; inst_num < insts_to_execute;
          ++inst_num) {

        DPRINTF(IEW, "Execute: Executing instructions from IQ.\n");

        DynInstPtr inst = instQueue.getInstToExecute();

        DPRINTF(IEW, "Execute: Processing PC %s, [tid:%i] [sn:%i].\n",
                inst->pcState(), inst->threadNumber,inst->seqNum);

        // Notify potential listeners that this instruction has started
        // executing
        ppExecute->notify(inst);

        // Check if the instruction is squashed; if so then skip it
        if (inst->isSquashed()) {
            DPRINTF(IEW, "Execute: Instruction was squashed. PC: %s, [tid:%i]"
                         " [sn:%i]\n", inst->pcState(), inst->threadNumber,
                         inst->seqNum);

            // Consider this instruction executed so that commit can go
            // ahead and retire the instruction.
            inst->setExecuted();

            // Not sure if I should set this here or just let commit try to
            // commit any squashed instructions.  I like the latter a bit more.
            inst->setCanCommit();

            ++iewExecSquashedInsts;

            continue;
        }

        Fault fault = NoFault;

        // Execute instruction.
        // Note that if the instruction faults, it will be handled
        // at the commit stage.
        if (inst->isMemRef()) {
            DPRINTF(IEW, "Execute: Calculating address for memory "
                    "reference.\n");

            // Tell the LDSTQ to execute this instruction (if it is a load).
            if (inst->isLoad()) {
                // Loads will mark themselves as executed, and their writeback
                // event adds the instruction to the queue to commit
                fault = ldstQueue.executeLoad(inst);

                if (inst->isTranslationDelayed() &&
                    fault == NoFault) {
                    // A hw page table walk is currently going on; the
                    // instruction must be deferred.
                    DPRINTF(IEW, "Execute: Delayed translation, deferring "
                            "load.\n");
                    instQueue.deferMemInst(inst);
                    continue;
                }

                if (inst->isDataPrefetch() || inst->isInstPrefetch()) {
                    inst->fault = NoFault;
                }
            } else if (inst->isStore()) {
                fault = ldstQueue.executeStore(inst);

                if (inst->isTranslationDelayed() &&
                    fault == NoFault) {
                    // A hw page table walk is currently going on; the
                    // instruction must be deferred.
                    DPRINTF(IEW, "Execute: Delayed translation, deferring "
                            "store.\n");
                    instQueue.deferMemInst(inst);
                    continue;
                }

                // If the store had a fault then it may not have a mem req
                if (fault != NoFault || !inst->readPredicate() ||
                        !inst->isStoreConditional()) {
					DPRINTF(IEW, "Store instruction is fault. [sn:%i]\n", inst->seqNum);
                    // If the instruction faulted, then we need to send it along
                    // to commit without the instruction completing.
                    // Send this instruction to commit, also make sure iew stage
                    // realizes there is activity.
                    inst->setExecuted();
                    instToCommit(inst);
                    activityThisCycle();

			if(isBCUsed == true)
			{
				// if instruction is bundle mode
				if(inst->bundle_info != NULL)
				{
//					setExceptionInBQ(inst);
				}
			}

                }

                // Store conditionals will mark themselves as
                // executed, and their writeback event will add the
                // instruction to the queue to commit.
            } else {
                panic("Unexpected memory type!\n");
            }

        } else {
            // If the instruction has already faulted, then skip executing it.
            // Such case can happen when it faulted during ITLB translation.
            // If we execute the instruction (even if it's a nop) the fault
            // will be replaced and we will lose it.
            if (inst->getFault() == NoFault) {
                inst->execute();
                if (!inst->readPredicate())
                    inst->forwardOldRegs();
            }

            inst->setExecuted();

            instToCommit(inst);
        }

        updateExeInstStats(inst);

        // Check if branch prediction was correct, if not then we need
        // to tell commit to squash in flight instructions.  Only
        // handle this if there hasn't already been something that
        // redirects fetch in this group of instructions.

        // This probably needs to prioritize the redirects if a different
        // scheduler is used.  Currently the scheduler schedules the oldest
        // instruction first, so the branch resolution order will be correct.
        ThreadID tid = inst->threadNumber;

        if (!fetchRedirect[tid] ||
            !toCommit->squash[tid] ||
            toCommit->squashedSeqNum[tid] > inst->seqNum) {

            // Prevent testing for misprediction on load instructions,
            // that have not been executed.
            bool loadNotExecuted = !inst->isExecuted() && inst->isLoad();

            if (inst->mispredicted() && !loadNotExecuted) {
                fetchRedirect[tid] = true;

                DPRINTF(IEW, "Execute: Branch mispredict detected.\n");
                DPRINTF(IEW, "Predicted target was PC: %s.\n",
                        inst->readPredTarg());
                DPRINTF(IEW, "Execute: Redirecting fetch to PC: %s.\n",
                        inst->pcState());
                // If incorrect, then signal the ROB that it must be squashed.
                squashDueToBranch(inst, tid);

                ppMispredict->notify(inst);

                if (inst->readPredTaken()) {
                    predictedTakenIncorrect++;
                } else {
                    predictedNotTakenIncorrect++;
                }
            } else if (ldstQueue.violation(tid)) {
                assert(inst->isMemRef());
                // If there was an ordering violation, then get the
                // DynInst that caused the violation.  Note that this
                // clears the violation signal.
                DynInstPtr violator;
                violator = ldstQueue.getMemDepViolator(tid);

                DPRINTF(IEW, "LDSTQ detected a violation. Violator PC: %s "
                        "[sn:%lli], inst PC: %s [sn:%lli]. Addr is: %#x.\n",
                        violator->pcState(), violator->seqNum,
                        inst->pcState(), inst->seqNum, inst->physEffAddrLow);

			if(isBCUsed == true)
			{
				// if instruction is bundle mode
				if(violator->bundle_info != NULL &&
						!isBundleStart(violator))
				{
					// set exception flag in bundle for solving deadlock
					setExceptionInBQ(violator);

					// change end seq for free bundle from BQ
					setEndSeqForViolation(violator);
				}
			}

                fetchRedirect[tid] = true;

                // Tell the instruction queue that a violation has occured.
                instQueue.violation(inst, violator);

                // Squash.
                squashDueToMemOrder(violator, tid);

                ++memOrderViolationEvents;
            }
        } else {
            // Reset any state associated with redirects that will not
            // be used.
            if (ldstQueue.violation(tid)) {
                assert(inst->isMemRef());

                DynInstPtr violator = ldstQueue.getMemDepViolator(tid);

                DPRINTF(IEW, "LDSTQ detected a violation.  Violator PC: "
                        "%s, inst PC: %s.  Addr is: %#x.\n",
                        violator->pcState(), inst->pcState(),
                        inst->physEffAddrLow);
                DPRINTF(IEW, "Violation will not be handled because "
                        "already squashing\n");

			if(isBCUsed == true)
			{
				// if instruction is bundle mode
				if(violator->bundle_info != NULL &&
						isBundleStart(violator))
				{
					// set exception flag in bundle for solving deadlock
					setExceptionInBQ(violator);

					// change end seq for free bundle from BQ
					setEndSeqForViolation(violator);
				}
			}

                ++memOrderViolationEvents;
            }
        }
    }

    // Update and record activity if we processed any instructions.
    if (inst_num) {
        if (exeStatus == Idle) {
            exeStatus = Running;
        }

        updatedQueues = true;

        cpu->activityThisCycle();
    }

    // Need to reset this in case a writeback event needs to write into the
    // iew queue.  That way the writeback event will write into the correct
    // spot in the queue.
    wbNumInst = 0;

}

template <class Impl>
void
DefaultIEW<Impl>::writebackInsts()
{
    // Loop through the head of the time buffer and wake any
    // dependents.  These instructions are about to write back.  Also
    // mark scoreboard that this instruction is finally complete.
    // Either have IEW have direct access to scoreboard, or have this
    // as part of backwards communication.
    for (int inst_num = 0; inst_num < wbWidth &&
             toCommit->insts[inst_num]; inst_num++) 
	{
        DynInstPtr inst = toCommit->insts[inst_num];
        ThreadID tid = inst->threadNumber;

		if(inst->isExecInIXU == true)
		{
			DPRINTF(IEW, "IXU: Instruction already passed writeback stage [sn:%i]\n"
					, inst->seqNum);

			inst->isExecInIXU = false;
			continue;
		}

        DPRINTF(IEW, "Sending instructions to commit, [sn:%lli] PC %s.\n",
                inst->seqNum, inst->pcState());

        iewInstsToCommit[tid]++;
        // Notify potential listeners that execution is complete for this
        // instruction.
        ppToCommit->notify(inst);

        // Some instructions will be sent to commit without having
        // executed because they need commit to handle them.
        // E.g. Strictly ordered loads have not actually executed when they
        // are first sent to commit.  Instead commit must tell the LSQ
        // when it's ready to execute the strictly ordered load.
        if (!inst->isSquashed() && inst->isExecuted() && inst->getFault() == NoFault)
		{
            for (int i = 0; i < inst->numDestRegs(); i++) {
                //mark as Ready
                DPRINTF(IEW,"Setting Destination Register %i\n",
                        inst->renamedDestRegIdx(i));
                scoreboard->setReg(inst->renamedDestRegIdx(i));
            }

			if(isIXUUsed == true)
			{
				assert(inst->isExecInIXU == false);

				int dependents = instQueue.wakeDependents(inst);

				if (dependents) {
					producerInst[tid]++;
					consumerInst[tid]+= dependents;
				}

				writebackCount[tid]++;
			}
			else
			{
				int dependents = instQueue.wakeDependents(inst);

				if (dependents) {
					producerInst[tid]++;
					consumerInst[tid]+= dependents;
				}

				writebackCount[tid]++;
			}
        }
    }
}

template<class Impl>
void
DefaultIEW<Impl>::tick()
{
    wbNumInst = 0;
    wbCycle = 0;

    wroteToTimeBuffer = false;
    updatedQueues = false;

    sortInsts();

    // Free function units marked as being freed this cycle.
    fuPool->processFreeUnits();

    list<ThreadID>::iterator threads = activeThreads->begin();
    list<ThreadID>::iterator end = activeThreads->end();

    // Check stall and squash signals, dispatch any instructions.
    while (threads != end) {
        ThreadID tid = *threads++;

        DPRINTF(IEW,"Issue: Processing [tid:%i]\n",tid);

        checkSignalsAndUpdate(tid);
        dispatch(tid);
    }

    if (exeStatus != Squashing) {
        executeInsts();

        writebackInsts();

        // Have the instruction queue try to schedule any ready instructions.
        // (In actuality, this scheduling is for instructions that will
        // be executed next cycle.)
        instQueue.scheduleReadyInsts();

        // Also should advance its own time buffers if the stage ran.
        // Not the best place for it, but this works (hopefully).
        issueToExecQueue.advance();
    }

    bool broadcast_free_entries = false;

    if (updatedQueues || exeStatus == Running || updateLSQNextCycle) {
        exeStatus = Idle;
        updateLSQNextCycle = false;

        broadcast_free_entries = true;
    }

    // Writeback any stores using any leftover bandwidth.
    ldstQueue.writebackStores();

    // Check the committed load/store signals to see if there's a load
    // or store to commit.  Also check if it's being told to execute a
    // nonspeculative instruction.
    // This is pretty inefficient...

    threads = activeThreads->begin();
    while (threads != end) {
        ThreadID tid = (*threads++);

        DPRINTF(IEW,"Processing [tid:%i]\n",tid);

        // Update structures based on instructions committed.
        if (fromCommit->commitInfo[tid].doneSeqNum != 0 &&
            !fromCommit->commitInfo[tid].squash &&
            !fromCommit->commitInfo[tid].robSquashing) {

            ldstQueue.commitStores(fromCommit->commitInfo[tid].doneSeqNum,tid);

            ldstQueue.commitLoads(fromCommit->commitInfo[tid].doneSeqNum,tid);

            updateLSQNextCycle = true;
            instQueue.commit(fromCommit->commitInfo[tid].doneSeqNum,tid);
        }

        if (fromCommit->commitInfo[tid].nonSpecSeqNum != 0) {

            //DPRINTF(IEW,"NonspecInst from thread %i",tid);
            if (fromCommit->commitInfo[tid].strictlyOrdered) {
                instQueue.replayMemInst(
                    fromCommit->commitInfo[tid].strictlyOrderedLoad);
                fromCommit->commitInfo[tid].strictlyOrderedLoad->setAtCommit();
            } else {
                instQueue.scheduleNonSpec(
                    fromCommit->commitInfo[tid].nonSpecSeqNum);
            }
        }

        if (broadcast_free_entries) {
            toFetch->iewInfo[tid].iqCount =
                instQueue.getCount(tid);
            toFetch->iewInfo[tid].ldstqCount =
                ldstQueue.getCount(tid);

            toRename->iewInfo[tid].usedIQ = true;
            toRename->iewInfo[tid].freeIQEntries =
                instQueue.numFreeEntries(tid);
            toRename->iewInfo[tid].usedLSQ = true;

            toRename->iewInfo[tid].freeLQEntries =
                ldstQueue.numFreeLoadEntries(tid);
            toRename->iewInfo[tid].freeSQEntries =
                ldstQueue.numFreeStoreEntries(tid);

            wroteToTimeBuffer = true;
        }

        DPRINTF(IEW, "[tid:%i], Dispatch dispatched %i instructions.\n",
                tid, toRename->iewInfo[tid].dispatched);
    }

    DPRINTF(IEW, "IQ has %i free entries (Can schedule: %i).  "
            "LQ has %i free entries. SQ has %i free entries.\n",
            instQueue.numFreeEntries(), instQueue.hasReadyInsts(),
            ldstQueue.numFreeLoadEntries(), ldstQueue.numFreeStoreEntries());

    updateStatus();

    if (wroteToTimeBuffer) {
        DPRINTF(Activity, "Activity this cycle.\n");
        cpu->activityThisCycle();
    }
}

template <class Impl>
void
DefaultIEW<Impl>::updateExeInstStats(DynInstPtr &inst)
{
    ThreadID tid = inst->threadNumber;

    iewExecutedInsts++;

#if TRACING_ON
    if (DTRACE(O3PipeView)) {
        inst->completeTick = curTick() - inst->fetchTick;
    }
#endif

    //
    //  Control operations
    //
    if (inst->isControl())
        iewExecutedBranches[tid]++;

    //
    //  Memory operations
    //
    if (inst->isMemRef()) {
        iewExecutedRefs[tid]++;

        if (inst->isLoad()) {
            iewExecLoadInsts[tid]++;
        }
    }
}

template <class Impl>
void
DefaultIEW<Impl>::checkMisprediction(DynInstPtr &inst)
{
    ThreadID tid = inst->threadNumber;

    if (!fetchRedirect[tid] ||
        !toCommit->squash[tid] ||
        toCommit->squashedSeqNum[tid] > inst->seqNum) {

        if (inst->mispredicted()) 
		{
            fetchRedirect[tid] = true;

            DPRINTF(IEW, "Execute: Branch mispredict detected.\n");
            DPRINTF(IEW, "Predicted target was PC:%#x, NPC:%#x.\n",
                    inst->predInstAddr(), inst->predNextInstAddr());
            DPRINTF(IEW, "Execute: Redirecting fetch to PC: %#x,"
                    " NPC: %#x.\n", inst->nextInstAddr(),
                    inst->nextInstAddr());
            // If incorrect, then signal the ROB that it must be squashed.
            squashDueToBranch(inst, tid);

            if (inst->readPredTaken()) {
                predictedTakenIncorrect++;
            } else {
                predictedNotTakenIncorrect++;
            }
        }
		else if(!inst->mispredicted())
		{
			DPRINTF(IEW, "Execute: Branch prediction is correct [sn:%i]\n",
					inst->seqNum);
		}
    }
}

template <typename Impl>
int DefaultIEW<Impl>::getLatency(DynInstPtr& _inst)
{
	Cycles op_latency = Cycles(1);
	OpClass op_class = _inst->opClass();

	if(op_class != No_OpClass)
	{
		op_latency = fuPool->getOpLatency(op_class);
	}

	return (int)op_latency;
}

template <typename Impl>
void DefaultIEW<Impl>::initializeIHT(IXU_history_entries *_IHT)
{
	_IHT->execution_type = OXU;
	_IHT->left_cycle = -1;
}

template <typename Impl>
bool DefaultIEW<Impl>::isAvailableInIXU(int src_reg)
{
	if(IXU_history_table[src_reg]->execution_type == IXU)
	{
		return true;
	}
	else
	{
		return false;
	}
}

template <typename Impl>
bool DefaultIEW<Impl>::canInstEnterIXU(DynInstPtr &inst)
{
	/* 1. instruction can be executed immediately */
	if(inst->readyToIssue())
	{
		DPRINTF(IEW, "IXU: instruction is ready to issue to IXU [sn:%i]\n",
				inst->seqNum);
		return true;
	}

	/* 2. instruction can get dependent data by forwarding result 
	 * of preceeding instruction */
	int num_of_src_regs = (int)inst->numSrcRegs();
//	int longest_left_cycle = 0;

	for(int src_idx=0; src_idx<num_of_src_regs; src_idx++)
	{
		int src_reg = (int)inst->getSrcRegister(src_idx);
		/* 1. If src reg is already ready state */
		if(inst->isReadySrcRegIdx(src_idx))
		{
			DPRINTF(IEW, "IXU: physical src reg %d is ready [sn:%i]\n",src_reg,
					inst->seqNum);
			continue;
		}
		/* 2. src reg is not ready, but can get data of src reg in IXU */
		else if(isAvailableInIXU(src_reg))
		{
			/* If left cycle exceeds IXU depth */
			if(IXU_history_table[src_reg]->left_cycle >= ixuDepth - 1)
			{
				return false;
			}

			DPRINTF(IEW, "IXU: physical src reg %d's leftcycle: %d [sn:%i]\n",
					src_reg, IXU_history_table[src_reg]->left_cycle, inst->seqNum);
			continue;
		}
		/* 3. src reg is never available when instruction run in IXU */
		else
		{
			return false;
		}
	}

	return true;
}

template <typename Impl>
int DefaultIEW<Impl>::getLongestLeftCycle(DynInstPtr &inst)
{
	if(inst->readyToIssue())
	{
		return -1;
	}

	int num_of_src_regs = (int)inst->numSrcRegs();
	int longest_left_cycle = -1;

	for(int src_idx=0; src_idx<num_of_src_regs; src_idx++)
	{
		if(inst->isReadySrcRegIdx(src_idx))
			continue;

		int src_reg = (int)inst->getSrcRegister(src_idx);
		int src_cycle = IXU_history_table[src_reg]->left_cycle;

		assert(src_cycle <= ixuDepth-1);

		/* Update longest left cycle among src reg requiring result */
		if(longest_left_cycle < src_cycle)
		{
			longest_left_cycle = src_cycle;
		}
	}

	return longest_left_cycle;
}

template <typename Impl>
void DefaultIEW<Impl>::setDestRegInIHT(DynInstPtr &inst)
{
	int num_of_dest_regs = (int)inst->numDestRegs();

	inst->isExecInIXU = true;

	for(int dest_idx = 0; dest_idx<num_of_dest_regs; dest_idx++)
	{
		int dest_reg = (int)inst->getDestRegister(dest_idx);
		assert(IXU_history_table[dest_reg]->execution_type == OXU);

		IXU_history_table[dest_reg]->execution_type = IXU;
		IXU_history_table[dest_reg]->left_cycle = getLongestLeftCycle(inst) + 1;
	}
}

template <typename Impl>
bool DefaultIEW<Impl>::isExecutedInIXU(DynInstPtr &inst)
{
	bool result = false;
	int count = 0;

	for(int dest_idx = 0; dest_idx < inst->numDestRegs(); dest_idx++)
	{
		if(IXU_history_table[(int)inst->getDestRegister(dest_idx)]->execution_type
				== IXU)
		{
			count++;
		}
	}

	if(count == inst->numDestRegs())
	{
		result = true;
	}

	return result;
}

template <typename Impl>
void DefaultIEW<Impl>::updateInstInIXUBuffer(void)
{
	/* Select buffer of which stage */
	for(int idx=1; idx<=ixuDepth; idx++)
	{
		int buf_size = buffer_of_ixu[idx].size();

		/* Select instruction in buffer of IXU */
		for(int buf_idx=0; buf_idx<buf_size; buf_idx++)
		{
			DynInstPtr inst = buffer_of_ixu[idx][buf_idx];
			bool isIssuable = true;
			int num_of_src_regs = inst->numSrcRegs();

		/* Check source register of an instruction */
			for(int src_idx=0; src_idx < num_of_src_regs; src_idx++)
			{
				int src_reg = inst->getSrcRegister(src_idx);

		/* If any source register is not ready */
				if(!scoreboard->getReg(src_reg))
				{
					isIssuable = false;
					DPRINTF(IEW, "IXU: Instruction[sn:%i] is not ready (Src:%d).\n",
							inst->seqNum, src_reg);
					break;
				}
			}

			if(isIssuable == true)
			{
				DPRINTF(IEW, "IXU: Instruction is ready to issue in %dth buffer. [sn:%i]\n",
						idx, inst->seqNum);
				inst->setCanIssue();
			}
		}
	}
}

template <typename Impl>
void DefaultIEW<Impl>::moveInstsToBuffer(void)
{
	int buf_size = buffer_of_ixu[0].size();

	for(int idx=0; idx<buf_size; idx++)
	{
		DynInstPtr inst = buffer_of_ixu[0].front();

		DPRINTF(IEW, "IXU: Instruction is moved from 0th buffer to 1st buffer. "
				" [sn:%i]\n", inst->seqNum);

		buffer_of_ixu[1].push_back(inst);
		buffer_of_ixu[0].pop_front();
	}
}

template <typename Impl>
void DefaultIEW<Impl>::updateIHTBeforeDispatch(void)
{
	/* Select each buffer in front of IXU */
	for(int buf_idx=1; buf_idx<=ixuDepth; buf_idx++)
	{
		int num_of_insts = buffer_of_ixu[buf_idx].size();

		/* Select each instruction in buffer */
		for(int idx=0; idx<num_of_insts; idx++)
		{
			DynInstPtr inst = buffer_of_ixu[buf_idx][idx];
			int num_of_dest_reg = inst->numDestRegs();

			/* Select dest reg index in an instruction */
			for(int reg_idx=0; reg_idx<num_of_dest_reg; reg_idx++)
			{
				int dest_reg = inst->getDestRegister(reg_idx);

				if(IXU_history_table[dest_reg]->execution_type == IXU 
						&& IXU_history_table[dest_reg]->left_cycle >= 0)
					IXU_history_table[dest_reg]->left_cycle--;
			}
		}
	}
}

template <typename Impl>
void DefaultIEW<Impl>::resetIHTatSquash(DynInstPtr &inst)
{
	int num_of_dest_regs = (int)inst->numDestRegs();

	inst->isExecInIXU = false;

	for(int dest_idx=0; dest_idx<num_of_dest_regs; dest_idx++)
	{
		int dest_reg = inst->getDestRegister(dest_idx);

		IXU_history_table[dest_reg]->execution_type = OXU;
		IXU_history_table[dest_reg]->left_cycle = -1;
	}
}

template <typename Impl>
void DefaultIEW<Impl>::writebackInstInIXU(DynInstPtr &inst)
{
	/* Deosn't access instruction queue */
	numOfIQAccessFronIXU++;

	int dependents = instQueue.wakeDependents(inst);
    ThreadID tid = inst->threadNumber;

	if (dependents) {
		producerInst[tid]++;
		consumerInst[tid]+= dependents;
	}
}

template<typename Impl>
void DefaultIEW<Impl>::countNumOfForwarding(DynInstPtr &inst)
{
	int num_of_src_regs = (int)inst->numSrcRegs();

	for(int src_idx=0; src_idx < num_of_src_regs; src_idx++)
	{
		int src_reg = inst->getSrcRegister(src_idx);

		if(!scoreboard->getReg(src_reg))
		{
			numOfForwardingInIXU++;
		}
	}
}

template <typename Impl>
void DefaultIEW<Impl>::setDestRegReady(DynInstPtr &inst, ThreadID tid)
{
	iewInstsToCommit[tid]++;
	// Notify potential listeners that execution is complete for this
	// instruction.
	ppToCommit->notify(inst);

	if (!inst->isSquashed() && inst->isExecuted() && inst->getFault() == NoFault
			&& inst->isExecInIXU == true)
	{
		int num_of_dest_regs = inst->numDestRegs();
		for(int i=0; i<num_of_dest_regs; i++)
		{
			int dest_reg = (int)inst->renamedDestRegIdx(i);

			DPRINTF(IEW, "Setting Destination Register %i [sn:%i]\n",
					inst->renamedDestRegIdx(i), inst->seqNum);

			/* make as ready */
			scoreboard->setReg(dest_reg);

			/* update execution type of destination reg as OXU */
			IXU_history_table[dest_reg]->execution_type = OXU;
		}

		writebackCount[tid]++;
	}
}

template <typename Impl>
void DefaultIEW<Impl>::setExceptionInBQ(DynInstPtr &inst)
{
	LWModule::BundleQueueEntry *bundle_info = inst->bundle_info;

	DPRINTF(IEW, "[BC] Exception flag sets [size:%d][start_seq:%i][sn:%i]\n",
		bundle_info->size, bundle_info->start_seq, inst->seqNum);

	lwModule->setExceptionToBQ(*bundle_info);
}

template <typename Impl>
bool DefaultIEW<Impl>::isBundleStart(DynInstPtr &inst)
{
	assert(inst->bundle_info != NULL);

	bool result = false;
	LWModule::BundleQueueEntry *bundle_info = inst->bundle_info;

	if(bundle_info->start_seq == inst->seqNum)
	{
		result = true;
	}

	return result;
}

template <typename Impl>
void DefaultIEW<Impl>::setEndSeqForViolation(DynInstPtr &inst)
{
	assert(inst->bundle_info != NULL);
	LWModule::BundleQueueEntry *bundle_info = inst->bundle_info;

	assert(bundle_info->exception == true);

	DPRINTF(IEW, "[BC] Modifying end seq [sn:%i]->[sn:%i]\n",
			bundle_info->end_seq, inst->seqNum);

	lwModule->setEndSeqToBQ(*bundle_info, ((inst->seqNum)-1));
}

#endif//__CPU_O3_IEW_IMPL_IMPL_HH__
