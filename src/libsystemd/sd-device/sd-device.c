/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <ctype.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#include "sd-device.h"

#include "alloc-util.h"
#include "device-internal.h"
#include "device-private.h"
#include "device-util.h"
#include "dirent-util.h"
#include "fd-util.h"
#include "fileio.h"
#include "format-util.h"
#include "fs-util.h"
#include "hashmap.h"
#include "id128-util.h"
#include "macro.h"
#include "netlink-util.h"
#include "parse-util.h"
#include "path-util.h"
#include "set.h"
#include "socket-util.h"
#include "stat-util.h"
#include "stdio-util.h"
#include "string-util.h"
#include "strv.h"
#include "strxcpyx.h"
#include "user-util.h"
#include "util.h"

int device_new_aux(sd_device **ret) {
        sd_device *device;

        assert(ret);

        device = new(sd_device, 1);
        if (!device)
                return -ENOMEM;

        *device = (sd_device) {
                .n_ref = 1,
                .watch_handle = -1,
                .devmode = MODE_INVALID,
                .devuid = UID_INVALID,
                .devgid = GID_INVALID,
                .action = _SD_DEVICE_ACTION_INVALID,
        };

        *ret = device;
        return 0;
}

static sd_device *device_free(sd_device *device) {
        assert(device);

        sd_device_unref(device->parent);
        free(device->syspath);
        free(device->sysname);
        free(device->devtype);
        free(device->devname);
        free(device->subsystem);
        free(device->driver_subsystem);
        free(device->driver);
        free(device->device_id);
        free(device->properties_strv);
        free(device->properties_nulstr);

        ordered_hashmap_free(device->properties);
        ordered_hashmap_free(device->properties_db);
        hashmap_free(device->sysattr_values);
        set_free(device->sysattrs);
        set_free(device->all_tags);
        set_free(device->current_tags);
        set_free(device->devlinks);

        return mfree(device);
}

DEFINE_PUBLIC_TRIVIAL_REF_UNREF_FUNC(sd_device, sd_device, device_free);

int device_add_property_aux(sd_device *device, const char *key, const char *value, bool db) {
        OrderedHashmap **properties;

        assert(device);
        assert(key);

        if (db)
                properties = &device->properties_db;
        else
                properties = &device->properties;

        if (value) {
                _cleanup_free_ char *new_key = NULL, *new_value = NULL, *old_key = NULL, *old_value = NULL;
                int r;

                r = ordered_hashmap_ensure_allocated(properties, &string_hash_ops_free_free);
                if (r < 0)
                        return r;

                new_key = strdup(key);
                if (!new_key)
                        return -ENOMEM;

                new_value = strdup(value);
                if (!new_value)
                        return -ENOMEM;

                old_value = ordered_hashmap_get2(*properties, key, (void**) &old_key);

                /* ordered_hashmap_replace() does not fail when the hashmap already has the entry. */
                r = ordered_hashmap_replace(*properties, new_key, new_value);
                if (r < 0)
                        return r;

                TAKE_PTR(new_key);
                TAKE_PTR(new_value);
        } else {
                _cleanup_free_ char *old_key = NULL, *old_value = NULL;

                old_value = ordered_hashmap_remove2(*properties, key, (void**) &old_key);
        }

        if (!db) {
                device->properties_generation++;
                device->properties_buf_outdated = true;
        }

        return 0;
}

int device_set_syspath(sd_device *device, const char *_syspath, bool verify) {
        _cleanup_free_ char *syspath = NULL;
        const char *devpath;
        int r;

        assert(device);
        assert(_syspath);

        /* must be a subdirectory of /sys */
        if (!path_startswith(_syspath, "/sys/"))
                return log_debug_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "sd-device: Syspath '%s' is not a subdirectory of /sys",
                                       _syspath);

        if (verify) {
                r = chase_symlinks(_syspath, NULL, 0, &syspath, NULL);
                if (r == -ENOENT)
                        return -ENODEV; /* the device does not exist (any more?) */
                if (r < 0)
                        return log_debug_errno(r, "sd-device: Failed to get target of '%s': %m", _syspath);

                if (!path_startswith(syspath, "/sys")) {
                        _cleanup_free_ char *real_sys = NULL, *new_syspath = NULL;
                        char *p;

                        /* /sys is a symlink to somewhere sysfs is mounted on? In that case, we convert the path to real sysfs to "/sys". */
                        r = chase_symlinks("/sys", NULL, 0, &real_sys, NULL);
                        if (r < 0)
                                return log_debug_errno(r, "sd-device: Failed to chase symlink /sys: %m");

                        p = path_startswith(syspath, real_sys);
                        if (!p)
                                return log_debug_errno(SYNTHETIC_ERRNO(ENODEV),
                                                       "sd-device: Canonicalized path '%s' does not starts with sysfs mount point '%s'",
                                                       syspath, real_sys);

                        new_syspath = path_join("/sys", p);
                        if (!new_syspath)
                                return -ENOMEM;

                        free_and_replace(syspath, new_syspath);
                        path_simplify(syspath);
                }

                if (path_startswith(syspath,  "/sys/devices/")) {
                        char *path;

                        /* all 'devices' require an 'uevent' file */
                        path = strjoina(syspath, "/uevent");
                        if (access(path, F_OK) < 0) {
                                if (errno == ENOENT)
                                        /* this is not a valid device */
                                        return -ENODEV;

                                return log_debug_errno(errno, "sd-device: cannot access uevent file for %s: %m", syspath);
                        }
                } else {
                        /* everything else just needs to be a directory */
                        if (!is_dir(syspath, false))
                                return -ENODEV;
                }
        } else {
                syspath = strdup(_syspath);
                if (!syspath)
                        return -ENOMEM;
        }

        devpath = syspath + STRLEN("/sys");

        if (devpath[0] != '/')
                /* '/sys' alone is not a valid device path */
                return -ENODEV;

        r = device_add_property_internal(device, "DEVPATH", devpath);
        if (r < 0)
                return r;

        free_and_replace(device->syspath, syspath);
        device->devpath = devpath;
        return 0;
}

_public_ int sd_device_new_from_syspath(sd_device **ret, const char *syspath) {
        _cleanup_(sd_device_unrefp) sd_device *device = NULL;
        int r;

        assert_return(ret, -EINVAL);
        assert_return(syspath, -EINVAL);

        r = device_new_aux(&device);
        if (r < 0)
                return r;

        r = device_set_syspath(device, syspath, true);
        if (r < 0)
                return r;

        *ret = TAKE_PTR(device);
        return 0;
}

_public_ int sd_device_new_from_devnum(sd_device **ret, char type, dev_t devnum) {
        char id[DECIMAL_STR_MAX(unsigned) * 2 + 1], *syspath;

        assert_return(ret, -EINVAL);
        assert_return(IN_SET(type, 'b', 'c'), -EINVAL);

        /* use /sys/dev/{block,char}/<maj>:<min> link */
        xsprintf(id, "%u:%u", major(devnum), minor(devnum));

        syspath = strjoina("/sys/dev/", (type == 'b' ? "block" : "char"), "/", id);

        return sd_device_new_from_syspath(ret, syspath);
}

