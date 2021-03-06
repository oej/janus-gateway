/*! \file   janus_mqttevh.c
 *
 * \author Copyright Olle E. Johansson <oej@edvina.net>
 *
 * Based on the mqtt transport by
 * \author Andrei Nesterov <ae.nesterov@gmail.com>
 * and the RabbitMQ event plugin by
 * \author Piter Konstantinov <pit.here@gmail.com>
 *
 * \copyright GNU General Public License v3
 * \brief Janus MQTT transport plugin
 *
 * \ingroup eventhandlers
 * \ref eventhandlers
 *
 * \todo Add will handling
 * \todo Add TLS support (not SSL)
 * \todo Add support for addplugin in topic
 * \todo Improve the MQTT connect message (the first one)
 * \todo Fix random client id if not configured
 */

#include "eventhandler.h"

/* We use the Paho MQTT library  
 * http://www.eclipse.org/paho/clients/c/
 */
#include <MQTTAsync.h>

#include "../debug.h"
#include "../config.h"
#include "../utils.h"
#include "../events.h"

/* Plugin information */
#define JANUS_MQTTEVH_VERSION               1
#define JANUS_MQTTEVH_VERSION_STRING        "0.1.0"
#define JANUS_MQTTEVH_DESCRIPTION           "An MQTT event handler plugin for Janus."
#define JANUS_MQTTEVH_NAME                  "JANUS MqttEventHandler plugin"
#define JANUS_MQTTEVH_AUTHOR                "Olle E. Johansson, Edvina AB"
#define JANUS_MQTTEVH_PACKAGE               "janus.eventhandler.mqttevh"

/* Plugin methods */
janus_eventhandler *create(void);
static int janus_mqttevh_init(const char *config_path);
static void janus_mqttevh_destroy(void);
static int janus_mqttevh_get_api_compatibility(void);
static int janus_mqttevh_get_version(void);
static const char *janus_mqttevh_get_version_string(void);
static const char *janus_mqttevh_get_description(void);
static const char *janus_mqttevh_get_name(void);
static const char *janus_mqttevh_get_author(void);
static const char *janus_mqttevh_get_package(void);
static void janus_mqttevh_incoming_event(json_t *event);

static int janus_mqttevh_send_message(void *context, const char *topic, json_t *message);
static void *janus_mqttevh_handler(void *data);


/* Event handler setup */
static janus_eventhandler janus_mqttevh =
        JANUS_EVENTHANDLER_INIT (
                .init = janus_mqttevh_init,
                .destroy = janus_mqttevh_destroy,

                .get_api_compatibility = janus_mqttevh_get_api_compatibility,
                .get_version = janus_mqttevh_get_version,
                .get_version_string = janus_mqttevh_get_version_string,
                .get_description = janus_mqttevh_get_description,
                .get_name = janus_mqttevh_get_name,
                .get_author = janus_mqttevh_get_author,
                .get_package = janus_mqttevh_get_package,

                .incoming_event = janus_mqttevh_incoming_event,

                .events_mask = JANUS_EVENT_TYPE_NONE
        );

/* Fix an exit event */
static json_t exit_event;

/* Destruction of events */
static void janus_mqttevh_event_free(json_t *event) {
        if(!event || event == &exit_event) {
                return;
	}
        json_decref(event);
}

/* Queue of events to handle */
static GAsyncQueue *events = NULL;

/* Plugin creator */
janus_eventhandler *create(void) {
        JANUS_LOG(LOG_VERB, "%s created!\n", JANUS_MQTTEVH_NAME);
        return &janus_mqttevh;
};

/* API flags */
static gboolean janus_mqtt_evh_enabled_ = FALSE;
static GThread *handler_thread;
static volatile gint initialized = 0, stopping = 0;

/* JSON serialization options */

#define DEFAULT_ADDPLUGIN	1
#define	DEFAULT_ADDEVENT	1
#define	DEFAULT_KEEPALIVE	30
#define	DEFAULT_CLEANSESSION	0	/* Off */
#define DEFAULT_TIMEOUT		30
#define DEFAULT_DISCONNECT_TIMEOUT	100
#define DEFAULT_QOS		0
#define DEFAULT_RETAIN		0
#define DEFAULT_WILL_CONTENT	"{\"event\" : \"disconnect\" }"
#define DEFAULT_WILL_RETAIN	1
#define DEFAULT_WILL_QOS	0
#define DEFAULT_BASETOPIC	"/janus/events"
#define DEFAULT_MQTTURL		"tcp://localhost:1883"
#define DEFAULT_JSON_FORMAT	JSON_INDENT(3) | JSON_PRESERVE_ORDER

