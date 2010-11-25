#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "dupin_internal.h"
#include "dupin_utils.h"
#include "dupin_view.h"

#include <stdlib.h>
#include <string.h>

/*

See http://wiki.apache.org/couchdb/Introduction_to_CouchDB_views

-> SORT Dupin table by key as primary key and pid as secondary key

*/

#define DUPIN_VIEW_SQL_MAIN_CREATE \
  "CREATE TABLE IF NOT EXISTS Dupin (\n" \
  "  id          CHAR(255) NOT NULL,\n" \
  "  pid         TEXT,\n" \
  "  key         TEXT,\n" \
  "  obj         TEXT,\n" \
  "  PRIMARY KEY(id)\n" \
  ");"

#define DUPIN_VIEW_SQL_CREATE_INDEX \
  "CREATE INDEX IF NOT EXISTS DupinKey ON Dupin (key);\n" \
  "CREATE INDEX IF NOT EXISTS DupinPid ON Dupin (pid);\n" \
  "CREATE INDEX IF NOT EXISTS DupinId ON Dupin (id);"

#define DUPIN_VIEW_SQL_DESC_CREATE \
  "CREATE TABLE IF NOT EXISTS DupinView (\n" \
  "  parent                    CHAR(255) NOT NULL,\n" \
  "  isdb                      BOOL DEFAULT TRUE,\n" \
  "  map                       TEXT,\n" \
  "  map_lang                  CHAR(255),\n" \
  "  reduce                    TEXT,\n" \
  "  reduce_lang               CHAR(255),\n" \
  "  sync_map_id               CHAR(255),\n" \
  "  sync_reduce_id            CHAR(255)\n" \
  ");"

#define DUPIN_VIEW_SQL_INSERT \
	"INSERT INTO Dupin (id, pid, key, obj) " \
        "VALUES('%q', '%q', '%q', '%q')"

#define DUPIN_VIEW_SQL_EXISTS \
	"SELECT count(id) FROM Dupin WHERE id = '%q' "

#define DUPIN_VIEW_SQL_TOTAL_REREDUCE \
	"SELECT count(id) FROM Dupin as d LEFT OUTER JOIN (select key as inner_key, count(*) as inner_count from Dupin GROUP BY inner_key HAVING inner_count > 1) ON d.key=inner_key  WHERE inner_count!=''; "

#define VIEW_SYNC_COUNT	100

#if 0
static void
dupin_view_debug_print_json_node (char * msg, JsonNode * node)
{
  g_assert (node != NULL);
 
  gchar * buffer;
  if (json_node_get_node_type (node) == JSON_NODE_VALUE)
    {
     buffer = g_strdup ( json_node_get_string (node) ); /* we should check number, boolean too */
    }
  else
   {
     JsonGenerator *gen = json_generator_new();
     json_generator_set_root (gen, node);
     g_object_set (gen, "pretty", TRUE, NULL);
     buffer = json_generator_to_data (gen,NULL);
     g_object_unref (gen);
   }
  g_message("%s - Json Node of type %d: %s\n",msg, (gint)json_node_get_node_type (node), buffer);
  g_free (buffer);
}
#endif

static gchar *dupin_view_generate_id (DupinView * view);

gchar **
dupin_get_views (Dupin * d)
{
  guint i;
  gsize size;
  gchar **ret;
  gpointer key;
  GHashTableIter iter;

  g_return_val_if_fail (d != NULL, NULL);

  g_mutex_lock (d->mutex);

  if (!(size = g_hash_table_size (d->views)))
    {
      g_mutex_unlock (d->mutex);
      return NULL;
    }

  ret = g_malloc (sizeof (gchar *) * (g_hash_table_size (d->views) + 1));

  i = 0;
  g_hash_table_iter_init (&iter, d->views);
  while (g_hash_table_iter_next (&iter, &key, NULL) == TRUE)
    ret[i++] = g_strdup (key);

  ret[i] = NULL;

  g_mutex_unlock (d->mutex);

  return ret;
}

gboolean
dupin_view_exists (Dupin * d, gchar * view)
{
  gboolean ret;

  g_mutex_lock (d->mutex);
  ret = g_hash_table_lookup (d->views, view) != NULL ? TRUE : FALSE;
  g_mutex_unlock (d->mutex);

  return ret;
}

DupinView *
dupin_view_open (Dupin * d, gchar * view, GError ** error)
{
  DupinView *ret;

  g_return_val_if_fail (d != NULL, NULL);
  g_return_val_if_fail (view != NULL, NULL);

  g_mutex_lock (d->mutex);

  if (!(ret = g_hash_table_lookup (d->views, view)) || ret->todelete == TRUE)
    {
      g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN,
		 "View '%s' doesn't exist.", view);

      g_mutex_unlock (d->mutex);
      return NULL;
    }
  else
    ret->ref++;

  g_mutex_unlock (d->mutex);

  return ret;
}

static gboolean
dupin_view_get_total_records_db (Dupin * d, gchar * parent, gsize * total)
{
  DupinDB *db;

  if (!(db = dupin_database_open (d, parent, NULL)))
    return FALSE;

  *total = dupin_database_count (db, DP_COUNT_EXIST);

  dupin_database_unref (db);
  return TRUE;
}

static gboolean
dupin_view_get_total_records_view (Dupin * d, gchar * parent, gsize * total)
{
  DupinView *view;

  if (!(view = dupin_view_open (d, parent, NULL)))
    return FALSE;

  if (dupin_view_record_get_total_records (view, total) == FALSE)
    {
      dupin_view_unref (view);
      return FALSE;
    }

  dupin_view_unref (view);
  return TRUE;
}

static gboolean
dupin_view_get_total_records (Dupin * d, gchar * parent, gboolean is_db, gsize * total)
{
  if (is_db)
    return dupin_view_get_total_records_db (d, parent, total);

  return dupin_view_get_total_records_view (d, parent, total);
}

DupinView *
dupin_view_new (Dupin * d, gchar * view, gchar * parent, gboolean is_db,
		gchar * map, DupinMRLang map_language, gchar * reduce,
		DupinMRLang reduce_language, GError ** error)
{
  DupinView *ret;
  gchar *path;
  gchar *name;

  gchar *str;
  gchar *errmsg;

  g_return_val_if_fail (d != NULL, NULL);
  g_return_val_if_fail (view != NULL, NULL);
  g_return_val_if_fail (parent != NULL, NULL);
  g_return_val_if_fail (dupin_util_is_valid_view_name (view) == TRUE, NULL);

  if (is_db == TRUE)
    g_return_val_if_fail (dupin_database_exists (d, parent) == TRUE, NULL);
  else
    g_return_val_if_fail (dupin_view_exists (d, parent) == TRUE, NULL);

  g_mutex_lock (d->mutex);

  if ((ret = g_hash_table_lookup (d->views, view)))
    {
      g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN,
		   "View '%s' already exist.", view);
      g_mutex_unlock (d->mutex);
      return NULL;
    }

  name = g_strdup_printf ("%s%s", view, DUPIN_VIEW_SUFFIX);
  path = g_build_path (G_DIR_SEPARATOR_S, d->path, name, NULL);
  g_free (name);

  if (!(ret = dupin_view_create (d, view, path, error)))
    {
      g_mutex_unlock (d->mutex);
      g_free (path);
      return NULL;
    }

  ret->map = g_strdup (map);
  ret->map_lang = map_language;

  if (reduce != NULL && strcmp(reduce,"(NULL)") && strcmp(reduce,"null") )
    {
      ret->reduce = g_strdup (reduce);
      ret->reduce_lang = reduce_language;
    }

  ret->parent = g_strdup (parent);
  ret->parent_is_db = is_db;

  g_free (path);
  ret->ref++;

  str =
    sqlite3_mprintf ("INSERT INTO DupinView "
		     "(parent, isdb, map, map_lang, reduce, reduce_lang) "
		     "VALUES('%q', '%s', '%q', '%q', '%q' ,'%q')", parent,
		     is_db ? "TRUE" : "FALSE", map,
		     dupin_util_mr_lang_to_string (map_language), reduce,
		     dupin_util_mr_lang_to_string (reduce_language));

  if (sqlite3_exec (ret->db, str, NULL, NULL, &errmsg) != SQLITE_OK)
    {
      g_mutex_unlock (d->mutex);

      g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN, "%s",
		   errmsg);

      sqlite3_free (errmsg);
      sqlite3_free (str);
      dupin_view_free (ret);
      return NULL;
    }

  sqlite3_free (str);

  /* NOTE - the respective map and reduce threads will add +1 top the these values */
  str = sqlite3_mprintf ("UPDATE DupinView SET sync_map_id = '0', sync_reduce_id = '0' ");

  if (sqlite3_exec (ret->db, str, NULL, NULL, &errmsg) != SQLITE_OK)
    {
      g_mutex_unlock (d->mutex);
      g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN, "%s",
		       errmsg);

      sqlite3_free (errmsg);
      sqlite3_free (str);
      dupin_view_free (ret);
      return NULL;
    }

  sqlite3_free (str);

  g_mutex_unlock (d->mutex);

  if (dupin_view_p_update (ret, error) == FALSE)
    {
      dupin_view_free (ret);
      return NULL;
    }

  g_mutex_lock (d->mutex);
  g_hash_table_insert (d->views, g_strdup (view), ret);
  g_mutex_unlock (d->mutex);

  dupin_view_sync (ret);
  return ret;
}

