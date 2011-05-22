/*
 * gitbrowser - A geany plugin to work with Git repositories.
 *
 * Copyright (C) 2011 by Emil Brink <emil@obsession.se>.
 *
 * This file is part of gitbrowser.
 *
 * gitbrowser is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Foobar is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with gitbrowser.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <string.h>

#include <gdk/gdkkeysyms.h>

#include "geanyplugin.h"

#define	MNEMONIC_NAME			"gitbrowser"
#define	CFG_REPOSITORIES		"repositories"
#define	CFG_QUICK_OPEN_FILTER_MAX_TIME	"quick_open_filter_max_time"
#define	CFG_QUICK_OPEN_HIDE_SRC		"quick_open_hide_re"
#define	PATH_SEPARATOR_CHAR		':'

GeanyPlugin         *geany_plugin;
GeanyData           *geany_data;
GeanyFunctions      *geany_functions;

PLUGIN_VERSION_CHECK(147)

PLUGIN_SET_INFO("Git Browser",
		"A minimalistic browser for Git repositories.",
		"0.2",
		"Emil Brink <emil@obsession.se>")

enum
{
	CMD_REPOSITORY_ADD = 0,
	CMD_REPOSITORY_ADD_FROM_DOCUMENT,
	CMD_REPOSITORY_REMOVE,
	CMD_REPOSITORY_REMOVE_ALL,
	CMD_REPOSITORY_OPEN_QUICK,
	CMD_REPOSITORY_OPEN_QUICK_FROM_DOCUMENT,
	CMD_REPOSITORY_MOVE_UP,
	CMD_REPOSITORY_MOVE_DOWN,

	CMD_DIR_EXPAND,
	CMD_DIR_COLLAPSE,

	CMD_FILE_OPEN,

	CMD_PREFERENCES,

	NUM_COMMANDS
};

enum {
	KEY_REPOSITORY_OPEN_QUICK_FROM_DOCUMENT,
	NUM_KEYS
};

typedef struct {
	gpointer		name;
	gpointer		path;
} QuickOpenPair;

typedef struct
{
	GtkWidget		*dialog;
	GtkWidget		*view;
	GtkTreeSelection	*selection;
	GtkListStore		*store;			/* Only pointers in here. */
	GString			*names;			/* All the names! */
	GArray			*array;			/* Used during construction. */
	gsize			array_size;
	GtkTreeModel		*filter;
	gchar			filter_text[128];	/* Cached so we don't need to query GtkEntry on each filter callback. */
	guint			filter_idle;
	GtkTreeIter		filter_iter;		/* For idle. */
} QuickOpenInfo;

typedef struct
{
	gchar		root_path[1024];		/* Root path, this is where the ".git/" subdirectory is. */
	QuickOpenInfo	quick_open;			/* State tracking for the "Quick Open" command's dialog. */
} Repository;

static struct
{
	GtkTreeModel	*model;
	GtkWidget	*view;
	GtkAction	*actions[NUM_COMMANDS];
	GtkWidget	*action_menu_items[NUM_COMMANDS];
	GtkWidget	*main_menu;
	GtkTreePath	*click_path;
	GRegex		*quick_open_hide;

	GHashTable	*repositories;			/* Hashed on root path. */

	GeanyKeyGroup	*key_group;

	gchar		*config_filename;
	StashGroup	*prefs;
	GtkWidget	*prefs_dlg;

	gchar		*quick_open_hide_src;
	gint		quick_open_filter_max_time;	/* In milliseconds. */
} gitbrowser;

/* -------------------------------------------------------------------------------------------------------------- */

Repository *	repository_new(const gchar *root_path);
Repository *	repository_find_by_path(const gchar *path);
void		repository_open_quick(Repository *repo);

static void	open_quick_reset_filter(void);

gboolean	tree_model_find_repository(GtkTreeModel *model, const gchar *root_path, GtkTreeIter *iter);
void		tree_model_build_repository(GtkTreeModel *model, GtkTreeIter *root, const gchar *root_path);
gboolean	tree_model_open_document(GtkTreeModel *model, GtkTreePath *path);
gboolean	tree_model_get_document_path(GtkTreeModel *model, const GtkTreeIter *iter, gchar *buf, gsize buf_max);
void		tree_model_foreach(GtkTreeModel *model, GtkTreeIter *root, void (*node_callback)(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer user), gpointer user);

/* -------------------------------------------------------------------------------------------------------------- */

/* Trickery to make the same function work both as the signal-handler, and for creating the actual GtkAction. Too weird? */
#define	CMD_INIT(n, l, tt, s)	if(action == NULL) { GtkAction **me = (GtkAction **) user; *me = gtk_action_new(n, _(l), _(tt), s); return; }

static void cmd_repository_add(GtkAction *action, gpointer user)
{
	static GtkWidget	*dialog = NULL;
	gint			response;

	CMD_INIT("repository-add", _("Add..."), _("Add a new repository based on a filesystem location."), GTK_STOCK_ADD)
	if(dialog == NULL)
	{
		dialog = gtk_file_chooser_dialog_new(_("Add Repository"), NULL, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, GTK_STOCK_OK, GTK_RESPONSE_OK, GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, NULL);
	}
	response = gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_hide(dialog);
	if(response == GTK_RESPONSE_OK)
	{
		gchar	*path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));

		if(path != NULL)
		{
			gchar	*git;

			/* Not already loaded? */
			if(repository_find_by_path(path) == NULL)
			{
				/* Does it even have a ".git" directory in it? */
				git = g_build_filename(path, ".git", NULL);
				if(g_file_test(git, G_FILE_TEST_IS_DIR))
				{
					Repository	*repo = repository_new(path);

					tree_model_build_repository(gitbrowser.model, NULL, repo->root_path);
				}
				g_free(git);
			}
			g_free(path);
		}
	}
}

static void cmd_repository_add_from_document(GtkAction *action, gpointer user)
{
	GeanyDocument	*doc;

	CMD_INIT("repository-add-from-document", _("Add from Document"), _("Add a new repository from the current document's location."), GTK_STOCK_ADD)

	if((doc = document_get_current()) != NULL)
	{
		GString	*tmp = g_string_new(doc->real_path);

		/* Step up through the directory hierarchy, looking for a ".git" directory that marks the repo's root. */
		while(TRUE)
		{
			gchar	*slash, *git, *name;

			if((slash = strrchr(tmp->str, G_DIR_SEPARATOR)) != NULL)
			{
				*slash = '\0';	/* Stamp out the slash, truncating the path. */
				git = g_build_filename(tmp->str, ".git", NULL);
				if(g_file_test(git, G_FILE_TEST_IS_DIR))
				{
					name = strrchr(tmp->str, G_DIR_SEPARATOR);
					if(name != NULL)
					{
						GtkTreeIter	iter;

						if(!tree_model_find_repository(gitbrowser.model, tmp->str, &iter))
						{
							Repository	*repo = repository_new(tmp->str);

							tree_model_build_repository(gitbrowser.model, NULL, repo->root_path);
						}
						tmp->str[0] = '\0';
					}
				}
				g_free(git);
			}
			else
				break;
		}
	}
}

