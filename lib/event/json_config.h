#ifndef LIB_EVENT_JSON_CONFIG_H_
#define LIB_EVENT_JSON_CONFIG_H_

#include "spdk/event.h"
#include "spdk/json.h"


void spdk_app_json_config_load_cb(void *_json_path, void *_done_event);

#endif /* LIB_EVENT_JSON_CONFIG_H_ */