struct dupin_view_p_update_t
{
  gchar *parent;
  gboolean isdb;
};

static int
dupin_view_p_update_cb (void *data, int argc, char **argv, char **col)
{
  struct dupin_view_p_update_t *update = data;

  if (argv[0] && *argv[0])
    update->parent = g_strdup (argv[0]);

  if (argv[1] && *argv[1])
    update->isdb = !strcmp (argv[1], "TRUE") ? TRUE : FALSE;

  return 0;
}

#define DUPIN_VIEW_P_SIZE	64

static void
dupin_view_p_update_real (DupinViewP * p, DupinView * view)
{
  if (p->views == NULL)
    {
      p->views = g_malloc (sizeof (DupinView *) * DUPIN_VIEW_P_SIZE);
      p->size = DUPIN_VIEW_P_SIZE;
    }

  else if (p->numb == p->size)
    {
      p->size += DUPIN_VIEW_P_SIZE;
      p->views = g_realloc (p->views, sizeof (DupinView *) * p->size);
    }

  p->views[p->numb] = view;
  p->numb++;
}

gboolean
dupin_view_p_update (DupinView * view, GError ** error)
{
  gchar *errmsg;
  struct dupin_view_p_update_t update;
  gchar *query = "SELECT parent, isdb FROM DupinView";

  memset (&update, 0, sizeof (struct dupin_view_p_update_t));

  g_mutex_lock (view->d->mutex);

  if (sqlite3_exec (view->db, query, dupin_view_p_update_cb, &update, &errmsg)
      != SQLITE_OK)
    {
      g_mutex_unlock (view->d->mutex);

      g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN, "%s",
		   errmsg);
      sqlite3_free (errmsg);
      return FALSE;
    }

  g_mutex_unlock (view->d->mutex);

  if (!update.parent)
    {
      g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN,
		   "Internal error.");
      return FALSE;
    }

  if (update.isdb == TRUE)
    {
      DupinDB *db;

      if (!(db = dupin_database_open (view->d, update.parent, error)))
	{
	  g_free (update.parent);
	  return FALSE;
	}

      g_mutex_lock (db->mutex);
      dupin_view_p_update_real (&db->views, view);
      g_mutex_unlock (db->mutex);

      dupin_database_unref (db);
    }
  else
    {
      DupinView *v;

      if (!(v = dupin_view_open (view->d, update.parent, error)))
	{
	  g_free (update.parent);
	  return FALSE;
	}

      g_mutex_lock (v->mutex);
      dupin_view_p_update_real (&v->views, view);
      g_mutex_unlock (v->mutex);

      dupin_view_unref (view);
    }

  g_free (update.parent);
  return TRUE;
}

void
dupin_view_p_record_insert (DupinViewP * p, gchar * id,
			    JsonObject * obj)
{
  gsize i;

  for (i = 0; i < p->numb; i++)
    {
      DupinView *view = p->views[i];
      JsonArray *array;

      /* VERY IMPORTANT - we do only map on record insertion - the reduce step is only done on sync - but the synced flag must not be set if reduce is still needed */

      if ((array = dupin_mr_record_map (view, obj)))
	{
	  GList *nodes, *n;
	  nodes = json_array_get_elements (array);

          for (n = nodes; n != NULL; n = n->next)
            {
              JsonNode * element_node = (JsonNode*)n->data;
              JsonObject *nobj = json_node_get_object (element_node);

              GList *nodes, *n;
              JsonNode *key_node=NULL;
	      nodes = json_object_get_members (nobj);
              for (n = nodes; n != NULL; n = n->next)
                {
                  gchar *member_name = (gchar *) n->data;
                  if (!strcmp (member_name, "key"))
                    {
		      /* we extract this for SQLite table indexing */
                      key_node = json_node_copy (json_object_get_member (nobj, member_name) );
                    }
                }
              g_list_free (nodes);

              JsonNode *pid_node=json_node_new (JSON_NODE_ARRAY);
              JsonArray *pid_array=json_array_new ();
              json_array_add_string_element (pid_array, id);
              json_node_take_array (pid_node, pid_array);

	      dupin_view_record_save_map (view, pid_node, key_node, element_node);

              json_node_free (pid_node);
              if (key_node != NULL)
                json_node_free (key_node);

	      dupin_view_p_record_insert (&view->views, id, nobj); /* TODO - check if this is nobj or obj ?! */
            }
          g_list_free (nodes);

	  json_array_unref (array);
	}
    }
}

void
dupin_view_p_record_delete (DupinViewP * p, gchar * pid)
{
  gsize i;

  for (i = 0; i < p->numb; i++)
    {
      DupinView *view = p->views[i];
      dupin_view_p_record_delete (&view->views, pid);
      dupin_view_record_delete (view, pid);
    }
}

