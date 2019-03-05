/* SPDX-License-Identifier: GPL-2.0+ */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "alloc-util.h"
#include "conf-files.h"
#include "device-private.h"
#include "device-util.h"
#include "dirent-util.h"
#include "escape.h"
#include "fd-util.h"
#include "fileio.h"
#include "fs-util.h"
#include "glob-util.h"
#include "libudev-util.h"
#include "mkdir.h"
#include "parse-util.h"
#include "path-util.h"
#include "proc-cmdline.h"
#include "stat-util.h"
#include "stdio-util.h"
#include "strbuf.h"
#include "string-util.h"
#include "strv.h"
#include "strxcpyx.h"
#include "sysctl-util.h"
#include "udev-builtin.h"
#include "udev.h"
#include "user-util.h"
#include "util.h"

#define PREALLOC_TOKEN          2048

struct uid_gid {
        unsigned name_off;
        union {
                uid_t uid;
                gid_t gid;
        };
};

static const char* const rules_dirs[] = {
        "/etc/udev/rules.d",
        "/run/udev/rules.d",
        UDEVLIBEXECDIR "/rules.d",
        NULL
};

struct UdevRules {
        usec_t dirs_ts_usec;
        ResolveNameTiming resolve_name_timing;

        /* every key in the rules file becomes a token */
        struct token *tokens;
        unsigned token_cur;
        unsigned token_max;

        /* all key strings are copied and de-duplicated in a single continuous string buffer */
        struct strbuf *strbuf;

        /* during rule parsing, uid/gid lookup results are cached */
        struct uid_gid *uids;
        unsigned uids_cur;
        unsigned uids_max;
        struct uid_gid *gids;
        unsigned gids_cur;
        unsigned gids_max;
};

static char *rules_str(UdevRules *rules, unsigned off) {
        return rules->strbuf->buf + off;
}

static unsigned rules_add_string(UdevRules *rules, const char *s) {
        return strbuf_add_string(rules->strbuf, s, strlen(s));
}

/* KEY=="", KEY!="", KEY+="", KEY-="", KEY="", KEY:="" */
enum operation_type {
        OP_UNSET,

        OP_MATCH,
        OP_NOMATCH,
        OP_MATCH_MAX,

        OP_ADD,
        OP_REMOVE,
        OP_ASSIGN,
        OP_ASSIGN_FINAL,
};

enum string_glob_type {
        GL_UNSET,
        GL_PLAIN,                       /* no special chars */
        GL_GLOB,                        /* shell globs ?,*,[] */
        GL_SPLIT,                       /* multi-value A|B */
        GL_SPLIT_GLOB,                  /* multi-value with glob A*|B* */
        GL_SOMETHING,                   /* commonly used "?*" */
};

enum string_subst_type {
        SB_UNSET,
        SB_NONE,
        SB_FORMAT,
        SB_SUBSYS,
};

/* tokens of a rule are sorted/handled in this order */
enum token_type {
        TK_UNSET,
        TK_RULE,

        TK_M_ACTION,                    /* val */
        TK_M_DEVPATH,                   /* val */
        TK_M_KERNEL,                    /* val */
        TK_M_DEVLINK,                   /* val */
        TK_M_NAME,                      /* val */
        TK_M_ENV,                       /* val, attr */
        TK_M_TAG,                       /* val */
        TK_M_SUBSYSTEM,                 /* val */
        TK_M_DRIVER,                    /* val */
        TK_M_WAITFOR,                   /* val */
        TK_M_ATTR,                      /* val, attr */
        TK_M_SYSCTL,                    /* val, attr */

        TK_M_PARENTS_MIN,
        TK_M_KERNELS,                   /* val */
        TK_M_SUBSYSTEMS,                /* val */
        TK_M_DRIVERS,                   /* val */
        TK_M_ATTRS,                     /* val, attr */
        TK_M_TAGS,                      /* val */
        TK_M_PARENTS_MAX,

        TK_M_TEST,                      /* val, mode_t */
        TK_M_PROGRAM,                   /* val */
        TK_M_IMPORT_FILE,               /* val */
        TK_M_IMPORT_PROG,               /* val */
        TK_M_IMPORT_BUILTIN,            /* val */
        TK_M_IMPORT_DB,                 /* val */
        TK_M_IMPORT_CMDLINE,            /* val */
        TK_M_IMPORT_PARENT,             /* val */
        TK_M_RESULT,                    /* val */
        TK_M_MAX,

        TK_A_STRING_ESCAPE_NONE,
        TK_A_STRING_ESCAPE_REPLACE,
        TK_A_DB_PERSIST,
        TK_A_INOTIFY_WATCH,             /* int */
        TK_A_DEVLINK_PRIO,              /* int */
        TK_A_OWNER,                     /* val */
        TK_A_GROUP,                     /* val */
        TK_A_MODE,                      /* val */
        TK_A_OWNER_ID,                  /* uid_t */
        TK_A_GROUP_ID,                  /* gid_t */
        TK_A_MODE_ID,                   /* mode_t */
        TK_A_TAG,                       /* val */
        TK_A_STATIC_NODE,               /* val */
        TK_A_SECLABEL,                  /* val, attr */
        TK_A_ENV,                       /* val, attr */
        TK_A_NAME,                      /* val */
        TK_A_DEVLINK,                   /* val */
        TK_A_ATTR,                      /* val, attr */
        TK_A_SYSCTL,                    /* val, attr */
        TK_A_RUN_BUILTIN,               /* val, bool */
        TK_A_RUN_PROGRAM,               /* val, bool */
        TK_A_GOTO,                      /* size_t */

        TK_END,
};

/* we try to pack stuff in a way that we take only 12 bytes per token */
struct token {
        union {
                unsigned char type;                /* same in rule and key */
                struct {
                        enum token_type type:8;
                        bool can_set_name:1;
                        bool has_static_node:1;
                        unsigned unused:6;
                        unsigned short token_count;
                        unsigned label_off;
                        unsigned short filename_off;
                        unsigned short filename_line;
                } rule;
                struct {
                        enum token_type type:8;
                        enum operation_type op:8;
                        enum string_glob_type glob:8;
                        enum string_subst_type subst:4;
                        enum string_subst_type attrsubst:4;
                        unsigned value_off;
                        union {
                                unsigned attr_off;
                                unsigned rule_goto;
                                mode_t mode;
                                uid_t uid;
                                gid_t gid;
                                int devlink_prio;
                                int watch;
                                enum udev_builtin_cmd builtin_cmd;
                        };
                } key;
        };
};

#define MAX_TK                64
struct rule_tmp {
        UdevRules *rules;
        struct token rule;
        struct token token[MAX_TK];
        unsigned token_cur;
};

#if ENABLE_DEBUG_UDEV
static const char *operation_str(enum operation_type type) {
        static const char *operation_strs[] = {
                [OP_UNSET] =            "UNSET",
                [OP_MATCH] =            "match",
                [OP_NOMATCH] =          "nomatch",
                [OP_MATCH_MAX] =        "MATCH_MAX",

                [OP_ADD] =              "add",
                [OP_REMOVE] =           "remove",
                [OP_ASSIGN] =           "assign",
                [OP_ASSIGN_FINAL] =     "assign-final",
        };

        return operation_strs[type];
}

static const char *string_glob_str(enum string_glob_type type) {
        static const char *string_glob_strs[] = {
                [GL_UNSET] =            "UNSET",
                [GL_PLAIN] =            "plain",
                [GL_GLOB] =             "glob",
                [GL_SPLIT] =            "split",
                [GL_SPLIT_GLOB] =       "split-glob",
                [GL_SOMETHING] =        "split-glob",
        };

        return string_glob_strs[type];
}

static const char *token_str(enum token_type type) {
        static const char *token_strs[] = {
                [TK_UNSET] =                    "UNSET",
                [TK_RULE] =                     "RULE",

                [TK_M_ACTION] =                 "M ACTION",
                [TK_M_DEVPATH] =                "M DEVPATH",
                [TK_M_KERNEL] =                 "M KERNEL",
                [TK_M_DEVLINK] =                "M DEVLINK",
                [TK_M_NAME] =                   "M NAME",
                [TK_M_ENV] =                    "M ENV",
                [TK_M_TAG] =                    "M TAG",
                [TK_M_SUBSYSTEM] =              "M SUBSYSTEM",
                [TK_M_DRIVER] =                 "M DRIVER",
                [TK_M_WAITFOR] =                "M WAITFOR",
                [TK_M_ATTR] =                   "M ATTR",
                [TK_M_SYSCTL] =                 "M SYSCTL",

                [TK_M_PARENTS_MIN] =            "M PARENTS_MIN",
                [TK_M_KERNELS] =                "M KERNELS",
                [TK_M_SUBSYSTEMS] =             "M SUBSYSTEMS",
                [TK_M_DRIVERS] =                "M DRIVERS",
                [TK_M_ATTRS] =                  "M ATTRS",
                [TK_M_TAGS] =                   "M TAGS",
                [TK_M_PARENTS_MAX] =            "M PARENTS_MAX",

                [TK_M_TEST] =                   "M TEST",
                [TK_M_PROGRAM] =                "M PROGRAM",
                [TK_M_IMPORT_FILE] =            "M IMPORT_FILE",
                [TK_M_IMPORT_PROG] =            "M IMPORT_PROG",
                [TK_M_IMPORT_BUILTIN] =         "M IMPORT_BUILTIN",
                [TK_M_IMPORT_DB] =              "M IMPORT_DB",
                [TK_M_IMPORT_CMDLINE] =         "M IMPORT_CMDLINE",
                [TK_M_IMPORT_PARENT] =          "M IMPORT_PARENT",
                [TK_M_RESULT] =                 "M RESULT",
                [TK_M_MAX] =                    "M MAX",

                [TK_A_STRING_ESCAPE_NONE] =     "A STRING_ESCAPE_NONE",
                [TK_A_STRING_ESCAPE_REPLACE] =  "A STRING_ESCAPE_REPLACE",
                [TK_A_DB_PERSIST] =             "A DB_PERSIST",
                [TK_A_INOTIFY_WATCH] =          "A INOTIFY_WATCH",
                [TK_A_DEVLINK_PRIO] =           "A DEVLINK_PRIO",
                [TK_A_OWNER] =                  "A OWNER",
                [TK_A_GROUP] =                  "A GROUP",
                [TK_A_MODE] =                   "A MODE",
                [TK_A_OWNER_ID] =               "A OWNER_ID",
                [TK_A_GROUP_ID] =               "A GROUP_ID",
                [TK_A_STATIC_NODE] =            "A STATIC_NODE",
                [TK_A_SECLABEL] =               "A SECLABEL",
                [TK_A_MODE_ID] =                "A MODE_ID",
                [TK_A_ENV] =                    "A ENV",
                [TK_A_TAG] =                    "A ENV",
                [TK_A_NAME] =                   "A NAME",
                [TK_A_DEVLINK] =                "A DEVLINK",
                [TK_A_ATTR] =                   "A ATTR",
                [TK_A_SYSCTL] =                 "A SYSCTL",
                [TK_A_RUN_BUILTIN] =            "A RUN_BUILTIN",
                [TK_A_RUN_PROGRAM] =            "A RUN_PROGRAM",
                [TK_A_GOTO] =                   "A GOTO",

                [TK_END] =                      "END",
        };

        return token_strs[type];
}

