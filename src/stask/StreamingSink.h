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

#ifndef STASK_STREAMINGSINK_H_
#define STASK_STREAMINGSINK_H_

#include "omnetpp.h"
#include "../msg/StreamingMessage_m.h"
#include "ISTask.h"

using namespace omnetpp;

namespace ecsnetpp {

class StreamingSink : public ISTask{
protected:
    virtual void initialize() override;
    virtual void handleMessage(cMessage *msg) override;
    virtual void finish() override;
private:
    // Collecting 99th percentile latency for end-to-end latency (network + processing) for each STask category
    std::vector<double> e2eLatencies;
    simsignal_t e2eP99Signal;
    double costPerGB = 0.0;
    long totalBytesReceived = 0;
    double slaLatency = 0.1; // seconds, read from parent CloudNodeA

    // Periodic window reporting
    cMessage* windowTimerMsg = nullptr;
    static constexpr double WINDOW_INTERVAL_S = 5.0;
    static constexpr int MAX_HOPS = 8;

    // Per-path window stats keyed by path string "sourceLabel:hop0Label:hop1Label:..."
    struct PathWindowStats {
        std::vector<double> e2eLatencies;
        std::vector<double> networkLatencies;
        double hopLinkSum[MAX_HOPS]  = {};
        double hopProcSum[MAX_HOPS]  = {};
        double hopQueueSum[MAX_HOPS] = {};
        int    hopCount[MAX_HOPS]    = {};
        int    maxHops = 0;
        double prevP99 = -1.0;  // kept across windows for trend tracking
        std::string hopLabels[MAX_HOPS];
        std::string sourceLabel;
    };
    std::map<std::string, PathWindowStats> pathStats;

};

} /* namespace ecsnetpp */

#endif /* STASK_STREAMINGSINK_H_ */
