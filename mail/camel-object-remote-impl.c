/*
 * 
 * Authors: Srinivasa Ragavan <sragavan@novell.com>
 *
 * */

#include <string.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <dbus/dbus.h>
#include "mail-dbus.h"
#include <camel/camel.h>
#include "camel-object-remote-impl.h"

extern GHashTable *store_hash;
extern GHashTable *folder_hash;
extern CamelSession *session;


#define CAMEL_SESSION_OBJECT_PATH "/org/gnome/evolution/camel/session"
#define MAIL_SESSION_OBJECT_PATH "/org/gnome/evolution/camel/session/mail"
#define CAMEL_FOLDER_OBJECT_PATH "/org/gnome/evolution/camel/folder"
#define CAMEL_STORE_OBJECT_PATH "/org/gnome/evolution/camel/store"

#define CAMEL_SESSION_INTERFACE	"org.gnome.evolution.camel.session.mail"
#define MAIL_SESSION_INTERFACE	"org.gnome.evolution.camel.session.mail"
#define CAMEL_STORE_INTERFACE "org.gnome.evolution.camel.store"
#define CAMEL_FOLDER_INTERFACE "org.gnome.evolution.camel.folder"

/* Session */
static DBusHandlerResult
dbus_listener_session_handler (DBusConnection *connection,
                                    DBusMessage    *message,
                                    void           *user_data);

static void 
session_signal_cb (CamelObject *sess, gpointer ev_data, gpointer data)
{
	/* Signal back to the caller */
	DBusMessage *signal;
	DBusConnection *dbus = e_dbus_connection_get();

	signal = dbus_message_new_signal (CAMEL_SESSION_OBJECT_PATH, 
					CAMEL_SESSION_INTERFACE, 
					"session_signal");
	/* It sucks here to pass the pointer across the object */
	dbus_message_append_args (signal, DBUS_TYPE_INT32, &ev_data, DBUS_TYPE_STRING, &data, DBUS_TYPE_INVALID);
	dbus_connection_send (dbus, signal, NULL);
	dbus_message_unref (signal);
	dbus_connection_flush(dbus);
}

static DBusHandlerResult
dbus_listener_session_handler (DBusConnection *connection,
                                    DBusMessage    *message,
                                    void           *user_data)
{
	const char *method = dbus_message_get_member (message);
	DBusMessage *return_val;
	gboolean added = FALSE;
	CamelSession *session = NULL;

  	printf ("D-Bus message: obj_path = '%s' interface = '%s' method = '%s' destination = '%s'\n",
           dbus_message_get_path (message),
           dbus_message_get_interface (message),
           dbus_message_get_member (message),
           dbus_message_get_destination (message));
	
	
	return_val = dbus_message_new_method_return (message);

	if (strcmp(method, "camel_object_hook_event") == 0) {
		char *signal, *data, *object_hash;
		unsigned int ret_id;

		dbus_message_get_args(message, NULL,
				DBUS_TYPE_STRING, &object_hash,
				DBUS_TYPE_STRING, &signal,
				DBUS_TYPE_STRING, &data,
				DBUS_TYPE_INVALID);

		ret_id = camel_object_hook_event (session, signal, (CamelObjectEventHookFunc)session_signal_cb, data);
		dbus_message_append_args (return_val, DBUS_TYPE_UINT32, &ret_id, DBUS_TYPE_INVALID);
		added = TRUE;
	} else /* FIXME: Free memory and return */
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (!added)
		dbus_message_append_args (return_val, DBUS_TYPE_INVALID);

	dbus_connection_send (connection, return_val, NULL);
	dbus_message_unref (return_val);
	dbus_connection_flush(connection);

	return DBUS_HANDLER_RESULT_HANDLED;

}

/* Store */
static DBusHandlerResult
dbus_listener_store_handler (DBusConnection *connection,
                                    DBusMessage    *message,
                                    void           *user_data);

static char *
hash_store (CamelObject *store)
{
	return e_dbus_get_store_hash (camel_service_get_url((CamelService *)store));
}

static void 
store_signal_cb (CamelObject *store, gpointer ev_data, gpointer data)
{
	/* Signal back to the caller */
	DBusMessage *signal;
	DBusConnection *dbus = e_dbus_connection_get();
	char *hash = hash_store (store);

	signal = dbus_message_new_signal (CAMEL_STORE_OBJECT_PATH, 
					CAMEL_STORE_INTERFACE, 
					"store_signal");
	/* It sucks here to pass the pointer across the object */
	dbus_message_append_args (signal, DBUS_TYPE_STRING, &hash, DBUS_TYPE_INT32, &ev_data, DBUS_TYPE_STRING, &data, DBUS_TYPE_INVALID);
	dbus_connection_send (dbus, signal, NULL);
	dbus_message_unref (signal);
	dbus_connection_flush(dbus);
}