static void dump_token(UdevRules *rules, struct token *token) {
        enum token_type type = token->type;
        enum operation_type op = token->key.op;
        enum string_glob_type glob = token->key.glob;
        const char *value = rules_str(rules, token->key.value_off);
        const char *attr = &rules->strbuf->buf[token->key.attr_off];

        switch (type) {
        case TK_RULE:
                {
                        const char *tks_ptr = (char *)rules->tokens;
                        const char *tk_ptr = (char *)token;
                        unsigned idx = (tk_ptr - tks_ptr) / sizeof(struct token);

                        log_debug("* RULE %s:%u, token: %u, count: %u, label: '%s'",
                                  &rules->strbuf->buf[token->rule.filename_off], token->rule.filename_line,
                                  idx, token->rule.token_count,
                                  &rules->strbuf->buf[token->rule.label_off]);
                        break;
                }
        case TK_M_ACTION:
        case TK_M_DEVPATH:
        case TK_M_KERNEL:
        case TK_M_SUBSYSTEM:
        case TK_M_DRIVER:
        case TK_M_WAITFOR:
        case TK_M_DEVLINK:
        case TK_M_NAME:
        case TK_M_KERNELS:
        case TK_M_SUBSYSTEMS:
        case TK_M_DRIVERS:
        case TK_M_TAGS:
        case TK_M_PROGRAM:
        case TK_M_IMPORT_FILE:
        case TK_M_IMPORT_PROG:
        case TK_M_IMPORT_DB:
        case TK_M_IMPORT_CMDLINE:
        case TK_M_IMPORT_PARENT:
        case TK_M_RESULT:
        case TK_A_NAME:
        case TK_A_DEVLINK:
        case TK_A_OWNER:
        case TK_A_GROUP:
        case TK_A_MODE:
        case TK_A_RUN_BUILTIN:
        case TK_A_RUN_PROGRAM:
                log_debug("%s %s '%s'(%s)",
                          token_str(type), operation_str(op), value, string_glob_str(glob));
                break;
        case TK_M_IMPORT_BUILTIN:
                log_debug("%s %i '%s'", token_str(type), token->key.builtin_cmd, value);
                break;
        case TK_M_ATTR:
        case TK_M_SYSCTL:
        case TK_M_ATTRS:
        case TK_M_ENV:
        case TK_A_ATTR:
        case TK_A_SYSCTL:
        case TK_A_ENV:
                log_debug("%s %s '%s' '%s'(%s)",
                          token_str(type), operation_str(op), attr, value, string_glob_str(glob));
                break;
        case TK_M_TAG:
        case TK_A_TAG:
                log_debug("%s %s '%s'", token_str(type), operation_str(op), value);
                break;
        case TK_A_STRING_ESCAPE_NONE:
        case TK_A_STRING_ESCAPE_REPLACE:
        case TK_A_DB_PERSIST:
                log_debug("%s", token_str(type));
                break;
        case TK_M_TEST:
                log_debug("%s %s '%s'(%s) %#o",
                          token_str(type), operation_str(op), value, string_glob_str(glob), token->key.mode);
                break;
        case TK_A_INOTIFY_WATCH:
                log_debug("%s %u", token_str(type), token->key.watch);
                break;
        case TK_A_DEVLINK_PRIO:
                log_debug("%s %u", token_str(type), token->key.devlink_prio);
                break;
        case TK_A_OWNER_ID:
                log_debug("%s %s %u", token_str(type), operation_str(op), token->key.uid);
                break;
        case TK_A_GROUP_ID:
                log_debug("%s %s %u", token_str(type), operation_str(op), token->key.gid);
                break;
        case TK_A_MODE_ID:
                log_debug("%s %s %#o", token_str(type), operation_str(op), token->key.mode);
                break;
        case TK_A_STATIC_NODE:
                log_debug("%s '%s'", token_str(type), value);
                break;
        case TK_A_SECLABEL:
                log_debug("%s %s '%s' '%s'", token_str(type), operation_str(op), attr, value);
                break;
        case TK_A_GOTO:
                log_debug("%s '%s' %u", token_str(type), value, token->key.rule_goto);
                break;
        case TK_END:
                log_debug("* %s", token_str(type));
                break;
        case TK_M_PARENTS_MIN:
        case TK_M_PARENTS_MAX:
        case TK_M_MAX:
        case TK_UNSET:
                log_debug("Unknown token type %u", type);
                break;
        }
}

static void dump_rules(UdevRules *rules) {
        unsigned i;

        log_debug("Dumping %u (%zu bytes) tokens, %zu (%zu bytes) strings",
                  rules->token_cur,
                  rules->token_cur * sizeof(struct token),
                  rules->strbuf->nodes_count,
                  rules->strbuf->len);
        for (i = 0; i < rules->token_cur; i++)
                dump_token(rules, &rules->tokens[i]);
}
#else
static void dump_token(UdevRules *rules, struct token *token) {}
static void dump_rules(UdevRules *rules) {}
#endif /* ENABLE_DEBUG_UDEV */

static int add_token(UdevRules *rules, struct token *token) {
        /* grow buffer if needed */
        if (rules->token_cur+1 >= rules->token_max) {
                struct token *tokens;
                unsigned add;

                /* double the buffer size */
                add = rules->token_max;
                if (add < 8)
                        add = 8;

                tokens = reallocarray(rules->tokens, rules->token_max + add, sizeof(struct token));
                if (!tokens)
                        return -1;
                rules->tokens = tokens;
                rules->token_max += add;
        }
        memcpy(&rules->tokens[rules->token_cur], token, sizeof(struct token));
        rules->token_cur++;
        return 0;
}

static void log_unknown_owner(sd_device *dev, int error, const char *entity, const char *owner) {
        if (IN_SET(abs(error), ENOENT, ESRCH))
                log_device_error(dev, "Specified %s '%s' unknown", entity, owner);
        else
                log_device_error_errno(dev, error, "Failed to resolve %s '%s': %m", entity, owner);
}

static uid_t add_uid(UdevRules *rules, const char *owner) {
        unsigned i;
        uid_t uid = 0;
        unsigned off;
        int r;

        /* lookup, if we know it already */
        for (i = 0; i < rules->uids_cur; i++) {
                off = rules->uids[i].name_off;
                if (streq(rules_str(rules, off), owner)) {
                        uid = rules->uids[i].uid;
                        return uid;
                }
        }
        r = get_user_creds(&owner, &uid, NULL, NULL, NULL, USER_CREDS_ALLOW_MISSING);
        if (r < 0)
                log_unknown_owner(NULL, r, "user", owner);

        /* grow buffer if needed */
        if (rules->uids_cur+1 >= rules->uids_max) {
                struct uid_gid *uids;
                unsigned add;

                /* double the buffer size */
                add = rules->uids_max;
                if (add < 1)
                        add = 8;

                uids = reallocarray(rules->uids, rules->uids_max + add, sizeof(struct uid_gid));
                if (!uids)
                        return uid;
                rules->uids = uids;
                rules->uids_max += add;
        }
        rules->uids[rules->uids_cur].uid = uid;
        off = rules_add_string(rules, owner);
        if (off <= 0)
                return uid;
        rules->uids[rules->uids_cur].name_off = off;
        rules->uids_cur++;
        return uid;
}

static gid_t add_gid(UdevRules *rules, const char *group) {
        unsigned i;
        gid_t gid = 0;
        unsigned off;
        int r;

        /* lookup, if we know it already */
        for (i = 0; i < rules->gids_cur; i++) {
                off = rules->gids[i].name_off;
                if (streq(rules_str(rules, off), group)) {
                        gid = rules->gids[i].gid;
                        return gid;
                }
        }
        r = get_group_creds(&group, &gid, USER_CREDS_ALLOW_MISSING);
        if (r < 0)
                log_unknown_owner(NULL, r, "group", group);

        /* grow buffer if needed */
        if (rules->gids_cur+1 >= rules->gids_max) {
                struct uid_gid *gids;
                unsigned add;

                /* double the buffer size */
                add = rules->gids_max;
                if (add < 1)
                        add = 8;

                gids = reallocarray(rules->gids, rules->gids_max + add, sizeof(struct uid_gid));
                if (!gids)
                        return gid;
                rules->gids = gids;
                rules->gids_max += add;
        }
        rules->gids[rules->gids_cur].gid = gid;
        off = rules_add_string(rules, group);
        if (off <= 0)
                return gid;
        rules->gids[rules->gids_cur].name_off = off;
        rules->gids_cur++;
        return gid;
}

static int import_property_from_string(sd_device *dev, char *line) {
        char *key;
        char *val;
        size_t len;

        /* find key */
        key = line;
        while (isspace(key[0]))
                key++;

        /* comment or empty line */
        if (IN_SET(key[0], '#', '\0'))
                return 0;

        /* split key/value */
        val = strchr(key, '=');
        if (!val)
                return -EINVAL;
        val[0] = '\0';
        val++;

        /* find value */
        while (isspace(val[0]))
                val++;

        /* terminate key */
        len = strlen(key);
        if (len == 0)
                return -EINVAL;
        while (isspace(key[len-1]))
                len--;
        key[len] = '\0';

        /* terminate value */
        len = strlen(val);
        if (len == 0)
                return -EINVAL;
        while (isspace(val[len-1]))
                len--;
        val[len] = '\0';

        if (len == 0)
                return -EINVAL;

        /* unquote */
        if (IN_SET(val[0], '"', '\'')) {
                if (len == 1 || val[len-1] != val[0])
                        return log_debug_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "Inconsistent quoting: '%s', skip",
                                               line);
                val[len-1] = '\0';
                val++;
        }

        return device_add_property(dev, key, val);
}

static int import_file_into_properties(sd_device *dev, const char *filename) {
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        f = fopen(filename, "re");
        if (!f)
                return -errno;

        for (;;) {
                _cleanup_free_ char *line = NULL;

                r = read_line(f, LONG_LINE_MAX, &line);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;

                (void) import_property_from_string(dev, line);
        }

        return 0;
}

static int import_program_into_properties(UdevEvent *event,
                                          usec_t timeout_usec,
                                          const char *program) {
        char result[UTIL_LINE_SIZE];
        char *line;
        int r;

        r = udev_event_spawn(event, timeout_usec, false, program, result, sizeof result);
        if (r < 0)
                return r;
        if (r > 0)
                return -EIO;

        line = result;
        while (line) {
                char *pos;

                pos = strchr(line, '\n');
                if (pos) {
                        pos[0] = '\0';
                        pos = &pos[1];
                }
                (void) import_property_from_string(event->dev, line);
                line = pos;
        }
        return 0;
}

static int import_parent_into_properties(sd_device *dev, const char *filter) {
        const char *key, *val;
        sd_device *parent;
        int r;

        assert(dev);
        assert(filter);

        r = sd_device_get_parent(dev, &parent);
        if (r < 0)
                return r;

        FOREACH_DEVICE_PROPERTY(parent, key, val)
                if (fnmatch(filter, key, 0) == 0)
                        device_add_property(dev, key, val);
        return 0;
}

static void attr_subst_subdir(char *attr, size_t len) {
        const char *pos, *tail, *path;
        _cleanup_closedir_ DIR *dir = NULL;
        struct dirent *dent;

        pos = strstr(attr, "/*/");
        if (!pos)
            return;

        tail = pos + 2;
        path = strndupa(attr, pos - attr + 1); /* include slash at end */
        dir = opendir(path);
        if (!dir)
                return;

        FOREACH_DIRENT_ALL(dent, dir, break)
                if (dent->d_name[0] != '.') {
                        char n[strlen(dent->d_name) + strlen(tail) + 1];

                        strscpyl(n, sizeof n, dent->d_name, tail, NULL);
                        if (faccessat(dirfd(dir), n, F_OK, 0) == 0) {
                                strscpyl(attr, len, path, n, NULL);
                                break;
                        }
                }
}