static void cmd_repository_remove(GtkAction *action, gpointer user)
{
	GtkTreeIter	iter;

	CMD_INIT("repository-remove", _("Remove"), _("Removes this repository from the tree view, forgetting all about it."), GTK_STOCK_DELETE);

	if(gtk_tree_model_get_iter(gitbrowser.model, &iter, gitbrowser.click_path))
	{
		gtk_tree_store_remove(GTK_TREE_STORE(gitbrowser.model), &iter);
	}
}

static void cmd_repository_remove_all(GtkAction *action, gpointer user)
{
	GtkTreeIter	iter, child;

	CMD_INIT("repository-remove-all", _("Remove All"), _("Removes all known repositories from the plugin's browser tree."), GTK_STOCK_CLEAR);

	if(gtk_tree_model_get_iter_first(gitbrowser.model, &iter))
	{
		while(gtk_tree_model_iter_children(gitbrowser.model, &child, &iter))
		{
			gtk_tree_store_remove(GTK_TREE_STORE(gitbrowser.model), &child);
		}
	}
}

static void cmd_repository_open_quick(GtkAction *action, gpointer user)
{
	GtkTreeIter	iter;
	Repository	*repo = NULL;

	CMD_INIT("repository-open-quick", _("Quick Open ..."), _("Opens a document anywhere in the repository, with filtering."), GTK_STOCK_FIND);

	if(gtk_tree_model_get_iter(gitbrowser.model, &iter, gitbrowser.click_path))
	{
		gchar	*path = NULL;

		gtk_tree_model_get(gitbrowser.model, &iter, 1, &path, -1);
		if(path != NULL)
		{
			repo = repository_find_by_path(path);
			g_free(path);
		}
	}
	repository_open_quick(repo);
}

static void cmd_repository_open_quick_from_document(GtkAction *action, gpointer user)
{
	GeanyDocument	*doc = document_get_current();
	Repository	*repo;

	CMD_INIT("repository-open-quick-from-document", _("Quick Open from Document ..."), _("Opens the Quick Open dialog for the current docuḿent's repository"), GTK_STOCK_FIND);

	if(doc == NULL)
		return;
	repo = repository_find_by_path(doc->real_path);
	repository_open_quick(repo);
}

static void cmd_repository_move_up(GtkAction *action, gpointer user)
{
	GtkTreeIter	here;

	CMD_INIT("repository-move-up", _("Move Up"), _("Moves a repository up in the list."), GTK_STOCK_GO_UP);

	if(gtk_tree_model_get_iter(GTK_TREE_MODEL(gitbrowser.model), &here, gitbrowser.click_path))
	{
		GtkTreePath	*path_prev;
		GtkTreeIter	prev;

		path_prev = gtk_tree_path_copy(gitbrowser.click_path);
		if(gtk_tree_path_prev(path_prev))
		{
			if(gtk_tree_model_get_iter(GTK_TREE_MODEL(gitbrowser.model), &prev, path_prev))
				gtk_tree_store_move_before(GTK_TREE_STORE(gitbrowser.model), &here, &prev);
		}
		gtk_tree_path_free(path_prev);
	}
}

static void cmd_repository_move_down(GtkAction *action, gpointer user)
{
	GtkTreeIter	here;

	CMD_INIT("repository-move-down", _("Move Down"), _("Moves a repository down in the list."), GTK_STOCK_GO_DOWN);

	if(gtk_tree_model_get_iter(GTK_TREE_MODEL(gitbrowser.model), &here, gitbrowser.click_path))
	{
		GtkTreePath	*path_next;
		GtkTreeIter	next;

		path_next = gtk_tree_path_copy(gitbrowser.click_path);
		gtk_tree_path_next(path_next);
		if(gtk_tree_model_get_iter(GTK_TREE_MODEL(gitbrowser.model), &next, path_next))
			gtk_tree_store_move_after(GTK_TREE_STORE(gitbrowser.model), &here, &next);
		gtk_tree_path_free(path_next);
	}
}

static void cmd_dir_expand(GtkAction *action, gpointer user)
{
	CMD_INIT("dir-expand", _("Expand"), _("Expands a directory node."), NULL);

	gtk_tree_view_expand_row(GTK_TREE_VIEW(gitbrowser.view), gitbrowser.click_path, TRUE);
}

static void cmd_dir_collapse(GtkAction *action, gpointer user)
{
	CMD_INIT("dir-collapse", _("Collapse"), _("Collapses a directory node."), NULL);

	gtk_tree_view_collapse_row(GTK_TREE_VIEW(gitbrowser.view), gitbrowser.click_path);
}

static void cmd_file_open(GtkAction *action, gpointer user)
{
	CMD_INIT("file-open", _("Open"), _("Opens a file as a new document, or focuses the document if already opened."), GTK_STOCK_OPEN);

	tree_model_open_document(gitbrowser.model, gitbrowser.click_path);
}

static void cmd_preferences(GtkAction *action, gpointer user)
{
	static GtkWidget	*wid1 = NULL, *wid2 = NULL;

	CMD_INIT("preferences", _("Preferences ..."), _("Opens the configuration window, where you can adjust settings."), GTK_STOCK_PREFERENCES);

	if(gitbrowser.prefs_dlg == NULL)
	{
		GtkWidget	*vbox, *table, *label;

		gitbrowser.prefs_dlg  = gtk_dialog_new_with_buttons(_("Preferences"), NULL, GTK_DIALOG_MODAL, GTK_STOCK_OK, GTK_RESPONSE_OK, GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, NULL);

		vbox = ui_dialog_vbox_new(GTK_DIALOG(gitbrowser.prefs_dlg));
		table = gtk_table_new(2, 2, FALSE);
		label = gtk_label_new(_("Hide Files Matching (RE)"));
		gtk_table_attach(GTK_TABLE(table), label, 0, 1, 0, 1,  0, 0, 0, 0);
		wid1 = gtk_entry_new();
		gtk_table_attach(GTK_TABLE(table), wid1, 1, 2, 0, 1,  GTK_EXPAND | GTK_FILL, 0, 0, 0);
		ui_hookup_widget(gitbrowser.prefs_dlg, wid1, CFG_QUICK_OPEN_HIDE_SRC);
		label = gtk_label_new(_("Filter Max Update Time (ms)"));
		gtk_table_attach(GTK_TABLE(table), label, 0, 1, 1, 2,  0, 0, 0, 0);
		wid2 = gtk_spin_button_new_with_range(10, 400, 10);
		gtk_table_attach(GTK_TABLE(table), wid2, 1, 2, 1, 2,  GTK_EXPAND | GTK_FILL, 0, 0, 0);
		ui_hookup_widget(gitbrowser.prefs_dlg, wid2, CFG_QUICK_OPEN_FILTER_MAX_TIME);

		gtk_box_pack_start(GTK_BOX(vbox), table, GTK_EXPAND, GTK_EXPAND, 0);
	}
	stash_group_display(gitbrowser.prefs, wid1);
	stash_group_display(gitbrowser.prefs, wid2);
	gtk_widget_show_all(gitbrowser.prefs_dlg);
	if(gtk_dialog_run(GTK_DIALOG(gitbrowser.prefs_dlg )) == GTK_RESPONSE_OK)
	{
		open_quick_reset_filter();
		stash_group_update(gitbrowser.prefs, wid1);
		stash_group_update(gitbrowser.prefs, wid2);
	}
	gtk_widget_hide(gitbrowser.prefs_dlg );
}

