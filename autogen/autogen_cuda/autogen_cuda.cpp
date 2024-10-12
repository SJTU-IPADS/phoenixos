#include "autogen_cuda.h"


pos_retval_t POSAutogener::__collect_pos_support_yaml(
    const std::string& file_path,
    pos_support_header_file_meta_t *header_file_meta
){
    pos_retval_t retval = POS_SUCCESS;
    uint64_t i, j;
    std::string api_type, param_type;
    std::vector<std::string> dependent_headers;
    pos_support_api_meta_t *api_meta;
    pos_support_resource_meta_t *resource_meta;
    YAML::Node config, api, resources;

    POS_CHECK_POINTER(header_file_meta);

    auto __parse_resources = [&](
        const char* resource_list_name,
        std::vector<pos_support_resource_meta_t*>* resource_list
    ) -> pos_retval_t {
        for(j=0; j<api[resource_list_name].size(); j++){
            resources = api[resource_list_name][j];

            POS_CHECK_POINTER(resource_list);
            POS_CHECK_POINTER(resource_meta = new pos_support_resource_meta_t);
            resource_list->push_back(resource_meta);
            
            // index of the parameter
            resource_meta->index = resources["index"].as<uint16_t>();
            
            // type of the parameter
            param_type = resources["type"].as<std::string>();
            if(param_type == std::string("cuda_memory")){
                resource_meta->type = kPOS_CUDAResource_Memory;
            } else if(param_type == std::string("cuda_stream")){
                resource_meta->type = kPOS_CUDAResource_Stream;
            } else if(param_type == std::string("cuda_event")){
                resource_meta->type = kPOS_CUDAResource_Event;
            } else if(param_type == std::string("cuda_stream")){
                resource_meta->type = kPOS_CUDAResource_Module;
            } else if(param_type == std::string("cuda_stream")){
                resource_meta->type = kPOS_CUDAResource_Function;
            } else {
                POS_WARN_C(
                    "invalid parameter type detected: api_name(%s), given_type(%s)",
                    api_meta->name.c_str(), param_type.c_str()
                );
                retval = POS_FAILED_INVALID_INPUT;
                goto exit;
            }
        }
    exit:
        return retval;
    };

    try {
        config = YAML::LoadFile(file_path);
        header_file_meta->file_name = config["header_file_name"].as<std::string>();

        if(config["dependent_headers"]){
            for(j=0; j<config["dependent_headers"].size(); j++){
                dependent_headers.push_back(config["dependent_headers"][j].as<std::string>());
            }
        }

        for(i=0; i<config["apis"].size(); i++){
            api = config["apis"][i];

            POS_CHECK_POINTER(api_meta = new pos_support_api_meta_t);

            // name of the API
            api_meta->name = api["name"].as<std::string>();

            // whether to customize the parser and worker logic of API
            api_meta->customize = api["customize"].as<bool>();

            // dependent headers to support hijacking this API
            api_meta->dependent_headers = dependent_headers;

            // API type
            api_type = api["type"].as<std::string>();
            if(api_type == std::string("create_resource")){
                api_meta->api_type = kPOS_API_Type_Create_Resource;
            } else if(api_type == std::string("delete_resource")){
                api_meta->api_type = kPOS_API_Type_Delete_Resource;
            } else if(api_type == std::string("get_resource")){
                api_meta->api_type = kPOS_API_Type_Get_Resource;
            } else if(api_type == std::string("set_resource")){
                api_meta->api_type = kPOS_API_Type_Set_Resource;
            } else {
                POS_WARN_C(
                    "invalid api type detected: api_name(%s), given_type(%s)",
                    api_meta->name.c_str(), api_type.c_str()
                );
                retval = POS_FAILED_INVALID_INPUT;
                goto exit;
            }

            // resources to be created by this API
            if(unlikely(POS_SUCCESS != (
                retval = __parse_resources("create_resources", &api_meta->create_resources)
            ))){ goto exit; }
            if(unlikely(POS_SUCCESS != (
                retval = __parse_resources("delete_resources", &api_meta->delete_resources)
            ))){ goto exit; }
            if(unlikely(POS_SUCCESS != (
                retval = __parse_resources("get_resources", &api_meta->get_resources)
            ))){ goto exit; }
            if(unlikely(POS_SUCCESS != (
                retval = __parse_resources("set_resources", &api_meta->set_resources)
            ))){ goto exit; }

            header_file_meta->api_map.insert({ api_meta->name, api_meta });
        }
    } catch (const YAML::Exception& e) {
        POS_WARN_C("failed to parse yaml file: path(%s), error(%s)", file_path.c_str(), e.what());
        retval = POS_FAILED_INVALID_INPUT;
        goto exit;
    }

exit:
    return retval;
}