static int get_key(char **line, char **key, enum operation_type *op, char **value) {
        char *linepos;
        char *temp;
        unsigned i, j;

        linepos = *line;
        if (!linepos || linepos[0] == '\0')
                return -1;

        /* skip whitespace */
        while (isspace(linepos[0]) || linepos[0] == ',')
                linepos++;

        /* get the key */
        if (linepos[0] == '\0')
                return -1;
        *key = linepos;

        for (;;) {
                linepos++;
                if (linepos[0] == '\0')
                        return -1;
                if (isspace(linepos[0]))
                        break;
                if (linepos[0] == '=')
                        break;
                if (IN_SET(linepos[0], '+', '-', '!', ':'))
                        if (linepos[1] == '=')
                                break;
        }

        /* remember end of key */
        temp = linepos;

        /* skip whitespace after key */
        while (isspace(linepos[0]))
                linepos++;
        if (linepos[0] == '\0')
                return -1;

        /* get operation type */
        if (linepos[0] == '=' && linepos[1] == '=') {
                *op = OP_MATCH;
                linepos += 2;
        } else if (linepos[0] == '!' && linepos[1] == '=') {
                *op = OP_NOMATCH;
                linepos += 2;
        } else if (linepos[0] == '+' && linepos[1] == '=') {
                *op = OP_ADD;
                linepos += 2;
        } else if (linepos[0] == '-' && linepos[1] == '=') {
                *op = OP_REMOVE;
                linepos += 2;
        } else if (linepos[0] == '=') {
                *op = OP_ASSIGN;
                linepos++;
        } else if (linepos[0] == ':' && linepos[1] == '=') {
                *op = OP_ASSIGN_FINAL;
                linepos += 2;
        } else
                return -1;

        /* terminate key */
        temp[0] = '\0';

        /* skip whitespace after operator */
        while (isspace(linepos[0]))
                linepos++;
        if (linepos[0] == '\0')
                return -1;

        /* get the value */
        if (linepos[0] == '"')
                linepos++;
        else
                return -1;
        *value = linepos;

        /* terminate */
        for (i = 0, j = 0; ; i++, j++) {

                if (linepos[i] == '"')
                        break;

                if (linepos[i] == '\0')
                        return -1;

                /* double quotes can be escaped */
                if (linepos[i] == '\\')
                        if (linepos[i+1] == '"')
                                i++;

                linepos[j] = linepos[i];
        }
        linepos[j] = '\0';

        /* move line to next key */
        *line = linepos + i + 1;
        return 0;
}

/* extract possible KEY{attr} */
static const char *get_key_attribute(char *str) {
        char *pos;
        char *attr;

        attr = strchr(str, '{');
        if (attr) {
                attr++;
                pos = strchr(attr, '}');
                if (!pos) {
                        log_error("Missing closing brace for format");
                        return NULL;
                }
                pos[0] = '\0';
                return attr;
        }
        return NULL;
}

static int rule_add_key(struct rule_tmp *rule_tmp, enum token_type type,
                        enum operation_type op,
                        const char *value, const void *data) {
        struct token *token = rule_tmp->token + rule_tmp->token_cur;
        const char *attr = NULL;

        if (rule_tmp->token_cur >= ELEMENTSOF(rule_tmp->token))
                return -E2BIG;

        memzero(token, sizeof(struct token));

        switch (type) {
        case TK_M_ACTION:
        case TK_M_DEVPATH:
        case TK_M_KERNEL:
        case TK_M_SUBSYSTEM:
        case TK_M_DRIVER:
        case TK_M_WAITFOR:
        case TK_M_DEVLINK:
        case TK_M_NAME:
        case TK_M_KERNELS:
        case TK_M_SUBSYSTEMS:
        case TK_M_DRIVERS:
        case TK_M_TAGS:
        case TK_M_PROGRAM:
        case TK_M_IMPORT_FILE:
        case TK_M_IMPORT_PROG:
        case TK_M_IMPORT_DB:
        case TK_M_IMPORT_CMDLINE:
        case TK_M_IMPORT_PARENT:
        case TK_M_RESULT:
        case TK_A_OWNER:
        case TK_A_GROUP:
        case TK_A_MODE:
        case TK_A_DEVLINK:
        case TK_A_NAME:
        case TK_A_GOTO:
        case TK_M_TAG:
        case TK_A_TAG:
        case TK_A_STATIC_NODE:
                token->key.value_off = rules_add_string(rule_tmp->rules, value);
                break;
        case TK_M_IMPORT_BUILTIN:
                token->key.value_off = rules_add_string(rule_tmp->rules, value);
                token->key.builtin_cmd = *(enum udev_builtin_cmd *)data;
                break;
        case TK_M_ENV:
        case TK_M_ATTR:
        case TK_M_SYSCTL:
        case TK_M_ATTRS:
        case TK_A_ATTR:
        case TK_A_SYSCTL:
        case TK_A_ENV:
        case TK_A_SECLABEL:
                attr = data;
                token->key.value_off = rules_add_string(rule_tmp->rules, value);
                token->key.attr_off = rules_add_string(rule_tmp->rules, attr);
                break;
        case TK_M_TEST:
                token->key.value_off = rules_add_string(rule_tmp->rules, value);
                if (data)
                        token->key.mode = *(mode_t *)data;
                break;
        case TK_A_STRING_ESCAPE_NONE:
        case TK_A_STRING_ESCAPE_REPLACE:
        case TK_A_DB_PERSIST:
                break;
        case TK_A_RUN_BUILTIN:
        case TK_A_RUN_PROGRAM:
                token->key.builtin_cmd = *(enum udev_builtin_cmd *)data;
                token->key.value_off = rules_add_string(rule_tmp->rules, value);
                break;
        case TK_A_INOTIFY_WATCH:
        case TK_A_DEVLINK_PRIO:
                token->key.devlink_prio = *(int *)data;
                break;
        case TK_A_OWNER_ID:
                token->key.uid = *(uid_t *)data;
                break;
        case TK_A_GROUP_ID:
                token->key.gid = *(gid_t *)data;
                break;
        case TK_A_MODE_ID:
                token->key.mode = *(mode_t *)data;
                break;
        case TK_RULE:
        case TK_M_PARENTS_MIN:
        case TK_M_PARENTS_MAX:
        case TK_M_MAX:
        case TK_END:
        case TK_UNSET:
                assert_not_reached("wrong type");
        }

        if (value && type < TK_M_MAX) {
                /* check if we need to split or call fnmatch() while matching rules */
                enum string_glob_type glob;
                bool has_split, has_glob;

                has_split = strchr(value, '|');
                has_glob = string_is_glob(value);
                if (has_split && has_glob)
                        glob = GL_SPLIT_GLOB;
                else if (has_split)
                        glob = GL_SPLIT;
                else if (has_glob) {
                        if (streq(value, "?*"))
                                glob = GL_SOMETHING;
                        else
                                glob = GL_GLOB;
                } else
                        glob = GL_PLAIN;

                token->key.glob = glob;
        }

        if (value && type > TK_M_MAX) {
                /* check if assigned value has substitution chars */
                if (value[0] == '[')
                        token->key.subst = SB_SUBSYS;
                else if (strchr(value, '%') || strchr(value, '$'))
                        token->key.subst = SB_FORMAT;
                else
                        token->key.subst = SB_NONE;
        }

        if (attr) {
                /* check if property/attribute name has substitution chars */
                if (attr[0] == '[')
                        token->key.attrsubst = SB_SUBSYS;
                else if (strchr(attr, '%') || strchr(attr, '$'))
                        token->key.attrsubst = SB_FORMAT;
                else
                        token->key.attrsubst = SB_NONE;
        }

        token->key.type = type;
        token->key.op = op;
        rule_tmp->token_cur++;

        return 0;
}

static int sort_token(UdevRules *rules, struct rule_tmp *rule_tmp) {
        unsigned i;
        unsigned start = 0;
        unsigned end = rule_tmp->token_cur;

        for (i = 0; i < rule_tmp->token_cur; i++) {
                enum token_type next_val = TK_UNSET;
                unsigned next_idx = 0;
                unsigned j;

                /* find smallest value */
                for (j = start; j < end; j++) {
                        if (rule_tmp->token[j].type == TK_UNSET)
                                continue;
                        if (next_val == TK_UNSET || rule_tmp->token[j].type < next_val) {
                                next_val = rule_tmp->token[j].type;
                                next_idx = j;
                        }
                }

                /* add token and mark done */
                if (add_token(rules, &rule_tmp->token[next_idx]) != 0)
                        return -1;
                rule_tmp->token[next_idx].type = TK_UNSET;

                /* shrink range */
                if (next_idx == start)
                        start++;
                if (next_idx+1 == end)
                        end--;
        }
        return 0;
}

#define LOG_RULE_FULL(level, fmt, ...) log_full(level, "%s:%u: " fmt, filename, lineno, ##__VA_ARGS__)
#define LOG_RULE_ERROR(fmt, ...) LOG_RULE_FULL(LOG_ERR, fmt, ##__VA_ARGS__)
#define LOG_RULE_WARNING(fmt, ...) LOG_RULE_FULL(LOG_WARNING, fmt, ##__VA_ARGS__)
#define LOG_RULE_DEBUG(fmt, ...) LOG_RULE_FULL(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_AND_RETURN(fmt, ...) { LOG_RULE_ERROR(fmt, __VA_ARGS__); return; }
#define LOG_AND_RETURN_ADD_KEY LOG_AND_RETURN("Temporary rule array too small, aborting event processing with %u items", rule_tmp.token_cur);

