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

#include "StreamingSink.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace ecsnetpp {

Define_Module(StreamingSink);

void StreamingSink::initialize() {
    e2eP99Signal = registerSignal("e2eP99");
    omnetpp::cModule* submodule = getParentModule()->getSubmodule("cpuCoreScheduler");
    myCpuCoreScheduler = check_and_cast<ICpuCoreScheduler *>(submodule);
    ackersEnabled = getAncestorPar("ackersEnabled").boolValue();
    perCoreFreq = getAncestorPar("perCoreFreq").doubleValue();
//    if (isProcessingDelayInCpuCycles) {
//        cyclesPerEvent = par("cyclesPerEvent").doubleValue();
//    } else {
        processingDelayPerEvent = par("processingDelayPerEvent").doubleValue();
//    }
    mySenders = par("mySenders").stringValue();
    mySTaskCategory = par("mySTaskCategory").stringValue();
    lastCPUIndex = 0;
    cpuCores = getAncestorPar("cores").longValue();
    if (cpuCores < 1) {
        throw new cRuntimeError("Number of CPU Cores is not set.");
    }

    // Read costPerGB from the cloud node's outgoing Ethernet channel
    cGate* ethOut = getParentModule()->gate("ethg$o", 0);
    cChannel* chan = ethOut ? ethOut->getChannel() : nullptr;
    if (chan && chan->hasPar("costPerGB")) {
        costPerGB = chan->par("costPerGB").doubleValue();
    }
    EV << getFullPath() << ": link costPerGB = " << costPerGB << " USD/GB" << endl;
//    calculateDelay();
}

void StreamingSink::handleMessage(cMessage *msg) {
    // std::cout << "Testing: " << msg->getName() << " arrived at " << getFullPath() << " at time " << simTime() << endl;
    if (msg->arrivedOn("fromCPU")) {
        StreamingMessage *pk = check_and_cast<StreamingMessage *>(msg);
        double _networkDelay = pk->getNetworkDelay();
        double _processingDelay = pk->getProcessingDelay();
        emit(transmissionTimeSignal, _networkDelay);
        emit(processingTimeSignal, _processingDelay);
//        const omnetpp::SimTime _latency = simTime() - pk->getStartTime();
        emit(latencySignal, _networkDelay + _processingDelay);
        double e2e = _networkDelay + _processingDelay;
        e2eLatencies.push_back(e2e);
        emit(edgeProcessingTimeSignal, pk->getEdgeProcessingDelay());
        emit(receivedStreamingMsgsSignal, pk);
        totalBytesReceived += pk->getByteLength();

//        sendAck(msg);
        delete msg;
    } else if (msg->arrivedOn("incomingStream")) {
        StreamingMessage *msgToSend = check_and_cast<StreamingMessage *>(msg);
        msgToSend->setSender(mySTaskCategory);

        msgToSend->setIsProcessingDelayInCyclesPerEvent(isProcessingDelayInCpuCycles);
        msgToSend->setProcessingDelayPerEvent(processingDelayPerEvent);

        msgToSend->setSelectivityRatio(1);
        long nextInLine = getNextProcessorCoreIndex();
        cModule *cpuCore = getParentModule()->getSubmodule("cpuCore", nextInLine);
        sendDirect(msgToSend, cpuCore, "incomingBus");
    }
//    StreamingMessage *pk = check_and_cast<StreamingMessage *>(msg);
//    const char* sender = pk->getSender();
//    if (strstr(mySenders, sender) == NULL) {
//        delete msg;
//    } else {
//        publishCpuStateChanged(States::CPU_BUSY);
//        emit(rcvdPkSignal, pk);
//        sendAck(msg);
//        delete msg;
//
//        publishCpuStateChanged(States::CPU_IDLE);
//    }
    }

    void StreamingSink::finish() {
        if (!e2eLatencies.empty()) {
            std::sort(e2eLatencies.begin(), e2eLatencies.end());
            int idx = (int)std::ceil(0.99 * e2eLatencies.size()) - 1;
            emit(e2eP99Signal, e2eLatencies[idx]);
        }

        double totalGB = totalBytesReceived / 1e9;
        double totalCostUSD = totalGB * costPerGB;
        emit(linkCostSignal, totalCostUSD);
        EV << getFullPath() << ": total bytes received = " << totalBytesReceived
           << " B (" << totalGB << " GB)"
           << ", link cost = $" << totalCostUSD << " USD"
           << " (@ $" << costPerGB << "/GB)" << endl;
    }
} /* namespace ecsnetpp */