#define DEFAULT_TLS_ENABLE	FALSE
#define DEFAULT_TLS_VERIFY_PEER	FALSE
#define DEFAULT_TLS_VERIFY_HOST	FALSE

static size_t json_format_ = DEFAULT_JSON_FORMAT;

/* MQTT client context */
typedef struct janus_mqttevh_context {
	/* THe Paho MQTT client data structure */
	MQTTAsync client;

	int addplugin;
	int addevent;

	/* Connection data - authentication and url */
	struct {
		int keep_alive_interval;
		int cleansession;
		char *client_id;
		char *username;
		char *password;
		char *url;
	} connect;

	struct {
		int timeout;
	} disconnect;

	/* Data for publishing events */
	struct {
		char *topic;
		int qos;
		int retain;
	} publish;

	/* If we loose connection, the will is our last publish */
	struct {
		char *topic;
		int qos;
		int retain;
		char *content;
	} will;

	/* TLS connection data */
	struct {
		gboolean enable;
		char *cacert_file;
		char *cert_file;
		char *key_file;
		gboolean verify_peer;
		gboolean verify_host;
	} tls;
} janus_mqttevh_context;

/* Event handler methods */
static void janus_mqttevh_client_connection_lost(void *context, char *cause);
static int janus_mqttevh_client_connect(janus_mqttevh_context *ctx);
static void janus_mqttevh_client_connect_success(void *context, MQTTAsync_successData *response);
static void janus_mqttevh_client_connect_failure(void *context, MQTTAsync_failureData *response);
static int janus_mqttevh_client_reconnect(janus_mqttevh_context *ctx);
static void janus_mqttevh_client_reconnect_success(void *context, MQTTAsync_successData *response);
static void janus_mqttevh_client_reconnect_failure(void *context, MQTTAsync_failureData *response);
static int janus_mqttevh_client_disconnect(janus_mqttevh_context *ctx);
static void janus_mqttevh_client_disconnect_success(void *context, MQTTAsync_successData *response);
static void janus_mqttevh_client_disconnect_failure(void *context, MQTTAsync_failureData *response);
static int janus_mqttevh_client_publish_message(janus_mqttevh_context *ctx, const char *topic, int retain, char *payload );
static void janus_mqttevh_client_publish_janus_success(void *context, MQTTAsync_successData *response);
static void janus_mqttevh_client_publish_janus_failure(void *context, MQTTAsync_failureData *response);
static void janus_mqttevh_client_destroy_context(janus_mqttevh_context **ctx);

/* We only handle a single connection */
static janus_mqttevh_context *context_ = NULL;


/* Janus API methods */
static int janus_mqttevh_get_api_compatibility(void) {
        /* Important! This is what your plugin MUST always return: don't lie here or bad things will happen */
        return JANUS_EVENTHANDLER_API_VERSION;
}

static int janus_mqttevh_get_version(void) {
	return JANUS_MQTTEVH_VERSION;
}

static const char *janus_mqttevh_get_version_string(void) {
	return JANUS_MQTTEVH_VERSION_STRING;
}

static const char *janus_mqttevh_get_description(void) {
	return JANUS_MQTTEVH_DESCRIPTION;
}

static const char *janus_mqttevh_get_name(void) {
	return JANUS_MQTTEVH_NAME;
}

static const char *janus_mqttevh_get_author(void) {
	return JANUS_MQTTEVH_AUTHOR;
}

static const char *janus_mqttevh_get_package(void) {
	return JANUS_MQTTEVH_PACKAGE;
}