void init_commands(GtkAction **actions, GtkWidget **menu_items)
{
	typedef void (*ActivateOrCreate)(GtkAction *action, gpointer user);
	const ActivateOrCreate funcs[] = {
		cmd_repository_add,
		cmd_repository_add_from_document,
		cmd_repository_remove,
		cmd_repository_remove_all,
		cmd_repository_open_quick,
		cmd_repository_open_quick_from_document,
		cmd_repository_move_up,
		cmd_repository_move_down,
		cmd_dir_expand,
		cmd_dir_collapse,
		cmd_file_open,
		cmd_preferences,
	};
	size_t	i;

	for(i = 0; i < sizeof funcs / sizeof *funcs; i++)
	{
		funcs[i](NULL, &actions[i]);
		g_signal_connect(G_OBJECT(actions[i]), "activate", G_CALLBACK(funcs[i]), NULL);
		menu_items[i] = gtk_action_create_menu_item(actions[i]);
		gtk_widget_show(menu_items[i]);
	}
}

/* -------------------------------------------------------------------------------------------------------------- */

/* Trivial convenience wrapper for g_spawn_sync(); returns command output. */
gboolean subprocess_run(const gchar* working_dir, gchar **argv, gchar **env, gchar **output, gchar **error)
{
	return g_spawn_sync(working_dir, argv, env, G_SPAWN_SEARCH_PATH, NULL, NULL, output, error, NULL, NULL);
}

/* -------------------------------------------------------------------------------------------------------------- */

/* Split the given multi-line string into individual lines, copying and returning each one.
 * Too long lines will be silently truncated, but properly skipped.
 * Returns pointer to start of next line, or NULL when no more lines are found.
*/
const gchar * tok_tokenize_next_line(const gchar *lines, gchar *buffer, size_t buf_size)
{
	if(lines == NULL || *lines == '\0' || buffer == NULL)
		return NULL;
	/* Copy characters until linefeed. Don't overflow. */
	buf_size--;
	while(*lines != '\0')
	{
		if(*lines == '\n')
		{
			*buffer = '\0';
			while(*lines == '\n')
				lines++;
			return lines;
		}
		if(buf_size > 0)
		{
			*buffer++ = *lines;
			buf_size--;
		}
		lines++;
	}
	return lines;
}

/* Scans forwards through text, looking for the next separator. Terminates (!) string and returns it, or returns NULL. */
gchar * tok_tokenize_next(gchar *text, gchar **endptr, gchar separator)
{
	gchar	*anchor;

	if(text == NULL || *text == '\0')
		return NULL;
	while(*text == separator)
		text++;
	anchor = text;
	while(*text && *text != separator)
		text++;
	if(*text == separator)
	{
		*text = '\0';
		if(endptr != NULL)
			*endptr = text + 1;
		return anchor;
	}
	else if(*text == '\0')
	{
		if(endptr != NULL)
			*endptr = NULL;
		return anchor;
	}
	return NULL;
}

/* -------------------------------------------------------------------------------------------------------------- */

Repository * repository_new(const gchar *root_path)
{
	Repository	*r = g_malloc(sizeof *r);

	g_strlcpy(r->root_path, root_path, sizeof r->root_path);

	r->quick_open.dialog = NULL;
	r->quick_open.filter = NULL;
	r->quick_open.selection = NULL;
	r->quick_open.store = NULL;
	r->quick_open.names = NULL;
	r->quick_open.view = NULL;
	r->quick_open.filter_text[0] = '\0';
	r->quick_open.filter_idle = 0;

	g_hash_table_insert(gitbrowser.repositories, r->root_path, r);

	return r;
}

/* Returns the repository to which the given path belongs, or NULL if the path is not part of a repository. */
Repository * repository_find_by_path(const gchar *path)
{
	GHashTableIter	iter;
	gpointer	key, value;

	g_hash_table_iter_init(&iter, gitbrowser.repositories);
	while(g_hash_table_iter_next(&iter, &key, &value))
	{
		Repository	*repo = value;

		if(strstr(path, repo->root_path) == path)
			return repo;
	}
	return NULL;
}

static gsize string_store(GString *string, const gchar *text)
{
	gsize	pos = string->len + 1;

	g_string_append_c(string, '\0');	/* Terminate previous name. */
	g_string_append(string, text);

	return pos;
}

static void recurse_repository_to_list(GtkTreeModel *model, GtkTreeIter *iter, gchar *path, gsize path_length, QuickOpenInfo *qoi)
{
	gchar		*dname, *fname, *get, *put;
	const gsize	old_length = path_length;
	GtkTreeIter	child;
	
	/* Loop over all nodes at this level. */
	do
	{
		gtk_tree_model_get(model, iter, 0, &dname, 1, &fname, -1);
		/* Append the local filename to the path. Ignore buffer overflow, for now. */
		for(get = fname, put = path + path_length; *get != '\0'; get++, put++, path_length++)
			*put = *get;
		*put = '\0';

		/* Do we need to recurse, here? */
		if(gtk_tree_model_iter_children(model, &child, iter))
		{
			/* Yes: time to iterate children, so append a separator. */
			strcpy(put, G_DIR_SEPARATOR_S);
			path_length += strlen(G_DIR_SEPARATOR_S);

			do
			{
				recurse_repository_to_list(model, &child, path, path_length, qoi);
			} while(gtk_tree_model_iter_next(model, &child));
		}
		else
		{
			if(gitbrowser.quick_open_hide == NULL || !g_regex_match(gitbrowser.quick_open_hide, dname, 0, NULL))
			{
				gchar		*dpath, *slash;
				QuickOpenPair	pair;

				/* Append name and path to the big string buffer, putting "naked" offsets in the pointers. */
				pair.name = GSIZE_TO_POINTER(string_store(qoi->names, dname));
				dpath = g_filename_display_name(path);
				/* Remove the last component, which is dname itself, and we don't want it in the Location column. */
				if((slash = g_utf8_strrchr(dpath, -1, G_DIR_SEPARATOR)) != NULL)
					*slash = '\0';
				pair.path = GSIZE_TO_POINTER(string_store(qoi->names, dpath));
				g_free(dpath);
				g_array_append_val(qoi->array, pair);
				qoi->array_size++;
			}
		}
		/* Undo our modifications to the global path. */
		path[old_length] = '\0';
		path_length = old_length;
	} while(gtk_tree_model_iter_next(model, iter));
}

