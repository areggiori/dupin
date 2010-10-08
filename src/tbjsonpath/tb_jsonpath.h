/******************************************************************************
* 	Copyright (c) 2007 All rights reserved
*		Asemantics S.r.l
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions
* are met:
*
* 1. Redistributions of source code must retain the above copyright
*    notice, this list of conditions and the following disclaimer. 
*
* 2. Redistributions in binary form must reproduce the above copyright
*    notice, this list of conditions and the following disclaimer in
*    the documentation and/or other materials provided with the
*    distribution.
*
* 3. The end-user documentation included with the redistribution,
*    if any, must include the following acknowledgment:
*       "This product includes software developed by 
*	 Asemantics S.r.l."
*    Alternately, this acknowledgment may appear in the software itself,
*    if and wherever such third-party acknowledgments normally appear.
*
* 4. All advertising materials mentioning features or use of this software
*    must display the following acknowledgement:
*    This product includes software developed by Asemantics S.r.l.
*    the Semantic Web company, Rome, London, Leiden and its contributors. 
*
* 5. Neither the name of the company nor the names of its contributors
*    may be used to endorse or promote products derived from this software
*    without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
* ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
* PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE
* LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
* INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
* CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
* ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
* OF THE POSSIBILITY OF SUCH DAMAGE.
*
******************************************************************************/

#ifndef _TB_JSONPATH_H_
#define _TB_JSONPATH_H_

#include <glib.h>

#include <json-glib/json-glib.h>
#include <json-glib/json-gobject.h>

#include "tb_jsonpath_function.h"

G_BEGIN_DECLS

typedef struct tb_jsonpath_result_t tb_jsonpath_result_t;

gboolean	tb_jsonpath_validate	(gchar *		jsonpath,
					 gssize			size,
					 GError **		error);

gboolean	tb_jsonpath_exec	(gchar *		jsonpath,
					 gssize			size,
					 JsonObject *		object,
					 tb_jsonpath_result_t ** result,
					 tb_jsonpath_functions_t *
					 			functions,
					 GError **		error);

gboolean	tb_jsonpath_result_next (tb_jsonpath_result_t * result,
					 JsonNode **		value);

gboolean	tb_jsonpath_result_prev (tb_jsonpath_result_t * result,
					 JsonNode **		value);

gint		tb_jsonpath_result_length
					(tb_jsonpath_result_t * result);

void		tb_jsonpath_result_free
					(tb_jsonpath_result_t * result);

/* ERROR */
enum {
  TB_ERROR_JSONPATH
};

GQuark     	tb_jsonpath_error_quark		(void);	

G_END_DECLS

#endif

/* EOF */