void
dupin_view_record_save_map (DupinView * view, JsonNode * pid, JsonNode * key, JsonNode * obj_node)
{
  GList *nodes, *n;
  JsonNode *node;
  JsonObject *obj;
  JsonGenerator *gen;

  const gchar *id = NULL;
  gchar *tmp, *errmsg, *obj_serialised=NULL, *key_serialised=NULL, *pid_serialised=NULL;
  JsonNode *key_node=NULL;
  JsonNode *pid_node=NULL;

  g_return_if_fail (json_node_get_node_type (obj_node) == JSON_NODE_OBJECT);

  obj = json_node_get_object (obj_node);

  if (key != NULL)
    key_node = json_node_copy (key);

  if (pid != NULL)
    pid_node = json_node_copy (pid);

  g_mutex_lock (view->mutex);

  nodes = json_object_get_members (obj);

  for (n = nodes; n != NULL; n = n->next)
    {
      gchar *member_name = (gchar *) n->data;

      if (!strcmp (member_name, "_id"))
        {
          /* NOTE - we always force a new _id - due records must be sorted by a controlled ID in a view for mp/r/rr purposes */
          json_object_remove_member (obj, member_name);
	}
    }
  g_list_free (nodes);

  if (!id && !(id = dupin_view_generate_id (view)))
    {
      g_mutex_unlock (view->mutex);
      if (key_node != NULL)
        json_node_free (key_node);
      if (pid_node != NULL)
        json_node_free (pid_node);
      return;
    }

  /* serialise the obj */
  node = json_node_new (JSON_NODE_OBJECT);

  if (node == NULL)
    {
      g_mutex_unlock (view->mutex);
      g_free ((gchar *)id);
      if (key_node != NULL)
        json_node_free (key_node);
      if (pid_node != NULL)
        json_node_free (pid_node);
      return;
    }

  json_node_set_object (node, obj);

  gen = json_generator_new();

  if (gen == NULL)
    {
      g_mutex_unlock (view->mutex);
      g_free ((gchar *)id);
      if (node != NULL)
        json_node_free (node);
      if (key_node != NULL)
        json_node_free (key_node);
      if (pid_node != NULL)
        json_node_free (pid_node);
      return;
    }

  json_generator_set_root (gen, node );
  obj_serialised = json_generator_to_data (gen,NULL);

  if (obj_serialised == NULL)
    {
      g_mutex_unlock (view->mutex);
      g_free ((gchar *)id);
      if (gen != NULL)
        g_object_unref (gen);
      if (node != NULL)
        json_node_free (node);
      if (key_node != NULL)
        json_node_free (key_node);
      if (pid_node != NULL)
        json_node_free (pid_node);
      return;
    }

  if (gen != NULL)
    g_object_unref (gen);
  if (node != NULL)
    json_node_free (node);

  /* serialise the key */

  if (key_node != NULL)
    {
      if (json_node_get_node_type (key_node) == JSON_NODE_VALUE)
        {
          if (json_node_get_value_type (key_node) == G_TYPE_STRING)
          {
            key_serialised = g_strdup_printf ("\"%s\"", json_node_get_string (key_node));
          }

          if (json_node_get_value_type (key_node) == G_TYPE_DOUBLE
                || json_node_get_value_type (key_node) == G_TYPE_FLOAT)
          {
            gdouble numb = json_node_get_double (key_node);
            key_serialised = g_strdup_printf ("%f", numb);
          }

          if (json_node_get_value_type (key_node) == G_TYPE_INT
                || json_node_get_value_type (key_node) == G_TYPE_INT64
                || json_node_get_value_type (key_node) == G_TYPE_UINT)
          {
            gint numb = (gint) json_node_get_int (key_node);
            key_serialised = g_strdup_printf ("%d", numb);
          }
          if (json_node_get_value_type (key_node) == G_TYPE_BOOLEAN)
          {
            key_serialised = g_strdup_printf (json_node_get_boolean (key_node) == TRUE ? "true" : "false");
          }

          if (key_node != NULL)
            json_node_free (key_node);
        }
      else
        {
          gen = json_generator_new();

          if (gen == NULL)
            {
              g_mutex_unlock (view->mutex);
              g_free ((gchar *)id);
              g_free (obj_serialised);
              if (key_node != NULL)
                json_node_free (key_node);
              if (pid_node != NULL)
                json_node_free (pid_node);
              return;
            }

          json_generator_set_root (gen, key_node );
          key_serialised = json_generator_to_data (gen,NULL);

          if (key_node != NULL)
            json_node_free (key_node);

          if (key_serialised == NULL)
            {
              g_mutex_unlock (view->mutex);
              g_free ((gchar *)id);
              g_free (obj_serialised);
              if (gen != NULL)
                g_object_unref (gen);
              if (pid_node != NULL)
                json_node_free (pid_node);
              return;
            }

          if (gen != NULL)
            g_object_unref (gen);
        }
    }

  if (pid_node != NULL)
    {
      gen = json_generator_new();

      if (gen == NULL)
        {
          g_mutex_unlock (view->mutex);
          g_free ((gchar *)id);
          g_free (obj_serialised);
          if (key_serialised)
            g_free (key_serialised);
          if (pid_node != NULL)
            json_node_free (pid_node);
          return;
        }

      json_generator_set_root (gen, pid_node );
      pid_serialised = json_generator_to_data (gen,NULL);

      if (pid_node != NULL)
        json_node_free (pid_node);

      if (pid_serialised == NULL)
        {
          g_mutex_unlock (view->mutex);
          g_free ((gchar *)id);
          g_free (obj_serialised);
          if (key_serialised)
            g_free (key_serialised);
          if (gen != NULL)
            g_object_unref (gen);
          return;
        }

      if (gen != NULL)
        g_object_unref (gen);
    }

  tmp = sqlite3_mprintf (DUPIN_VIEW_SQL_INSERT, id, pid_serialised, key_serialised, obj_serialised);

//g_message("query: %s\n",tmp);

  if (sqlite3_exec (view->db, tmp, NULL, NULL, &errmsg) != SQLITE_OK)
    {
      g_error("dupin_view_record_save_map: %s", errmsg);
      sqlite3_free (errmsg);
    }

  g_mutex_unlock (view->mutex);

  sqlite3_free (tmp);
  g_free (obj_serialised);
  if (key_serialised)
    g_free (key_serialised);
  if (pid_serialised)
    g_free (pid_serialised);
  g_free ((gchar *)id);
}

static void
dupin_view_generate_id_create (DupinView * view, gchar id[255])
{
  do
    {
      dupin_util_generate_id (id);
    }
  while (dupin_view_record_exists_real (view, id, FALSE) == TRUE);
}

static gchar *
dupin_view_generate_id (DupinView * view)
{
  gchar id[255];

  dupin_view_generate_id_create (view, id);
  return g_strdup (id);
}

void
dupin_view_record_delete (DupinView * view, gchar * pid)
{
  gchar *query;
  gchar *errmsg;

  /* NOTE - hack to avoid to keep another table and be able to delete entries
            from a view generated from multiple input documents */
     
  query = sqlite3_mprintf ("DELETE FROM Dupin WHERE pid LIKE '%%\"%q\"%%' ;", pid); /* TODO - might need double %% to escape % for mprintf */

g_message("dupin_view_record_delete() query=%s\n",query);

  g_mutex_lock (view->mutex);

  if (sqlite3_exec (view->db, query, NULL, NULL, &errmsg) != SQLITE_OK)
    {
      g_error("dupin_view_record_delete: %s", errmsg);
      sqlite3_free (errmsg);
    }

  g_mutex_unlock (view->mutex);

  sqlite3_free (query);
}

void
dupin_view_ref (DupinView * view)
{
  Dupin *d;

  g_return_if_fail (view != NULL);

  d = view->d;

  g_mutex_lock (d->mutex);
  view->ref++;
  g_mutex_unlock (d->mutex);
}

void
dupin_view_unref (DupinView * view)
{
  Dupin *d;

  g_return_if_fail (view != NULL);

  d = view->d;
  g_mutex_lock (d->mutex);

  if (view->ref >= 0)
    view->ref--;

  if (view->ref == 0 && view->todelete == TRUE)
    g_hash_table_remove (d->views, view->name);

  g_mutex_unlock (d->mutex);
}

gboolean
dupin_view_delete (DupinView * view, GError ** error)
{
  Dupin *d;

  g_return_val_if_fail (view != NULL, FALSE);

  d = view->d;

  g_mutex_lock (d->mutex);
  view->todelete = TRUE;
  g_mutex_unlock (d->mutex);

  return TRUE;
}

gboolean
dupin_view_force_quit (DupinView * view, GError ** error)
{
  Dupin *d;

  g_return_val_if_fail (view != NULL, FALSE);

  d = view->d;

  g_mutex_lock (d->mutex);
  view->sync_toquit = TRUE;
  g_mutex_unlock (d->mutex);

  return TRUE;
}

const gchar *
dupin_view_get_name (DupinView * view)
{
  g_return_val_if_fail (view != NULL, NULL);
  return view->name;
}

const gchar *
dupin_view_get_parent (DupinView * view)
{
  g_return_val_if_fail (view != NULL, NULL);

  return view->parent;
}

gboolean
dupin_view_get_parent_is_db (DupinView * view)
{
  g_return_val_if_fail (view != NULL, FALSE);

  return view->parent_is_db;
}

const gchar *
dupin_view_get_map (DupinView * view)
{
  g_return_val_if_fail (view != NULL, NULL);

  return view->map;
}

DupinMRLang
dupin_view_get_map_language (DupinView * view)
{
  g_return_val_if_fail (view != NULL, 0);

  return view->map_lang;
}

const gchar *
dupin_view_get_reduce (DupinView * view)
{
  g_return_val_if_fail (view != NULL, NULL);

  return view->reduce;
}

DupinMRLang
dupin_view_get_reduce_language (DupinView * view)
{
  g_return_val_if_fail (view != NULL, 0);

  return view->reduce_lang;
}

