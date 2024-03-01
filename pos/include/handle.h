#pragma once

#include <iostream>
#include <vector>
#include <algorithm>
#include <string>
#include <map>
#include <set>
#include <unordered_map>
#include <type_traits>

#include <stdint.h>
#include <assert.h>

#include "pos/include/common.h"
#include "pos/include/log.h"
#include "pos/include/utils/bipartite_graph.h"
#include "pos/include/checkpoint.h"

#define kPOS_HandleDefaultSize   (1<<4)

/*!
 *  \brief  idx of base resource types
 */
enum pos_handle_type_id_t : uint64_t {
    kPOS_ResourceTypeId_Unknown = 0,
    kPOS_ResourceTypeId_Device,
    kPOS_ResourceTypeId_Memory,
    kPOS_ResourceTypeId_Num_Base_Type
};

/*!
 *  \brief  status of a handle instance
 */
enum pos_handle_status_t : uint8_t {
    /*!
     *  \brief  the resource behind this handle is active 
     *          on the XPU device, if an op rely on this 
     *          handle, it's ok to launch
     */
    kPOS_HandleStatus_Active = 0,

    /*!
     *  \brief  the resource behind this handle has been
     *          released manually by the client
     *  \note   this status is marked under worker function
     */
    kPOS_HandleStatus_Deleted,

    /*!
     *  \brief  the resource behind this handle are going
     *          to be deleted
     *  \note   this status is marked under runtime function
     *  \note   once the handle is marked as this status in
     *          the runtime function, subsequent runtime
     *          function won't obtain this handle under
     *          get_handle_by_client_addr
     *  \note   it's ok for collect_broken_handles to skip
     *          such handle, as they still active currently
     *          (will be deleted under subsequent op)   
     */
    kPOS_HandleStatus_Delete_Pending,

    /*!
     *  \brief  the resource behind this handle is pending
     *          to be created on XPU
     */
    kPOS_HandleStatus_Create_Pending,

    /*!
     *  \brief  the resource behind this handle is broken
     *          on the XPU device, one need to restore the
     *          resource before launching any op that rely
     *          on it
     */
    kPOS_HandleStatus_Broken
};


// forward declaration
template<class T_POSHandle>
class POSHandleManager;


/*!
 *  \brief  a mapping of client-side and server-side handle, along with its metadata
 */
class POSHandle {
 public:
    /*!
     *  \param  client_addr_    the mocked client-side address of the handle
     *  \param  size_           size of the handle it self
     *  \param  hm              handle manager which this handle belongs to
     *  \param  state_size_     size of the resource state behind this handle
     *  \note   this constructor is for software resource, whose client-side address
     *          and server-side address could be seperated
     */
    POSHandle(
        void *client_addr_, size_t size_, void* hm, size_t state_size_=0
    ) : client_addr(client_addr_), server_addr(nullptr), size(size_),
        dag_vertex_id(0), resource_type_id(kPOS_ResourceTypeId_Unknown),
        status(kPOS_HandleStatus_Create_Pending), state_size(state_size_),
        ckpt_bag(nullptr), _hm(hm) {}
    
    /*!
     *  \param  size_           size of the resources represented by this handle
     *  \param  hm              handle manager which this handle belongs to
     *  \param  state_size_     size of the resource state behind this handle
     *  \note   this constructor is for hardware resource, whose client-side address
     *          and server-side address should be equal (e.g., memory)
     */
    POSHandle(size_t size_, void* hm, size_t state_size_=0)
        :   client_addr(nullptr), server_addr(nullptr), size(size_),
            dag_vertex_id(0), resource_type_id(kPOS_ResourceTypeId_Unknown),
            status(kPOS_HandleStatus_Create_Pending), state_size(state_size_),
            ckpt_bag(nullptr), _hm(hm) {}
    
    virtual ~POSHandle() = default;

    /*!
     *  \brief  setting the server-side address of the handle after finishing allocation
     *  \param  addr  the server-side address of the handle
     */
    inline void set_server_addr(void *addr){ server_addr = addr; }
    
    /*!
     *  \brief  setting both the client-side and server-side address of the handle 
     *          after finishing allocation
     *  \param  addr        the setting address of the handle
     *  \param  handle_ptr  pointer to current handle
     *  \return POS_SUCCESS for successfully setting
     *          POS_FAILED_ALREADY_EXIST for duplication failed;
     */
    pos_retval_t set_passthrough_addr(void *addr, POSHandle* handle_ptr);