static void add_rule(UdevRules *rules, char *line,
                     const char *filename, unsigned filename_off, unsigned lineno) {
        char *linepos;
        const char *attr;
        struct rule_tmp rule_tmp = {
                .rules = rules,
                .rule.type = TK_RULE,
        };
        int r;

        /* the offset in the rule is limited to unsigned short */
        if (filename_off < USHRT_MAX)
                rule_tmp.rule.rule.filename_off = filename_off;
        rule_tmp.rule.rule.filename_line = lineno;

        linepos = line;
        for (;;) {
                char *key;
                char *value;
                enum operation_type op;

                if (get_key(&linepos, &key, &op, &value) != 0) {
                        /* Avoid erroring on trailing whitespace. This is probably rare
                         * so save the work for the error case instead of always trying
                         * to strip the trailing whitespace with strstrip(). */
                        while (isblank(*linepos))
                                linepos++;

                        /* If we aren't at the end of the line, this is a parsing error.
                         * Make a best effort to describe where the problem is. */
                        if (!strchr(NEWLINE, *linepos)) {
                                char buf[2] = {*linepos};
                                _cleanup_free_ char *tmp;

                                tmp = cescape(buf);
                                LOG_RULE_ERROR("Invalid key/value pair, starting at character %tu ('%s')", linepos - line + 1, tmp);
                                if (*linepos == '#')
                                        LOG_RULE_ERROR("Hint: comments can only start at beginning of line");
                        }
                        break;
                }

                if (streq(key, "ACTION")) {
                        if (op > OP_MATCH_MAX)
                                LOG_AND_RETURN("Invalid %s operation", key);

                        if (rule_add_key(&rule_tmp, TK_M_ACTION, op, value, NULL) < 0)
                                LOG_AND_RETURN_ADD_KEY;

                } else if (streq(key, "DEVPATH")) {
                        if (op > OP_MATCH_MAX)
                                LOG_AND_RETURN("Invalid %s operation", key);

                        if (rule_add_key(&rule_tmp, TK_M_DEVPATH, op, value, NULL) < 0)
                                LOG_AND_RETURN_ADD_KEY;

                } else if (streq(key, "KERNEL")) {
                        if (op > OP_MATCH_MAX)
                                LOG_AND_RETURN("Invalid %s operation", key);

                        if (rule_add_key(&rule_tmp, TK_M_KERNEL, op, value, NULL) < 0)
                                LOG_AND_RETURN_ADD_KEY;

                } else if (streq(key, "SUBSYSTEM")) {
                        if (op > OP_MATCH_MAX)
                                LOG_AND_RETURN("Invalid %s operation", key);

                        /* bus, class, subsystem events should all be the same */
                        if (STR_IN_SET(value, "subsystem", "bus", "class")) {
                                if (!streq(value, "subsystem"))
                                        LOG_RULE_WARNING("'%s' must be specified as 'subsystem'; please fix", value);

                                r = rule_add_key(&rule_tmp, TK_M_SUBSYSTEM, op, "subsystem|class|bus", NULL);
                        } else
                                r = rule_add_key(&rule_tmp, TK_M_SUBSYSTEM, op, value, NULL);
                        if (r < 0)
                                LOG_AND_RETURN_ADD_KEY;

                } else if (streq(key, "DRIVER")) {
                        if (op > OP_MATCH_MAX)
                                LOG_AND_RETURN("Invalid %s operation", key);

                        if (rule_add_key(&rule_tmp, TK_M_DRIVER, op, value, NULL) < 0)
                                LOG_AND_RETURN_ADD_KEY;

                } else if (startswith(key, "ATTR{")) {
                        attr = get_key_attribute(key + STRLEN("ATTR"));
                        if (!attr)
                                LOG_AND_RETURN("Failed to parse %s attribute", "ATTR");

                        if (op == OP_REMOVE)
                                LOG_AND_RETURN("Invalid %s operation", "ATTR");

                        if (op < OP_MATCH_MAX)
                                r = rule_add_key(&rule_tmp, TK_M_ATTR, op, value, attr);
                        else
                                r = rule_add_key(&rule_tmp, TK_A_ATTR, op, value, attr);
                        if (r < 0)
                                LOG_AND_RETURN_ADD_KEY;

                } else if (startswith(key, "SYSCTL{")) {
                        attr = get_key_attribute(key + STRLEN("SYSCTL"));
                        if (!attr)
                                LOG_AND_RETURN("Failed to parse %s attribute", "ATTR");

                        if (op == OP_REMOVE)
                                LOG_AND_RETURN("Invalid %s operation", "ATTR");

                        if (op < OP_MATCH_MAX)
                                r = rule_add_key(&rule_tmp, TK_M_SYSCTL, op, value, attr);
                        else
                                r = rule_add_key(&rule_tmp, TK_A_SYSCTL, op, value, attr);
                        if (r < 0)
                                LOG_AND_RETURN_ADD_KEY;

                } else if (startswith(key, "SECLABEL{")) {
                        attr = get_key_attribute(key + STRLEN("SECLABEL"));
                        if (!attr)
                                LOG_AND_RETURN("Failed to parse %s attribute", "SECLABEL");

                        if (op == OP_REMOVE)
                                LOG_AND_RETURN("Invalid %s operation", "SECLABEL");

                        if (rule_add_key(&rule_tmp, TK_A_SECLABEL, op, value, attr) < 0)
                                LOG_AND_RETURN_ADD_KEY;

                } else if (streq(key, "KERNELS")) {
                        if (op > OP_MATCH_MAX)
                                LOG_AND_RETURN("Invalid %s operation", key);

                        if (rule_add_key(&rule_tmp, TK_M_KERNELS, op, value, NULL) < 0)
                                LOG_AND_RETURN_ADD_KEY;

                } else if (streq(key, "SUBSYSTEMS")) {
                        if (op > OP_MATCH_MAX)
                                LOG_AND_RETURN("Invalid %s operation", key);

                        if (rule_add_key(&rule_tmp, TK_M_SUBSYSTEMS, op, value, NULL) < 0)
                                LOG_AND_RETURN_ADD_KEY;

                } else if (streq(key, "DRIVERS")) {
                        if (op > OP_MATCH_MAX)
                                LOG_AND_RETURN("Invalid %s operation", key);

                        if (rule_add_key(&rule_tmp, TK_M_DRIVERS, op, value, NULL) < 0)
                                LOG_AND_RETURN_ADD_KEY;

                } else if (startswith(key, "ATTRS{")) {
                        if (op > OP_MATCH_MAX)
                                LOG_AND_RETURN("Invalid %s operation", "ATTRS");

                        attr = get_key_attribute(key + STRLEN("ATTRS"));
                        if (!attr)
                                LOG_AND_RETURN("Failed to parse %s attribute", "ATTRS");

                        if (startswith(attr, "device/"))
                                LOG_RULE_WARNING("'device' link may not be available in future kernels; please fix");
                        if (strstr(attr, "../"))
                                LOG_RULE_WARNING("Direct reference to parent sysfs directory, may break in future kernels; please fix");
                        if (rule_add_key(&rule_tmp, TK_M_ATTRS, op, value, attr) < 0)
                                LOG_AND_RETURN_ADD_KEY;

                } else if (streq(key, "TAGS")) {
                        if (op > OP_MATCH_MAX)
                                LOG_AND_RETURN("Invalid %s operation", key);

                        if (rule_add_key(&rule_tmp, TK_M_TAGS, op, value, NULL) < 0)
                                LOG_AND_RETURN_ADD_KEY;

                } else if (startswith(key, "ENV{")) {
                        attr = get_key_attribute(key + STRLEN("ENV"));
                        if (!attr)
                                LOG_AND_RETURN("Failed to parse %s attribute", "ENV");

                        if (op == OP_REMOVE)
                                LOG_AND_RETURN("Invalid %s operation", "ENV");

                        if (op < OP_MATCH_MAX)
                                r = rule_add_key(&rule_tmp, TK_M_ENV, op, value, attr);
                        else {
                                if (STR_IN_SET(attr,
                                               "ACTION",
                                               "SUBSYSTEM",
                                               "DEVTYPE",
                                               "MAJOR",
                                               "MINOR",
                                               "DRIVER",
                                               "IFINDEX",
                                               "DEVNAME",
                                               "DEVLINKS",
                                               "DEVPATH",
                                               "TAGS"))
                                        LOG_AND_RETURN("Invalid ENV attribute, '%s' cannot be set", attr);

                                r = rule_add_key(&rule_tmp, TK_A_ENV, op, value, attr);
                        }
                        if (r < 0)
                                LOG_AND_RETURN_ADD_KEY;

                } else if (streq(key, "TAG")) {
                        if (op < OP_MATCH_MAX)
                                r = rule_add_key(&rule_tmp, TK_M_TAG, op, value, NULL);
                        else
                                r = rule_add_key(&rule_tmp, TK_A_TAG, op, value, NULL);
                        if (r < 0)
                                LOG_AND_RETURN_ADD_KEY;

                } else if (streq(key, "PROGRAM")) {
                        if (op == OP_REMOVE)
                                LOG_AND_RETURN("Invalid %s operation", key);

                        if (rule_add_key(&rule_tmp, TK_M_PROGRAM, op, value, NULL) < 0)
                                LOG_AND_RETURN_ADD_KEY;

                } else if (streq(key, "RESULT")) {
                        if (op > OP_MATCH_MAX)
                                LOG_AND_RETURN("Invalid %s operation", key);

                        if (rule_add_key(&rule_tmp, TK_M_RESULT, op, value, NULL) < 0)
                                LOG_AND_RETURN_ADD_KEY;

                } else if (startswith(key, "IMPORT")) {
                        attr = get_key_attribute(key + STRLEN("IMPORT"));
                        if (!attr) {
                                LOG_RULE_WARNING("Ignoring IMPORT{} with missing type");
                                continue;
                        }
                        if (op == OP_REMOVE)
                                LOG_AND_RETURN("Invalid %s operation", "IMPORT");

                        if (streq(attr, "program")) {
                                /* find known built-in command */
                                if (value[0] != '/') {
                                        const enum udev_builtin_cmd cmd = udev_builtin_lookup(value);

                                        if (cmd >= 0) {
                                                LOG_RULE_DEBUG("IMPORT found builtin '%s', replacing", value);
                                                if (rule_add_key(&rule_tmp, TK_M_IMPORT_BUILTIN, op, value, &cmd) < 0)
                                                        LOG_AND_RETURN_ADD_KEY;
                                                continue;
                                        }
                                }
                                r = rule_add_key(&rule_tmp, TK_M_IMPORT_PROG, op, value, NULL);
                        } else if (streq(attr, "builtin")) {
                                const enum udev_builtin_cmd cmd = udev_builtin_lookup(value);

                                if (cmd < 0) {
                                        LOG_RULE_WARNING("IMPORT{builtin} '%s' unknown, ignoring", value);
                                        continue;
                                } else
                                        r = rule_add_key(&rule_tmp, TK_M_IMPORT_BUILTIN, op, value, &cmd);
                        } else if (streq(attr, "file"))
                                r = rule_add_key(&rule_tmp, TK_M_IMPORT_FILE, op, value, NULL);
                        else if (streq(attr, "db"))
                                r = rule_add_key(&rule_tmp, TK_M_IMPORT_DB, op, value, NULL);
                        else if (streq(attr, "cmdline"))
                                r = rule_add_key(&rule_tmp, TK_M_IMPORT_CMDLINE, op, value, NULL);
                        else if (streq(attr, "parent"))
                                r = rule_add_key(&rule_tmp, TK_M_IMPORT_PARENT, op, value, NULL);
                        else {
                                LOG_RULE_ERROR("Ignoring unknown %s{} type '%s'", "IMPORT", attr);
                                continue;
                        }
                        if (r < 0)
                                LOG_AND_RETURN_ADD_KEY;

                } else if (startswith(key, "TEST")) {
                        mode_t mode = 0;

                        if (op > OP_MATCH_MAX)
                                LOG_AND_RETURN("Invalid %s operation", "TEST");

                        attr = get_key_attribute(key + STRLEN("TEST"));
                        if (attr) {
                                mode = strtol(attr, NULL, 8);
                                r = rule_add_key(&rule_tmp, TK_M_TEST, op, value, &mode);
                        } else
                                r = rule_add_key(&rule_tmp, TK_M_TEST, op, value, NULL);
                        if (r < 0)
                                LOG_AND_RETURN_ADD_KEY;

                } else if (startswith(key, "RUN")) {
                        attr = get_key_attribute(key + STRLEN("RUN"));
                        if (!attr)
                                attr = "program";
                        if (op == OP_REMOVE)
                                LOG_AND_RETURN("Invalid %s operation", "RUN");

                        if (streq(attr, "builtin")) {
                                const enum udev_builtin_cmd cmd = udev_builtin_lookup(value);

                                if (cmd < 0) {
                                        LOG_RULE_ERROR("RUN{builtin}: '%s' unknown, ignoring", value);
                                        continue;
                                } else
                                        r = rule_add_key(&rule_tmp, TK_A_RUN_BUILTIN, op, value, &cmd);
                        } else if (streq(attr, "program")) {
                                const enum udev_builtin_cmd cmd = _UDEV_BUILTIN_MAX;

                                r = rule_add_key(&rule_tmp, TK_A_RUN_PROGRAM, op, value, &cmd);
                        } else {
                                LOG_RULE_ERROR("Ignoring unknown %s{} type '%s'", "RUN", attr);
                                continue;
                        }
                        if (r < 0)
                                LOG_AND_RETURN_ADD_KEY;

                } else if (streq(key, "LABEL")) {
                        if (op == OP_REMOVE)
                                LOG_AND_RETURN("Invalid %s operation", key);

                        rule_tmp.rule.rule.label_off = rules_add_string(rules, value);

                } else if (streq(key, "GOTO")) {
                        if (op == OP_REMOVE)
                                LOG_AND_RETURN("Invalid %s operation", key);

                        if (rule_add_key(&rule_tmp, TK_A_GOTO, 0, value, NULL) < 0)
                                LOG_AND_RETURN_ADD_KEY;

                } else if (startswith(key, "NAME")) {
                        if (op == OP_REMOVE)
                                LOG_AND_RETURN("Invalid %s operation", key);

                        if (op < OP_MATCH_MAX)
                                r = rule_add_key(&rule_tmp, TK_M_NAME, op, value, NULL);
                        else {
                                if (streq(value, "%k")) {
                                        LOG_RULE_WARNING("NAME=\"%%k\" is ignored, because it breaks kernel supplied names; please remove");
                                        continue;
                                }
                                if (isempty(value)) {
                                        LOG_RULE_DEBUG("NAME=\"\" is ignored, because udev will not delete any device nodes; please remove");
                                        continue;
                                }
                                r = rule_add_key(&rule_tmp, TK_A_NAME, op, value, NULL);
                        }
                        if (r < 0)
                                LOG_AND_RETURN_ADD_KEY;
                        rule_tmp.rule.rule.can_set_name = true;

                } else if (streq(key, "SYMLINK")) {
                        if (op == OP_REMOVE)
                                LOG_AND_RETURN("Invalid %s operation", key);

                        if (op < OP_MATCH_MAX)
                                r = rule_add_key(&rule_tmp, TK_M_DEVLINK, op, value, NULL);
                        else
                                r = rule_add_key(&rule_tmp, TK_A_DEVLINK, op, value, NULL);
                        if (r < 0)
                                LOG_AND_RETURN_ADD_KEY;
                        rule_tmp.rule.rule.can_set_name = true;

                } else if (streq(key, "OWNER")) {
                        uid_t uid;
                        char *endptr;

                        if (op == OP_REMOVE)
                                LOG_AND_RETURN("Invalid %s operation", key);

                        uid = strtoul(value, &endptr, 10);
                        if (endptr[0] == '\0')
                                r = rule_add_key(&rule_tmp, TK_A_OWNER_ID, op, NULL, &uid);
                        else if (rules->resolve_name_timing == RESOLVE_NAME_EARLY && !strchr("$%", value[0])) {
                                uid = add_uid(rules, value);
                                r = rule_add_key(&rule_tmp, TK_A_OWNER_ID, op, NULL, &uid);
                        } else if (rules->resolve_name_timing != RESOLVE_NAME_NEVER)
                                r = rule_add_key(&rule_tmp, TK_A_OWNER, op, value, NULL);
                        else {
                                LOG_RULE_DEBUG("Resolving user name is disabled, ignoring %s=%s", key, value);
                                continue;
                        }
                        if (r < 0)
                                LOG_AND_RETURN_ADD_KEY;

                        rule_tmp.rule.rule.can_set_name = true;

                } else if (streq(key, "GROUP")) {
                        gid_t gid;
                        char *endptr;

                        if (op == OP_REMOVE)
                                LOG_AND_RETURN("Invalid %s operation", key);

                        gid = strtoul(value, &endptr, 10);
                        if (endptr[0] == '\0')
                                r = rule_add_key(&rule_tmp, TK_A_GROUP_ID, op, NULL, &gid);
                        else if ((rules->resolve_name_timing == RESOLVE_NAME_EARLY) && !strchr("$%", value[0])) {
                                gid = add_gid(rules, value);
                                r = rule_add_key(&rule_tmp, TK_A_GROUP_ID, op, NULL, &gid);
                        } else if (rules->resolve_name_timing != RESOLVE_NAME_NEVER)
                                r = rule_add_key(&rule_tmp, TK_A_GROUP, op, value, NULL);
                        else {
                                LOG_RULE_DEBUG("Resolving group name is disabled, ignoring %s=%s", key, value);
                                continue;
                        }
                        if (r < 0)
                                LOG_AND_RETURN_ADD_KEY;

                        rule_tmp.rule.rule.can_set_name = true;

                } else if (streq(key, "MODE")) {
                        mode_t mode;
                        char *endptr;

                        if (op == OP_REMOVE)
                                LOG_AND_RETURN("Invalid %s operation", key);

                        mode = strtol(value, &endptr, 8);
                        if (endptr[0] == '\0')
                                r = rule_add_key(&rule_tmp, TK_A_MODE_ID, op, NULL, &mode);
                        else
                                r = rule_add_key(&rule_tmp, TK_A_MODE, op, value, NULL);
                        if (r < 0)
                                LOG_AND_RETURN_ADD_KEY;

                        rule_tmp.rule.rule.can_set_name = true;

                } else if (streq(key, "OPTIONS")) {
                        const char *pos;

                        if (op == OP_REMOVE)
                                LOG_AND_RETURN("Invalid %s operation", key);

                        pos = strstr(value, "link_priority=");
                        if (pos) {
                                int prio = atoi(pos + STRLEN("link_priority="));

                                if (rule_add_key(&rule_tmp, TK_A_DEVLINK_PRIO, op, NULL, &prio) < 0)
                                        LOG_AND_RETURN_ADD_KEY;
                        }

                        pos = strstr(value, "string_escape=");
                        if (pos) {
                                pos += STRLEN("string_escape=");
                                if (startswith(pos, "none"))
                                        r = rule_add_key(&rule_tmp, TK_A_STRING_ESCAPE_NONE, op, NULL, NULL);
                                else if (startswith(pos, "replace"))
                                        r = rule_add_key(&rule_tmp, TK_A_STRING_ESCAPE_REPLACE, op, NULL, NULL);
                                else {
                                        LOG_RULE_ERROR("OPTIONS: unknown string_escape mode '%s', ignoring", pos);
                                        r = 0;
                                }
                                if (r < 0)
                                        LOG_AND_RETURN_ADD_KEY;
                        }

                        pos = strstr(value, "db_persist");
                        if (pos)
                                if (rule_add_key(&rule_tmp, TK_A_DB_PERSIST, op, NULL, NULL) < 0)
                                        LOG_AND_RETURN_ADD_KEY;

                        pos = strstr(value, "nowatch");
                        if (pos) {
                                static const int zero = 0;
                                if (rule_add_key(&rule_tmp, TK_A_INOTIFY_WATCH, op, NULL, &zero) < 0)
                                        LOG_AND_RETURN_ADD_KEY;
                        } else {
                                static const int one = 1;
                                pos = strstr(value, "watch");
                                if (pos)
                                        if (rule_add_key(&rule_tmp, TK_A_INOTIFY_WATCH, op, NULL, &one) < 0)
                                                LOG_AND_RETURN_ADD_KEY;
                        }

                        pos = strstr(value, "static_node=");
                        if (pos) {
                                pos += STRLEN("static_node=");
                                if (rule_add_key(&rule_tmp, TK_A_STATIC_NODE, op, pos, NULL) < 0)
                                        LOG_AND_RETURN_ADD_KEY;
                                rule_tmp.rule.rule.has_static_node = true;
                        }

                } else
                        LOG_AND_RETURN("Unknown key '%s'", key);
        }

        /* add rule token and sort tokens */
        rule_tmp.rule.rule.token_count = 1 + rule_tmp.token_cur;
        if (add_token(rules, &rule_tmp.rule) != 0 || sort_token(rules, &rule_tmp) != 0)
                LOG_RULE_ERROR("Failed to add rule token");
}

