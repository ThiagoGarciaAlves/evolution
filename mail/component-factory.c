/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/* component-factory.c
 *
 * Authors: Ettore Perazzoli <ettore@ximian.com>
 *
 * Copyright (C) 2000  Ximian, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <bonobo/bonobo-generic-factory.h>
#include <gal/widgets/e-gui-utils.h>

#include "camel.h"

#include "Evolution.h"
#include "evolution-storage.h"
#include "evolution-wizard.h"

#include "folder-browser-factory.h"
#include "evolution-shell-component.h"
#include "evolution-shell-component-dnd.h"
#include "folder-browser.h"
#include "mail.h"		/* YUCK FIXME */
#include "mail-config.h"
#include "mail-tools.h"
#include "mail-ops.h"
#include "mail-offline-handler.h"
#include "mail-local.h"
#include "mail-session.h"
#include "mail-mt.h"
#include "mail-importer.h"
#include "mail-vfolder.h"             /* vfolder_create_storage */

#include "component-factory.h"

#include "mail-send-recv.h"

char *default_drafts_folder_uri;
CamelFolder *drafts_folder = NULL;
char *default_sent_folder_uri;
CamelFolder *sent_folder = NULL;
char *default_outbox_folder_uri;
CamelFolder *outbox_folder = NULL;
char *evolution_dir;

#define COMPONENT_FACTORY_ID "OAFIID:GNOME_Evolution_Mail_ShellComponentFactory"
#define SUMMARY_FACTORY_ID   "OAFIID:GNOME_Evolution_Mail_ExecutiveSummaryComponentFactory"

static BonoboGenericFactory *component_factory = NULL;
static GHashTable *storages_hash;
static EvolutionShellComponent *shell_component;

enum {
	ACCEPTED_DND_TYPE_MESSAGE_RFC822,
	ACCEPTED_DND_TYPE_X_EVOLUTION_MESSAGE,
	ACCEPTED_DND_TYPE_TEXT_URI_LIST,
};

static char *accepted_dnd_types[] = {
	"message/rfc822",
	"x-evolution-message",    /* ...from an evolution message list... */
	"text/uri-list",          /* ...from nautilus... */
	NULL
};

enum {
	EXPORTED_DND_TYPE_TEXT_URI_LIST,
};

static char *exported_dnd_types[] = {
	"text/uri-list",          /* we have to export to nautilus as text/uri-list */
	NULL
};

static const EvolutionShellComponentFolderType folder_types[] = {
	{ "mail", "evolution-inbox.png", TRUE, accepted_dnd_types, exported_dnd_types },
	{ "mailstorage", "evolution-inbox.png", FALSE, NULL, NULL },
	{ "vtrash", "evolution-trash.png", FALSE, accepted_dnd_types, exported_dnd_types },
	{ NULL, NULL, FALSE, NULL, NULL }
};

static const char *schema_types[] = {
	"mailto",
	NULL
};

/* EvolutionShellComponent methods and signals.  */

static BonoboControl *
create_noselect_control (void)
{
	GtkWidget *label;

	label = gtk_label_new (_("This folder cannot contain messages."));
	gtk_widget_show (label);
	return bonobo_control_new (label);
}

static EvolutionShellComponentResult
create_view (EvolutionShellComponent *shell_component,
	     const char *physical_uri,
	     const char *folder_type,
	     BonoboControl **control_return,
	     void *closure)
{
	EvolutionShellClient *shell_client;
	GNOME_Evolution_Shell corba_shell;
	BonoboControl *control;
	
	shell_client = evolution_shell_component_get_owner (shell_component);
	corba_shell = bonobo_object_corba_objref (BONOBO_OBJECT (shell_client));

	if (g_strcasecmp (folder_type, "mail") == 0) {
		const char *noselect;
		CamelURL *url;
		
		url = camel_url_new (physical_uri, NULL);
		noselect = url ? camel_url_get_param (url, "noselect") : NULL;
		if (noselect && !g_strcasecmp (noselect, "yes"))
			control = create_noselect_control ();
		else
			control = folder_browser_factory_new_control (physical_uri,
								      corba_shell);
		camel_url_free (url);
	} else if (g_strcasecmp (folder_type, "mailstorage") == 0) {
		CamelService *store;
		EvolutionStorage *storage;
		
		store = camel_session_get_service (session, physical_uri,
						   CAMEL_PROVIDER_STORE, NULL);
		if (!store)
			return EVOLUTION_SHELL_COMPONENT_NOTFOUND;
		storage = g_hash_table_lookup (storages_hash, store);
		if (!storage) {
			camel_object_unref (CAMEL_OBJECT (store));
			return EVOLUTION_SHELL_COMPONENT_NOTFOUND;
		}
		
		if (!gtk_object_get_data (GTK_OBJECT (storage), "connected"))
			mail_scan_subfolders (CAMEL_STORE(store), storage);
		camel_object_unref (CAMEL_OBJECT (store));
		
		control = folder_browser_factory_new_control ("", corba_shell);
	} else if (g_strcasecmp (folder_type, "vtrash") == 0) {
		control = folder_browser_factory_new_control ("vtrash:file:/", corba_shell);
	} else
		return EVOLUTION_SHELL_COMPONENT_UNSUPPORTEDTYPE;
	
	if (!control)
		return EVOLUTION_SHELL_COMPONENT_NOTFOUND;
	
	*control_return = control;
	return EVOLUTION_SHELL_COMPONENT_OK;
}

