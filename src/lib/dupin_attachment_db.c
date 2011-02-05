#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "dupin_internal.h"
#include "dupin_utils.h"
#include "dupin_attachment_db.h"

#include <stdlib.h>
#include <string.h>

#define DUPIN_ATTACHMENT_DB_SQL_MAIN_CREATE \
  "CREATE TABLE IF NOT EXISTS Dupin (\n" \
  "  id        CHAR(255) NOT NULL,\n" \
  "  title     CHAR(255) NOT NULL,\n" \
  "  type      CHAR(255) DEFAULT 'application/octect-stream',\n" \
  "  length    INTEGER DEFAULT 0,\n" \
  "  hash      CHAR(255),\n" \
  "  content   BLOB NOT NULL DEFAULT '',\n" \
  "  PRIMARY KEY(id, title)\n" \
  ");"

#define DUPIN_ATTACHMENT_DB_SQL_CREATE_INDEX \
  "CREATE INDEX IF NOT EXISTS DupinId ON Dupin (id);\n" \
  "CREATE INDEX IF NOT EXISTS DupinTitle ON Dupin (title);"

#define DUPIN_ATTACHMENT_DB_SQL_DESC_CREATE \
  "CREATE TABLE IF NOT EXISTS DupinAttachmentDB (\n" \
  "  parent                    CHAR(255) NOT NULL\n" \
  ");"

gchar **
dupin_get_attachment_dbs (Dupin * d)
{
  guint i;
  gsize size;
  gchar **ret;
  gpointer key;
  GHashTableIter iter;

  g_return_val_if_fail (d != NULL, NULL);

  g_mutex_lock (d->mutex);

  if (!(size = g_hash_table_size (d->attachment_dbs)))
    {
      g_mutex_unlock (d->mutex);
      return NULL;
    }

  ret = g_malloc (sizeof (gchar *) * (g_hash_table_size (d->attachment_dbs) + 1));

  i = 0;
  g_hash_table_iter_init (&iter, d->attachment_dbs);
  while (g_hash_table_iter_next (&iter, &key, NULL) == TRUE)
    ret[i++] = g_strdup (key);

  ret[i] = NULL;

  g_mutex_unlock (d->mutex);

  return ret;
}

gboolean
dupin_attachment_db_exists (Dupin * d, gchar * attachment_db)
{
  gboolean ret;

  g_mutex_lock (d->mutex);
  ret = g_hash_table_lookup (d->attachment_dbs, attachment_db) != NULL ? TRUE : FALSE;
  g_mutex_unlock (d->mutex);

  return ret;
}

DupinAttachmentDB *
dupin_attachment_db_open (Dupin * d, gchar * attachment_db, GError ** error)
{
  DupinAttachmentDB *ret;

  g_return_val_if_fail (d != NULL, NULL);
  g_return_val_if_fail (attachment_db != NULL, NULL);

  g_mutex_lock (d->mutex);

  if (!(ret = g_hash_table_lookup (d->attachment_dbs, attachment_db)) || ret->todelete == TRUE)
    {
      g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN,
		 "Attachment DB '%s' doesn't exist.", attachment_db);

      g_mutex_unlock (d->mutex);
      return NULL;
    }
  else
    ret->ref++;

  g_mutex_unlock (d->mutex);

  return ret;
}

