/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * The Evolution addressbook client object.
 *
 * Author:
 *   Nat Friedman (nat@ximian.com)
 *
 * Copyright 1999, 2000, Ximian, Inc.
 */

#include <config.h>
#include <gtk/gtksignal.h>
#include <gtk/gtkmarshal.h>
#include <libgnome/gnome-defs.h>
#include <liboaf/liboaf.h>

#include "addressbook.h"
#include "e-card-cursor.h"
#include "e-book-listener.h"
#include "e-book.h"

GtkObjectClass *e_book_parent_class;

#define CARDSERVER_OAF_ID "OAFIID:GNOME_Evolution_Wombat_ServerFactory"

typedef enum {
	URINotLoaded,
	URILoading,
	URILoaded
} EBookLoadState;

struct _EBookPrivate {
	GNOME_Evolution_Addressbook_BookFactory  book_factory;
	EBookListener	      *listener;

	GNOME_Evolution_Addressbook_Book         corba_book;

	EBookLoadState         load_state;

	/*
	 * The operation queue.  New operations are appended to the
	 * end of the queue.  When responses come back from the PAS,
	 * the op structures are popped off the front of the queue.
	 */
	GList                 *pending_ops;

	guint op_tag;

	gchar *uri;
};

enum {
	OPEN_PROGRESS,
	WRITABLE_STATUS,
	LINK_STATUS,
	LAST_SIGNAL
};

static guint e_book_signals [LAST_SIGNAL];

typedef struct {
	guint     tag;
	gboolean  active;
	gpointer  cb;
	gpointer  closure;
	EBookViewListener *listener;
} EBookOp;

/*
 * Local response queue management.
 */

static void
e_book_op_free (EBookOp *op)
{
	if (op->listener) {
		bonobo_object_unref (BONOBO_OBJECT (op->listener));
		op->listener = NULL;
	}
	g_free (op);
}

static guint
e_book_queue_op (EBook    *book,
		 gpointer  cb,
		 gpointer  closure,
		 EBookViewListener *listener)
{
	EBookOp *op;

	op           = g_new0 (EBookOp, 1);
	op->tag      = book->priv->op_tag++;
	op->active   = TRUE;
	op->cb       = cb;
	op->closure  = closure;
	op->listener = listener;

	if (op->listener)
		bonobo_object_ref (BONOBO_OBJECT (op->listener));

	book->priv->pending_ops =
		g_list_append (book->priv->pending_ops, op);

	return op->tag;
}

/*
 * Local response queue management.
 */
static void
e_book_unqueue_op (EBook    *book)
{
	EBookOp *op;
	GList *removed;

	removed = g_list_last (book->priv->pending_ops);

	if (removed) {
		book->priv->pending_ops = g_list_remove_link (book->priv->pending_ops,
							      removed);
		op = removed->data;
		e_book_op_free (op);
		g_list_free_1 (removed);
		book->priv->op_tag--;
	}
}

static EBookOp *
e_book_pop_op (EBook *book)
{
	GList   *popped;
	EBookOp *op;

	if (book->priv->pending_ops == NULL)
		return NULL;

	op = book->priv->pending_ops->data;

	popped = book->priv->pending_ops;
	book->priv->pending_ops =
		g_list_remove_link (book->priv->pending_ops,
				    book->priv->pending_ops);

	g_list_free_1 (popped);

	return op;
}

static gboolean
e_book_cancel_op (EBook *book, guint tag)
{
	GList *iter;
	gboolean cancelled = FALSE;

	for (iter = book->priv->pending_ops; iter != NULL && !cancelled; iter = g_list_next (iter)) {
		EBookOp *op = iter->data;
		if (op->tag == tag) {
			op->active = FALSE;
			cancelled = TRUE;
		}
	}
	
	return cancelled;
}

static void
e_book_do_response_create_card (EBook                 *book,
				EBookListenerResponse *resp)
{
	EBookOp *op;

	op = e_book_pop_op (book);

	if (op == NULL) {
		g_warning ("e_book_do_response_create_card: Cannot find operation "
			   "in local op queue!\n");
		return;
	}

	if (op->cb)
		((EBookIdCallback) op->cb) (book, resp->status, resp->id, op->closure);
	g_free (resp->id);
	e_book_op_free (op);
}

static void
e_book_do_response_generic (EBook                 *book,
			    EBookListenerResponse *resp)
{
	EBookOp *op;

	op = e_book_pop_op (book);

	if (op == NULL) {
		g_warning ("e_book_do_response_generic: Cannot find operation "
			   "in local op queue!\n");
	}

	if (op->cb)
		((EBookCallback) op->cb) (book, resp->status, op->closure);

	e_book_op_free (op);
}

