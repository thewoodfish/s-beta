/* 
 * Author: Woodfish
 * File: substrate_interface.h
 * Desc: A specialized library to interface with a Substrate node.
 * Date: May 27 20:47
 * Licence: MIT
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include "utility.h"
#include "network.h"
#include "substrate_interface.h"
#include "../xxHash/xxhash.h"

// initialize Self struct
void init_client(
    const char* url, int websocket, int ss58_format, void* type_registry, const char* type_registry_preset, void* cache_region, void* runtime_config,
    bool use_remote_preset, struct Ws_option* ws_options, bool auto_discover, bool auto_reconnect
) {
    if ((strlen(url) == 0 && !websocket) || (strlen(url) > 0 && websocket)) {
        printf("%s", "Either 'url' or 'websocket' must be provided\n");
        exit(1);
    }

    // allocate global buffer
    buffer = (char*) malloc(GLOBAL_BUFFER_SPACE);
    chain_method = (char*) malloc(30);

    char* qbuf = (char*) malloc(10); // dummy buffer used for comparisons

    // runtime config 
    Self.runtime_config = runtime_config;
    Self.cache_region = cache_region;

    // allocate storage for latest cached block hash
    Self.block_hash = (char*) malloc(70);

    // allocate space for properties
    Self.properties = (struct Props*) malloc(sizeof(__Pr));
    Self.properties->ss58Format = ss58_format ? ss58_format : 42; // default is 42

    // allocate space for runtime config parameters
    Self.runtime_config = (struct Runtime_Config*) malloc(sizeof(__R_con));

    // allocate space for runtime version
    Self.run_version = (struct Runtime_Version*) malloc(sizeof(struct Runtime_Version));

    Self.type_registry_preset = type_registry_preset ? alloc_mem(type_registry_preset) : NULL;
    strcpy(Self.type_registry_preset, type_registry_preset);

    Self.type_registry = type_registry;

    Self.url = alloc_mem(url);
    strcpy(Self.url, url);

    Self.request_id = 1;

    // websocket connection options
    if (ws_options) {
        Self.ws_options = malloc(sizeof(ws_options));

        Self.ws_options->max_size = !Self.ws_options->max_size ?  (long) pow(2, 32) : 0;
        Self.ws_options->read_limit = !Self.ws_options->read_limit ?  (long) pow(2, 32) : 0;
        Self.ws_options->write_limit = !Self.ws_options->write_limit ?  (long) pow(2, 32) : 0;
    }

    if (Self.url && ((!strcmp(slice(Self.url, buffer, 0, 6), "wss://") && zero_buffer()) || !strcmp(slice(Self.url, qbuf, 0, 5), "ws://"))) {
        zero_buffer();
        connect_websocket();
    }
    else if (websocket) 
        Self.websocket = websocket;

    Self.default_handlers = malloc(sizeof(_d_andler));
    strcpy(Self.default_handlers->content_type, "application/json");
    strcpy(Self.default_handlers->cache_control, "no-cache");

    Self.config = malloc(sizeof(_c_def));
    Self.config->use_remote_preset = use_remote_preset ? use_remote_preset : true;
    Self.config->auto_discover = auto_discover ? auto_discover : true; 
    Self.config->auto_reconnect = auto_reconnect ? auto_reconnect : true;

    free(qbuf);

    // Self.session = session();

    // reload_type_registry(use_remote_preset, auto_discover);

} 

/**
* @brief Connect to a websocket
*/

static void connect_websocket()
{
    char* qbuf = (char*) malloc(10); // dummy buffer used for comparisons

    if (Self.url && (!strcmp(slice(Self.url, buffer, 0, 6), "wss://") || !strcmp(slice(Self.url, qbuf, 0, 5), "ws://"))) {
        printf("Connecting to %s ...\n", Self.url);

        // zero out buffer
        zero_buffer();

        // create connection and return socket descriptor
        Self.websocket = connect_websock(Self.url);

        possibly_exit_rudely();
    }

    free(qbuf);
}

static void possibly_exit_rudely(void) {
    if (Self.websocket <= 0) {
        printf("%s\n", "Aborting...");
        free_all_mem();
        exit(1);
    }
}