/* Send an JSON message to a MQTT topic */
static int janus_mqttevh_send_message(void *context, const char *topic, json_t *message) {
	char *payload = NULL;
	int rc = 0;
	janus_mqttevh_context *ctx;

	if(message == NULL) {
		return -1;
	}
	if(context == NULL) {
		/* We have no context, so skip and move on */
		json_decref(message);
		return -1;
	}
	JANUS_LOG(LOG_INFO, "About to send message to %s \n", topic);

#ifdef SKREP
	if (payload != NULL) {
		/* Free previous message */
		free(payload);
		payload = NULL;
	}
#endif
	ctx = (janus_mqttevh_context *) context;

	payload = json_dumps(message, json_format_);
	if (payload == NULL) {
		JANUS_LOG(LOG_ERR, "Can't convert message to string format \n");
		json_decref(message);
		return 0;
	}
	JANUS_LOG(LOG_INFO, "Converted message to JSON for %s \n", topic);
	/* Ok, lets' get rid of the message */
	json_decref(message);

	rc = janus_mqttevh_client_publish_message(ctx, topic, ctx->publish.retain, payload);

	if(rc != MQTTASYNC_SUCCESS) {
		JANUS_LOG(LOG_INFO, "Can't publish to MQTT topic: %s, return code: %d\n", ctx->publish.topic, rc);
	}

	JANUS_LOG(LOG_INFO, "Done with message to JSON for %s \n", topic);

	return 0;
}

static void janus_mqttevh_client_connection_lost(void *context, char *cause) {

	/* Notify handlers about this transport being gone */
	janus_mqttevh_context *ctx = (janus_mqttevh_context *)context;

	JANUS_LOG(LOG_INFO, "MQTT EVH connection %s lost cause of %s. Reconnecting...\n", ctx->connect.url, cause);
}

/* Set up connection to MQTT broker */
static int janus_mqttevh_client_connect(janus_mqttevh_context *ctx) {
	int rc;

	MQTTAsync_connectOptions options = MQTTAsync_connectOptions_initializer;
	options.keepAliveInterval = ctx->connect.keep_alive_interval;
	options.cleansession = ctx->connect.cleansession;
	options.username = ctx->connect.username;
	options.password = ctx->connect.password;
	options.automaticReconnect = TRUE;
	options.onSuccess = janus_mqttevh_client_connect_success;
	options.onFailure = janus_mqttevh_client_connect_failure;
	options.context = ctx;

	rc = MQTTAsync_connect(ctx->client, &options);
	return rc;
}

/* Callback for succesful connection to MQTT broker */
static void janus_mqttevh_client_connect_success(void *context, MQTTAsync_successData *response) {
	char topicbuf[512];

	JANUS_LOG(LOG_INFO, "MQTT EVH client has been successfully connected to the broker\n");

	janus_mqttevh_context *ctx = (janus_mqttevh_context *)context;

	json_t *info = json_object();

	json_object_set_new(info, "event", json_string("connected"));
	json_object_set_new(info, "eventhandler", json_string(JANUS_MQTTEVH_PACKAGE));
	snprintf(topicbuf, sizeof(topicbuf), "%s/%s", ctx->publish.topic, "status");
	json_incref(info);

	janus_mqttevh_send_message(context,  topicbuf, info);

	json_decref(info);
}

/* Callback for MQTT broker connection failure */
static void janus_mqttevh_client_connect_failure(void *context, MQTTAsync_failureData *response) {
	int rc = response ? response->code : 0;

	/* Notify handlers about this transport failure */
	JANUS_LOG(LOG_ERR, "MQTT EVH client has failed connecting to the broker, return code: %d. Reconnecting...\n", rc);

}

/* MQTT broker Reconnect function */
static int janus_mqttevh_client_reconnect(janus_mqttevh_context *ctx) {
	JANUS_LOG(LOG_INFO, "MQTT EVH client reconnecting to %s. Reconnecting...\n", ctx->connect.url);

	MQTTAsync_disconnectOptions options = MQTTAsync_disconnectOptions_initializer;
	options.onSuccess = janus_mqttevh_client_reconnect_success;
	options.onFailure = janus_mqttevh_client_reconnect_failure;
	options.context = ctx;
	options.timeout = ctx->disconnect.timeout;

	return MQTTAsync_disconnect(ctx->client, &options);
}

/* Callback for successful reconnection to MQTT broker */
static void janus_mqttevh_client_reconnect_success(void *context, MQTTAsync_successData *response) {
	janus_mqttevh_context *ctx = (janus_mqttevh_context *)context;
	char topicbuf[512];

	JANUS_LOG(LOG_INFO, "MQTT EVH client has been disconnected from %s. Reconnecting...\n", ctx->connect.url);

	int rc = janus_mqttevh_client_connect(context);
	if(rc != MQTTASYNC_SUCCESS) {
		const char *error;
		switch(rc) {
			case 1: error = "Connection refused - protocol version";
				break;
			case 2: error = "Connection refused - identifier rejected";
				break;
			case 3: error = "Connection refused - server unavailable";
				break;
			case 4: error = "Connection refused - bad credentials";
				break;
			case 5: error = "Connection refused - not authroized";
				break;
			default: error = "Connection refused - unknown error";
				break;
		}
		JANUS_LOG(LOG_FATAL, "Can't connect to MQTT broker, return code: %d (%s)\n", rc, error);
		return;
	} 
	json_t *info = json_object();
	json_object_set_new(info, "event", json_string("connected"));
	snprintf(topicbuf, sizeof(topicbuf), "%s/%s", ctx->publish.topic, "status");
	//janus_mqttevh_send_message(context,  topicbuf, info);
}

