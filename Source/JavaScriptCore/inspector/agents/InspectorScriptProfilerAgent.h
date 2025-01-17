/*
 * Copyright (C) 2015 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef InspectorScriptProfilerAgent_h
#define InspectorScriptProfilerAgent_h

#include "InspectorBackendDispatchers.h"
#include "InspectorFrontendDispatchers.h"
#include "inspector/InspectorAgentBase.h"
#include "inspector/ScriptDebugServer.h"
#include <wtf/Noncopyable.h>

namespace JSC {
class Profile;
}

namespace Inspector {

typedef String ErrorString;

class JS_EXPORT_PRIVATE InspectorScriptProfilerAgent final : public InspectorAgentBase, public ScriptProfilerBackendDispatcherHandler, public JSC::Debugger::ProfilingClient {
    WTF_MAKE_NONCOPYABLE(InspectorScriptProfilerAgent);
public:
    InspectorScriptProfilerAgent(AgentContext&);
    virtual ~InspectorScriptProfilerAgent();

    virtual void didCreateFrontendAndBackend(FrontendRouter*, BackendDispatcher*) override;
    virtual void willDestroyFrontendAndBackend(DisconnectReason) override;

    // ScriptProfilerBackendDispatcherHandler
    virtual void startTracking(ErrorString&, const bool* profile) override;
    virtual void stopTracking(ErrorString&) override;

    // Debugger::ProfilingClient
    virtual bool isAlreadyProfiling() const override;
    virtual double willEvaluateScript(JSC::JSGlobalObject&) override;
    virtual void didEvaluateScript(JSC::JSGlobalObject&, double, JSC::ProfilingReason) override;

private:
    struct Event {
        Event(double start, double end) : startTime(start), endTime(end) { }
        double startTime { 0 };
        double endTime { 0 };
    };

    void addEvent(double startTime, double endTime, JSC::ProfilingReason);
    void trackingComplete();

    std::unique_ptr<ScriptProfilerFrontendDispatcher> m_frontendDispatcher;
    RefPtr<ScriptProfilerBackendDispatcher> m_backendDispatcher;
    Vector<RefPtr<JSC::Profile>> m_profiles;
    InspectorEnvironment& m_environment;
    bool m_tracking { false };
    bool m_enableLegacyProfiler { false };
    bool m_activeEvaluateScript { false };
};

} // namespace Inspector

#endif // InspectorScriptProfilerAgent_h