static int parse_file(UdevRules *rules, const char *filename) {
        _cleanup_fclose_ FILE *f = NULL;
        unsigned first_token;
        unsigned filename_off;
        char line[UTIL_LINE_SIZE];
        int line_nr = 0;
        unsigned i;

        f = fopen(filename, "re");
        if (!f) {
                if (errno == ENOENT)
                        return 0;

                return -errno;
        }

        if (null_or_empty_fd(fileno(f))) {
                log_debug("Skipping empty file: %s", filename);
                return 0;
        } else
                log_debug("Reading rules file: %s", filename);

        first_token = rules->token_cur;
        filename_off = rules_add_string(rules, filename);

        while (fgets(line, sizeof(line), f)) {
                char *key;
                size_t len;

                /* skip whitespace */
                line_nr++;
                key = line;
                while (isspace(key[0]))
                        key++;

                /* comment */
                if (key[0] == '#')
                        continue;

                len = strlen(line);
                if (len < 3)
                        continue;

                /* continue reading if backslash+newline is found */
                while (line[len-2] == '\\') {
                        if (!fgets(&line[len-2], (sizeof(line)-len)+2, f))
                                break;
                        if (strlen(&line[len-2]) < 2)
                                break;
                        line_nr++;
                        len = strlen(line);
                }

                if (len+1 >= sizeof(line)) {
                        log_error("Line too long '%s':%u, ignored", filename, line_nr);
                        continue;
                }
                add_rule(rules, key, filename, filename_off, line_nr);
        }

        /* link GOTOs to LABEL rules in this file to be able to fast-forward */
        for (i = first_token+1; i < rules->token_cur; i++) {
                if (rules->tokens[i].type == TK_A_GOTO) {
                        char *label = rules_str(rules, rules->tokens[i].key.value_off);
                        unsigned j;

                        for (j = i+1; j < rules->token_cur; j++) {
                                if (rules->tokens[j].type != TK_RULE)
                                        continue;
                                if (rules->tokens[j].rule.label_off == 0)
                                        continue;
                                if (!streq(label, rules_str(rules, rules->tokens[j].rule.label_off)))
                                        continue;
                                rules->tokens[i].key.rule_goto = j;
                                break;
                        }
                        if (rules->tokens[i].key.rule_goto == 0)
                                log_error("GOTO '%s' has no matching label in: '%s'", label, filename);
                }
        }
        return 0;
}

int udev_rules_new(UdevRules **ret_rules, ResolveNameTiming resolve_name_timing) {
        _cleanup_(udev_rules_freep) UdevRules *rules = NULL;
        _cleanup_strv_free_ char **files = NULL;
        char **f;
        int r;

        assert(resolve_name_timing >= 0 && resolve_name_timing < _RESOLVE_NAME_TIMING_MAX);

        rules = new(UdevRules, 1);
        if (!rules)
                return -ENOMEM;

        *rules = (UdevRules) {
                .resolve_name_timing = resolve_name_timing,
        };

        /* init token array and string buffer */
        rules->tokens = malloc_multiply(PREALLOC_TOKEN, sizeof(struct token));
        if (!rules->tokens)
                return -ENOMEM;
        rules->token_max = PREALLOC_TOKEN;

        rules->strbuf = strbuf_new();
        if (!rules->strbuf)
                return -ENOMEM;

        udev_rules_check_timestamp(rules);

        r = conf_files_list_strv(&files, ".rules", NULL, 0, rules_dirs);
        if (r < 0)
                return log_error_errno(r, "Failed to enumerate rules files: %m");

        /*
         * The offset value in the rules strct is limited; add all
         * rules file names to the beginning of the string buffer.
         */
        STRV_FOREACH(f, files)
                rules_add_string(rules, *f);

        STRV_FOREACH(f, files)
                parse_file(rules, *f);

        struct token end_token = { .type = TK_END };
        add_token(rules, &end_token);
        log_debug("Rules contain %zu bytes tokens (%u * %zu bytes), %zu bytes strings",
                  rules->token_max * sizeof(struct token), rules->token_max, sizeof(struct token), rules->strbuf->len);

        /* cleanup temporary strbuf data */
        log_debug("%zu strings (%zu bytes), %zu de-duplicated (%zu bytes), %zu trie nodes used",
                  rules->strbuf->in_count, rules->strbuf->in_len,
                  rules->strbuf->dedup_count, rules->strbuf->dedup_len, rules->strbuf->nodes_count);
        strbuf_complete(rules->strbuf);

        /* cleanup uid/gid cache */
        rules->uids = mfree(rules->uids);
        rules->uids_cur = 0;
        rules->uids_max = 0;
        rules->gids = mfree(rules->gids);
        rules->gids_cur = 0;
        rules->gids_max = 0;

        dump_rules(rules);
        *ret_rules = TAKE_PTR(rules);
        return 0;
}

UdevRules *udev_rules_free(UdevRules *rules) {
        if (!rules)
                return NULL;
        free(rules->tokens);
        strbuf_cleanup(rules->strbuf);
        free(rules->uids);
        free(rules->gids);
        return mfree(rules);
}

bool udev_rules_check_timestamp(UdevRules *rules) {
        if (!rules)
                return false;

        return paths_check_timestamp(rules_dirs, &rules->dirs_ts_usec, true);
}

