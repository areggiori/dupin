#ifndef _DSERVER_H_
#define _DSERVER_H_

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#include <glib-object.h>

#include "../lib/dupin.h"

typedef struct ds_httpd_thread_t DSHttpdThread;
struct ds_httpd_thread_t
{
  DSGlobal *	data;
  GThread *	thread;

  GMutex *	mutex;

  GMainContext * context;
  GMainLoop *	loop;

  GSource *	timeout_source;

  GList *	clients;
  guint		clients_numb;
};

typedef struct ds_map_t DSMap;
struct ds_map_t
{
  gchar *	filename;
  time_t	mtime;

  gchar *	mime;
  GMappedFile *	map;

  GList *	unrefnode;
  guint		ref;
};

typedef enum
{
  DS_HTTPD_REQUEST_GET,
  DS_HTTPD_REQUEST_POST,
  DS_HTTPD_REQUEST_PUT,
  DS_HTTPD_REQUEST_DELETE
} HttpdRequest;

typedef enum
{
  DS_HTTPD_OUTPUT_NONE = 0,
  DS_HTTPD_OUTPUT_STRING,
  DS_HTTPD_OUTPUT_IO,
  DS_HTTPD_OUTPUT_MAP,
  DS_HTTPD_OUTPUT_BLOB,
  DS_HTTPD_OUTPUT_CHANGES_COMET
} DSHttpdOutputType;

typedef struct ds_httpd_client_t DSHttpdClient;
struct ds_httpd_client_t
{
  DSHttpdThread * thread;

  gchar *	ip;

  GIOChannel *	channel;

  GSource *	timeout_source;
  GSource *	channel_source;

  GList *	headers;
  guint		headers_numb;
 
  HttpdRequest	request;
  GList *	request_path;
  GList *	request_arguments;

  gchar *	body;
  gsize		body_size;
  gsize		body_done;

  DSHttpdOutputType output_type;

  gchar *	output_header;
  gsize		output_header_size;
  gsize		output_header_done;

  gchar *	input_mime;
  gchar *	output_mime;

  gsize		output_size;

  gchar *	dupin_error_msg;

  union
  {
    struct
    {
      gchar *	string;
      gsize	done;
    } string;

    struct
    {
      GIOChannel *	io;
      gchar 		string[4096];
      gsize		size;
      gsize		done;
    } io;

    struct
    {
      gchar *	contents;
      gsize	done;
      DSMap *	map;
    } map;

    struct
    {
      DupinAttachmentRecord * record;

      gchar 		string[4096];
      gsize		size;
      gsize		done;
      gsize		offset;
    } blob;

    struct
    {
      DupinDB *		   db;
      DupinLinkB *	   linkb;

      gchar 		   string[4096];
      gsize		   size;
      gsize		   done;
      gsize		   offset;

      gchar * 		   change_string;
      gsize		   change_size;
      gboolean 		   change_generated;
      guint		   change_errors;
      gsize		   change_last_seq;
      gsize		   change_total_changes;

      guint		   param_heartbeat;
      guint		   param_timeout;
      gboolean 		   param_descending;
      gsize		   param_since;
      DupinChangesFeedType param_feed;
      DupinChangesType	   param_style;
      gboolean		   param_include_docs;
      gboolean		   param_include_links;
      gchar *		   param_context_id;
      gchar *		   param_tag;
    } changes_comet;

  } output;
};

GQuark		ds_error_quark	(void);

#endif

/* EOF */