static void
create_folder_done (char *uri, CamelFolder *folder, void *data)
{
	GNOME_Evolution_ShellComponentListener listener = data;
	CORBA_Environment ev;
	GNOME_Evolution_ShellComponentListener_Result result;

	if (folder)
		result = GNOME_Evolution_ShellComponentListener_OK;
	else
		result = GNOME_Evolution_ShellComponentListener_INVALID_URI;

	CORBA_exception_init(&ev);
	GNOME_Evolution_ShellComponentListener_notifyResult(listener, result, &ev);
	CORBA_Object_release(listener, &ev);
	CORBA_exception_free(&ev);
}

static void
create_folder (EvolutionShellComponent *shell_component,
	       const char *physical_uri,
	       const char *type,
	       const GNOME_Evolution_ShellComponentListener listener,
	       void *closure)
{
	char *uri;
	CORBA_Environment ev;
	
	CORBA_exception_init(&ev);
	if (!strcmp (type, "mail")) {
		/* This makes the uri start with mbox://file://, which
		   looks silly but turns into a CamelURL that has
		   url->provider of "mbox" */
		uri = g_strdup_printf ("mbox://%s", physical_uri);
		mail_create_folder (uri, create_folder_done, CORBA_Object_duplicate (listener, &ev));
	} else {
		GNOME_Evolution_ShellComponentListener_notifyResult (
			listener, GNOME_Evolution_ShellComponentListener_UNSUPPORTED_TYPE, &ev);
	}
	CORBA_exception_free (&ev);
}

static void
remove_folder_done (char *uri, gboolean removed, void *data)
{
	GNOME_Evolution_ShellComponentListener listener = data;
	GNOME_Evolution_ShellComponentListener_Result result;
	CORBA_Environment ev;
	
	if (removed)
		result = GNOME_Evolution_ShellComponentListener_OK;
	else
		result = GNOME_Evolution_ShellComponentListener_INVALID_URI;
	
	CORBA_exception_init (&ev);
	GNOME_Evolution_ShellComponentListener_notifyResult (listener, result, &ev);
	CORBA_Object_release (listener, &ev);
	CORBA_exception_free (&ev);
}

static void
remove_folder (EvolutionShellComponent *shell_component,
	       const char *physical_uri,
	       const char *type,
	       const GNOME_Evolution_ShellComponentListener listener,
	       void *closure)
{
	CORBA_Environment ev;
	
	CORBA_exception_init (&ev);
	
	if (strcmp (type, "mail") != 0) {
		GNOME_Evolution_ShellComponentListener_notifyResult (listener,
								     GNOME_Evolution_ShellComponentListener_UNSUPPORTED_TYPE, &ev);
		CORBA_exception_free (&ev);
		return;
	}
	
	mail_remove_folder (physical_uri, remove_folder_done, CORBA_Object_duplicate (listener, &ev));
	CORBA_exception_free (&ev);
}

static void
xfer_folder_done (gboolean ok, void *data)
{
	GNOME_Evolution_ShellComponentListener listener = data;
	GNOME_Evolution_ShellComponentListener_Result result;
	CORBA_Environment ev;
	
	if (ok)
		result = GNOME_Evolution_ShellComponentListener_OK;
	else
		result = GNOME_Evolution_ShellComponentListener_INVALID_URI;
	
	CORBA_exception_init (&ev);
	GNOME_Evolution_ShellComponentListener_notifyResult (listener, result, &ev);
	CORBA_Object_release (listener, &ev);
	CORBA_exception_free (&ev);
}