static int match_key(UdevRules *rules, struct token *token, const char *val) {
        char *key_value = rules_str(rules, token->key.value_off);
        char *pos;
        bool match = false;

        if (!val)
                val = "";

        switch (token->key.glob) {
        case GL_PLAIN:
                match = (streq(key_value, val));
                break;
        case GL_GLOB:
                match = (fnmatch(key_value, val, 0) == 0);
                break;
        case GL_SPLIT:
                {
                        const char *s;
                        size_t len;

                        s = rules_str(rules, token->key.value_off);
                        len = strlen(val);
                        for (;;) {
                                const char *next;

                                next = strchr(s, '|');
                                if (next) {
                                        size_t matchlen = (size_t)(next - s);

                                        match = (matchlen == len && strneq(s, val, matchlen));
                                        if (match)
                                                break;
                                } else {
                                        match = (streq(s, val));
                                        break;
                                }
                                s = &next[1];
                        }
                        break;
                }
        case GL_SPLIT_GLOB:
                {
                        char value[UTIL_PATH_SIZE];

                        strscpy(value, sizeof(value), rules_str(rules, token->key.value_off));
                        key_value = value;
                        while (key_value) {
                                pos = strchr(key_value, '|');
                                if (pos) {
                                        pos[0] = '\0';
                                        pos = &pos[1];
                                }
                                match = (fnmatch(key_value, val, 0) == 0);
                                if (match)
                                        break;
                                key_value = pos;
                        }
                        break;
                }
        case GL_SOMETHING:
                match = (val[0] != '\0');
                break;
        case GL_UNSET:
                return -1;
        }

        if (match && (token->key.op == OP_MATCH))
                return 0;
        if (!match && (token->key.op == OP_NOMATCH))
                return 0;
        return -1;
}

static int match_attr(UdevRules *rules, sd_device *dev, UdevEvent *event, struct token *cur) {
        char nbuf[UTIL_NAME_SIZE], vbuf[UTIL_NAME_SIZE];
        const char *name, *value;
        size_t len;

        name = rules_str(rules, cur->key.attr_off);
        switch (cur->key.attrsubst) {
        case SB_FORMAT:
                udev_event_apply_format(event, name, nbuf, sizeof(nbuf), false);
                name = nbuf;
                _fallthrough_;
        case SB_NONE:
                if (sd_device_get_sysattr_value(dev, name, &value) < 0)
                        return -1;
                break;
        case SB_SUBSYS:
                if (util_resolve_subsys_kernel(name, vbuf, sizeof(vbuf), true) != 0)
                        return -1;
                value = vbuf;
                break;
        default:
                return -1;
        }

        /* remove trailing whitespace, if not asked to match for it */
        len = strlen(value);
        if (len > 0 && isspace(value[len-1])) {
                const char *key_value;
                size_t klen;

                key_value = rules_str(rules, cur->key.value_off);
                klen = strlen(key_value);
                if (klen > 0 && !isspace(key_value[klen-1])) {
                        if (value != vbuf) {
                                strscpy(vbuf, sizeof(vbuf), value);
                                value = vbuf;
                        }
                        while (len > 0 && isspace(vbuf[--len]))
                                vbuf[len] = '\0';
                }
        }

        return match_key(rules, cur, value);
}

enum escape_type {
        ESCAPE_UNSET,
        ESCAPE_NONE,
        ESCAPE_REPLACE,
};