    /*!
    *  \brief  record a new parent handle of current handle
    */
    inline void record_parent_handle(POSHandle* parent){
        POS_CHECK_POINTER(parent);
        parent_handles.push_back(parent);
    }

    struct _pos_broken_handle_list_iter;

    /*!
     *  \brief  wrapper map to store broken handles
     */
    typedef struct pos_broken_handle_list {
        /*!
         *  \brief  list of broken handles
         *  \note   outter index: layer id
         */
        std::vector<std::vector<POSHandle*>*> _broken_handles;
        inline uint16_t get_nb_layers(){ return _broken_handles.size(); }

        /*!
         *  \brief  add new broken handle to the map
         *  \param  layer_id    index of the layer that this broken handle locates in
         *  \param  handle      pointer to the broken handle
         */
        inline void add_handle(uint16_t layer_id, POSHandle* handle){
            std::vector<POSHandle*> *vec;

            while(layer_id >= _broken_handles.size()){
                POS_CHECK_POINTER(vec = new std::vector<POSHandle*>());
                _broken_handles.push_back(vec);
            }

            _broken_handles[layer_id]->push_back(handle);
        }

        /*!
         *  \brief  reset this map (i.e., clear all recorded broken handles)
         */
        inline void reset(){
            uint16_t i;
            for(i=0; i<_broken_handles.size(); i++){
                if(likely(_broken_handles[i] != nullptr)){
                    _broken_handles[i]->clear();
                }
            }
        }

        /*!
         *  \brief  repeatly call this function to traverse the current list
         *  \param  layer_id_keeper     keeping the intermedia traverse layer id
         *                              [default value should be the return value of get_nb_layers()]
         *  \param  handle_id_keeper    keeping the intermedia traverse handle id within the layer
         *                              [default value should be 0]
         *  \return non-nullptr for the obtained handle; nullptr for reaching the end of traversing
         */
        inline POSHandle* reverse_get_handle(uint16_t& layer_id_keeper, uint64_t& handle_id_keeper){
            POSHandle *retval = nullptr;
            
            POS_CHECK_POINTER(_broken_handles[layer_id_keeper]);

            if(unlikely(handle_id_keeper >= _broken_handles[layer_id_keeper]->size())){
                if(layer_id_keeper == 0){
                    goto exit;
                } else {
                    layer_id_keeper -= 1;
                    handle_id_keeper = 0;
                }
            }

            POS_CHECK_POINTER(_broken_handles[layer_id_keeper]);

            if(_broken_handles[layer_id_keeper]->size() > 0)
                retval = (*(_broken_handles[layer_id_keeper]))[handle_id_keeper];
            
            handle_id_keeper += 1;

        exit:
            return retval;
        }

        /*!
         *  \brief  deconstructor
         */
        ~pos_broken_handle_list(){
            uint16_t i;
            for(i=0; i<_broken_handles.size(); i++){
                if(likely(_broken_handles[i] != nullptr))
                   delete _broken_handles[i];
            }
        }
    } pos_broken_handle_list_t;

    /*!
     *  \brief  collect all broken handles along the handle trees
     *  \note   this function will call recursively, aware of performance issue!
     *  \param  broken_handle_list  list of broken handles, 
     *  \param  layer_id            index of the layer at this call
     */
    inline void collect_broken_handles(pos_broken_handle_list_t *broken_handle_list, uint16_t layer_id = 0){
        uint64_t i;

        POS_CHECK_POINTER(broken_handle_list);

        // insert itself to the nonactive_handles map if itsn't active
        if(unlikely(status != kPOS_HandleStatus_Active && status != kPOS_HandleStatus_Delete_Pending)){
            broken_handle_list->add_handle(layer_id, this);
        }
        
        // iterate over its parent
        for(i=0; i<parent_handles.size(); i++){
            parent_handles[i]->collect_broken_handles(broken_handle_list, layer_id+1);
        }
    }

    /*!
     *  \brief  identify whether a given address is located within the resource
     *          that current handle represents
     *  \param  addr    the given address
     *  \param  offset  pointer to store the offset of the given address from the base
     *                  address, if the given address is located within the resource
     *                  that current handle represents
     *  \return identify result
     */
    inline bool is_client_addr_in_range(void *addr, uint64_t *offset=nullptr){
        bool result;

        result = ((uint64_t)client_addr <= (uint64_t)addr) && ((uint64_t)addr < (uint64_t)(client_addr)+size);

        if(result && offset != nullptr){
            *offset = (uint64_t)addr - (uint64_t)client_addr;
        }

        return result;
    }