static void
xfer_folder (EvolutionShellComponent *shell_component,
	     const char *source_physical_uri,
	     const char *destination_physical_uri,
	     const char *type,
	     gboolean remove_source,
	     const GNOME_Evolution_ShellComponentListener listener,
	     void *closure)
{
	CORBA_Environment ev;
	const char *noselect;
	CamelFolder *source;
	CamelException ex;
	GPtrArray *uids;
	CamelURL *url;
	
	url = camel_url_new (destination_physical_uri, NULL);
	noselect = url ? camel_url_get_param (url, "noselect") : NULL;
	
	if (noselect && !g_strcasecmp (noselect, "yes")) {
		camel_url_free (url);
		GNOME_Evolution_ShellComponentListener_notifyResult (listener, 
								     GNOME_Evolution_ShellComponentListener_UNSUPPORTED_OPERATION, &ev);
		return;
	}
	
	camel_url_free (url);
	
	if (strcmp (type, "mail") != 0) {
		GNOME_Evolution_ShellComponentListener_notifyResult (listener,
								     GNOME_Evolution_ShellComponentListener_UNSUPPORTED_TYPE, &ev);
		return;
	}

	camel_exception_init (&ex);
	source = mail_tool_uri_to_folder (source_physical_uri, &ex);
	camel_exception_clear (&ex);
	
	CORBA_exception_init (&ev);
	if (source) {
		uids = camel_folder_get_uids (source);
		mail_transfer_messages (source, uids, remove_source, destination_physical_uri,
					xfer_folder_done,
					CORBA_Object_duplicate (listener, &ev));
	} else
		GNOME_Evolution_ShellComponentListener_notifyResult (listener, GNOME_Evolution_ShellComponentListener_INVALID_URI, &ev);
	CORBA_exception_free (&ev);
}

#if 0
static void
populate_folder_context_menu (EvolutionShellComponent *shell_component,
			      BonoboUIComponent *uic,
			      const char *physical_uri,
			      const char *type,
			      void *closure)
{
#ifdef TRANSLATORS_ONLY
	static char popup_xml_i18n[] = {N_("Properties..."), N_("Change this folder's properties")};
#endif
	static char popup_xml[] =
		"<menuitem name=\"ChangeFolderProperties\" verb=\"ChangeFolderProperties\""
		"          _label=\"Properties...\" _tip=\"Change this folder's properties\"/>";

	if (strcmp (type, "mail") != 0)
		return;

	bonobo_ui_component_set_translate (uic, EVOLUTION_SHELL_COMPONENT_POPUP_PLACEHOLDER,
					   popup_xml, NULL);
}
#endif

static char *
get_dnd_selection (EvolutionShellComponent *shell_component,
		   const char *physical_uri,
		   int type,
		   int *format_return,
		   const char **selection_return,
		   int *selection_length_return,
		   void *closure)
{
	g_print ("should get dnd selection for %s\n", physical_uri);
	
	return NULL;
}

/* Destination side DnD */
static CORBA_boolean
destination_folder_handle_motion (EvolutionShellComponentDndDestinationFolder *folder,
				  const char *physical_uri,
				  const GNOME_Evolution_ShellComponentDnd_DestinationFolder_Context *destination_context,
				  GNOME_Evolution_ShellComponentDnd_Action *suggested_action_return,
				  gpointer user_data)
{
	const char *noselect;
	CamelURL *url;
	
	g_print ("in destination_folder_handle_motion (%s)\n", physical_uri);
	
	url = camel_url_new (physical_uri, NULL);
	noselect = camel_url_get_param (url, "noselect");
	
	if (noselect && !g_strcasecmp (noselect, "yes"))
		/* uh, no way to say "illegal" */
		*suggested_action_return = GNOME_Evolution_ShellComponentDnd_ACTION_DEFAULT;
	else
		*suggested_action_return = GNOME_Evolution_ShellComponentDnd_ACTION_MOVE;
	
	camel_url_free (url);
	
	return TRUE;
}

static void
message_rfc822_dnd (CamelFolder *dest, CamelStream *stream, CamelException *ex)
{
	CamelMimeParser *mp;
	
	mp = camel_mime_parser_new ();
	camel_mime_parser_scan_from (mp, TRUE);
	camel_mime_parser_init_with_stream (mp, stream);
	
	while (camel_mime_parser_step (mp, 0, 0) == HSCAN_FROM) {
		CamelMessageInfo *info;
		CamelMimeMessage *msg;
		
		msg = camel_mime_message_new ();
		if (camel_mime_part_construct_from_parser (CAMEL_MIME_PART (msg), mp) == -1) {
			camel_object_unref (CAMEL_OBJECT (msg));
			break;
		}
		
		/* append the message to the folder... */
		info = g_new0 (CamelMessageInfo, 1);
		camel_folder_append_message (dest, msg, info, ex);
		camel_object_unref (CAMEL_OBJECT (msg));
		
		if (camel_exception_is_set (ex))
			break;
		
		/* skip over the FROM_END state */
		camel_mime_parser_step (mp, 0, 0);
	}
	
	camel_object_unref (CAMEL_OBJECT (mp));
}