DupinAttachmentDB *
dupin_attachment_db_new (Dupin * d, gchar * attachment_db,
                         gchar * parent,
		         GError ** error)
{
  DupinAttachmentDB *ret;
  gchar *path;
  gchar *name;

  gchar *str;
  gchar *errmsg;

  g_return_val_if_fail (d != NULL, NULL);
  g_return_val_if_fail (attachment_db != NULL, NULL);
  g_return_val_if_fail (parent != NULL, NULL);
  g_return_val_if_fail (dupin_util_is_valid_attachment_db_name (attachment_db) == TRUE, NULL);

  g_return_val_if_fail (dupin_database_exists (d, parent) == TRUE, NULL);

  g_mutex_lock (d->mutex);

  if ((ret = g_hash_table_lookup (d->attachment_dbs, attachment_db)))
    {
      g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN,
		   "Attachment DB '%s' already exist.", attachment_db);
      g_mutex_unlock (d->mutex);
      return NULL;
    }

  name = g_strdup_printf ("%s%s", attachment_db, DUPIN_ATTACHMENT_DB_SUFFIX);
  path = g_build_path (G_DIR_SEPARATOR_S, d->path, name, NULL);
  g_free (name);

  if (!(ret = dupin_attachment_db_create (d, attachment_db, path, error)))
    {
      g_mutex_unlock (d->mutex);
      g_free (path);
      return NULL;
    }

  ret->parent = g_strdup (parent);

  g_free (path);
  ret->ref++;

  str =
    sqlite3_mprintf ("INSERT INTO DupinAttachmentDB "
		     "(parent) "
		     "VALUES('%q')", parent);

  if (sqlite3_exec (ret->db, str, NULL, NULL, &errmsg) != SQLITE_OK)
    {
      g_mutex_unlock (d->mutex);

      g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN, "%s",
		   errmsg);

      sqlite3_free (errmsg);
      sqlite3_free (str);
      dupin_attachment_db_free (ret);
      return NULL;
    }

  sqlite3_free (str);

  g_mutex_unlock (d->mutex);

  if (dupin_attachment_db_p_update (ret, error) == FALSE)
    {
      dupin_attachment_db_free (ret);
      return NULL;
    }

  g_mutex_lock (d->mutex);
  g_hash_table_insert (d->attachment_dbs, g_strdup (attachment_db), ret);
  g_mutex_unlock (d->mutex);

  return ret;
}

struct dupin_attachment_db_p_update_t
{
  gchar *parent;
};

static int
dupin_attachment_db_p_update_cb (void *data, int argc, char **argv, char **col)
{
  struct dupin_attachment_db_p_update_t *update = data;

  if (argv[0] && *argv[0])
    update->parent = g_strdup (argv[0]);

  return 0;
}

#define DUPIN_ATTACHMENT_DB_P_SIZE	64

static void
dupin_attachment_db_p_update_real (DupinAttachmentDBP * p, DupinAttachmentDB * attachment_db)
{
  if (p->attachment_dbs == NULL)
    {
      p->attachment_dbs = g_malloc (sizeof (DupinAttachmentDB *) * DUPIN_ATTACHMENT_DB_P_SIZE);
      p->size = DUPIN_ATTACHMENT_DB_P_SIZE;
    }

  else if (p->numb == p->size)
    {
      p->size += DUPIN_ATTACHMENT_DB_P_SIZE;
      p->attachment_dbs = g_realloc (p->attachment_dbs, sizeof (DupinAttachmentDB *) * p->size);
    }

  p->attachment_dbs[p->numb] = attachment_db;
  p->numb++;
}

gboolean
dupin_attachment_db_p_update (DupinAttachmentDB * attachment_db, GError ** error)
{
  gchar *errmsg;
  struct dupin_attachment_db_p_update_t update;
  gchar *query = "SELECT parent FROM DupinAttachmentDB";

  memset (&update, 0, sizeof (struct dupin_attachment_db_p_update_t));

  g_mutex_lock (attachment_db->d->mutex);

  if (sqlite3_exec (attachment_db->db, query, dupin_attachment_db_p_update_cb, &update, &errmsg)
      != SQLITE_OK)
    {
      g_mutex_unlock (attachment_db->d->mutex);

      g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN, "%s",
		   errmsg);
      sqlite3_free (errmsg);
      return FALSE;
    }

  g_mutex_unlock (attachment_db->d->mutex);

  if (!update.parent)
    {
      g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN,
		   "Internal error.");
      return FALSE;
    }

  DupinDB *db;

  if (!(db = dupin_database_open (attachment_db->d, update.parent, error)))
    {
      g_free (update.parent);
      return FALSE;
    }

  g_mutex_lock (db->mutex);
  dupin_attachment_db_p_update_real (&db->attachment_dbs, attachment_db);
  g_mutex_unlock (db->mutex);

  dupin_database_unref (db);

  /* make sure parameters are set after dupin server restart on existing database */

  if (attachment_db->parent == NULL)
    attachment_db->parent = update.parent;
  else
    g_free (update.parent);

  return TRUE;
}

void
dupin_attachment_db_p_record_insert (DupinAttachmentDBP * p,
				     gchar *       id,
				     gchar *       title,
  				     gsize         length,
  				     gchar *       type,
			    	     const void ** content)
{
  g_return_if_fail (p != NULL);
  g_return_if_fail (id != NULL);
  g_return_if_fail (title != NULL);
  g_return_if_fail (length >= 0);
  g_return_if_fail (type != NULL);
  g_return_if_fail (content != NULL);

  gsize i;

  for (i = 0; i < p->numb; i++)
    {
      DupinAttachmentDB *attachment_db = p->attachment_dbs[i];

      if (dupin_attachment_record_create (attachment_db, id, title, length, type, content) == FALSE)
        {
	  return;
        }
    }
}