/**
* @brief Close the websocket
*
* @return Returns 0 on success, -1 otherwise.
*/
void close_websocket() 
{
    close_ws();

    // clear all RPC messages
    free_all_mem();
}

/**
* @brief Method that handles the actual RPC request to the Substrate node. The other implemented functions eventually
*        use this method to perform the request.

* @param result_handler: Callback function that processes the result received from the node
* @param method: method of the JSONRPC request
* @param params: a list containing the parameters of the JSONRPC request

* @returns a struct with the parsed result of the request.
*/

static char* rpc_request(char* method, char** params, void* result_handler)
{
    struct Payload pl;
    int request_id, update_nr, subscription_id;
    char* json_string;
    char* json_body = NULL;
    struct Req_queue* req; 
    struct Req_queue* rmq;

    // first allocate RMQ struct
    req = (struct Req_queue*) malloc(sizeof(__RMQ));
    
    request_id = Self.request_id;
    Self.request_id++;

    strcpy(pl.jsonrpc, "2.0");

    pl.method = alloc_mem(method);
    strcpy(pl.method, method);

    pl.params = params;
    pl.id = request_id;

    fprintf(stderr, "RPC request #%d: \"%s\"\n", request_id, method);

    if (Self.websocket) {
        // convert to JSON string
        json_string = alloc_mem(json_dump_payload(&pl));
        strcpy(json_string, json_dump_payload(&pl));
        zero_buffer();

        // send
        if (websocket_send(json_string) == -1) {
            // try to reconnect and send
            if (Self.config->auto_reconnect && strlen(Self.url) > 0) {
                printf("%s", "Connection closed: Trying to reconnect...");
                connect_websocket();

                rpc_request(method, params, result_handler);
            } else {
                // Fatal Error
                printf("%s", "Error: Could not reach server");
                exit(1);
            }
        }
        
        while (!json_body) {
            // receive message from server into buffer
            if (websocket_recv(buffer) == -1)
                fprintf(stderr, "%s", "Error, could not read from socket!\n");

            // busy wait on queue
            while (!flag)   ;
            reset_flag();

            // extract fields & data from JSON string. Check for errors
            parse_json_string(req, buffer);
            zero_buffer();

            // check for errors
            if (req->err_flag) {
                fprintf(stderr, "Error %s\n", req->result);
                // clear result
                memset(req->result, 0x00, strlen(req->result));
                return NULL;
            }

            // chain to linked list {to be freed later}
            append_rpc_message(req);

            /******** WEBSOCKET SUBSCRIPTIONS ***********/

            // // search for subscriptions
            // rmq = Self.rpc_message_queue;
            // while (rmq != NULL) {
            //     if (rmq->id && rmq->id == request_id) {

            //         if (result_handler != NULL) {
            //             // Set subscription ID and only listen to messages containing this ID
            //             subscription_id = rmq->id;
            //             fprintf(stderr, "Websocket subscription [%d] created", subscription_id);
            //         } else {
            //             json_body = alloc_mem(rmq->result);
            //             strcpy(json_body, rmq->result);
            //         }

            //         // remove from queue
            //         remove_rpc_message(rmq);
            //     }
            // }
            /******** WEBSOCKET SUBSCRIPTIONS ***********/

            json_body = req->result;

        }

    }

    return json_body;

}

/**
* @brief Free all RPC message queue
*/
static void free_all_mem() {
    while (Self.rpc_message_queue != NULL) {
        remove_rpc_message(Self.rpc_message_queue);
        Self.rpc_message_queue = Self.rpc_message_queue->next;
    }

    free(buffer);
    free(chain_method);
}

char* sc_name() {
    possibly_exit_rudely();

    char* buf;
    char** param = NULL;

    // check for errors
    if (buf = rpc_request("system_name", param, NULL)) {
        if (Self.name == NULL) {
            Self.name = alloc_mem(buf);
            strcpy(Self.name, buf);
        }
    }

    return Self.name;
}

struct Props* sc_properties() {
    possibly_exit_rudely();

    char* buf;
    char** param = NULL;

    if (!strlen(Self.properties->tokenSymbol)) {
        buf = rpc_request("system_properties", param, NULL);
        if (strcmp(buf, "empty")) { // if buf is not "empty"
            parse_system_props(Self.properties, buf);
        }
    }

    return Self.properties;
}

