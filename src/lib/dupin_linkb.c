#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "dupin_internal.h"
#include "dupin_utils.h"
#include "dupin_date.h"
#include "dupin_linkb.h"
#include "dupin_link_record.h"
#include "dupin_view.h"

#include <stdlib.h>
#include <string.h>

#define DUPIN_LINKB_SQL_MAIN_CREATE \
  "CREATE TABLE IF NOT EXISTS Dupin (\n" \
  "  seq         INTEGER PRIMARY KEY AUTOINCREMENT,\n" \
  "  id          CHAR(255) NOT NULL,\n" \
  "  rev         INTEGER NOT NULL DEFAULT 1,\n" \
  "  hash        CHAR(255) NOT NULL,\n" \
  "  obj         TEXT,\n" \
  "  deleted     BOOL DEFAULT FALSE,\n" \
  "  tm          INTEGER NOT NULL,\n" \
  "  expire_tm   INTEGER NOT NULL DEFAULT 0,\n" \
  "  context_id  CHAR(255) NOT NULL,\n" \
  "  label       CHAR(255) NOT NULL,\n" \
  "  href        TEXT NOT NULL,\n" \
  "  rel         TEXT NOT NULL,\n" \
  "  is_weblink  BOOL DEFAULT FALSE,\n" \
  "  authority   TEXT DEFAULT NULL,\n" \
  "  rev_head    BOOL DEFAULT TRUE,\n" \
  "  UNIQUE      (id, rev)\n" \
  ");"

#define DUPIN_LINKB_SQL_CREATE_INDEX \
  "CREATE INDEX IF NOT EXISTS DupinId ON Dupin (id);\n" \
  "CREATE INDEX IF NOT EXISTS DupinIdRev ON Dupin (id,rev);\n" \
  "CREATE INDEX IF NOT EXISTS DupinIdRevHead ON Dupin (id,rev_head);\n" \
  "CREATE INDEX IF NOT EXISTS DupinContextId ON Dupin (context_id);\n" \
  "CREATE INDEX IF NOT EXISTS DupinObj ON Dupin (obj);\n" \
  "CREATE INDEX IF NOT EXISTS DupinHrefDeletedAuthority ON Dupin (href,deleted,authority);"

#define DUPIN_LINKB_SQL_DESC_CREATE \
  "CREATE TABLE IF NOT EXISTS DupinLinkB (\n" \
  "  total_webl_ins  INTEGER NOT NULL DEFAULT 0,\n" \
  "  total_webl_del  INTEGER NOT NULL DEFAULT 0,\n" \
  "  total_rel_ins   INTEGER NOT NULL DEFAULT 0,\n" \
  "  total_rel_del   INTEGER NOT NULL DEFAULT 0,\n" \
  "  parent          CHAR(255) NOT NULL,\n" \
  "  isdb            BOOL DEFAULT TRUE,\n" \
  "  compact_id      CHAR(255) NOT NULL DEFAULT '0',\n" \
  "  check_id        CHAR(255) NOT NULL DEFAULT '0',\n" \
  "  creation_time   CHAR(255) NOT NULL DEFAULT '0'\n" \
  ");\n" \
  "PRAGMA user_version = 5"

#define DUPIN_LINKB_SQL_DESC_UPGRADE_FROM_VERSION_1 \
  "ALTER TABLE Dupin      ADD COLUMN authority   TEXT DEFAULT NULL;\n" \
  "ALTER TABLE Dupin      ADD COLUMN expire_tm   INTEGER NOT NULL DEFAULT 0;\n" \
  "ALTER TABLE DupinLinkB ADD COLUMN creation_time CHAR(255) NOT NULL DEFAULT '0';\n" \
  "DROP  INDEX IF EXISTS  DupinHrefDeletedTag;\n" \
  "CREATE INDEX IF NOT EXISTS DupinHrefDeletedAuthority ON Dupin (href,deleted,authority);\n" \
  "PRAGMA user_version = 5"

/* NOTE - added seq INTEGER PRIMARY KEY AUTOINCREMENT and UNIQUE (id) but not
          included in upgrade from version 2 below because its a new way of
	  generating a primary key */

#define DUPIN_LINKB_SQL_DESC_UPGRADE_FROM_VERSION_2 \
  "ALTER TABLE Dupin      ADD COLUMN authority   TEXT DEFAULT NULL;\n" \
  "ALTER TABLE Dupin      ADD COLUMN expire_tm   INTEGER NOT NULL DEFAULT 0;\n" \
  "DROP  INDEX IF EXISTS  DupinHrefDeletedTag;\n" \
  "CREATE INDEX IF NOT EXISTS DupinHrefDeletedAuthority ON Dupin (href,deleted,authority);\n" \
  "PRAGMA user_version = 5"

#define DUPIN_LINKB_SQL_DESC_UPGRADE_FROM_VERSION_3 \
  "ALTER TABLE Dupin      ADD COLUMN authority   TEXT DEFAULT NULL;\n" \
  "ALTER TABLE Dupin      ADD COLUMN expire_tm   INTEGER NOT NULL DEFAULT 0;\n" \
  "DROP  INDEX IF EXISTS  DupinHrefDeletedTag;\n" \
  "CREATE INDEX IF NOT EXISTS DupinHrefDeletedAuthority ON Dupin (href,deleted,authority);\n" \
  "PRAGMA user_version = 5"

#define DUPIN_LINKB_SQL_DESC_UPGRADE_FROM_VERSION_4 \
  "ALTER TABLE Dupin      ADD COLUMN authority   TEXT DEFAULT NULL;\n" \
  "DROP  INDEX IF EXISTS  DupinHrefDeletedTag;\n" \
  "CREATE INDEX IF NOT EXISTS DupinHrefDeletedAuthority ON Dupin (href,deleted,authority);\n" \
  "PRAGMA user_version = 5"

#define DUPIN_LINKB_SQL_USES_OLD_ROWID \
        "SELECT seq FROM Dupin"

#define DUPIN_LINKB_SQL_TOTAL \
        "SELECT count(*) AS c FROM Dupin AS d WHERE d.rev_head = 'TRUE' "

#define DUPIN_LINKB_COMPACT_COUNT 1000
#define DUPIN_LINKB_CHECK_COUNT   1000

gchar **
dupin_get_linkbases (Dupin * d)
{
  guint i;
  gsize size;
  gchar **ret;
  gpointer key;
  GHashTableIter iter;

  g_return_val_if_fail (d != NULL, NULL);

  g_rw_lock_reader_lock (d->rwlock);

  if (!(size = g_hash_table_size (d->linkbs)))
    {
      g_rw_lock_reader_unlock (d->rwlock);
      return NULL;
    }

  ret = g_malloc (sizeof (gchar *) * (g_hash_table_size (d->linkbs) + 1));

  i = 0;
  g_hash_table_iter_init (&iter, d->linkbs);
  while (g_hash_table_iter_next (&iter, &key, NULL) == TRUE)
    ret[i++] = g_strdup (key);

  ret[i] = NULL;

  g_rw_lock_reader_unlock (d->rwlock);

  return ret;
}

gboolean
dupin_linkbase_exists (Dupin * d, gchar * linkb_name)
{
  gboolean ret;

  g_rw_lock_reader_lock (d->rwlock);
  DupinLinkB * linkb = g_hash_table_lookup (d->linkbs, linkb_name); 
  ret = ((linkb != NULL) && linkb->todelete == FALSE) ? TRUE : FALSE;
  g_rw_lock_reader_unlock (d->rwlock);

  return ret;
}

DupinLinkB *
dupin_linkbase_open (Dupin * d, gchar * linkb, GError ** error)
{
  DupinLinkB *ret;

  g_return_val_if_fail (d != NULL, NULL);
  g_return_val_if_fail (linkb != NULL, NULL);

  g_rw_lock_reader_lock (d->rwlock);

  if (!(ret = g_hash_table_lookup (d->linkbs, linkb)) || ret->todelete == TRUE)
    {
      if (error != NULL && *error != NULL)
        g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN,
		 "Linkbase '%s' doesn't exist.", linkb);

      g_rw_lock_reader_unlock (d->rwlock);
      return NULL;
    }
  else
    {
      ret->ref++;

#if DEBUG
      fprintf(stderr,"dupin_linkbase_open: (%p) name=%s \t ref++=%d\n", g_thread_self (), linkb, (gint) ret->ref);
#endif
    }

  g_rw_lock_reader_unlock (d->rwlock);

  return ret;
}