    /*!
     *  \brief  mark the status of this handle
     *  \param  status the status to mark
     *  \note   this function would call the inner function within the corresponding handle manager
     */
    void mark_status(pos_handle_status_t status);


    /*!
     *  \brief  record host value of the handle under specific version
     *  \param  data    pointer to the remoting buffer, which contains the host-side value
     *  \param  size    size of the host-side value
     *  \param  version version (pc index) of the host-side value
     */
    inline void record_host_value(void* data, uint64_t size, uint64_t version){
        uint8_t* host_value;

        POS_CHECK_POINTER(data);
        POS_ASSERT(size > 0);

        host_value = (uint8_t*)malloc(size);
        POS_CHECK_POINTER(host_value);
        memcpy(host_value, data, size);

        host_value_map[version] = { host_value, size };
    }

    /*!
     *  \brief  checkpoint the state of the resource behind this handle
     *  \note   only handle of stateful resource should implement this method
     *  \param  version_id  version of this checkpoint
     *  \param  stream_id   index of the stream to do this checkpoint
     *  \return POS_SUCCESS for successfully checkpointed
     */
    virtual pos_retval_t checkpoint(uint64_t version_id, uint64_t stream_id=0) const { 
        return POS_FAILED_NOT_IMPLEMENTED; 
    }

    /*!
     *  \brief  invalidate the latest checkpoint due to computation / checkpoint conflict
     *          (used by async checkpoint)
     *  \return POS_SUCCESS for successfully invalidate
     *          POS_NOT_READY for no checkpoint had been record
     */
    virtual pos_retval_t invalidate_latest_checkpoint() {
        return POS_FAILED_NOT_IMPLEMENTED;
    }

    /*!
     *  \brief  restore the current handle when it becomes broken status
     *  \return POS_SUCCESS for successfully restore
     */
    virtual pos_retval_t restore(){ return POS_FAILED_NOT_IMPLEMENTED; }
    

    /*!
     *  \brief  obtain the resource name begind this handle
     *  \return resource name begind this handle
     */
    virtual std::string get_resource_name(){ return std::string("unknown"); }

    /*!
     *  \brief  serilize the state of current handle into the binary area
     *  \param  serilized_area  pointer to the binary area
     *  \return POS_SUCCESS for successfully serilization
     */
    pos_retval_t serilize(void** serilized_area){
        pos_retval_t retval = POS_SUCCESS;
        uint64_t offset = 0;
        void* tmp_serilized_area;
        
        POS_CHECK_POINTER(serilized_area);

        *serilized_area = malloc(this->__get_basic_serilize_size() + this->__get_extra_serilize_size());
        POS_CHECK_POINTER(*serilized_area);

        retval = this->__serilize_basic(*serilized_area, offset);
        if(unlikely(retval != POS_SUCCESS)){
            POS_WARN_C("failed to serilize basic fields of handle");
            goto exit;
        }

        retval = this->__serilize_extra(*serilized_area, offset);
        if(unlikely(retval != POS_SUCCESS)){
            POS_WARN_C("failed to serilize extra fields of handle");
            goto exit;
        }
        
    exit:
        return retval;
    }

    /*!
    *  \brief  the typeid of the resource kind which this handle represents
    *  \note   the children class of this base class should replace this value
    *          with their own typeid
    */
    pos_resource_typeid_t resource_type_id;

    // status of the resource behind this handle
    pos_handle_status_t status;

    // the mocked client-side address of the handle
    void *client_addr;

    // the actual server-side address of the handle
    void *server_addr;

    // pointer to the instance of parent handle
    std::vector<POSHandle*> parent_handles;

    // id of the DAG vertex of this handle
    pos_vertex_id_t dag_vertex_id;

    /*!
    *  \brief    size of the resources represented by this handle
    *  \example  the size of the buffer represented by current handler 
    *            (i.e., a device memory pointer)
    *  \note     for some handles (e.g., cudaStream_t), this value should remain
    *            constant —— kPOS_HandleDefaultSize
    */
    size_t size;

    /*!
     *  \brief  size of the resource state behind this handle
     */
    size_t state_size;

    /*!
     *  \brief  bag of checkpoints, implemented by different ckpt optimization level
     *  \note   it must be initialized by different implementations of stateful handle,
     *          as they might require different allocators and deallocators, see function
     *          init_ckpt_bag
     */
    POSCheckpointBag *ckpt_bag;