static gint cb_array_sort(gconstpointer a, gconstpointer b)
{
	const QuickOpenPair	*pa = a, *pb = b;
	const gint		ret = g_utf8_collate(pa->path, pb->path);

	if(ret == 0)
		return g_utf8_collate(pa->name, pb->name);
	return ret;
}

static void repository_to_list(const Repository *repo, GtkTreeModel *model, QuickOpenInfo *qoi)
{
	GtkTreeIter	root, iter;
	gboolean	found = FALSE;
	gchar		buf[2048];
	gsize		len;

	if(!gtk_tree_model_get_iter_first(model, &root) || !gtk_tree_model_iter_children(model, &iter, &root))
		return;
	/* Walk the toplevel nodes, which should be very very few, looking for the given repository.
	 * This seems a bit clumsy, but it's a trade-off between i.e. maintaining an iter to the repo
	 * at all times (which is annoying when repos are added/moved/deleted) and this. This won.
	*/
	do
	{
		gchar	*path;

		gtk_tree_model_get(model, &iter, 1, &path, -1);
		found = strcmp(path, repo->root_path) == 0;
		g_free(path);
	} while(!found && gtk_tree_model_iter_next(model, &iter));
	if(!found)
		return;
	/* Now 'iter' is the root of the repository we want to linearize. */
	root = iter;
	if(!gtk_tree_model_iter_children(model, &iter, &root))
		return;
	/* Now 'iter' is finally pointing and the repository's first file. */
	len = g_snprintf(buf, sizeof buf, "%s%s", repo->root_path, G_DIR_SEPARATOR_S);
	if(len < sizeof buf)
	{
		GTimer	*tmr = g_timer_new();
		gsize	i;

		qoi->array = g_array_new(FALSE, FALSE, sizeof (QuickOpenPair));
		qoi->array_size = 0;
		recurse_repository_to_list(model, &iter, buf, len, qoi);
		/* Now we need to fixup. */
		for(i = 0; i < qoi->array_size; i++)
		{
			QuickOpenPair	*pair = &g_array_index(qoi->array, QuickOpenPair, i);

			pair->name = qoi->names->str + GPOINTER_TO_SIZE(pair->name);
			pair->path = qoi->names->str + GPOINTER_TO_SIZE(pair->path);
		}
		/* Now sort the array, hoping that's faster than sorting a tree model later on. */
		g_array_sort(qoi->array, cb_array_sort);
		/* Finally, use the array to populate the list store. */
		for(i = 0; i < qoi->array_size; i++)
		{
			const QuickOpenPair	*pair = &g_array_index(qoi->array, QuickOpenPair, i);
			gtk_list_store_insert_with_values(qoi->store, &iter, INT_MAX, 0, pair->name, 1, pair->path, 2, TRUE, -1);
		}
		/* We no longer need the sorting array, so throw it away. */
		g_array_free(qoi->array, TRUE);
		g_timer_destroy(tmr);
	}
}

void repository_save_all(GtkTreeModel *model)
{
	GtkTreeIter	root, iter;
	GString		*repos;
	GKeyFile	*out;
	gchar		*data;

	if(!gtk_tree_model_get_iter_first(model, &root) || !gtk_tree_model_iter_children(model, &iter, &root))
		return;
	repos = g_string_new("");
	do
	{
		gchar	*dpath, *fn;

		gtk_tree_model_get(model, &iter, 1, &dpath, -1);
		if((fn = g_filename_from_utf8(dpath, -1, NULL, NULL, NULL)) != NULL)
		{
			if(repos->len > 0)
				g_string_append_c(repos, PATH_SEPARATOR_CHAR);
			g_string_append(repos, fn);
			g_free(fn);
		}
	} while(gtk_tree_model_iter_next(model, &iter));

	out = g_key_file_new();
	g_key_file_set_string(out, MNEMONIC_NAME, CFG_REPOSITORIES, repos->str);
	g_string_free(repos, TRUE);
	stash_group_save_to_key_file(gitbrowser.prefs, out);

	if((data = g_key_file_to_data(out, NULL, NULL)) != NULL)
	{
		utils_write_file(gitbrowser.config_filename, data);
		g_free(data);
	}
	g_key_file_free(out);
}

void repository_load_all(void)
{
	GKeyFile	*in;

	in = g_key_file_new();
	if(g_key_file_load_from_file(in, gitbrowser.config_filename, G_KEY_FILE_NONE, NULL))
	{
		gchar	*str;

		if((str = g_key_file_get_string(in, MNEMONIC_NAME, CFG_REPOSITORIES, NULL)) != NULL)
		{
			gchar	separator[] = { PATH_SEPARATOR_CHAR, '\0' };
			gchar	**repo_vector = g_strsplit(str, separator, 0);
			gsize	i;

			for(i = 0; repo_vector[i] != NULL; i++)
			{
				Repository	*repo = repository_new(repo_vector[i]);
				tree_model_build_repository(gitbrowser.model, NULL, repo->root_path);
			}
			g_free(str);
		}
	}
	stash_group_load_from_key_file(gitbrowser.prefs, in);
	open_quick_reset_filter();

	g_key_file_free(in);
}

static void evt_open_quick_selection_changed(GtkTreeSelection *sel, gpointer user)
{
	QuickOpenInfo	*qoi = user;

	gtk_dialog_set_response_sensitive(GTK_DIALOG(qoi->dialog), GTK_RESPONSE_OK, gtk_tree_selection_count_selected_rows(sel) > 0);
}

static void evt_open_quick_view_row_activated(GtkWidget *view, GtkTreePath *path, GtkTreeViewColumn *column, gpointer user)
{
	QuickOpenInfo	*qoi = user;

	gtk_dialog_response(GTK_DIALOG(qoi->dialog), GTK_RESPONSE_OK);
}

static gboolean cb_open_quick_filter_idle(gpointer user)
{
	QuickOpenInfo	*qoi = user;
	guint		i;
	GtkTreePath	*first;
	gboolean	valid = TRUE;
	GTimer		*tmr;
	const gdouble	max_time = 1e-3 * gitbrowser.quick_open_filter_max_time;

	tmr = g_timer_new();
	for(i = 0; g_timer_elapsed(tmr, NULL) < max_time && valid; i++)
	{
		gchar		*name;
		gboolean	old_visible, new_visible;

		gtk_tree_model_get(GTK_TREE_MODEL(qoi->store), &qoi->filter_iter, 0, &name, 2, &old_visible, -1);
		new_visible = strstr(name, qoi->filter_text) != NULL;
		if(new_visible != old_visible)
			gtk_list_store_set(GTK_LIST_STORE(qoi->store), &qoi->filter_iter, 2, new_visible, -1);
		valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(qoi->store), &qoi->filter_iter);
	}
	g_timer_destroy(tmr);
	if(!valid)
	{
		/* Done! */
		first = gtk_tree_path_new_first();
		gtk_tree_view_set_cursor(GTK_TREE_VIEW(qoi->view), first, NULL, FALSE);
		gtk_tree_path_free(first);
		qoi->filter_idle = 0;
		return FALSE;
	}
	return TRUE;
}