static CORBA_boolean
destination_folder_handle_drop (EvolutionShellComponentDndDestinationFolder *dest_folder,
				const char *physical_uri,
				const GNOME_Evolution_ShellComponentDnd_DestinationFolder_Context *destination_context,
				const GNOME_Evolution_ShellComponentDnd_Action action,
				const GNOME_Evolution_ShellComponentDnd_Data *data,
				gpointer user_data)
{
	char *tmp, *url, **urls, *in, *inptr, *inend;
	gboolean retval = FALSE;
	const char *noselect;
	CamelFolder *folder;
	CamelStream *stream;
	CamelException ex;
	GPtrArray *uids;
	CamelURL *uri;
	int i, type, fd;
	
	if (action == GNOME_Evolution_ShellComponentDnd_ACTION_LINK)
		return FALSE; /* we can't create links */
	
	uri = camel_url_new (physical_uri, NULL);
	noselect = uri ? camel_url_get_param (uri, "noselect") : NULL;
	if (noselect && !g_strcasecmp (noselect, "yes")) {
		camel_url_free (uri);
		return FALSE;
	}
	camel_url_free (uri);
	
	g_print ("in destination_folder_handle_drop (%s)\n", physical_uri);
	
	for (type = 0; accepted_dnd_types[type]; type++)
		if (!strcmp (destination_context->dndType, accepted_dnd_types[type]))
			break;
	
	camel_exception_init (&ex);
	
	switch (type) {
	case ACCEPTED_DND_TYPE_TEXT_URI_LIST:
		folder = mail_tool_uri_to_folder (physical_uri, NULL);
		if (!folder)
			return FALSE;
		
		tmp = g_strndup (data->bytes._buffer, data->bytes._length);
		urls = g_strsplit (tmp, "\n", 0);
		g_free (tmp);
		
		retval = TRUE;
		for (i = 0; urls[i] != NULL && retval; i++) {
			/* get the path component */
			url = g_strstrip (urls[i]);
			
			uri = camel_url_new (url, NULL);
			g_free (url);
			url = uri->path;
			uri->path = NULL;
			camel_url_free (uri);
			
			fd = open (url, O_RDONLY);
			if (fd == -1) {
				g_free (url);
				/* FIXME: okay, so what do we do in this case? */
				continue;
			}
			
			stream = camel_stream_fs_new_with_fd (fd);
			message_rfc822_dnd (folder, stream, &ex);
			camel_object_unref (CAMEL_OBJECT (stream));
			camel_object_unref (CAMEL_OBJECT (folder));
			
			retval = !camel_exception_is_set (&ex);
			
			if (action == GNOME_Evolution_ShellComponentDnd_ACTION_MOVE && retval)
				unlink (url);
			
			g_free (url);
		}
		
		g_free (urls);
		break;
	case ACCEPTED_DND_TYPE_MESSAGE_RFC822:
		folder = mail_tool_uri_to_folder (physical_uri, &ex);
		if (!folder) {
			camel_exception_clear (&ex);
			return FALSE;
		}
		
		/* write the message(s) out to a CamelStream so we can use it */
		stream = camel_stream_mem_new ();
		camel_stream_write (stream, data->bytes._buffer, data->bytes._length);
		camel_stream_reset (stream);
		
		message_rfc822_dnd (folder, stream, &ex);
		camel_object_unref (CAMEL_OBJECT (stream));
		camel_object_unref (CAMEL_OBJECT (folder));
		break;
	case ACCEPTED_DND_TYPE_X_EVOLUTION_MESSAGE:
		/* format: "uri uid1\0uid2\0uid3\0...\0uidn" */
		
		in = data->bytes._buffer;
		inend = in + data->bytes._length;
		
		inptr = strchr (in, ' ');
		url = g_strndup (in, inptr - in);
		
		folder = mail_tool_uri_to_folder (url, &ex);
		g_free (url);
		
		if (!folder) {
			camel_exception_clear (&ex);
			return FALSE;
		}
		
		/* split the uids */
		inptr++;
		uids = g_ptr_array_new ();
		while (inptr < inend) {
			char *start = inptr;
			
			while (inptr < inend && *inptr)
				inptr++;
			
			g_ptr_array_add (uids, g_strndup (start, inptr - start));
			inptr++;
		}
		
		mail_transfer_messages (folder, uids,
					action == GNOME_Evolution_ShellComponentDnd_ACTION_MOVE,
					physical_uri, NULL, NULL);
		
		camel_object_unref (CAMEL_OBJECT (folder));
		break;
	default:
		break;
	}
	
	camel_exception_clear (&ex);
	
	return retval;
}