gsize
dupin_view_get_size (DupinView * view)
{
  struct stat st;

  g_return_val_if_fail (view != NULL, 0);

  if (g_stat (view->path, &st) != 0)
    return 0;

  return (gsize) st.st_size;
}

/* Internal: */
void
dupin_view_free (DupinView * view)
{
  if (view->todelete == TRUE)
    g_unlink (view->path);

  g_cond_free(view->sync_map_has_new_work);

  if (view->db)
    sqlite3_close (view->db);

  if (view->name)
    g_free (view->name);

  if (view->path)
    g_free (view->path);

  if (view->parent)
    g_free (view->parent);

  if (view->mutex)
    g_mutex_free (view->mutex);

  if (view->views.views)
    g_free (view->views.views);

  if (view->map)
    g_free (view->map);

  if (view->reduce)
    g_free (view->reduce);

  g_free (view);
}

static int
dupin_view_create_cb (void *data, int argc, char **argv, char **col)
{
  DupinView *view = data;

  if (argc == 6)
    {
      view->map = g_strdup (argv[0]);
      view->map_lang = dupin_util_mr_lang_to_enum (argv[1]);

      if (argv[2] != NULL && strcmp(argv[2],"(NULL)") && strcmp(argv[2],"null") )
        {
          view->reduce = g_strdup (argv[2]);
          view->reduce_lang = dupin_util_mr_lang_to_enum (argv[3]);
        }

      view->parent = g_strdup (argv[4]);
      view->parent_is_db = strcmp (argv[5], "TRUE") == 0 ? TRUE : FALSE;
    }

  return 0;
}

DupinView *
dupin_view_create (Dupin * d, gchar * name, gchar * path, GError ** error)
{
  gchar *query;
  gchar *errmsg;
  DupinView *view;

  view = g_malloc0 (sizeof (DupinView));

  view->sync_map_total_records = 0;
  view->sync_map_processed_count = 0;
  view->sync_reduce_total_records = 0;
  view->sync_reduce_processed_count = 0;

  view->sync_map_has_new_work = g_cond_new();

  view->d = d;

  view->name = g_strdup (name);
  view->path = g_strdup (path);

  if (sqlite3_open (view->path, &view->db) != SQLITE_OK)
    {
      g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN,
		   "View error.");
      dupin_view_free (view);
      return NULL;
    }

  if (sqlite3_exec (view->db, DUPIN_VIEW_SQL_MAIN_CREATE, NULL, NULL, &errmsg)
      				!= SQLITE_OK
      || sqlite3_exec (view->db, DUPIN_VIEW_SQL_DESC_CREATE, NULL, NULL,
		       &errmsg) != SQLITE_OK
      || sqlite3_exec (view->db, DUPIN_VIEW_SQL_CREATE_INDEX, NULL, NULL,
		       &errmsg) != SQLITE_OK)
    {
      g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN, "%s",
		   errmsg);
      sqlite3_free (errmsg);
      dupin_view_free (view);
      return NULL;
    }

  query =
    "SELECT map, map_lang, reduce, reduce_lang, parent, isdb FROM DupinView";

  if (sqlite3_exec (view->db, query, dupin_view_create_cb, view, &errmsg) !=
      SQLITE_OK)
    {
      g_set_error (error, dupin_error_quark (), DUPIN_ERROR_OPEN, "%s",
		   errmsg);
      sqlite3_free (errmsg);
      dupin_view_free (view);
    }

  view->mutex = g_mutex_new ();

  return view;
}

static int
dupin_view_count_cb (void *data, int argc, char **argv, char **col)
{
  gsize *size = data;

  if (argv[0] && *argv[0])
    *size = atoi (argv[0]);

  return 0;
}

gsize
dupin_view_count (DupinView * view)
{
  gsize size;
  gchar *query;

  g_return_val_if_fail (view != NULL, 0);

  query = "SELECT count(id) as c FROM Dupin";

  g_mutex_lock (view->mutex);

  if (sqlite3_exec (view->db, query, dupin_view_count_cb, &size, NULL) !=
      SQLITE_OK)
    {
      g_mutex_unlock (view->mutex);
      return 0;
    }

  g_mutex_unlock (view->mutex);
  return size;
}

static int
dupin_view_sync_cb (void *data, int argc, char **argv, char **col)
{
  gchar **sync_id = data;

  if (argv[0] && *argv[0])
    *sync_id = g_strdup (argv[0]);

  return 0;
}

struct dupin_view_sync_t
{
  JsonNode *obj;
  gchar *id;
  JsonNode *pid; /* array or null */
  JsonNode *key; /* array or null */
};

static void
dupin_view_sync_thread_real_map (DupinView * view, GList * list)
{
  for (; list; list = list->next)
    {
      struct dupin_view_sync_t *data = list->data;
      JsonArray *array;
      JsonObject * data_obj = json_node_get_object (data->obj);

      gchar * id = g_strdup ( (gchar *)json_node_get_string ( json_array_get_element ( json_node_get_array (data->pid), 0) ) );

      if ((array = dupin_mr_record_map (view, data_obj)))
	{
	  GList *nodes, *n;
	  nodes = json_array_get_elements (array);

          for (n = nodes; n != NULL; n = n->next)
            {
              JsonObject *nobj;
              JsonNode *element_node = (JsonNode*)n->data;

              nobj = json_node_get_object (element_node);

              GList *nodes, *n;
              JsonNode *key_node=NULL;
              nodes = json_object_get_members (nobj);
              for (n = nodes; n != NULL; n = n->next)
                {
                  gchar *member_name = (gchar *) n->data;
                  if (!strcmp (member_name, "key"))
                    {
		      /* we extract this for SQLite table indexing */
                      key_node = json_node_copy ( json_object_get_member (nobj, member_name));
                    }
                }
              g_list_free (nodes);

	      dupin_view_record_save_map (view, data->pid, key_node, element_node);

              g_mutex_lock (view->mutex);
              view->sync_map_processed_count++;
              g_mutex_unlock (view->mutex);

              if (key_node != NULL)
                json_node_free (key_node);
 
	      dupin_view_p_record_insert (&view->views, id, nobj); /* TODO - check if this is nobj or obj ?! */
            }
          g_list_free (nodes);

	  json_array_unref (array);
	}

        g_free(id);
    }
}

