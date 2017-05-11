/*
 * BaseMaster.h
 *
 *  Created on: Aug 7, 2014
 *      Author: thardin
 */

#ifndef BASEMASTER_H_
#define BASEMASTER_H_

#include "FMIClient.h"
#ifdef USE_GPL
#include <gsl/gsl_multiroots.h>
#include "common/fmigo_storage.h"
using namespace fmigo_storage;
#endif

namespace fmitcp_master {
    class BaseMaster {
    protected:
        std::vector<FMIClient*> m_clients;
        std::vector<WeakConnection> m_weakConnections;
        OutputRefsType clientWeakRefs;

        InputRefsValuesType initialNonReals;  //for loop solver

#ifdef USE_GPL
        class FmigoStorage m_fmigoStorage;//(std::vector<size_t>(0));
#endif

    public:
        //number of pending requests sent to clients
        size_t getNumPendingRequests() const;

        explicit BaseMaster(std::vector<FMIClient*> clients, std::vector<WeakConnection> weakConnections);
        virtual ~BaseMaster();

#ifdef USE_GPL
        static int loop_residual_f(const gsl_vector *x, void *params, gsl_vector *f);

        inline FmigoStorage & get_storage(){return m_fmigoStorage;}
        void storage_alloc(const std::vector<FMIClient*> &clients){
            vector<size_t> states({});
            vector<size_t> indicators({});
            for(auto client: clients) {
                states.push_back(client->getNumContinuousStates());
                indicators.push_back(client->getNumEventIndicators());
            }
            get_storage().allocate_storage(states,indicators);
        }
#endif

        void solveLoops();
        virtual void prepare() {};
        virtual void runIteration(double t, double dt) = 0;

#define on(name) void name(FMIClient* slave) {}
        on(onSlaveInstantiated)
        on(onSlaveInitialized)
        on(onSlaveTerminated)
        on(onSlaveFreed)
        on(onSlaveStepped)
        on(onSlaveGotVersion)
        on(onSlaveSetReal)
        on(onSlaveGotState)
        on(onSlaveSetState)
        on(onSlaveFreedState)
        on(onSlaveDirectionalDerivative)

        //T is needed because func maybe of a function in Client (from which FMIClient is derived)
        void send(std::vector<FMIClient*> fmus, std::string str) {
            for (auto it = fmus.begin(); it != fmus.end(); it++) {
                (*it)->sendMessage(str);
            }
        }

        //like send() but only for one FMU
        void send(FMIClient *fmu, std::string str) {
            fmu->sendMessage(str);
        }

        //send() followed by wait() (blocking)
        void sendWait(std::vector<FMIClient*> fmus, std::string str) {
            send(fmus, str);
            wait();
        }

        //like sendWait() but only for one FMU (blocking)
        void sendWait(FMIClient *fmu, std::string str) {
            send(fmu, str);
            wait();
        }

        void wait();

        //implemented by StrongMaster to allow the main loop to fill in zeroes for the forces in the last row in the CSV output
        virtual int getNumForceOutputs() const {
          return 0;
        }
    };
};

#endif /* BASEMASTER_H_ */