static struct {
	char *name, **uri;
	CamelFolder **folder;
} standard_folders[] = {
	{ "Drafts", &default_drafts_folder_uri, &drafts_folder },
	{ "Outbox", &default_outbox_folder_uri, &outbox_folder },
	{ "Sent", &default_sent_folder_uri, &sent_folder },
};

static void
unref_standard_folders (void)
{
	int i;
	
	for (i = 0; i < sizeof (standard_folders) / sizeof (standard_folders[0]); i++) {
		if (standard_folders[i].folder)
			camel_object_unref (CAMEL_OBJECT (*standard_folders[i].folder));
	}
}

static void
got_folder (char *uri, CamelFolder *folder, void *data)
{
	CamelFolder **fp = data;
	
	if (folder) {
		*fp = folder;
		camel_object_ref (CAMEL_OBJECT (folder));
	}
}

static void
owner_set_cb (EvolutionShellComponent *shell_component,
	      EvolutionShellClient *shell_client,
	      const char *evolution_homedir,
	      gpointer user_data)
{
	GNOME_Evolution_Shell corba_shell;
	const GSList *accounts;
#ifdef ENABLE_NNTP
	const GSList *news;
#endif
	int i;

	g_print ("evolution-mail: Yeeeh! We have an owner!\n");	/* FIXME */

	evolution_dir = g_strdup (evolution_homedir);
	mail_session_init ();
	
	storages_hash = g_hash_table_new (NULL, NULL);

	vfolder_create_storage (shell_component);

	corba_shell = bonobo_object_corba_objref (BONOBO_OBJECT (shell_client));
	
	accounts = mail_config_get_accounts ();
	mail_load_storages (corba_shell, accounts, TRUE);
	
#ifdef ENABLE_NNTP
	news = mail_config_get_news ();
	mail_load_storages (corba_shell, news, FALSE);
#endif

	mail_local_storage_startup (shell_client, evolution_dir);
	mail_importer_init (shell_client);
	
	for (i = 0; i < sizeof (standard_folders) / sizeof (standard_folders[0]); i++) {
		*standard_folders[i].uri = g_strdup_printf ("file://%s/local/%s", evolution_dir, standard_folders[i].name);
		mail_msg_wait (mail_get_folder (*standard_folders[i].uri, got_folder, standard_folders[i].folder));
	}
	
	mail_session_enable_interaction (TRUE);
	
	mail_autoreceive_setup ();
}

static void
free_storage (gpointer service, gpointer storage, gpointer data)
{
	camel_service_disconnect (CAMEL_SERVICE (service), TRUE, NULL);
	camel_object_unref (CAMEL_OBJECT (service));
	bonobo_object_unref (BONOBO_OBJECT (storage));
}

static gboolean
idle_quit (gpointer user_data)
{
	if (e_list_length (folder_browser_factory_get_control_list ()))
		return TRUE;

	bonobo_object_unref (BONOBO_OBJECT (component_factory));
	g_hash_table_foreach (storages_hash, free_storage, NULL);
	g_hash_table_destroy (storages_hash);

	gtk_main_quit ();

	return FALSE;
}	

static void
owner_unset_cb (EvolutionShellComponent *shell_component, gpointer user_data)
{
	if (mail_config_get_empty_trash_on_exit ())
		empty_trash (NULL, NULL, NULL);
	
	unref_standard_folders ();
	mail_importer_uninit ();
	
	g_idle_add_full (G_PRIORITY_LOW, idle_quit, NULL, NULL);
}

static void
debug_cb (EvolutionShellComponent *shell_component, gpointer user_data)
{
	extern gboolean camel_verbose_debug;
	
	camel_verbose_debug = 1;
}

static void
handle_external_uri_cb (EvolutionShellComponent *shell_component,
			const char *uri,
			void *data)
{
	if (strncmp (uri, "mailto:", 7) != 0) {
		/* FIXME: Exception?  The EvolutionShellComponent object should
		   give me a chance to do so, but currently it doesn't.  */
		g_warning ("Invalid URI requested to mail component -- %s", uri);
		return;
	}
		
	/* FIXME: Sigh.  This shouldn't be here.  But the code is messy, so
	   I'll just put it here anyway.  */
	send_to_url (uri);
}

