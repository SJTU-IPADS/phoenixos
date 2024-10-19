/*
 * Copyright 2024 The PhoenixOS Authors. All rights reserved.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <mutex>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include "pos/include/common.h"
#include "pos/include/log.h"
#include "pos/include/handle.h"
#include "pos/include/client.h"
#include "pos/include/parser.h"
#include "pos/include/worker.h"
#include "pos/include/transport.h"
#include "pos/include/oob.h"
#include "pos/include/api_context.h"
#include "pos/include/utils/timer.h"
#include "pos/include/utils/lockfree_queue.h"


// forward declaration
class POSWorkspace;


/*!
 *  \brief  function prototypes for cli oob server
 */
namespace oob_functions {
    POS_OOB_DECLARE_SVR_FUNCTIONS(agent_register_client);
    POS_OOB_DECLARE_SVR_FUNCTIONS(agent_unregister_client);
    POS_OOB_DECLARE_SVR_FUNCTIONS(cli_migration_signal);
    POS_OOB_DECLARE_SVR_FUNCTIONS(cli_restore_signal);
    POS_OOB_DECLARE_SVR_FUNCTIONS(utils_mock_api_call);
}; // namespace oob_functions


/*!
 *  \brief  runtime workspace configuration
 *  \note   these configurations can be updated via CLI or workspace internal programs
 */
class POSWorkspaceConf {
 public:
    POSWorkspaceConf(POSWorkspace *root_ws);
    ~POSWorkspaceConf() = default;

    // configuration index in this container
    enum ConfigType : uint16_t {
        kRuntimeDaemonLogPath = 0,
        kRuntimeClientLogPath,
        kEvalCkptIntervfalMs,
        kUnknown
    }; 

    /*!
     *  \brief  set sepecific configuration in the workspace
     *  \note   should be thread-safe
     *  \param  conf_type   type of the configuration
     *  \param  val         value to set
     *  \return POS_SUCCESS for successfully setting
     */
    pos_retval_t set(ConfigType conf_type, std::string val);

    /*!
     *  \brief  obtain sepecific configuration in the workspace
     *  \note   should be thread-safe
     *  \param  conf_type   type of the configuration
     *  \param  val         value to get
     *  \return POS_SUCCESS for successfully getting
     */
    pos_retval_t get(ConfigType conf_type, std::string& val);

 private:
    friend class POSWorkspace;

    // ====== runtime configurations ======
    // path of the daemon's log
    std::string _runtime_daemon_log_path;
    // path of the client's log
    std::string _runtime_client_log_path;

    // ====== evaluation configurations ======
    // continuous checkpoint interval (ticks)
    uint64_t _eval_ckpt_interval_tick;

    // workspace that this configuration container attached to
    POSWorkspace *_root_ws;

    // mutex to avoid contension
    std::mutex _mutex;
};


enum pos_queue_position_t : uint8_t {
    kPOS_Queue_Position_Worker = 0,
    kPOS_Queue_Position_Parser
};


enum pos_queue_type_t : uint8_t {
    kPOS_Queue_Type_WQ = 0,
    kPOS_Queue_Type_CQ
};


/*!
 * \brief   base workspace of PhoenixOS
 */
class POSWorkspace {
 public:
    /*!
     *  \brief  constructor
     */
    POSWorkspace();

    /*!
     *  \brief  deconstructor
     */
    ~POSWorkspace();

    /*!
     *  \brief  initialize the workspace
     *  \return POS_SUCCESS for successfully initialization
     */
    pos_retval_t init();

    /*!
     *  \brief  shutdown the POS server
     */
    pos_retval_t deinit();


    /* =============== client management functions =============== */
 public:
    /*!
     *  \brief  create and add a new client to the workspace
     *  \param  param   parameter to create the client
     *  \param  clnt    pointer to the POSClient to be added
     *  \return POS_SUCCESS for successfully added
     */
    virtual pos_retval_t create_client(pos_create_client_param_t& param, POSClient** clnt){
        return POS_FAILED_NOT_IMPLEMENTED;
    }

    /*!
     *  \brief  remove a client by given uuid
     *  \param  uuid    specified uuid of the client to be removed
     *  \return POS_FAILED_NOT_EXIST for no client with the given uuid exists;
     *          POS_SUCCESS for successfully removing
     */
    pos_retval_t remove_client(pos_client_uuid_t uuid);

    /*!
     *  \brief  obtain client by given uuid
     *  \param  uuid    uuid of the client
     *  \return pointer to the corresponding POSClient
     */
    inline POSClient* get_client_by_uuid(pos_client_uuid_t uuid);

    /*!
     *  \brief  obtain client map
     *  \return client map
     */
    inline std::map<pos_client_uuid_t, POSClient*>& get_client_map(){
        return this->_client_map;
    }
    /* ============ end of client management functions =========== */


    /* =============== queue management functions =============== */
 protected:
    friend class POSParser;
    friend class POSWorker;

    /*!
     *  \brief  dequeue a wqe from parser work queue
     *  \note   this function is called within parser thread
     *  \param  uuid    the uuid to identify client
     *  \return POS_SUCCESS for successfully dequeued
     */
    POSAPIContext_QE* dequeue_parser_job(pos_client_uuid_t uuid);

