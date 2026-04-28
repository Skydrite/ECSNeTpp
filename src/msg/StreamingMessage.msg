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

namespace ecsnetpp;

cplusplus {{
#include "omnetpp.h"
}}
//class noncobject SomeType;

//cplusplus {{
//#include <map>
//typedef OtherType std::map<int,int>;
//}}
//class noncobject OtherType;

//
// TODO generated message class
//
packet StreamingMessage {
	int messageId;
	string sender;
//	int size;
	string content;
	bool isProcessingDelayInCyclesPerEvent;
//	double cyclesPerEvent;
	double processingDelayPerEvent;
	double selectivityRatio;
	simtime_t startTime;
	simtime_t operatorIngressTime;
	simtime_t channelIngressTime;
	double networkDelay;
	double processingDelay;
	double edgeProcessingDelay;

	// Per-hop telemetry (max 8 hops)
	int numHops = 0;
	bool pendingHopStamp = false;  // set true by supervisor after link stamp; cleared by CPUCore after proc stamp
	double hopLinkMs[8];           // WAN link delay to reach each hop (ms)
	double hopProcMs[8];           // CPU processing delay at each hop destination (ms)
	int hopQueueDepth[8];          // CPUCore queue depth when message was processed at each hop
	string hopNodeLabel[8];        // "category@nodeName" stamped by CPUCore at each hop

	// Source identity — set once at creation, never overwritten
	string sourceLabel = "";       // "category@nodeName" of the generating source task
}