static int device_new_from_main_ifname(sd_device **ret, const char *ifname) {
        const char *syspath;

        assert(ret);
        assert(ifname);

        syspath = strjoina("/sys/class/net/", ifname);
        return sd_device_new_from_syspath(ret, syspath);
}

_public_ int sd_device_new_from_ifname(sd_device **ret, const char *ifname) {
        _cleanup_free_ char *main_name = NULL;
        int r;

        assert_return(ret, -EINVAL);
        assert_return(ifname, -EINVAL);

        r = parse_ifindex(ifname);
        if (r > 0)
                return sd_device_new_from_ifindex(ret, r);

        if (ifname_valid(ifname)) {
                r = device_new_from_main_ifname(ret, ifname);
                if (r >= 0)
                        return r;
        }

        r = rtnl_resolve_link_alternative_name(NULL, ifname, &main_name);
        if (r < 0)
                return r;

        return device_new_from_main_ifname(ret, main_name);
}

_public_ int sd_device_new_from_ifindex(sd_device **ret, int ifindex) {
        char ifname[IF_NAMESIZE + 1];

        assert_return(ret, -EINVAL);
        assert_return(ifindex > 0, -EINVAL);

        if (!format_ifname(ifindex, ifname))
                return -ENODEV;

        return device_new_from_main_ifname(ret, ifname);
}

static int device_strjoin_new(
                const char *a,
                const char *b,
                const char *c,
                const char *d,
                sd_device **ret) {

        const char *p;
        int r;

        p = strjoina(a, b, c, d);
        if (access(p, F_OK) < 0)
                return IN_SET(errno, ENOENT, ENAMETOOLONG) ? 0 : -errno; /* If this sysfs is too long then it doesn't exist either */

        r = sd_device_new_from_syspath(ret, p);
        if (r < 0)
                return r;

        return 1;
}

_public_ int sd_device_new_from_subsystem_sysname(
                sd_device **ret,
                const char *subsystem,
                const char *sysname) {

        const char *s;
        char *name;
        int r;

        assert_return(ret, -EINVAL);
        assert_return(path_is_normalized(subsystem), -EINVAL);
        assert_return(path_is_normalized(sysname), -EINVAL);

        if (streq(subsystem, "subsystem")) {

                FOREACH_STRING(s, "/sys/subsystem/", "/sys/bus/", "/sys/class/") {
                        r = device_strjoin_new(s, sysname, NULL, NULL, ret);
                        if (r < 0)
                                return r;
                        if (r > 0)
                                return 0;
                }

        } else  if (streq(subsystem, "module")) {

                r = device_strjoin_new("/sys/module/", sysname, NULL, NULL, ret);
                if (r < 0)
                        return r;
                if (r > 0)
                        return 0;

        } else if (streq(subsystem, "drivers")) {
                const char *sep;

                sep = strchr(sysname, ':');
                if (sep && sep[1] != '\0') { /* Require ":" and something non-empty after that. */
                        const char *subsys;

                        subsys = memdupa_suffix0(sysname, sep - sysname);
                        sep++;

                        FOREACH_STRING(s, "/sys/subsystem/", "/sys/bus/") {
                                r = device_strjoin_new(s, subsys, "/drivers/", sep, ret);
                                if (r < 0)
                                        return r;
                                if (r > 0)
                                        return 0;
                        }
                }
        }

        /* translate sysname back to sysfs filename */
        name = strdupa(sysname);
        for (size_t i = 0; name[i]; i++)
                if (name[i] == '/')
                        name[i] = '!';

        FOREACH_STRING(s, "/sys/subsystem/", "/sys/bus/") {
                r = device_strjoin_new(s, subsystem, "/devices/", name, ret);
                if (r < 0)
                        return r;
                if (r > 0)
                        return 0;
        }

        r = device_strjoin_new("/sys/class/", subsystem, "/", name, ret);
        if (r < 0)
                return r;
        if (r > 0)
                return 0;

        r = device_strjoin_new("/sys/firmware/", subsystem, "/", sysname, ret);
        if (r < 0)
                return r;
        if (r > 0)
                return 0;

        return -ENODEV;
}

_public_ int sd_device_new_from_stat_rdev(sd_device **ret, const struct stat *st) {
        char type;

        assert_return(ret, -EINVAL);
        assert_return(st, -EINVAL);

        if (S_ISBLK(st->st_mode))
                type = 'b';
        else if (S_ISCHR(st->st_mode))
                type = 'c';
        else
                return -ENOTTY;

        return sd_device_new_from_devnum(ret, type, st->st_rdev);
}

int device_set_devtype(sd_device *device, const char *devtype) {
        _cleanup_free_ char *t = NULL;
        int r;

        assert(device);
        assert(devtype);

        t = strdup(devtype);
        if (!t)
                return -ENOMEM;

        r = device_add_property_internal(device, "DEVTYPE", t);
        if (r < 0)
                return r;

        return free_and_replace(device->devtype, t);
}

int device_set_ifindex(sd_device *device, const char *name) {
        int r, ifindex;

        assert(device);
        assert(name);

        ifindex = parse_ifindex(name);
        if (ifindex < 0)
                return ifindex;

        r = device_add_property_internal(device, "IFINDEX", name);
        if (r < 0)
                return r;

        device->ifindex = ifindex;

        return 0;
}

int device_set_devname(sd_device *device, const char *devname) {
        _cleanup_free_ char *t = NULL;
        int r;

        assert(device);
        assert(devname);

        if (devname[0] != '/')
                t = strjoin("/dev/", devname);
        else
                t = strdup(devname);
        if (!t)
                return -ENOMEM;

        r = device_add_property_internal(device, "DEVNAME", t);
        if (r < 0)
                return r;

        return free_and_replace(device->devname, t);
}

int device_set_devmode(sd_device *device, const char *_devmode) {
        unsigned devmode;
        int r;

        assert(device);
        assert(_devmode);

        r = safe_atou(_devmode, &devmode);
        if (r < 0)
                return r;

        if (devmode > 07777)
                return -EINVAL;

        r = device_add_property_internal(device, "DEVMODE", _devmode);
        if (r < 0)
                return r;

        device->devmode = devmode;

        return 0;
}

int device_set_devnum(sd_device *device, const char *major, const char *minor) {
        unsigned maj, min = 0;
        int r;

        assert(device);
        assert(major);

        r = safe_atou(major, &maj);
        if (r < 0)
                return r;
        if (maj == 0)
                return 0;

        if (minor) {
                r = safe_atou(minor, &min);
                if (r < 0)
                        return r;
        }

        r = device_add_property_internal(device, "MAJOR", major);
        if (r < 0)
                return r;

        if (minor) {
                r = device_add_property_internal(device, "MINOR", minor);
                if (r < 0)
                        return r;
        }

        device->devnum = makedev(maj, min);

        return 0;
}

