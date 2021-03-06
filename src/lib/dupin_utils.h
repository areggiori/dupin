#ifndef _DUPIN_UTILS_H_
#define _DUPIN_UTILS_H_

#include <dupin.h>
#include <sqlite3.h>
#include <errno.h>
#include <stdlib.h>

G_BEGIN_DECLS

#define DUPIN_UTIL_DUMP_JSON(msg, node) do { \
        gchar * string = dupin_util_json_serialize (node); \
	g_message ("%s: %s", msg, string); \
	g_free (string); \
        } while (0)

gboolean	dupin_util_is_valid_db_name	(gchar *	db);

gboolean	dupin_util_is_valid_linkb_name	(gchar *	linkb);

gboolean	dupin_util_is_valid_view_name	(gchar *	view);

gboolean	dupin_util_is_valid_attachment_db_name
						(gchar * attachment_db);

gint		dupin_util_dupin_mode_to_sqlite_mode
						(DupinSQLiteOpenType dupin_mode);

gboolean	dupin_util_is_valid_record_id	(gchar *	id);

gboolean	dupin_util_is_valid_record_type	(gchar *	type);

gboolean	dupin_util_is_valid_absolute_uri
						(gchar *	uri);

gchar *		dupin_util_json_strescape	(const gchar *	string);

gchar *		dupin_util_json_serialize	(JsonNode * node);

gchar *		dupin_util_json_value_to_string (JsonNode * node);

JsonNode *	dupin_util_json_node_clone	(JsonNode * node,
						 GError **  error);

JsonNode *     	dupin_util_json_node_object_patch
						(JsonNode * input,
						 JsonNode * changes); 

JsonNode * 	dupin_util_json_node_object_filter_fields
						(JsonNode * node,
						 DupinFieldsFormatType format,
						 gchar **   fields,
						 gboolean not,
						 GError **  error);

JsonNode * 	dupin_util_json_node_object_grep_nodes
						(JsonNode * node,
						 DupinFieldsFormatType format,
						 gchar **   fields,
						 DupinFieldsFormatType filter_op,
						 gchar **   filter_values,
						 GError **  error);

gboolean	dupin_util_poli_is_primary_field
						(gchar **   profiles,
						 gchar *    type,
						 gchar *    field_name,
						 GError **  error);

GList * 	dupin_util_poli_get_primary_fields
						(gchar **   profiles,
						 gchar *    type,
						 JsonNode * obj_node,
						 GError **  error);

void		dupin_util_poli_get_primary_fields_list_close
						(GList * primary_fields);

void		dupin_sqlite_json_filterby	(sqlite3_context *ctx,
						 int argc,
						 sqlite3_value **argv);

gint		dupin_sqlite_subs_mgr_busy_handler
						(sqlite3* dbconn,
						 gchar *sql_stmt,
						 gint (*callback_func)(void *, gint, char **, gchar **),
						 void *args,
						 gchar **error_msg,
						 gint rc);

gboolean	dupin_util_is_valid_obj		(JsonObject *obj);

gchar *		dupin_util_generate_id		(GError **  error);

gboolean	dupin_util_is_valid_view_engine_lang
						(gchar *	lang);

DupinViewEngineLang
		dupin_util_view_engine_lang_to_enum
						(gchar *	lang);

const gchar *	dupin_util_view_engine_lang_to_string
						(DupinViewEngineLang	lang);

gchar *		dupin_util_utf8_normalize	(const gchar *text);

gchar *		dupin_util_utf8_casefold_normalize
						(const gchar *text);

gint		dupin_util_utf8_compare
						(const gchar *t1, const gchar *t2);

gint		dupin_util_utf8_ncompare
						(const gchar *t1, const gchar *t2);

gint		dupin_util_utf8_casecmp		(const gchar *t1, const gchar *t2);

gint		dupin_util_utf8_ncasecmp	(const gchar *t1, const gchar *t2);

gchar *		dupin_util_utf8_create_key_gen 	(const gchar *text, gint case_sen,
						 gchar * (*keygen) (const gchar * text, gssize size));

gchar *		dupin_util_utf8_create_key	(const gchar *text, gint case_sen);

gchar *		dupin_util_utf8_create_key_for_filename
						(const gchar *text, gint case_sen);

gboolean        dupin_util_mvcc_new   		(guint revision,
                                       		 gchar * hash,
                                         	 gchar mvcc[DUPIN_ID_MAX_LEN]);

gboolean        dupin_util_is_valid_mvcc	(gchar * mvcc);

gint		dupin_util_mvcc_revision_cmp	(gchar * mvcc_a,
						 gchar * mvcc_b);

gboolean        dupin_util_mvcc_get_revision	(gchar * mvcc,
                                         	 guint * revision);

gboolean        dupin_util_mvcc_get_hash	(gchar * mvcc,
                                         	 gchar hash[DUPIN_ID_HASH_ALGO_LEN]);

DupinCollateType
		dupin_util_get_collate_type	(JsonNode * node);

int		dupin_util_collation		(void        * ref,
						 int         left_len,
						 const void  *left_void,
						 int         right_len,
						 const void  *right_void);

int		dupin_util_collation_compare_pair
						(JsonNode * left_node,
						 JsonNode * right_node);

gchar *        dupin_util_json_string_normalize	(gchar * input_string);

gchar *        dupin_util_json_string_normalize_docid
						(gchar * input_string_docid);

gchar *        dupin_util_json_string_normalize_rev
						(gchar * input_string_rev);

gboolean       dupin_util_http_if_none_match 	(gchar * header_if_none_match,
						 gchar * etag);

gboolean       dupin_util_http_if_modified_since
						(gchar * header_if_modified_since,
						 gsize last_modified);

/* k/v pairs for argument lists */

typedef struct dupin_keyvalue_t    dupin_keyvalue_t;

struct dupin_keyvalue_t
{
  gchar *       key;
  gchar *       value;
};

dupin_keyvalue_t * dupin_keyvalue_new         (gchar *        key,
                                         gchar *        value) G_GNUC_WARN_UNUSED_RESULT;

void            dupin_keyvalue_destroy     (dupin_keyvalue_t * data);

G_END_DECLS

#endif

/* EOF */