static void
e_book_do_response_get_vcard (EBook                 *book,
			      EBookListenerResponse *resp)
{
	EBookOp *op;
	ECard *card;

	op = e_book_pop_op (book);

	if (op == NULL) {
		g_warning ("e_book_do_response_get_vcard: Cannot find operation "
			   "in local op queue!\n");
		return;
	}
	if (resp->vcard != NULL) {

		card = e_card_new(resp->vcard);

		if (card != NULL) {
			e_card_set_book (card, book);
			if (op->cb) {
				if (op->active)
					((EBookCardCallback) op->cb) (book, resp->status, card, op->closure);
				else
					((EBookCardCallback) op->cb) (book, E_BOOK_STATUS_CANCELLED, NULL, op->closure);
			}

			gtk_object_unref(GTK_OBJECT(card));
		} else {
			((EBookCursorCallback) op->cb) (book, resp->status, NULL, op->closure);
		}
	} else {
		((EBookCardCallback) op->cb) (book, resp->status, NULL, op->closure);
	}

	g_free (resp->vcard);
	e_book_op_free (op);
}

static void
e_book_do_response_get_cursor (EBook                 *book,
			       EBookListenerResponse *resp)
{
	CORBA_Environment ev;
	EBookOp *op;
	ECardCursor *cursor;

	op = e_book_pop_op (book);

	if (op == NULL) {
		g_warning ("e_book_do_response_get_cursor: Cannot find operation "
			   "in local op queue!\n");
		return;
	}

	cursor = e_card_cursor_new(resp->cursor);

	if (cursor != NULL) {
		if (op->cb) {
			if (op->active)
				((EBookCursorCallback) op->cb) (book, resp->status, cursor, op->closure);
			else
				((EBookCursorCallback) op->cb) (book, E_BOOK_STATUS_CANCELLED, NULL, op->closure);
		}

		/*
		 * Release the remote GNOME_Evolution_Addressbook_Book in the PAS.
		 */
		CORBA_exception_init (&ev);

		bonobo_object_release_unref (resp->cursor, &ev);

		if (ev._major != CORBA_NO_EXCEPTION) {
			g_warning ("e_book_do_response_get_cursor: Exception releasing "
				   "remote GNOME_Evolution_Addressbook_CardCursor interface!\n");
		}

		CORBA_exception_free (&ev);

		gtk_object_unref(GTK_OBJECT(cursor));
	} else {
		((EBookCursorCallback) op->cb) (book, E_BOOK_STATUS_CANCELLED, NULL, op->closure);
	}
	
	e_book_op_free (op);
}

static void
e_book_do_response_get_view (EBook                 *book,
			     EBookListenerResponse *resp)
{
	CORBA_Environment ev;
	EBookOp *op;
	EBookView *book_view;

	op = e_book_pop_op (book);

	if (op == NULL) {
		g_warning ("e_book_do_response_get_view: Cannot find operation "
			   "in local op queue!\n");
		return;
	}
	
	book_view = e_book_view_new (resp->book_view, op->listener);

	if (book_view != NULL) {
		e_book_view_set_book (book_view, book);

		/* Only execute the callback if the operation is still flagged as active (i.e. hasn't
		   been cancelled.  This is mildly wasteful since we unnecessaryily create the
		   book_view, etc... but I'm leery of tinkering with the CORBA magic. */
		if (op->cb) {
			if (op->active)
				((EBookBookViewCallback) op->cb) (book, resp->status, book_view, op->closure);
			else
				((EBookBookViewCallback) op->cb) (book, E_BOOK_STATUS_CANCELLED, NULL, op->closure);
		}
	
		/*
		 * Release the remote GNOME_Evolution_Addressbook_Book in the PAS.
		 */
		CORBA_exception_init (&ev);

		bonobo_object_release_unref  (resp->book_view, &ev);

		if (ev._major != CORBA_NO_EXCEPTION) {
			g_warning ("e_book_do_response_get_view: Exception releasing "
				   "remote GNOME_Evolution_Addressbook_BookView interface!\n");
		}

		CORBA_exception_free (&ev);

		gtk_object_unref(GTK_OBJECT(book_view));
	} else {
		e_book_view_listener_stop (op->listener);
		((EBookBookViewCallback) op->cb) (book, E_BOOK_STATUS_CANCELLED, NULL, op->closure);
	}

	e_book_op_free (op);
}