DupinLinkB *
dupin_linkbase_new (Dupin * d,
		    gchar * linkb,
		    gchar * parent,
		    gboolean parent_is_db,
		    GError ** error)
{
  DupinLinkB *ret;
  gchar *path;
  gchar *name;
  gchar * str;
  gchar * errmsg;

  g_return_val_if_fail (d != NULL, NULL);
  g_return_val_if_fail (linkb != NULL, NULL);
  g_return_val_if_fail (parent != NULL, NULL);
  g_return_val_if_fail (dupin_util_is_valid_linkb_name (linkb) == TRUE, NULL);

  if (parent_is_db == TRUE)
    g_return_val_if_fail (dupin_database_exists (d, parent) == TRUE, NULL);
  else
    return NULL;

  g_rw_lock_writer_lock (d->rwlock);

  if ((ret = g_hash_table_lookup (d->linkbs, linkb)))
    {
      if (error != NULL && *error != NULL)
        g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN,
		   "Linkbase '%s' already exist.", linkb);
      g_rw_lock_writer_unlock (d->rwlock);
      return NULL;
    }

  name = g_strdup_printf ("%s%s", linkb, DUPIN_LINKB_SUFFIX);
  path = g_build_path (G_DIR_SEPARATOR_S, d->path, name, NULL);
  g_free (name);

  if (!(ret = dupin_linkb_connect (d, linkb, path, DP_SQLITE_OPEN_CREATE, error)))
    {
      g_rw_lock_writer_unlock (d->rwlock);
      g_free (path);
      return NULL;
    }

  g_free (path);

  ret->parent = g_strdup (parent);
  ret->parent_is_db = parent_is_db;

  ret->ref++;

#if DEBUG
  fprintf(stderr,"dupin_linkbase_new: (%p) name=%s \t ref++=%d\n", g_thread_self (), linkb, (gint) ret->ref);
#endif

  if (dupin_linkbase_begin_transaction (ret, error) < 0)
    {
      g_rw_lock_writer_unlock (d->rwlock);
      dupin_linkb_disconnect (ret);
      return NULL;
    }

  gchar * creation_time = g_strdup_printf ("%" G_GSIZE_FORMAT, dupin_date_timestamp_now (0));

  str = sqlite3_mprintf ("INSERT OR REPLACE INTO DupinLinkB "
                         "(parent, isdb, compact_id, check_id, creation_time) "
                         "VALUES('%q', '%s', 0, 0, '%q')", parent, parent_is_db ? "TRUE" : "FALSE", creation_time);

  g_free (creation_time);

  if (sqlite3_exec (ret->db, str, NULL, NULL, &errmsg) != SQLITE_OK)
    {
      g_rw_lock_writer_unlock (d->rwlock);

      if (error != NULL && *error != NULL)
        g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN, "%s", errmsg);

      sqlite3_free (errmsg);
      sqlite3_free (str);
      dupin_linkbase_rollback_transaction (ret, error);
      dupin_linkb_disconnect (ret);
      return NULL;
    }

  if (dupin_linkbase_commit_transaction (ret, error) < 0)
    {
      g_rw_lock_writer_unlock (d->rwlock);
      dupin_linkb_disconnect (ret);
      return NULL;
    }

  sqlite3_free (str);

  g_rw_lock_writer_unlock (d->rwlock);

  if (dupin_linkbase_p_update (ret, error) == FALSE)
    {
      dupin_linkb_disconnect (ret);
      return NULL;
    }

  g_rw_lock_writer_lock (d->rwlock);
  g_hash_table_insert (d->linkbs, g_strdup (linkb), ret);
  g_rw_lock_writer_unlock (d->rwlock);

  return ret;
}

static int
dupin_linkbase_p_update_cb (void *data, int argc, char **argv, char **col)
{
  struct dupin_linkbase_p_update_t *update = data;

  if (argv[0] && *argv[0])
    update->parent = g_strdup (argv[0]);

  if (argv[1] && *argv[1])
    update->isdb = !g_strcmp0 (argv[1], "TRUE") ? TRUE : FALSE;

  return 0;
}

#define DUPIN_LINKBASE_P_SIZE       64

static void
dupin_linkbase_p_update_real (DupinLinkBP * p,
			      DupinLinkB * linkb)
{
  g_rw_lock_reader_lock (linkb->rwlock);
  gboolean todelete = linkb->todelete;
  g_rw_lock_reader_unlock (linkb->rwlock);

  if (todelete == TRUE)
    {
      if (p->linkbs != NULL)
        {
          /* NOTE - need to remove pointer from parent if linkb is "hot deleted" */

          DupinLinkB ** linkbs = g_malloc (sizeof (DupinLinkB *) * p->size);

          gint i;
          gint current_numb = p->numb;
          p->numb = 0;
          for (i=0; i < current_numb ; i++)
            { 
              if (p->linkbs[i] != linkb)
                { 
                  linkbs[p->numb] = p->linkbs[i];
                  p->numb++;
                } 
            } 
          g_free (p->linkbs);
          p->linkbs = linkbs;
        }

      return;
    }

  if (p->linkbs == NULL)
    {
      p->linkbs = g_malloc (sizeof (DupinLinkB *) * DUPIN_LINKBASE_P_SIZE);
      p->size = DUPIN_LINKBASE_P_SIZE;
    }

  else if (p->numb == p->size)
    {
      p->size += DUPIN_LINKBASE_P_SIZE;
      p->linkbs = g_realloc (p->linkbs, sizeof (DupinLinkB *) * p->size);
    }

  p->linkbs[p->numb] = linkb;
  p->numb++;
}

gboolean
dupin_linkbase_p_update (DupinLinkB * linkb, GError ** error)
{
  gchar *errmsg;
  gchar *query = "SELECT parent, isdb FROM DupinLinkB LIMIT 1";

  struct dupin_linkbase_p_update_t update;
  memset (&update, 0, sizeof (struct dupin_linkbase_p_update_t));

  if (sqlite3_exec (linkb->db, query, dupin_linkbase_p_update_cb, &update, &errmsg)
      != SQLITE_OK)
    {
      if (error != NULL && *error != NULL)
        g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN, "%s", errmsg);
      sqlite3_free (errmsg);
      return FALSE;
    }

  if (!update.parent)
    {
      if (error != NULL && *error != NULL)
        g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN,
                   "Internal error.");
      return FALSE;
    }

  if (update.isdb == TRUE)
    {
      g_rw_lock_reader_lock (linkb->rwlock);
      gboolean todelete = linkb->todelete;
      g_rw_lock_reader_unlock (linkb->rwlock);

      gboolean parent_exists = dupin_database_exists (linkb->d, update.parent);

      if (parent_exists == TRUE)
        {
          DupinDB * db = NULL;

	  if (!(db = dupin_database_open (linkb->d, update.parent, error)))
            {
              g_free (update.parent);
              return FALSE;
            }

          g_rw_lock_writer_lock (db->rwlock);
          dupin_linkbase_p_update_real (&db->linkbs, linkb);
          g_rw_lock_writer_unlock (db->rwlock);

          dupin_database_unref (db);
        }

        /* NOTE - effectively ignore if parent has gone before the linkbase due they co-reference */

      else if (todelete == FALSE)
        {
          g_free (update.parent);
          return FALSE;
        }
    }
  else
    {
      g_warning ("dupin_linkbase_p_update() parent type unimplemented");

      g_free (update.parent);
      return FALSE;
    }

  /* make sure parameters are set after dupin server restart on existing link base */

  if (linkb->parent == NULL)
    linkb->parent = update.parent;
  else
    g_free (update.parent);

  linkb->parent_is_db = update.isdb;

  return TRUE;
}

void
dupin_linkbase_p_record_insert (DupinLinkBP * p,
				gchar * id,
				JsonObject * obj)
{
  gsize i;

  for (i = 0; i < p->numb; i++)
    {
      DupinLinkB *linkb = p->linkbs[i];

      dupin_linkbase_p_record_insert (&linkb->linkbs, id, obj);
    }
}

void
dupin_linkbase_p_record_delete (DupinLinkBP * p,
				gchar * pid)
{
  gsize i;

  for (i = 0; i < p->numb; i++)
    {
      DupinLinkB *linkb = p->linkbs[i];

      dupin_linkbase_p_record_delete (&linkb->linkbs, pid);

      /* TODO - delete any link where 'pid' is context_id or href of links; and viceversa */
    }
}

void
dupin_linkbase_ref (DupinLinkB * linkb)
{
  g_return_if_fail (linkb != NULL);

  g_rw_lock_writer_lock (linkb->rwlock);

  linkb->ref++;

#if DEBUG
  fprintf(stderr,"dupin_linkbase_ref: (%p) name=%s \t ref++=%d\n", g_thread_self (), linkb->name, (gint) linkb->ref);
#endif

  g_rw_lock_writer_unlock (linkb->rwlock);
}

void
dupin_linkbase_unref (DupinLinkB * linkb)
{
  Dupin *d;

  g_return_if_fail (linkb != NULL);

  d = linkb->d;

  g_rw_lock_writer_lock (linkb->rwlock);

  if (linkb->ref > 0)
    {
      linkb->ref--;

#if DEBUG
      fprintf(stderr,"dupin_linkbase_unref: (%p) name=%s \t ref--=%d\n", g_thread_self (), linkb->name, (gint) linkb->ref);
#endif
    }

  g_rw_lock_writer_unlock (linkb->rwlock);

  if (linkb->todelete == TRUE &&
      dupin_linkbase_is_compacting (linkb) == FALSE &&
      dupin_linkbase_is_checking (linkb) == FALSE)
    {
      if (linkb->ref > 0)
        {
          g_warning ("dupin_linkbase_unref: (thread=%p) linkbase %s flagged for deletion but can't free it due ref is %d\n", g_thread_self (), linkb->name, (gint) linkb->ref);
        }
      else
        {
	  if (dupin_linkbase_p_update (linkb, NULL) == FALSE)
            {
              g_warning("dupin_linkbase_unref: could not remove reference from parent for linkbase '%s'\n", linkb->name);
            }

          g_rw_lock_writer_lock (d->rwlock);
          g_hash_table_remove (d->linkbs, linkb->name);
          g_rw_lock_writer_unlock (d->rwlock);
        }
    }
}