static int handle_uevent_line(sd_device *device, const char *key, const char *value, const char **major, const char **minor) {
        int r;

        assert(device);
        assert(key);
        assert(value);
        assert(major);
        assert(minor);

        if (streq(key, "DEVTYPE")) {
                r = device_set_devtype(device, value);
                if (r < 0)
                        return r;
        } else if (streq(key, "IFINDEX")) {
                r = device_set_ifindex(device, value);
                if (r < 0)
                        return r;
        } else if (streq(key, "DEVNAME")) {
                r = device_set_devname(device, value);
                if (r < 0)
                        return r;
        } else if (streq(key, "DEVMODE")) {
                r = device_set_devmode(device, value);
                if (r < 0)
                        return r;
        } else if (streq(key, "MAJOR"))
                *major = value;
        else if (streq(key, "MINOR"))
                *minor = value;
        else {
                r = device_add_property_internal(device, key, value);
                if (r < 0)
                        return r;
        }

        return 0;
}

int device_read_uevent_file(sd_device *device) {
        _cleanup_free_ char *uevent = NULL;
        const char *syspath, *key = NULL, *value = NULL, *major = NULL, *minor = NULL;
        char *path;
        size_t uevent_len;
        int r;

        enum {
                PRE_KEY,
                KEY,
                PRE_VALUE,
                VALUE,
                INVALID_LINE,
        } state = PRE_KEY;

        assert(device);

        if (device->uevent_loaded || device->sealed)
                return 0;

        r = sd_device_get_syspath(device, &syspath);
        if (r < 0)
                return r;

        device->uevent_loaded = true;

        path = strjoina(syspath, "/uevent");

        r = read_full_virtual_file(path, &uevent, &uevent_len);
        if (IN_SET(r, -EACCES, -ENOENT))
                /* The uevent files may be write-only, or the device may not have uevent file. */
                return 0;
        if (r < 0)
                return log_device_debug_errno(device, r, "sd-device: Failed to read uevent file '%s': %m", path);

        for (size_t i = 0; i < uevent_len; i++)
                switch (state) {
                case PRE_KEY:
                        if (!strchr(NEWLINE, uevent[i])) {
                                key = &uevent[i];

                                state = KEY;
                        }

                        break;
                case KEY:
                        if (uevent[i] == '=') {
                                uevent[i] = '\0';

                                state = PRE_VALUE;
                        } else if (strchr(NEWLINE, uevent[i])) {
                                uevent[i] = '\0';
                                log_device_debug(device, "sd-device: Invalid uevent line '%s', ignoring", key);

                                state = PRE_KEY;
                        }

                        break;
                case PRE_VALUE:
                        value = &uevent[i];
                        state = VALUE;

                        _fallthrough_; /* to handle empty property */
                case VALUE:
                        if (strchr(NEWLINE, uevent[i])) {
                                uevent[i] = '\0';

                                r = handle_uevent_line(device, key, value, &major, &minor);
                                if (r < 0)
                                        log_device_debug_errno(device, r, "sd-device: Failed to handle uevent entry '%s=%s', ignoring: %m", key, value);

                                state = PRE_KEY;
                        }

                        break;
                default:
                        assert_not_reached("Invalid state when parsing uevent file");
                }

        if (major) {
                r = device_set_devnum(device, major, minor);
                if (r < 0)
                        log_device_debug_errno(device, r, "sd-device: Failed to set 'MAJOR=%s' or 'MINOR=%s' from '%s', ignoring: %m", major, minor, path);
        }

        return 0;
}

_public_ int sd_device_get_ifindex(sd_device *device, int *ifindex) {
        int r;

        assert_return(device, -EINVAL);

        r = device_read_uevent_file(device);
        if (r < 0)
                return r;

        if (device->ifindex <= 0)
                return -ENOENT;

        if (ifindex)
                *ifindex = device->ifindex;

        return 0;
}

_public_ int sd_device_new_from_device_id(sd_device **ret, const char *id) {
        int r;

        assert_return(ret, -EINVAL);
        assert_return(id, -EINVAL);

        switch (id[0]) {
        case 'b':
        case 'c': {
                dev_t devt;

                if (isempty(id))
                        return -EINVAL;

                r = parse_dev(id + 1, &devt);
                if (r < 0)
                        return r;

                return sd_device_new_from_devnum(ret, id[0], devt);
        }

        case 'n': {
                int ifindex;

                ifindex = parse_ifindex(id + 1);
                if (ifindex < 0)
                        return ifindex;

                return sd_device_new_from_ifindex(ret, ifindex);
        }

        case '+': {
                const char *subsys, *sep;

                sep = strchr(id + 1, ':');
                if (!sep || sep - id - 1 > NAME_MAX)
                        return -EINVAL;

                subsys = memdupa_suffix0(id + 1, sep - id - 1);

                return sd_device_new_from_subsystem_sysname(ret, subsys, sep + 1);
        }

        default:
                return -EINVAL;
        }
}

_public_ int sd_device_get_syspath(sd_device *device, const char **ret) {
        assert_return(device, -EINVAL);

        assert(path_startswith(device->syspath, "/sys/"));

        if (ret)
                *ret = device->syspath;

        return 0;
}

static int device_new_from_child(sd_device **ret, sd_device *child) {
        _cleanup_free_ char *path = NULL;
        const char *subdir, *syspath;
        int r;

        assert(ret);
        assert(child);

        r = sd_device_get_syspath(child, &syspath);
        if (r < 0)
                return r;

        path = strdup(syspath);
        if (!path)
                return -ENOMEM;
        subdir = path + STRLEN("/sys");

        for (;;) {
                char *pos;

                pos = strrchr(subdir, '/');
                if (!pos || pos < subdir + 2)
                        return -ENODEV;

                *pos = '\0';

                r = sd_device_new_from_syspath(ret, path);
                if (r < 0)
                        continue;

                return 0;
        }
}

_public_ int sd_device_get_parent(sd_device *child, sd_device **ret) {
        assert_return(child, -EINVAL);

        if (!child->parent_set) {
                child->parent_set = true;

                (void) device_new_from_child(&child->parent, child);
        }

        if (!child->parent)
                return -ENOENT;

        if (ret)
                *ret = child->parent;
        return 0;
}

int device_set_subsystem(sd_device *device, const char *subsystem) {
        _cleanup_free_ char *s = NULL;
        int r;

        assert(device);

        if (subsystem) {
                s = strdup(subsystem);
                if (!s)
                        return -ENOMEM;
        }

        r = device_add_property_internal(device, "SUBSYSTEM", s);
        if (r < 0)
                return r;

        device->subsystem_set = true;
        return free_and_replace(device->subsystem, s);
}

