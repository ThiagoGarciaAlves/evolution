/*
 * e-mail-account-manager.h
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with the program; if not, see <http://www.gnu.org/licenses/>
 *
 */

#ifndef E_MAIL_ACCOUNT_MANAGER_H
#define E_MAIL_ACCOUNT_MANAGER_H

#include <gtk/gtk.h>
#include <libedataserver/e-source-registry.h>

/* Standard GObject macros */
#define E_TYPE_MAIL_ACCOUNT_MANAGER \
	(e_mail_account_manager_get_type ())
#define E_MAIL_ACCOUNT_MANAGER(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), E_TYPE_MAIL_ACCOUNT_MANAGER, EMailAccountManager))
#define E_MAIL_ACCOUNT_MANAGER_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), E_TYPE_MAIL_ACCOUNT_MANAGER, EMailAccountManagerClass))
#define E_IS_MAIL_ACCOUNT_MANAGER(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), E_TYPE_MAIL_ACCOUNT_MANAGER))
#define E_IS_MAIL_ACCOUNT_MANAGER_CLASS(cls) \
	(G_TYPE_CHECK_INSTANCE_CLASS \
	((cls), E_TYPE_MAIL_ACCOUNT_MANAGER))
#define E_MAIL_ACCOUNT_MANAGER_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), E_TYPE_MAIL_ACCOUNT_MANAGER, EMailAccountManagerClass))

G_BEGIN_DECLS

typedef struct _EMailAccountManager EMailAccountManager;
typedef struct _EMailAccountManagerClass EMailAccountManagerClass;
typedef struct _EMailAccountManagerPrivate EMailAccountManagerPrivate;

struct _EMailAccountManager {
	GtkTable parent;
	EMailAccountManagerPrivate *priv;
};

struct _EMailAccountManagerClass {
	GtkTableClass parent_class;

	void		(*add_account)		(EMailAccountManager *manager);
	void		(*edit_account)		(EMailAccountManager *manager);
	void		(*delete_account)	(EMailAccountManager *manager);
};

GType		e_mail_account_manager_get_type	(void) G_GNUC_CONST;
GtkWidget *	e_mail_account_manager_new	(ESourceRegistry *registry);
void		e_mail_account_manager_add_account
						(EMailAccountManager *manager);
void		e_mail_account_manager_edit_account
						(EMailAccountManager *manager);
void		e_mail_account_manager_delete_account
						(EMailAccountManager *manager);
ESourceRegistry *
		e_mail_account_manager_get_registry
						(EMailAccountManager *manager);

G_END_DECLS

#endif /* E_MAIL_ACCOUNT_MANAGER_H */