char* sc_chain() {
    possibly_exit_rudely();

    char* buf;
    char** param = NULL;

    // check for errors
    if (buf = rpc_request("system_chain", param, NULL)) {
        if (Self.chain == NULL) {
            Self.chain = alloc_mem(buf);
            strcpy(Self.chain, buf);
        }
    }

    return Self.chain;
}

char* sc_version() {
    possibly_exit_rudely();

    char* buf;
    char** param = NULL;

    // check for errors
    if (buf = rpc_request("system_version", param, NULL)) {
        if (Self.version == NULL) {
            Self.version = alloc_mem(buf);
            strcpy(Self.version, buf);
        }
    }

    return Self.version;
}

int sc_token_decimals() {
    possibly_exit_rudely();

    if (!Self.token_decimals) 
        if (Self.properties->tokenDecimals) 
            Self.token_decimals = Self.properties->tokenDecimals;
    
    return Self.token_decimals;
}

int set_token_decimal(int val) {
    possibly_exit_rudely();

    Self.token_decimals = val;

    return Self.token_decimals;
}

char* sc_token_symbol() {
    possibly_exit_rudely();

    if (!strlen(Self.token_symbol)) {
        if (strlen(Self.properties->tokenSymbol)) 
            strcpy(Self.token_symbol, Self.properties->tokenSymbol);
        else 
            strcpy(Self.token_symbol, "UNIT");
    }

    return Self.token_symbol;
}

char* set_token_symbol(const char* token) {
    clear_n_copy(Self.token_symbol, token);

    return Self.token_symbol;
}

int sc_ss58_format() {
    possibly_exit_rudely();

    if (!Self.ss58_format) 
        if (Self.properties->ss58Format) 
            Self.ss58_format = Self.properties->ss58Format;
    else
        Self.ss58_format = 42;
    
    return Self.ss58_format;
}

int set_ss58_format(int val) {
    possibly_exit_rudely();

    Self.ss58_format = val;
    Self.runtime_config->ss58_format = val;

    return Self.ss58_format;
}

char* sc_get_chain_head() {
    possibly_exit_rudely();

    char** param = NULL;
    return rpc_request("chain_getHead", param, NULL);
}

char* sc_get_chain_finalised_head() {
    possibly_exit_rudely();

    char** param = NULL;
    return rpc_request("chain_getFinalisedHead", param, NULL);
}

char* sc_get_block_hash(const char* block_id) {
    possibly_exit_rudely();

    // make sure 'block_id' is a hexadecimal string
    char* buf;
    char* param[2];
    char* result;

    buf = (char*) malloc(10);

    sprintf(buf, "%s", block_id);
    add_param(param, buf);

    result = rpc_request("chain_getBlockHash", param, NULL);

    free(buf);
    return result;
}

inline static void add_param(char** param, char* buf) {
    param[0] = buf;
    param[1] = NULL;
}

struct Block* sc_get_chain_block(const char* block_hash, const char* block_id) 
{
    possibly_exit_rudely();

    char* param[2];
    char* buf;

    if (block_id)
        block_hash = sc_get_block_hash(block_id);

    add_param(param, (char*) block_hash);

    buf = rpc_request("chain_getBlock", param, NULL);

    // if error or is_empty
    if (is_error(buf))
        return NULL;

    // save block data to client
    return parse_and_cache_block(buf, "getBlock");
}

inline static bool is_error(const char* buf) {
    bool res;

    if (!buf || !strcmp(buf, "empty") || !strcmp(buf, "null") || strstr(buf, "Error") || !strcmp(buf, "(null)")) 
        res = true;
    else    
        res = false;

    return res;
}

int sc_get_block_number(const char* block_hash) 
{
    possibly_exit_rudely();

    char* buf;
    char* param[2];
    int block_no;
    struct Block* bl;

    block_no = 0;
    add_param(param, (char*) block_hash);
    buf = rpc_request("chain_getHeader", param, NULL);

    // if error or is_empty
    if (is_error(buf)) ;
    else {
        // save block data to client
        bl = parse_and_cache_block(buf, "getHeader");
        block_no = bl->block_number;

        // free block, we're not appending to cache
        free(bl);
    }

    return block_no;
}