static gboolean
dupin_view_sync_thread_map_db (DupinView * view, gsize count)
{
  gchar * sync_map_id = NULL;
  gsize rowid;
  gchar * errmsg;
  DupinDB *db;
  GList *results, *list;

  GList *l = NULL;
  gboolean ret = TRUE;

  gchar *str;

  if (!(db = dupin_database_open (view->d, view->parent, NULL)))
    return FALSE;

  /* get last position we reduced and get anything up to count after that */
  gchar * previous_sync_map_id=NULL;
  gchar * query = "SELECT sync_map_id as c FROM DupinView";
  g_mutex_lock (view->mutex);

  if (sqlite3_exec (view->db, query, dupin_view_sync_cb, &previous_sync_map_id, &errmsg) != SQLITE_OK)
    {
      g_mutex_unlock (view->mutex);

      g_error("dupin_view_sync_thread_map_db: %s", errmsg);
      sqlite3_free (errmsg);

      dupin_database_unref (db);
      return FALSE;
    }

  g_mutex_unlock (view->mutex);

  gsize start_rowid = (previous_sync_map_id != NULL) ? atoi(previous_sync_map_id)+1 : 1;

  if (dupin_record_get_list (db, count, 0, start_rowid, 0, DP_COUNT_EXIST, DP_ORDERBY_ROWID, FALSE, &results, NULL) ==
      FALSE || !results)
    {
      if (previous_sync_map_id != NULL)
        g_free(previous_sync_map_id);
      dupin_database_unref (db);
      return FALSE;
    }

  if (g_list_length (results) != count)
    ret = FALSE;

g_message("dupin_view_sync_thread_map_db(%p)    g_list_length (results) = %d\n", g_thread_self (), (gint) g_list_length (results) );

  gsize total_processed = (gint)start_rowid;

  for (list = results; list; list = list->next)
    {
      /* NOTE - we do *not* count deleted records are processed */

      struct dupin_view_sync_t *data =
	g_malloc0 (sizeof (struct dupin_view_sync_t));

      JsonNode * obj = dupin_record_get_revision (list->data, -1);

      if (obj)
        data->obj = json_node_copy (obj);

      data->pid = json_node_new (JSON_NODE_ARRAY);
      JsonArray *pid_array=json_array_new ();

      rowid = dupin_record_get_rowid (list->data);

//g_message("dupin_view_sync_thread_map_db()  total_processed=%d\n", (gint)total_processed);

      if (sync_map_id != NULL)
        g_free(sync_map_id);
        
      sync_map_id = g_strdup_printf ("%i", (gint)rowid);

//g_message("dupin_view_sync_thread_map_db(%p) sync_map_id=%s as fetched",g_thread_self (), sync_map_id);

      json_array_add_string_element (pid_array, (gchar *) dupin_record_get_id (list->data));
      json_node_take_array (data->pid, pid_array);

      /* NOTE - key not set for dupin_view_sync_thread_map_db() - see dupin_view_sync_thread_map_view() instead */

      l = g_list_append (l, data);

      if (total_processed == view->sync_map_total_records)
        {
          ret=FALSE;
          break;
        }

      total_processed++;
    }

  dupin_view_sync_thread_real_map (view, l);

  /* g_list_foreach (l, (GFunc) g_free, NULL); */
  /* NOTE - free each list JSON node properly - the following is not freeing the json_node_copy() above */
  for (; l; l = l->next)
    {
      struct dupin_view_sync_t *data = l->data;
      if (data->obj)
        json_node_free (data->obj);
      json_node_free (data->pid);
      g_free (data);
    }
  g_list_free (l);
  dupin_record_get_list_close (results);

//g_message("dupin_view_sync_thread_map_db() sync_map_id=%s as to be stored",sync_map_id);

  if (previous_sync_map_id != NULL)
    g_free(previous_sync_map_id);

  str = sqlite3_mprintf ("UPDATE DupinView SET sync_map_id = '%q'", sync_map_id);

  if (sync_map_id != NULL)
    g_free (sync_map_id);

  g_mutex_lock (view->mutex);

  if (sqlite3_exec (view->db, str, NULL, NULL, &errmsg) != SQLITE_OK)
    {
      g_mutex_unlock (view->mutex);
      sqlite3_free (str);

      dupin_database_unref (db);

      g_error("dupin_view_sync_thread_map_db: %s", errmsg);
      sqlite3_free (errmsg);

      return FALSE;
    }

  g_mutex_unlock (view->mutex);

  sqlite3_free (str);

  dupin_database_unref (db);
  return ret;
}


static gboolean
dupin_view_sync_thread_map_view (DupinView * view, gsize count)
{
  DupinView *v;
  GList *results, *list;
  gchar * sync_map_id = NULL;
  gsize rowid;

  GList *l = NULL;
  gboolean ret = TRUE;

  gchar *str;
  gchar *errmsg;

  if (!(v = dupin_view_open (view->d, view->parent, NULL)))
    return FALSE;

  /* get last position we reduced and get anything up to count after that */
  gchar * previous_sync_map_id=NULL;
  gchar * query = "SELECT sync_map_id as c FROM DupinView";
  g_mutex_lock (view->mutex);

  if (sqlite3_exec (view->db, query, dupin_view_sync_cb, &previous_sync_map_id, &errmsg) != SQLITE_OK)
    {
      g_mutex_unlock (view->mutex);

      g_error("dupin_view_sync_thread_map_view: %s", errmsg);
      sqlite3_free (errmsg);

      dupin_view_unref (v);
      return FALSE;
    }

  g_mutex_unlock (view->mutex);

  gsize start_rowid = (previous_sync_map_id != NULL) ? atoi(previous_sync_map_id)+1 : 1;

  if (dupin_view_record_get_list (v, count, 0, start_rowid, 0, DP_ORDERBY_ROWID, FALSE, FALSE, &results, NULL) ==
      FALSE || !results)
    {
      if (previous_sync_map_id != NULL)
        g_free(previous_sync_map_id);
      dupin_view_unref (v);
      return FALSE;
    }

  if (g_list_length (results) != count)
    ret = FALSE;

g_message("dupin_view_sync_thread_map_view(%p)    g_list_length (results) = %d\n", g_thread_self (), (gint) g_list_length (results) );

  gsize total_processed = (gint)start_rowid;

  for (list = results; list; list = list->next)
    {
      /* NOTE - views are read-only we do not have to skip deleted records - see dupin_view_sync_thread_map_db() instead */

      struct dupin_view_sync_t *data =
	g_malloc0 (sizeof (struct dupin_view_sync_t));

      JsonNode * obj = dupin_view_record_get (list->data);

      if (obj)
        data->obj = json_node_copy (obj);

      /* TODO - check shouldn't this be more simply json_node_copy (dupin_view_record_get_pid (list->data))  or not ?! */
      data->pid = json_node_new (JSON_NODE_ARRAY);
      JsonArray *pid_array=json_array_new ();

      /* NOTE - key not set for dupin_view_sync_thread_map_db() - see dupin_view_sync_thread_map_view() instead */

      rowid = dupin_record_get_rowid (list->data);

      if (sync_map_id != NULL)
        g_free(sync_map_id);
        
      sync_map_id = g_strdup_printf ("%i", (gint)rowid);

      json_array_add_string_element (pid_array, (gchar *) dupin_view_record_get_id (list->data));
      json_node_take_array (data->pid, pid_array);

//g_message("dupin_view_sync_thread_map_view() sync_map_id=%s as fetched",sync_map_id);

      JsonNode * key = dupin_view_record_get_key (list->data);

      if (key)
        data->key = json_node_copy (key);

      l = g_list_append (l, data);

      if (total_processed == view->sync_map_total_records)
        {
          ret=FALSE;
          break;
        }

      total_processed++;
    }

  dupin_view_sync_thread_real_map (view, l);

  /* g_list_foreach (l, (GFunc) g_free, NULL); */
  /* NOTE - free each list JSON node properly - the following is not freeing the json_node_copy() above */
  for (; l; l = l->next)
    {
      struct dupin_view_sync_t *data = l->data;
      if (data->obj)
        json_node_free (data->obj);
      if (data->key)
        json_node_free (data->key);
      json_node_free (data->pid);
      g_free (data);
    }
  g_list_free (l);
  dupin_view_record_get_list_close (results);

//g_message("dupin_view_sync_thread_map_view() sync_map_id=%s as to be stored",sync_map_id);

  if (previous_sync_map_id != NULL)
    g_free(previous_sync_map_id);

  str = sqlite3_mprintf ("UPDATE DupinView SET sync_map_id = '%q'", sync_map_id);

  if (sync_map_id != NULL)
    g_free (sync_map_id);

//g_message("dupin_view_sync_thread_map_view() query=%s\n",str);

  g_mutex_lock (view->mutex);

  if (sqlite3_exec (view->db, str, NULL, NULL, &errmsg) != SQLITE_OK)
    {
      g_mutex_unlock (view->mutex);
      sqlite3_free (str);

      dupin_view_unref (v);

      g_error("dupin_view_sync_thread_map_view: %s", errmsg);
      sqlite3_free (errmsg);

      return FALSE;
    }

  g_mutex_unlock (view->mutex);

  sqlite3_free (str);

  dupin_view_unref (v);
  return ret;
}

static gboolean
dupin_view_sync_thread_map (DupinView * view, gsize count)
{
  if (view->parent_is_db == TRUE)
    return dupin_view_sync_thread_map_db (view, count);

  return dupin_view_sync_thread_map_view (view, count);
}