static void
e_book_do_response_get_changes (EBook                 *book,
				EBookListenerResponse *resp)
{
	CORBA_Environment ev;
	EBookOp *op;
	EBookView *book_view;

	op = e_book_pop_op (book);

	if (op == NULL) {
		g_warning ("e_book_do_response_get_changes: Cannot find operation "
			   "in local op queue!\n");
		return;
	}

	book_view = e_book_view_new (resp->book_view, op->listener);

	if (book_view != NULL) {
		e_book_view_set_book (book_view, book);
	
		if (op->cb) {
			if (op->active)
				((EBookBookViewCallback) op->cb) (book, resp->status, book_view, op->closure);
			else
				((EBookBookViewCallback) op->cb) (book, E_BOOK_STATUS_CANCELLED, NULL, op->closure);
		}

		/*
		 * Release the remote GNOME_Evolution_Addressbook_BookView in the PAS.
		 */
		CORBA_exception_init (&ev);

		bonobo_object_release_unref  (resp->book_view, &ev);

		if (ev._major != CORBA_NO_EXCEPTION) {
			g_warning ("e_book_do_response_get_changes: Exception releasing "
				   "remote GNOME_Evolution_Addressbook_BookView interface!\n");
		}

		CORBA_exception_free (&ev);

		gtk_object_unref(GTK_OBJECT(book_view));
	} else {
		e_book_view_listener_stop (op->listener);
		((EBookBookViewCallback) op->cb) (book, E_BOOK_STATUS_CANCELLED, NULL, op->closure);
	}

	e_book_op_free (op);
}

static void
e_book_do_response_open (EBook                 *book,
			 EBookListenerResponse *resp)
{
	EBookOp *op;

	if (resp->status == E_BOOK_STATUS_SUCCESS) {
		book->priv->corba_book  = resp->book;
		book->priv->load_state  = URILoaded;
	}

	op = e_book_pop_op (book);

	if (op == NULL) {
		g_warning ("e_book_do_response_open: Cannot find operation "
			   "in local op queue!\n");
		return;
	}

	if (op->cb)
		((EBookCallback) op->cb) (book, resp->status, op->closure);
	e_book_op_free (op);
}

static void
e_book_do_progress_event (EBook                 *book,
			  EBookListenerResponse *resp)
{
	gtk_signal_emit (GTK_OBJECT (book), e_book_signals [OPEN_PROGRESS],
			 resp->msg, resp->percent);

	g_free (resp->msg);
}

static void
e_book_do_link_event (EBook                 *book,
		      EBookListenerResponse *resp)
{
	gtk_signal_emit (GTK_OBJECT (book), e_book_signals [LINK_STATUS],
			 resp->connected);
}

static void
e_book_do_writable_event (EBook                 *book,
			  EBookListenerResponse *resp)
{
	gtk_signal_emit (GTK_OBJECT (book), e_book_signals [WRITABLE_STATUS],
			 resp->writable);
}

static void
e_book_do_response_get_supported_fields (EBook                 *book,
					 EBookListenerResponse *resp)
{
	EBookOp *op;

	op = e_book_pop_op (book);

	if (op == NULL) {
		g_warning ("e_book_do_response_get_supported_fields: Cannot find operation "
			   "in local op queue!\n");
		return;
	}

	if (op->cb) {
		if (op->active)
			((EBookFieldsCallback) op->cb) (book, resp->status, resp->fields, op->closure);
		else
			((EBookFieldsCallback) op->cb) (book, E_BOOK_STATUS_CANCELLED, NULL, op->closure);
	}

	e_book_op_free (op);
}

/*
 * Reading notices out of the EBookListener's queue.
 */
static void
e_book_check_listener_queue (EBookListener *listener, EBook *book)
{
	EBookListenerResponse *resp;

	resp = e_book_listener_pop_response (listener);

	if (resp == NULL)
		return;

	switch (resp->op) {
	case CreateCardResponse:
		e_book_do_response_create_card (book, resp);
		break;
	case RemoveCardResponse:
	case ModifyCardResponse:
	case AuthenticationResponse:
		e_book_do_response_generic (book, resp);
		break;
	case GetCardResponse:
		e_book_do_response_get_vcard (book, resp);
		break;
	case GetCursorResponse:
		e_book_do_response_get_cursor (book, resp);
		break;
	case GetBookViewResponse:
		e_book_do_response_get_view(book, resp);
		break;
	case GetChangesResponse:
		e_book_do_response_get_changes(book, resp);
		break;
	case OpenBookResponse:
		e_book_do_response_open (book, resp);
		break;
	case GetSupportedFieldsResponse:
		e_book_do_response_get_supported_fields (book, resp);
		break;

	case OpenProgressEvent:
		e_book_do_progress_event (book, resp);
		break;
	case LinkStatusEvent:
		e_book_do_link_event (book, resp);
		break;
	case WritableStatusEvent:
		e_book_do_writable_event (book, resp);
		break;
	default:
		g_error ("EBook: Unknown operation %d in listener queue!\n",
			 resp->op);
	}

	g_free (resp);
}