int device_set_drivers_subsystem(sd_device *device) {
        _cleanup_free_ char *subsystem = NULL;
        const char *syspath, *drivers, *p;
        int r;

        assert(device);

        r = sd_device_get_syspath(device, &syspath);
        if (r < 0)
                return r;

        drivers = strstr(syspath, "/drivers/");
        if (!drivers)
                return -EINVAL;

        for (p = drivers - 1; p >= syspath; p--)
                if (*p == '/')
                        break;

        if (p <= syspath)
                /* syspath does not start with /sys/ ?? */
                return -EINVAL;
        p++;
        if (p >= drivers)
                /* refuse duplicated slashes */
                return -EINVAL;

        subsystem = strndup(p, drivers - p);
        if (!subsystem)
                return -ENOMEM;

        r = device_set_subsystem(device, "drivers");
        if (r < 0)
                return r;

        return free_and_replace(device->driver_subsystem, subsystem);
}

_public_ int sd_device_get_subsystem(sd_device *device, const char **ret) {
        int r;

        assert_return(device, -EINVAL);

        if (!device->subsystem_set) {
                _cleanup_free_ char *subsystem = NULL;
                const char *syspath;
                char *path;

                r = sd_device_get_syspath(device, &syspath);
                if (r < 0)
                        return r;

                /* read 'subsystem' link */
                path = strjoina(syspath, "/subsystem");
                r = readlink_value(path, &subsystem);
                if (r < 0 && r != -ENOENT)
                        return log_device_debug_errno(device, r,
                                                      "sd-device: Failed to read subsystem for %s: %m",
                                                      device->devpath);

                if (subsystem)
                        r = device_set_subsystem(device, subsystem);
                /* use implicit names */
                else if (path_startswith(device->devpath, "/module/"))
                        r = device_set_subsystem(device, "module");
                else if (strstr(syspath, "/drivers/"))
                        r = device_set_drivers_subsystem(device);
                else if (PATH_STARTSWITH_SET(device->devpath, "/subsystem/",
                                                              "/class/",
                                                              "/bus/"))
                        r = device_set_subsystem(device, "subsystem");
                else {
                        device->subsystem_set = true;
                        r = 0;
                }
                if (r < 0)
                        return log_device_debug_errno(device, r,
                                                      "sd-device: Failed to set subsystem for %s: %m",
                                                      device->devpath);
        }

        if (!device->subsystem)
                return -ENOENT;

        if (ret)
                *ret = device->subsystem;
        return 0;
}

_public_ int sd_device_get_devtype(sd_device *device, const char **devtype) {
        int r;

        assert_return(device, -EINVAL);

        r = device_read_uevent_file(device);
        if (r < 0)
                return r;

        if (!device->devtype)
                return -ENOENT;

        if (devtype)
                *devtype = device->devtype;

        return !!device->devtype;
}

_public_ int sd_device_get_parent_with_subsystem_devtype(sd_device *child, const char *subsystem, const char *devtype, sd_device **ret) {
        sd_device *parent = NULL;
        int r;

        assert_return(child, -EINVAL);
        assert_return(subsystem, -EINVAL);

        r = sd_device_get_parent(child, &parent);
        while (r >= 0) {
                const char *parent_subsystem = NULL;

                (void) sd_device_get_subsystem(parent, &parent_subsystem);
                if (streq_ptr(parent_subsystem, subsystem)) {
                        const char *parent_devtype = NULL;

                        if (!devtype)
                                break;

                        (void) sd_device_get_devtype(parent, &parent_devtype);
                        if (streq_ptr(parent_devtype, devtype))
                                break;
                }
                r = sd_device_get_parent(parent, &parent);
        }

        if (r < 0)
                return r;

        if (ret)
                *ret = parent;
        return 0;
}

_public_ int sd_device_get_devnum(sd_device *device, dev_t *devnum) {
        int r;

        assert_return(device, -EINVAL);

        r = device_read_uevent_file(device);
        if (r < 0)
                return r;

        if (major(device->devnum) <= 0)
                return -ENOENT;

        if (devnum)
                *devnum = device->devnum;

        return 0;
}

int device_set_driver(sd_device *device, const char *driver) {
        _cleanup_free_ char *d = NULL;
        int r;

        assert(device);

        if (driver) {
                d = strdup(driver);
                if (!d)
                        return -ENOMEM;
        }

        r = device_add_property_internal(device, "DRIVER", d);
        if (r < 0)
                return r;

        device->driver_set = true;
        return free_and_replace(device->driver, d);
}

_public_ int sd_device_get_driver(sd_device *device, const char **ret) {
        assert_return(device, -EINVAL);

        if (!device->driver_set) {
                _cleanup_free_ char *driver = NULL;
                const char *syspath;
                char *path;
                int r;

                r = sd_device_get_syspath(device, &syspath);
                if (r < 0)
                        return r;

                path = strjoina(syspath, "/driver");
                r = readlink_value(path, &driver);
                if (r < 0 && r != -ENOENT)
                        return log_device_debug_errno(device, r,
                                                      "sd-device: readlink(\"%s\") failed: %m", path);

                r = device_set_driver(device, driver);
                if (r < 0)
                        return log_device_debug_errno(device, r,
                                                      "sd-device: Failed to set driver \"%s\": %m", driver);
        }

        if (!device->driver)
                return -ENOENT;

        if (ret)
                *ret = device->driver;
        return 0;
}

_public_ int sd_device_get_devpath(sd_device *device, const char **devpath) {
        assert_return(device, -EINVAL);

        assert(device->devpath);
        assert(device->devpath[0] == '/');

        if (devpath)
                *devpath = device->devpath;
        return 0;
}

_public_ int sd_device_get_devname(sd_device *device, const char **devname) {
        int r;

        assert_return(device, -EINVAL);

        r = device_read_uevent_file(device);
        if (r < 0)
                return r;

        if (!device->devname)
                return -ENOENT;

        assert(path_startswith(device->devname, "/dev/"));

        if (devname)
                *devname = device->devname;
        return 0;
}

static int device_set_sysname_and_sysnum(sd_device *device) {
        _cleanup_free_ char *sysname = NULL;
        const char *sysnum = NULL;
        const char *pos;
        size_t len = 0;

        if (!device->devpath)
                return -EINVAL;

        pos = strrchr(device->devpath, '/');
        if (!pos)
                return -EINVAL;
        pos++;

        /* devpath is not a root directory */
        if (*pos == '\0' || pos <= device->devpath)
                return -EINVAL;

        sysname = strdup(pos);
        if (!sysname)
                return -ENOMEM;

        /* some devices have '!' in their name, change that to '/' */
        while (sysname[len] != '\0') {
                if (sysname[len] == '!')
                        sysname[len] = '/';

                len++;
        }

        /* trailing number */
        while (len > 0 && isdigit(sysname[--len]))
                sysnum = &sysname[len];

        if (len == 0)
                sysnum = NULL;

        device->sysnum = sysnum;
        return free_and_replace(device->sysname, sysname);
}