void
dupin_view_sync_record_update (DupinView * view, gchar * previous_rowid, gint replace_rowid,
                          gchar * key, gchar * value, gchar * pid)
{
  gchar *query, *errmsg;
  gchar *replace_rowid_str=NULL;
  replace_rowid_str = g_strdup_printf ("%d", (gint)replace_rowid);

/* TODO - escape keys due we do not catch erros below !!!!! */

  query = sqlite3_mprintf ("DELETE FROM Dupin WHERE key='%q' AND ROWID > %q AND ROWID < %q ;",
				key,
				(previous_rowid != NULL) ? previous_rowid : "0",
				replace_rowid_str);

//g_message("dupin_view_sync_record_update() delete query=%s\n",query);

  g_mutex_lock (view->mutex);

  if (sqlite3_exec (view->db, query, NULL, NULL, &errmsg) != SQLITE_OK)
    {
      g_mutex_unlock (view->mutex);
      sqlite3_free (query);
      g_free (replace_rowid_str);

      g_error("dupin_view_sync_record_update: %s", errmsg);
      sqlite3_free (errmsg);

      return;
    }

  g_mutex_unlock (view->mutex);

  sqlite3_free (query);

  query = sqlite3_mprintf ("UPDATE Dupin SET key='%q', pid='%q', obj='%q' WHERE rowid=%q ;",
				key,
				pid,
				value,
				replace_rowid_str);

//g_message("dupin_view_sync_record_update() update query=%s\n",query);

  g_mutex_lock (view->mutex);

  if (sqlite3_exec (view->db, query, NULL, NULL, &errmsg) != SQLITE_OK)
    {
      g_mutex_unlock (view->mutex);
      sqlite3_free (query);
      g_free (replace_rowid_str);

      g_error("dupin_view_sync_record_update: %s", errmsg);
      sqlite3_free (errmsg);

      return;
    }

  g_mutex_unlock (view->mutex);

  sqlite3_free (query);

  g_free (replace_rowid_str);
}