/**
 * e_book_load_uri:
 */

gboolean
e_book_load_uri (EBook                     *book,
		 const char                *uri,
		 EBookCallback              open_response,
		 gpointer                   closure)
{
	CORBA_Environment ev;

	g_return_val_if_fail (book != NULL,          FALSE);
	g_return_val_if_fail (E_IS_BOOK (book),      FALSE);
	g_return_val_if_fail (uri != NULL,           FALSE);
	g_return_val_if_fail (open_response != NULL, FALSE);

	if (book->priv->load_state != URINotLoaded) {
		g_warning ("e_book_load_uri: Attempted to load a URI "
			   "on a book which already has a URI loaded!\n");
		return FALSE;
	}

	g_free (book->priv->uri);
	book->priv->uri = g_strdup (uri);

	/*
	 * Create our local BookListener interface.
	 */
	book->priv->listener = e_book_listener_new ();
	if (book->priv->listener == NULL) {
		g_warning ("e_book_load_uri: Could not create EBookListener!\n");
		return FALSE;
	}

	gtk_signal_connect (GTK_OBJECT (book->priv->listener), "responses_queued",
			    e_book_check_listener_queue, book);
	
	/*
	 * Load the addressbook into the PAS.
	 */
	CORBA_exception_init (&ev);

	e_book_queue_op (book, open_response, closure, NULL);

	GNOME_Evolution_Addressbook_BookFactory_openBook (
		book->priv->book_factory, uri,
		bonobo_object_corba_objref (BONOBO_OBJECT (book->priv->listener)),
		&ev);

	if (ev._major != CORBA_NO_EXCEPTION) {
		g_warning ("e_book_load_uri: CORBA exception while opening addressbook!\n");
		CORBA_exception_free (&ev);
		e_book_unqueue_op (book);
		return FALSE;
	}

	CORBA_exception_free (&ev);

	book->priv->load_state = URILoading;

	/* Now we play the waiting game. */

	return TRUE;
}

/**
 * e_book_unload_uri:
 */
void
e_book_unload_uri (EBook *book)
{
	CORBA_Environment ev;

	g_return_if_fail (book != NULL);
	g_return_if_fail (E_IS_BOOK (book));

	/*
	 * FIXME: Make sure this works if the URI is still being
	 * loaded.
	 */
	if (book->priv->load_state != URILoaded) {
		g_warning ("e_book_unload_uri: No URI is loaded!\n");
		return;
	}

	/*
	 * Release the remote GNOME_Evolution_Addressbook_Book in the PAS.
	 */
	CORBA_exception_init (&ev);

	bonobo_object_release_unref  (book->priv->corba_book, &ev);

	if (ev._major != CORBA_NO_EXCEPTION) {
		g_warning ("e_book_unload_uri: Exception releasing "
			   "remote book interface!\n");
	}

	CORBA_exception_free (&ev);

	e_book_listener_stop (book->priv->listener);
	bonobo_object_unref (BONOBO_OBJECT (book->priv->listener));

	book->priv->listener   = NULL;
	book->priv->load_state = URINotLoaded;
}

const char *
e_book_get_uri (EBook *book)
{
	g_return_val_if_fail (book && E_IS_BOOK (book), NULL);

	return book->priv->uri;
}

char *
e_book_get_static_capabilities (EBook *book)
{
	CORBA_Environment ev;
	char *temp;
	char *ret_val;

	CORBA_exception_init (&ev);

	if (book->priv->load_state != URILoaded) {
		g_warning ("e_book_unload_uri: No URI is loaded!\n");
		return g_strdup("");
	}

	temp = GNOME_Evolution_Addressbook_Book_getStaticCapabilities(book->priv->corba_book, &ev);

	if (ev._major != CORBA_NO_EXCEPTION) {
		g_warning ("e_book_get_static_capabilities: Exception "
			   "during get_static_capabilities!\n");
		CORBA_exception_free (&ev);
		return NULL;
	}

	ret_val = g_strdup(temp);
	CORBA_free(temp);

	CORBA_exception_free (&ev);

	return ret_val;
}

guint
e_book_get_supported_fields (EBook              *book,
			     EBookFieldsCallback cb,
			     gpointer            closure)
{
	CORBA_Environment ev;
	guint tag;

	CORBA_exception_init (&ev);

	if (book->priv->load_state != URILoaded) {
		g_warning ("e_book_unload_uri: No URI is loaded!\n");
		return 0;
	}

	tag = e_book_queue_op (book, cb, closure, NULL);

	GNOME_Evolution_Addressbook_Book_getSupportedFields(book->priv->corba_book, &ev);

	if (ev._major != CORBA_NO_EXCEPTION) {
		g_warning ("e_book_get_supported_fields: Exception "
			   "during get_supported_fields!\n");
		CORBA_exception_free (&ev);
		e_book_unqueue_op (book);
		return 0;
	}

	CORBA_exception_free (&ev);

	return tag;
}