int udev_rules_apply_to_event(
                UdevRules *rules,
                UdevEvent *event,
                usec_t timeout_usec,
                Hashmap *properties_list) {
        sd_device *dev = event->dev;
        enum escape_type esc = ESCAPE_UNSET;
        struct token *cur, *rule;
        const char *action, *val;
        bool can_set_name;
        int r;

        if (!rules->tokens)
                return 0;

        r = sd_device_get_property_value(dev, "ACTION", &action);
        if (r < 0)
                return r;

        can_set_name = (!streq(action, "remove") &&
                        (sd_device_get_devnum(dev, NULL) >= 0 ||
                         sd_device_get_ifindex(dev, NULL) >= 0));

        /* loop through token list, match, run actions or forward to next rule */
        cur = &rules->tokens[0];
        rule = cur;
        for (;;) {
                dump_token(rules, cur);
                switch (cur->type) {
                case TK_RULE:
                        /* current rule */
                        rule = cur;
                        /* possibly skip rules which want to set NAME, SYMLINK, OWNER, GROUP, MODE */
                        if (!can_set_name && rule->rule.can_set_name)
                                goto nomatch;
                        esc = ESCAPE_UNSET;
                        break;
                case TK_M_ACTION:
                        if (match_key(rules, cur, action) != 0)
                                goto nomatch;
                        break;
                case TK_M_DEVPATH:
                        if (sd_device_get_devpath(dev, &val) < 0)
                                goto nomatch;
                        if (match_key(rules, cur, val) != 0)
                                goto nomatch;
                        break;
                case TK_M_KERNEL:
                        if (sd_device_get_sysname(dev, &val) < 0)
                                goto nomatch;
                        if (match_key(rules, cur, val) != 0)
                                goto nomatch;
                        break;
                case TK_M_DEVLINK: {
                        const char *devlink;
                        bool match = false;

                        FOREACH_DEVICE_DEVLINK(dev, devlink)
                                if (match_key(rules, cur, devlink + STRLEN("/dev/")) == 0) {
                                        match = true;
                                        break;
                                }

                        if (!match)
                                goto nomatch;
                        break;
                }
                case TK_M_NAME:
                        if (match_key(rules, cur, event->name) != 0)
                                goto nomatch;
                        break;
                case TK_M_ENV: {
                        const char *key_name = rules_str(rules, cur->key.attr_off);

                        if (sd_device_get_property_value(dev, key_name, &val) < 0) {
                                /* check global properties */
                                if (properties_list)
                                        val = hashmap_get(properties_list, key_name);
                                else
                                        val = NULL;
                        }

                        if (match_key(rules, cur, strempty(val)))
                                goto nomatch;
                        break;
                }
                case TK_M_TAG: {
                        bool match = false;
                        const char *tag;

                        FOREACH_DEVICE_TAG(dev, tag)
                                if (streq(rules_str(rules, cur->key.value_off), tag)) {
                                        match = true;
                                        break;
                                }

                        if ((!match && (cur->key.op != OP_NOMATCH)) ||
                            (match && (cur->key.op == OP_NOMATCH)))
                                goto nomatch;
                        break;
                }
                case TK_M_SUBSYSTEM:
                        if (sd_device_get_subsystem(dev, &val) < 0)
                                goto nomatch;
                        if (match_key(rules, cur, val) != 0)
                                goto nomatch;
                        break;
                case TK_M_DRIVER:
                        if (sd_device_get_driver(dev, &val) < 0)
                                goto nomatch;
                        if (match_key(rules, cur, val) != 0)
                                goto nomatch;
                        break;
                case TK_M_ATTR:
                        if (match_attr(rules, dev, event, cur) != 0)
                                goto nomatch;
                        break;
                case TK_M_SYSCTL: {
                        char filename[UTIL_PATH_SIZE];
                        _cleanup_free_ char *value = NULL;
                        size_t len;

                        udev_event_apply_format(event, rules_str(rules, cur->key.attr_off), filename, sizeof(filename), false);
                        sysctl_normalize(filename);
                        if (sysctl_read(filename, &value) < 0)
                                goto nomatch;

                        len = strlen(value);
                        while (len > 0 && isspace(value[--len]))
                                value[len] = '\0';
                        if (match_key(rules, cur, value) != 0)
                                goto nomatch;
                        break;
                }
                case TK_M_KERNELS:
                case TK_M_SUBSYSTEMS:
                case TK_M_DRIVERS:
                case TK_M_ATTRS:
                case TK_M_TAGS: {
                        struct token *next;

                        /* get whole sequence of parent matches */
                        next = cur;
                        while (next->type > TK_M_PARENTS_MIN && next->type < TK_M_PARENTS_MAX)
                                next++;

                        /* loop over parents */
                        event->dev_parent = dev;
                        for (;;) {
                                struct token *key;

                                /* loop over sequence of parent match keys */
                                for (key = cur; key < next; key++ ) {
                                        dump_token(rules, key);
                                        switch(key->type) {
                                        case TK_M_KERNELS:
                                                if (sd_device_get_sysname(event->dev_parent, &val) < 0)
                                                        goto try_parent;
                                                if (match_key(rules, key, val) != 0)
                                                        goto try_parent;
                                                break;
                                        case TK_M_SUBSYSTEMS:
                                                if (sd_device_get_subsystem(event->dev_parent, &val) < 0)
                                                        goto try_parent;
                                                if (match_key(rules, key, val) != 0)
                                                        goto try_parent;
                                                break;
                                        case TK_M_DRIVERS:
                                                if (sd_device_get_driver(event->dev_parent, &val) < 0)
                                                        goto try_parent;
                                                if (match_key(rules, key, val) != 0)
                                                        goto try_parent;
                                                break;
                                        case TK_M_ATTRS:
                                                if (match_attr(rules, event->dev_parent, event, key) != 0)
                                                        goto try_parent;
                                                break;
                                        case TK_M_TAGS: {
                                                bool match = sd_device_has_tag(event->dev_parent, rules_str(rules, cur->key.value_off));

                                                if (match && key->key.op == OP_NOMATCH)
                                                        goto try_parent;
                                                if (!match && key->key.op == OP_MATCH)
                                                        goto try_parent;
                                                break;
                                        }
                                        default:
                                                goto nomatch;
                                        }
                                }
                                break;

                        try_parent:
                                if (sd_device_get_parent(event->dev_parent, &event->dev_parent) < 0) {
                                        event->dev_parent = NULL;
                                        goto nomatch;
                                }
                        }
                        /* move behind our sequence of parent match keys */
                        cur = next;
                        continue;
                }
                case TK_M_TEST: {
                        char filename[UTIL_PATH_SIZE];
                        struct stat statbuf;
                        int match;

                        udev_event_apply_format(event, rules_str(rules, cur->key.value_off), filename, sizeof(filename), false);
                        if (util_resolve_subsys_kernel(filename, filename, sizeof(filename), false) != 0) {
                                if (filename[0] != '/') {
                                        char tmp[UTIL_PATH_SIZE];

                                        if (sd_device_get_syspath(dev, &val) < 0)
                                                goto nomatch;

                                        strscpy(tmp, sizeof(tmp), filename);
                                        strscpyl(filename, sizeof(filename), val, "/", tmp, NULL);
                                }
                        }
                        attr_subst_subdir(filename, sizeof(filename));

                        match = (stat(filename, &statbuf) == 0);
                        if (match && cur->key.mode > 0)
                                match = ((statbuf.st_mode & cur->key.mode) > 0);
                        if (match && cur->key.op == OP_NOMATCH)
                                goto nomatch;
                        if (!match && cur->key.op == OP_MATCH)
                                goto nomatch;
                        break;
                }
                case TK_M_PROGRAM: {
                        char program[UTIL_PATH_SIZE], result[UTIL_LINE_SIZE];

                        event->program_result = mfree(event->program_result);
                        udev_event_apply_format(event, rules_str(rules, cur->key.value_off), program, sizeof(program), false);
                        log_device_debug(dev, "PROGRAM '%s' %s:%u",
                                         program,
                                         rules_str(rules, rule->rule.filename_off),
                                         rule->rule.filename_line);

                        if (udev_event_spawn(event, timeout_usec, true, program, result, sizeof(result)) != 0) {
                                if (cur->key.op != OP_NOMATCH)
                                        goto nomatch;
                        } else {
                                int count;

                                delete_trailing_chars(result, "\n");
                                if (IN_SET(esc, ESCAPE_UNSET, ESCAPE_REPLACE)) {
                                        count = util_replace_chars(result, UDEV_ALLOWED_CHARS_INPUT);
                                        if (count > 0)
                                                log_device_debug(dev, "Replaced %i character(s) from result of '%s'" , count, program);
                                }
                                event->program_result = strdup(result);
                                if (cur->key.op == OP_NOMATCH)
                                        goto nomatch;
                        }
                        break;
                }
                case TK_M_IMPORT_FILE: {
                        char import[UTIL_PATH_SIZE];

                        udev_event_apply_format(event, rules_str(rules, cur->key.value_off), import, sizeof(import), false);
                        if (import_file_into_properties(dev, import) != 0)
                                if (cur->key.op != OP_NOMATCH)
                                        goto nomatch;
                        break;
                }
                case TK_M_IMPORT_PROG: {
                        char import[UTIL_PATH_SIZE];

                        udev_event_apply_format(event, rules_str(rules, cur->key.value_off), import, sizeof(import), false);
                        log_device_debug(dev, "IMPORT '%s' %s:%u",
                                         import,
                                         rules_str(rules, rule->rule.filename_off),
                                         rule->rule.filename_line);

                        if (import_program_into_properties(event, timeout_usec, import) != 0)
                                if (cur->key.op != OP_NOMATCH)
                                        goto nomatch;
                        break;
                }
                case TK_M_IMPORT_BUILTIN: {
                        char command[UTIL_PATH_SIZE];

                        if (udev_builtin_run_once(cur->key.builtin_cmd)) {
                                /* check if we ran already */
                                if (event->builtin_run & (1 << cur->key.builtin_cmd)) {
                                        log_device_debug(dev, "IMPORT builtin skip '%s' %s:%u",
                                                         udev_builtin_name(cur->key.builtin_cmd),
                                                         rules_str(rules, rule->rule.filename_off),
                                                         rule->rule.filename_line);
                                        /* return the result from earlier run */
                                        if (event->builtin_ret & (1 << cur->key.builtin_cmd))
                                                if (cur->key.op != OP_NOMATCH)
                                                        goto nomatch;
                                        break;
                                }
                                /* mark as ran */
                                event->builtin_run |= (1 << cur->key.builtin_cmd);
                        }

                        udev_event_apply_format(event, rules_str(rules, cur->key.value_off), command, sizeof(command), false);
                        log_device_debug(dev, "IMPORT builtin '%s' %s:%u",
                                         udev_builtin_name(cur->key.builtin_cmd),
                                         rules_str(rules, rule->rule.filename_off),
                                         rule->rule.filename_line);

                        r = udev_builtin_run(dev, cur->key.builtin_cmd, command, false);
                        if (r < 0) {
                                /* remember failure */
                                log_device_debug_errno(dev, r, "IMPORT builtin '%s' fails: %m",
                                                       udev_builtin_name(cur->key.builtin_cmd));
                                event->builtin_ret |= (1 << cur->key.builtin_cmd);
                                if (cur->key.op != OP_NOMATCH)
                                        goto nomatch;
                        }
                        break;
                }
                case TK_M_IMPORT_DB: {
                        const char *key;

                        key = rules_str(rules, cur->key.value_off);
                        if (event->dev_db_clone &&
                            sd_device_get_property_value(event->dev_db_clone, key, &val) >= 0)
                                device_add_property(dev, key, val);
                        else if (cur->key.op != OP_NOMATCH)
                                goto nomatch;
                        break;
                }
                case TK_M_IMPORT_CMDLINE: {
                        _cleanup_free_ char *value = NULL;
                        bool imported = false;
                        const char *key;

                        key = rules_str(rules, cur->key.value_off);
                        r = proc_cmdline_get_key(key, PROC_CMDLINE_VALUE_OPTIONAL, &value);
                        if (r < 0)
                                log_device_debug_errno(dev, r, "Failed to read %s from /proc/cmdline, ignoring: %m", key);
                        else if (r > 0) {
                                imported = true;

                                if (value)
                                        device_add_property(dev, key, value);
                                else
                                        /* we import simple flags as 'FLAG=1' */
                                        device_add_property(dev, key, "1");
                        }

                        if (!imported && cur->key.op != OP_NOMATCH)
                                goto nomatch;
                        break;
                }
                case TK_M_IMPORT_PARENT: {
                        char import[UTIL_PATH_SIZE];

                        udev_event_apply_format(event, rules_str(rules, cur->key.value_off), import, sizeof(import), false);
                        if (import_parent_into_properties(dev, import) != 0)
                                if (cur->key.op != OP_NOMATCH)
                                        goto nomatch;
                        break;
                }
                case TK_M_RESULT:
                        if (match_key(rules, cur, event->program_result) != 0)
                                goto nomatch;
                        break;
                case TK_A_STRING_ESCAPE_NONE:
                        esc = ESCAPE_NONE;
                        break;
                case TK_A_STRING_ESCAPE_REPLACE:
                        esc = ESCAPE_REPLACE;
                        break;
                case TK_A_DB_PERSIST:
                        device_set_db_persist(dev);
                        break;
                case TK_A_INOTIFY_WATCH:
                        if (event->inotify_watch_final)
                                break;
                        if (cur->key.op == OP_ASSIGN_FINAL)
                                event->inotify_watch_final = true;
                        event->inotify_watch = cur->key.watch;
                        break;
                case TK_A_DEVLINK_PRIO:
                        device_set_devlink_priority(dev, cur->key.devlink_prio);
                        break;
                case TK_A_OWNER: {
                        char owner[UTIL_NAME_SIZE];
                        const char *ow = owner;

                        if (event->owner_final)
                                break;
                        if (cur->key.op == OP_ASSIGN_FINAL)
                                event->owner_final = true;
                        udev_event_apply_format(event, rules_str(rules, cur->key.value_off), owner, sizeof(owner), false);
                        event->owner_set = true;
                        r = get_user_creds(&ow, &event->uid, NULL, NULL, NULL, USER_CREDS_ALLOW_MISSING);
                        if (r < 0) {
                                log_unknown_owner(dev, r, "user", owner);
                                event->uid = 0;
                        }
                        log_device_debug(dev, "OWNER %u %s:%u",
                                         event->uid,
                                         rules_str(rules, rule->rule.filename_off),
                                         rule->rule.filename_line);
                        break;
                }
                case TK_A_GROUP: {
                        char group[UTIL_NAME_SIZE];
                        const char *gr = group;

                        if (event->group_final)
                                break;
                        if (cur->key.op == OP_ASSIGN_FINAL)
                                event->group_final = true;
                        udev_event_apply_format(event, rules_str(rules, cur->key.value_off), group, sizeof(group), false);
                        event->group_set = true;
                        r = get_group_creds(&gr, &event->gid, USER_CREDS_ALLOW_MISSING);
                        if (r < 0) {
                                log_unknown_owner(dev, r, "group", group);
                                event->gid = 0;
                        }
                        log_device_debug(dev, "GROUP %u %s:%u",
                                         event->gid,
                                         rules_str(rules, rule->rule.filename_off),
                                         rule->rule.filename_line);
                        break;
                }
                case TK_A_MODE: {
                        char mode_str[UTIL_NAME_SIZE];
                        mode_t mode;

                        if (event->mode_final)
                                break;
                        udev_event_apply_format(event, rules_str(rules, cur->key.value_off), mode_str, sizeof(mode_str), false);
                        r = parse_mode(mode_str, &mode);
                        if (r < 0) {
                                log_device_error_errno(dev, r, "Failed to parse mode '%s': %m", mode_str);
                                break;
                        }
                        if (cur->key.op == OP_ASSIGN_FINAL)
                                event->mode_final = true;
                        event->mode_set = true;
                        event->mode = mode;
                        log_device_debug(dev, "MODE %#o %s:%u",
                                         event->mode,
                                         rules_str(rules, rule->rule.filename_off),
                                         rule->rule.filename_line);
                        break;
                }
                case TK_A_OWNER_ID:
                        if (event->owner_final)
                                break;
                        if (cur->key.op == OP_ASSIGN_FINAL)
                                event->owner_final = true;
                        event->owner_set = true;
                        event->uid = cur->key.uid;
                        log_device_debug(dev, "OWNER %u %s:%u",
                                         event->uid,
                                         rules_str(rules, rule->rule.filename_off),
                                         rule->rule.filename_line);
                        break;
                case TK_A_GROUP_ID:
                        if (event->group_final)
                                break;
                        if (cur->key.op == OP_ASSIGN_FINAL)
                                event->group_final = true;
                        event->group_set = true;
                        event->gid = cur->key.gid;
                        log_device_debug(dev, "GROUP %u %s:%u",
                                         event->gid,
                                         rules_str(rules, rule->rule.filename_off),
                                         rule->rule.filename_line);
                        break;
                case TK_A_MODE_ID:
                        if (event->mode_final)
                                break;
                        if (cur->key.op == OP_ASSIGN_FINAL)
                                event->mode_final = true;
                        event->mode_set = true;
                        event->mode = cur->key.mode;
                        log_device_debug(dev, "MODE %#o %s:%u",
                                         event->mode,
                                         rules_str(rules, rule->rule.filename_off),
                                         rule->rule.filename_line);
                        break;
                case TK_A_SECLABEL: {
                        _cleanup_free_ char *name = NULL, *label = NULL;
                        char label_str[UTIL_LINE_SIZE] = {};

                        name = strdup(rules_str(rules, cur->key.attr_off));
                        if (!name)
                                return log_oom();

                        udev_event_apply_format(event, rules_str(rules, cur->key.value_off), label_str, sizeof(label_str), false);
                        if (!isempty(label_str))
                                label = strdup(label_str);
                        else
                                label = strdup(rules_str(rules, cur->key.value_off));
                        if (!label)
                                return log_oom();

                        if (IN_SET(cur->key.op, OP_ASSIGN, OP_ASSIGN_FINAL))
                                ordered_hashmap_clear_free_free(event->seclabel_list);

                        r = ordered_hashmap_ensure_allocated(&event->seclabel_list, NULL);
                        if (r < 0)
                                return log_oom();

                        r = ordered_hashmap_put(event->seclabel_list, name, label);
                        if (r < 0)
                                return log_oom();
                        log_device_debug(dev, "SECLABEL{%s}='%s' %s:%u",
                                         name, label,
                                         rules_str(rules, rule->rule.filename_off),
                                         rule->rule.filename_line);
                        name = label = NULL;

                        break;
                }
                case TK_A_ENV: {
                        char value_new[UTIL_NAME_SIZE];
                        const char *name, *value_old;

                        name = rules_str(rules, cur->key.attr_off);
                        val = rules_str(rules, cur->key.value_off);
                        if (val[0] == '\0') {
                                if (cur->key.op == OP_ADD)
                                        break;
                                device_add_property(dev, name, NULL);
                                break;
                        }

                        if (cur->key.op == OP_ADD &&
                            sd_device_get_property_value(dev, name, &value_old) >= 0) {
                                char temp[UTIL_NAME_SIZE];

                                /* append value separated by space */
                                udev_event_apply_format(event, val, temp, sizeof(temp), false);
                                strscpyl(value_new, sizeof(value_new), value_old, " ", temp, NULL);
                        } else
                                udev_event_apply_format(event, val, value_new, sizeof(value_new), false);

                        device_add_property(dev, name, value_new);
                        break;
                }
                case TK_A_TAG: {
                        char tag[UTIL_PATH_SIZE];
                        const char *p;

                        udev_event_apply_format(event, rules_str(rules, cur->key.value_off), tag, sizeof(tag), false);
                        if (IN_SET(cur->key.op, OP_ASSIGN, OP_ASSIGN_FINAL))
                                device_cleanup_tags(dev);
                        for (p = tag; *p != '\0'; p++) {
                                if ((*p >= 'a' && *p <= 'z') ||
                                    (*p >= 'A' && *p <= 'Z') ||
                                    (*p >= '0' && *p <= '9') ||
                                    IN_SET(*p, '-', '_'))
                                        continue;
                                log_device_error(dev, "Ignoring invalid tag name '%s'", tag);
                                break;
                        }
                        if (cur->key.op == OP_REMOVE)
                                device_remove_tag(dev, tag);
                        else
                                device_add_tag(dev, tag);
                        break;
                }
                case TK_A_NAME: {
                        char name_str[UTIL_PATH_SIZE];
                        const char *name;
                        int count;

                        name = rules_str(rules, cur->key.value_off);
                        if (event->name_final)
                                break;
                        if (cur->key.op == OP_ASSIGN_FINAL)
                                event->name_final = true;
                        udev_event_apply_format(event, name, name_str, sizeof(name_str), false);
                        if (IN_SET(esc, ESCAPE_UNSET, ESCAPE_REPLACE)) {
                                count = util_replace_chars(name_str, "/");
                                if (count > 0)
                                        log_device_debug(dev, "Replaced %i character(s) from result of NAME=\"%s\"", count, name);
                        }
                        if (sd_device_get_devnum(dev, NULL) >= 0 &&
                            (sd_device_get_devname(dev, &val) < 0 ||
                             !streq(name_str, val + STRLEN("/dev/")))) {
                                log_device_error(dev, "Kernel device nodes cannot be renamed, ignoring NAME=\"%s\"; please fix it in %s:%u\n",
                                                 name,
                                                 rules_str(rules, rule->rule.filename_off),
                                                 rule->rule.filename_line);
                                break;
                        }
                        if (free_and_strdup(&event->name, name_str) < 0)
                                return log_oom();

                        log_device_debug(dev, "NAME '%s' %s:%u",
                                         event->name,
                                         rules_str(rules, rule->rule.filename_off),
                                         rule->rule.filename_line);
                        break;
                }
                case TK_A_DEVLINK: {
                        char temp[UTIL_PATH_SIZE], filename[UTIL_PATH_SIZE], *pos, *next;
                        int count = 0;

                        if (event->devlink_final)
                                break;
                        if (sd_device_get_devnum(dev, NULL) < 0)
                                break;
                        if (cur->key.op == OP_ASSIGN_FINAL)
                                event->devlink_final = true;
                        if (IN_SET(cur->key.op, OP_ASSIGN, OP_ASSIGN_FINAL))
                                device_cleanup_devlinks(dev);

                        /* allow  multiple symlinks separated by spaces */
                        udev_event_apply_format(event, rules_str(rules, cur->key.value_off), temp, sizeof(temp), esc != ESCAPE_NONE);
                        if (esc == ESCAPE_UNSET)
                                count = util_replace_chars(temp, "/ ");
                        else if (esc == ESCAPE_REPLACE)
                                count = util_replace_chars(temp, "/");
                        if (count > 0)
                                log_device_debug(dev, "Replaced %i character(s) from result of LINK" , count);
                        pos = temp;
                        while (isspace(pos[0]))
                                pos++;
                        next = strchr(pos, ' ');
                        while (next) {
                                next[0] = '\0';
                                log_device_debug(dev, "LINK '%s' %s:%u", pos,
                                                 rules_str(rules, rule->rule.filename_off), rule->rule.filename_line);
                                strscpyl(filename, sizeof(filename), "/dev/", pos, NULL);
                                device_add_devlink(dev, filename);
                                while (isspace(next[1]))
                                        next++;
                                pos = &next[1];
                                next = strchr(pos, ' ');
                        }
                        if (pos[0] != '\0') {
                                log_device_debug(dev, "LINK '%s' %s:%u", pos,
                                                 rules_str(rules, rule->rule.filename_off), rule->rule.filename_line);
                                strscpyl(filename, sizeof(filename), "/dev/", pos, NULL);
                                device_add_devlink(dev, filename);
                        }
                        break;
                }
                case TK_A_ATTR: {
                        char attr[UTIL_PATH_SIZE], value[UTIL_NAME_SIZE];
                        _cleanup_fclose_ FILE *f = NULL;
                        const char *key_name;

                        key_name = rules_str(rules, cur->key.attr_off);
                        if (util_resolve_subsys_kernel(key_name, attr, sizeof(attr), false) != 0 &&
                            sd_device_get_syspath(dev, &val) >= 0)
                                strscpyl(attr, sizeof(attr), val, "/", key_name, NULL);
                        attr_subst_subdir(attr, sizeof(attr));

                        udev_event_apply_format(event, rules_str(rules, cur->key.value_off), value, sizeof(value), false);
                        log_device_debug(dev, "ATTR '%s' writing '%s' %s:%u", attr, value,
                                         rules_str(rules, rule->rule.filename_off),
                                         rule->rule.filename_line);
                        f = fopen(attr, "we");
                        if (!f)
                                log_device_error_errno(dev, errno, "Failed to open ATTR{%s} for writing: %m", attr);
                        else if (fprintf(f, "%s", value) <= 0)
                                log_device_error_errno(dev, errno, "Failed to write ATTR{%s}: %m", attr);
                        break;
                }
                case TK_A_SYSCTL: {
                        char filename[UTIL_PATH_SIZE], value[UTIL_NAME_SIZE];

                        udev_event_apply_format(event, rules_str(rules, cur->key.attr_off), filename, sizeof(filename), false);
                        sysctl_normalize(filename);
                        udev_event_apply_format(event, rules_str(rules, cur->key.value_off), value, sizeof(value), false);
                        log_device_debug(dev, "SYSCTL '%s' writing '%s' %s:%u", filename, value,
                                         rules_str(rules, rule->rule.filename_off), rule->rule.filename_line);
                        r = sysctl_write(filename, value);
                        if (r < 0)
                                log_device_error_errno(dev, r, "Failed to write SYSCTL{%s}='%s': %m", filename, value);
                        break;
                }
                case TK_A_RUN_BUILTIN:
                case TK_A_RUN_PROGRAM: {
                        _cleanup_free_ char *cmd = NULL;

                        if (IN_SET(cur->key.op, OP_ASSIGN, OP_ASSIGN_FINAL))
                                ordered_hashmap_clear_free_key(event->run_list);

                        r = ordered_hashmap_ensure_allocated(&event->run_list, NULL);
                        if (r < 0)
                                return log_oom();

                        cmd = strdup(rules_str(rules, cur->key.value_off));
                        if (!cmd)
                                return log_oom();

                        r = ordered_hashmap_put(event->run_list, cmd, INT_TO_PTR(cur->key.builtin_cmd));
                        if (r < 0)
                                return log_oom();

                        cmd = NULL;

                        log_device_debug(dev, "RUN '%s' %s:%u",
                                         rules_str(rules, cur->key.value_off),
                                         rules_str(rules, rule->rule.filename_off),
                                         rule->rule.filename_line);
                        break;
                }
                case TK_A_GOTO:
                        if (cur->key.rule_goto == 0)
                                break;
                        cur = &rules->tokens[cur->key.rule_goto];
                        continue;
                case TK_END:
                        return 0;

                case TK_M_PARENTS_MIN:
                case TK_M_PARENTS_MAX:
                case TK_M_MAX:
                case TK_UNSET:
                        log_device_error(dev, "Wrong type %u", cur->type);
                        goto nomatch;
                }

                cur++;
                continue;
        nomatch:
                /* fast-forward to next rule */
                cur = rule + rule->rule.token_count;
        }

        return 0;
}

