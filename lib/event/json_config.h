#ifndef LIB_EVENT_JSON_CONFIG_H_
#define LIB_EVENT_JSON_CONFIG_H_

#include "spdk/event.h"
#include "spdk/json.h"


/*
 * _app_opts - pointer to struct spdk_app_opts
 * _done_even - event called when configuration loading is done.
 */
void spdk_app_json_config_load_cb(void *_app_opts, void *_done_event);

#endif /* LIB_EVENT_JSON_CONFIG_H_ */
