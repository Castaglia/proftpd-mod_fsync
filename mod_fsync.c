/*
 * ProFTPD: mod_fsync -- a module for using fsync to periodically force writes
 * Copyright (c) 2004-2017 TJ Saunders
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307, USA.
 *
 * As a special exemption, TJ Saunders and other respective copyright holders
 * give permission to link this program with OpenSSL, and distribute the
 * resulting executable, without including the source code for OpenSSL in the
 * source distribution.
 *
 * This is mod_fsync, contrib software for proftpd 1.3.x and above.
 * For more information contact TJ Saunders <tj@castaglia.org>.
 */

#include "conf.h"
#include "privs.h"

#define MOD_FSYNC_VERSION		"mod_fsync/0.3"

/* Make sure the version of proftpd is as necessary. */
#if PROFTPD_VERSION_NUMBER < 0x0001030001
# error "ProFTPD 1.3.0rc1 or later required"
#endif

module fsync_module;

static int fsync_logfd = -1;
static array_header *fsync_fds = NULL;
static off_t fsync_nwritten = 0;
static off_t fsync_threshold = 0;

/* FSIO handlers
 */

static int fsync_close(pr_fh_t *fh, int fd) {

  /* Remove this fd from the tracked list, if present.  We don't need
   * to explicitly flush any disk buffer via fsync(), as that's handled
   * by the kernel.
   */

  if (fsync_fds != NULL &&
      fsync_fds->nelts > 0) {
    register unsigned int i;
    int *fds = fsync_fds->elts;

    for (i = 0; i < fsync_fds->nelts; i++) {
      if (fds[i] == fd) {
        (void) pr_log_writefile(fsync_logfd, MOD_FSYNC_VERSION,
          "removing '%s' (%d) from list", fh->fh_path, fd);
        fds[i] = -1;
        break;
      }
    }
  }

  return close(fd);
}

static int fsync_open(pr_fh_t *fh, const char *path, int flags) {
  int fd = open(path, flags, PR_OPEN_MODE);

  /* Error out now if the file couldn't be opened. */
  if (fd < 0)
    return fd;

  /* If the path is being opened for writing, once opened, add the
   * fd to a tracking list.
   */
  if ((flags & O_WRONLY) ||
      (flags & O_RDWR)) {
    register unsigned int i;
    int *fds, added = FALSE;

    if (!fsync_fds) {
      pool *p = session.pool ? session.pool : permanent_pool;
      fsync_fds = make_array(p, 1, sizeof(int));
    }

    /* Run through the list, checking for an empty slot first.  No need
     * to allocate more entries than necessary.
     */

    fds = fsync_fds->elts;
    for (i = 0; i < fsync_fds->nelts; i++) {
      if (fds[i] == -1) {
        /* Found an empty slot. */
        fds[i] = fd;

        added = TRUE;
        break;
      }
    }

    if (!added)
      *((int *) push_array(fsync_fds)) = fd;

    (void) pr_log_writefile(fsync_logfd, MOD_FSYNC_VERSION,
      "added '%s' (%d) to the list", path, fd);
  }

  return fd;
}

static int fsync_write(pr_fh_t *fh, int fd, const char *buf, size_t size) {
  int res;

  /* Force a fsync() on all the tracked fds (which have been opened for
   * writing) after a certain global amount of data has been written
   * to the filesystem.  (Note: do this even if the files are on separate
   * disks?)  After doing so, reset the global bytes-written counter.
   */

  res = write(fd, buf, size);
  if (res < 0)
    return res;

  fsync_nwritten += res;

  if (fsync_fds != NULL &&
      fsync_nwritten >= fsync_threshold) {
    register unsigned int i;
    int *fds = fsync_fds->elts;

    (void) pr_log_writefile(fsync_logfd, MOD_FSYNC_VERSION,
      "FsyncThreshold (%" PR_LU ") reached, syncing %d descriptors",
      (pr_off_t) fsync_threshold, fsync_fds->nelts);

    for (i = 0; i < fsync_fds->nelts; i++) {
      if (fds[i] != -1) {

#ifdef HAVE_FDATASYNC
        if (fdatasync(fds[i]) < 0) {
#else
        if (fsync(fds[i]) < 0) {
#endif /* HAVE_FDATASYNC */
          (void) pr_log_writefile(fsync_logfd, MOD_FSYNC_VERSION,
            "error sync'ing data for %d: %s", fds[i], strerror(errno));
        }
      }
    }

    /* Reset the counter. */
    fsync_nwritten = 0;
  }

  return res;
}

/* Configuration handlers
 */

/* usage: FsyncEngine on|off */
MODRET set_fsyncengine(cmd_rec *cmd) {
  int engine = -1;
  config_rec *c;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT);

  engine = get_boolean(cmd, 1);
  if (engine == -1) {
    CONF_ERROR(cmd, "expected Boolean parameter");
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = palloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = engine;

  return PR_HANDLED(cmd);
}

/* usage: FsyncLog path */
MODRET set_fsynclog(cmd_rec *cmd) {
  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT);

  if (pr_fs_valid_path(cmd->argv[1]) < 0) {
    CONF_ERROR(cmd, "must be an absolute path");
  }

  add_config_param_str(cmd->argv[0], 1, cmd->argv[1]);
  return PR_HANDLED(cmd);
}

