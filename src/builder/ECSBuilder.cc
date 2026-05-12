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

#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <set>
#include <vector>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include "ECSBuilder.h"
#include "inet/networklayer/common/L3AddressResolver.h"

using namespace tinyxml2;
using namespace omnetpp;

namespace ecsnetpp {

Define_Module(ECSBuilder);


struct cmp_str {
    bool operator()(char const *a, char const *b) {
        return std::strcmp(a, b) < 0;
    }
};

// ---------------------------------------------------------------------------
// XML deep-clone helpers (tinyxml2 has no built-in recursive clone)
// ---------------------------------------------------------------------------
static void deepCopyChildren(XMLDocument& doc, XMLElement* dst, const XMLElement* src) {
    for (const XMLNode* child = src->FirstChild(); child; child = child->NextSibling()) {
        if (const XMLElement* e = child->ToElement()) {
            XMLElement* newElem = doc.NewElement(e->Name());
            for (const XMLAttribute* a = e->FirstAttribute(); a; a = a->Next())
                newElem->SetAttribute(a->Name(), a->Value());
            deepCopyChildren(doc, newElem, e);
            dst->InsertEndChild(newElem);
        } else if (const XMLText* t = child->ToText()) {
            dst->InsertEndChild(doc.NewText(t->Value()));
        }
    }
}

static XMLElement* deepClone(XMLDocument& doc, const XMLElement* src) {
    XMLElement* dst = doc.NewElement(src->Name());
    for (const XMLAttribute* a = src->FirstAttribute(); a; a = a->Next())
        dst->SetAttribute(a->Name(), a->Value());
    deepCopyChildren(doc, dst, src);
    return dst;
}

// ---------------------------------------------------------------------------
// Replica placement generator
// ---------------------------------------------------------------------------
std::string ECSBuilder::generateReplicaPlacement(const char* originalFile) {
    int replicasPerOp = (int)par("replicasPerOperator").longValue();
    if (replicasPerOp <= 1) {
        // No replicas requested — use original file unchanged.
        return std::string(originalFile);
    }

    int numPi3Bs   = (int)getAncestorPar("numPiModel3Bs").longValue();
    int numTransit = (int)getAncestorPar("numTransitNodes").longValue();
    int numCloud   = (int)getAncestorPar("numCloudNodes").longValue();

    // All physical node slots, ordered transit → cloud → edge.
    // Transit and cloud nodes are preferred for replicas (higher bandwidth);
    // edge nodes are last resort (WiFi-only, constrained throughput).
    struct Slot { std::string name; int idx; };
    std::vector<Slot> allSlots;
    for (int i = 0; i < numTransit;  i++) allSlots.push_back({"transitNodes", i});
    for (int i = 0; i < numCloud;    i++) allSlots.push_back({"cloudNodes",   i});
    for (int i = 0; i < numPi3Bs;   i++) allSlots.push_back({"pi3Bs",        i});

    const char* OP_TYPE = "ecsnetpp.stask.StreamingOperator";

    // --- Parse original ---
    XMLDocument origDoc;
    if (origDoc.LoadFile(originalFile) != XML_SUCCESS)
        throw cRuntimeError("Cannot read placement file: %s", originalFile);

    // --- Build operator→downstream-categories map from topology file ---
    // Key: sender category  Value: set of categories it forwards to
    std::map<std::string, std::set<std::string>> downstreamOf;
    {
        std::fstream topoFile(par("dspTopologyFile").stringValue(), std::ios::in);
        std::string line;
        while (std::getline(topoFile, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::vector<std::string> tok = cStringTokenizer(line.c_str()).asVector();
            if (tok.size() >= 2)
                downstreamOf[tok[0]].insert(tok[1]);
        }
    }

    // --- Build (nodeName_idx) → set of hosted categories from original placement ---
    std::map<std::string, std::set<std::string>> nodeCategories;
    {
        const XMLElement* d = origDoc.FirstChildElement("devices")->FirstChildElement("device");
        while (d) {
            const char* dn  = d->FirstChildElement("name")->GetText();
            const char* di  = d->FirstChildElement("index-range")->GetText();
            std::vector<int> idxs = cStringTokenizer(di, "..").asIntVector();
            int idxFrom = idxs[0], idxTo = (idxs.size() > 1) ? idxs[1] : idxs[0];
            const XMLElement* tl = d->FirstChildElement("tasks");
            const XMLElement* t  = tl ? tl->FirstChildElement("task") : nullptr;
            while (t) {
                const XMLElement* catEl = t->FirstChildElement("category");
                if (catEl) {
                    for (int i = idxFrom; i <= idxTo; i++) {
                        std::string key = std::string(dn) + "_" + std::to_string(i);
                        nodeCategories[key].insert(catEl->GetText());
                    }
                }
                t = t->NextSiblingElement("task");
            }
            d = d->NextSiblingElement("device");
        }
    }

    // --- Build output document (deep copy of original + replica devices) ---
    XMLDocument outDoc;
    outDoc.InsertFirstChild(outDoc.NewDeclaration());
    XMLElement* outRoot = outDoc.NewElement("devices");
    outDoc.InsertEndChild(outRoot);

    // Per-category info collected during the first pass over the original.
    // category -> slots that already host an active instance
    std::map<std::string, std::vector<Slot>> opActiveSlots;
    // category -> pointer to one task XML element to use as template
    std::map<std::string, const XMLElement*> opTaskTemplate;

    XMLElement* device = origDoc.FirstChildElement("devices")->FirstChildElement("device");
    while (device) {
        // --- Deep-copy this device into the output doc ---
        XMLElement* outDevice = deepClone(outDoc, device);

        // --- Collect operator info and mark actives ---
        const char* devName  = device->FirstChildElement("name")->GetText();
        const char* idxText  = device->FirstChildElement("index-range")->GetText();
        std::vector<int> indices = cStringTokenizer(idxText, "..").asIntVector();
        int idxFrom = indices[0];
        int idxTo   = (indices.size() > 1) ? indices[1] : indices[0];

        // Walk original tasks and parallel output tasks together so we can
        // inject <active>true</active> into the cloned operator entries.
        XMLElement* srcTasks = device->FirstChildElement("tasks");
        XMLElement* dstTasks = outDevice->FirstChildElement("tasks");
        const XMLElement* srcTask = srcTasks ? srcTasks->FirstChildElement("task") : nullptr;
        XMLElement*       dstTask = dstTasks ? dstTasks->FirstChildElement("task") : nullptr;

        while (srcTask) {
            const char* type = "";
            const char* cat  = "";
            const XMLElement* typeEl = srcTask->FirstChildElement("type");
            const XMLElement* catEl  = srcTask->FirstChildElement("category");
            if (typeEl) type = typeEl->GetText();
            if (catEl)  cat  = catEl->GetText();

            if (strcmp(type, OP_TYPE) == 0) {
                // Record template and active slots for this category.
                opTaskTemplate[cat] = srcTask;
                for (int i = idxFrom; i <= idxTo; i++)
                    opActiveSlots[cat].push_back({devName, i});

                // Inject <active>true</active> into the cloned task.
                if (dstTask) {
                    XMLElement* activeEl = outDoc.NewElement("active");
                    activeEl->SetText("true");
                    dstTask->InsertEndChild(activeEl);
                }
            }
            srcTask = srcTask->NextSiblingElement("task");
            if (dstTask) dstTask = dstTask->NextSiblingElement("task");
        }

        outRoot->InsertEndChild(outDevice);
        device = device->NextSiblingElement("device");
    }

    // --- Second pass: add dormant replica <device> blocks ---
    for (auto& kv : opTaskTemplate) {
        const std::string& cat       = kv.first;
        const XMLElement*  tmplTask  = kv.second;
        const char*        origName  = tmplTask->FirstChildElement("name")->GetText();

        std::vector<Slot>& activeSlots = opActiveSlots[cat];
        int maxNewReplicas = replicasPerOp - (int)activeSlots.size();
        if (maxNewReplicas <= 0) continue;

        int replicasAdded = 0;
        int replicaNum    = 2; // naming: origName_r2, origName_r3, ...

        for (auto& slot : allSlots) {
            if (replicasAdded >= maxNewReplicas) break;

            // Skip slots already hosting an active instance of this category.
            bool alreadyUsed = false;
            for (auto& as : activeSlots) {
                if (as.name == slot.name && as.idx == slot.idx) {
                    alreadyUsed = true;
                    break;
                }
            }
            if (alreadyUsed) continue;

            // Skip slots that host any downstream task of this operator category.
            // Placing a replica co-located with its downstream task inserts it as
            // an unintended relay in the original routing path.
            bool hasDownstream = false;
            std::string slotKey = slot.name + "_" + std::to_string(slot.idx);
            auto dsIt = downstreamOf.find(cat);
            if (dsIt != downstreamOf.end()) {
                auto ncIt = nodeCategories.find(slotKey);
                if (ncIt != nodeCategories.end()) {
                    for (const auto& dc : dsIt->second) {
                        if (ncIt->second.count(dc)) { hasDownstream = true; break; }
                    }
                }
            }
            if (hasDownstream) continue;

            // Build a new <device> block for this replica.
            XMLElement* repDevice = outDoc.NewElement("device");

            XMLElement* nameEl = outDoc.NewElement("name");
            nameEl->SetText(slot.name.c_str());
            repDevice->InsertEndChild(nameEl);

            XMLElement* idxEl = outDoc.NewElement("index-range");
            idxEl->SetText(std::to_string(slot.idx).c_str());
            repDevice->InsertEndChild(idxEl);

            XMLElement* tasksEl = outDoc.NewElement("tasks");

            // Clone the template task and update name + active flag.
            XMLElement* repTask = deepClone(outDoc, tmplTask);

            XMLElement* repNameEl = repTask->FirstChildElement("name");
            if (repNameEl) {
                std::string newName = std::string(origName) + "_r" + std::to_string(replicaNum++);
                repNameEl->SetText(newName.c_str());
            }

            XMLElement* activeEl = outDoc.NewElement("active");
            activeEl->SetText("false");
            repTask->InsertEndChild(activeEl);

            tasksEl->InsertEndChild(repTask);
            repDevice->InsertEndChild(tasksEl);
            outRoot->InsertEndChild(repDevice);

            replicasAdded++;
        }
    }

    // --- Write output file next to the original ---
    std::string origPath = originalFile;
    size_t dot = origPath.rfind('.');
    std::string outPath = (dot != std::string::npos)
        ? origPath.substr(0, dot) + "_with_replicas.xml"
        : origPath + "_with_replicas.xml";

    if (outDoc.SaveFile(outPath.c_str()) != XML_SUCCESS)
        throw cRuntimeError("Cannot write replica placement file: %s", outPath.c_str());

    std::cout << "[ECSBuilder] Replica placement written to: " << outPath << endl;
    return outPath;
}

void ECSBuilder::initialize() {
    hasGlobalSupervisor = getAncestorPar("hasGlobalSupervisor").boolValue();
    if (hasGlobalSupervisor) {
        globalSupervisor = check_and_cast<GlobalStreamingSupervisor *>(getParentModule()->getModuleByPath(".globalSupervisorNode.globalSupervisor"));
    }
    ackersEnabled = getAncestorPar("ackersEnabled").boolValue();

    // Start TCP report stream server (Simulator → Optimizer push channel).
    int reportPort = (int)par("tcpReportPort").longValue();
    if (reportPort > 0) {
        reportRunning = true;
        reportThread = std::thread(&ECSBuilder::reportServerLoop, this, reportPort);
    }

    // Start TCP optimizer command server (Optimizer → Simulator commands).
    int tcpPort = (int)par("tcpOptimizerPort").longValue();
    if (tcpPort > 0) {
        tcpRunning = true;
        tcpThread = std::thread(&ECSBuilder::tcpServerLoop, this, tcpPort);
    }

    // build the network in event 1, because it is undefined whether the simkernel
    // will implicitly initialize modules created *during* initialization, or this needs
    // to be done manually.
    scheduleAt(simTime() + 1, new cMessage());
}

void ECSBuilder::handleMessage(cMessage *msg) {
    if (!msg->isSelfMessage())
        throw cRuntimeError("This module does not process messages.");

    // Periodic GLOBAL_SNAPSHOT emission.
    if (msg == snapshotMsg) {
        emitGlobalSnapshot();
        scheduleAt(simTime() + SNAPSHOT_INTERVAL_S, snapshotMsg);
        return;
    }

    // Poll for optimizer commands received over TCP.
    if (msg == pollMsg) {
        std::string line;
        int clientFd = -1;
        {
            std::unique_lock<std::mutex> lock(cmdMutex);
            if (cmdPending) {
                line     = cmdLine;
                clientFd = cmdClientFd;
            }
        }

        if (clientFd >= 0) {
            std::string ack = dispatchCommand(line);
            std::string ackLine = ack + "\n";
            ::send(clientFd, ackLine.c_str(), ackLine.size(), 0);
            close(clientFd);

            {
                std::unique_lock<std::mutex> lock(cmdMutex);
                cmdPending  = false;
                cmdClientFd = -1;
            }
            cmdCv.notify_one();
        }

        scheduleAt(simTime() + 1.0, pollMsg);
        return;
    }

    // TODO: replace with optimizer signal — uncomment to re-enable hardcoded split
    // if (msg == splitTriggerMsg) {
    //     activateFirstReplica("inflateOp");
    //     delete msg;
    //     splitTriggerMsg = nullptr;
    //     return;
    // }

    delete msg;
    executeAllocationPlan(getParentModule());

    // Start polling for TCP commands now that the network is built.
    if ((int)par("tcpOptimizerPort").longValue() > 0) {
        pollMsg = new cMessage("tcpPoll");
        scheduleAt(simTime() + 1.0, pollMsg);
    }
}

void ECSBuilder::connect(cGate *src, cGate *dest, double delay, double ber, double datarate) {
    cDatarateChannel *channel = nullptr;
    if (delay > 0 || ber > 0 || datarate > 0) {
        channel = cDatarateChannel::create("channel");
        if (delay > 0)
            channel->setDelay(delay);
        if (ber > 0)
            channel->setBitErrorRate(ber);
        if (datarate > 0)
            channel->setDatarate(datarate);
    }
    src->connectTo(dest, channel);
}

void ECSBuilder::executeAllocationPlan(cModule *parent) {
    const char* origAllocationFile = par("allocationPlanFile").stringValue();
    std::string replicaFilePath    = generateReplicaPlacement(origAllocationFile);
    const char* allocationFileName = replicaFilePath.c_str();
    const char* STREAMING_SOURCE_NAME = "ecsnetpp.stask.StreamingSource";
    const char* STREAMING_OPERATOR_NAME = "ecsnetpp.stask.StreamingOperator";
    std::string line;

    // maps needed to create connections between tasks
    // all in memory
    std::map<std::string, std::vector<cModule *>> allocationMap;
    std::map<std::string, int> incomingConnectionsCountMap;
    std::map<std::string, int> outgoingConnectionsCountMap;
    std::map<std::string, std::map<std::string, int>> incomingConnectionsUsageMap;
    std::map<std::string, std::map<std::string, int>> outgoingConnectionsUsageMap;
    std::map<std::string, std::string> staskNameToCategoryMap;
    std::map<std::string, std::vector<std::string>> staskDownstreamCategoryToSenderMap;
    std::map<std::string, std::vector<std::string>> staskSenderToDownstreamStaskCategoryMap;
    std::map<std::string, std::vector<std::string>> staskCategoryToParentMap;
    std::map<std::string, std::string> staskNameToSendersStringMap;

    XMLDocument doc;
    XMLError error = doc.LoadFile(allocationFileName);
    if (error != tinyxml2::XML_SUCCESS) {
        throw cRuntimeError("Unable to read the xml file : %s", allocationFileName);
    }

    XMLElement* root = doc.FirstChildElement("devices");
    if (root == nullptr) {
        throw cRuntimeError("Malformed XML! Root is null.");
    }
    XMLElement* device = root->FirstChildElement("device");

    while (device != nullptr) {
        const char* parentName = device->FirstChildElement("name")->GetText();
        const char* indexRange = device->FirstChildElement("index-range")->GetText();

        std::vector<int> deviceIndices = cStringTokenizer(indexRange, "..").asIntVector();

        // iterate per each task
        XMLElement* tasks = device->FirstChildElement("tasks");
        XMLElement* task = tasks->FirstChildElement("task");
        while (task != nullptr) {
            const char* staskName = task->FirstChildElement("name")->GetText();
            const char* staskCategory = task->FirstChildElement("category")->GetText();
            const char* staskType = task->FirstChildElement("type")->GetText();
            bool delayInCpuCycles = false;
            bool delayInTime = false;
            const char* delay;

            cBoolParImpl* isProcessingDelayInCpuCyclesPar = new cBoolParImpl();
            isProcessingDelayInCpuCyclesPar->setName("isProcessingDelayInCpuCycles");
            isProcessingDelayInCpuCyclesPar->setBoolValue(false);

            // configure processing delay for each operator
            // TODO: Setup per-processor-architecture delay configurations
            XMLElement* processingDelayXmlElement = task->FirstChildElement("processingdelay");
            if (processingDelayXmlElement != nullptr) {
                XMLElement* delayElement = processingDelayXmlElement->FirstChildElement("cpucycles");
                if (delayElement != nullptr) {
                    delayInCpuCycles = true;
                    isProcessingDelayInCpuCyclesPar->setBoolValue(true);
                    delay = delayElement->GetText();
                } else {
                    delayElement = processingDelayXmlElement->FirstChildElement("measuredtime");
                    if (delayElement != nullptr) {
                        delayInCpuCycles = false;
                        delay = delayElement->GetText();
                    }
                }
            }

            if (deviceIndices.size() == 1) {
                deviceIndices[1] = deviceIndices[0];
            }

            for (int i = deviceIndices[0]; i <= deviceIndices[1]; i++) {
                //TODO: Take this out from the loop - reads the xml per each iteration
                cModule* _parent = getParentModule()->getSubmodule(parentName, i);
                if (nullptr == _parent) {
                    std::cout << "Node " << parentName << "[" << i << "] is not present in the network." << endl;
                    continue;
                }

                /*std::stringstream ss;
                ss << staskName << i;
                const char* newStaskName = ss.str().c_str();*/
                std::string newStaskNameStr = std::string(staskName) + std::to_string(i);
                const char* newStaskName = newStaskNameStr.c_str();

                cModuleType *modtype = cModuleType::find(staskType);
                if (!modtype) {
                    throw cRuntimeError("Module type `%s' for node `%s' not found", staskType, newStaskName);
                }
                cModule *stask = modtype->create(newStaskName, _parent);

//                if (delayInCpuCycles) {
//                    std::cout << "$$$$$$$$$$$DELAY=" << delayInCpuCycles << endl;
//                    cDoubleParImpl *cyclesPerEvent = new cDoubleParImpl();
//                    cyclesPerEvent->setName("cyclesPerEvent");
//                    cyclesPerEvent->setUnit("MHz");
//                    cyclesPerEvent->setDoubleValue(atof(delay));
//                    stask->addPar(cyclesPerEvent);
//                } else {
//                    std::cout << "##########DELAY=" << delayInCpuCycles << endl;
//                    cDoubleParImpl *processingTimePerEvent = new cDoubleParImpl();
//                    processingTimePerEvent->setName("processingDelayPerEvent");
//                    processingTimePerEvent->setUnit("ns");
//                    processingTimePerEvent->setDoubleValue(atof(delay));
//                    stask->addPar(processingTimePerEvent);
//                }

                cDoubleParImpl *processingDelayPerEvent = new cDoubleParImpl();
                processingDelayPerEvent->setName("processingDelayPerEvent");
                processingDelayPerEvent->setDoubleValue(atof(delay));
                stask->addPar(processingDelayPerEvent);

                // add previously created boolean par
                stask->addPar(isProcessingDelayInCpuCyclesPar);

                cStringParImpl *mySTaskCategory = new cStringParImpl();
                mySTaskCategory->setName("mySTaskCategory");
                mySTaskCategory->setStringValue(staskCategory);
                stask->addPar(mySTaskCategory);

                if (strncmp(STREAMING_SOURCE_NAME, staskType, strlen(STREAMING_SOURCE_NAME)) == 0) {
                    // enable distributions for source message size
                    setupDistribution(task, "msgsizedistribution", "isSourceMsgSizeDistributed", "mySourceMsgSizeDistributionModuleName", stask,
                            _parent, "msgsize", "msgSize");
                    // enable distributions for source event rate
                    setupDistribution(task, "sourceevdistribution", "isSourceEvRateDistributed", "mySourceEvRateDistributionModuleName", stask,
                            _parent, "eventrate", "eventRate");

                } else if (strncmp(STREAMING_OPERATOR_NAME, staskType, strlen(STREAMING_OPERATOR_NAME)) == 0) {
                    // enable distributions for operator selectivity
                    setupDistribution(task, "selectivitydistribution", "isOperatorSelectivityDistributed",
                            "myOperatorSelectivityDistributionModuleName", stask, _parent, "selectivity", "selectivityRatio");
                    // enable distributions for operator productivity
                    setupDistribution(task, "productivitydistribution", "isOperatorProductivityDistributed",
                            "myOperatorProductivityDistributionModuleName", stask, _parent, "productivity", "productivityRatio");
                }

                std::string _parentName = _parent->getFullPath();
                allocationMap[_parentName].push_back(stask);
                incomingConnectionsCountMap[staskCategory] = 0;
                outgoingConnectionsCountMap[staskCategory] = 0;
                incomingConnectionsUsageMap[_parentName][newStaskName] = 0;
                outgoingConnectionsUsageMap[_parentName][newStaskName] = 0;
                staskNameToCategoryMap[newStaskName] = staskCategory;

                // Inactive replicas are deployed on the node but excluded from
                // upstream routing so no traffic is sent to them at startup.
                bool taskIsActive = true;
                XMLElement* activeXml = task->FirstChildElement("active");
                if (activeXml && activeXml->GetText()) {
                    std::string av = activeXml->GetText();
                    taskIsActive = (av != "false" && av != "0");
                }
                if (taskIsActive) {
                    staskCategoryToParentMap[staskCategory].push_back(_parentName);
                    categoryActiveNodePath[staskCategory] = _parentName;
                } else {
                    inactiveReplicaNodePaths[staskCategory].push_back(_parentName);
                }
            }

            task = task->NextSiblingElement("task");
        }
        device = device->NextSiblingElement("device");
    }

    std::map<std::string, std::map<std::string, bool>> connectedSTasks;

    // read and create connections
    std::fstream connectionsFile(par("dspTopologyFile").stringValue(), std::ios::in);
    while (getline(connectionsFile, line, '\n')) {
        if (line.empty() || line[0] == '#')
            continue;
        std::vector<std::string> tokens = cStringTokenizer(line.c_str()).asVector();
        if (tokens.size() != 3)
            throw cRuntimeError("wrong line in parameters file: 3 items required, line: \"%s\"", line.c_str());

        // get fields from tokens
        std::string srcSTaskCategory = tokens[0];
        std::string destSTaskCategory = tokens[1];
        bool connected = true; // third column ignored — any value treated as connected
        connectedSTasks[srcSTaskCategory][destSTaskCategory] = connected;
        if (connected) {
            // count maps were initialized earlier
            incomingConnectionsCountMap[destSTaskCategory] += 1;
            outgoingConnectionsCountMap[srcSTaskCategory] += 1;
            staskDownstreamCategoryToSenderMap[destSTaskCategory].push_back(srcSTaskCategory);
            staskSenderToDownstreamStaskCategoryMap[srcSTaskCategory].push_back(destSTaskCategory);
        }
    }

    std::map<std::string, std::vector<cModule *>>::iterator it;
    // iterate per each parent
    for (it = allocationMap.begin(); it != allocationMap.end(); ++it) {
        std::string _parentName = it->first;
        std::vector<cModule *>::iterator it2;
        std::vector<cModule *> tmp = it->second;

        // iterate per each stask in the vector
        for (it2 = tmp.begin(); it2 != tmp.end(); ++it2) {
            std::vector<cModule *>::iterator it3;
            cModule *_src = check_and_cast<cModule *>(*it2);
            std::string _srcSTaskCategory = staskNameToCategoryMap[_src->getFullName()];

            std::stringstream ss;
            for (size_t i = 0; i < staskDownstreamCategoryToSenderMap[_srcSTaskCategory].size(); ++i) {
                if (i != 0) {
                    ss << ",";
                }
                ss << staskDownstreamCategoryToSenderMap[_srcSTaskCategory][i];
            }

            cStringParImpl *mySenders = new cStringParImpl();
            mySenders->setName("mySenders");
            mySenders->setStringValue(ss.str().c_str());
            _src->addPar(mySenders);

            // read params from the ini file, etc
            _src->finalizeParameters();

            // iterate per each stask in the vector
            for (it3 = tmp.begin(); it3 != tmp.end(); ++it3) {
                cModule *_dest = check_and_cast<cModule *>(*it3);
                std::string _destSTaskCategory = staskNameToCategoryMap[_dest->getFullName()];

                if (_src->getFullName() != _dest->getFullName()) {
                    //std::cout << "CHECKING: " << _srcSTaskCategory
                    //          << " -> " << _destSTaskCategory << endl;
                    if (connectedSTasks[_srcSTaskCategory][_destSTaskCategory]) {
                        //std::cout << "CONNECTING: " << _src->getFullPath()
                        //          << " -> " << _dest->getFullPath() << endl;
                        cGate *srcOut, *destIn;

                        srcOut = _src->getOrCreateFirstUnconnectedGate("outgoingStream", 0, false, true);
                        destIn = _dest->getOrCreateFirstUnconnectedGate("incomingStream", 0, false, true);

                        connect(srcOut, destIn, -1, -1, -1);
                        incomingConnectionsUsageMap[_parentName][_dest->getFullName()] += 1;
                        outgoingConnectionsUsageMap[_parentName][_src->getFullName()] += 1;
                    }
                }
            }
        }
    }

    for (it = allocationMap.begin(); it != allocationMap.end(); ++it) {
        std::string _parentName = it->first;
        cModule *_parent = getParentModule()->getModuleByPath(_parentName.c_str());
        std::vector<cModule *>::iterator it2;
        std::vector<cModule *> tmp = it->second;

        // iterate per each stask in the vector
        for (it2 = tmp.begin(); it2 != tmp.end(); ++it2) {
            std::vector<cModule *>::iterator it3;
            cModule *_src = check_and_cast<cModule *>(*it2);
            cGate *taskIn, *taskOut, *supervisorIn, *supervisorOut;
            cModule *supervisor = _parent->getSubmodule("supervisor");

            std::string _srcCategory = staskNameToCategoryMap[_src->getFullName()];
            std::vector<std::string> senders = staskDownstreamCategoryToSenderMap[_srcCategory];
            int senderIndex = 0;
            while (incomingConnectionsUsageMap[_parentName][_src->getFullName()]
                   < incomingConnectionsCountMap[_srcCategory]) {
                taskIn = _src->getOrCreateFirstUnconnectedGate("incomingStream", 0, false, true);
                supervisorOut = supervisor->getOrCreateFirstUnconnectedGate("streamingPortOut", 0, false, true);
                int gateIndex = supervisorOut->getIndex();
                connect(supervisorOut, taskIn, -1, -1, -1);

                // Map exactly ONE sender to this gate
                // Each gate handles one incoming sender
                StreamingSupervisor *_sv = check_and_cast<StreamingSupervisor *>(supervisor);
                if (senderIndex < (int)senders.size()) {
                    _sv->addSenderToLocalGateMapping(senders[senderIndex], gateIndex);
                    senderIndex++;
                }

                incomingConnectionsUsageMap[_parentName][_src->getFullName()] += 1;
            }

            while (outgoingConnectionsUsageMap[_parentName][_src->getFullName()]
                    < outgoingConnectionsCountMap[staskNameToCategoryMap[_src->getFullName()]]) {
                taskOut = _src->getOrCreateFirstUnconnectedGate("outgoingStream", 0, false, true);
                supervisorIn = supervisor->getOrCreateFirstUnconnectedGate("streamingPortIn", 0, false, true);
                connect(taskOut, supervisorIn, -1, -1, -1);
                outgoingConnectionsUsageMap[_parentName][_src->getFullName()] += 1;
            }

            if (ackersEnabled) {
                cGate *srcToAcker, *supToAcker;
                srcToAcker = _src->gate("ackerOut");
                supToAcker = supervisor->getOrCreateFirstUnconnectedGate("sendToAcker", 0, false, true);
                connect(srcToAcker, supToAcker, -1, -1, -1);
            }

        }   // end of per each stask per parent->vector

        StreamingSupervisor *_supervisor = check_and_cast<StreamingSupervisor *>(_parent->getSubmodule("supervisor"));

        // Only add routing entries for tasks hosted on THIS device
        for (it2 = tmp.begin(); it2 != tmp.end(); ++it2) {
            cModule *_hostedTask = check_and_cast<cModule *>(*it2);
            std::string _hostedTaskCategory = staskNameToCategoryMap[_hostedTask->getFullName()];

            std::vector<std::string> _downstreamCategories = staskSenderToDownstreamStaskCategoryMap[_hostedTaskCategory];

            for (size_t i = 0; i < _downstreamCategories.size(); i++) {
                std::string destCategory = _downstreamCategories[i];
                _supervisor->addSTaskCategoryToDownstreamNodeMapping(
                        _hostedTaskCategory,
                        staskCategoryToParentMap[destCategory]
                );
                // Record this supervisor + its sender category: when a new replica of
                // destCategory is activated, we update senderStaskCategoryToDownstreamNodeIPMap
                // keyed by _hostedTaskCategory (the actual forwarding key), not destCategory.
                auto& svList = categoryUpstreamSupervisors[destCategory];
                auto entry = std::make_pair(_supervisor, _hostedTaskCategory);
                if (std::find(svList.begin(), svList.end(), entry) == svList.end())
                    svList.push_back(entry);
            }
        }
        _supervisor->resolveDownstreamNodeIPs();
    }

    if (hasGlobalSupervisor) {
        std::map<std::string, std::vector<std::string>>::iterator _senderIt;
        for (_senderIt = staskSenderToDownstreamStaskCategoryMap.begin();
             _senderIt != staskSenderToDownstreamStaskCategoryMap.end(); ++_senderIt) {
            std::string sender = _senderIt->first;
            std::vector<std::string> _staskCategories = _senderIt->second;
            for (size_t i = 0; i < _staskCategories.size(); i++) {
                globalSupervisor->addSTaskCategoryToDownstreamNodeMapping(
                        sender,
                        staskCategoryToParentMap[_staskCategories[i]]
                );
            }
        }
        globalSupervisor->resolveDownstreamNodeIPs();
    }

    for (it = allocationMap.begin(); it != allocationMap.end(); ++it) {
        std::vector<cModule *>::iterator it2;
        std::vector<cModule *> tmp = it->second;

        for (it2 = tmp.begin(); it2 != tmp.end(); ++it2) {
            cModule *_module = check_and_cast<cModule *>(*it2);
            _module->buildInside();
        }
    }

    // multi-stage init
    bool more = true;
    for (int stage = 0; more; stage++) {
        more = false;
        for (it = allocationMap.begin(); it != allocationMap.end(); ++it) {
            std::vector<cModule *>::iterator it2;
            std::vector<cModule *> tmp = it->second;

            for (it2 = tmp.begin(); it2 != tmp.end(); ++it2) {
                cModule *_module = check_and_cast<cModule *>(*it2);
                if (_module->callInitialize(stage)) {
                    more = true;
                }
            }
        }
    }

    // TODO: replace with optimizer signal — uncomment to re-enable hardcoded split
    // if (!inactiveReplicaNodePaths.empty()) {
    //     splitTriggerMsg = new cMessage("splitTrigger");
    //     scheduleAt(SimTime(25), splitTriggerMsg);
    //     std::cout << "[ECSBuilder] Hardcoded split trigger scheduled at t=25s (TODO: remove)" << endl;
    // }

    // Collect sink module pointers and wire up the report emitter callback.
    for (it = allocationMap.begin(); it != allocationMap.end(); ++it) {
        for (cModule* mod : it->second) {
            if (StreamingSink* sink = dynamic_cast<StreamingSink*>(mod)) {
                sinkModules.push_back(sink);
                sink->reportEmitter = [this](const std::string& json) { emitToClient(json); };
            }
        }
    }

    // Build node inventory for GLOBAL_SNAPSHOT.
    // Include transitNodes and cloudNodes (skip pi3Bs — edge devices have no WAN cost or link delay).
    int numTransit = (int)getAncestorPar("numTransitNodes").longValue();
    int numCloud   = (int)getAncestorPar("numCloudNodes").longValue();
    double transitCost = getAncestorPar("transitLinkCostPerGB").doubleValue();
    double cloudCost   = getAncestorPar("cloudLinkCostPerGB").doubleValue();

    for (int i = 0; i < numTransit; i++) {
        cModule* node = getParentModule()->getSubmodule("transitNodes", i);
        if (!node) continue;
        NodeInfo ni;
        ni.deviceLabel  = std::string("transitNodes[") + std::to_string(i) + "]";
        ni.module       = node;
        ni.coresTotal   = (int)node->par("cores").longValue();
        ni.linkDelayMs  = node->par("linkDelay").doubleValue() * 1000.0;  // linkDelay is @unit(s) → convert to ms
        ni.linkCostPerGB = transitCost;
        snapshotNodes.push_back(ni);
    }
    for (int i = 0; i < numCloud; i++) {
        cModule* node = getParentModule()->getSubmodule("cloudNodes", i);
        if (!node) continue;
        NodeInfo ni;
        ni.deviceLabel  = std::string("cloudNodes[") + std::to_string(i) + "]";
        ni.module       = node;
        ni.coresTotal   = (int)node->par("cores").longValue();
        ni.linkDelayMs  = getAncestorPar("eToCDelayMean").doubleValue();  // eToCDelayMean is @unit(ms) → already in ms
        ni.linkCostPerGB = cloudCost;
        snapshotNodes.push_back(ni);
    }

    snapshotMsg = new cMessage("globalSnapshot");
    scheduleAt(simTime() + SNAPSHOT_INTERVAL_S, snapshotMsg);
}

void ECSBuilder::setupDistribution(XMLElement* task, const char* taskDistributionXmlElementName, const char* isDistributionEnabledBoolVarName,
        const char* distributionModuleName, cModule* stask, cModule* _parent, const char* nonDistributedValueXmlElementName,
        const char* nonDistributedValueVarName) {

    XMLElement* taskDistributionXmlElement = task->FirstChildElement(taskDistributionXmlElementName);

    cBoolParImpl *isDistributionEnabledBoolPar = new cBoolParImpl();
    isDistributionEnabledBoolPar->setName(isDistributionEnabledBoolVarName);
    isDistributionEnabledBoolPar->setBoolValue(false);

    if (taskDistributionXmlElement != nullptr) {
        const char* distName = taskDistributionXmlElement->FirstChildElement("name")->GetText();
        const char* distType = taskDistributionXmlElement->FirstChildElement("type")->GetText();

        isDistributionEnabledBoolPar->setBoolValue(true);

        cStringParImpl *distributionModule = new cStringParImpl();
        distributionModule->setName(distributionModuleName);
        distributionModule->setStringValue(distName);
        stask->addPar(distributionModule);

        cModuleType *distModType = cModuleType::find(distType);
        if (!distModType) {
            throw cRuntimeError("Module type `%s' not found", distType);
        }
        cModule *dist = distModType->create(distName, _parent);

        XMLElement* distValues = taskDistributionXmlElement->FirstChildElement("values");
        if (distValues != nullptr) {
            XMLElement* distValueElement = distValues->FirstChildElement();

            while (distValueElement != nullptr) {
                const char* distValue = distValueElement->GetText();
                const char* distValueName = distValueElement->Name();

                cDoubleParImpl *distValuePar = new cDoubleParImpl();
                distValuePar->setName(distValueName);
                double __distValue = atof(distValue);
                distValuePar->setDoubleValue(__distValue);
                dist->addPar(distValuePar);

                distValueElement = distValueElement->NextSiblingElement();
            }
        }

        dist->buildInside();
        dist->callInitialize();
    } else {
        // if a distribution is not specified, read the fixed value for the parameter
        // TODO handle the scenario if this value is not present
        const char* nonDistributedValue = task->FirstChildElement(nonDistributedValueXmlElementName)->GetText();
        cDoubleParImpl *nonDistributedValuePar = new cDoubleParImpl();
        nonDistributedValuePar->setName(nonDistributedValueVarName);
        nonDistributedValuePar->setDoubleValue(atof(nonDistributedValue));
        stask->addPar(nonDistributedValuePar);
    }
    stask->addPar(isDistributionEnabledBoolPar);
}

void ECSBuilder::activateFirstReplica(const std::string& category) {
    auto it = inactiveReplicaNodePaths.find(category);
    if (it == inactiveReplicaNodePaths.end() || it->second.empty()) {
        std::cout << "[ECSBuilder] No inactive replicas found for category: " << category << endl;
        return;
    }

    std::string replicaNodePath = it->second[0];
    inet::L3Address replicaIP = inet::L3AddressResolver().resolve(replicaNodePath.c_str());

    auto& supervisors = categoryUpstreamSupervisors[category];
    for (auto& [sv, senderCat] : supervisors) {
        sv->activateReplica(senderCat, replicaIP);
    }

    // Remove from inactive list so a second trigger would pick the next replica
    it->second.erase(it->second.begin());
    categoryActiveNodePath[category] = replicaNodePath;

    std::cout << "[ECSBuilder] Activated replica of '" << category
              << "' on " << replicaNodePath
              << " (" << replicaIP << ") at t=" << simTime()
              << " — updated " << supervisors.size() << " supervisor(s)." << endl;
}

// ---------------------------------------------------------------------------
// GLOBAL_SNAPSHOT — emitted every SNAPSHOT_INTERVAL_S sim-seconds to stdout.
// ---------------------------------------------------------------------------
void ECSBuilder::emitGlobalSnapshot() {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);

    oss << "{\"type\":\"GLOBAL_SNAPSHOT\",\"sim_time_s\":" << simTime().dbl() << ",\"devices\":[";

    // Aggregate observed link delays per device across all sinks
    std::map<std::string, std::pair<double,int>> obsAccum;  // device -> (sum_ms, count)
    for (StreamingSink* sink : sinkModules) {
        for (auto& kv : sink->getLatestLinkDelayPerDevice()) {
            obsAccum[kv.first].first  += kv.second;
            obsAccum[kv.first].second += 1;
        }
    }

    bool firstDevice = true;
    for (auto& ni : snapshotNodes) {
        int queueTotal = 0;
        for (int c = 0; c < ni.coresTotal; c++) {
            cModule* coreModule = ni.module->getSubmodule("cpuCore", c);
            if (!coreModule) continue;
            CPUCore* core = check_and_cast<CPUCore*>(coreModule);
            queueTotal += core->getTotalQueueDepth();
        }

        // Observed link delay: average of all sink-window measurements for this device.
        // Falls back to configured value if no traffic has passed through yet.
        double obsDelay = ni.linkDelayMs;
        auto obsIt = obsAccum.find(ni.deviceLabel);
        if (obsIt != obsAccum.end() && obsIt->second.second > 0)
            obsDelay = obsIt->second.first / obsIt->second.second;

        if (!firstDevice) oss << ",";
        firstDevice = false;

        oss << "{";
        oss << "\"device\":\""              << ni.deviceLabel << "\",";
        oss << "\"cores_total\":"           << ni.coresTotal    << ",";
        oss << "\"queue_depth_total\":"     << queueTotal       << ",";
        oss << "\"link_delay_ms\":"         << ni.linkDelayMs   << ",";
        oss << "\"observed_link_delay_ms\":" << obsDelay        << ",";
        oss << "\"link_cost_per_gb\":"      << ni.linkCostPerGB;
        oss << "}";
    }

    oss << "]}";
    emitToClient(oss.str());
}

// ---------------------------------------------------------------------------
// Command dispatcher — called on the simulation thread from the pollMsg handler.
// Parses a space-delimited command line, executes it, and returns the ACK string.
// ---------------------------------------------------------------------------
std::string ECSBuilder::dispatchCommand(const std::string& line) {
    std::vector<std::string> tokens = cStringTokenizer(line.c_str()).asVector();
    if (tokens.empty())
        return "ERROR UNKNOWN - empty command";

    const std::string& action = tokens[0];

    if (action == "SPLIT_LOAD") {
        if (tokens.size() < 3)
            return "ERROR SPLIT_LOAD - usage: SPLIT_LOAD <operator> <replica> <strategy>";
        const std::string& opCategory  = tokens[1];
        const std::string& replicaName = tokens[2];  // logged; category used for lookup

        auto it = inactiveReplicaNodePaths.find(opCategory);
        if (it == inactiveReplicaNodePaths.end() || it->second.empty()) {
            return "ERROR SPLIT_LOAD " + opCategory + " no dormant replica found for " + opCategory;
        }

        activateFirstReplica(opCategory);

        std::ostringstream ack;
        ack << "OK SPLIT_LOAD " << opCategory << " " << simTime().dbl();
        std::cout << "[ECSBuilder] SPLIT_LOAD: activated replica '" << replicaName
                  << "' for category '" << opCategory << "' at t=" << simTime() << endl;
        return ack.str();
    }

    if (action == "RE_ROUTE") {
        // RE_ROUTE <operator> <from_device> <to_device>
        // e.g. RE_ROUTE inflateOp transitNodes[0] transitNodes[1]
        if (tokens.size() < 4)
            return "ERROR RE_ROUTE - usage: RE_ROUTE <operator> <from_device> <to_device>";

        const std::string& opCategory  = tokens[1];
        const std::string& fromDevice  = tokens[2];  // short name e.g. "transitNodes[0]"
        const std::string& toDevice    = tokens[3];

        // Check there is a dormant replica to activate on to_device
        auto replicaIt = inactiveReplicaNodePaths.find(opCategory);
        if (replicaIt == inactiveReplicaNodePaths.end() || replicaIt->second.empty())
            return "ERROR RE_ROUTE " + opCategory + " no dormant replica found for " + opCategory;

        // Find replica path matching to_device
        std::string replicaPath;
        int replicaIdx = -1;
        std::string parentPrefix = getParentModule()->getFullPath() + ".";
        std::string toDeviceFull = parentPrefix + toDevice;
        for (int i = 0; i < (int)replicaIt->second.size(); i++) {
            if (replicaIt->second[i] == toDeviceFull) {
                replicaPath = replicaIt->second[i];
                replicaIdx  = i;
                break;
            }
        }
        if (replicaIdx < 0)
            return "ERROR RE_ROUTE " + opCategory + " no replica pre-deployed on " + toDevice;

        // Resolve IPs
        std::string fromDeviceFull = parentPrefix + fromDevice;
        inet::L3Address fromIP = inet::L3AddressResolver().resolve(fromDeviceFull.c_str());
        inet::L3Address toIP   = inet::L3AddressResolver().resolve(replicaPath.c_str());

        // 1. Activate replica on to_device (adds toIP to upstream round-robin lists)
        auto& supervisors = categoryUpstreamSupervisors[opCategory];
        for (auto& [sv, senderCat] : supervisors)
            sv->activateReplica(senderCat, toIP);

        // 2. Deactivate from_device (remove fromIP from upstream round-robin lists)
        for (auto& [sv, senderCat] : supervisors)
            sv->deactivateNode(senderCat, fromIP);

        // 3. Update tracking state
        replicaIt->second.erase(replicaIt->second.begin() + replicaIdx);
        categoryActiveNodePath[opCategory] = replicaPath;

        std::ostringstream ack;
        ack << "OK RE_ROUTE " << opCategory << " " << simTime().dbl();
        std::cout << "[ECSBuilder] RE_ROUTE: moved '" << opCategory
                  << "' from " << fromDevice << " to " << toDevice
                  << " at t=" << simTime() << endl;
        return ack.str();
    }

    if (action == "HOLD") {
        std::string reason = (tokens.size() > 1) ? tokens[1] : "UNSPECIFIED";
        std::cout << "[ECSBuilder] HOLD received: " << reason
                  << " at t=" << simTime() << endl;
        std::ostringstream ack;
        ack << "OK HOLD - " << simTime().dbl();
        return ack.str();
    }

    return "ERROR UNKNOWN - unrecognised action: " + action;
}

// ---------------------------------------------------------------------------
// Report stream helpers — push JSON reports to any connected Optimizer client.
// ---------------------------------------------------------------------------

// Writes json to stdout and, if a client is connected on the report port, also
// pushes it over that TCP connection. Callable from the simulation thread only.
void ECSBuilder::emitToClient(const std::string& json) {
    std::string line = json + "\n";

    // Always write to stdout so terminal/log captures still work.
    std::cout << line;
    std::cout.flush();

    // Push to TCP client if one is connected.
    int fd;
    {
        std::lock_guard<std::mutex> lock(reportClientMutex);
        fd = reportClientFd;
    }
    if (fd < 0) return;

    ssize_t sent = ::send(fd, line.c_str(), line.size(), MSG_NOSIGNAL);
    if (sent < 0) {
        // Client disconnected; clear the fd so we stop trying.
        std::lock_guard<std::mutex> lock(reportClientMutex);
        if (reportClientFd == fd) {
            close(reportClientFd);
            reportClientFd = -1;
        }
        std::cout << "[ECSBuilder] Report stream: optimizer disconnected\n";
    }
}

// Listens for incoming Optimizer connections on the report port.
// Accepts one client at a time; replaces any existing client on reconnect.
// Runs on a background thread; never reads from clients (push-only channel).
void ECSBuilder::reportServerLoop(int port) {
    reportServerFd = socket(AF_INET, SOCK_STREAM, 0);
    if (reportServerFd < 0) {
        std::cerr << "[ECSBuilder] Report server: socket() failed\n";
        return;
    }

    int opt = 1;
    setsockopt(reportServerFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(reportServerFd, (sockaddr*)&addr, sizeof(addr)) < 0 ||
        listen(reportServerFd, 1) < 0) {
        std::cerr << "[ECSBuilder] Report server: bind/listen failed on port " << port << "\n";
        close(reportServerFd);
        reportServerFd = -1;
        return;
    }

    std::cout << "[ECSBuilder] TCP report stream server listening on port " << port << "\n";

    while (reportRunning) {
        int clientFd = accept(reportServerFd, nullptr, nullptr);
        if (clientFd < 0) break;  // server socket closed by finish()

        std::cout << "[ECSBuilder] Report stream: optimizer connected\n";

        std::lock_guard<std::mutex> lock(reportClientMutex);
        if (reportClientFd >= 0) close(reportClientFd);  // close previous client on reconnect
        reportClientFd = clientFd;
    }

    close(reportServerFd);
    reportServerFd = -1;
}

// ---------------------------------------------------------------------------
// TCP server — runs on a background thread; reads one command line per
// connection, hands it to the simulation thread via cmdMutex/cmdCv, and waits
// for the simulation thread to send the ACK before looping back to accept().
// ---------------------------------------------------------------------------
void ECSBuilder::tcpServerLoop(int port) {
    tcpServerFd = socket(AF_INET, SOCK_STREAM, 0);
    if (tcpServerFd < 0) {
        std::cerr << "[ECSBuilder] TCP server: socket() failed" << std::endl;
        return;
    }

    int opt = 1;
    setsockopt(tcpServerFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(tcpServerFd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[ECSBuilder] TCP server: bind() failed on port " << port << std::endl;
        close(tcpServerFd);
        tcpServerFd = -1;
        return;
    }
    if (listen(tcpServerFd, 1) < 0) {
        std::cerr << "[ECSBuilder] TCP server: listen() failed" << std::endl;
        close(tcpServerFd);
        tcpServerFd = -1;
        return;
    }

    std::cout << "[ECSBuilder] TCP optimizer server listening on port " << port << std::endl;

    while (tcpRunning) {
        int clientFd = accept(tcpServerFd, nullptr, nullptr);
        if (clientFd < 0) break;  // server socket closed by finish()

        std::cout << "[ECSBuilder] TCP optimizer connected" << std::endl;

        // Read one newline-terminated command line.
        std::string line;
        char c;
        while (recv(clientFd, &c, 1, 0) == 1) {
            if (c == '\n' || c == '\r') break;
            line += c;
        }

        if (!line.empty()) {
            // Hand off to simulation thread: store line + fd, signal, then wait.
            {
                std::unique_lock<std::mutex> lock(cmdMutex);
                cmdLine     = line;
                cmdClientFd = clientFd;
                cmdPending  = true;
            }
            cmdCv.notify_one();

            // Block until simulation thread sends ACK and clears cmdPending.
            {
                std::unique_lock<std::mutex> lock(cmdMutex);
                cmdCv.wait(lock, [this]{ return !cmdPending || !tcpRunning; });
            }
            // clientFd was closed by the simulation thread.
        } else {
            close(clientFd);
        }

        std::cout << "[ECSBuilder] TCP optimizer command processed" << std::endl;
    }

    close(tcpServerFd);
    tcpServerFd = -1;
}

void ECSBuilder::finish() {
    // --- Shut down report stream server ---
    reportRunning = false;
    {
        std::lock_guard<std::mutex> lock(reportClientMutex);
        if (reportClientFd >= 0) { close(reportClientFd); reportClientFd = -1; }
    }
    if (reportServerFd >= 0) {
        shutdown(reportServerFd, SHUT_RDWR);
        close(reportServerFd);
        reportServerFd = -1;
    }
    if (reportThread.joinable()) reportThread.detach();

    // --- Shut down command server ---
    tcpRunning = false;
    // Wake the TCP thread if it is blocked in cmdCv.wait() after handing off a command.
    cmdCv.notify_all();
    if (tcpServerFd >= 0) {
        shutdown(tcpServerFd, SHUT_RDWR);
        close(tcpServerFd);
        tcpServerFd = -1;
    }
    // Detach rather than join — accept() may not unblock reliably on WSL2.
    if (tcpThread.joinable()) tcpThread.detach();

    cancelAndDelete(pollMsg);
    pollMsg = nullptr;
    cancelAndDelete(snapshotMsg);
    snapshotMsg = nullptr;
}

} /* namespace ecsnetpp */
