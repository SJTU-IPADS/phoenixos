#include "pos/include/agent.h"

#include "yaml-cpp/yaml.h"


POSAgentConf::POSAgentConf(POSAgent *root_agent) : _root_agent(root_agent) {}


pos_retval_t POSAgentConf::load_config(std::string &&file_path){
    pos_retval_t retval = POS_SUCCESS;
    YAML::Node config;

    POS_ASSERT(file_path.size() > 0);

    if(unlikely(!std::filesystem::exists(file_path))){
        POS_WARN_C(
            "failed to load agent configuration, no file exist: file_path(%s)", file_path.c_str()
        );
        retval = POS_FAILED_INVALID_INPUT;
        goto exit;
    }

    try {
        config = YAML::LoadFile(file_path);
        
        // load job name
        if(config["job_name"]){
            this->_job_name = config["job_name"].as<std::string>();
            if(unlikely(this->_job_name.size() == 0)){
                POS_WARN_C(
                    "failed to load agent configuration, no job name provided: file_path(%s)", file_path.c_str()
                );
                retval = POS_FAILED_INVALID_INPUT;
                goto exit;
            }
            if(unlikely(this->_job_name.size() > oob_functions::agent_register_client::kMaxJobNameLen)){
                POS_WARN_C(
                    "failed to load agent configuration, job name too long: job_name(%s), len(%lu), max(%lu)",
                    file_path.c_str(), file_path.size()+1, oob_functions::agent_register_client::kMaxJobNameLen
                );
                retval = POS_FAILED_INVALID_INPUT;
                goto exit;
            }
        } else {
            POS_WARN_C(
                "failed to load agent configuration, no job name provided: file_path(%s)", file_path.c_str()
            );
            retval = POS_FAILED_INVALID_INPUT;
            goto exit;
        }
        
        // load daemon addr
        if(config["job_name"]){
            this->_daemon_addr = config["daemon_addr"].as<std::string>();
        } else {
            this->_daemon_addr = "127.0.0.1";
        }
    } catch (const YAML::Exception& e) {
        POS_WARN_C("failed to parse yaml file: path(%s), error(%s)", file_path.c_str(), e.what());
        retval = POS_FAILED_INVALID_INPUT;
        goto exit;
    }

    POS_DEBUG_C("loaded config from %s", file_path.c_str());

exit:
    return retval;
}


POSAgent::POSAgent() : _agent_conf(this) {
    oob_functions::agent_register_client::oob_call_data_t call_data;

    // load configurations
    if(unlikely(POS_SUCCESS != this->_agent_conf.load_config())){
        POS_ERROR_C("failed to load agent configuration");
    }
    
    this->_pos_oob_client = new POSOobClient(
        /* agent */ this,
        /* req_functions */ {
            {   kPOS_OOB_Msg_Agent_Register_Client,   oob_functions::agent_register_client::clnt    },
            {   kPOS_OOB_Msg_Agent_Unregister_Client, oob_functions::agent_unregister_client::clnt  },
        },
        /* local_port */ POS_OOB_CLIENT_DEFAULT_PORT,
        /* local_ip */ "0.0.0.0",
        /* server_port */ POS_OOB_SERVER_DEFAULT_PORT,
        /* server_ip */ this->_agent_conf._daemon_addr.c_str()
    );
    POS_CHECK_POINTER(this->_pos_oob_client);

    // register client
    call_data.job_name = this->_agent_conf._job_name;
    if(POS_SUCCESS != this->_pos_oob_client->call(kPOS_OOB_Msg_Agent_Register_Client, &call_data)){
        POS_ERROR_C_DETAIL("failed to register the client");
    }
    POS_DEBUG_C("successfully register client: uuid(%lu)", this->_uuid);
}


POSAgent::~POSAgent(){
    if(POS_SUCCESS != this->_pos_oob_client->call(kPOS_OOB_Msg_Agent_Unregister_Client, nullptr)){
        POS_ERROR_C_DETAIL("failed to unregister the client");
    }
    delete this->_pos_oob_client;
}


pos_retval_t POSAgent::oob_call(pos_oob_msg_typeid_t id, void* data){
    POS_CHECK_POINTER(data);
    return this->_pos_oob_client->call(id, data);
}