pos_retval_t POSAutogener::__collect_vendor_header_file(
    const std::string& file_path,
    pos_vendor_header_file_meta_t* vendor_header_file_meta,
    pos_support_header_file_meta_t* support_header_file_meta
){
    pos_retval_t retval = POS_SUCCESS;
    CXIndex index;
    CXTranslationUnit unit;
    CXCursor cursor;
    
    struct __clang_param_wrapper {
        pos_vendor_header_file_meta_t* vendor_header_file_meta;
        pos_support_header_file_meta_t* support_header_file_meta;
    };

    POS_CHECK_POINTER(vendor_header_file_meta);
    POS_CHECK_POINTER(support_header_file_meta);

    __clang_param_wrapper param {
        .vendor_header_file_meta = vendor_header_file_meta,
        .support_header_file_meta = support_header_file_meta
    };

    index = clang_createIndex(0, 0);
    unit = clang_parseTranslationUnit(
        index, file_path.c_str(), nullptr, 0, nullptr, 0, CXTranslationUnit_None
    );

    if(unlikely(unit == nullptr)){
        POS_WARN_C("failed to create CXTranslationUnit for file: path(%s)", file_path.c_str());
        retval = POS_FAILED;
        goto exit;
    }

    // scan across all function declarations
    cursor = clang_getTranslationUnitCursor(unit);
    clang_visitChildren(
        /* parent */ cursor,
        /* visitor */
        [](CXCursor cursor, CXCursor parent, CXClientData client_data) -> CXChildVisitResult {
            int i, num_args;
            std::string func_name_cppstr;
            CXString func_name;
            CXString func_ret_type;
            CXCursor arg_cursor;
            __clang_param_wrapper *param = nullptr;
            pos_vendor_header_file_meta_t *vendor_header_file_meta = nullptr;
            pos_support_header_file_meta_t *support_header_file_meta = nullptr;
            pos_vendor_api_meta_t *api_meta = nullptr;
            pos_vendor_param_meta_t *param_meta = nullptr;

            if (clang_getCursorKind(cursor) == CXCursor_FunctionDecl) {
                param = reinterpret_cast<__clang_param_wrapper*>(client_data);
                POS_CHECK_POINTER(param);
                vendor_header_file_meta = reinterpret_cast<pos_vendor_header_file_meta_t*>(param->vendor_header_file_meta);
                POS_CHECK_POINTER(vendor_header_file_meta);
                support_header_file_meta = reinterpret_cast<pos_support_header_file_meta_t*>(param->support_header_file_meta);
                POS_CHECK_POINTER(support_header_file_meta);

                func_name_cppstr = std::string(clang_getCString(clang_getCursorSpelling(cursor)));
                if(support_header_file_meta->api_map.count(func_name_cppstr) == 0){
                    goto cursor_traverse_exit;
                }

                POS_CHECK_POINTER(api_meta = new pos_vendor_api_meta_t);
                vendor_header_file_meta->api_map.insert({ func_name_cppstr, api_meta });
                api_meta->name = clang_getCursorSpelling(cursor);
                api_meta->return_type = clang_getCursorResultType(cursor);
                // returnType = clang_getTypeSpelling(clang_getCursorResultType(cursor));

                num_args = clang_Cursor_getNumArguments(cursor);
                for(i=0; i<num_args; i++){
                    POS_CHECK_POINTER(param_meta = new pos_vendor_param_meta_t);
                    api_meta->params.push_back(param_meta);
                    arg_cursor = clang_Cursor_getArgument(cursor, i);
                    param_meta->name = clang_getCursorSpelling(arg_cursor);
                    param_meta->type = clang_getCursorType(arg_cursor);
                }
            }
        
        cursor_traverse_exit:
            return CXChildVisit_Recurse;
        },
        /* client_data */ &param
    );
    clang_disposeTranslationUnit(unit);

exit:
    return retval;
}