gboolean
dupin_linkbase_delete (DupinLinkB * linkb, GError ** error)
{
  g_return_val_if_fail (linkb != NULL, FALSE);

  g_rw_lock_writer_lock (linkb->rwlock);
  linkb->todelete = TRUE;
  g_rw_lock_writer_unlock (linkb->rwlock);

  return TRUE;
}

const gchar *
dupin_linkbase_get_name (DupinLinkB * linkb)
{
  g_return_val_if_fail (linkb != NULL, NULL);
  return linkb->name;
}

gsize
dupin_linkbase_get_size (DupinLinkB * linkb)
{
  struct stat st;

  g_return_val_if_fail (linkb != NULL, 0);

  if (g_stat (linkb->path, &st) != 0)
    return 0;

  return (gsize) st.st_size;
}

static int
dupin_linkbase_get_creation_time_cb (void *data, int argc, char **argv, char **col)
{
  gsize *creation_time = data;

  if (argv[0])
    *creation_time = (gsize) g_ascii_strtoll (argv[0], NULL, 10);

  return 0;
}

gboolean
dupin_linkbase_get_creation_time (DupinLinkB * linkb, gsize * creation_time)
{
  gchar * query;
  gchar * errmsg=NULL;

  g_return_val_if_fail (linkb != NULL, 0);

  *creation_time = 0;

  /* get creation time out of linkbase */
  query = "SELECT creation_time as creation_time FROM DupinLinkB";

  if (sqlite3_exec (linkb->db, query, dupin_linkbase_get_creation_time_cb, creation_time, &errmsg) != SQLITE_OK)
    {
      g_error("dupin_linkbase_get_creation_time: %s", errmsg);
      sqlite3_free (errmsg);

      return FALSE;
    }

  return TRUE;
}

gchar *
dupin_linkbase_generate_id_real (DupinLinkB * linkb,
				 GError ** error,
				 gboolean lock)
{
  g_return_val_if_fail (linkb != NULL, NULL);

  while (TRUE)
    { 
      gchar * id = NULL;

      id = dupin_util_generate_id (error);

      if (id != NULL)
        {   
          if (dupin_link_record_exists_real (linkb, id, FALSE) == TRUE)
            { 
              g_free (id);
            }
          else
            { 
              return id;
            }
        }
      else
        break;
    }

  return NULL;
}

gchar *
dupin_linkbase_generate_id (DupinLinkB * linkb, GError ** error)
{
  g_return_val_if_fail (linkb != NULL, NULL);

 return dupin_linkbase_generate_id_real (linkb, error, TRUE);
}

gchar *
dupin_linkbase_get_parent (DupinLinkB * linkb)
{
  g_return_val_if_fail (linkb != NULL, NULL);

  return linkb->parent;
}

gboolean
dupin_linkbase_get_parent_is_db (DupinLinkB * linkb)
{
  g_return_val_if_fail (linkb != NULL, FALSE);

  return linkb->parent_is_db;
}

/* Internal: */
void
dupin_linkb_disconnect (DupinLinkB * linkb)
{
  g_return_if_fail (linkb != NULL);

#if DEBUG
  g_message("dupin_linkb_disconnect: total number of changes for '%s' linkbase: %d\n", linkb->name, (gint)sqlite3_total_changes (linkb->db));
#endif

  if (linkb->db)
    sqlite3_close (linkb->db);

  if (linkb->todelete == TRUE)
    g_unlink (linkb->path);

  if (linkb->name)
    g_free (linkb->name);

  if (linkb->path)
    g_free (linkb->path);

  if (linkb->parent)
    g_free (linkb->parent);

  if (linkb->rwlock)
    {
      g_rw_lock_clear (linkb->rwlock);
      g_free (linkb->rwlock);
    }

  if (linkb->views.views)
    g_free (linkb->views.views);

  if (linkb->linkbs.linkbs)
    g_free (linkb->linkbs.linkbs);

  if (linkb->error_msg)
    g_free (linkb->error_msg);

  if (linkb->warning_msg)
    g_free (linkb->warning_msg);

  g_free (linkb);
}

static int
dupin_linkb_get_user_version_cb (void *data, int argc, char **argv,
                                    char **col)
{
  gint *user_version = data;

  if (argv[0])
    *user_version = atoi (argv[0]);

  return 0;
}

DupinLinkB *
dupin_linkb_connect (Dupin * d, gchar * name, gchar * path,
		     DupinSQLiteOpenType mode,
		     GError ** error)
{
  gchar *errmsg;
  DupinLinkB *linkb;

  linkb = g_malloc0 (sizeof (DupinLinkB));

  linkb->d = d;

  linkb->name = g_strdup (name);
  linkb->path = g_strdup (path);

  linkb->tocompact = FALSE;
  linkb->topurge = FALSE;
  linkb->compact_processed_count = 0;

  linkb->tocheck = FALSE;
  linkb->check_processed_count = 0;

  linkb->rwlock = g_new0 (GRWLock, 1);
  g_rw_lock_init (linkb->rwlock);

  if (sqlite3_open_v2 (linkb->path, &linkb->db, dupin_util_dupin_mode_to_sqlite_mode (mode), NULL) != SQLITE_OK)
    {
      if (error != NULL && *error != NULL)
        g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN,
		   "Linkbase error.");
      dupin_linkb_disconnect (linkb);
      return NULL;
    }

  sqlite3_busy_timeout (linkb->db, DUPIN_SQLITE_TIMEOUT);

  if (mode == DP_SQLITE_OPEN_CREATE)
    {
      if (sqlite3_exec (linkb->db, "PRAGMA journal_mode = WAL", NULL, NULL, &errmsg) != SQLITE_OK
          || sqlite3_exec (linkb->db, "PRAGMA encoding = \"UTF-8\"", NULL, NULL, &errmsg) != SQLITE_OK)
        {
          if (error != NULL && *error != NULL)
            g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN, "Cannot set pragma journal_mode or encoding: %s",
		   errmsg);
          sqlite3_free (errmsg);
          dupin_linkb_disconnect (linkb);
          return NULL;
        }

      if (dupin_linkbase_begin_transaction (linkb, error) < 0)
        {
          dupin_linkb_disconnect (linkb);
          return NULL;
        }

      if (sqlite3_exec (linkb->db, DUPIN_LINKB_SQL_MAIN_CREATE, NULL, NULL, &errmsg) != SQLITE_OK
          || sqlite3_exec (linkb->db, DUPIN_LINKB_SQL_CREATE_INDEX, NULL, NULL, &errmsg) != SQLITE_OK
          || sqlite3_exec (linkb->db, DUPIN_LINKB_SQL_DESC_CREATE, NULL, NULL, &errmsg) != SQLITE_OK)
        {
          if (error != NULL && *error != NULL)
            g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN, "%s",
		   errmsg);
          sqlite3_free (errmsg);
          dupin_linkbase_rollback_transaction (linkb, error);
          dupin_linkb_disconnect (linkb);
          return NULL;
        }

      if (dupin_linkbase_commit_transaction (linkb, error) < 0)
        {
          dupin_linkb_disconnect (linkb);
          return NULL;
        }
    }

  /* check linkbase version */
  gint user_version = 0;

  if (sqlite3_exec (linkb->db, "PRAGMA user_version", dupin_linkb_get_user_version_cb, &user_version, &errmsg) != SQLITE_OK)
    {
      /* default to 1 if not found or error - TODO check not SQLITE_OK error only */
      user_version = 1;
    }

  if (user_version > DUPIN_SQLITE_MAX_USER_VERSION)
    {
      if (error != NULL && *error != NULL)
        g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN, "SQLite linkbase user version (%d) is newer than I know how to work with (%d).",
			user_version, DUPIN_SQLITE_MAX_USER_VERSION);
      sqlite3_free (errmsg);
      dupin_linkb_disconnect (linkb);
      return NULL;
    }

  if (user_version <= 1)
    {
      if (sqlite3_exec (linkb->db, DUPIN_LINKB_SQL_DESC_UPGRADE_FROM_VERSION_1, NULL, NULL, &errmsg) != SQLITE_OK)
        {
          if (error != NULL && *error != NULL)
            g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN, "%s",
                   errmsg);
          sqlite3_free (errmsg);
          dupin_linkb_disconnect (linkb);
          return NULL;
        }
    }
  else if (user_version == 2)
    {
      if (sqlite3_exec (linkb->db, DUPIN_LINKB_SQL_DESC_UPGRADE_FROM_VERSION_2, NULL, NULL, &errmsg) != SQLITE_OK)
        {
	  if (error != NULL && *error != NULL)
            g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN, "%s",
                   errmsg);
          sqlite3_free (errmsg);
          dupin_linkb_disconnect (linkb);
          return NULL;
        }
    }
  else if (user_version == 3)
    {
      if (sqlite3_exec (linkb->db, DUPIN_LINKB_SQL_DESC_UPGRADE_FROM_VERSION_3, NULL, NULL, &errmsg) != SQLITE_OK)
        {
	  if (error != NULL && *error != NULL)
            g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN, "%s",
                   errmsg);
          sqlite3_free (errmsg);
          dupin_linkb_disconnect (linkb);
          return NULL;
        }
    }
  else if (user_version == 4)
    {
      if (sqlite3_exec (linkb->db, DUPIN_LINKB_SQL_DESC_UPGRADE_FROM_VERSION_4, NULL, NULL, &errmsg) != SQLITE_OK)
        {
	  if (error != NULL && *error != NULL)
            g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN, "%s",
                   errmsg);
          sqlite3_free (errmsg);
          dupin_linkb_disconnect (linkb);
          return NULL;
        }
    }

  if (sqlite3_exec (linkb->db, DUPIN_LINKB_SQL_USES_OLD_ROWID, NULL, NULL, &errmsg) != SQLITE_OK)
    {   
      if (error != NULL && *error != NULL)
        g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN, "%s", errmsg);
      sqlite3_free (errmsg);

      g_warning ("dupin_linkb_connect: Consider to recreate your %s SQLite database and reingest your data. Since version 3 the Dupin table uses a seq column INTEGER PRIMARY KEY AUTOINCREMENT as ROWID and UNIQUE (id, rev) constraint rather then PRIMARY KEY(id, rev). See http://www.sqlite.org/autoinc.html for more information.\n", path);
    }

  gchar * cache_size = g_strdup_printf ("PRAGMA cache_size = %d", DUPIN_SQLITE_CACHE_SIZE);
  if (sqlite3_exec (linkb->db, "PRAGMA temp_store = memory", NULL, NULL, &errmsg) != SQLITE_OK
      || sqlite3_exec (linkb->db, cache_size, NULL, NULL, &errmsg) != SQLITE_OK)
    {   
      if (error != NULL && *error != NULL)
        g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN, "Cannot set pragma temp_store: %s",
                   errmsg);
      sqlite3_free (errmsg);
      if (cache_size)
        g_free (cache_size);
      dupin_linkb_disconnect (linkb);
      return NULL;
    }

  if (cache_size)
    g_free (cache_size);

  /*
   TODO - check if the below can be optimized using NORMAL or OFF and use separated syncing thread
          see also http://www.sqlite.org/pragma.html#pragma_synchronous
   */

  if (sqlite3_exec (linkb->db, "PRAGMA synchronous = NORMAL", NULL, NULL, &errmsg) != SQLITE_OK)
    {   
      if (error != NULL && *error != NULL)
        g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN, "Cannot set pragma synchronous: %s",
                   errmsg);
      sqlite3_free (errmsg);
      dupin_linkb_disconnect (linkb);
      return NULL;
    }

  /* NOTE - we know this is inefficient, but we need it till proper Elastic search or lucene used as frontend */

  sqlite3_create_function(linkb->db, "filterBy", 5, SQLITE_ANY, d, dupin_sqlite_json_filterby, NULL, NULL);

  return linkb;
}

