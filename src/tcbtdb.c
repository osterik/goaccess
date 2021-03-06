/**
 * tcbtdb.c -- Tokyo Cabinet database functions
 * Copyright (C) 2009-2014 by Gerardo Orellana <goaccess@prosoftcorp.com>
 * GoAccess - An Ncurses apache weblog analyzer & interactive viewer
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * A copy of the GNU General Public License is attached to this
 * source distribution for its full text.
 *
 * Visit http://goaccess.prosoftcorp.com for new releases.
 */

#include <errno.h>
#include <tcutil.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "tcbtdb.h"
#include "tcabdb.h"

#ifdef HAVE_LIBGEOIP
#include "geolocation.h"
#endif

#include "error.h"
#include "xmalloc.h"

#ifdef TCB_BTREE
char *
tc_db_set_path (const char *dbname, int module)
{
  char *path;
  int cx;

  if (conf.db_path != NULL) {
    cx = snprintf (NULL, 0, "%s%dm%s", conf.db_path, module, dbname) + 1;
    path = xmalloc (cx);
    sprintf (path, "%s%dm%s", conf.db_path, module, dbname);
  } else {
    cx = snprintf (NULL, 0, "%s%dm%s", TC_DBPATH, module, dbname) + 1;
    path = xmalloc (cx);
    sprintf (path, "%s%dm%s", TC_DBPATH, module, dbname);
  }

  return path;
}

static int
set_dbparam (char *params, int len, const char *fmt, ...)
{
  int n;
  va_list args;

  va_start (args, fmt);
  n = vsnprintf (params + len, DB_PARAMS - len, fmt, args);
  va_end (args);

  if (n < 0) {
    n = 0;
    LOG_DEBUG (("Output error is encountered on set_dbparam\n"));
  } else if (n >= DB_PARAMS - len) {
    LOG_DEBUG (("Output truncated on set_dbparam\n"));
    n = DB_PARAMS - len;
  }

  return n;
}

/* set database parameters */
void
tc_db_get_params (char *params, const char *path)
{
  int len = 0;
  long xmmap = conf.xmmap;
  uint32_t lcnum, ncnum, lmemb, nmemb, bnum;

  /* copy path name to buffer */
  len += set_dbparam (params, len, "%s", path);

  /* caching parameters of a B+ tree database object */
  lcnum = conf.cache_lcnum > 0 ? conf.cache_lcnum : TC_LCNUM;
  len += set_dbparam (params, len, "#%s=%d", "lcnum", lcnum);

  ncnum = conf.cache_ncnum > 0 ? conf.cache_ncnum : TC_NCNUM;
  len += set_dbparam (params, len, "#%s=%d", "ncnum", ncnum);

  /* set the size of the extra mapped memory */
  if (xmmap > 0)
    len += set_dbparam (params, len, "#%s=%ld", "xmsiz", xmmap);

  lmemb = conf.tune_lmemb > 0 ? conf.tune_lmemb : TC_LMEMB;
  len += set_dbparam (params, len, "#%s=%d", "lmemb", lmemb);

  nmemb = conf.tune_nmemb > 0 ? conf.tune_nmemb : TC_NMEMB;
  len += set_dbparam (params, len, "#%s=%d", "nmemb", nmemb);

  bnum = conf.tune_bnum > 0 ? conf.tune_bnum : TC_BNUM;
  len += set_dbparam (params, len, "#%s=%d", "bnum", bnum);

  /* compression */
  len += set_dbparam (params, len, "#%s=%c", "opts", 'l');

  if (conf.compression == TC_BZ2) {
    len += set_dbparam (params, len, "%c", 'b');
  } else if (conf.compression == TC_ZLIB) {
    len += set_dbparam (params, len, "%c", 'd');
  }

  /* open flags. create a new database if not exist, otherwise read it */
  len += set_dbparam (params, len, "#%s=%s", "mode", "wc");
  /* if not loading from disk, truncate regardless if a db file exists */
  if (!conf.load_from_disk)
    len += set_dbparam (params, len, "%c", 't');

  LOG_DEBUG (("%s\n", path));
  LOG_DEBUG (("params: %s\n", params));
}