static void evt_open_quick_entry_changed(GtkWidget *wid, gpointer user)
{
	QuickOpenInfo	*qoi = user;

	g_strlcpy(qoi->filter_text, gtk_entry_buffer_get_text(gtk_entry_get_buffer(GTK_ENTRY(wid))), sizeof qoi->filter_text);
	if(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(qoi->store), &qoi->filter_iter))
	{
		if(qoi->filter_idle == 0)
		{
			qoi->filter_idle = g_idle_add(cb_open_quick_filter_idle, qoi);
		}
	}
	gtk_entry_set_icon_sensitive(GTK_ENTRY(wid), GTK_ENTRY_ICON_SECONDARY, qoi->filter_text[0] != '\0');
}

static void evt_open_quick_entry_icon_release(GtkWidget *wid, GtkEntryIconPosition position, GdkEvent *evt, gpointer user)
{
	gtk_entry_set_text(GTK_ENTRY(wid), "");	/* There's only one icon, so no need to figure out which was clicked. */
}

static gboolean evt_open_quick_entry_key_press(GtkWidget *wid, GdkEventKey *evt, gpointer user)
{
	QuickOpenInfo	*qoi = user;

	if(evt->type == GDK_KEY_PRESS)
	{
		/* This is, unfortunately, something of a hack: to make cursor up/down keys
		 * in the entry affect the selection in the tree view, we copy the event,
		 * poke the window member, and re-emit it. The best thing is that it works.
		*/
		if(evt->keyval == GDK_KEY_Up || evt->keyval == GDK_KEY_Down ||
		   evt->keyval == GDK_KEY_Page_Up || evt->keyval == GDK_KEY_Page_Down)
		{
			GdkEventKey	copy = *evt;

			copy.window = gtk_widget_get_window(qoi->view);
			copy.send_event = TRUE;
			gtk_widget_grab_focus(qoi->view);
			gtk_main_do_event((GdkEvent *) &copy);
			gtk_widget_grab_focus(wid);

			return TRUE;
		}
	}
	return FALSE;
}

static void cdf_open_quick_filename(GtkTreeViewColumn *tree_column, GtkCellRenderer *cell, GtkTreeModel *model, GtkTreeIter *iter, gpointer user)
{
	gchar	*filename;

	gtk_tree_model_get(model, iter, 0, &filename, -1);
	g_object_set(G_OBJECT(cell), "text", filename, NULL);
}

static void cdf_open_quick_location(GtkTreeViewColumn *tree_column, GtkCellRenderer *cell, GtkTreeModel *model, GtkTreeIter *iter, gpointer user)
{
	gchar	*location;

	gtk_tree_model_get(model, iter, 1, &location, -1);
	g_object_set(G_OBJECT(cell), "text", location, NULL);
}

void repository_open_quick(Repository *repo)
{
	QuickOpenInfo	*qoi;

	if(!repo)
	{
		msgwin_status_add(_("Current document is not part of a known repository. Use Add to add a repository."));
		return;
	}
	qoi = &repo->quick_open;

	if(qoi->dialog == NULL)
	{
		GtkWidget		*vbox, *label, *scwin, *entry, *title;
		GtkCellRenderer         *cr;
		GtkTreeViewColumn       *vc;

		qoi->store = gtk_list_store_new(3, G_TYPE_POINTER, G_TYPE_POINTER, G_TYPE_BOOLEAN);
		qoi->names = g_string_sized_new(32 << 10);
		repository_to_list(repo, gitbrowser.model, qoi);

		qoi->dialog = gtk_dialog_new_with_buttons(_("Git Repository Quick Open"), NULL, GTK_DIALOG_MODAL, GTK_STOCK_OK, GTK_RESPONSE_OK, GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, NULL);
		gtk_dialog_set_default_response(GTK_DIALOG(qoi->dialog), GTK_RESPONSE_OK);
		gtk_window_set_default_size(GTK_WINDOW(qoi->dialog), 600, 600);
		vbox = ui_dialog_vbox_new(GTK_DIALOG(qoi->dialog));
		label = gtk_label_new(_("Select one or more document(s) to open. Type to filter filenames."));
		gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);
		qoi->filter = gtk_tree_model_filter_new(GTK_TREE_MODEL(qoi->store), NULL);
		gtk_tree_model_filter_set_visible_column(GTK_TREE_MODEL_FILTER(qoi->filter), 2);	/* Filter on the boolean column. */
		qoi->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(qoi->filter));

		vc = gtk_tree_view_column_new();
		cr = gtk_cell_renderer_text_new();
		title = gtk_label_new(_("Filename"));
		gtk_widget_show(title);
		gtk_tree_view_column_set_widget(vc, title);
		gtk_tree_view_column_set_sort_column_id(vc, 0);
		gtk_tree_view_append_column(GTK_TREE_VIEW(qoi->view), vc);
		gtk_tree_view_column_pack_start(vc, cr, TRUE);
		gtk_tree_view_column_set_cell_data_func(vc, cr, cdf_open_quick_filename, qoi, NULL);

		vc = gtk_tree_view_column_new();
		cr = gtk_cell_renderer_text_new();
		title = gtk_label_new(_("Location"));
		gtk_widget_show(title);
		gtk_tree_view_column_set_widget(vc, title);
		gtk_tree_view_column_set_sort_column_id(vc, 1);
		gtk_tree_view_append_column(GTK_TREE_VIEW(qoi->view), vc);
		gtk_tree_view_column_pack_start(vc, cr, TRUE);
		gtk_tree_view_column_set_cell_data_func(vc, cr, cdf_open_quick_location, qoi, NULL);
		gtk_tree_view_set_headers_clickable(GTK_TREE_VIEW(qoi->view), FALSE);

		scwin = gtk_scrolled_window_new(NULL, NULL);
		gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scwin), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
		g_signal_connect(G_OBJECT(qoi->view), "row_activated", G_CALLBACK(evt_open_quick_view_row_activated), qoi);
		gtk_container_add(GTK_CONTAINER(scwin), qoi->view);
		gtk_box_pack_start(GTK_BOX(vbox), scwin, TRUE, TRUE, 0);
		entry = gtk_entry_new();
		gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
		gtk_entry_set_icon_from_stock(GTK_ENTRY(entry), GTK_ENTRY_ICON_SECONDARY, GTK_STOCK_CLEAR);
		gtk_entry_set_icon_sensitive(GTK_ENTRY(entry), GTK_ENTRY_ICON_SECONDARY, FALSE);
		g_signal_connect(G_OBJECT(entry), "changed", G_CALLBACK(evt_open_quick_entry_changed), qoi);
		g_signal_connect(G_OBJECT(entry), "key-press-event", G_CALLBACK(evt_open_quick_entry_key_press), qoi);
		g_signal_connect(G_OBJECT(entry), "icon-release", G_CALLBACK(evt_open_quick_entry_icon_release), qoi);
		gtk_box_pack_start(GTK_BOX(vbox), entry, FALSE, FALSE, 0);

		gtk_dialog_set_response_sensitive(GTK_DIALOG(qoi->dialog), GTK_RESPONSE_OK, FALSE);

		gtk_widget_show_all(vbox);

		qoi->selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(qoi->view));
		gtk_tree_selection_set_mode(qoi->selection, GTK_SELECTION_MULTIPLE);
		g_signal_connect(G_OBJECT(qoi->selection), "changed", G_CALLBACK(evt_open_quick_selection_changed), qoi);

		gtk_widget_grab_focus(entry);
	}
	if(gtk_dialog_run(GTK_DIALOG(qoi->dialog)) == GTK_RESPONSE_OK)
	{
		GList	*selection = gtk_tree_selection_get_selected_rows(qoi->selection, NULL), *iter;

		for(iter = selection; iter != NULL; iter = g_list_next(iter))
		{
			GtkTreePath	*unfiltered;

			if((unfiltered = gtk_tree_model_filter_convert_path_to_child_path(GTK_TREE_MODEL_FILTER(qoi->filter), iter->data)) != NULL)
			{
				GtkTreeIter	here;

				if(gtk_tree_model_get_iter(GTK_TREE_MODEL(qoi->store), &here, unfiltered))
				{
					gchar	buf[2048], *dpath, *dname, *fn;
					gint	len;

					gtk_tree_model_get(GTK_TREE_MODEL(qoi->store), &here, 0, &dname, 1, &dpath, -1);
					if((len = g_snprintf(buf, sizeof buf, "%s%s%s", dpath, G_DIR_SEPARATOR_S, dname)) < sizeof buf)
					{
						if((fn = g_filename_from_utf8(buf, (gssize) len, NULL, NULL, NULL)) != NULL)
						{
							document_open_file(buf, FALSE, NULL, NULL);
							g_free(fn);
						}
					}
				}
				gtk_tree_path_free(unfiltered);
			}
		}
		g_list_foreach(selection, (GFunc) gtk_tree_path_free, NULL);
		g_list_free(selection);
	}
	gtk_widget_hide(qoi->dialog);
}