char* sc_get_metadata(const char* block_hash) 
{
    possibly_exit_rudely();

    char* param[2] = { NULL, NULL };
    char* buf;

    // set global variable to indicate that it's metadata that we're getting
    // because its soo large unlike other data bytes array
    strcpy(chain_method, "state_getMetadata");
    
    if (block_hash)
        add_param(param, (char*) block_hash);

    buf = rpc_request("state_getMetadata", param, NULL);
    
    // clear
    clear_n_copy(chain_method, "");

    return buf;
}

char* sc_get_storage_by_key(const char* key) 
{
    possibly_exit_rudely();
    char* param[2];

    add_param(param, (char*) key);

    return rpc_request("state_getStorageAt", param, NULL);
}

void sc_get_block_runtime_version(struct Runtime_Version* runv, const char* block_hash)
{
    possibly_exit_rudely();

    char* buf;
    char* param[2];
    add_param(param, (char*) block_hash);

    buf = rpc_request("chain_getRuntimeVersion", param, NULL);
    if (is_error(buf))
        return;
    
    decode_runtime_string(runv, buf);
}

char* generate_storage_hash(const char* storage_module, const char* storage_function, char** params, char** hashers)
{
    // requires scale codec library
}

char* convert_storage_parameter(const char* scale_type, const char* value)
{
    // requires scale codec library
}

void init_runtime(const char* block_h, const char* block_id) 
{
    char* param[2];
    char* buf;
    struct Block* bl;
    struct Runtime_Version* run_v;
    char* meta;

    char* runtime_block_hash = (char*) malloc(70);
    char* block_hash = (char*) malloc(70);
    char* parent_hash = (char*) malloc(70);

    if (block_id && *block_hash) {
        fprintf(stderr, "%s\n", "Cannot provide block hash and block id at the same time");
        return;
    }

    if (*block_hash && !strcmp(Self.block_hash, block_hash) || (block_id && Self.block_id == hex_to_int(block_id)))
        return;

    if (block_id) 
        clear_n_copy(block_hash, sc_get_block_hash(block_id));

    if (*block_hash) 
        clear_n_copy(block_hash, sc_get_chain_head());

    clear_n_copy(Self.block_hash, block_hash);
    Self.block_id = hex_to_int(block_id);

    add_param(param, Self.block_hash);

    // get parent hash
    buf = rpc_request("chain_getHeader", param, NULL);
    bl = parse_and_cache_block(buf, "getHeader");

    // copy into parent hash
    strcpy(parent_hash, bl->parent_hash);

    if (!strcmp(parent_hash, "0x0000000000000000000000000000000000000000000000000000000000000000"))
        strcpy(runtime_block_hash, Self.block_hash);
    else
        strcpy(runtime_block_hash, parent_hash);

    sc_get_block_runtime_version(run_v, runtime_block_hash);
    
    if (Self.runtime_version == run_v->spec_version) 
        return;

    Self.runtime_version = run_v->spec_version;
    Self.transaction_version = run_v->transaction_version;

    // check if metadata is in cache, else retrieve and add it
    if (!metadata_is_cached()) {
        printf("Retreived metadata for %d from cache\n", Self.runtime_version);
    } else {
        // fetch metadata and cache it
        meta = (char*) malloc(70410);
        strcpy(meta, sc_get_metadata(runtime_block_hash));

        // add to cache
        cache_metadata(meta);

        printf("Retreived metadata for %d from substrate node\n", Self.runtime_version);
    }

    free(block_hash);
    free(parent_hash);
    free(runtime_block_hash);
} 

inline static void cache_metadata(const char* buf) 
{
    struct Metadata_Cache* end = Self.m_cache;
    struct Metadata_Cache* new;

    new = (struct Metadata_Cache*) malloc(sizeof(struct Metadata_Cache));
    new->runtime_v = Self.runtime_version;
    new->metadata = (char*) buf;
    new->next = NULL;
    
    if (end == NULL) 
        end = new;
    else {
        // else keep looping unti end
        while (end != NULL) 
            end = end->next;

        end->next = new;
    }
}

inline static bool metadata_is_cached() {
    // check if metadata for a rumtime version is cached
    struct Metadata_Cache* mc = Self.m_cache;
    
    while (mc != NULL) {
        if (mc->runtime_v == Self.runtime_version)
            return true;

        mc = mc->next;
    }

    return false;
}