/* Callback for MQTT broker reconnect failure */
static void janus_mqttevh_client_reconnect_failure(void *context, MQTTAsync_failureData *response) {
	int rc = response ? response->code : 0;
	JANUS_LOG(LOG_ERR, "MQTT EVH client failed reconnecting to MQTT broker, return code: %d\n", rc);
}

/* Disconnect from MQTT broker */
static int janus_mqttevh_client_disconnect(janus_mqttevh_context *ctx) {
	MQTTAsync_disconnectOptions options = MQTTAsync_disconnectOptions_initializer;
	options.onSuccess = janus_mqttevh_client_disconnect_success;
	options.onFailure = janus_mqttevh_client_disconnect_failure;
	options.context = ctx;
	options.timeout = ctx->disconnect.timeout;
	return MQTTAsync_disconnect(ctx->client, &options);
}

/* Callback for succesful MQTT disconnect */
static void janus_mqttevh_client_disconnect_success(void *context, MQTTAsync_successData *response) {

	/* Notify handlers about this transport being gone */
	janus_mqttevh_context *ctx = (janus_mqttevh_context *)context;

	JANUS_LOG(LOG_INFO, "MQTT EVH client has been successfully disconnected from %s. Destroying the client...\n", ctx->connect.url);
	janus_mqttevh_client_destroy_context(&ctx);
}

/* Callback for MQTT disconnect failure */
void janus_mqttevh_client_disconnect_failure(void *context, MQTTAsync_failureData *response) {
	int rc = response ? response->code : 0;
	janus_mqttevh_context *ctx = (janus_mqttevh_context *)context;

	JANUS_LOG(LOG_ERR, "Can't disconnect from MQTT EVH broker %s, return code: %d\n", ctx->connect.url, rc);

	janus_mqttevh_client_destroy_context(&ctx);
}


/* Publish mqtt message using paho
 * Payload is a string. JSON objects should be stringified before calling this function.
 */
static int janus_mqttevh_client_publish_message(janus_mqttevh_context *ctx, const char *topic, int retain, char *payload ) 
{
	int rc; 

	MQTTAsync_responseOptions options;
	MQTTAsync_message msg = MQTTAsync_message_initializer;

	msg.payload = payload;
	msg.payloadlen = strlen(payload);
	msg.qos = ctx->publish.qos;
	msg.retained = retain;

	/* TODO: The payload if generated by json_dumps needs to be freed
	free(payload);
	payload = (char *) NULL;
	*/

	options.context = ctx;
	options.onSuccess = janus_mqttevh_client_publish_janus_success;
	options.onFailure = janus_mqttevh_client_publish_janus_failure;
	rc = MQTTAsync_sendMessage(ctx->client, topic, &msg, &options);
	if (rc == MQTTASYNC_SUCCESS) {
		JANUS_LOG(LOG_INFO, "MQTT EVH message sent to topic %s on %s. Result %d\n", topic, ctx->connect.url, rc);
	} else {
		JANUS_LOG(LOG_INFO, "FAILURE: MQTT EVH message propably not sent to topic %s on %s. Result %d\n", topic, ctx->connect.url, rc);
	}

	return rc;
}

/* Callback for successful MQTT publish */
static void janus_mqttevh_client_publish_janus_success(void *context, MQTTAsync_successData *response) {
	janus_mqttevh_context *ctx = (janus_mqttevh_context *)context;
	JANUS_LOG(LOG_HUGE, "MQTT EVH client has successfully published to MQTT base topic: %s\n", ctx->publish.topic);
}

/* Callback for MQTT publish failure 
 * 	Should we bring message into queue? Right now, we just drop it.
 */
static void janus_mqttevh_client_publish_janus_failure(void *context, MQTTAsync_failureData *response) {
	janus_mqttevh_context *ctx = (janus_mqttevh_context *)context;
	int rc = response ? response->code : 0;
	JANUS_LOG(LOG_ERR, "MQTT EVH client has failed publishing to MQTT topic: %s, return code: %d\n", ctx->publish.topic, rc);
}


