/*
 * Copyright (C) 2016 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "SamplingProfiler.h"

#if ENABLE(SAMPLING_PROFILER)

#include "CallFrame.h"
#include "CodeBlock.h"
#include "Debugger.h"
#include "Executable.h"
#include "HeapInlines.h"
#include "HeapIterationScope.h"
#include "InlineCallFrame.h"
#include "Interpreter.h"
#include "JSCJSValueInlines.h"
#include "JSFunction.h"
#include "LLIntPCRanges.h"
#include "MarkedBlock.h"
#include "MarkedBlockSet.h"
#include "SlotVisitor.h"
#include "SlotVisitorInlines.h"
#include "VM.h"
#include "VMEntryScope.h"

namespace JSC {

static double sNumTotalStackTraces = 0;
static double sNumUnverifiedStackTraces = 0;
static double sNumTotalWalks = 0;
static double sNumFailedWalks = 0;
static const uint32_t sNumWalkReportingFrequency = 50;
static const double sWalkErrorPercentage = .05;
static const bool sReportStatsOnlyWhenTheyreAboveThreshold = false;
static const bool sReportStats = false;

using FrameType = SamplingProfiler::FrameType;

ALWAYS_INLINE static void reportStats()
{
    if (sReportStats && sNumTotalWalks && static_cast<uint64_t>(sNumTotalWalks) % sNumWalkReportingFrequency == 0) {
        if (!sReportStatsOnlyWhenTheyreAboveThreshold || (sNumFailedWalks / sNumTotalWalks > sWalkErrorPercentage)) {
            dataLogF("Num total walks: %llu. Failed walks percent: %lf\n",
                static_cast<uint64_t>(sNumTotalWalks), sNumFailedWalks / sNumTotalWalks);
            dataLogF("Total stack traces: %llu. Needs verification percent: %lf\n",
                static_cast<uint64_t>(sNumTotalStackTraces), sNumUnverifiedStackTraces / sNumTotalStackTraces);
        }
    }
}

class FrameWalker {
public:
    FrameWalker(ExecState* callFrame, VM& vm, const LockHolder& codeBlockSetLocker, const LockHolder& machineThreadsLocker)
        : m_vm(vm)
        , m_callFrame(callFrame)
        , m_vmEntryFrame(vm.topVMEntryFrame)
        , m_codeBlockSetLocker(codeBlockSetLocker)
        , m_machineThreadsLocker(machineThreadsLocker)
    {
    }

    size_t walk(Vector<SamplingProfiler::StackFrame>& stackTrace, bool& didRunOutOfSpace, bool& stacktraceNeedsVerification)
    {
        stacktraceNeedsVerification = false;
        if (sReportStats)
            sNumTotalWalks++;
        resetAtMachineFrame();
        size_t maxStackTraceSize = stackTrace.size();
        while (!isAtTop() && !m_bailingOut && m_depth < maxStackTraceSize) {
            while (m_inlineCallFrame && m_depth < maxStackTraceSize) {
                CodeBlock* codeBlock = m_inlineCallFrame->baselineCodeBlock.get();
                RELEASE_ASSERT(isValidCodeBlock(codeBlock));
                stackTrace[m_depth] = SamplingProfiler::StackFrame(SamplingProfiler::FrameType::VerifiedExecutable, codeBlock->ownerExecutable());
                m_depth++;
                m_inlineCallFrame = m_inlineCallFrame->directCaller.inlineCallFrame;
            }

            if (m_depth >= maxStackTraceSize)
                break;

            CodeBlock* codeBlock = m_callFrame->codeBlock();
            if (isValidCodeBlock(codeBlock)) {
                ExecutableBase* executable = codeBlock->ownerExecutable();
                stackTrace[m_depth] = SamplingProfiler::StackFrame(FrameType::VerifiedExecutable,executable);
            } else {
                stacktraceNeedsVerification = true;
                JSValue unsafeCallee = m_callFrame->unsafeCallee();
                stackTrace[m_depth] = SamplingProfiler::StackFrame(FrameType::UnverifiedCallee, JSValue::encode(unsafeCallee));
            }
            m_depth++;
            advanceToParentFrame();
            resetAtMachineFrame();
        }
        didRunOutOfSpace = m_depth >= maxStackTraceSize && !isAtTop();
        reportStats();
        return m_depth;
    }

    bool wasValidWalk() const
    {
        return !m_bailingOut;
    }

private:

    void advanceToParentFrame()
    {
        m_callFrame = m_callFrame->callerFrame(m_vmEntryFrame);
    }

    bool isAtTop() const
    {
        return !m_callFrame;
    }

    void resetAtMachineFrame()
    {
        m_inlineCallFrame = nullptr;

        if (isAtTop())
            return;

        if (!isValidFramePointer(m_callFrame)) {
            // Guard against pausing the process at weird program points.
            m_bailingOut = true;
            if (sReportStats)
                sNumFailedWalks++;
            return;
        }

#if ENABLE(DFG_JIT)
        // If the frame doesn't have a code block, then it's not a 
        // DFG/FTL frame which means we're not an inlined frame.
        CodeBlock* codeBlock = m_callFrame->codeBlock();
        if (!codeBlock)
            return;

        if (!isValidCodeBlock(codeBlock)) {
            m_bailingOut = true;
            if (sReportStats)
                sNumFailedWalks++;
            return;
        }

        // If the code block does not have any code origins, then there's no
        // inlining. Hence, we're not at an inlined frame.
        if (!codeBlock->hasCodeOrigins())
            return;

        CallSiteIndex index = m_callFrame->callSiteIndex();
        if (!codeBlock->canGetCodeOrigin(index)) {
            // FIXME:
            // For the most part, we only fail here when we're looking
            // at the top most call frame. All other parent call frames
            // should have set the CallSiteIndex when making a call.
            //
            // We should resort to getting information from the PC=>CodeOrigin mapping
            // once we implement it: https://bugs.webkit.org/show_bug.cgi?id=152629
            return;
        }
        m_inlineCallFrame = codeBlock->codeOrigin(index).inlineCallFrame;
#endif // !ENABLE(DFG_JIT)
    }

    bool isValidFramePointer(ExecState* exec)
    {
        uint8_t* fpCast = bitwise_cast<uint8_t*>(exec);
        for (MachineThreads::Thread* thread = m_vm.heap.machineThreads().threadsListHead(m_machineThreadsLocker); thread; thread = thread->next) {
            uint8_t* stackBase = static_cast<uint8_t*>(thread->stackBase);
            uint8_t* stackLimit = static_cast<uint8_t*>(thread->stackEnd);
            RELEASE_ASSERT(stackBase);
            RELEASE_ASSERT(stackLimit);
            if (fpCast <= stackBase && fpCast >= stackLimit)
                return true;
        }
        return false;
    }

    bool isValidCodeBlock(CodeBlock* codeBlock)
    {
        if (!codeBlock)
            return false;
        bool result = m_vm.heap.codeBlockSet().contains(m_codeBlockSetLocker, codeBlock);
        return result;
    }

    VM& m_vm;
    ExecState* m_callFrame;
    VMEntryFrame* m_vmEntryFrame;
    const LockHolder& m_codeBlockSetLocker;
    const LockHolder& m_machineThreadsLocker;
    bool m_bailingOut { false };
    InlineCallFrame* m_inlineCallFrame;
    size_t m_depth { 0 };
};

SamplingProfiler::SamplingProfiler(VM& vm, RefPtr<Stopwatch>&& stopwatch)
    : m_vm(vm)
    , m_stopwatch(WTFMove(stopwatch))
    , m_indexOfNextStackTraceToVerify(0)
    , m_timingInterval(std::chrono::microseconds(1000))
    , m_totalTime(0)
    , m_timerQueue(WorkQueue::create("jsc.sampling-profiler.queue", WorkQueue::Type::Serial, WorkQueue::QOS::UserInteractive))
    , m_jscExecutionThread(nullptr)
    , m_isActive(false)
    , m_isPaused(false)
    , m_hasDispatchedFunction(false)
{
    if (sReportStats) {
        sNumTotalWalks = 0;
        sNumFailedWalks = 0;
    }

    m_currentFrames.grow(256);

    m_handler = [this] () {
        LockHolder samplingProfilerLocker(m_lock);
        if (!m_isActive || !m_jscExecutionThread || m_isPaused) {
            m_hasDispatchedFunction = false;
            deref();
            return;
        }

        if (m_vm.entryScope) {
            double nowTime = m_stopwatch->elapsedTime();

            LockHolder machineThreadsLocker(m_vm.heap.machineThreads().getLock());
            LockHolder codeBlockSetLocker(m_vm.heap.codeBlockSet().getLock());
            LockHolder executableAllocatorLocker(m_vm.executableAllocator.getLock());

            bool didSuspend = m_jscExecutionThread->suspend();
            if (didSuspend) {
                // While the JSC thread is suspended, we can't do things like malloc because the JSC thread
                // may be holding the malloc lock.
                ExecState* callFrame;
                void* pc;
                {
                    MachineThreads::Thread::Registers registers;
                    m_jscExecutionThread->getRegisters(registers);
                    callFrame = static_cast<ExecState*>(registers.framePointer());
                    pc = registers.instructionPointer();
                    m_jscExecutionThread->freeRegisters(registers);
                }
                // FIXME: Lets have a way of detecting when we're parsing code.
                // https://bugs.webkit.org/show_bug.cgi?id=152761
                if (m_vm.executableAllocator.isValidExecutableMemory(executableAllocatorLocker, pc)) {
                    if (m_vm.isExecutingInRegExpJIT) {
                        // FIXME: We're executing a regexp. Lets gather more intersting data.
                        // https://bugs.webkit.org/show_bug.cgi?id=152729
                        callFrame = m_vm.topCallFrame; // We need to do this or else we'd fail our backtrace validation b/c this isn't a JS frame.
                    }
                } else if (LLInt::isLLIntPC(pc)) {
                    // We're okay to take a normal stack trace when the PC
                    // is in LLInt code.
                } else {
                    // We resort to topCallFrame to see if we can get anything
                    // useful. We usually get here when we're executing C code.
                    callFrame = m_vm.topCallFrame;
                }

                size_t walkSize;
                bool wasValidWalk;
                bool didRunOutOfVectorSpace;
                bool stacktraceNeedsVerification;
                {
                    FrameWalker walker(callFrame, m_vm, codeBlockSetLocker, machineThreadsLocker);
                    walkSize = walker.walk(m_currentFrames, didRunOutOfVectorSpace, stacktraceNeedsVerification);
                    wasValidWalk = walker.wasValidWalk();
                }

                m_jscExecutionThread->resume();

                // We can now use data structures that malloc, and do other interesting things, again.

                // FIXME: It'd be interesting to take data about the program's state when
                // we fail to take a stack trace: https://bugs.webkit.org/show_bug.cgi?id=152758
                if (wasValidWalk && walkSize) {
                    if (sReportStats) {
                        sNumTotalStackTraces++;
                        if (stacktraceNeedsVerification)
                            sNumUnverifiedStackTraces++;
                    }
                    Vector<StackFrame> stackTrace;
                    stackTrace.reserveInitialCapacity(walkSize);
                    for (size_t i = 0; i < walkSize; i++) {
                        StackFrame frame = m_currentFrames[i];
                        stackTrace.uncheckedAppend(frame); 
                        if (frame.frameType == FrameType::VerifiedExecutable)
                            m_seenExecutables.add(frame.u.verifiedExecutable);
                    }

                    m_stackTraces.append(StackTrace{ stacktraceNeedsVerification, nowTime, WTFMove(stackTrace) });

                    if (didRunOutOfVectorSpace)
                        m_currentFrames.grow(m_currentFrames.size() * 1.25);

                    m_totalTime += nowTime - m_lastTime;
                }
            }
        }

        m_lastTime = m_stopwatch->elapsedTime();

        dispatchFunction(samplingProfilerLocker);
    };
}

SamplingProfiler::~SamplingProfiler()
{
}

void SamplingProfiler::processUnverifiedStackTraces()
{
    // This function needs to be called from the JSC execution thread.
    RELEASE_ASSERT(m_lock.isLocked());

    TinyBloomFilter filter = m_vm.heap.objectSpace().blocks().filter();
    MarkedBlockSet& markedBlockSet = m_vm.heap.objectSpace().blocks();

    for (unsigned i = m_indexOfNextStackTraceToVerify; i < m_stackTraces.size(); i++) {
        StackTrace& stackTrace = m_stackTraces[i]; 
        if (!stackTrace.needsVerification)
            continue;
        stackTrace.needsVerification = false;

        for (StackFrame& stackFrame : stackTrace.frames) {
            if (stackFrame.frameType != FrameType::UnverifiedCallee) {
                RELEASE_ASSERT(stackFrame.frameType == FrameType::VerifiedExecutable);
                continue;
            }

            JSValue callee = JSValue::decode(stackFrame.u.unverifiedCallee);
            if (!Heap::isValueGCObject(filter, markedBlockSet, callee)) {
                stackFrame.frameType = FrameType::Unknown;
                continue;
            }

            JSCell* calleeCell = callee.asCell();
            auto frameTypeFromCallData = [&] () -> FrameType {
                FrameType result = FrameType::Unknown;
                CallData callData;
                CallType callType;
                callType = getCallData(calleeCell, callData);
                if (callType == CallTypeHost)
                    result = FrameType::Host;

                return result;
            };

            if (calleeCell->type() != JSFunctionType) {
                stackFrame.frameType = frameTypeFromCallData();
                continue;
            }
            ExecutableBase* executable = static_cast<JSFunction*>(calleeCell)->executable();
            if (!executable) {
                stackFrame.frameType = frameTypeFromCallData();
                continue;
            }

            RELEASE_ASSERT(Heap::isPointerGCObject(filter, markedBlockSet, executable));
            stackFrame.frameType = FrameType::VerifiedExecutable;
            stackFrame.u.verifiedExecutable = executable;
            m_seenExecutables.add(executable);
        }
    }

    m_indexOfNextStackTraceToVerify = m_stackTraces.size();
}

void SamplingProfiler::visit(SlotVisitor& slotVisitor)
{
    RELEASE_ASSERT(m_lock.isLocked());
    for (ExecutableBase* executable : m_seenExecutables)
        slotVisitor.appendUnbarrieredReadOnlyPointer(executable);
}

void SamplingProfiler::shutdown()
{
    stop();
}

void SamplingProfiler::start()
{
    LockHolder locker(m_lock);
    m_isActive = true;
    dispatchIfNecessary(locker);
}

void SamplingProfiler::stop()
{
    LockHolder locker(m_lock);
    m_isActive = false;
    reportStats();
}

void SamplingProfiler::pause()
{
    LockHolder locker(m_lock);
    m_isPaused = true;
    reportStats();
}

void SamplingProfiler::noticeCurrentThreadAsJSCExecutionThread(const LockHolder&)
{
    ASSERT(m_lock.isLocked());
    m_jscExecutionThread = m_vm.heap.machineThreads().machineThreadForCurrentThread();
}

void SamplingProfiler::noticeCurrentThreadAsJSCExecutionThread()
{
    LockHolder locker(m_lock);
    noticeCurrentThreadAsJSCExecutionThread(locker);
}

void SamplingProfiler::dispatchIfNecessary(const LockHolder& locker)
{
    if (m_isActive && !m_hasDispatchedFunction && m_jscExecutionThread && m_vm.entryScope) {
        ref(); // Matching deref() is inside m_handler when m_handler stops recursing.
        dispatchFunction(locker);
    }
}

void SamplingProfiler::dispatchFunction(const LockHolder&)
{
    m_hasDispatchedFunction = true;
    m_isPaused = false;
    m_lastTime = m_stopwatch->elapsedTime();
    m_timerQueue->dispatchAfter(m_timingInterval, m_handler);
}

void SamplingProfiler::noticeJSLockAcquisition()
{
    LockHolder locker(m_lock);
    noticeCurrentThreadAsJSCExecutionThread(locker);
}

void SamplingProfiler::noticeVMEntry()
{
    LockHolder locker(m_lock);
    ASSERT(m_vm.entryScope);
    noticeCurrentThreadAsJSCExecutionThread(locker);
    m_lastTime = m_stopwatch->elapsedTime();
    dispatchIfNecessary(locker);
}

void SamplingProfiler::clearData()
{
    LockHolder locker(m_lock);
    m_stackTraces.clear();
    m_seenExecutables.clear();
    m_indexOfNextStackTraceToVerify = 0;
}

static String displayName(const SamplingProfiler::StackFrame& stackFrame)
{
    if (stackFrame.frameType == FrameType::Unknown)
        return ASCIILiteral("<unknown>");
    if (stackFrame.frameType == FrameType::Host)
        return ASCIILiteral("<host>");
    RELEASE_ASSERT(stackFrame.frameType != FrameType::UnverifiedCallee);

    ExecutableBase* executable = stackFrame.u.verifiedExecutable;
    if (executable->isHostFunction())
        return ASCIILiteral("<host>");

    if (executable->isFunctionExecutable()) {
        String result = static_cast<FunctionExecutable*>(executable)->inferredName().string();
        if (!result.isEmpty())
            return result;
        return ASCIILiteral("<anonymous-function>");
    }
    if (executable->isEvalExecutable())
        return ASCIILiteral("<eval>");
    if (executable->isProgramExecutable())
        return ASCIILiteral("<global>");
    if (executable->isModuleProgramExecutable())
        return ASCIILiteral("<module>");

    RELEASE_ASSERT_NOT_REACHED();
    return "";
}

String SamplingProfiler::stacktracesAsJSON()
{
    m_lock.lock();
    {
        HeapIterationScope heapIterationScope(m_vm.heap);
        processUnverifiedStackTraces();
    }

    StringBuilder json;
    json.appendLiteral("[");

    bool loopedOnce = false;
    auto comma = [&] {
        if (loopedOnce)
            json.appendLiteral(",");
    };
    for (const StackTrace& stackTrace : m_stackTraces) {
        comma();
        json.appendLiteral("[");
        loopedOnce = false;
        for (const StackFrame& stackFrame : stackTrace.frames) {
            comma();
            json.appendLiteral("\"");
            json.append(displayName(stackFrame));
            json.appendLiteral("\"");
            loopedOnce = true;
        }
        json.appendLiteral("]");
        loopedOnce = true;
    }

    json.appendLiteral("]");

    m_lock.unlock();

    return json.toString();
}

} // namespace JSC

namespace WTF {

using namespace JSC;

void printInternal(PrintStream& out, SamplingProfiler::FrameType frameType)
{
    switch (frameType) {
    case SamplingProfiler::FrameType::VerifiedExecutable:
        out.print("VerifiedExecutable");
        break;
    case SamplingProfiler::FrameType::UnverifiedCallee:
        out.print("UnverifiedCallee");
        break;
    case SamplingProfiler::FrameType::Host:
        out.print("Host");
        break;
    case SamplingProfiler::FrameType::Unknown:
        out.print("Unknown");
        break;
    }
}

} // namespace WTF

#endif // ENABLE(SAMPLING_PROFILER)