    /*!
     *  \brief  map between (1) dag pc to (2) host-side new value of 
     *          the resource behind this handle
     *  \note   1. for those APIs which bring new value to handle from the host-side,
     *          we need to cache the host-side value in case we would reply this
     *          API call later
     *  \note   2. we might need to cache multiple versions of host-side new values,
     *          so we use a map here
     */
    std::map<uint64_t, std::pair<uint8_t*, uint64_t>> host_value_map;
    
 protected:
    /*!
     *  \note   the belonging handle manager
     */
    void *_hm;

    /*!
     *  \brief  obtain the serilization size of basic fields of POSHandle
     *  \return the serilization size of basic fields of POSHandle
     */
    inline uint64_t __get_basic_serilize_size(){
        return (
            /* resource_type_id */      sizeof(pos_resource_typeid_t)
            /* client_addr */           + sizeof(uint64_t)
            /* server_addr */           + sizeof(uint64_t)
            /* nb_parent_handle */      + sizeof(uint64_t)
            /* parent_handle_indices */ + parent_handles.size() * sizeof(pos_vertex_id_t)
            /* dag_vertex_id */         + sizeof(pos_vertex_id_t)
            /* size */                  + sizeof(uint64_t)
            /* state_size */            + sizeof(uint64_t)

            // TODO: in the future we might serilize multiple versions of checkpoint
            /* checkpoint version */    + sizeof(uint64_t)
            /* checkpoint size */       + sizeof(uint64_t)
            /* ckpt_state */            + state_size
        );
    }

    /*!
     *  \brief  obtain the serilization size of extra fields of specific POSHandle type
     *  \return the serilization size of extra fields of POSHandle
     */
    virtual uint64_t __get_extra_serilize_size(){
        return 0;
    }

    /*!
     *  \brief  serilize spefic field of the handle to the serilization area
     *  \param  dptr    the serilization memory to store the field
     *  \param  sptr    address of the field to be serilized
     *  \param  size    size of the field to be serilized
     *  \param  offset  offset from the base of serilization area after serilization this field
     */
    static void __serilize_write_field(void* dptr, void* sptr, uint64_t size, uint64_t& offset){
        if(likely(size > 0)){
            memcpy(dptr, sptr, size);
            dptr += size;
            offset += size;
        }
    }

    /*!
     *  \brief  serilize the basic state of current handle into the binary area
     *  \param  serilized_area  pointer to the binary area
     *  \param  offset          offset within the serlized_area after serilize basic fields
     *  \return POS_SUCCESS for successfully serilization
     */
    pos_retval_t __serilize_basic(void* serilized_area, uint64_t& offset){
        pos_retval_t retval = POS_SUCCESS;
        void *ptr = serilized_area;
        void *ckpt_data;
        uint64_t ckpt_version, ckpt_size;
        uint64_t _nb_parent_handles;
        std::pair<uint8_t*, uint64_t> host_ckpt;

        POS_CHECK_POINTER(ptr);
        
        _nb_parent_handles = parent_handles.size();

        POSHandle::__serilize_write_field(ptr, &resource_type_id, sizeof(pos_resource_typeid_t), offset);
        POSHandle::__serilize_write_field(ptr, &client_addr, sizeof(uint64_t), offset);
        POSHandle::__serilize_write_field(ptr, &server_addr, sizeof(uint64_t), offset);
        POSHandle::__serilize_write_field(ptr, &_nb_parent_handles, sizeof(uint64_t), offset);
        for(auto& parent_handle : parent_handles){
            POSHandle::__serilize_write_field(ptr, &(parent_handle->dag_vertex_id), sizeof(uint64_t), offset);
        }
        POSHandle::__serilize_write_field(ptr, &dag_vertex_id, sizeof(pos_vertex_id_t), offset);
        POSHandle::__serilize_write_field(ptr, &size, sizeof(uint64_t), offset);
        POSHandle::__serilize_write_field(ptr, &state_size, sizeof(uint64_t), offset);

        // copy checkpoint
        retval = ckpt_bag->get_latest_checkpoint(&ckpt_data, ckpt_version, ckpt_size);
        if(unlikely(retval == POS_FAILED_NOT_READY)){
            // no checkpoint found, we need to use the state passed from the host
            if(likely(host_value_map.size() > 0)){
                ckpt_version = (host_value_map.rbegin())->first;
                host_ckpt = (host_value_map.rbegin())->second;
                ckpt_data = host_ckpt.first;
                ckpt_size = host_ckpt.second;
            } else {
                ckpt_version = 0;
                ckpt_size = 0;
            }
        } else if(unlikely(retval != POS_SUCCESS)){
            POS_WARN_C_DETAIL("failed to obtain checkpoint while serilizing, is checkpointing turned on?");
            ckpt_version = 0;
            ckpt_size = 0;
        }

        POSHandle::__serilize_write_field(ptr, &ckpt_version, sizeof(uint64_t), offset);
        POSHandle::__serilize_write_field(ptr, &ckpt_size, sizeof(uint64_t), offset);
        if(likely(ckpt_size > 0)){
            POSHandle::__serilize_write_field(ptr, ckpt_data, state_size, offset);
        }
        
    exit:
        return retval;
    }