    /*!
     *  \brief  push completion queue element to completion queue
     *  \tparam qt  completion queue position, either from parser or worker
     *  \note   this function is called within parser / worker thread
     *  \param  cqe poiner of the cqe to be inserted
     *  \return POS_SUCCESS for successfully insertion
     */
    template<pos_queue_position_t qposition>
    pos_retval_t push_cq(POSAPIContext_QE *cqe);

    /*!
     *  \brief  create a new queue pair between frontend and runtime for the client specified with uuid
     *  \param  uuid    the uuid to identify client where to create queue pair
     *  \return POS_FAILED_ALREADY_EXIST for duplicated queue pair;
     *          POS_SUCCESS for successfully created
     */
    pos_retval_t __create_qp(pos_client_uuid_t uuid);

    /*!
     *  \brief  remove a queue pair of the client specified with uuid
     *  \param  uuid    the uuid to identify client where to remove queue pair
     *  \return POS_FAILED_NOT_EXIST for no queue pair exist
     *          POS_SUCCESS for successfully removing
     */
    pos_retval_t __remove_qp(pos_client_uuid_t uuid);

    /*!
     *  \brief  polling the completion queue from parser / worker of a specific client
     *  \tparam qt  completion queue position, either from parser or worker
     *  \param  uuid    uuid for specifying client
     *  \param  cqes    returned cqes
     *  \return POS_SUCCESS for successfully polling
     */
    template<pos_queue_position_t qt>
    pos_retval_t __poll_cq(pos_client_uuid_t uuid, std::vector<POSAPIContext_QE*>* cqes);

    /*!
     *  \brief  remove queue by given uuid
     *  \tparam qtype       type of the queue to be deleted: CQ/WQ
     *  \tparam qposition   position of the queue to be deleted: Runtime/Worker
     *  \param  uuid        specified uuid of the queue pair to be removed
     *  \note   work queue should be lazyly removed as they shared across theads
     *  \return POS_FAILED_NOT_EXIST for no work queue with the given uuid exists;
     *          POS_SUCCESS for successfully removing
     */
    template<pos_queue_type_t qtype, pos_queue_position_t qposition>
    pos_retval_t __remove_q(pos_client_uuid_t uuid);
    /* ============ end of queue management functions =========== */
 
 public:
    /*!
     *  \brief  entrance of POS :)
     *  \param  api_id          index of the called API
     *  \param  uuid            uuid of the remote client
     *  \param  is_sync         indicate whether the api is a sync one
     *  \param  param_desps     description of all parameters of the call
     *  \param  ret_data        pointer to the data to be returned
     *  \param  ret_data_len    length of the data to be returned
     *  \return return code on specific XPU platform
     */
    int pos_process(
        uint64_t api_id, pos_client_uuid_t uuid, std::vector<POSAPIParamDesp_t> param_desps,
        void* ret_data=nullptr, uint64_t ret_data_len=0
    );

    // api manager
    POSApiManager *api_mgnr;

    // api id to mark an checkpoint op (different by platforms)
    uint64_t checkpoint_api_id;

    // idx of all stateful resources (handles)
    std::vector<uint64_t> stateful_handle_type_idx;

    // dynamic configuration of this workspace
    POSWorkspaceConf ws_conf;

    // TSC timer of the workspace
    POSUtilTscTimer tsc_timer;

 protected:
    /*!
     *  \brief  out-of-band server
     *  \note   use cases: intereact with CLI, and also agent-side
     */
    POSOobServer *_oob_server;

    // queue pairs between frontend and runtime (per client)
    std::map<pos_client_uuid_t, POSLockFreeQueue<POSAPIContext_QE_t*>*> _parser_wqs;
    std::map<pos_client_uuid_t, POSLockFreeQueue<POSAPIContext_QE_t*>*> _parser_cqs;

    // completion queue between frontend and worker (per client)
    std::map<pos_client_uuid_t, POSLockFreeQueue<POSAPIContext_QE_t*>*> _worker_cqs;

    // map of clients
    std::map<pos_client_uuid_t, POSClient*> _client_map;

    // the max uuid that has been recorded
    pos_client_uuid_t _current_max_uuid;

    // context for creating the client
    pos_client_cxt_t _template_client_cxt;

    /*!
     *  \brief  initialize the workspace
     *  \note   create device context inside this function, implementation on specific platform
     *  \return POS_SUCCESS for successfully initialization
     */
    virtual pos_retval_t __init() { return POS_FAILED_NOT_IMPLEMENTED; }

    /*!
     *  \brief  deinitialize the workspace
     *  \note   destory device context inside this function, implementation on specific platform
     *  \return POS_SUCCESS for successfully deinitialization
     */
    virtual pos_retval_t __deinit(){ return POS_FAILED_NOT_IMPLEMENTED; }

    /*!
     *  \brief  preserve resource on posd
     *  \param  rid     the resource type to preserve
     *  \param  data    source data for preserving
     *  \return POS_SUCCESS for successfully preserving
     */
    virtual pos_retval_t preserve_resource(pos_resource_typeid_t rid, void *data){
        return POS_FAILED_NOT_IMPLEMENTED;
    }
    
    void parse_command_line_options(int argc, char *argv[]);
};