/* -------------------------------------------------------------------------------------------------------------- */

GtkTreeModel * tree_model_new(void)
{
	GtkTreeStore	*ts;
	GtkTreeIter	iter;

	/* First column is display text, second is corresponding path (or path part). */
	ts = gtk_tree_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
	gtk_tree_store_append(ts, &iter, NULL);
	gtk_tree_store_set(ts, &iter, 0, _("Repositories (Right-click to add)"), 1, NULL,-1);

	return GTK_TREE_MODEL(ts);
}

/* Look up a repository, by searching for a node immediately under the root that has the given path as its data. */
gboolean tree_model_find_repository(GtkTreeModel *model, const gchar *root_path, GtkTreeIter *iter)
{
	GtkTreeIter	root;
	gboolean	found = FALSE;

	if(gtk_tree_model_get_iter_first(model, &root))
	{
		if(gtk_tree_model_iter_children(model, iter, &root))
		{
			gchar	*data;

			do
			{
				gtk_tree_model_get(model, iter, 1, &data, -1);
				if(data != NULL)
				{
					found = strcmp(data, root_path) == 0;
					g_free(data);
				}
			} while(gtk_tree_model_iter_next(model, iter));
		}
	}
	return found;
}

static guint	tree_model_build_populate(GtkTreeModel *model, gchar *lines, GtkTreeIter *parent);
static guint	tree_model_build_traverse(GtkTreeModel *model, GNode *root, GtkTreeIter *parent);

void tree_model_build_repository(GtkTreeModel *model, GtkTreeIter *repo, const gchar *root_path)
{
	GtkTreeIter	new;
	const gchar	*slash;
	gchar		*git_ls_files[] = { "git", "ls-files", NULL };
	gchar		*git_stdout = NULL, *git_stderr = NULL;
	GTimer		*timer;

	slash = strrchr(root_path, G_DIR_SEPARATOR);
	if(slash == NULL)
		slash = root_path;
	else
		slash++;

	if(repo == NULL)
	{
		GtkTreeIter	iter;

		if(gtk_tree_model_get_iter_first(model, &iter))
		{
			repo = &new;
			gtk_tree_store_append(GTK_TREE_STORE(model), repo, &iter);
		}
	}
	/* At this point, we have a root iter in the tree, which we need to populate. */
	gtk_tree_store_set(GTK_TREE_STORE(model), repo, 0, slash, 1, root_path,-1);

	/* Now list the repository, and build a tree representation. Easy-peasy, right? */
	timer = g_timer_new();
	if(subprocess_run(root_path, git_ls_files, NULL, &git_stdout, &git_stderr))
	{
		GtkTreePath	*path;
		guint		counter = tree_model_build_populate(model, git_stdout, repo);

		g_free(git_stdout);
		g_free(git_stderr);

		path = gtk_tree_model_get_path(model, repo);
		gtk_tree_view_expand_to_path(GTK_TREE_VIEW(gitbrowser.view), path);
		gtk_tree_view_set_cursor_on_cell(GTK_TREE_VIEW(gitbrowser.view), path, NULL, NULL, FALSE);
		gtk_tree_path_free(path);
		msgwin_status_add(_("Built repository \"%s\", %lu files added in %.1f ms."), slash, (unsigned long) counter, 1e3 * g_timer_elapsed(timer, NULL));
	}
	g_timer_destroy(timer);
}

/* Look for a child with the given text as its data; if not found it's added in the proper location and returned. */
static GNode * get_child(GNode *root, const gchar *text)
{
	GNode	*here;

	if(root == NULL || text == NULL)
		return NULL;

	if((here = g_node_first_child(root)) != NULL)
	{
		while(here != NULL)
		{
			const gint	rel = strcmp(text, here->data);

			if(rel == 0)
				return here;
			if(rel < 0)
				return g_node_insert_data_before(root, here, (gpointer) text);
			here = g_node_next_sibling(here);
		}
	}
	return g_node_append_data(root, (gpointer) text);
}

static guint tree_model_build_populate(GtkTreeModel *model, gchar *lines, GtkTreeIter *parent)
{
	gchar	*line, *nextline, *dir, *endptr;
	GNode	*root = g_node_new(""), *prev;
	guint	count;

	/* Let's cheat: build a GNode n:ary tree first, then use that to build GtkTreeModel data. */
	while((line = tok_tokenize_next(lines, &nextline, '\n')) != NULL)
	{
		prev = root;
		dir = line;
		while((dir = tok_tokenize_next(dir, &endptr, G_DIR_SEPARATOR)) != NULL)
		{
			prev = get_child(prev, dir);
			dir = endptr;
		}
		if(prev == root)
			get_child(root, line);
		lines = nextline;
	}
	count = tree_model_build_traverse(model, root, parent);
	g_node_destroy(root);
	return count;
}