static gboolean
e_book_construct (EBook *book)
{
	g_return_val_if_fail (book != NULL,     FALSE);
	g_return_val_if_fail (E_IS_BOOK (book), FALSE);

	/*
	 * Connect to the Personal Addressbook Server.
	 */

	book->priv->book_factory = (GNOME_Evolution_Addressbook_BookFactory)
		oaf_activate_from_id (CARDSERVER_OAF_ID, 0, NULL, NULL);
	if (book->priv->book_factory == CORBA_OBJECT_NIL) {
		g_warning ("e_book_construct: Could not obtain a handle "
			   "to the Personal Addressbook Server!\n");
		return FALSE;
	}

	return TRUE;
}

/**
 * e_book_new:
 */
EBook *
e_book_new (void)
{
	EBook *book;

	book = gtk_type_new (E_BOOK_TYPE);

	if (! e_book_construct (book)) {
		gtk_object_unref (GTK_OBJECT (book));
		return NULL;
	}

	return book;
}

/* User authentication. */

void
e_book_authenticate_user (EBook         *book,
			  const char    *user,
			  const char    *passwd,
			  EBookCallback cb,
			  gpointer      closure)
{
	CORBA_Environment  ev;

	g_return_if_fail (book != NULL);
	g_return_if_fail (E_IS_BOOK (book));

	if (book->priv->load_state != URILoaded) {
		g_warning ("e_book_authenticate_user: No URI loaded!\n");
		return;
	}

	CORBA_exception_init (&ev);

	e_book_queue_op (book, cb, closure, NULL);

	GNOME_Evolution_Addressbook_Book_authenticateUser (book->priv->corba_book,
							   user,
							   passwd,
							   &ev);

	if (ev._major != CORBA_NO_EXCEPTION) {
		g_warning ("e_book_authenticate_user: Exception authenticating user with the PAS!\n");
		CORBA_exception_free (&ev);
		e_book_unqueue_op (book);
		return;
	}

	CORBA_exception_free (&ev);
}

/* Fetching cards */

/**
 * e_book_get_card:
 */
guint
e_book_get_card (EBook             *book,
		 const char        *id,
		 EBookCardCallback  cb,
		 gpointer           closure)
{
	CORBA_Environment ev;
	guint tag;

	g_return_val_if_fail (book != NULL,     0);
	g_return_val_if_fail (E_IS_BOOK (book), 0);

	if (book->priv->load_state != URILoaded) {
		g_warning ("e_book_get_card: No URI loaded!\n");
		return 0;
	}

	CORBA_exception_init (&ev);

	tag = e_book_queue_op (book, cb, closure, NULL);

	GNOME_Evolution_Addressbook_Book_getVCard (book->priv->corba_book, (const GNOME_Evolution_Addressbook_VCard) id, &ev);

	if (ev._major != CORBA_NO_EXCEPTION) {
		g_warning ("e_book_get_card: Exception "
			   "getting card!\n");
		CORBA_exception_free (&ev);
		e_book_unqueue_op (book);
		return 0;
	}

	CORBA_exception_free (&ev);

	return tag;
}

/* Deleting cards. */

/**
 * e_book_remove_card:
 */
gboolean
e_book_remove_card (EBook         *book,
		    ECard         *card,
		    EBookCallback  cb,
		    gpointer       closure)
{
	const char *id;

	g_return_val_if_fail (book != NULL,     FALSE);
	g_return_val_if_fail (E_IS_BOOK (book), FALSE);
	g_return_val_if_fail (card != NULL,     FALSE);
	g_return_val_if_fail (E_IS_CARD (card), FALSE);

	if (book->priv->load_state != URILoaded) {
		g_warning ("e_book_remove_card: No URI loaded!\n");
		return FALSE;
	}

	id = e_card_get_id (card);
	g_assert (id != NULL);

	return e_book_remove_card_by_id (book, id, cb, closure);
}

/**
 * e_book_remove_card_by_id:
 */
gboolean
e_book_remove_card_by_id (EBook         *book,
			  const char    *id,
			  EBookCallback  cb,
			  gpointer       closure)