/* Open the database handle */
TCBDB *
tc_bdb_create (const char *dbname, int module)
{
  TCBDB *bdb;
  char *path = NULL;
  int ecode;
  uint32_t lcnum, ncnum, lmemb, nmemb, bnum, flags;

  path = tc_db_set_path (dbname, module);
  bdb = tcbdbnew ();

  lcnum = conf.cache_lcnum > 0 ? conf.cache_lcnum : TC_LCNUM;
  ncnum = conf.cache_ncnum > 0 ? conf.cache_ncnum : TC_NCNUM;

  /* set the caching parameters of a B+ tree database object */
  if (!tcbdbsetcache (bdb, lcnum, ncnum)) {
    free (path);
    FATAL ("Unable to set TCB cache");
  }

  /* set the size of the extra mapped memory */
  if (conf.xmmap > 0 && !tcbdbsetxmsiz (bdb, conf.xmmap)) {
    free (path);
    FATAL ("Unable to set TCB xmmap.");
  }

  lmemb = conf.tune_lmemb > 0 ? conf.tune_lmemb : TC_LMEMB;
  nmemb = conf.tune_nmemb > 0 ? conf.tune_nmemb : TC_NMEMB;
  bnum = conf.tune_bnum > 0 ? conf.tune_bnum : TC_BNUM;

  /* compression */
  flags = BDBTLARGE;
  if (conf.compression == TC_BZ2) {
    flags |= BDBTBZIP;
  } else if (conf.compression == TC_ZLIB) {
    flags |= BDBTDEFLATE;
  }

  /* set the tuning parameters */
  tcbdbtune (bdb, lmemb, nmemb, bnum, 8, 10, flags);

  /* open flags */
  flags = BDBOWRITER | BDBOCREAT;
  if (!conf.load_from_disk)
    flags |= BDBOTRUNC;

  /* attempt to open the database */
  if (!tcbdbopen (bdb, path, flags)) {
    free (path);
    ecode = tcbdbecode (bdb);

    FATAL ("%s", tcbdberrmsg (ecode));
  }
  free (path);

  return bdb;
}

/* Close the database handle */
int
tc_bdb_close (void *db, char *dbname)
{
  TCBDB *bdb = db;
  int ecode;

  if (bdb == NULL)
    return 1;

  /* close the database */
  if (!tcbdbclose (bdb)) {
    ecode = tcbdbecode (bdb);
    FATAL ("%s", tcbdberrmsg (ecode));
  }
  /* delete the object */
  tcbdbdel (bdb);

  /* remove database file */
  if (!conf.keep_db_files && !tcremovelink (dbname))
    LOG_DEBUG (("Unable to remove DB: %s\n", dbname));
  free (dbname);

  return 0;
}

static int
find_int_key_in_list (void *data, void *needle)
{
  return (*(int *) data) == (*(int *) needle) ? 1 : 0;
}

static int
is_value_in_tclist (TCLIST * tclist, void *value)
{
  int i, sz;
  int *val;

  if (!tclist)
    return 0;

  for (i = 0; i < tclistnum (tclist); ++i) {
    val = (int *) tclistval (tclist, i, &sz);
    if (find_int_key_in_list (value, val))
      return 1;
  }

  return 0;
}

/* Insert a string key and the corresponding string value.
 * Note: If the key exists, the value is not replaced.
 * Return -1 if the operation failed, otherwise 0. */
int
ins_igsl (void *hash, int key, int value)
{
  TCLIST *list;
  int in_list = 0;

  if (!hash)
    return -1;

  /* key found, check if key exists within the list */
  if ((list = tcbdbget4 (hash, &key, sizeof (int))) != NULL) {
    if (is_value_in_tclist (list, &value))
      in_list = 1;
    tclistdel (list);
  }
  /* if not on the list, add it */
  if (!in_list && tcbdbputdup (hash, &key, sizeof (int), &value, sizeof (int)))
    return 0;

  return -1;
}
#endif