/* Traverse the children of the given GNode tree, and build a corresponding GtkTreeModel.
 * The traversal order is special: inner nodes first, to group directories on top.
*/
static guint tree_model_build_traverse(GtkTreeModel *model, GNode *root, GtkTreeIter *parent)
{
	GNode		*child;
	GtkTreeIter	iter;
	gchar		*dname;
	guint		count = 0;

	/* Inner nodes. */
	for(child = g_node_first_child(root); child != NULL; child = g_node_next_sibling(child))
	{
		if(g_node_first_child(child) == NULL)
			continue;
		/* We now know this is an inner node; add tree node and recurse. */
		gtk_tree_store_append(GTK_TREE_STORE(model), &iter, parent);
		dname = g_filename_display_name(child->data);
		gtk_tree_store_set(GTK_TREE_STORE(model), &iter, 0, dname, 1, child->data, -1);
		g_free(dname);
		count += tree_model_build_traverse(model, child, &iter);	/* Don't count inner node itself. */
	}
	/* Leaves. */
	for(child = g_node_first_child(root); child != NULL; child = g_node_next_sibling(child))
	{
		if(g_node_first_child(child) != NULL)
			continue;
		gtk_tree_store_append(GTK_TREE_STORE(model), &iter, parent);
		dname = g_filename_display_name(child->data);
		gtk_tree_store_set(GTK_TREE_STORE(model), &iter, 0, dname, 1, child->data,-1);
		g_free(dname);
		count += 1;
	}
	return count;
}

gboolean tree_model_open_document(GtkTreeModel *model, GtkTreePath *path)
{
	GtkTreeIter	iter, child;

	if(gtk_tree_model_get_iter(model, &iter, path))
	{
		GString	*path = g_string_sized_new(1024);
		gchar	*component;

		/* Walk towards the root, building the filename as we go. */
		do
		{
			gtk_tree_model_get(model, &iter, 1, &component, -1);
			if(component != NULL)
			{
				if(path->len > 0)
					g_string_prepend(path, G_DIR_SEPARATOR_S);
				g_string_prepend(path, component);
				g_free(component);
			}
			child = iter;
		} while(gtk_tree_model_iter_parent(model, &iter, &child));
		document_open_file(path->str, FALSE, NULL, NULL);
		g_string_free(path, TRUE);
		return TRUE;
	}
	return FALSE;
}

/* Gets the full path, in the local system's encoding, for the indicated document. Returns FALSE if given an inner node. */
gboolean tree_model_get_document_path(GtkTreeModel *model, const GtkTreeIter *iter, gchar *buf, gsize buf_max)
{
	if(!gtk_tree_model_iter_has_child(model, (GtkTreeIter *) iter))
	{
		GString		*path = g_string_sized_new(1024);
		GtkTreeIter	here = *iter, child;
		gboolean	ok;

		/* Walk towards the root, building the filename as we go. */
		do
		{
			gchar	*component = NULL;

			gtk_tree_model_get(model, &here, 1, &component, -1);
			if(component != NULL)
			{
				if(path->len > 0)
					g_string_prepend(path, G_DIR_SEPARATOR_S);
				g_string_prepend(path, component);
				g_free(component);
			}
			child = here;
		} while(gtk_tree_model_iter_parent(model, &here, &child));
		ok = g_strlcpy(buf, path->str, buf_max) < buf_max;
		g_string_free(path, TRUE);

		return ok;
	}
	return FALSE;
}

void tree_model_foreach(GtkTreeModel *model, GtkTreeIter *root, void (*node_callback)(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer user), gpointer user)
{
	do
	{
		GtkTreeIter	child;

		if(gtk_tree_model_iter_children(model, &child, root))
		{
			tree_model_foreach(model, &child, node_callback, user);
		}
		else
		{
			GtkTreePath	*here;

			here = gtk_tree_model_get_path(model, root);
			node_callback(model, here, root, user);
			gtk_tree_path_free(here);
		}
	} while(gtk_tree_model_iter_next(model, root));
}

static gboolean evt_menu_selection_done(GtkWidget *wid, gpointer user)
{
	if(gitbrowser.click_path != NULL)
	{
		gtk_tree_path_free(gitbrowser.click_path);
		gitbrowser.click_path = NULL;
	}
	return FALSE;
}

/* On deactivation, remove all widgets that are not separators, so they aren't destroyed. */
static void evt_menu_deactivate(GtkWidget *wid, gpointer user)
{
	GList	*children = gtk_container_get_children(GTK_CONTAINER(wid)), *iter;

	for(iter = children; iter != NULL; iter = g_list_next(iter))
	{
		if(G_OBJECT_TYPE(G_OBJECT(iter->data)) != GTK_TYPE_SEPARATOR_MENU_ITEM)
			gtk_container_remove(GTK_CONTAINER(wid), iter->data);
	}
	g_list_free(children);
}

/* Creates a new popup menu suitable for use in our tree, and connects the selection-done signal. */
static GtkWidget * menu_popup_create(void)
{
	GtkWidget	*menu;

	menu = gtk_menu_new();
	g_signal_connect(G_OBJECT(menu), "selection_done", G_CALLBACK(evt_menu_selection_done), NULL);
	g_signal_connect(G_OBJECT(menu), "deactivate", G_CALLBACK(evt_menu_deactivate), NULL);

	return menu;
}

static void menu_popup_repositories(GdkEventButton *evt)
{
	gitbrowser.main_menu = menu_popup_create();
	gtk_menu_shell_append(GTK_MENU_SHELL(gitbrowser.main_menu), gitbrowser.action_menu_items[CMD_REPOSITORY_OPEN_QUICK_FROM_DOCUMENT]);
	gtk_menu_shell_append(GTK_MENU_SHELL(gitbrowser.main_menu), gtk_separator_menu_item_new());
	gtk_menu_shell_append(GTK_MENU_SHELL(gitbrowser.main_menu), gitbrowser.action_menu_items[CMD_REPOSITORY_ADD]);
	gtk_menu_shell_append(GTK_MENU_SHELL(gitbrowser.main_menu), gitbrowser.action_menu_items[CMD_REPOSITORY_ADD_FROM_DOCUMENT]);
	gtk_menu_shell_append(GTK_MENU_SHELL(gitbrowser.main_menu), gtk_separator_menu_item_new());
	gtk_menu_shell_append(GTK_MENU_SHELL(gitbrowser.main_menu), gitbrowser.action_menu_items[CMD_REPOSITORY_REMOVE_ALL]);
	gtk_menu_shell_append(GTK_MENU_SHELL(gitbrowser.main_menu), gtk_separator_menu_item_new());
	gtk_menu_shell_append(GTK_MENU_SHELL(gitbrowser.main_menu), gitbrowser.action_menu_items[CMD_PREFERENCES]);
	gtk_widget_show_all(gitbrowser.main_menu);
	gtk_menu_popup(GTK_MENU(gitbrowser.main_menu), NULL, NULL, NULL, NULL, evt->button, evt->time);
}