/* Destroy Janus MQTT event handler session context */
static void janus_mqttevh_client_destroy_context(janus_mqttevh_context **ptr) {
	JANUS_LOG(LOG_INFO, "About to destroy context... \n");

	janus_mqttevh_context *ctx = (janus_mqttevh_context *)*ptr;

	if(ctx) {
		MQTTAsync_destroy(&ctx->client);
		if (ctx->publish.topic != NULL) {
			g_free(ctx->publish.topic);
		}
		if (ctx->connect.username != NULL) {
			g_free(ctx->connect.username);
		}
		if (ctx->connect.password != NULL) {
			g_free(ctx->connect.password);
		}
		g_free(ctx);
		*ptr = NULL;
	}

	JANUS_LOG(LOG_INFO, "%s destroyed!\n", JANUS_MQTTEVH_NAME);
}

/* This is not used here, but required by the api (even though docs says no ) */
static int janus_mqttevh_client_message_arrived(void *context, char *topicName, int topicLen, MQTTAsync_message *message) {
        janus_mqttevh_context *ctx = (janus_mqttevh_context *)context;
        gchar *topic = g_strndup(topicName, topicLen);
        //const gboolean janus = janus_mqtt_evh_enabled_ &&  !strcasecmp(topic, ctx->subscribe.topic);
        const gboolean janus = janus_mqtt_evh_enabled_ ;
        g_free(topic);

        if(janus && message->payloadlen) {
                JANUS_LOG(LOG_HUGE, "MQTT %s: Receiving %s EVH message over MQTT: %s\n", ctx->connect.url, "Janus", (char *)message->payload);
        }

        MQTTAsync_freeMessage(&message);
        MQTTAsync_free(topicName);
        return TRUE;
}

static void janus_mqttevh_client_delivery_complete(void *context, MQTTAsync_token token) {
	/* If you send with QoS, you get confirmation here */
}