static void
user_create_new_item_cb (EvolutionShellComponent *shell_component,
			 const char *id,
			 const char *parent_folder_physical_uri,
			 const char *parent_folder_type,
			 gpointer data)
{
	if (!strcmp (id, "message")) {
		send_to_url (NULL);
		return;
	} 

	g_warning ("Don't know how to create item of type \"%s\"", id);
}

static BonoboObject *
component_fn (BonoboGenericFactory *factory, void *closure)
{
	EvolutionShellComponentDndDestinationFolder *destination_interface;
	MailOfflineHandler *offline_handler;
	
	shell_component = evolution_shell_component_new (folder_types,
							 schema_types,
							 create_view,
							 create_folder,
							 remove_folder,
							 xfer_folder,
							 /*populate_folder_context_menu*/NULL,
							 get_dnd_selection,
							 NULL);
	
	destination_interface = evolution_shell_component_dnd_destination_folder_new (destination_folder_handle_motion,
										      destination_folder_handle_drop,
										      shell_component);
	
	bonobo_object_add_interface (BONOBO_OBJECT (shell_component),
				     BONOBO_OBJECT (destination_interface));
	
	evolution_mail_config_wizard_init ();

	evolution_shell_component_add_user_creatable_item (shell_component, "message", _("New Mail Message"), _("New _Mail Message"), 'm');

	gtk_signal_connect (GTK_OBJECT (shell_component), "owner_set",
			    GTK_SIGNAL_FUNC (owner_set_cb), NULL);
	gtk_signal_connect (GTK_OBJECT (shell_component), "owner_unset",
			    GTK_SIGNAL_FUNC (owner_unset_cb), NULL);
	gtk_signal_connect (GTK_OBJECT (shell_component), "debug",
			    GTK_SIGNAL_FUNC (debug_cb), NULL);
	gtk_signal_connect (GTK_OBJECT (shell_component), "destroy",
			    GTK_SIGNAL_FUNC (owner_unset_cb), NULL);
	gtk_signal_connect (GTK_OBJECT (shell_component), "handle_external_uri",
			    GTK_SIGNAL_FUNC (handle_external_uri_cb), NULL);
	gtk_signal_connect (GTK_OBJECT (shell_component), "user_create_new_item",
			    GTK_SIGNAL_FUNC (user_create_new_item_cb), NULL);
	
	offline_handler = mail_offline_handler_new ();
	bonobo_object_add_interface (BONOBO_OBJECT (shell_component), BONOBO_OBJECT (offline_handler));
	
	return BONOBO_OBJECT (shell_component);
}

void
component_factory_init (void)
{
	component_factory = bonobo_generic_factory_new (COMPONENT_FACTORY_ID,
							component_fn, NULL);

	evolution_mail_config_factory_init ();
	evolution_folder_info_factory_init ();

	if (component_factory == NULL) {
		e_notice (NULL, GNOME_MESSAGE_BOX_ERROR,
			  _("Cannot initialize Evolution's mail component."));
		exit (1);
	}
}

static int
storage_create_folder (EvolutionStorage *storage,
		       const char *path,
		       const char *type,
		       const char *description,
		       const char *parent_physical_uri,
		       gpointer user_data)
{
	CamelStore *store = user_data;
	char *prefix, *name;
	CamelURL *url;
	CamelException ex;
	CamelFolderInfo *fi;

	if (strcmp (type, "mail") != 0)
		return EVOLUTION_STORAGE_ERROR_UNSUPPORTED_TYPE;

	name = strrchr (path, '/');
	if (!name++)
		return EVOLUTION_STORAGE_ERROR_INVALID_URI;
	
	camel_exception_init (&ex);
	if (*parent_physical_uri) {
		url = camel_url_new (parent_physical_uri, NULL);
		if (!url)
			return EVOLUTION_STORAGE_ERROR_INVALID_URI;
		
		fi = camel_store_create_folder (store, url->path + 1, name, &ex);
		camel_url_free (url);
	} else
		fi = camel_store_create_folder (store, NULL, name, &ex);
	
	if (camel_exception_is_set (&ex)) {
		/* FIXME: do better than this */
		camel_exception_clear (&ex);
		return EVOLUTION_STORAGE_ERROR_INVALID_URI;
	}
	
	if (camel_store_supports_subscriptions (store))
		camel_store_subscribe_folder (store, fi->full_name, NULL);
	
	prefix = g_strndup (path, name - path - 1);
	folder_created (store, prefix, fi);
	g_free (prefix);

	camel_store_free_folder_info (store, fi);
	
	return EVOLUTION_STORAGE_OK;
}