static void menu_popup_repository(GdkEventButton *evt)
{
	GtkWidget	*menu = menu_popup_create();

	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gitbrowser.action_menu_items[CMD_REPOSITORY_OPEN_QUICK]);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gitbrowser.action_menu_items[CMD_REPOSITORY_MOVE_UP]);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gitbrowser.action_menu_items[CMD_REPOSITORY_MOVE_DOWN]);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gitbrowser.action_menu_items[CMD_REPOSITORY_REMOVE]);
	gtk_widget_show_all(menu);
	gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, evt->button, evt->time);
}

static void menu_popup_directory(GdkEventButton *evt)
{
	GtkWidget	*menu = menu_popup_create();

	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gitbrowser.action_menu_items[CMD_DIR_EXPAND]);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gitbrowser.action_menu_items[CMD_DIR_COLLAPSE]);

	gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, evt->button, evt->time);
}

static void menu_popup_file(GdkEventButton *evt)
{
	GtkWidget	*menu = menu_popup_create();

	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gitbrowser.action_menu_items[CMD_FILE_OPEN]);

	gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, evt->button, evt->time);
}

static gboolean evt_tree_button_press(GtkWidget *wid, GdkEventButton *evt, gpointer user)
{
	if(gitbrowser.click_path != NULL)
	{
		gtk_tree_path_free(gitbrowser.click_path);
		gitbrowser.click_path = NULL;
	}

	gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(wid), evt->x, evt->y, &gitbrowser.click_path, NULL, NULL, NULL);
	if(gitbrowser.click_path != NULL)
	{
		gint		depth;
		const gint	*indices = gtk_tree_path_get_indices_with_depth(gitbrowser.click_path, &depth);
		gboolean	is_dir = FALSE;
		GtkTreeIter	iter;

		if(indices == NULL)
			return FALSE;

		if(depth >= 3)			/* Need to determine if clicked path is file or directory. */
		{
			if(gtk_tree_model_get_iter(gitbrowser.model, &iter, gitbrowser.click_path))
				is_dir = gtk_tree_model_iter_has_child(gitbrowser.model, &iter);
		}

		if(evt->type == GDK_2BUTTON_PRESS && evt->button == 1 && depth >= 3)
		{
			if(is_dir)
			{
				if(!gtk_tree_view_collapse_row(GTK_TREE_VIEW(gitbrowser.view), gitbrowser.click_path))
					gtk_tree_view_expand_row(GTK_TREE_VIEW(gitbrowser.view), gitbrowser.click_path, TRUE);
			}
			else
				gtk_action_activate(gitbrowser.actions[CMD_FILE_OPEN]);
		}
		else if(evt->type == GDK_BUTTON_PRESS && evt->button == 3)
		{
			if(depth == 1 && indices[0] == 0)
				menu_popup_repositories(evt);
			else if(depth == 2)
				menu_popup_repository(evt);
			else if(depth >= 3)
			{
				if(is_dir)
					menu_popup_directory(evt);
				else
					menu_popup_file(evt);
			}
			return TRUE;
		}
	}
	return FALSE;
}

GtkWidget * tree_view_new(GtkTreeModel *model)
{
	GtkWidget		*view;
        GtkCellRenderer         *cr;
        GtkTreeViewColumn       *vc;

	view = gtk_tree_view_new_with_model(model);

	cr = gtk_cell_renderer_text_new();
	vc = gtk_tree_view_column_new_with_attributes("(string)", cr, "text", 0, NULL);
	gtk_tree_view_append_column(GTK_TREE_VIEW(view), vc);

	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(gitbrowser.view), FALSE);

	g_signal_connect(G_OBJECT(view), "button_press_event", G_CALLBACK(evt_tree_button_press), NULL);

	return view;
}

/* -------------------------------------------------------------------------------------------------------------- */

static void open_quick_reset_filter(void)
{
	if(gitbrowser.quick_open_hide != NULL)
	{
		g_regex_unref(gitbrowser.quick_open_hide);
	}
	if(gitbrowser.quick_open_hide_src[0] != '\0')
		gitbrowser.quick_open_hide = g_regex_new(gitbrowser.quick_open_hide_src, 0, 0, NULL);
	else
		gitbrowser.quick_open_hide = NULL;
}

/* -------------------------------------------------------------------------------------------------------------- */

static void cb_key_group_callback(guint key_id)
{
	switch(key_id)
	{
	case KEY_REPOSITORY_OPEN_QUICK_FROM_DOCUMENT:
		gtk_action_activate(gitbrowser.actions[CMD_REPOSITORY_OPEN_QUICK_FROM_DOCUMENT]);
		break;
	}
}

void plugin_init(GeanyData *geany_data)
{
	GtkWidget	*scwin;
	gchar		*dir;

	init_commands(gitbrowser.actions, gitbrowser.action_menu_items);

	gitbrowser.model = tree_model_new();
	gitbrowser.view = tree_view_new(gitbrowser.model);
	gitbrowser.repositories = g_hash_table_new(g_str_hash, g_str_equal);
	gitbrowser.quick_open_filter_max_time = 50;
	gitbrowser.quick_open_hide = NULL;

	gitbrowser.key_group = plugin_set_key_group(geany_plugin, MNEMONIC_NAME, NUM_KEYS, NULL);
	keybindings_set_item(gitbrowser.key_group, KEY_REPOSITORY_OPEN_QUICK_FROM_DOCUMENT, cb_key_group_callback, GDK_KEY_O, GDK_SHIFT_MASK | GDK_CONTROL_MASK, "repository-open-quick-from-document", _("Quick Open from Document"), gitbrowser.action_menu_items[CMD_REPOSITORY_OPEN_QUICK_FROM_DOCUMENT]);

	dir = g_strconcat(geany->app->configdir, G_DIR_SEPARATOR_S, "plugins", G_DIR_SEPARATOR_S, MNEMONIC_NAME, NULL);
	utils_mkdir(dir, TRUE);
	gitbrowser.config_filename = g_strconcat(dir, G_DIR_SEPARATOR_S, MNEMONIC_NAME ".conf", NULL);
	g_free(dir);

	gitbrowser.prefs = stash_group_new(MNEMONIC_NAME);
	stash_group_add_entry(gitbrowser.prefs, &gitbrowser.quick_open_hide_src, CFG_QUICK_OPEN_HIDE_SRC, NULL, CFG_QUICK_OPEN_HIDE_SRC);
	stash_group_add_spin_button_integer(gitbrowser.prefs, &gitbrowser.quick_open_filter_max_time, CFG_QUICK_OPEN_FILTER_MAX_TIME, 50, CFG_QUICK_OPEN_FILTER_MAX_TIME);
	gitbrowser.prefs_dlg = NULL;

	repository_load_all();

	scwin = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scwin), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(scwin), gitbrowser.view);
	gtk_widget_show_all(scwin);
	gtk_notebook_append_page(GTK_NOTEBOOK(geany->main_widgets->sidebar_notebook), scwin, gtk_label_new("Git Browser"));
}

void plugin_cleanup(void)
{
	repository_save_all(gitbrowser.model);
	stash_group_free(gitbrowser.prefs);
	g_free(gitbrowser.config_filename);
	g_hash_table_destroy(gitbrowser.repositories);
}
