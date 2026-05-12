//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
// 
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/.
// 

#ifndef BUILDER_ECSBUILDER_H_
#define BUILDER_ECSBUILDER_H_

#include <omnetpp.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <string>
#include "tinyxml2.h"
#include "../global/GlobalStreamingSupervisor.h"
#include "../stask/StreamingSupervisor.h"
#include "../cpu/CPUCore.h"
#include "../stask/StreamingSink.h"

using namespace tinyxml2;

namespace ecsnetpp {

class ECSBuilder : public cSimpleModule {
//public:
//    ECSBuilder();
//    virtual ~ECSBuilder();
protected:
    GlobalStreamingSupervisor *globalSupervisor;
    bool ackersEnabled;
    bool hasGlobalSupervisor;

    // TODO: remove — temporary hardcoded split trigger; replace with optimizer signal
    cMessage* splitTriggerMsg = nullptr;

    // --- GLOBAL_SNAPSHOT ---
    static constexpr double SNAPSHOT_INTERVAL_S = 10.0;
    cMessage* snapshotMsg = nullptr;
    struct NodeInfo {
        std::string deviceLabel;  // e.g. "transitNodes[0]"
        cModule*    module;       // pointer to the CloudNodeA module
        int         coresTotal;
        double      linkDelayMs;
        double      linkCostPerGB;
    };
    std::vector<NodeInfo> snapshotNodes;
    std::vector<StreamingSink*> sinkModules;  // collected at startup for observed link delay queries
    void emitGlobalSnapshot();

    // --- TCP report stream (Simulator → Optimizer, port 9998) ---
    // Simulator acts as server; Optimizer connects once and reads newline-delimited JSON.
    std::thread             reportThread;
    std::atomic<bool>       reportRunning{false};
    int                     reportServerFd = -1;
    std::mutex              reportClientMutex;
    int                     reportClientFd = -1;
    void reportServerLoop(int port);
    // Writes json to the connected report client (if any) and to stdout.
    void emitToClient(const std::string& json);

    // --- TCP optimizer interface (Optimizer → Simulator, port 9999) ---
    std::thread             tcpThread;
    std::atomic<bool>       tcpRunning{false};
    int                     tcpServerFd = -1;
    cMessage*               pollMsg     = nullptr;
    void tcpServerLoop(int port);
    std::string dispatchCommand(const std::string& line);

    // Shared state between TCP thread and simulation thread.
    // TCP thread writes cmdLine + cmdClientFd, sets cmdPending, then waits.
    // Simulation thread (pollMsg) reads, executes, sends ACK, clears cmdPending, notifies.
    std::mutex              cmdMutex;
    std::condition_variable cmdCv;
    bool                    cmdPending  = false;
    std::string             cmdLine;
    int                     cmdClientFd = -1;

    // category -> node full-paths hosting inactive replicas (populated during executeAllocationPlan)
    std::map<std::string, std::vector<std::string>> inactiveReplicaNodePaths;
    // category -> full path of the currently active node for that operator (e.g. "SimpleEdgeCloudEnvironment.transitNodes[0]")
    std::map<std::string, std::string> categoryActiveNodePath;
    // destCategory -> [(supervisor, senderCategory)] pairs.
    // The senderCategory is the task hosted on that supervisor's node that routes to destCategory.
    // Both pieces are needed to update the correct routing entry on activation.
    std::map<std::string, std::vector<std::pair<StreamingSupervisor*, std::string>>> categoryUpstreamSupervisors;
    // Activates the first dormant replica for the given operator category.
    void activateFirstReplica(const std::string& category);

protected:
    virtual void initialize() override;
    virtual void handleMessage(cMessage *msg) override;
    virtual void finish() override;
    void connect(cGate *src, cGate *dest, double delay, double ber, double datarate);
    void executeAllocationPlan(cModule *parent);
    // Reads originalFile, generates a companion *_with_replicas.xml that pre-deploys
    // dormant operator replicas across all node tiers, and returns its path.
    // Returns originalFile unchanged if replicasPerOperator <= 1.
    std::string generateReplicaPlacement(const char* originalFile);
    void setupDistribution(XMLElement* task, const char* taskDistributionXmlElementName, const char* isDistributionEnabledBoolVarName,
            const char* distributionModuleName, cModule* stask, cModule* _parent, const char* nonDistributedValueXmlElementName,
            const char* nonDistributedValueVarName);
};

} /* namespace ecsnetpp */

#endif /* BUILDER_ECSBUILDER_H_ */