static gboolean
dupin_view_sync_thread_reduce (DupinView * view, gsize count, gboolean rereduce)
{
  if (view->reduce == NULL)
    return FALSE;

g_message("dupin_view_sync_thread_reduce(%p) count=%d\n",g_thread_self (), (gint)count);

  GList *results, *list;
  GList *nodes, *n;
  gchar * sync_reduce_id = NULL;

  gboolean ret = TRUE;

  gchar *str, *errmsg;

  JsonGenerator * gen=NULL;
  JsonNode * key=NULL;
  JsonNode * pid=NULL;
  JsonNode * reduce_parameters_obj_key=NULL;
  JsonObject * reduce_parameters_obj_key_o=NULL;
  JsonArray * reduce_parameters_obj_key_keys=NULL;
  JsonArray * reduce_parameters_obj_key_keys_i=NULL;
  JsonArray * reduce_parameters_obj_key_values=NULL;
  JsonArray * reduce_parameters_obj_key_pids=NULL;
  JsonNode  * reduce_parameters_obj_key_rowid=NULL;
  gsize rowid;
  gchar * key_string = NULL;
  gchar * query;

  /* get last position we reduced and get anything up to count after that */
  gchar * previous_sync_reduce_id=NULL;
  query = "SELECT sync_reduce_id as c FROM DupinView";
  g_mutex_lock (view->mutex);

  if (sqlite3_exec (view->db, query, dupin_view_sync_cb, &previous_sync_reduce_id, &errmsg) != SQLITE_OK)
    {
      g_mutex_unlock (view->mutex);

      g_error("dupin_view_sync_thread_reduce: %s", errmsg);
      sqlite3_free (errmsg);

      return FALSE;
    }

  g_mutex_unlock (view->mutex);

  gsize start_rowid = (previous_sync_reduce_id != NULL) ? atoi(previous_sync_reduce_id)+1 : 1;

  if (dupin_view_record_get_list (view, count, 0, start_rowid, 0, (rereduce) ? DP_ORDERBY_KEY : DP_ORDERBY_ROWID, FALSE, rereduce, &results, NULL) ==
      FALSE || !results)
    {
      if (previous_sync_reduce_id != NULL)
        g_free(previous_sync_reduce_id);

      return FALSE;
    }

  if (g_list_length (results) != count)
    ret = FALSE;

g_message("dupin_view_sync_thread_reduce(%p)    g_list_length (results) = %d\n", g_thread_self (), (gint) g_list_length (results) );

  gsize total_processed = (gint)start_rowid;

  JsonNode * reduce_parameters = json_node_new (JSON_NODE_OBJECT);
  JsonObject * reduce_parameters_obj = json_object_new ();

  for (list = results; list; list = list->next)
    {
      /* NOTE - views are read-only we do not have to skip deleted records - see dupin_view_sync_thread_map_db() instead */

      key = dupin_view_record_get_key (list->data);
      rowid = dupin_view_record_get_rowid (list->data);
      pid = dupin_view_record_get_pid (list->data);

      /* NOTE - silently ignore bad records for the moment assuming 'null' is returned as valid JSON_NODE_NULL from above call */
      if (!key)
        continue;

      key = json_node_copy (key);

      if (pid)
        {
          pid = json_array_get_element (json_node_get_array (pid), 0);
          if (pid)
            pid = json_node_copy (pid);
        }
 
      if (!pid)
        pid = json_node_new (JSON_NODE_NULL);
 
      if (json_node_get_node_type (key) == JSON_NODE_VALUE)
        {
          if (json_node_get_value_type (key) == G_TYPE_STRING)
          {
            key_string = g_strdup_printf ("\"%s\"", json_node_get_string (key));
          }

          if (json_node_get_value_type (key) == G_TYPE_DOUBLE
                || json_node_get_value_type (key) == G_TYPE_FLOAT)
          {
            gdouble numb = json_node_get_double (key);
            key_string = g_strdup_printf ("%f", numb);
          }

          if (json_node_get_value_type (key) == G_TYPE_INT
                || json_node_get_value_type (key) == G_TYPE_INT64
                || json_node_get_value_type (key) == G_TYPE_UINT)
          {
            gint numb = (gint) json_node_get_int (key);
            key_string = g_strdup_printf ("%d", numb);
          }
          if (json_node_get_value_type (key) == G_TYPE_BOOLEAN)
          {
            key_string = g_strdup_printf (json_node_get_boolean (key) == TRUE ? "true" : "false");
          }
        }
      else
        {
          gen = json_generator_new();
          json_generator_set_root (gen, key );
          key_string = json_generator_to_data (gen,NULL);
          g_object_unref (gen);
        }

//g_message("key_string =%s\n",key_string);

      reduce_parameters_obj_key = json_object_get_member (reduce_parameters_obj, key_string);

      if (!reduce_parameters_obj_key)
        {
          reduce_parameters_obj_key = json_node_new (JSON_NODE_OBJECT);
          reduce_parameters_obj_key_o = json_object_new ();
          json_node_take_object (reduce_parameters_obj_key, reduce_parameters_obj_key_o);

          reduce_parameters_obj_key_keys = json_array_new ();
          reduce_parameters_obj_key_values = json_array_new ();
          reduce_parameters_obj_key_pids = json_array_new ();
          reduce_parameters_obj_key_rowid = json_node_new (JSON_NODE_VALUE);

          json_object_set_array_member (reduce_parameters_obj_key_o, "keys", reduce_parameters_obj_key_keys);
          json_object_set_array_member (reduce_parameters_obj_key_o, "values", reduce_parameters_obj_key_values);
          json_object_set_array_member (reduce_parameters_obj_key_o, "pids", reduce_parameters_obj_key_pids);
          json_object_set_member (reduce_parameters_obj_key_o, "rowid", reduce_parameters_obj_key_rowid);

          /* TODO - check if we need double to gurantee larger number of rowids and use json_node_set_number () */
          json_node_set_int (reduce_parameters_obj_key_rowid, rowid);

          json_object_set_member (reduce_parameters_obj, key_string, reduce_parameters_obj_key);
        }
      else
        {
//dupin_view_debug_print_json_node("Key did exist \n",reduce_parameters_obj_key);

          reduce_parameters_obj_key_o = json_node_get_object (reduce_parameters_obj_key);

          reduce_parameters_obj_key_keys = json_node_get_array (json_object_get_member (reduce_parameters_obj_key_o, "keys"));
          reduce_parameters_obj_key_values = json_node_get_array (json_object_get_member (reduce_parameters_obj_key_o, "values"));
          reduce_parameters_obj_key_pids = json_node_get_array (json_object_get_member (reduce_parameters_obj_key_o, "pids"));
          reduce_parameters_obj_key_rowid = json_object_get_member (reduce_parameters_obj_key_o, "rowid");

          if ( json_node_get_int (reduce_parameters_obj_key_rowid) < rowid )
            json_node_set_int (reduce_parameters_obj_key_rowid, rowid);
        }

      /* i-esim [k,pid] pair */
      reduce_parameters_obj_key_keys_i = json_array_new ();
      json_array_add_element (reduce_parameters_obj_key_keys_i, key);
      json_array_add_element (reduce_parameters_obj_key_keys_i, pid);
      json_array_add_array_element (reduce_parameters_obj_key_keys, reduce_parameters_obj_key_keys_i);

      /* i-esim value */
      JsonNode * reduce_parameters_obj_key_values_i = dupin_view_record_get (list->data);
      if (reduce_parameters_obj_key_values_i)
        {
          reduce_parameters_obj_key_values_i = json_object_get_member (json_node_get_object (reduce_parameters_obj_key_values_i), "value");
          if (reduce_parameters_obj_key_values_i)
            reduce_parameters_obj_key_values_i = json_node_copy (reduce_parameters_obj_key_values_i);
        }

      if (!reduce_parameters_obj_key_values_i)
          reduce_parameters_obj_key_values_i = json_node_new (JSON_NODE_NULL);

      json_array_add_element (reduce_parameters_obj_key_values, reduce_parameters_obj_key_values_i);
      json_array_add_element (reduce_parameters_obj_key_pids, json_node_copy (pid));

//g_message("dupin_view_sync_thread_reduce(%p)  total_processed=%d\n", g_thread_self (), (gint)total_processed);

      if (sync_reduce_id != NULL)
        g_free(sync_reduce_id);
        
      sync_reduce_id = g_strdup_printf ("%i", (gint)rowid);

//g_message("dupin_view_sync_thread_reduce(%p) sync_reduce_id=%s as fetched",g_thread_self (), sync_reduce_id);

      g_free (key_string);

      if (total_processed == view->sync_reduce_total_records)
        {
          ret=FALSE;
          break;
        }

      total_processed++;
    }

  g_mutex_lock (view->mutex);
  view->sync_reduce_processed_count = total_processed;
  g_mutex_unlock (view->mutex);

  json_node_take_object (reduce_parameters, reduce_parameters_obj);

//dupin_view_debug_print_json_node ("REDUCE parameters:", reduce_parameters);

  nodes = json_object_get_members (reduce_parameters_obj);
  for (n = nodes; n != NULL; n = n->next)
    {
      gchar *member_name = (gchar *) n->data;

      /* call reduce for each group of keys */

      /* call function(keys, values, rereduce)  values = [ v1, v2... vN ] */

      JsonNode * result = dupin_mr_record_reduce (view,
						  (rereduce) ? NULL : json_node_get_array (json_object_get_member ( json_node_get_object(json_object_get_member (reduce_parameters_obj, member_name)), "keys")),
						  json_node_get_array (json_object_get_member ( json_node_get_object(json_object_get_member (reduce_parameters_obj, member_name)), "values")),
						  rereduce);

      if (result != NULL)
        {
          gchar * result_string=NULL;
          gchar * pids_string=NULL;
          JsonNode * result_obj_node = json_node_new (JSON_NODE_OBJECT);
          JsonObject * result_obj = json_object_new ();

          json_node_take_object (result_obj_node, result_obj);
          
          json_object_set_member (result_obj, "value", result);

	  JsonParser * parser = json_parser_new ();

          if (json_parser_load_from_data (parser, member_name, strlen (member_name), NULL) == FALSE)
            {
              if (parser != NULL)
                g_object_unref (parser);
              json_node_free (result_obj_node);
              g_free (result_string);
              g_free (pids_string);

	      ret = FALSE;
              break;
            }

          json_object_set_member (result_obj, "key", json_node_copy (json_parser_get_root (parser)));

          if (parser != NULL)
            g_object_unref (parser);

          gen = json_generator_new();
          json_generator_set_root (gen, result_obj_node );
          result_string = json_generator_to_data (gen,NULL);
          g_object_unref (gen);

          json_node_free (result_obj_node);

//g_message ("RESULT:%s\n", result_string);

          gen = json_generator_new();
          json_generator_set_root (gen, json_object_get_member ( json_node_get_object(json_object_get_member (reduce_parameters_obj, member_name)), "pids") );
          pids_string = json_generator_to_data (gen,NULL);
          g_object_unref (gen);

          /* TODO - check if we need double to gurantee larger number of rowids and use json_node_get_number () in the below */

	  /* delete all rows but last one and replace last one with result where last one is rowid */
          dupin_view_sync_record_update (view,
				    previous_sync_reduce_id,
				    (gint)json_node_get_int (json_object_get_member ( json_node_get_object(json_object_get_member (reduce_parameters_obj, member_name)), "rowid")),
                                    member_name,
                                    result_string,
				    pids_string);

          g_free (result_string);
          g_free (pids_string);
        }

      /* just append to the end for the moment - DEBUG */
    }
  g_list_free (nodes);

  if (previous_sync_reduce_id != NULL)
    g_free(previous_sync_reduce_id);
  
  json_node_free (reduce_parameters); /* it shoulf freee the whole tree of objects, arrays and value ... */

  dupin_view_record_get_list_close (results);

//g_message("dupin_view_sync_thread_reduce() sync_reduce_id=%s as to be stored",sync_reduce_id);

  str = sqlite3_mprintf ("UPDATE DupinView SET sync_reduce_id = '%q'", sync_reduce_id); /* is the ROWID we stopped */

  if (sync_reduce_id != NULL)
    g_free(sync_reduce_id);

//g_message("dupin_view_sync_thread_reduce() query=%s\n",str);

  g_mutex_lock (view->mutex);

  if (sqlite3_exec (view->db, str, NULL, NULL, &errmsg) != SQLITE_OK)
    {
      g_mutex_unlock (view->mutex);
      sqlite3_free (str);

      g_error("dupin_view_sync_thread_reduce: %s", errmsg);
      sqlite3_free (errmsg);

      return FALSE;
    }

  g_mutex_unlock (view->mutex);

  sqlite3_free (str);

  return ret;
}

static int
dupin_view_sync_total_rereduce_cb (void *data, int argc, char **argv,
                                  char **col)
{
  gsize *numb = data;

  if (argv[0])
    *numb = atoi (argv[0]);

  return 0;
}

/* NOTE - bear in mind SQLite might be able to store more than gsize total records
          see also ROWID and http://www.sqlite.org/autoinc.html */

gboolean
dupin_view_sync_total_rereduce (DupinView * view, gsize * total)
{
  g_return_val_if_fail (view != NULL, FALSE);

  gchar *tmp, *errmsg;

  *total = 0;

  tmp = sqlite3_mprintf (DUPIN_VIEW_SQL_TOTAL_REREDUCE);

  g_mutex_lock (view->mutex);

  if (sqlite3_exec (view->db, tmp, dupin_view_sync_total_rereduce_cb, total, &errmsg) != SQLITE_OK)
    {
      g_mutex_unlock (view->mutex);
      sqlite3_free (tmp);

      g_error("dupin_view_sync_total_rereduce: %s", errmsg);
      sqlite3_free (errmsg);

      return FALSE;
    }

  g_mutex_unlock (view->mutex);

  sqlite3_free (tmp);

  return TRUE;
}