{
	CORBA_Environment ev;

	g_return_val_if_fail (book != NULL,     FALSE);
	g_return_val_if_fail (E_IS_BOOK (book), FALSE);
	g_return_val_if_fail (id != NULL,       FALSE);

	if (book->priv->load_state != URILoaded) {
		g_warning ("e_book_remove_card_by_id: No URI loaded!\n");
		return FALSE;
	}

	CORBA_exception_init (&ev);

	e_book_queue_op (book, cb, closure, NULL);

	GNOME_Evolution_Addressbook_Book_removeCard (
		book->priv->corba_book, (const GNOME_Evolution_Addressbook_CardId) id, &ev);

	if (ev._major != CORBA_NO_EXCEPTION) {
		g_warning ("e_book_remove_card_by_id: CORBA exception "
			   "talking to PAS!\n");
		CORBA_exception_free (&ev);
		e_book_unqueue_op (book);
		return FALSE;
	}
	
	CORBA_exception_free (&ev);

	return TRUE;
}

/* Adding cards. */

/**
 * e_book_add_card:
 */
gboolean
e_book_add_card (EBook           *book,
		 ECard           *card,
		 EBookIdCallback  cb,
		 gpointer         closure)

{
	char     *vcard;
	gboolean  retval;

	g_return_val_if_fail (book != NULL,     FALSE);
	g_return_val_if_fail (E_IS_BOOK (book), FALSE);
	g_return_val_if_fail (card != NULL,     FALSE);
	g_return_val_if_fail (E_IS_CARD (card), FALSE);

	if (book->priv->load_state != URILoaded) {
		g_warning ("e_book_add_card: No URI loaded!\n");
		return FALSE;
	}

	vcard = e_card_get_vcard_assume_utf8 (card);

	if (vcard == NULL) {
		g_warning ("e_book_add_card: Cannot convert card to VCard string!\n");
		return FALSE;
	}

	retval = e_book_add_vcard (book, vcard, cb, closure);

	g_free (vcard);

	if (card->book && card->book != book)
		gtk_object_unref (GTK_OBJECT (card->book));
	card->book = book;
	gtk_object_ref (GTK_OBJECT (card->book));

	return retval;
}

/**
 * e_book_add_vcard:
 */
gboolean
e_book_add_vcard (EBook           *book,
		  const char      *vcard,
		  EBookIdCallback  cb,
		  gpointer         closure)
{
	CORBA_Environment ev;

	g_return_val_if_fail (book  != NULL,    FALSE);
	g_return_val_if_fail (E_IS_BOOK (book), FALSE);
	g_return_val_if_fail (vcard != NULL,    FALSE);

	if (book->priv->load_state != URILoaded) {
		g_warning ("e_book_add_vcard: No URI loaded!\n");
		return FALSE;
	}

	CORBA_exception_init (&ev);

	e_book_queue_op (book, (EBookCallback) cb, closure, NULL);

	GNOME_Evolution_Addressbook_Book_addCard (
		book->priv->corba_book, (const GNOME_Evolution_Addressbook_VCard) vcard, &ev);

	if (ev._major != CORBA_NO_EXCEPTION) {
		g_warning ("e_book_add_vcard: Exception adding card to PAS!\n");
		CORBA_exception_free (&ev);
		e_book_unqueue_op (book);
		return FALSE;
	}

	CORBA_exception_free (&ev);

	return TRUE;
}

/* Modifying cards. */

/**
 * e_book_commit_card:
 */
gboolean
e_book_commit_card (EBook         *book,
		    ECard         *card,
		    EBookCallback  cb,
		    gpointer       closure)
{
	char     *vcard;
	gboolean  retval;
	
	g_return_val_if_fail (book  != NULL,    FALSE);
	g_return_val_if_fail (E_IS_BOOK (book), FALSE);
	g_return_val_if_fail (card != NULL,     FALSE);
	g_return_val_if_fail (E_IS_CARD (card), FALSE);

	if (book->priv->load_state != URILoaded) {
		g_warning ("e_book_commit_card: No URI loaded!\n");
		return FALSE;
	}

	vcard = e_card_get_vcard_assume_utf8 (card);

	if (vcard == NULL) {
		g_warning ("e_book_commit_card: Error "
			   "getting VCard for card!\n");
		return FALSE;
	}

	retval = e_book_commit_vcard (book, vcard, cb, closure);

	g_free (vcard);

	if (card->book && card->book != book)
		gtk_object_unref (GTK_OBJECT (card->book));
	card->book = book;
	gtk_object_ref (GTK_OBJECT (card->book));

	return retval;
}

/**
 * e_book_commit_vcard:
 */