/* NOTE - 0 = ok, 1 = already in transaction, -1 = error */

gint
dupin_linkbase_begin_transaction (DupinLinkB * linkb, GError ** error)
{
  g_return_val_if_fail (linkb != NULL, -1);

  gchar *errmsg;
  gint rc = -1;

  if (linkb->d->bulk_transaction == TRUE)
    {
#if DEBUG
      g_message ("dupin_linkbase_begin_transaction: linkbase %s transaction ALREADY open", linkb->name);
#endif

      return 1;
    }

  rc = sqlite3_exec (linkb->db, "BEGIN TRANSACTION", NULL, NULL, &errmsg);

  if (rc == SQLITE_BUSY)
    {
        rc = dupin_sqlite_subs_mgr_busy_handler(linkb->db, "BEGIN TRANSACTION", NULL, NULL, &errmsg, rc);
    }

  if (rc != SQLITE_OK)
    {
      if (error != NULL && *error != NULL)
        g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN, "Cannot begin linkbase %s transaction: %s", linkb->name, errmsg);

      sqlite3_free (errmsg);

      return -1;
    }

#if DEBUG
  g_message ("dupin_linkbase_begin_transaction: linkbase %s transaction begin", linkb->name);
#endif

  return 0;
}

gint
dupin_linkbase_rollback_transaction (DupinLinkB * linkb, GError ** error)
{
  g_return_val_if_fail (linkb != NULL, -1);

  gchar *errmsg;
  gint rc = -1;

  rc = sqlite3_exec (linkb->db, "ROLLBACK", NULL, NULL, &errmsg);

  if (rc == SQLITE_BUSY)
    {
        rc = dupin_sqlite_subs_mgr_busy_handler(linkb->db, "ROLLBACK", NULL, NULL, &errmsg, rc);
    }

  if (rc != SQLITE_OK)
    {
      if (error != NULL && *error != NULL)
        g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN, "Cannot rollback linkbase %s transaction: %s", linkb->name, errmsg);

      sqlite3_free (errmsg);

      return -1;
    }

#if DEBUG
  g_message ("dupin_linkbase_rollback_transaction: linkbase %s transaction rollback", linkb->name);
#endif

  return 0;
}

gint
dupin_linkbase_commit_transaction (DupinLinkB * linkb, GError ** error)
{
  g_return_val_if_fail (linkb != NULL, -1);

  gchar *errmsg;
  gint rc = -1;

  if (linkb->d->bulk_transaction == TRUE)
    {
#if DEBUG
      g_message ("dupin_linkbase_commit_transaction: linkbase %s transaction commit POSTPONED", linkb->name);
#endif

      return 1;
    }

  rc = sqlite3_exec (linkb->db, "COMMIT", NULL, NULL, &errmsg);

  if (rc == SQLITE_BUSY)
    {
        rc = dupin_sqlite_subs_mgr_busy_handler(linkb->db, "COMMIT", NULL, NULL, &errmsg, rc);
    }

  if (rc != SQLITE_OK)
    {
      if (error != NULL && *error != NULL)
        g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN, "Cannot commit linkbase %s transaction: %s", linkb->name, errmsg);

      sqlite3_free (errmsg);

      return -1;
    }

#if DEBUG
  g_message ("dupin_linkbase_commit_transaction: linkbase %s transaction commit", linkb->name);
#endif

  return 0;
}

gsize
dupin_linkbase_count (DupinLinkB * linkb,
		      DupinLinksType links_type,
	              DupinCountType count_type)
{
  g_return_val_if_fail (linkb != NULL, 0);

  struct dupin_link_record_select_total_t count;
  memset (&count, 0, sizeof (count));

  if (sqlite3_exec (linkb->db, DUPIN_LINKB_SQL_GET_TOTALS, dupin_link_record_select_total_cb, &count, NULL) !=
      SQLITE_OK)
    {
      return 0;
    }

  if (count_type == DP_COUNT_EXIST)
    {
      if (links_type == DP_LINK_TYPE_WEB_LINK)
        return count.total_webl_ins;

      else if (links_type == DP_LINK_TYPE_RELATIONSHIP)
        return count.total_rel_ins;

      else
        return count.total_webl_ins + count.total_rel_ins;
    }
  else if (count_type == DP_COUNT_DELETE)
    {
      if (links_type == DP_LINK_TYPE_WEB_LINK)
        return count.total_webl_del;

      else if (links_type == DP_LINK_TYPE_RELATIONSHIP)
        return count.total_rel_del;

      else
        return count.total_webl_del + count.total_rel_del;
    }
  else if (count_type == DP_COUNT_CHANGES)
    {
      return count.total_webl_ins + count.total_rel_ins + count.total_webl_del + count.total_rel_del;
    }
  else
    {
      return count.total_webl_ins + count.total_rel_ins + count.total_webl_del + count.total_rel_del;
    }
}

static int
dupin_linkbase_get_max_rowid_cb (void *data, int argc, char **argv,
                                  char **col)
{
  gsize *max_rowid = data;

  if (argv[0])
    *max_rowid = (gsize) g_ascii_strtoll (argv[0], NULL, 10);

  return 0;
}

gboolean
dupin_linkbase_get_max_rowid (DupinLinkB * linkb, gsize * max_rowid)
{
  gchar *query;
  gchar * errmsg=NULL;

  g_return_val_if_fail (linkb != NULL, 0);

  *max_rowid=0;

  query = "SELECT max(ROWID) as max_rowid FROM Dupin";

  if (sqlite3_exec (linkb->db, query, dupin_linkbase_get_max_rowid_cb, max_rowid, &errmsg) !=
      SQLITE_OK)
    {
      g_error("dupin_linkbase_get_max_rowid: %s", errmsg);
      sqlite3_free (errmsg);

      return FALSE;
    }

  return TRUE;
}