static gpointer
dupin_view_sync_map_thread (DupinView * view)
{
  gchar * errmsg;

  dupin_view_ref (view);

g_message("dupin_view_sync_map_thread(%p) started with %d total records to MAP\n",g_thread_self (), (gint)view->sync_map_total_records);

  g_mutex_lock (view->mutex);
  view->sync_map_processed_count = 0;
  g_mutex_unlock (view->mutex);

  while (view->sync_toquit == FALSE || view->todelete == FALSE)
    {
      gboolean map_operation = dupin_view_sync_thread_map (view, VIEW_SYNC_COUNT);

      /* NOTE - make sure waiting reduce thread is started as soon as the first set of mapped results is ready 
                 the sync_map_processed_count is set to the total of mapped results so far and used for very basic IPC map -> reduce threads */

      if (view->reduce != NULL)
        {
g_message("dupin_view_sync_map_thread(%p) Mapped %d records out of %d in total\n", g_thread_self (), (gint)view->sync_map_processed_count, (gint)view->sync_map_total_records);

          g_mutex_lock (view->mutex);
	  g_cond_signal(view->sync_map_has_new_work);
          g_mutex_unlock (view->mutex);
        }

      if (map_operation == FALSE)
        {
g_message("dupin_view_sync_map_thread(%p) Mapped TOTAL %d records out of %d in total\n", g_thread_self (), (gint)view->sync_map_processed_count, (gint)view->sync_map_total_records);

          gchar *query = "UPDATE DupinView SET sync_map_id = NULL";

          g_mutex_lock (view->mutex);

           if (sqlite3_exec (view->db, query, NULL, NULL, &errmsg) != SQLITE_OK)
             {
               g_mutex_unlock (view->mutex);

               g_error("dupin_view_sync_map_thread: %s", errmsg);
               sqlite3_free (errmsg);

               break;
             }

          g_mutex_unlock (view->mutex);

          break;
        }
    }

g_message("dupin_view_sync_map_thread(%p) finished to map %d total records and view map part is in sync\n",g_thread_self (), (gint)view->sync_map_total_records);

  g_mutex_lock (view->mutex);
  view->sync_map_thread = NULL;
  g_mutex_unlock (view->mutex);
  dupin_view_unref (view);
  g_thread_exit (NULL);

  return NULL;
}

static gpointer
dupin_view_sync_reduce_thread (DupinView * view)
{
  gchar * query;
  gchar * errmsg;

  dupin_view_ref (view);

  g_mutex_lock (view->mutex);
  view->sync_reduce_processed_count = 0;
  view->sync_reduce_total_records = 0;
  g_mutex_unlock (view->mutex);

  gboolean rereduce = FALSE;
  gsize total_rereduce = 0;

g_message("dupin_view_sync_reduce_thread(%p) started", g_thread_self ());

  /* NOTE - if map hangs, reduce also hangs - for the moment we should make sure a _rest method is allowed on views to avoid disasters */

  /* TODO - added processing step when restarted and sync_reduce_id is set to the ID of view table latest record processed, and continue
            and when done, wait for another bunch ... */

  while (view->sync_toquit == FALSE || view->todelete == FALSE)
    {
g_message("rereduce=%d\n", rereduce);

      if (rereduce == FALSE)
        {
          g_mutex_lock (view->mutex);
          g_cond_wait(view->sync_map_has_new_work, view->mutex);
          g_mutex_unlock (view->mutex);
        }

      if (view->sync_map_processed_count > view->sync_reduce_total_records /* got a new bunch to work on */
	  || rereduce)
        {
          g_mutex_lock (view->mutex);
          view->sync_reduce_total_records = (rereduce) ? total_rereduce : view->sync_map_processed_count;
          g_mutex_unlock (view->mutex);

g_message("dupin_view_sync_reduce_thread(%p) got %d records to REDUCE (rereduce=%d)\n",g_thread_self (), (gint)view->sync_reduce_total_records,(gint)rereduce);

          while (dupin_view_sync_thread_reduce (view, VIEW_SYNC_COUNT, rereduce) == TRUE);

g_message("dupin_view_sync_reduce_thread(%p) Reduced %d records of %d\n", g_thread_self (), (gint)view->sync_reduce_processed_count, (gint)view->sync_reduce_total_records);
        }

      if (!view->sync_map_thread) /* map finished */
        {
g_message("Map was finished in meantime\n");

	  /* check if there is anything to re-reduce */
          total_rereduce = 0;
          dupin_view_sync_total_rereduce (view, &total_rereduce);

g_message("Done first round of reduce but there are still %d record to re-reduce\n", (gint)total_rereduce);

/* TODO - fix this it causes an infinite loop !!!! */

          query = "UPDATE DupinView SET sync_reduce_id = NULL";

          g_mutex_lock (view->mutex);

          if (sqlite3_exec (view->db, query, NULL, NULL, &errmsg) != SQLITE_OK)
            {
              g_mutex_unlock (view->mutex);

              g_error("dupin_view_sync_reduce_thread: %s", errmsg);
              sqlite3_free (errmsg);

              break;
            }

          g_mutex_unlock (view->mutex);

	  /* TODO - add re-reduce step and iter till done before terminating reduce thread */

          if (total_rereduce > 0)
            {
              /* still work to do */
              rereduce = TRUE;
g_message("Going to re-reduce\n");
            }
          else
            {
g_message("Done rereduce=%d\n", (gint)rereduce);
              rereduce = FALSE;

	      break; /* both terminated, amen */
            }
        }
    }

g_message("dupin_view_sync_reduce_thread(%p) finished to reduce %d total records and view reduce part is in sync\n",g_thread_self (), (gint)view->sync_reduce_total_records);

  g_mutex_lock (view->mutex);
  view->sync_reduce_thread = NULL;
  g_mutex_unlock (view->mutex);
  dupin_view_unref (view);
  g_thread_exit (NULL);

  return NULL;
}

/* NOTE- we try to spawn two threads map, reduce 
         when reduce is done we re-reduce till no map and reduce is still running,
         finished scan and only one key is left (count=1) */

void
dupin_view_sync (DupinView * view)
{
  gchar *sync_map_id = NULL;
  gchar *sync_reduce_id = NULL;
  gchar *query;
  gchar *errmsg;

  /* TODO - have a master sync thread which manage the all three raher than have chain of
            dependency between map, reduce and re-reduce threads */

  if (!view->sync_map_thread)
    {
      query = "SELECT sync_map_id as c FROM DupinView";

      g_mutex_lock (view->mutex);

      if (sqlite3_exec (view->db, query, dupin_view_sync_cb, &sync_map_id, &errmsg) !=
           SQLITE_OK)
        {
          g_mutex_unlock (view->mutex);

          g_error("dupin_view_sync: %s", errmsg);
          sqlite3_free (errmsg);

          return;
        }

      if (dupin_view_get_total_records (view->d, view->parent, view->parent_is_db, &view->sync_map_total_records) == FALSE)
        {
          g_mutex_unlock (view->mutex);
          return;
        }

      if (sync_map_id != NULL)
        {
          view->sync_map_thread =
	        g_thread_create ((GThreadFunc) dupin_view_sync_map_thread, view, FALSE, NULL);

          g_free (sync_map_id);
        }
      g_mutex_unlock (view->mutex);
    }

  if (view->reduce != NULL
      && !view->sync_reduce_thread)
    {
      query = "SELECT sync_reduce_id as c FROM DupinView";

      g_mutex_lock (view->mutex);

      if (sqlite3_exec (view->db, query, dupin_view_sync_cb, &sync_reduce_id, &errmsg) !=
           SQLITE_OK)
        {
          g_mutex_unlock (view->mutex);

          g_error("dupin_view_sync: %s", errmsg);
          sqlite3_free (errmsg);

          return;
        }

      if (sync_reduce_id != NULL
          || view->sync_map_thread)
        {
          view->sync_reduce_thread =
	    g_thread_create ((GThreadFunc) dupin_view_sync_reduce_thread, view, FALSE, NULL);

	  g_free (sync_reduce_id);
        }

      g_mutex_unlock (view->mutex);
    }
}

gboolean
dupin_view_is_sync (DupinView * view)
{
  g_return_val_if_fail (view != NULL, FALSE);

  if (view->sync_map_thread
      || view->sync_reduce_thread)
    return FALSE;

  return TRUE;
}

/* EOF */