gboolean
e_book_commit_vcard (EBook         *book,
		     const char    *vcard,
		     EBookCallback  cb,
		     gpointer       closure)
{
	CORBA_Environment ev;

	g_return_val_if_fail (book  != NULL,    FALSE);
	g_return_val_if_fail (E_IS_BOOK (book), FALSE);
	g_return_val_if_fail (vcard != NULL,    FALSE);

	if (book->priv->load_state != URILoaded) {
		g_warning ("e_book_commit_vcard: No URI loaded!\n");
		return FALSE;
	}

	CORBA_exception_init (&ev);

	e_book_queue_op (book, cb, closure, NULL);

	GNOME_Evolution_Addressbook_Book_modifyCard (
		book->priv->corba_book, (const GNOME_Evolution_Addressbook_VCard) vcard, &ev);

	if (ev._major != CORBA_NO_EXCEPTION) {
		g_warning ("e_book_commit_vcard: Exception "
			   "modifying card in PAS!\n");
		CORBA_exception_free (&ev);
		e_book_unqueue_op (book);
		return FALSE;
	}

	CORBA_exception_free (&ev);

	return TRUE;
}

/**
 * e_book_check_connection:
 */
gboolean
e_book_check_connection (EBook *book)
{
	CORBA_Environment ev;

	g_return_val_if_fail (book != NULL,     FALSE);
	g_return_val_if_fail (E_IS_BOOK (book), FALSE);

	if (book->priv->load_state != URILoaded) {
		g_warning ("e_book_check_connection: No URI loaded!\n");
		return FALSE;
	}

	CORBA_exception_init (&ev);

	GNOME_Evolution_Addressbook_Book_checkConnection (book->priv->corba_book, &ev);

	if (ev._major != CORBA_NO_EXCEPTION) {
		g_warning ("e_book_check_connection: Exception "
			   "querying the PAS!\n");
		CORBA_exception_free (&ev);
		return FALSE;
	}
	
	CORBA_exception_free (&ev);

	return TRUE;
}

guint
e_book_get_cursor       (EBook               *book,
			 gchar               *query,
			 EBookCursorCallback  cb,
			 gpointer             closure)
{
	CORBA_Environment ev;
	guint tag;
  
	g_return_val_if_fail (book != NULL,     0);
	g_return_val_if_fail (E_IS_BOOK (book), 0);

	if (book->priv->load_state != URILoaded) {
		g_warning ("e_book_check_connection: No URI loaded!\n");
		return 0;
	}
	
	CORBA_exception_init (&ev);

	tag = e_book_queue_op (book, cb, closure, NULL);
	
	GNOME_Evolution_Addressbook_Book_getCursor (book->priv->corba_book, query, &ev);

	if (ev._major != CORBA_NO_EXCEPTION) {
		g_warning ("e_book_get_all_cards: Exception "
			   "querying list of cards!\n");
		CORBA_exception_free (&ev);
		e_book_unqueue_op (book);
		return 0;
	}
	
	CORBA_exception_free (&ev);

	return tag;
}

guint
e_book_get_book_view       (EBook                 *book,
			    const gchar           *query,
			    EBookBookViewCallback  cb,
			    gpointer               closure)
{
	CORBA_Environment ev;
	EBookViewListener *listener;
	guint tag;
  
	g_return_val_if_fail (book != NULL,     0);
	g_return_val_if_fail (E_IS_BOOK (book), 0);

	if (book->priv->load_state != URILoaded) {
		g_warning ("e_book_get_book_view: No URI loaded!\n");
		return 0;
	}

	listener = e_book_view_listener_new();
	
	CORBA_exception_init (&ev);

	tag = e_book_queue_op (book, cb, closure, listener);
	
	GNOME_Evolution_Addressbook_Book_getBookView (book->priv->corba_book, bonobo_object_corba_objref(BONOBO_OBJECT(listener)), query, &ev);

	if (ev._major != CORBA_NO_EXCEPTION) {
		g_warning ("e_book_get_book_view: Exception "
			   "getting book_view!\n");
		CORBA_exception_free (&ev);
		e_book_unqueue_op (book);
		return 0;
	}
	
	CORBA_exception_free (&ev);

	return tag;
}

guint
e_book_get_changes         (EBook                 *book,
			    gchar                 *changeid,
			    EBookBookViewCallback  cb,
			    gpointer               closure)
{
	CORBA_Environment ev;
	EBookViewListener *listener;
	guint tag;
  
	g_return_val_if_fail (book != NULL,     0);
	g_return_val_if_fail (E_IS_BOOK (book), 0);

	if (book->priv->load_state != URILoaded) {
		g_warning ("e_book_get_changes: No URI loaded!\n");
		return FALSE;
	}

	listener = e_book_view_listener_new();
	
	CORBA_exception_init (&ev);

	tag = e_book_queue_op (book, cb, closure, listener);
	
	GNOME_Evolution_Addressbook_Book_getChanges (book->priv->corba_book, bonobo_object_corba_objref(BONOBO_OBJECT(listener)), changeid, &ev);

	if (ev._major != CORBA_NO_EXCEPTION) {
		g_warning ("e_book_changes: Exception "
			   "getting changes!\n");
		CORBA_exception_free (&ev);
		e_book_unqueue_op (book);
		return 0;
	}
	
	CORBA_exception_free (&ev);

	return tag;
}