void
dupin_attachment_db_p_record_delete (DupinAttachmentDBP * p,
				     gchar *       id,
				     gchar *       title)
{
  g_return_if_fail (p != NULL);
  g_return_if_fail (id != NULL);
  g_return_if_fail (title != NULL);

  gsize i;

  for (i = 0; i < p->numb; i++)
    {
      DupinAttachmentDB *attachment_db = p->attachment_dbs[i];

      if (dupin_attachment_record_delete (attachment_db, id, title) == FALSE)
        {
          return;
        }
    }
}

void
dupin_attachment_db_ref (DupinAttachmentDB * attachment_db)
{
  Dupin *d;

  g_return_if_fail (attachment_db != NULL);

  d = attachment_db->d;

  g_mutex_lock (d->mutex);
  attachment_db->ref++;
  g_mutex_unlock (d->mutex);
}

void
dupin_attachment_db_unref (DupinAttachmentDB * attachment_db)
{
  Dupin *d;

  g_return_if_fail (attachment_db != NULL);

  d = attachment_db->d;
  g_mutex_lock (d->mutex);

  if (attachment_db->ref >= 0)
    attachment_db->ref--;

  if (attachment_db->ref != 0 && attachment_db->todelete == TRUE)
    g_warning ("dupin_attachment_db_unref: (thread=%p) attachment database %s flagged for deletion but can't free it due ref is %d\n", g_thread_self (), attachment_db->name, (gint) attachment_db->ref);

  if (attachment_db->ref == 0 && attachment_db->todelete == TRUE)
    g_hash_table_remove (d->attachment_dbs, attachment_db->name);

  g_mutex_unlock (d->mutex);
}

gboolean
dupin_attachment_db_delete (DupinAttachmentDB * attachment_db, GError ** error)
{
  Dupin *d;

  g_return_val_if_fail (attachment_db != NULL, FALSE);

  d = attachment_db->d;

  g_mutex_lock (d->mutex);
  attachment_db->todelete = TRUE;
  g_mutex_unlock (d->mutex);

  return TRUE;
}

const gchar *
dupin_attachment_db_get_name (DupinAttachmentDB * attachment_db)
{
  g_return_val_if_fail (attachment_db != NULL, NULL);
  return attachment_db->name;
}

const gchar *
dupin_attachment_db_get_parent (DupinAttachmentDB * attachment_db)
{
  g_return_val_if_fail (attachment_db != NULL, NULL);

  return attachment_db->parent;
}

gsize
dupin_attachment_db_get_size (DupinAttachmentDB * attachment_db)
{
  struct stat st;

  g_return_val_if_fail (attachment_db != NULL, 0);

  if (g_stat (attachment_db->path, &st) != 0)
    return 0;

  return (gsize) st.st_size;
}

/* Internal: */
void
dupin_attachment_db_free (DupinAttachmentDB * attachment_db)
{
  g_message("dupin_attachment_db_free: total number of changes for '%s' attachments database: %d\n", attachment_db->name, (gint)sqlite3_total_changes (attachment_db->db));

  if (attachment_db->db)
    sqlite3_close (attachment_db->db);

  if (attachment_db->todelete == TRUE)
    g_unlink (attachment_db->path);

  if (attachment_db->name)
    g_free (attachment_db->name);

  if (attachment_db->path)
    g_free (attachment_db->path);

  if (attachment_db->parent)
    g_free (attachment_db->parent);

  if (attachment_db->mutex)
    g_mutex_free (attachment_db->mutex);

  if (attachment_db->error_msg)
    g_free (attachment_db->error_msg);

  if (attachment_db->warning_msg)
    g_free (attachment_db->warning_msg);

  g_free (attachment_db);
}

static int
dupin_attachment_db_create_cb (void *data, int argc, char **argv, char **col)
{
  DupinAttachmentDB *attachment_db = data;

  if (argc == 1)
    {
      attachment_db->parent = g_strdup (argv[0]);
    }

  return 0;
}