_public_ int sd_device_get_sysname(sd_device *device, const char **ret) {
        int r;

        assert_return(device, -EINVAL);

        if (!device->sysname) {
                r = device_set_sysname_and_sysnum(device);
                if (r < 0)
                        return r;
        }

        if (ret)
                *ret = device->sysname;
        return 0;
}

_public_ int sd_device_get_sysnum(sd_device *device, const char **ret) {
        int r;

        assert_return(device, -EINVAL);

        if (!device->sysname) {
                r = device_set_sysname_and_sysnum(device);
                if (r < 0)
                        return r;
        }

        if (!device->sysnum)
                return -ENOENT;

        if (ret)
                *ret = device->sysnum;
        return 0;
}

_public_ int sd_device_get_action(sd_device *device, sd_device_action_t *ret) {
        assert_return(device, -EINVAL);

        if (device->action < 0)
                return -ENOENT;

        if (ret)
                *ret = device->action;

        return 0;
}

_public_ int sd_device_get_seqnum(sd_device *device, uint64_t *ret) {
        assert_return(device, -EINVAL);

        if (device->seqnum == 0)
                return -ENOENT;

        if (ret)
                *ret = device->seqnum;

        return 0;
}

static bool is_valid_tag(const char *tag) {
        assert(tag);

        return !strchr(tag, ':') && !strchr(tag, ' ');
}

int device_add_tag(sd_device *device, const char *tag, bool both) {
        int r, added;

        assert(device);
        assert(tag);

        if (!is_valid_tag(tag))
                return -EINVAL;

        /* Definitely add to the "all" list of tags (i.e. the sticky list) */
        added = set_put_strdup(&device->all_tags, tag);
        if (added < 0)
                return added;

        /* And optionally, also add it to the current list of tags */
        if (both) {
                r = set_put_strdup(&device->current_tags, tag);
                if (r < 0) {
                        if (added > 0)
                                (void) set_remove(device->all_tags, tag);

                        return r;
                }
        }

        device->tags_generation++;
        device->property_tags_outdated = true;

        return 0;
}

int device_add_devlink(sd_device *device, const char *devlink) {
        int r;

        assert(device);
        assert(devlink);

        r = set_put_strdup(&device->devlinks, devlink);
        if (r < 0)
                return r;

        device->devlinks_generation++;
        device->property_devlinks_outdated = true;

        return 0;
}

static int device_add_property_internal_from_string(sd_device *device, const char *str) {
        _cleanup_free_ char *key = NULL;
        char *value;
        int r;

        assert(device);
        assert(str);

        key = strdup(str);
        if (!key)
                return -ENOMEM;

        value = strchr(key, '=');
        if (!value)
                return -EINVAL;

        *value = '\0';

        if (isempty(++value))
                value = NULL;

        /* Add the property to both sd_device::properties and sd_device::properties_db,
         * as this is called by only handle_db_line(). */
        r = device_add_property_aux(device, key, value, false);
        if (r < 0)
                return r;

        return device_add_property_aux(device, key, value, true);
}

int device_set_usec_initialized(sd_device *device, usec_t when) {
        char s[DECIMAL_STR_MAX(usec_t)];
        int r;

        assert(device);

        xsprintf(s, USEC_FMT, when);

        r = device_add_property_internal(device, "USEC_INITIALIZED", s);
        if (r < 0)
                return r;

        device->usec_initialized = when;
        return 0;
}

static int handle_db_line(sd_device *device, char key, const char *value) {
        char *path;
        int r;

        assert(device);
        assert(value);

        switch (key) {
        case 'G': /* Any tag */
        case 'Q': /* Current tag */
                r = device_add_tag(device, value, key == 'Q');
                if (r < 0)
                        return r;

                break;
        case 'S':
                path = strjoina("/dev/", value);
                r = device_add_devlink(device, path);
                if (r < 0)
                        return r;

                break;
        case 'E':
                r = device_add_property_internal_from_string(device, value);
                if (r < 0)
                        return r;

                break;
        case 'I': {
                usec_t t;

                r = safe_atou64(value, &t);
                if (r < 0)
                        return r;

                r = device_set_usec_initialized(device, t);
                if (r < 0)
                        return r;

                break;
        }
        case 'L':
                r = safe_atoi(value, &device->devlink_priority);
                if (r < 0)
                        return r;

                break;
        case 'W':
                /* Deprecated. Previously, watch handle is both saved in database and /run/udev/watch.
                 * However, the handle saved in database may not be updated when the handle is updated
                 * or removed. Moreover, it is not necessary to store the handle within the database,
                 * as its value becomes meaningless when udevd is restarted. */
                break;
        case 'V':
                r = safe_atou(value, &device->database_version);
                if (r < 0)
                        return r;

                break;
        default:
                log_device_debug(device, "sd-device: Unknown key '%c' in device db, ignoring", key);
        }

        return 0;
}

int device_get_device_id(sd_device *device, const char **ret) {
        assert(device);
        assert(ret);

        if (!device->device_id) {
                _cleanup_free_ char *id = NULL;
                const char *subsystem;
                dev_t devnum;
                int ifindex, r;

                r = sd_device_get_subsystem(device, &subsystem);
                if (r < 0)
                        return r;

                if (sd_device_get_devnum(device, &devnum) >= 0) {
                        assert(subsystem);

                        /* use dev_t — b259:131072, c254:0 */
                        r = asprintf(&id, "%c%u:%u",
                                     streq(subsystem, "block") ? 'b' : 'c',
                                     major(devnum), minor(devnum));
                        if (r < 0)
                                return -ENOMEM;
                } else if (sd_device_get_ifindex(device, &ifindex) >= 0) {
                        /* use netdev ifindex — n3 */
                        r = asprintf(&id, "n%u", (unsigned) ifindex);
                        if (r < 0)
                                return -ENOMEM;
                } else {
                        /* use $subsys:$sysname — pci:0000:00:1f.2
                         * sysname() has '!' translated, get it from devpath
                         */
                        const char *sysname;

                        sysname = basename(device->devpath);
                        if (!sysname)
                                return -EINVAL;

                        if (!subsystem)
                                return -EINVAL;

                        if (streq(subsystem, "drivers"))
                                /* the 'drivers' pseudo-subsystem is special, and needs the real subsystem
                                 * encoded as well */
                                id = strjoin("+drivers:", device->driver_subsystem, ":", sysname);
                        else
                                id = strjoin("+", subsystem, ":", sysname);
                        if (!id)
                                return -ENOMEM;
                }

                if (!filename_is_valid(id))
                        return -EINVAL;

                device->device_id = TAKE_PTR(id);
        }

        *ret = device->device_id;
        return 0;
}