/* Plugin implementation */
static int janus_mqttevh_init(const char *config_path) {
	int res = 0;
	janus_config_item *url_item;
	janus_config_item *username_item, *password_item, *topic_item, *addevent_item;
	janus_config_item *keep_alive_interval_item, *cleansession_item, *disconnect_timeout_item, *qos_item, *retain_item;

	if(g_atomic_int_get(&stopping)) {
		/* Still stopping from before */
		return -1;
	}
	if(config_path == NULL) {
		/* Invalid arguments */
		return -1;
	}
	/* Read configuration */
	char filename[255];
	g_snprintf(filename, 255, "%s/%s.cfg", config_path, JANUS_MQTTEVH_PACKAGE);
	JANUS_LOG(LOG_VERB, "Configuration file: %s\n", filename);
	janus_config *config = janus_config_parse(filename);
	if(config != NULL) {
		janus_config_print(config);
	}

	/* Initializing context */
	janus_mqttevh_context *ctx = g_malloc0(sizeof(struct janus_mqttevh_context));
	context_ = ctx;

	/* Set default values */
	/* Strings are set to default values later */
	ctx->addplugin = DEFAULT_ADDPLUGIN;
	ctx->addevent = DEFAULT_ADDEVENT;
	ctx->publish.qos = DEFAULT_QOS;
	ctx->publish.retain = DEFAULT_RETAIN;
	ctx->connect.username = NULL;
	ctx->connect.password = NULL;
	ctx->disconnect.timeout = DEFAULT_TIMEOUT;
	ctx->will.qos = DEFAULT_WILL_QOS;
	ctx->will.retain = DEFAULT_WILL_RETAIN;

	ctx->tls.enable = DEFAULT_TLS_ENABLE;
	ctx->tls.verify_peer = DEFAULT_TLS_VERIFY_PEER;
	ctx->tls.verify_host = DEFAULT_TLS_VERIFY_HOST;

	/* Setup the event handler, if required */
	janus_config_item *item = janus_config_get_item_drilldown(config, "general", "enabled");

	if(!item || !item->value || !janus_is_true(item->value)) {
		JANUS_LOG(LOG_WARN, "MQTT event handler disabled\n");
		goto error;
	}
	janus_mqtt_evh_enabled_ = TRUE;

	/* MQTT URL */
	url_item = janus_config_get_item_drilldown(config, "general", "url");
	ctx->connect.url= g_strdup((url_item && url_item->value) ? url_item->value : DEFAULT_MQTTURL);

	janus_config_item *client_id_item = janus_config_get_item_drilldown(config, "general", "client_id");

	ctx->connect.client_id = g_strdup((client_id_item && client_id_item->value) ? client_id_item->value : "guest");

	username_item = janus_config_get_item_drilldown(config, "general", "username");
	if (username_item && username_item->value) {
		ctx->connect.username = g_strdup(username_item->value);
	} else {
		ctx->connect.username = NULL;
	}
	
	password_item = janus_config_get_item_drilldown(config, "general", "password");
	if (password_item && password_item->value) {
		ctx->connect.password = g_strdup(password_item->value);
	} else {
		ctx->connect.password = NULL;
	}

	janus_config_item *json_item = janus_config_get_item_drilldown(config, "general", "json");
	if(json_item && json_item->value) {
		/* Check how we need to format/serialize the JSON output */
		if(!strcasecmp(json_item->value, "indented")) {
			/* Default: indented, we use three spaces for that */
			json_format_ = JSON_INDENT(3) | JSON_PRESERVE_ORDER;
		} else if(!strcasecmp(json_item->value, "plain")) {
			/* Not indented and no new lines, but still readable */
			json_format_ = JSON_INDENT(0) | JSON_PRESERVE_ORDER;
		} else if(!strcasecmp(json_item->value, "compact")) {
			/* Compact, so no spaces between separators */
			json_format_ = JSON_COMPACT | JSON_PRESERVE_ORDER;
		} else {
			JANUS_LOG(LOG_WARN, "Unsupported JSON format option '%s', using default (indented)\n", json_item->value);
			json_format_ = DEFAULT_JSON_FORMAT;
		}
	}


	/* Which events should we subscribe to? */
	item = janus_config_get_item_drilldown(config, "general", "events");
	if(item && item->value) {
		if(!strcasecmp(item->value, "none")) {
			/* Don't subscribe to anything at all */
			janus_flags_reset(&janus_mqttevh.events_mask);
		} else if(!strcasecmp(item->value, "all")) {
			/* Subscribe to everything */
			janus_flags_set(&janus_mqttevh.events_mask, JANUS_EVENT_TYPE_ALL);
		} else {
			/* Check what we need to subscribe to */
			gchar **subscribe = g_strsplit(item->value, ",", -1);
			if(subscribe != NULL) {
				gchar *index = subscribe[0];
				if(index != NULL) {
					int i=0;
					while(index != NULL) {
						while(isspace(*index)) {
							index++;
						}
						if(strlen(index)) {
							int flag = event_label_to_flag(index);
							if (flag) {
								janus_flags_set(&janus_mqttevh.events_mask, flag);
							}
						}
						i++;
						index = subscribe[i];
					}
				}
				g_strfreev(subscribe);
			}
		}
	}

	/* Connect configuration */
	keep_alive_interval_item = janus_config_get_item_drilldown(config, "general", "keep_alive_interval");
	ctx->connect.keep_alive_interval = (keep_alive_interval_item && keep_alive_interval_item->value) ? atoi(keep_alive_interval_item->value) : DEFAULT_KEEPALIVE;

	cleansession_item = janus_config_get_item_drilldown(config, "general", "cleansession");
	ctx->connect.cleansession = (cleansession_item && cleansession_item->value) ? atoi(cleansession_item->value) : DEFAULT_CLEANSESSION;

	/* Disconnect configuration */
	disconnect_timeout_item = janus_config_get_item_drilldown(config, "general", "disconnect_timeout");
	ctx->disconnect.timeout = (disconnect_timeout_item && disconnect_timeout_item->value) ? atoi(disconnect_timeout_item->value) : DEFAULT_DISCONNECT_TIMEOUT;

	topic_item = janus_config_get_item_drilldown(config, "general", "topic");
	if(!topic_item || !topic_item->value) {
		ctx->publish.topic = g_strdup(DEFAULT_BASETOPIC);
	} else {
		ctx->publish.topic = g_strdup(topic_item->value);
	}
	addevent_item = janus_config_get_item_drilldown(config, "general", "addevent");
	if(addevent_item && addevent_item->value && janus_is_true(addevent_item->value)) {
		ctx->addevent = TRUE;
	}
	retain_item = janus_config_get_item_drilldown(config, "general", "retain");
	if(retain_item && retain_item->value && janus_is_true(retain_item->value)) {
		ctx->publish.retain = atoi(retain_item->value);;
	}

	qos_item = janus_config_get_item_drilldown(config, "general", "qos");
	ctx->publish.qos = (qos_item && qos_item->value) ? atoi(qos_item->value) : 1;

	/* TLS config*/
	item = janus_config_get_item_drilldown(config, "general", "tls_enable");
	/* for old people */
	if (!item) {
		item = janus_config_get_item_drilldown(config, "general", "ssl_enable");
	}

	if(!item || !item->value || !janus_is_true(item->value)) {
		JANUS_LOG(LOG_INFO, "MQTTEventHandler: MQTT TLS support disabled\n");
	} else {
		ctx->tls.enable = TRUE;
		item = janus_config_get_item_drilldown(config, "general", "tls_cacert");
		if (!item) {
			item = janus_config_get_item_drilldown(config, "general", "ssl_cacert");
		}
		if(item && item->value) {
			ctx->tls.cacert_file = g_strdup(item->value);
		}

		item = janus_config_get_item_drilldown(config, "general", "tls_client_cert");
		if (!item) {
			item = janus_config_get_item_drilldown(config, "general", "ssl_client_cert");
		}
		if(item && item->value) {
			ctx->tls.cert_file = g_strdup(item->value);
		}
		item = janus_config_get_item_drilldown(config, "general", "tls_client_key");
		if (!item) {
			item = janus_config_get_item_drilldown(config, "general", "ssl_client_key");
		}
		if(item && item->value) {
			ctx->tls.key_file = g_strdup(item->value);
		}
		item = janus_config_get_item_drilldown(config, "general", "tls_verify_peer");
		if (!item) {
			item = janus_config_get_item_drilldown(config, "general", "ssl_verify_peer");
		}
		if(item && item->value && janus_is_true(item->value)) {
			ctx->tls.verify_peer = TRUE;
		}
		item = janus_config_get_item_drilldown(config, "general", "tls_verify_hostname");
		if (!item) {
			item = janus_config_get_item_drilldown(config, "general", "ssl_verify_hostname");
		}
		if(item && item->value && janus_is_true(item->value)) {
			ctx->tls.verify_host = TRUE;
		}
	}

	if(!janus_mqtt_evh_enabled_ ) {
		JANUS_LOG(LOG_WARN, "MQTT event handler support disabled, giving up\n");
		goto error;
	}

	/* Create a MQTT client */
	res = MQTTAsync_create(
		&ctx->client,
		ctx->connect.url,
		ctx->connect.client_id,
		MQTTCLIENT_PERSISTENCE_NONE,
		NULL);

	 if (res != MQTTASYNC_SUCCESS) {
		JANUS_LOG(LOG_FATAL, "Can't setup library for connection to  MQTT broker %s: error %d creating client...\n", ctx->connect.url, res);
		goto error;
	}
	/* Set callbacks. We should not really subscribe to anything but nevertheless */
	res = MQTTAsync_setCallbacks(ctx->client, 
			ctx,
			janus_mqttevh_client_connection_lost,
			janus_mqttevh_client_message_arrived,	//Needed
			janus_mqttevh_client_delivery_complete);

	if (res != MQTTASYNC_SUCCESS) {
		JANUS_LOG(LOG_FATAL, "Event handler : Can't setup MQTT broker %s: error %d setting up callbacks...\n", ctx->connect.url, res);
		goto error;
	}
	JANUS_LOG(LOG_INFO, "Event handler : About to connect to  MQTT broker %s: ...\n", ctx->connect.url);

	/* Connecting to the broker */
	int rc = janus_mqttevh_client_connect(ctx);
	if(rc != MQTTASYNC_SUCCESS) {

		const char *error;
		switch(rc) {
			case 1: error = "Connection refused - protocol version";
				break;
			case 2: error = "Connection refused - identifier rejected";
				break;
			case 3: error = "Connection refused - server unavailable";
				break;
			case 4: error = "Connection refused - bad credentials";
				break;
			case 5: error = "Connection refused - not authroized";
				break;
			default: error = "Connection refused - unknown error";
				break;
		}
		JANUS_LOG(LOG_FATAL, "Can't connect to MQTT broker, return code: %d (%s)\n", rc, error);
		goto error;
	} 

	/* Initialize the events queue */
	events = g_async_queue_new_full((GDestroyNotify) janus_mqttevh_event_free);
	g_atomic_int_set(&initialized, 1);

	/* Create the event handler thread */
	GError *error = NULL;
	handler_thread = g_thread_try_new("janus mqttevh handler", janus_mqttevh_handler, ctx, &error);
	if(error != NULL) {
		g_atomic_int_set(&initialized, 0);
		JANUS_LOG(LOG_FATAL, "Got error %d (%s) trying to launch the MQTT EventHandler handler thread...\n", error->code, error->message ? error->message : "??");
		goto error;
	}

	/* Done */
	if(config) {
		janus_config_destroy(config);
	}

	JANUS_LOG(LOG_INFO, "%s initialized!\n", JANUS_MQTTEVH_NAME);
	return 0;

error:
	/* If we got here, something went wrong */
	janus_mqttevh_client_destroy_context(&ctx);

	if(config) {
		janus_config_destroy(config);
	}
	return -1;
}

