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

package ecsnetpp.builder;
//
// Dynamically allocates tasks to the host network, 
// with the allocation plan coming from a xml file.
//
simple ECSBuilder
{
    parameters:
        @display("i=block/cogwheel_s");
        string allocationPlanFile;
        string dspTopologyFile;
        // Total operator instances per category (1 active + N-1 dormant replicas).
        // Set to 1 to disable replica generation entirely.
        int replicasPerOperator = default(2);
        // TCP port the simulator listens on for optimizer commands.
        // Set to 0 to disable the TCP server (offline/batch mode).
        int tcpOptimizerPort = default(9999);
        // TCP port on which the simulator pushes JSON report stream to the optimizer.
        // Optimizer connects once and reads newline-delimited JSON (TRIGGER_REPORT / GLOBAL_SNAPSHOT).
        // Set to 0 to disable (reports go to stdout only).
        int tcpReportPort = default(9998);
}