    /*!
     *  \brief  serilize the extra state of current handle into the binary area
     *  \param  serilized_area  pointer to the binary area
     *  \param  offset          offset within the serlized_area after serilize basic fields
     *  \return POS_SUCCESS for successfully serilization
     */
    virtual pos_retval_t __serilize_extra(void* serilized_area, uint64_t& offset){
        return POS_SUCCESS;
    }

    /*!
     *  \brief  initialize checkpoint bag of this handle
     *  \note   it must be implemented by different implementations of stateful 
     *          handle, as they might require different allocators and deallocators
     *  \return POS_SUCCESS for successfully initialization
     */
    virtual pos_retval_t init_ckpt_bag(){ return POS_FAILED_NOT_IMPLEMENTED; }
};

/*!
 *  \brief   manager for handles of a specific kind of resource
 *  \tparam  T_POSHandle  specific handle class for the resource
 */
template<class T_POSHandle>
class POSHandleManager {
 public:
    // range of the mocked client-side address
    #define kPOS_ResourceBaseAddr   0x555500000000
    #define kPOS_ResourceEndAddr    0xFFFFFFFFFFF0

    /*!
     *  \brief  constructor
     *  \param  passthrough indicate whether the handle's client-side and server-side address
     *                      are equal (true for hardware resource, false for software resource)
     */
    POSHandleManager(bool passthrough = false) 
        : _base_ptr(kPOS_ResourceBaseAddr), _passthrough(passthrough) {}

    ~POSHandleManager() = default;
    
    /*!
     *  \brief  allocate new mocked resource within the manager
     *  \param  handle          pointer to the mocked handle of the newly allocated resource
     *  \param  related_handles all related handles for helping allocate the mocked resource
     *                          (note: these related handles might be other types)
     *  \param  size            size of the newly allocated resource
     *  \param  expected_addr   the expected mock addr to allocate the resource (optional)
     *  \param  state_size      size of resource state behind this handle  
     *  \return POS_FAILED_DRAIN for run out of virtual address space; 
     *          POS_SUCCESS for successfully allocation
     */
    virtual pos_retval_t allocate_mocked_resource(
        T_POSHandle** handle, 
        std::map</* type */ uint64_t, std::vector<POSHandle*>> related_handles,
        size_t size = kPOS_HandleDefaultSize,
        uint64_t expected_addr = 0,
        uint64_t state_size = 0
    );
    
    /*!
     *  \brief  record a new handle that will be modified
     *  \param  handle  the handle that will be modified
     */
    inline void record_modified_handle(T_POSHandle* handle){
        POS_CHECK_POINTER(handle);
        _modified_handles.insert(handle);
    }

    /*!
     *  \brief  clear all records of modified handles
     */
    inline void clear_modified_handle(){ 
        _modified_handles.clear();
    }

    /*!
     *  \brief  get all records of modified handles
     *  \return all records of modified handles
     */
    inline std::set<T_POSHandle*>& get_modified_handles(){
        return _modified_handles;
    }

    /*!
     *  \brief  obtain a handle by given client-side address
     *  \param  client_addr the given client-side address
     *  \param  handle      the resulted handle
     *  \param  offset      pointer to store the offset of the given address from the base address
     *  \return POS_FAILED_NOT_EXIST for no corresponding handle exists;
     *          POS_SUCCESS for successfully founded
     */
    virtual pos_retval_t get_handle_by_client_addr(void* client_addr, T_POSHandle** handle, uint64_t* offset=nullptr);

    /*!
     *  \brief    last-used handle
     *  \example  for device handle manager, one need to record the last-used device for later usage
     *            (e.g., cudaGetDevice, cudaMalloc)
     */
    T_POSHandle* latest_used_handle;