static int
storage_remove_folder (EvolutionStorage *storage,
		       const char *path,
		       const char *physical_uri,
		       gpointer user_data)
{
	CamelStore *store = user_data;
	CamelURL *url = NULL;
	CamelFolderInfo *fi;
	CamelException ex;
	
	g_warning ("storage_remove_folder: path=\"%s\"; uri=\"%s\"", path, physical_uri);
	
	if (*physical_uri) {
		if (strncmp (physical_uri, "vtrash:", 7) == 0)
			return EVOLUTION_STORAGE_ERROR_INVALID_URI;
		
		url = camel_url_new (physical_uri, NULL);
		if (!url)
			return EVOLUTION_STORAGE_ERROR_INVALID_URI;
	} else {
		if (!*path)
			return EVOLUTION_STORAGE_ERROR_INVALID_URI;
	}
	
	camel_exception_init (&ex);
	fi = camel_store_get_folder_info (store, url ? url->path + 1 : path + 1,
					  CAMEL_STORE_FOLDER_INFO_FAST, &ex);
	if (url)
		camel_url_free (url);
	if (camel_exception_is_set (&ex))
		goto exception;
	
	camel_store_delete_folder (store, fi->full_name, &ex);
	if (camel_exception_is_set (&ex))
		goto exception;
	
	if (camel_store_supports_subscriptions (store))
		camel_store_unsubscribe_folder (store, fi->full_name, NULL);
	
	evolution_storage_removed_folder (storage, path);
	
	camel_store_free_folder_info (store, fi);
	
	return EVOLUTION_STORAGE_OK;
	
 exception:
	/* FIXME: do better than this... */
	
	if (fi)
		camel_store_free_folder_info (store, fi);
	
	return EVOLUTION_STORAGE_ERROR_INVALID_URI;
}

static void
add_storage (const char *name, const char *uri, CamelService *store,
	     GNOME_Evolution_Shell corba_shell, CamelException *ex)
{
	EvolutionStorage *storage;
	EvolutionStorageResult res;
	
	storage = evolution_storage_new (name, uri, "mailstorage");
	gtk_signal_connect (GTK_OBJECT (storage), "create_folder",
			    GTK_SIGNAL_FUNC (storage_create_folder),
			    store);
	gtk_signal_connect (GTK_OBJECT (storage), "remove_folder",
			    GTK_SIGNAL_FUNC (storage_remove_folder),
			    store);
	
	res = evolution_storage_register_on_shell (storage, corba_shell);
	
	switch (res) {
	case EVOLUTION_STORAGE_OK:
		mail_hash_storage (store, storage);
		mail_scan_subfolders (CAMEL_STORE (store), storage);
		/* falllll */
	case EVOLUTION_STORAGE_ERROR_ALREADYREGISTERED:
	case EVOLUTION_STORAGE_ERROR_EXISTS:
		return;
	default:
		camel_exception_set (ex, CAMEL_EXCEPTION_SYSTEM,
				     _("Cannot register storage with shell"));
		break;
	}
}

void
mail_load_storage_by_uri (GNOME_Evolution_Shell shell, const char *uri, const char *name)
{
	CamelException ex;
	CamelService *store;
	CamelProvider *prov;
	
	camel_exception_init (&ex);
	
	/* Load the service (don't connect!). Check its provider and
	 * see if this belongs in the shell's folder list. If so, add
	 * it.
	 */
		
	prov = camel_session_get_provider (session, uri, &ex);
	if (prov == NULL) {
		/* FIXME: real error dialog */
		g_warning ("couldn't get service %s: %s\n", uri,
			   camel_exception_get_description (&ex));
		camel_exception_clear (&ex);
		return;
	}
		
	/* FIXME: this case is ambiguous for things like the
	 * mbox provider, which can really be a spool
	 * (/var/spool/mail/user) or a storage (~/mail/, eg).
	 * That issue can't be resolved on the provider level
	 * -- it's a per-URL problem.
	 *  MPZ Added a hack to let spool protocol through temporarily ...
	 */
	if ((!(prov->flags & CAMEL_PROVIDER_IS_STORAGE) ||
	     !(prov->flags & CAMEL_PROVIDER_IS_REMOTE))
	    && !((strcmp(prov->protocol, "spool") == 0)
		 || strcmp(prov->protocol, "maildir") == 0))
		return;
		
	store = camel_session_get_service (session, uri,
					   CAMEL_PROVIDER_STORE, &ex);
	if (store == NULL) {
		/* FIXME: real error dialog */
		g_warning ("couldn't get service %s: %s\n", uri,
			   camel_exception_get_description (&ex));
		camel_exception_clear (&ex);
		return;
	}

	if (name == NULL) {
		char *service_name;

		service_name = camel_service_get_name (store, TRUE);
		add_storage (service_name, uri, store, shell, &ex);
		g_free (service_name);
	} else
		add_storage (name, uri, store, shell, &ex);

	if (camel_exception_is_set (&ex)) {
		/* FIXME: real error dialog */
		g_warning ("Cannot load storage: %s",
			   camel_exception_get_description (&ex));
		camel_exception_clear (&ex);
	}
		
	camel_object_unref (CAMEL_OBJECT (store));
}