int udev_rules_apply_static_dev_perms(UdevRules *rules) {
        struct token *cur;
        struct token *rule;
        uid_t uid = 0;
        gid_t gid = 0;
        mode_t mode = 0;
        _cleanup_strv_free_ char **tags = NULL;
        char **t;
        FILE *f = NULL;
        _cleanup_free_ char *path = NULL;
        int r;

        if (!rules->tokens)
                return 0;

        cur = &rules->tokens[0];
        rule = cur;
        for (;;) {
                switch (cur->type) {
                case TK_RULE:
                        /* current rule */
                        rule = cur;

                        /* skip rules without a static_node tag */
                        if (!rule->rule.has_static_node)
                                goto next;

                        uid = 0;
                        gid = 0;
                        mode = 0;
                        tags = strv_free(tags);
                        break;
                case TK_A_OWNER_ID:
                        uid = cur->key.uid;
                        break;
                case TK_A_GROUP_ID:
                        gid = cur->key.gid;
                        break;
                case TK_A_MODE_ID:
                        mode = cur->key.mode;
                        break;
                case TK_A_TAG:
                        r = strv_extend(&tags, rules_str(rules, cur->key.value_off));
                        if (r < 0)
                                goto finish;

                        break;
                case TK_A_STATIC_NODE: {
                        char device_node[UTIL_PATH_SIZE];
                        char tags_dir[UTIL_PATH_SIZE];
                        char tag_symlink[UTIL_PATH_SIZE];
                        struct stat stats;

                        /* we assure, that the permissions tokens are sorted before the static token */

                        if (mode == 0 && uid == 0 && gid == 0 && !tags)
                                goto next;

                        strscpyl(device_node, sizeof(device_node), "/dev/", rules_str(rules, cur->key.value_off), NULL);
                        if (stat(device_node, &stats) != 0)
                                break;
                        if (!S_ISBLK(stats.st_mode) && !S_ISCHR(stats.st_mode))
                                break;

                        /* export the tags to a directory as symlinks, allowing otherwise dead nodes to be tagged */
                        if (tags) {
                                STRV_FOREACH(t, tags) {
                                        _cleanup_free_ char *unescaped_filename = NULL;

                                        strscpyl(tags_dir, sizeof(tags_dir), "/run/udev/static_node-tags/", *t, "/", NULL);
                                        r = mkdir_p(tags_dir, 0755);
                                        if (r < 0)
                                                return log_error_errno(r, "Failed to create %s: %m", tags_dir);

                                        unescaped_filename = xescape(rules_str(rules, cur->key.value_off), "/.");

                                        strscpyl(tag_symlink, sizeof(tag_symlink), tags_dir, unescaped_filename, NULL);
                                        r = symlink(device_node, tag_symlink);
                                        if (r < 0 && errno != EEXIST)
                                                return log_error_errno(errno, "Failed to create symlink %s -> %s: %m",
                                                                       tag_symlink, device_node);
                                }
                        }

                        /* don't touch the permissions if only the tags were set */
                        if (mode == 0 && uid == 0 && gid == 0)
                                break;

                        if (mode == 0) {
                                if (gid > 0)
                                        mode = 0660;
                                else
                                        mode = 0600;
                        }
                        if (mode != (stats.st_mode & 01777)) {
                                r = chmod(device_node, mode);
                                if (r < 0)
                                        return log_error_errno(errno, "Failed to chmod '%s' %#o: %m",
                                                               device_node, mode);
                                else
                                        log_debug("chmod '%s' %#o", device_node, mode);
                        }

                        if ((uid != 0 && uid != stats.st_uid) || (gid != 0 && gid != stats.st_gid)) {
                                r = chown(device_node, uid, gid);
                                if (r < 0)
                                        return log_error_errno(errno, "Failed to chown '%s' %u %u: %m",
                                                               device_node, uid, gid);
                                else
                                        log_debug("chown '%s' %u %u", device_node, uid, gid);
                        }

                        utimensat(AT_FDCWD, device_node, NULL, 0);
                        break;
                }
                case TK_END:
                        goto finish;
                }

                cur++;
                continue;
next:
                /* fast-forward to next rule */
                cur = rule + rule->rule.token_count;
                continue;
        }

finish:
        if (f) {
                fflush(f);
                fchmod(fileno(f), 0644);
                if (ferror(f) || rename(path, "/run/udev/static_node-tags") < 0) {
                        unlink_noerrno("/run/udev/static_node-tags");
                        unlink_noerrno(path);
                        return -errno;
                }
        }

        return 0;
}