int device_read_db_internal_filename(sd_device *device, const char *filename) {
        _cleanup_free_ char *db = NULL;
        const char *value;
        size_t db_len;
        char key = '\0';  /* Unnecessary initialization to appease gcc-12.0.0-0.4.fc36 */
        int r;

        enum {
                PRE_KEY,
                KEY,
                PRE_VALUE,
                VALUE,
                INVALID_LINE,
        } state = PRE_KEY;

        assert(device);
        assert(filename);

        r = read_full_file(filename, &db, &db_len);
        if (r < 0) {
                if (r == -ENOENT)
                        return 0;

                return log_device_debug_errno(device, r, "sd-device: Failed to read db '%s': %m", filename);
        }

        /* devices with a database entry are initialized */
        device->is_initialized = true;

        device->db_loaded = true;

        for (size_t i = 0; i < db_len; i++) {
                switch (state) {
                case PRE_KEY:
                        if (!strchr(NEWLINE, db[i])) {
                                key = db[i];

                                state = KEY;
                        }

                        break;
                case KEY:
                        if (db[i] != ':') {
                                log_device_debug(device, "sd-device: Invalid db entry with key '%c', ignoring", key);

                                state = INVALID_LINE;
                        } else {
                                db[i] = '\0';

                                state = PRE_VALUE;
                        }

                        break;
                case PRE_VALUE:
                        value = &db[i];

                        state = VALUE;

                        break;
                case INVALID_LINE:
                        if (strchr(NEWLINE, db[i]))
                                state = PRE_KEY;

                        break;
                case VALUE:
                        if (strchr(NEWLINE, db[i])) {
                                db[i] = '\0';
                                r = handle_db_line(device, key, value);
                                if (r < 0)
                                        log_device_debug_errno(device, r, "sd-device: Failed to handle db entry '%c:%s', ignoring: %m",
                                                               key, value);

                                state = PRE_KEY;
                        }

                        break;
                default:
                        return log_device_debug_errno(device, SYNTHETIC_ERRNO(EINVAL), "sd-device: invalid db syntax.");
                }
        }

        return 0;
}

int device_read_db_internal(sd_device *device, bool force) {
        const char *id, *path;
        int r;

        assert(device);

        if (device->db_loaded || (!force && device->sealed))
                return 0;

        r = device_get_device_id(device, &id);
        if (r < 0)
                return r;

        path = strjoina("/run/udev/data/", id);

        return device_read_db_internal_filename(device, path);
}

_public_ int sd_device_get_is_initialized(sd_device *device) {
        int r;

        assert_return(device, -EINVAL);

        r = device_read_db(device);
        if (r < 0)
                return r;

        return device->is_initialized;
}

_public_ int sd_device_get_usec_initialized(sd_device *device, uint64_t *ret) {
        int r;

        assert_return(device, -EINVAL);

        r = device_read_db(device);
        if (r < 0)
                return r;

        if (!device->is_initialized)
                return -EBUSY;

        if (device->usec_initialized == 0)
                return -ENODATA;

        if (ret)
                *ret = device->usec_initialized;

        return 0;
}

_public_ int sd_device_get_usec_since_initialized(sd_device *device, uint64_t *usec) {
        usec_t now_ts;
        int r;

        assert_return(device, -EINVAL);

        r = device_read_db(device);
        if (r < 0)
                return r;

        if (!device->is_initialized)
                return -EBUSY;

        if (device->usec_initialized == 0)
                return -ENODATA;

        now_ts = now(CLOCK_MONOTONIC);

        if (now_ts < device->usec_initialized)
                return -EIO;

        if (usec)
                *usec = now_ts - device->usec_initialized;
        return 0;
}

_public_ const char *sd_device_get_tag_first(sd_device *device) {
        void *v;

        assert_return(device, NULL);

        (void) device_read_db(device);

        device->all_tags_iterator_generation = device->tags_generation;
        device->all_tags_iterator = ITERATOR_FIRST;

        (void) set_iterate(device->all_tags, &device->all_tags_iterator, &v);
        return v;
}

_public_ const char *sd_device_get_tag_next(sd_device *device) {
        void *v;

        assert_return(device, NULL);

        (void) device_read_db(device);

        if (device->all_tags_iterator_generation != device->tags_generation)
                return NULL;

        (void) set_iterate(device->all_tags, &device->all_tags_iterator, &v);
        return v;
}

static bool device_database_supports_current_tags(sd_device *device) {
        assert(device);

        (void) device_read_db(device);

        /* The current tags (saved in Q field) feature is implemented in database version 1.
         * If the database version is 0, then the tags (NOT current tags, saved in G field) are not
         * sticky. Thus, we can safely bypass the operations for the current tags (Q) to tags (G). */

        return device->database_version >= 1;
}

_public_ const char *sd_device_get_current_tag_first(sd_device *device) {
        void *v;

        assert_return(device, NULL);

        if (!device_database_supports_current_tags(device))
                return sd_device_get_tag_first(device);

        (void) device_read_db(device);

        device->current_tags_iterator_generation = device->tags_generation;
        device->current_tags_iterator = ITERATOR_FIRST;

        (void) set_iterate(device->current_tags, &device->current_tags_iterator, &v);
        return v;
}

_public_ const char *sd_device_get_current_tag_next(sd_device *device) {
        void *v;

        assert_return(device, NULL);

        if (!device_database_supports_current_tags(device))
                return sd_device_get_tag_next(device);

        (void) device_read_db(device);

        if (device->current_tags_iterator_generation != device->tags_generation)
                return NULL;

        (void) set_iterate(device->current_tags, &device->current_tags_iterator, &v);
        return v;
}

_public_ const char *sd_device_get_devlink_first(sd_device *device) {
        void *v;

        assert_return(device, NULL);

        (void) device_read_db(device);

        device->devlinks_iterator_generation = device->devlinks_generation;
        device->devlinks_iterator = ITERATOR_FIRST;

        (void) set_iterate(device->devlinks, &device->devlinks_iterator, &v);
        return v;
}

_public_ const char *sd_device_get_devlink_next(sd_device *device) {
        void *v;

        assert_return(device, NULL);

        (void) device_read_db(device);

        if (device->devlinks_iterator_generation != device->devlinks_generation)
                return NULL;

        (void) set_iterate(device->devlinks, &device->devlinks_iterator, &v);
        return v;
}

int device_properties_prepare(sd_device *device) {
        int r;

        assert(device);

        r = device_read_uevent_file(device);
        if (r < 0)
                return r;

        r = device_read_db(device);
        if (r < 0)
                return r;

        if (device->property_devlinks_outdated) {
                _cleanup_free_ char *devlinks = NULL;

                r = set_strjoin(device->devlinks, " ", false, &devlinks);
                if (r < 0)
                        return r;

                if (!isempty(devlinks)) {
                        r = device_add_property_internal(device, "DEVLINKS", devlinks);
                        if (r < 0)
                                return r;
                }

                device->property_devlinks_outdated = false;
        }

        if (device->property_tags_outdated) {
                _cleanup_free_ char *tags = NULL;

                r = set_strjoin(device->all_tags, ":", true, &tags);
                if (r < 0)
                        return r;

                if (!isempty(tags)) {
                        r = device_add_property_internal(device, "TAGS", tags);
                        if (r < 0)
                                return r;
                }

                tags = mfree(tags);
                r = set_strjoin(device->current_tags, ":", true, &tags);
                if (r < 0)
                        return r;

                if (!isempty(tags)) {
                        r = device_add_property_internal(device, "CURRENT_TAGS", tags);
                        if (r < 0)
                                return r;
                }

                device->property_tags_outdated = false;
        }

        return 0;
}