static int
dupin_linkbase_get_changes_list_cb (void *data, int argc, char **argv, char **col)
{
  struct dupin_linkbase_get_changes_list_t *s = data;

  guint rev = 0;
  gsize tm = 0;
  gsize expire_tm = 0;
  gchar *id = NULL;
  gchar *hash = NULL;
  gchar *obj = NULL;
  gboolean delete = FALSE;
  gsize rowid=0;
  gint i;
  gchar *context_id = NULL;
  gboolean is_weblink = FALSE;
  gchar *href = NULL;
  gchar *rel = NULL;
  gchar *authority = NULL; 
  gchar *label = NULL; 

  for (i = 0; i < argc; i++)
    {
      /* shouldn't this be double and use g_ascii_strtoll() ?!? */
      if (!g_strcmp0 (col[i], "rev"))
        rev = atoi (argv[i]);

      else if (!g_strcmp0 (col[i], "tm"))
        tm = (gsize) g_ascii_strtoll (argv[i], NULL, 10);

      else if (!g_strcmp0 (col[i], "expire_tm"))
        expire_tm = (gsize) g_ascii_strtoll (argv[i], NULL, 10);

      else if (!g_strcmp0 (col[i], "hash"))
        hash = argv[i];

      else if (!g_strcmp0 (col[i], "obj"))
        obj = argv[i];

      else if (!g_strcmp0 (col[i], "deleted"))
        delete = !g_strcmp0 (argv[i], "TRUE") ? TRUE : FALSE;

      else if (!g_strcmp0 (col[i], "rowid"))
        rowid = (gsize) g_ascii_strtoll (argv[i], NULL, 10);

      else if (!g_strcmp0 (col[i], "id"))
        id = argv[i];

      else if (!g_strcmp0 (col[i], "context_id"))
        context_id = argv[i];

      else if (!g_strcmp0 (col[i], "href"))
        href = argv[i];

      else if (!g_strcmp0 (col[i], "rel"))
        rel = argv[i];

      else if (!g_strcmp0 (col[i], "is_weblink"))
        is_weblink = !g_strcmp0 (argv[i], "TRUE") ? TRUE : FALSE;

      else if (!g_strcmp0 (col[i], "authority"))
        authority = argv[i];

      else if (!g_strcmp0 (col[i], "label"))
        label = argv[i];
    }

  if (rev && hash !=NULL)
    {
      JsonNode *change_node=json_node_new (JSON_NODE_OBJECT);
      JsonObject *change=json_object_new();
      json_node_take_object (change_node, change);

      json_object_set_int_member (change,"seq", rowid);
      json_object_set_string_member (change,"id", id);

      if (delete == TRUE)
        json_object_set_boolean_member (change, "deleted", delete);

      JsonNode *change_details_node=json_node_new (JSON_NODE_ARRAY);
      JsonArray *change_details=json_array_new();
      json_node_take_array (change_details_node, change_details);
      json_object_set_member (change, "changes", change_details_node);

      JsonNode * node = json_node_new (JSON_NODE_OBJECT);
      JsonObject * node_obj = json_object_new ();
      json_node_take_object (node, node_obj);
      json_array_add_element (change_details, node);  

      gchar mvcc[DUPIN_ID_MAX_LEN];
      dupin_util_mvcc_new (rev, hash, mvcc);

      json_object_set_string_member (node_obj, "rev", mvcc);
      gchar * created = dupin_date_timestamp_to_http_date (tm);
      json_object_set_string_member (node_obj, RESPONSE_OBJ_CREATED, created);
      g_free (created);
      if (expire_tm != 0)
        {
          gchar * expire = dupin_date_timestamp_to_http_date (expire_tm);
          json_object_set_string_member (node_obj, RESPONSE_OBJ_EXPIRE, expire);
          g_free (expire);
        }

      json_object_set_string_member (node_obj, "context_id", context_id);
      json_object_set_string_member (node_obj, "label", label);
      json_object_set_string_member (node_obj, "href", href);
      if (rel != NULL)
        json_object_set_string_member (node_obj, "rel", rel);
      if (is_weblink == TRUE)
        json_object_set_boolean_member (change, "is_weblink", is_weblink);
      if (authority != NULL)
        json_object_set_string_member (node_obj, "authority", authority);

      if (s->style == DP_CHANGES_MAIN_ONLY)
        {
        }
      else if (s->style == DP_CHANGES_ALL_DOCS)
        {
        }

      s->list = g_list_append (s->list, change_node);
    }

  return 0;
}

gboolean
dupin_linkbase_get_changes_list (DupinLinkB *              linkb,
                                 guint                  count,
                                 guint                  offset,
                                 gsize                  since,
                                 gsize                  to,
         			 DupinChangesType	changes_type,
				 DupinCountType         count_type,
                                 DupinOrderByType       orderby_type,
                                 gboolean               descending,
				 gchar *		context_id,
				 gchar **               authorities,
				 DupinFilterByType      authorities_type,
                                 GList **               list,
                                 GError **              error)
{
  GString *str;
  gchar *tmp;
  gchar *errmsg;
  gchar *check_deleted="";
  gchar *check_linktype="";

  struct dupin_linkbase_get_changes_list_t s;

  g_return_val_if_fail (linkb != NULL, FALSE);
  g_return_val_if_fail (list != NULL, FALSE);

  if (context_id != NULL)
    g_return_val_if_fail (dupin_link_record_util_is_valid_context_id (context_id) == TRUE, FALSE);

  memset (&s, 0, sizeof (s));
  s.style = changes_type;

  str = g_string_new ("SELECT id, rev, hash, obj, deleted, tm, expire_tm, ROWID AS rowid, context_id, href, rel, is_weblink, authority, label FROM Dupin as d WHERE d.rev_head = 'TRUE' ");

  if (count_type == DP_COUNT_EXIST)
    check_deleted = " d.deleted = 'FALSE' ";
  else if (count_type == DP_COUNT_DELETE)
    check_deleted = " d.deleted = 'TRUE' ";

  if (changes_type == DP_CHANGES_WEB_LINKS)
    check_linktype = " d.is_weblink = 'TRUE' ";
  else if (changes_type == DP_CHANGES_RELATIONSHIPS)
    check_linktype = " d.is_weblink = 'FALSE' ";

  gchar * op = "AND";

  if (since > 0 && to > 0)
    {
      g_string_append_printf (str, " %s d.ROWID > %d AND d.ROWID <= %d ", op, (gint)since, (gint)to);
    }
  else if (since > 0)
    {
      g_string_append_printf (str, " %s d.ROWID > %d ", op, (gint)since);
    }
  else if (to > 0)
    {
      g_string_append_printf (str, " %s d.ROWID <= %d ", op, (gint)to);
    }

  if (context_id != NULL)
    {
      gchar * tmp2 = sqlite3_mprintf (" %s d.context_id = '%q' ", op, context_id);
      str = g_string_append (str, tmp2);
      sqlite3_free (tmp2);
    }

  if (g_strcmp0 (check_deleted, ""))
    {
      g_string_append_printf (str, " %s %s ", op, check_deleted);
    }

  if (g_strcmp0 (check_linktype, ""))
    {
      g_string_append_printf (str, " %s %s ", op, check_linktype);
    }

  if (authorities != NULL
      && authorities_type != DP_FILTERBY_PRESENT)
    {
      if (authorities[0])
        {
          gchar * tmp2 = sqlite3_mprintf (" %s ( ", op);
          str = g_string_append (str, tmp2);
          sqlite3_free (tmp2);
        }

      gint i;
      for (i = 0; authorities[i]; i++)
        {
          gchar * tmp2;

          if (authorities_type == DP_FILTERBY_EQUALS)
            tmp2 = sqlite3_mprintf (" d.authority = '%q' ", authorities[i]);
          else if (authorities_type == DP_FILTERBY_CONTAINS)
            tmp2 = sqlite3_mprintf (" d.authority LIKE '%%%q%%' ", authorities[i]);
          else if (authorities_type == DP_FILTERBY_STARTS_WITH)
            tmp2 = sqlite3_mprintf (" d.authority LIKE '%q%%' ", authorities[i]);

          str = g_string_append (str, tmp2);
          sqlite3_free (tmp2);
          if (authorities[i+1])
            str = g_string_append (str, " OR ");
        }

      if (authorities[0])
        str = g_string_append (str, " ) ");
    }
  else
    {
      if (authorities_type == DP_FILTERBY_PRESENT)
        {
          gchar * tmp2 = tmp2 = sqlite3_mprintf (" %s ( d.authority IS NOT NULL OR d.authority != '' ) ", op);
          str = g_string_append (str, tmp2);
          sqlite3_free (tmp2);
        }
    }

  //str = g_string_append (str, " GROUP BY d.id "); 

  str = g_string_append (str, " ORDER BY d.ROWID");

  if (descending)
    str = g_string_append (str, " DESC");

  if (count || offset)
    {
      str = g_string_append (str, " LIMIT ");

      if (offset)
        g_string_append_printf (str, "%u", offset);

      if (offset && count)
        str = g_string_append (str, ",");

      if (count)
        g_string_append_printf (str, "%u", count);
    }

  tmp = g_string_free (str, FALSE);

#if DEBUG
  g_message("dupin_linkbase_get_changes_list() query=%s\n",tmp);
#endif

  if (sqlite3_exec (linkb->db, tmp, dupin_linkbase_get_changes_list_cb, &s, &errmsg) !=
      SQLITE_OK)
    {
      if (error != NULL && *error != NULL)
        g_set_error (error, dupin_error_quark (), DUPIN_ERROR_CRUD, "%s",
                   errmsg);

      sqlite3_free (errmsg);
      g_free (tmp);
      return FALSE;
    }

  g_free (tmp);

  *list = s.list;
  return TRUE;
}

void
dupin_linkbase_get_changes_list_close
				(GList *                list)
{
  while (list)
    {
      if (list->data != NULL)
        json_node_free (list->data);
      list = g_list_remove (list, list->data);
    }
}