/**
 * e_book_cancel
 */

void
e_book_cancel (EBook *book, guint tag)
{
	g_return_if_fail (book != NULL);
	g_return_if_fail (E_IS_BOOK (book));
	g_return_if_fail (tag != 0);

	/* In an attempt to be useful, we take a bit of extra care in reporting
	   errors.  This might come in handy someday. */
	if (tag >= book->priv->op_tag)
		g_warning ("Attempt to cancel unassigned operation (%u)", tag);
	else if (! e_book_cancel_op (book, tag))
		g_warning ("Attempt to cancel unknown operation (%u)", tag); 
}

/**
 * e_book_get_name:
 */
char *
e_book_get_name (EBook *book)
{
	CORBA_Environment  ev;
	char              *retval;
	char              *name;

	g_return_val_if_fail (book != NULL,     NULL);
	g_return_val_if_fail (E_IS_BOOK (book), NULL);

	if (book->priv->load_state != URILoaded) {
		g_warning ("e_book_get_name: No URI loaded!\n");
		return NULL;
	}

	CORBA_exception_init (&ev);

	name = GNOME_Evolution_Addressbook_Book_getName (book->priv->corba_book, &ev);

	if (ev._major != CORBA_NO_EXCEPTION) {
		g_warning ("e_book_get_name: Exception getting name from PAS!\n");
		CORBA_exception_free (&ev);
		return NULL;
	}

	CORBA_exception_free (&ev);

	if (name == NULL) {
		g_warning ("e_book_get_name: Got NULL name from PAS!\n");
		return NULL;
	}

	retval = g_strdup (name);
	CORBA_free (name);

	return retval;
}

static void
e_book_init (EBook *book)
{
	book->priv             = g_new0 (EBookPrivate, 1);
	book->priv->load_state = URINotLoaded;
	book->priv->op_tag     = 1;
	book->priv->uri        = NULL;
}

static void
e_book_destroy (GtkObject *object)
{
	EBook             *book = E_BOOK (object);
	CORBA_Environment  ev;

	if (book->priv->load_state == URILoaded)
		e_book_unload_uri (book);

	CORBA_exception_init (&ev);

	CORBA_Object_release (book->priv->book_factory, &ev);
	if (ev._major != CORBA_NO_EXCEPTION) {
		g_warning ("EBook: Exception while releasing BookFactory\n");

		CORBA_exception_free (&ev);
		CORBA_exception_init (&ev);
	}

	g_free (book->priv->uri);

	g_free (book->priv);

	GTK_OBJECT_CLASS (e_book_parent_class)->destroy (object);
}

static void
e_book_class_init (EBookClass *klass)
{
	GtkObjectClass *object_class = (GtkObjectClass *) klass;

	e_book_parent_class = gtk_type_class (gtk_object_get_type ());

	e_book_signals [LINK_STATUS] =
		gtk_signal_new ("link_status",
				GTK_RUN_LAST,
				object_class->type,
				GTK_SIGNAL_OFFSET (EBookClass, link_status),
				gtk_marshal_NONE__BOOL,
				GTK_TYPE_NONE, 1,
				GTK_TYPE_BOOL);

	e_book_signals [WRITABLE_STATUS] =
		gtk_signal_new ("writable_status",
				GTK_RUN_LAST,
				object_class->type,
				GTK_SIGNAL_OFFSET (EBookClass, writable_status),
				gtk_marshal_NONE__BOOL,
				GTK_TYPE_NONE, 1,
				GTK_TYPE_BOOL);

	gtk_object_class_add_signals (object_class, e_book_signals,
				      LAST_SIGNAL);

	object_class->destroy = e_book_destroy;
}

/**
 * e_book_get_type:
 */
GtkType
e_book_get_type (void)
{
	static GtkType type = 0;

	if (! type) {
		GtkTypeInfo info = {
			"EBook",
			sizeof (EBook),
			sizeof (EBookClass),
			(GtkClassInitFunc)  e_book_class_init,
			(GtkObjectInitFunc) e_book_init,
			NULL, /* reserved 1 */
			NULL, /* reserved 2 */
			(GtkClassInitFunc) NULL
		};

		type = gtk_type_unique (gtk_object_get_type (), &info);
	}

	return type;
}