_public_ const char *sd_device_get_property_first(sd_device *device, const char **_value) {
        const char *key;
        int r;

        assert_return(device, NULL);

        r = device_properties_prepare(device);
        if (r < 0)
                return NULL;

        device->properties_iterator_generation = device->properties_generation;
        device->properties_iterator = ITERATOR_FIRST;

        (void) ordered_hashmap_iterate(device->properties, &device->properties_iterator, (void**)_value, (const void**)&key);
        return key;
}

_public_ const char *sd_device_get_property_next(sd_device *device, const char **_value) {
        const char *key;
        int r;

        assert_return(device, NULL);

        r = device_properties_prepare(device);
        if (r < 0)
                return NULL;

        if (device->properties_iterator_generation != device->properties_generation)
                return NULL;

        (void) ordered_hashmap_iterate(device->properties, &device->properties_iterator, (void**)_value, (const void**)&key);
        return key;
}

static int device_sysattrs_read_all_internal(sd_device *device, const char *subdir) {
        _cleanup_free_ char *path_dir = NULL;
        _cleanup_closedir_ DIR *dir = NULL;
        struct dirent *dent;
        const char *syspath;
        int r;

        r = sd_device_get_syspath(device, &syspath);
        if (r < 0)
                return r;

        if (subdir) {
                _cleanup_free_ char *p = NULL;

                p = path_join(syspath, subdir, "uevent");
                if (!p)
                        return -ENOMEM;

                if (access(p, F_OK) >= 0)
                        /* this is a child device, skipping */
                        return 0;
                if (errno != ENOENT) {
                        log_device_debug_errno(device, errno, "sd-device: Failed to stat %s, ignoring subdir: %m", p);
                        return 0;
                }

                path_dir = path_join(syspath, subdir);
                if (!path_dir)
                        return -ENOMEM;
        }

        dir = opendir(path_dir ?: syspath);
        if (!dir)
                return -errno;

        FOREACH_DIRENT_ALL(dent, dir, return -errno) {
                _cleanup_free_ char *path = NULL, *p = NULL;
                struct stat statbuf;

                if (dot_or_dot_dot(dent->d_name))
                        continue;

                /* only handle symlinks, regular files, and directories */
                if (!IN_SET(dent->d_type, DT_LNK, DT_REG, DT_DIR))
                        continue;

                if (subdir) {
                        p = path_join(subdir, dent->d_name);
                        if (!p)
                                return -ENOMEM;
                }

                if (dent->d_type == DT_DIR) {
                        /* read subdirectory */
                        r = device_sysattrs_read_all_internal(device, p ?: dent->d_name);
                        if (r < 0)
                                return r;

                        continue;
                }

                path = path_join(syspath, p ?: dent->d_name);
                if (!path)
                        return -ENOMEM;

                if (lstat(path, &statbuf) != 0)
                        continue;

                if (!(statbuf.st_mode & S_IRUSR))
                        continue;

                r = set_put_strdup(&device->sysattrs, p ?: dent->d_name);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int device_sysattrs_read_all(sd_device *device) {
        int r;

        assert(device);

        if (device->sysattrs_read)
                return 0;

        r = device_sysattrs_read_all_internal(device, NULL);
        if (r < 0)
                return r;

        device->sysattrs_read = true;

        return 0;
}

_public_ const char *sd_device_get_sysattr_first(sd_device *device) {
        void *v;
        int r;

        assert_return(device, NULL);

        if (!device->sysattrs_read) {
                r = device_sysattrs_read_all(device);
                if (r < 0) {
                        errno = -r;
                        return NULL;
                }
        }

        device->sysattrs_iterator = ITERATOR_FIRST;

        (void) set_iterate(device->sysattrs, &device->sysattrs_iterator, &v);
        return v;
}

_public_ const char *sd_device_get_sysattr_next(sd_device *device) {
        void *v;

        assert_return(device, NULL);

        if (!device->sysattrs_read)
                return NULL;

        (void) set_iterate(device->sysattrs, &device->sysattrs_iterator, &v);
        return v;
}

_public_ int sd_device_has_tag(sd_device *device, const char *tag) {
        assert_return(device, -EINVAL);
        assert_return(tag, -EINVAL);

        (void) device_read_db(device);

        return set_contains(device->all_tags, tag);
}

_public_ int sd_device_has_current_tag(sd_device *device, const char *tag) {
        assert_return(device, -EINVAL);
        assert_return(tag, -EINVAL);

        if (!device_database_supports_current_tags(device))
                return sd_device_has_tag(device, tag);

        (void) device_read_db(device);

        return set_contains(device->current_tags, tag);
}

_public_ int sd_device_get_property_value(sd_device *device, const char *key, const char **ret_value) {
        const char *value;
        int r;

        assert_return(device, -EINVAL);
        assert_return(key, -EINVAL);

        r = device_properties_prepare(device);
        if (r < 0)
                return r;

        value = ordered_hashmap_get(device->properties, key);
        if (!value)
                return -ENOENT;

        if (ret_value)
                *ret_value = value;
        return 0;
}

_public_ int sd_device_get_trigger_uuid(sd_device *device, sd_id128_t *ret) {
        const char *s;
        sd_id128_t id;
        int r;

        assert_return(device, -EINVAL);

        /* Retrieves the UUID attached to a uevent when triggering it from userspace via
         * sd_device_trigger_with_uuid() or an equivalent interface. Returns -ENOENT if the record is not
         * caused by a synthetic event and -ENODATA if it was but no UUID was specified */

        r = sd_device_get_property_value(device, "SYNTH_UUID", &s);
        if (r < 0)
                return r;

        if (streq(s, "0")) /* SYNTH_UUID=0 is set whenever a device is triggered by userspace without specifying a UUID */
                return -ENODATA;

        r = sd_id128_from_string(s, &id);
        if (r < 0)
                return r;

        if (ret)
                *ret = id;

        return 0;
}

static int device_cache_sysattr_value(sd_device *device, const char *key, char *value) {
        _cleanup_free_ char *new_key = NULL, *old_value = NULL;
        int r;

        assert(device);
        assert(key);

        /* This takes the reference of the input value. The input value may be NULL.
         * This replaces the value if it already exists. */

        /* First, remove the old cache entry. So, we do not need to clear cache on error. */
        old_value = hashmap_remove2(device->sysattr_values, key, (void **) &new_key);
        if (!new_key) {
                new_key = strdup(key);
                if (!new_key)
                        return -ENOMEM;
        }

        r = hashmap_ensure_put(&device->sysattr_values, &string_hash_ops_free_free, new_key, value);
        if (r < 0)
                return r;

        TAKE_PTR(new_key);

        return 0;
}

static int device_get_cached_sysattr_value(sd_device *device, const char *_key, const char **_value) {
        const char *key = NULL, *value;

        assert(device);
        assert(_key);

        value = hashmap_get2(device->sysattr_values, _key, (void **) &key);
        if (!key)
                return -ENOENT;

        if (_value)
                *_value = value;
        return 0;
}

/* We cache all sysattr lookups. If an attribute does not exist, it is stored
 * with a NULL value in the cache, otherwise the returned string is stored */
_public_ int sd_device_get_sysattr_value(sd_device *device, const char *sysattr, const char **ret_value) {
        _cleanup_free_ char *value = NULL;
        const char *path, *syspath, *cached_value = NULL;
        struct stat statbuf;
        int r;

        assert_return(device, -EINVAL);
        assert_return(sysattr, -EINVAL);

        /* look for possibly already cached result */
        r = device_get_cached_sysattr_value(device, sysattr, &cached_value);
        if (r != -ENOENT) {
                if (r < 0)
                        return r;

                if (!cached_value)
                        /* we looked up the sysattr before and it did not exist */
                        return -ENOENT;

                if (ret_value)
                        *ret_value = cached_value;

                return 0;
        }

        r = sd_device_get_syspath(device, &syspath);
        if (r < 0)
                return r;

        path = prefix_roota(syspath, sysattr);
        if (lstat(path, &statbuf) < 0) {
                int k;

                r = -errno;

                /* remember that we could not access the sysattr */
                k = device_cache_sysattr_value(device, sysattr, NULL);
                if (k < 0)
                        log_device_debug_errno(device, k,
                                               "sd-device: failed to cache attribute '%s' with NULL, ignoring: %m",
                                               sysattr);

                return r;
        } else if (S_ISLNK(statbuf.st_mode)) {
                /* Some core links return only the last element of the target path,
                 * these are just values, the paths should not be exposed. */
                if (STR_IN_SET(sysattr, "driver", "subsystem", "module")) {
                        r = readlink_value(path, &value);
                        if (r < 0)
                                return r;
                } else
                        return -EINVAL;
        } else if (S_ISDIR(statbuf.st_mode))
                /* skip directories */
                return -EISDIR;
        else if (!(statbuf.st_mode & S_IRUSR))
                /* skip non-readable files */
                return -EPERM;
        else {
                size_t size;

                /* Read attribute value, Some attributes contain embedded '\0'. So, it is necessary to
                 * also get the size of the result. See issue #20025. */
                r = read_full_virtual_file(path, &value, &size);
                if (r < 0)
                        return r;

                /* drop trailing newlines */
                while (size > 0 && strchr(NEWLINE, value[--size]))
                        value[size] = '\0';
        }

        /* Unfortunately, we need to return 'const char*' instead of 'char*'. Hence, failure in caching
         * sysattr value is critical unlike the other places. */
        r = device_cache_sysattr_value(device, sysattr, value);
        if (r < 0) {
                log_device_debug_errno(device, r,
                                       "sd-device: failed to cache attribute '%s' with '%s'%s: %m",
                                       sysattr, value, ret_value ? "" : ", ignoring");
                if (ret_value)
                        return r;
        } else if (ret_value)
                *ret_value = TAKE_PTR(value);

        return 0;
}

static void device_remove_cached_sysattr_value(sd_device *device, const char *_key) {
        _cleanup_free_ char *key = NULL;

        assert(device);
        assert(_key);

        free(hashmap_remove2(device->sysattr_values, _key, (void **) &key));
}

_public_ int sd_device_set_sysattr_value(sd_device *device, const char *sysattr, const char *_value) {
        _cleanup_free_ char *value = NULL;
        const char *syspath, *path;
        size_t len;
        int r;

        assert_return(device, -EINVAL);
        assert_return(sysattr, -EINVAL);

        /* Set the attribute and save it in the cache. */

        if (!_value) {
                /* If input value is NULL, then clear cache and not write anything. */
                device_remove_cached_sysattr_value(device, sysattr);
                return 0;
        }

        r = sd_device_get_syspath(device, &syspath);
        if (r < 0)
                return r;

        path = prefix_roota(syspath, sysattr);

        len = strlen(_value);

        /* drop trailing newlines */
        while (len > 0 && strchr(NEWLINE, _value[len - 1]))
                len --;

        /* value length is limited to 4k */
        if (len > 4096)
                return -EINVAL;

        value = strndup(_value, len);
        if (!value)
                return -ENOMEM;

        r = write_string_file(path, value, WRITE_STRING_FILE_DISABLE_BUFFER | WRITE_STRING_FILE_NOFOLLOW);
        if (r < 0) {
                /* On failure, clear cache entry, as we do not know how it fails. */
                device_remove_cached_sysattr_value(device, sysattr);
                return r;
        }

        /* Do not cache action string written into uevent file. */
        if (streq(sysattr, "uevent"))
                return 0;

        r = device_cache_sysattr_value(device, sysattr, value);
        if (r < 0)
                log_device_debug_errno(device, r,
                                       "sd-device: failed to cache attribute '%s' with '%s', ignoring: %m",
                                       sysattr, value);
        else
                TAKE_PTR(value);

        return 0;
}

_public_ int sd_device_set_sysattr_valuef(sd_device *device, const char *sysattr, const char *format, ...) {
        _cleanup_free_ char *value = NULL;
        va_list ap;
        int r;

        assert_return(device, -EINVAL);
        assert_return(sysattr, -EINVAL);

        if (!format) {
                device_remove_cached_sysattr_value(device, sysattr);
                return 0;
        }

        va_start(ap, format);
        r = vasprintf(&value, format, ap);
        va_end(ap);

        if (r < 0)
                return -ENOMEM;

        return sd_device_set_sysattr_value(device, sysattr, value);
}

_public_ int sd_device_trigger(sd_device *device, sd_device_action_t action) {
        const char *s;

        assert_return(device, -EINVAL);

        s = device_action_to_string(action);
        if (!s)
                return -EINVAL;

        /* This uses the simple no-UUID interface of kernel < 4.13 */
        return sd_device_set_sysattr_value(device, "uevent", s);
}

_public_ int sd_device_trigger_with_uuid(
                sd_device *device,
                sd_device_action_t action,
                sd_id128_t *ret_uuid) {

        char buf[ID128_UUID_STRING_MAX];
        const char *s, *j;
        sd_id128_t u;
        int r;

        assert_return(device, -EINVAL);

        /* If no one wants to know the UUID, use the simple interface from pre-4.13 times */
        if (!ret_uuid)
                return sd_device_trigger(device, action);

        s = device_action_to_string(action);
        if (!s)
                return -EINVAL;

        r = sd_id128_randomize(&u);
        if (r < 0)
                return r;

        id128_to_uuid_string(u, buf);
        j = strjoina(s, " ", buf);

        r = sd_device_set_sysattr_value(device, "uevent", j);
        if (r < 0)
                return r;

        *ret_uuid = u;
        return 0;
}