static int
dupin_linkbase_get_total_changes_cb (void *data, int argc, char **argv, char **col)
{
  gsize *numb = data;

  if (argv[0] && *argv[0])
    *numb = (gsize) g_ascii_strtoll (argv[0], NULL, 10);

  return 0;
}

gboolean
dupin_linkbase_get_total_changes
				(DupinLinkB *           linkb,
                                 gsize *                total,
                                 gsize                  since,
                                 gsize                  to,
         			 DupinChangesType	changes_type,
			 	 DupinCountType         count_type,
                                 gboolean               inclusive_end,
				 gchar *                context_id,
				 gchar **               authorities,
				 DupinFilterByType      authorities_type,
                                 GError **              error)
{
  g_return_val_if_fail (linkb != NULL, FALSE);

  gchar *errmsg;
  gchar *tmp;
  GString *str;

  if (context_id != NULL)
    g_return_val_if_fail (dupin_link_record_util_is_valid_context_id (context_id) == TRUE, FALSE);

  *total = 0;

  gchar *check_deleted="";
  gchar *check_linktype="";

  str = g_string_new (DUPIN_LINKB_SQL_TOTAL);

  if (count_type == DP_COUNT_EXIST)
    check_deleted = " d.deleted = 'FALSE' ";
  else if (count_type == DP_COUNT_DELETE)
    check_deleted = " d.deleted = 'TRUE' ";

  if (changes_type == DP_CHANGES_WEB_LINKS)
    check_linktype = " d.is_weblink = 'TRUE' ";
  else if (changes_type == DP_CHANGES_RELATIONSHIPS)
    check_linktype = " d.is_weblink = 'FALSE' ";

  gchar * op = "AND";

  if (since > 0 && to > 0)
    {
      g_string_append_printf (str, " %s d.ROWID >= %d AND d.ROWID <= %d ", op, (gint)since, (gint)to);
      op = "AND";
    }
  else if (since > 0)
    {
      g_string_append_printf (str, " %s d.ROWID >= %d ", op, (gint)since);
      op = "AND";
    }
  else if (to > 0)
    {
      g_string_append_printf (str, " %s d.ROWID <= %d ", op, (gint)to);
      op = "AND";
    }

  if (g_strcmp0 (check_deleted, ""))
    {
      if (!g_strcmp0 (op, ""))
        op = "WHERE";

      g_string_append_printf (str, " %s %s ", op, check_deleted);
      op = "AND";
    }

  if (g_strcmp0 (check_linktype, ""))
    {
      if (!g_strcmp0 (op, ""))
        op = "WHERE";

      g_string_append_printf (str, " %s %s ", op, check_linktype);
      op = "AND";
    }

  if (context_id != NULL)
    {
      if (!g_strcmp0 (op, ""))
        op = "WHERE";

      gchar * tmp2 = sqlite3_mprintf (" %s d.context_id = '%q' ", op, context_id);
      str = g_string_append (str, tmp2);
      sqlite3_free (tmp2);
      op = "AND";
    }

  if (authorities != NULL
      && authorities_type != DP_FILTERBY_PRESENT)
    {
      if (!g_strcmp0 (op, ""))
        op = "WHERE";

      if (authorities[0])
        {
          gchar * tmp2 = sqlite3_mprintf (" %s ( ", op);
          str = g_string_append (str, tmp2);
          sqlite3_free (tmp2);
        }

      gint i;
      for (i = 0; authorities[i]; i++)
        {
          gchar * tmp2;

          if (authorities_type == DP_FILTERBY_EQUALS)
            tmp2 = sqlite3_mprintf (" d.authority = '%q' ", authorities[i]);
          else if (authorities_type == DP_FILTERBY_CONTAINS)
            tmp2 = sqlite3_mprintf (" d.authority LIKE '%%%q%%' ", authorities[i]);
          else if (authorities_type == DP_FILTERBY_STARTS_WITH)
            tmp2 = sqlite3_mprintf (" d.authority LIKE '%q%%' ", authorities[i]);

          str = g_string_append (str, tmp2);
          sqlite3_free (tmp2);
          if (authorities[i+1])
            str = g_string_append (str, " OR ");
        }

      if (authorities[0])
        str = g_string_append (str, " ) ");
    }
  else
    {
      if (!g_strcmp0 (op, ""))
        op = "WHERE";

      if (authorities_type == DP_FILTERBY_PRESENT)
        {
          gchar * tmp2 = tmp2 = sqlite3_mprintf (" %s ( d.authority IS NOT NULL OR d.authority != '' ) ", op);
          str = g_string_append (str, tmp2);
          sqlite3_free (tmp2);
        }
    }

  // TODO - check if we need this group by - see above dupin_linkbase_get_changes_list()
  //str = g_string_append (str, " GROUP BY d.id "); 

  tmp = g_string_free (str, FALSE);

#if DEBUG
  g_message("dupin_linkbase_get_total_changes() query=%s\n",tmp);
#endif

  *total = 0;

  if (sqlite3_exec (linkb->db, tmp, dupin_linkbase_get_total_changes_cb, total, &errmsg) !=
      SQLITE_OK)
    {
      if (error != NULL && *error != NULL)
        g_set_error (error, dupin_error_quark (), DUPIN_ERROR_CRUD, "%s",
                   errmsg);

      sqlite3_free (errmsg);
      g_free (tmp);
      return FALSE;
    }

  g_free (tmp);

  return TRUE;
}

/* Linkbase compaction */

static int
dupin_linkbase_compact_cb (void *data, int argc, char **argv, char **col)
{
  gchar **compact_id = data;

  if (argv[0] && *argv[0])
    *compact_id = g_strdup (argv[0]);

  return 0;
}

gboolean
dupin_linkbase_thread_compact (DupinLinkB * linkb, gsize count)
{
  g_return_val_if_fail (linkb != NULL, FALSE);

  gchar * compact_id = NULL;
  gsize rowid;
  gchar * errmsg;
  GList *results, *list;

  g_rw_lock_reader_lock (linkb->rwlock);
  gboolean topurge = linkb->topurge;
  g_rw_lock_reader_unlock (linkb->rwlock);

  gboolean ret = TRUE;

  gchar *str;

  /* get last position we compacted and get anything up to count after that */

  gchar * query = "SELECT compact_id as c FROM DupinLinkB LIMIT 1";

  if (sqlite3_exec (linkb->db, query, dupin_linkbase_compact_cb, &compact_id, &errmsg) != SQLITE_OK)
    {
      g_error("dupin_linkbase_thread_compact: %s", errmsg);
      sqlite3_free (errmsg);

      return FALSE;
    }

  gsize start_rowid = (compact_id != NULL) ? (gsize) g_ascii_strtoll (compact_id, NULL, 10)+1 : 1;

  if (dupin_link_record_get_list (linkb, count, 0, start_rowid, 0, DP_LINK_TYPE_ANY, NULL, NULL, NULL, TRUE, DP_COUNT_ALL, DP_ORDERBY_ROWID, FALSE,
				  NULL, NULL, DP_FILTERBY_EQUALS, NULL, DP_FILTERBY_EQUALS, NULL, DP_FILTERBY_EQUALS,
                                  NULL, DP_FILTERBY_EQUALS, NULL, DP_FIELDS_FORMAT_DOTTED, DP_FILTERBY_EQUALS, NULL, &results, NULL) == FALSE || !results)
    {
      if (compact_id != NULL)
        g_free(compact_id);

      return FALSE;
    }

  if (g_list_length (results) != count)
    ret = FALSE;

  for (list = results; list; list = list->next)
    {
      DupinLinkRecord * record = list->data;

      /* NOTE - check if record expired and delete it eventually */

      if (dupin_link_record_is_expired (record, NULL) == TRUE)
        {
          if (!(dupin_link_record_delete (record, NULL, NULL)))
            continue;
        }

      gchar *tmp;

      rowid = dupin_link_record_get_rowid (record);

      guint last_revision = record->last->revision;

      if (dupin_link_record_is_deleted (record, NULL) == TRUE)
        {
          if (topurge == TRUE)
            {
	      /* NOTE - need to decrese deleted counter */

	      struct dupin_link_record_select_total_t t;
              memset (&t, 0, sizeof (t));

              if (sqlite3_exec (linkb->db, DUPIN_LINKB_SQL_GET_TOTALS, dupin_link_record_select_total_cb, &t, &errmsg) != SQLITE_OK)
                {
                  g_error ("dupin_linkbase_thread_compact: %s", errmsg);
                  sqlite3_free (errmsg);

                  return FALSE;
                }
              else
                {
                  if (dupin_link_record_is_weblink (record) == TRUE)
                    {
                      t.total_webl_del--;
                    }
                  else
                    {
                      t.total_rel_del--;
                    }

                  tmp = sqlite3_mprintf (DUPIN_LINKB_SQL_SET_TOTALS, (gint)t.total_webl_ins, (gint)t.total_webl_del, (gint)t.total_rel_ins, (gint)t.total_rel_del);

                  if (sqlite3_exec (linkb->db, tmp, NULL, NULL, &errmsg) != SQLITE_OK)
                    {
                      g_error ("dupin_linkbase_thread_compact: %s", errmsg);
                      sqlite3_free (errmsg);

                      sqlite3_free (tmp);

                      return FALSE;
                    }
                }

              if (tmp != NULL)
                sqlite3_free (tmp);

              /* wipe anything about ID */

              tmp = sqlite3_mprintf ("DELETE FROM Dupin WHERE id = '%q'", (gchar *) dupin_link_record_get_id (record));
	    }
          else
            {
              tmp = sqlite3_mprintf ("DELETE FROM Dupin WHERE id = '%q' AND rev < %d", (gchar *) dupin_link_record_get_id (record), (gint)last_revision);
            }
	}
      else
        {
          tmp = sqlite3_mprintf ("DELETE FROM Dupin WHERE id = '%q' AND rev < %d", (gchar *) dupin_link_record_get_id (record), (gint)last_revision);
	}

#if DEBUG
      g_message("dupin_linkbase_thread_compact: query=%s\n", tmp);
#endif

      if (sqlite3_exec (linkb->db, tmp, NULL, NULL, &errmsg) != SQLITE_OK)
        {
          sqlite3_free (tmp);

          g_error ("dupin_linkbase_thread_compact: %s", errmsg);

          sqlite3_free (errmsg);

          return FALSE;
        }

      linkb->compact_processed_count++;

      sqlite3_free (tmp);

      if (compact_id != NULL)
        g_free(compact_id);

      compact_id = g_strdup_printf ("%i", (gint)rowid);

#if DEBUG
      g_message("dupin_linkbase_thread_compact(%p) compact_id=%s as fetched",g_thread_self (), compact_id);
#endif
    }
  
  dupin_link_record_get_list_close (results);

#if DEBUG
  g_message("dupin_linkbase_thread_compact() compact_id=%s as to be stored",compact_id);

  g_message("dupin_linkbase_thread_compact(%p)  finished last_compact_rowid=%s - compacted %d\n", g_thread_self (), compact_id, (gint)linkb->compact_processed_count);
#endif

  str = sqlite3_mprintf ("UPDATE DupinLinkB SET compact_id = '%q'", compact_id);

  if (compact_id != NULL)
    g_free (compact_id);

  if (sqlite3_exec (linkb->db, str, NULL, NULL, &errmsg) != SQLITE_OK)
    {
      sqlite3_free (str);

      g_error("dupin_linkbase_thread_compact: %s", errmsg);
      sqlite3_free (errmsg);

      return FALSE;
    }

  sqlite3_free (str);

  return ret;
}