    /*!
     *  \brief  obtain the number of recorded handles
     *  \return the number of recorded handles
     */
    inline uint64_t get_nb_handles(){ return _handles.size(); }

    /*!
     *  \brief  obtain a handle by given index
     *  \param  id  the specified index
     *  \return pointer to the founed handle or nullptr
     */
    inline T_POSHandle* get_handle_by_id(uint64_t id){
        if(unlikely(id >= this->get_nb_handles())){
            return nullptr;
        } else {
            return _handles[id];
        }
    }

    inline pos_retval_t mark_handle_status(T_POSHandle *handle, pos_handle_status_t status){
        typename std::map<uint64_t, T_POSHandle*>::iterator handle_map_iter;
        
        POS_CHECK_POINTER(handle);
        
        switch (status)
        {
        case kPOS_HandleStatus_Active:
            handle->status = kPOS_HandleStatus_Active;
            POS_DEBUG_C(
                "mark handle as \"Active\" status: client_addr(%p), server_addr(%p)",
                handle->client_addr, handle->server_addr
            );
            break;

        case kPOS_HandleStatus_Broken:
            handle->status = kPOS_HandleStatus_Broken;
            POS_DEBUG_C(
                "mark handle as \"Broken\" status: client_addr(%p), server_addr(%p)",
                handle->client_addr, handle->server_addr
            );
            break;

        case kPOS_HandleStatus_Create_Pending:
            handle->status = kPOS_HandleStatus_Create_Pending;
            POS_DEBUG_C(
                "mark handle as \"Create_Pending\" status: client_addr(%p), server_addr(%p)",
                handle->client_addr, handle->server_addr
            );
            break;

        case kPOS_HandleStatus_Delete_Pending:
            handle->status = kPOS_HandleStatus_Delete_Pending;

            // remove the handle from the address map
            handle_map_iter = _handle_address_map.find((uint64_t)(handle->client_addr));
            if (likely(handle_map_iter != _handle_address_map.end())) {
                _deleted_handle_address_map.insert({
                    /* client_addr */ (uint64_t)(handle->client_addr),
                    /* handle */ handle_map_iter->second
                });
                _handle_address_map.erase((uint64_t)(handle->client_addr));   
            }

            POS_DEBUG_C(
                "mark handle as \"Delete_Pending\" status: client_addr(%p), server_addr(%p)",
                handle->client_addr, handle->server_addr
            );
            break;

        case kPOS_HandleStatus_Deleted:
            handle->status = kPOS_HandleStatus_Deleted;

            // remove the handle from the address map (should be already deleted in the last case)
            handle_map_iter = _handle_address_map.find((uint64_t)(handle->client_addr));
            if (unlikely(handle_map_iter != _handle_address_map.end())) {
                POS_WARN_C_DETAIL("remove handle from address map when mark it as deleted, is this a bug?");
                _deleted_handle_address_map.insert({
                    /* client_addr */ (uint64_t)(handle->client_addr),
                    /* handle */ handle_map_iter->second
                });
                _handle_address_map.erase((uint64_t)(handle->client_addr));
            }

            POS_DEBUG_C(
                "mark handle as \"Deleted\" status: client_addr(%p), server_addr(%p)",
                handle->client_addr, handle->server_addr
            );
            break;
        
        default:
            POS_ERROR_C_DETAIL("unknown status %u", status);
        }
    }

    /*!
     *  \brief  record handle address to the address map
     *  \note   this function should be called right after a handle obtain its client-side address:
     *          (1) for non-passthrough handle: called within __allocate_mocked_resource;
     *          (2) for passthrough handle: called within handle->set_server_addr
     *  \param  addr    client-side address of the handle
     *  \param  handle  the handle to be recorded
     *  \return POS_SUCCESS for successfully recorded;
     *          POS_FAILED_ALREADY_EXIST for duplication failed
     */
    inline pos_retval_t record_handle_address(void* addr, T_POSHandle* handle){
        pos_retval_t retval = POS_SUCCESS;
        T_POSHandle *__tmp;
        uint64_t addr_u64 = (uint64_t)(addr);

        POS_CHECK_POINTER(handle);

        if(likely(POS_FAILED_NOT_EXIST == __get_handle_by_client_addr(addr, &__tmp))){
            _handle_address_map[addr_u64] = handle;
        } else {
            POS_CHECK_POINTER(__tmp);

            /*!
             *  \note   no need to be failed here, some handle will record duplicated resources on purpose, 
             *          e.g., CUFunction
             */
            // POS_WARN_C(
            //     "try to record duplicated handle to the manager: new_addr(%p), new_size(%lu), old_addr(%p), old_size(%lu)",
            //     addr, handle->size, __tmp->client_addr, __tmp->size
            // );
            // retval = POS_FAILED_ALREADY_EXIST;
        }

    exit:
        return retval;
    }

