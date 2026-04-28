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
#include <iomanip>
#include <sstream>
#include <vector>

namespace ecsnetpp {

Define_Module(StreamingSink);

void StreamingSink::initialize() {
    e2eP99Signal = registerSignal("e2eP99");
    omnetpp::cModule* submodule = getParentModule()->getSubmodule("cpuCoreScheduler");
    myCpuCoreScheduler = check_and_cast<ICpuCoreScheduler *>(submodule);
    ackersEnabled = getAncestorPar("ackersEnabled").boolValue();
    perCoreFreq = getAncestorPar("perCoreFreq").doubleValue();
    slaLatency = getAncestorPar("slaLatency").doubleValue();
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

    windowTimerMsg = new cMessage("windowTimer");
    scheduleAt(simTime() + WINDOW_INTERVAL_S, windowTimerMsg);
}

void StreamingSink::handleMessage(cMessage *msg) {
    if (msg->isSelfMessage()) {
        simtime_t windowStart = simTime() - WINDOW_INTERVAL_S;

        // --- per-hop bottleneck thresholds ---
        static constexpr double NETWORK_THRESHOLD_FACTOR      = 0.5;
        static constexpr int    COMPUTE_QUEUE_THRESHOLD       = 5;
        static constexpr double COMPUTE_PROC_THRESHOLD_FACTOR = 0.3;

        auto fmtHop = [](const std::string& s) -> std::string {
            auto p = s.find('@');
            return (p != std::string::npos) ? s.substr(0, p) + " @ " + s.substr(p + 1) : s;
        };

        int totalEvents = 0;
        for (auto& kv : pathStats) totalEvents += (int)kv.second.e2eLatencies.size();

        if (totalEvents > 0) {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(3);
            oss << "\n[WINDOW t=" << windowStart << "-" << simTime() << "]\n"
                << "  sink:   " << mySTaskCategory << "\n"
                << "  node:   " << getParentModule()->getFullPath() << "\n"
                << "  l_target_s: " << slaLatency << "\n"
                << "  paths:  " << pathStats.size() << "\n";

            for (auto& kv : pathStats) {
                const std::string& pathKey = kv.first;
                PathWindowStats& ps = kv.second;
                int n = (int)ps.e2eLatencies.size();
                if (n == 0) continue;

                std::vector<double> sortedE2e = ps.e2eLatencies;
                std::sort(sortedE2e.begin(), sortedE2e.end());
                double e2eP99 = sortedE2e[(int)std::ceil(0.99 * n) - 1];
                double e2eSum = 0;
                for (double v : ps.e2eLatencies) e2eSum += v;

                const char* trend = "STABLE";
                if (ps.prevP99 >= 0) {
                    if      (e2eP99 > ps.prevP99 * 1.05) trend = "RISING";
                    else if (e2eP99 < ps.prevP99 * 0.95) trend = "FALLING";
                }
                ps.prevP99 = e2eP99;

                double budgetPct = (slaLatency > 0) ? (e2eP99 / slaLatency * 100.0) : 0.0;
                const char* trigger;
                if      (e2eP99 > slaLatency)  trigger = "BREACH";
                else if (budgetPct >= 90.0)     trigger = "WARN";
                else                            trigger = "OK";

                oss << "  ---\n"
                    << "  path_key:        " << pathKey << "\n"
                    << "  events:          " << n << "\n"
                    << "  p99_current_s:   " << e2eP99 << "\n"
                    << "  e2e_mean_s:      " << (e2eSum / n) << "\n"
                    << "  p99_trend:       " << trend << "\n"
                    << std::setprecision(1)
                    << "  budget_used_pct: " << budgetPct << "\n"
                    << "  trigger_reason:  " << trigger << "\n";

                if (ps.maxHops > 0) {
                    oss << std::fixed << std::setprecision(3);
                    oss << "  hops:\n";
                    for (int h = 0; h < ps.maxHops && h < MAX_HOPS; h++) {
                        int cnt = ps.hopCount[h];
                        if (cnt == 0) continue;
                        double linkMean  = ps.hopLinkSum[h]  / cnt;
                        double procMean  = ps.hopProcSum[h]  / cnt;
                        int    queueMean = (int)(ps.hopQueueSum[h] / cnt);
                        std::string hopLabel = ps.hopLabels[h].empty() ? "?" : fmtHop(ps.hopLabels[h]);

                        double fairShareMs = (slaLatency * 1000.0) / ps.maxHops;
                        bool networkBad = linkMean  > fairShareMs * NETWORK_THRESHOLD_FACTOR;
                        bool computeBad = queueMean >= COMPUTE_QUEUE_THRESHOLD
                                       || procMean  > fairShareMs * COMPUTE_PROC_THRESHOLD_FACTOR;
                        const char* bottleneck;
                        if      ( networkBad &&  computeBad) bottleneck = "BOTH";
                        else if ( networkBad && !computeBad) bottleneck = "NETWORK";
                        else if (!networkBad &&  computeBad) bottleneck = "COMPUTE";
                        else                                  bottleneck = "OK";

                        oss << "    - hop: " << h << "  (" << hopLabel << ")\n"
                            << "      link_ms:       " << linkMean  << "\n"
                            << "      processing_ms: " << procMean  << "\n"
                            << "      queue_depth:   " << queueMean << "\n"
                            << "      bottleneck:    " << bottleneck << "\n";
                    }
                }

                // reset per-path window data (preserve prevP99)
                ps.e2eLatencies.clear();
                ps.networkLatencies.clear();
                std::fill(ps.hopLinkSum,  ps.hopLinkSum  + MAX_HOPS, 0.0);
                std::fill(ps.hopProcSum,  ps.hopProcSum  + MAX_HOPS, 0.0);
                std::fill(ps.hopQueueSum, ps.hopQueueSum + MAX_HOPS, 0.0);
                std::fill(ps.hopCount,    ps.hopCount    + MAX_HOPS, 0);
                ps.maxHops = 0;
            }
            std::cout << oss.str() << endl;
        } else {
            std::cout << "[WINDOW t=" << windowStart << "-" << simTime() << "]"
                      << "  events=0  sink=" << mySTaskCategory
                      << "  node=" << getParentModule()->getFullPath() << endl;
        }

        scheduleAt(simTime() + WINDOW_INTERVAL_S, windowTimerMsg);
        return;
    }

    if (msg->arrivedOn("fromCPU")) {
        StreamingMessage *pk = check_and_cast<StreamingMessage *>(msg);

        // Build path key from sourceLabel + hop node labels
        std::string pathKey = pk->getSourceLabel();
        int numHops = pk->getNumHops();
        for (int h = 0; h < numHops && h < MAX_HOPS; h++) {
            pathKey += ":";
            pathKey += pk->getHopNodeLabel(h);
        }

        double _networkDelay = pk->getNetworkDelay();
        double _processingDelay = pk->getProcessingDelay();
        emit(transmissionTimeSignal, _networkDelay);
        emit(processingTimeSignal, _processingDelay);
        emit(latencySignal, _networkDelay + _processingDelay);
        double e2e = (simTime() - pk->getCreationTime()).dbl();
        e2eLatencies.push_back(e2e);

        // Accumulate into per-path stats
        PathWindowStats& ps = pathStats[pathKey];
        ps.e2eLatencies.push_back(e2e);
        ps.networkLatencies.push_back(_networkDelay);
        ps.sourceLabel = pk->getSourceLabel();
        for (int h = 0; h < numHops && h < MAX_HOPS; h++) {
            ps.hopLinkSum[h]  += pk->getHopLinkMs(h);
            ps.hopProcSum[h]  += pk->getHopProcMs(h);
            ps.hopQueueSum[h] += pk->getHopQueueDepth(h);
            ps.hopCount[h]++;
            ps.hopLabels[h] = pk->getHopNodeLabel(h);
        }
        if (numHops > ps.maxHops) ps.maxHops = numHops;

        emit(edgeProcessingTimeSignal, pk->getEdgeProcessingDelay());
        emit(receivedStreamingMsgsSignal, pk);
        totalBytesReceived += pk->getByteLength();

        delete msg;
    } else if (msg->arrivedOn("incomingStream")) {
        StreamingMessage *msgToSend = check_and_cast<StreamingMessage *>(msg);

        // Local delivery (no TCP supervisor): stamp 0ms link delay so CPUCore records si as a hop
        if (!msgToSend->getPendingHopStamp()) {
            int hop = msgToSend->getNumHops();
            if (hop >= 0 && hop < 8) {
                msgToSend->setHopLinkMs(hop, 0.0);
                msgToSend->setPendingHopStamp(true);
            }
        }

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
        cancelAndDelete(windowTimerMsg);
        if (!e2eLatencies.empty()) {
            std::sort(e2eLatencies.begin(), e2eLatencies.end());
            int idx = (int)std::ceil(0.99 * e2eLatencies.size()) - 1;
            double finalP99 = e2eLatencies[idx];
            emit(e2eP99Signal, finalP99);
            EV << getFullPath() << ": final e2e P99 = " << finalP99 << "s"
               << "  SLA = " << slaLatency << "s"
               << "  SLA " << (finalP99 > slaLatency ? "BREACHED" : "MET") << endl;
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