static DBusHandlerResult
dbus_listener_store_handler (DBusConnection *connection,
                                    DBusMessage    *message,
                                    void           *user_data)
{
	const char *method = dbus_message_get_member (message);
	DBusMessage *return_val;
	gboolean added = FALSE;

  	printf ("D-Bus message: obj_path = '%s' interface = '%s' method = '%s' destination = '%s'\n",
           dbus_message_get_path (message),
           dbus_message_get_interface (message),
           dbus_message_get_member (message),
           dbus_message_get_destination (message));
	
	
	return_val = dbus_message_new_method_return (message);

	if (strcmp(method, "camel_object_hook_event") == 0) {
		char *signal, *data, *object_hash;
		unsigned int ret_id;
		CamelObject *obj;

		dbus_message_get_args(message, NULL,
				DBUS_TYPE_STRING, &object_hash,
				DBUS_TYPE_STRING, &signal,
				DBUS_TYPE_STRING, &data,
				DBUS_TYPE_INVALID);
		obj = g_hash_table_lookup (store_hash, object_hash);
		ret_id = camel_object_hook_event (obj, signal, (CamelObjectEventHookFunc)store_signal_cb, data);
		dbus_message_append_args (return_val, DBUS_TYPE_UINT32, &ret_id, DBUS_TYPE_INVALID);
		added = TRUE;
	} else /* FIXME: Free memory and return */
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (!added)
		dbus_message_append_args (return_val, DBUS_TYPE_INVALID);

	dbus_connection_send (connection, return_val, NULL);
	dbus_message_unref (return_val);
	dbus_connection_flush(connection);

	return DBUS_HANDLER_RESULT_HANDLED;

}


/* Folder */
static DBusHandlerResult
dbus_listener_folder_handler (DBusConnection *connection,
                                    DBusMessage    *message,
                                    void           *user_data);

static char *
hash_folder (CamelObject *obj)
{
	CamelFolder *folder = (CamelFolder *)obj;
	return e_dbus_get_folder_hash (camel_service_get_url((CamelService *)folder->parent_store), folder->full_name);
}

static void 
folder_signal_cb (CamelObject *folder, gpointer ev_data, gpointer data)
{
	/* Signal back to the caller */
	DBusMessage *signal;
	DBusConnection *dbus = e_dbus_connection_get();
	char *hash = hash_folder(folder);
	signal = dbus_message_new_signal (CAMEL_FOLDER_OBJECT_PATH, 
					CAMEL_FOLDER_INTERFACE, 
					"folder_signal");
	/* It sucks here to pass the pointer across the object */
	dbus_message_append_args (signal, DBUS_TYPE_STRING, &hash, DBUS_TYPE_INT32, &ev_data, DBUS_TYPE_STRING, &data, DBUS_TYPE_INVALID);
	dbus_connection_send (dbus, signal, NULL);
	dbus_message_unref (signal);
	dbus_connection_flush(dbus);
}

static DBusHandlerResult
dbus_listener_folder_handler (DBusConnection *connection,
                                    DBusMessage    *message,
                                    void           *user_data)
{
	const char *method = dbus_message_get_member (message);
	DBusMessage *return_val;
	gboolean added = FALSE;

  	printf ("D-Bus message: obj_path = '%s' interface = '%s' method = '%s' destination = '%s'\n",
           dbus_message_get_path (message),
           dbus_message_get_interface (message),
           dbus_message_get_member (message),
           dbus_message_get_destination (message));
	
	
	return_val = dbus_message_new_method_return (message);

	if (strcmp(method, "camel_object_hook_event") == 0) {
		char *signal, *data, *object_hash;
		unsigned int ret_id;
		CamelObject *obj;

		dbus_message_get_args(message, NULL,
				DBUS_TYPE_STRING, &object_hash,
				DBUS_TYPE_STRING, &signal,
				DBUS_TYPE_STRING, &data,
				DBUS_TYPE_INVALID);
		obj = g_hash_table_lookup (folder_hash, object_hash);
		ret_id = camel_object_hook_event (obj, signal, (CamelObjectEventHookFunc)folder_signal_cb, data);
		dbus_message_append_args (return_val, DBUS_TYPE_UINT32, &ret_id, DBUS_TYPE_INVALID);
		added = TRUE;
	} else /* FIXME: Free memory and return */
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (!added)
		dbus_message_append_args (return_val, DBUS_TYPE_INVALID);

	dbus_connection_send (connection, return_val, NULL);
	dbus_message_unref (return_val);
	dbus_connection_flush(connection);

	return DBUS_HANDLER_RESULT_HANDLED;

}


void
camel_object_remote_impl_init ()
{
	/* Do it better */
	//e_dbus_register_handler (CAMEL_SESSION_OBJECT_PATH, dbus_listener_session_handler, NULL);
	//e_dbus_register_handler (CAMEL_STORE_OBJECT_PATH, dbus_listener_store_handler, NULL);
	//e_dbus_register_handler (CAMEL_FOLDER_OBJECT_PATH, dbus_listener_folder_handler, NULL);
}