/*! \brief  Janus shutting down, clean up and get out of here */
static void janus_mqttevh_destroy(void) {

	if(!g_atomic_int_get(&initialized)) {
		/* We never started, so just quit */
		return;
	}
	g_atomic_int_set(&stopping, 1);

	/* Put the exit event on the queue to stop the other thread */
	g_async_queue_push(events, &exit_event);

	if(handler_thread != NULL) {
		g_thread_join(handler_thread);
		handler_thread = NULL;
	}

	g_async_queue_unref(events);
	events = NULL;

	/* Shut down the MQTT connection now */
	janus_mqttevh_client_disconnect(context_);

	g_atomic_int_set(&initialized, 0);
	g_atomic_int_set(&stopping, 0);
	JANUS_LOG(LOG_INFO, "%s destroyed!\n", JANUS_MQTTEVH_NAME);
}


/*! \brief Handle incoming event and push it on the queue for
   a separate thread to handle */
static void janus_mqttevh_incoming_event(json_t *event) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		/* Janus is closing or the plugin is: unref the event as we won't handle it */
		json_decref(event);
		return;
	}
	json_incref(event);
	g_async_queue_push(events, event);
}


/* Thread to handle incoming events and push them out on the MQTT

	We will publish events on multiple topics,
	depending on event type
	If base topic is configured to
		/janus/events
	then a handle event will be published to
		/janus/events/handle

*/
static void *janus_mqttevh_handler(void *data) {
	janus_mqttevh_context *ctx = (janus_mqttevh_context *) data;
	json_t *event = NULL;
	char topicbuf[512];

	JANUS_LOG(LOG_VERB, "Joining MqttEventHandler handler thread\n");

	while(g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping)) {
		/* Get event from queue */
		event = g_async_queue_pop(events);
		if(event == NULL) {	/* There was nothing in the queue */
			continue;
		}
		if(event == &exit_event) {
			break;
		}
		/* Handle event: just for fun, let's see how long it took for us to take care of this */
		json_t *created = json_object_get(event, "timestamp");
		if(created && json_is_integer(created)) {
			gint64 then = json_integer_value(created);
			gint64 now = janus_get_monotonic_time();
			JANUS_LOG(LOG_DBG, "Handled event after %"SCNu64" us\n", now-then);
		}

		int type = json_integer_value(json_object_get(event, "type"));
		const char *elabel = event_type_to_label(type);
		const char *ename = event_type_to_name(type);

		/* Hack to test new functions */
		if (elabel && ename) {
			JANUS_LOG(LOG_DBG, "Event label %s, name %s\n", elabel, ename);
			json_object_set_new(event, "eventtype", json_string(ename));
		} else {
			JANUS_LOG(LOG_DBG, "Can't get event label or name\n");
		}

		if(!g_atomic_int_get(&stopping)) {
			/* Convert event to string */
			if (ctx->addevent) {
				snprintf(topicbuf, sizeof(topicbuf), "%s/%s", ctx->publish.topic, event_type_to_label(type));
				JANUS_LOG(LOG_DBG, "Debug: MQTT Publish event on %s\n", topicbuf);
				janus_mqttevh_send_message(ctx, topicbuf, event);
			} else {
				janus_mqttevh_send_message(ctx, ctx->publish.topic, event);
			}
		}

		JANUS_LOG(LOG_VERB, "Debug: Thread done publishing MQTT Publish event on %s\n", topicbuf);
	}
	JANUS_LOG(LOG_VERB, "Leaving MQTTEventHandler handler thread\n");
	return NULL;
}