/* FIXME: 'is_account_data' is an ugly hack, if we remove support for NNTP we can take it out -- fejj */
void
mail_load_storages (GNOME_Evolution_Shell shell, const GSList *sources, gboolean is_account_data)
{
	CamelException ex;
	const GSList *iter;
	
	camel_exception_init (&ex);
	
	/* Load each service (don't connect!). Check its provider and
	 * see if this belongs in the shell's folder list. If so, add
	 * it.
	 */
	
	for (iter = sources; iter; iter = iter->next) {
		const MailConfigAccount *account = NULL;
		const MailConfigService *service = NULL;
		char *name;
		
		if (is_account_data) {
			account = iter->data;
			service = account->source;
			name = account->name;
		} else {
			service = iter->data;
			name = NULL;
		}
		
		if (service == NULL || service->url == NULL || service->url[0] == '\0' || !service->enabled)
			continue;
		
		mail_load_storage_by_uri (shell, service->url, name);
	}
}

void
mail_hash_storage (CamelService *store, EvolutionStorage *storage)
{
	camel_object_ref (CAMEL_OBJECT (store));
	g_hash_table_insert (storages_hash, store, storage);
}

EvolutionStorage*
mail_lookup_storage (CamelStore *store)
{
	EvolutionStorage *storage;
	
	/* Because the storages_hash holds a reference to each store
	 * used as a key in it, none of them will ever be gc'ed, meaning
	 * any call to camel_session_get_{service,store} with the same
	 * URL will always return the same object. So this works.
	 */
	
	storage = g_hash_table_lookup (storages_hash, store);
	if (storage)
		bonobo_object_ref (BONOBO_OBJECT (storage));
	
	return storage;
}

void
mail_remove_storage (CamelStore *store)
{
	EvolutionStorage *storage;
	EvolutionShellClient *shell_client;
	GNOME_Evolution_Shell corba_shell;
	
	/* Because the storages_hash holds a reference to each store
	 * used as a key in it, none of them will ever be gc'ed, meaning
	 * any call to camel_session_get_{service,store} with the same
	 * URL will always return the same object. So this works.
	 */
	
	storage = g_hash_table_lookup (storages_hash, store);
	g_hash_table_remove (storages_hash, store);
	
	shell_client = evolution_shell_component_get_owner (shell_component);
	corba_shell = bonobo_object_corba_objref (BONOBO_OBJECT (shell_client));
	
	evolution_storage_deregister_on_shell (storage, corba_shell);
	
	camel_service_disconnect (CAMEL_SERVICE (store), TRUE, NULL);
	camel_object_unref (CAMEL_OBJECT (store));
}

void
mail_remove_storage_by_uri (const char *uri)
{
	CamelProvider *prov;
	CamelException ex;
			
	camel_exception_init (&ex);
	prov = camel_session_get_provider (session, uri, &ex);
	if (prov != NULL && prov->flags & CAMEL_PROVIDER_IS_STORAGE &&
	    prov->flags & CAMEL_PROVIDER_IS_REMOTE) {
		CamelService *store;
				
		store = camel_session_get_service (session, uri,
						   CAMEL_PROVIDER_STORE, &ex);
		if (store != NULL) {
			g_warning ("removing storage: %s", uri);
			mail_remove_storage (CAMEL_STORE (store));
			camel_object_unref (CAMEL_OBJECT (store));
		}
	} else
		g_warning ("%s is not a remote storage.", uri);
	camel_exception_clear (&ex);
}

int
mail_storages_count (void)
{
	return g_hash_table_size (storages_hash);
}

void
mail_storages_foreach (GHFunc func, gpointer data)
{
	g_hash_table_foreach (storages_hash, func, data);
}