 protected:
    uint64_t _base_ptr;
    
    /*!
     *  \brief  indicate whether the handle's client-side and server-side address are 
     *          equal (true for hardware resource, false for software resource)
     */
    bool _passthrough;

    std::vector<T_POSHandle*> _handles;

    /*!
     *  \brief  this map records all modified buffers since last checkpoint, 
     *          will be updated during parsing, and cleared during launching
     *          checkpointing op
     */
    std::set<T_POSHandle*> _modified_handles;

    /*!
     *  \brief  allocate new mocked resource within the manager
     *  \param  handle          pointer to the mocked handle of the newly allocated resource
     *  \param  size            size of the newly allocated resource
     *  \param  expected_addr   the expected mock addr to allocate the resource (optional)
     *  \note   this function should be internally invoked by allocate_mocked_resource, which leave 
     *          to children class to implement
     *  \return POS_FAILED_DRAIN for run out of virtual address space; 
     *          POS_SUCCESS for successfully allocation
     */
    pos_retval_t __allocate_mocked_resource(
        T_POSHandle** handle, size_t size=kPOS_HandleDefaultSize, uint64_t expected_addr=0, uint64_t state_size = 0
    );

    /*!
     *  \brief  obtain a handle by given client-side address
     *  \param  client_addr the given client-side address
     *  \param  handle      the resulted handle
     *  \param  offset      pointer to store the offset of the given address from the base address
     *  \note   this function should be internally invoked by get_handle_by_client_addr, which leave 
     *          to children class to implement
     *  \return POS_FAILED_NOT_EXIST for no corresponding handle exists;
     *          POS_SUCCESS for successfully founded
     */
    pos_retval_t __get_handle_by_client_addr(void* client_addr, T_POSHandle** handle, uint64_t* offset=nullptr);

 private:
    std::map<uint64_t, T_POSHandle*> _handle_address_map;
    std::unordered_map<uint64_t, T_POSHandle*> _deleted_handle_address_map;
};


/*!
 *  \brief  allocate new mocked resource within the manager
 *  \param  handle          pointer to the mocked handle of the newly allocated resource
 *  \param  size            size of the newly allocated resource
 *  \param  related_handles all related handles for helping allocate the mocked resource
 *                          (note: these related handles might be other types)
 *  \param  expected_addr   the expected mock addr to allocate the resource (optional)
 *  \return POS_FAILED_DRAIN for run out of virtual address space; 
 *          POS_SUCCESS for successfully allocation
 */
template<class T_POSHandle>
pos_retval_t POSHandleManager<T_POSHandle>::allocate_mocked_resource(
    T_POSHandle** handle,
    std::map</* type */ uint64_t, std::vector<POSHandle*>> related_handles,
    size_t size,
    uint64_t expected_addr,
    uint64_t state_size
){
    return __allocate_mocked_resource(handle, size, state_size);
}

/*!
 *  \brief  allocate new mocked resource within the manager
 *  \param  handle          pointer to the mocked handle of the newly allocated resource
 *  \param  size            size of the newly allocated resource
 *  \param  expected_addr   the expected mock addr to allocate the resource (optional)
 *  \note   this function should be internally invoked by allocate_mocked_resource, which leave to children class to implement
 *  \return POS_FAILED_DRAIN for run out of virtual address space;
 *          POS_FAILED_ALREADY_EXIST for duplication failed;
 *          POS_SUCCESS for successfully allocation
 */