pos_retval_t POSAutogener::__generate_api_parser(
    pos_vendor_api_meta_t* vendor_api_meta, pos_support_api_meta_t* support_api_meta
){
    pos_retval_t retval = POS_SUCCESS;
    uint64_t i;
    POSCodeGen_CppSourceFile *parser_file;
    POSCodeGen_CppBlock *ps_function_namespace, *api_namespace, *parser_function;
    std::string api_snake_name;

    POS_CHECK_POINTER(vendor_api_meta);
    POS_CHECK_POINTER(support_api_meta);

    if(support_api_meta->customize == true){
        goto exit;
    }

    api_snake_name = posautogen_utils_camel2snake(support_api_meta->name);

    parser_file = new POSCodeGen_CppSourceFile(
        this->parser_directory 
        + std::string("/")
        + support_api_meta->name
        + std::string(".cpp")
    );
    POS_CHECK_POINTER(parser_file);
    
    // add headers
    parser_file->add_include("#include <iostream>");
    parser_file->add_include("#include \"pos/include/common.h\"");
    parser_file->add_include("#include \"pos/include/dag.h\"");
    parser_file->add_include("#include \"pos/cuda_impl/handle.h\"");
    parser_file->add_include("#include \"pos/cuda_impl/parser.h\"");
    parser_file->add_include("#include \"pos/cuda_impl/client.h\"");
    parser_file->add_include("#include \"pos/cuda_impl/api_context.h\"");
    for(i=0; i<support_api_meta->dependent_headers.size(); i++){
        parser_file->add_include(std::format("#include <{}>", support_api_meta->dependent_headers[i]));
    }

    // create ps_function namespace
    ps_function_namespace = new POSCodeGen_CppBlock(
        /* field name */ "namespace ps_functions",
        /* need_braces */ true,
        /* need_foot_comment */ true
    );
    POS_CHECK_POINTER(ps_function_namespace);
    parser_file->add_block(ps_function_namespace);

    // create api namespace
    retval = ps_function_namespace->allocate_block(
        /* field name */ std::string("namespace ") + api_snake_name,
        /* new_block */ &api_namespace,
        /* need_braces */ true,
        /* need_foot_comment */ true,
        /* level_offset */ 0
    );
    if(unlikely(retval != POS_SUCCESS)){
        POS_WARN_C(
            "failed to allocate cpp block for api namespace while generating parser function: "
            "api_name(%s)",
            api_snake_name
        );
    }
    POS_CHECK_POINTER(api_namespace);

    // create function POS_RT_FUNC_PARSER
    retval = api_namespace->allocate_block(
        /* field name */ std::string("POS_RT_FUNC_PARSER()"),
        /* new_block */ &parser_function,
        /* need_braces */ true,
        /* need_foot_comment */ false,
        /* level_offset */ 1
    );
    if(unlikely(retval != POS_SUCCESS)){
        POS_WARN_C(
            "failed to allocate cpp block for POS_RT_FUNC_PARSER while generating parser function: "
            "api_name(%s)",
            api_snake_name
        );
    }
    POS_CHECK_POINTER(parser_function);

    // declare variables in the parser
    parser_function->declare_var("pos_retval_t retval = POS_SUCCESS;");
    parser_function->declare_var("POSClient_CUDA *client;");

    // check input pointers for wqe and ws
    parser_function->append_content(
        "POS_CHECK_POINTER(wqe);\n"
        "POS_CHECK_POINTER(ws);"
    );

    // obtain client
    parser_function->append_content(
        "client = (POSClient_CUDA*)(wqe->client);\n"
        "POS_CHECK_POINTER(client);"
    );

    // do runtime debug check
    parser_function->append_content(std::format(
        "#if POS_ENABLE_DEBUG_CHECK\n"
        "    // check whether given parameter is valid\n"
        "   if(unlikely(wqe->api_cxt->params.size() != {})) {{\n"
        "       POS_WARN(\n"
        "           \"parse({}): failed to parse, given %lu params, {} expected\",\n"
        "           wqe->api_cxt->params.size()\n"
        "       );\n"
        "       retval = POS_FAILED_INVALID_INPUT;\n"
        "       goto exit;\n"
        "   }}\n"
        "#endif\n"
        ,
        vendor_api_meta->params.size(),
        api_snake_name,
        vendor_api_meta->params.size()
    ));

    // obtain resources
    

    parser_file->archive();

exit:
    return retval;
}