DupinAttachmentDB *
dupin_attachment_db_create (Dupin * d, gchar * name, gchar * path, GError ** error)
{
  gchar *query;
  gchar *errmsg;
  DupinAttachmentDB *attachment_db;

  attachment_db = g_malloc0 (sizeof (DupinAttachmentDB));

  attachment_db->d = d;

  attachment_db->name = g_strdup (name);
  attachment_db->path = g_strdup (path);

  if (sqlite3_open (attachment_db->path, &attachment_db->db) != SQLITE_OK)
    {
      g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN,
		   "Attachment DB error.");
      dupin_attachment_db_free (attachment_db);
      return NULL;
    }

  if (sqlite3_exec (attachment_db->db, DUPIN_ATTACHMENT_DB_SQL_MAIN_CREATE, NULL, NULL, &errmsg)
      				!= SQLITE_OK
      || sqlite3_exec (attachment_db->db, DUPIN_ATTACHMENT_DB_SQL_DESC_CREATE, NULL, NULL,
		       &errmsg) != SQLITE_OK
      || sqlite3_exec (attachment_db->db, DUPIN_ATTACHMENT_DB_SQL_CREATE_INDEX, NULL, NULL,
		       &errmsg) != SQLITE_OK)
    {
      g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN, "%s",
		   errmsg);
      sqlite3_free (errmsg);
      dupin_attachment_db_free (attachment_db);
      return NULL;
    }

  query =
    "SELECT parent FROM DupinAttachmentDB";

  if (sqlite3_exec (attachment_db->db, query, dupin_attachment_db_create_cb, attachment_db, &errmsg) !=
      SQLITE_OK)
    {
      g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN, "%s",
		   errmsg);
      sqlite3_free (errmsg);
      dupin_attachment_db_free (attachment_db);
    }

  attachment_db->mutex = g_mutex_new ();

  return attachment_db;
}

static int
dupin_attachment_db_count_cb (void *data, int argc, char **argv, char **col)
{
  gsize *size = data;

  if (argv[0] && *argv[0])
    *size = atoi (argv[0]);

  return 0;
}

gsize
dupin_attachment_db_count (DupinAttachmentDB * attachment_db)
{
  gsize size;
  gchar *query;

  g_return_val_if_fail (attachment_db != NULL, 0);

  query = "SELECT count(*) as c FROM Dupin";

  g_mutex_lock (attachment_db->mutex);

  if (sqlite3_exec (attachment_db->db, query, dupin_attachment_db_count_cb, &size, NULL) !=
      SQLITE_OK)
    {
      g_mutex_unlock (attachment_db->mutex);
      return 0;
    }

  g_mutex_unlock (attachment_db->mutex);
  return size;
}

void
dupin_attachment_db_set_error (DupinAttachmentDB * attachment_db,
                          gchar * msg)
{
  g_return_if_fail (attachment_db != NULL);
  g_return_if_fail (msg != NULL);

  dupin_attachment_db_clear_error (attachment_db);

  attachment_db->error_msg = g_strdup ( msg );

  return;
}

void
dupin_attachment_db_clear_error (DupinAttachmentDB * attachment_db)
{
  g_return_if_fail (attachment_db != NULL);

  if (attachment_db->error_msg != NULL)
    g_free (attachment_db->error_msg);

  attachment_db->error_msg = NULL;

  return;
}

gchar * dupin_attachment_db_get_error (DupinAttachmentDB * attachment_db)
{
  g_return_val_if_fail (attachment_db != NULL, NULL);

  return attachment_db->error_msg;
}

void dupin_attachment_db_set_warning (DupinAttachmentDB * attachment_db,
                                 gchar * msg)
{
  g_return_if_fail (attachment_db != NULL);
  g_return_if_fail (msg != NULL);

  dupin_attachment_db_clear_warning (attachment_db);

  attachment_db->warning_msg = g_strdup ( msg );

  return;
}

void dupin_attachment_db_clear_warning (DupinAttachmentDB * attachment_db)
{
  g_return_if_fail (attachment_db != NULL);

  if (attachment_db->warning_msg != NULL)
    g_free (attachment_db->warning_msg);

  attachment_db->warning_msg = NULL;

  return;
}

gchar * dupin_attachment_db_get_warning (DupinAttachmentDB * attachment_db)
{
  g_return_val_if_fail (attachment_db != NULL, NULL);

  return attachment_db->warning_msg;
}

/* EOF */