template<class T_POSHandle>
pos_retval_t POSHandleManager<T_POSHandle>::__allocate_mocked_resource(
    T_POSHandle** handle,
    size_t size,
    uint64_t expected_addr,
    uint64_t state_size
){
    pos_retval_t retval = POS_SUCCESS;

    POS_CHECK_POINTER(handle);

    if(this->_passthrough){
        *handle = new T_POSHandle(size, this, state_size);
        POS_CHECK_POINTER(*handle);
    } else {
        // if one want to create on an expected address, we directly move the pointer forward
        if(unlikely(expected_addr != 0)){
            _base_ptr = expected_addr;
        }

        // make sure the resource to be allocated won't exceed the range
        if(unlikely(kPOS_ResourceEndAddr - _base_ptr < size)){
            POS_WARN_C(
                "failed to allocate new resource, exceed range: request %lu bytes, yet %lu bytes left",
                size, kPOS_ResourceEndAddr - _base_ptr
            );
            retval = POS_FAILED_DRAIN;
            *handle = nullptr;
            goto exit;
        }

        *handle = new T_POSHandle((void*)_base_ptr, size, this, state_size);
        POS_CHECK_POINTER(*handle);

        // record client-side address to the map
        retval = record_handle_address((void*)(_base_ptr), *handle);
        if(unlikely(POS_SUCCESS != retval)){
            goto exit;
        }

        _base_ptr += size;
    }

    POS_DEBUG_C(
        "allocate new resource: _base_ptr(%p), size(%lu), POSHandle.resource_type_id(%u)",
        _base_ptr, size, (*handle)->resource_type_id
    );

    this->_handles.push_back(*handle);

  exit:
    return retval;
}

/*!
 *  \brief  obtain a handle by given client-side address
 *  \param  client_addr the given client-side address
 *  \param  handle      the resulted handle
 *  \param  offset      pointer to store the offset of the given address from the base address
 *  \return POS_FAILED_NOT_EXIST for no corresponding handle exists;
 *          POS_SUCCESS for successfully founded
 */
template<class T_POSHandle>
pos_retval_t POSHandleManager<T_POSHandle>::get_handle_by_client_addr(void* client_addr, T_POSHandle** handle, uint64_t* offset){
    return __get_handle_by_client_addr(client_addr, handle, offset);
}

/*!
 *  \brief  obtain a handle by given client-side address
 *  \param  client_addr the given client-side address
 *  \param  handle      the resulted handle
 *  \param  offset      pointer to store the offset of the given address from the base address
 *  \note   this function should be internally invoked by get_handle_by_client_addr, which leave to children class to implement
 *  \return POS_FAILED_NOT_EXIST for no corresponding handle exists;
 *          POS_SUCCESS for successfully founded
 */
template<class T_POSHandle>
pos_retval_t POSHandleManager<T_POSHandle>::__get_handle_by_client_addr(void* client_addr, T_POSHandle** handle, uint64_t* offset){
    pos_retval_t ret = POS_SUCCESS;
    T_POSHandle *handle_ptr;
    uint64_t i;
    uint64_t client_addr_u64 = (uint64_t)(client_addr);

    typename std::map<uint64_t, T_POSHandle*>::iterator handle_map_iter;

    POS_CHECK_POINTER(handle);
    
    /*!
     *  \note   direct case: the given address is exactly the base address
     */
    if(unlikely(this->_handle_address_map.count(client_addr_u64) > 0)){
        *handle = this->_handle_address_map[client_addr_u64];

        /*!
         *  \note   those handle that has been deleted (i.e., kPOS_HandleStatus_Deleted) and 
         *          are going to be deleted (i.e., kPOS_HandleStatus_Delete_Pending) must be
         *          not in the map! 
         */
        POS_ASSERT(
            (*handle)->status != kPOS_HandleStatus_Deleted 
            && (*handle)->status != kPOS_HandleStatus_Delete_Pending
        );

        if(unlikely(offset != nullptr)){
            *offset = 0;
        }
        goto exit;
    }
    
    /*!
     *  \brief  indirect case: the given address is beyond the base address
     *  \note   most of query will fall back to this part
     */
    handle_map_iter = this->_handle_address_map.lower_bound(client_addr_u64);
    if(handle_map_iter != this->_handle_address_map.begin()){
        // get the first handle less than the given address
        handle_map_iter--;
        handle_ptr = handle_map_iter->second;

        POS_ASSERT(
            handle_ptr->status != kPOS_HandleStatus_Deleted && handle_ptr->status != kPOS_HandleStatus_Delete_Pending
        );

        if(likely(
            (uint64_t)(handle_ptr->client_addr) <= client_addr_u64 
            && client_addr_u64 < (uint64_t)(handle_ptr->client_addr) + handle_ptr->size
        )){
            *handle = handle_ptr;

            if(offset != nullptr){
                *offset = client_addr_u64 - (uint64_t)(handle_ptr->client_addr);
            }

            goto exit;
        }
    }

not_found:
    *handle = nullptr;
    ret = POS_FAILED_NOT_EXIST;

exit:
    return ret;
}