void
dupin_linkbase_compact_func (gpointer data, gpointer user_data)
{
  DupinLinkB * linkb = (DupinLinkB*) data;
  gchar * errmsg;

  dupin_linkbase_ref (linkb);

#if DEBUG
  g_message("dupin_linkbase_compact_func(%p) started\n",g_thread_self ());
#endif

  g_rw_lock_writer_lock (linkb->rwlock);
  linkb->tocompact = TRUE;
  linkb->compact_thread = g_thread_self ();
  linkb->compact_processed_count = 0;
  g_rw_lock_writer_unlock (linkb->rwlock);

  while (linkb->todelete == FALSE)
    {
      gboolean compact_operation = dupin_linkbase_thread_compact (linkb, DUPIN_LINKB_COMPACT_COUNT);

      if (compact_operation == FALSE)
        {
#if DEBUG
          g_message("dupin_linkbase_compact_func(%p) Compacted TOTAL %d records\n", g_thread_self (), (gint)linkb->compact_processed_count);
#endif

          /* claim disk space back */

	  /* NOTE - wait till next transaction is finished */

          if (linkb->d->bulk_transaction == TRUE)
            {
#if DEBUG
              g_message("dupin_linkbase_compact_func(%p) waiting for transaction to finish\n", g_thread_self ());
#endif

              continue;
            }

          /* NOTE - make sure last transaction is commited */

          if (dupin_linkbase_commit_transaction (linkb, NULL) < 0)
            {
              dupin_linkbase_rollback_transaction (linkb, NULL);
            }

#if DEBUG
          g_message("dupin_linkbase_compact_func: VACUUM and ANALYZE\n");
#endif

          if (sqlite3_exec (linkb->db, "VACUUM", NULL, NULL, &errmsg) != SQLITE_OK
             || sqlite3_exec (linkb->db, "ANALYZE Dupin", NULL, NULL, &errmsg) != SQLITE_OK)
            {
              g_error ("dupin_linkbase_compact_func: %s", errmsg);
              sqlite3_free (errmsg);
              break;
            }

          break;
        }
    }

#if DEBUG
  g_message("dupin_linkbase_compact_func(%p) finished and linkbase is compacted\n",g_thread_self ());
#endif

  g_rw_lock_writer_lock (linkb->rwlock);
  linkb->tocompact = FALSE;
  linkb->topurge = FALSE;
  linkb->compact_thread = NULL;
  g_rw_lock_writer_unlock (linkb->rwlock);

  dupin_linkbase_unref (linkb);
}

void
dupin_linkbase_compact (DupinLinkB * linkb,
			gboolean purge)
{
  g_return_if_fail (linkb != NULL);

  if (dupin_linkbase_is_compacting (linkb))
    {
#if DEBUG
      g_message("dupin_linkbase_compact(%p): linkbase is still compacting linkb->compact_thread=%p\n", g_thread_self (), linkb->compact_thread);
#endif
    }
  else
    {
#if DEBUG
      g_message("dupin_linkbase_compact(%p): push to thread pools linkb->compact_thread=%p\n", g_thread_self (), linkb->compact_thread);
#endif

      GError * error = NULL;

      g_rw_lock_writer_lock (linkb->rwlock);
      linkb->topurge = purge;
      g_rw_lock_writer_unlock (linkb->rwlock);

      if (!linkb->compact_thread)
        {
          g_thread_pool_push(linkb->d->linkb_compact_workers_pool, linkb, &error);

	  if (error != NULL)
            {
              g_error("dupin_linkbase_compact: linkbase %s compact thread creation error: %s", linkb->name, error->message);

              dupin_linkbase_set_error (linkb, error->message);

              g_error_free (error);
            }
        }
    }
}

gboolean
dupin_linkbase_is_compacting (DupinLinkB * linkb)
{
  g_return_val_if_fail (linkb != NULL, FALSE);

  if (linkb->compact_thread)
    return TRUE;

  return FALSE;
}

gboolean
dupin_linkbase_is_compacted (DupinLinkB * linkb)
{
  g_return_val_if_fail (linkb != NULL, FALSE);

  if (dupin_linkbase_is_compacting (linkb))
    return FALSE;

  return linkb->tocompact ? FALSE : TRUE;
}

/* Links checking */

static int
dupin_linkbase_check_cb (void *data, int argc, char **argv, char **col)
{
  gchar **check_id = data;

  if (argv[0] && *argv[0])
    *check_id = g_strdup (argv[0]);

  return 0;
}