/* usage: FsyncThreshold size */
MODRET set_fsyncthreshold(cmd_rec *cmd) {
  char *tmp = NULL;
  off_t threshold;
  config_rec *c;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT);

#ifdef HAVE_STRTOULL
  threshold = strtoull(cmd->argv[1], &tmp, 10);
#else
  threshold = strtoul(cmd->argv[1], &tmp, 10);
#endif /* HAVE_STRTOULL */

  if (tmp &&
      *tmp) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "'", cmd->argv[1],
      "' is not a valid threshold", NULL));
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = palloc(c->pool, sizeof(off_t));
  *((off_t *) c->argv[0]) = threshold;

  return PR_HANDLED(cmd);
}

/* Event handlers
 */

#if defined(PR_SHARED_MODULE)
static void fsync_mod_unload_ev(const void *event_data, void *user_data) {
  if (strcmp("mod_fsync.c", (const char *) event_data) == 0) {
    (void) pr_unmount_fs("/", "fsync");
    pr_event_unregister(&fsync_module, NULL, NULL);
  }
}
#endif /* PR_SHARED_MODULE */

static void fsync_postparse_ev(const void *event_data, void *user_data) {
  config_rec *c;
  pr_fs_t *fs;
  int engine = FALSE;

  c = find_config(main_server->conf, CONF_PARAM, "FsyncEngine", FALSE);
  if (c)
    engine = *((int *) c->argv[0]);

  if (!engine)
    return;

  c = find_config(main_server->conf, CONF_PARAM, "FsyncLog", FALSE);
  if (c) {
    int res;
    char *path = c->argv[0];

    PRIVS_ROOT
    res = pr_log_openfile(path, &fsync_logfd, 0660);
    PRIVS_RELINQUISH

    switch (res) {
      case 0:
        break;

      case -1:
        pr_log_debug(DEBUG1, MOD_FSYNC_VERSION
          ": unable to open FsyncLog '%s': %s", path, strerror(errno));
        break;

      case PR_LOG_SYMLINK:
        pr_log_debug(DEBUG1, MOD_FSYNC_VERSION
          ": unable to open FsyncLog '%s': %s", path, "is a symlink");
        break;

      case PR_LOG_WRITABLE_DIR:
        pr_log_debug(DEBUG0, MOD_FSYNC_VERSION
          ": unable to open FsyncLog '%s': %s", path,
          "parent directory is world-writable");
        break;
    }
  }

  c = find_config(main_server->conf, CONF_PARAM, "FsyncThreshold", FALSE);
  if (c == NULL) {
    /* This is a required directive. */
    (void) pr_log_writefile(fsync_logfd, MOD_FSYNC_VERSION,
      "missing required FsyncThreshold directive, disabling module");
    return;
  }

  fsync_threshold = *((off_t *) c->argv[0]);

  /* Register our custom filesystem. */
  fs = pr_register_fs(permanent_pool, "fsync", "/");
  if (fs == NULL) {
    (void) pr_log_writefile(fsync_logfd, MOD_FSYNC_VERSION,
      "error registering FS: %s", strerror(errno));
    return;
  }

  /* Add our custom FSIO handlers. */
  fs->close = fsync_close;
  fs->open = fsync_open;
  fs->write = fsync_write;

  return;
}

/* Initialization functions
 */

static int fsync_init(void) {
#if defined(PR_SHARED_MODULE)
  pr_event_register(&fsync_module, "core.module-unload", fsync_mod_unload_ev,
    NULL);
#endif /* PR_SHARED_MODULE */

  pr_event_register(&fsync_module, "core.postparse", fsync_postparse_ev,
    NULL);

  return 0;
}

/* Module API tables
 */

static conftable fsync_conftab[] = {
  { "FsyncEngine",		set_fsyncengine,	NULL },
  { "FsyncLog",			set_fsynclog,		NULL },
  { "FsyncThreshold",		set_fsyncthreshold,	NULL },
  { NULL }
};

module fsync_module = {
  NULL, NULL,

  /* Module API version 2.0 */
  0x20,

  /* Module name */
  "fsync",

  /* Module configuration handler table */
  fsync_conftab,

  /* Module command handler table */
  NULL,

  /* Module authentication handler table */
  NULL,

  /* Module initialization function */
  fsync_init,

  /* Session initialization function */
  NULL,

  /* Module version */
  MOD_FSYNC_VERSION
};