gboolean
dupin_linkbase_thread_check (DupinLinkB * linkb, gsize count)
{
  g_return_val_if_fail (linkb != NULL, FALSE);

  DupinDB * parent_db=NULL;
  DupinLinkB * parent_linkb=NULL;
  gchar * check_id = NULL;
  gsize rowid;
  gchar * errmsg;
  GList *results, *list;

  gboolean ret = TRUE;

  gchar *str;

  /* get last position we checked and get anything up to count after that */

  gchar * query = "SELECT check_id as c FROM DupinLinkB LIMIT 1";

  if (sqlite3_exec (linkb->db, query, dupin_linkbase_check_cb, &check_id, &errmsg) != SQLITE_OK)
    {
      g_error("dupin_linkbase_thread_check: %s", errmsg);
      sqlite3_free (errmsg);

      return FALSE;
    }

  /* RULE 1 - for each link record check whether or not the countext_id has been deleted, and delete the link itself (I.e. mark link as deleted only) */

  if (dupin_linkbase_get_parent_is_db (linkb) == TRUE )
    {
      if (! (parent_db = dupin_database_open (linkb->d, dupin_linkbase_get_parent (linkb), NULL)))
        {
          g_error("dupin_linkbase_thread_check: Cannot connect to parent database %s", dupin_linkbase_get_parent (linkb));
          return FALSE;
        }
    }
  else
    {
      if (!(parent_linkb = dupin_linkbase_open (linkb->d, dupin_linkbase_get_parent (linkb), NULL)))
        {
          g_error("dupin_linkbase_thread_check: Cannot connect to parent linkbase %s", dupin_linkbase_get_parent (linkb));
          return FALSE;
        }
    }

  gsize start_rowid = (check_id != NULL) ? (gsize) g_ascii_strtoll (check_id, NULL, 10)+1 : 1;

  if (dupin_link_record_get_list (linkb, count, 0, start_rowid, 0, DP_LINK_TYPE_ANY, NULL, NULL, NULL, TRUE, DP_COUNT_EXIST, DP_ORDERBY_ROWID, FALSE,
				  NULL, NULL, DP_FILTERBY_EQUALS, NULL, DP_FILTERBY_EQUALS, NULL, DP_FILTERBY_EQUALS,
                                  NULL, DP_FILTERBY_EQUALS, NULL, DP_FIELDS_FORMAT_DOTTED, DP_FILTERBY_EQUALS, NULL, &results, NULL) ==
      FALSE || !results)
    {
      if (check_id != NULL)
        g_free(check_id);

      return FALSE;
    }

  if (g_list_length (results) != count)
    ret = FALSE;

  for (list = results; list; list = list->next)
    {
      DupinLinkRecord * record = list->data;

      /* we try to be clever in case the below delete will interfer - we really hope not :) */
      if (dupin_link_record_is_deleted (record, NULL) == TRUE)
        continue;

      /* STEP A - check if the context_id of the link record has been deleted */
      gchar * context_id = (gchar *)dupin_link_record_get_context_id (record);

#if DEBUG
      g_message("dupin_linkbase_thread_check(%p) checking STEP A for context_id=%s\n",g_thread_self (), context_id);
#endif

      if (dupin_linkbase_get_parent_is_db (linkb) == TRUE )
        {
          DupinRecord * doc_id_record = NULL;

          if (!(doc_id_record = dupin_record_read (parent_db, context_id, NULL)))
            {
              g_error ("dupin_linkbase_thread_check: Cannot read record from parent database");
              break;
            }

         if (dupin_record_is_deleted (doc_id_record, NULL) == FALSE)
           {
             dupin_record_close (doc_id_record);
	     continue;
           }
         dupin_record_close (doc_id_record);
       }
     else
       {
          DupinLinkRecord * link_id_record = NULL;

          if (!(link_id_record = dupin_link_record_read (parent_linkb, context_id, NULL)))
            {
              g_error ("dupin_linkbase_thread_check: Cannot read record from parent linkbase");
              break;
            }

         if (dupin_link_record_is_deleted (link_id_record, NULL) == FALSE)
           {
             dupin_link_record_close (link_id_record);
	     continue;
           }
         dupin_link_record_close (link_id_record);
       }

     /* STEP B - delete (update) the record */

#if DEBUG
     g_message("dupin_linkbase_thread_check(%p) STEP B for context_id=%s\n",g_thread_self (), context_id);
#endif

     /* NOTE - hopefully this will work and will not generate any problems (I.e. modifiying DB while reading from it
	       with the results cursor - but the ROWID is going to be higher anyway, so we should be safe also for views */

     if (!(dupin_link_record_delete (record, NULL, NULL)))
        {
          g_error ("dupin_linkbase_thread_check: Cannot delete link record");
          break;
        }

#if DEBUG
     g_message("dupin_linkbase_thread_check(%p) STEP B DONE for context_id=%s\n",g_thread_self (), context_id);
#endif

     linkb->check_processed_count++;

     rowid = dupin_link_record_get_rowid (record); // NOTE this is the rowid of the previous revision

     if (check_id != NULL)
       g_free(check_id);

     check_id = g_strdup_printf ("%i", (gint)rowid);

#if DEBUG
      g_message("dupin_linkbase_thread_check(%p) check_id=%s as fetched",g_thread_self (), check_id);
#endif
    }
  
  dupin_link_record_get_list_close (results);

  if (parent_db != NULL)
    dupin_database_unref (parent_db);

  if (parent_linkb != NULL)
    dupin_linkbase_unref (parent_linkb);

#if DEBUG
  g_message("dupin_linkbase_thread_check() check_id=%s as to be stored",check_id);

  g_message("dupin_linkbase_thread_check(%p)  finished last_check_rowid=%s - checked %d\n", g_thread_self (), check_id, (gint)linkb->check_processed_count);
#endif

  str = sqlite3_mprintf ("UPDATE DupinLinkB SET check_id = '%q'", check_id);

  if (check_id != NULL)
    g_free (check_id);

  if (sqlite3_exec (linkb->db, str, NULL, NULL, &errmsg) != SQLITE_OK)
    {
      sqlite3_free (str);

      g_error("dupin_linkbase_thread_check: %s", errmsg);
      sqlite3_free (errmsg);

      return FALSE;
    }

  sqlite3_free (str);

  return ret;
}

void
dupin_linkbase_check_func (gpointer data, gpointer user_data)
{
  DupinLinkB * linkb = (DupinLinkB*) data;

  dupin_linkbase_ref (linkb);

#if DEBUG
  g_message("dupin_linkbase_check_func(%p) started\n",g_thread_self ());
#endif

  g_rw_lock_writer_lock (linkb->rwlock);
  linkb->tocheck = TRUE;
  linkb->check_thread = g_thread_self ();
  linkb->check_processed_count = 0;
  g_rw_lock_writer_unlock (linkb->rwlock);

  while (linkb->todelete == FALSE)
    {
      gboolean check_operation = dupin_linkbase_thread_check (linkb, DUPIN_LINKB_CHECK_COUNT);

      if (check_operation == FALSE)
        {
#if DEBUG
          g_message("dupin_linkbase_check_func(%p) Checked TOTAL %d records\n", g_thread_self (), (gint)linkb->check_processed_count);
#endif

/* WE DO NOT DELETE ANYTHING YET with CHECK - add this later eventually if needed */
#if 0
          /* claim disk space back */

	  /* NOTE - wait till next transaction is finished */

          if (linkb->d->bulk_transaction == TRUE)
            {
#if DEBUG
              g_message("dupin_linkbase_check_func(%p) waiting for transaction to finish\n", g_thread_self ());
#endif

              continue;
            }

          /* NOTE - make sure last transaction is commited */

          if (dupin_linkbase_commit_transaction (linkb, NULL) < 0)
            {
              dupin_linkbase_rollback_transaction (linkb, NULL);
            }

#if DEBUG
          g_message("dupin_linkbase_check_func: VACUUM and ANALYZE\n");
#endif

          if (sqlite3_exec (linkb->db, "VACUUM", NULL, NULL, &errmsg) != SQLITE_OK
             || sqlite3_exec (linkb->db, "ANALYZE Dupin", NULL, NULL, &errmsg) != SQLITE_OK)
            {
              g_error ("dupin_linkbase_check_func: %s", errmsg);
              sqlite3_free (errmsg);
              break;
            }

#endif

          break;
        }
    }

#if DEBUG
  g_message("dupin_linkbase_check_func(%p) finished and linkbase is checked\n",g_thread_self ());
#endif

  g_rw_lock_writer_lock (linkb->rwlock);
  linkb->tocheck = FALSE;
  linkb->check_thread = NULL;
  g_rw_lock_writer_unlock (linkb->rwlock);

  dupin_linkbase_unref (linkb);
}

void
dupin_linkbase_check (DupinLinkB * linkb)
{
  g_return_if_fail (linkb != NULL);

  if (dupin_linkbase_is_checking (linkb))
    {
#if DEBUG
      g_message("dupin_linkbase_check(%p): linkbase is still checking linkb->check_thread=%p\n", g_thread_self (), linkb->check_thread);
#endif
    }
  else
    {
#if DEBUG
      g_message("dupin_linkbase_check(%p): push to thread pools linkb->check_thread=%p\n", g_thread_self (), linkb->check_thread);
#endif

      GError * error = NULL;

      if (!linkb->check_thread)
        {
          g_thread_pool_push(linkb->d->linkb_check_workers_pool, linkb, &error);

	  if (error != NULL)
            {
              g_error("dupin_linkbase_check: linkbase %s check thread creation error: %s", linkb->name, error->message);

              dupin_linkbase_set_error (linkb, error->message);

              g_error_free (error);
            }
        }
    }
}

gboolean
dupin_linkbase_is_checking (DupinLinkB * linkb)
{
  g_return_val_if_fail (linkb != NULL, FALSE);

  if (linkb->check_thread)
    return TRUE;

  return FALSE;
}

gboolean
dupin_linkbase_is_checked (DupinLinkB * linkb)
{
  g_return_val_if_fail (linkb != NULL, FALSE);

  if (dupin_linkbase_is_checking (linkb))
    return FALSE;

  return linkb->tocheck ? FALSE : TRUE;
}

void
dupin_linkbase_set_error (DupinLinkB * linkb,
                          gchar * msg)
{
  g_return_if_fail (linkb != NULL);
  g_return_if_fail (msg != NULL);

  dupin_linkbase_clear_error (linkb);

  linkb->error_msg = g_strdup ( msg );

  return;
}

void
dupin_linkbase_clear_error (DupinLinkB * linkb)
{
  g_return_if_fail (linkb != NULL);

  if (linkb->error_msg != NULL)
    g_free (linkb->error_msg);

  linkb->error_msg = NULL;

  return;
}

gchar * dupin_linkbase_get_error (DupinLinkB * linkb)
{
  g_return_val_if_fail (linkb != NULL, NULL);

  return linkb->error_msg;
}

void dupin_linkbase_set_warning (DupinLinkB * linkb,
                                 gchar * msg)
{
  g_return_if_fail (linkb != NULL);
  g_return_if_fail (msg != NULL);

  dupin_linkbase_clear_warning (linkb);

  linkb->warning_msg = g_strdup ( msg );

  return;
}

void dupin_linkbase_clear_warning (DupinLinkB * linkb)
{
  g_return_if_fail (linkb != NULL);

  if (linkb->warning_msg != NULL)
    g_free (linkb->warning_msg);

  linkb->warning_msg = NULL;

  return;
}

gchar * dupin_linkbase_get_warning (DupinLinkB * linkb)
{
  g_return_val_if_fail (linkb != NULL, NULL);

  return linkb->warning_msg;
}

/* EOF */
