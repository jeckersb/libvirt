/*
 * Copyright (C) 2010-2012 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * POSIX DAC security driver
 */

#include <config.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "security_dac.h"
#include "virterror_internal.h"
#include "util.h"
#include "memory.h"
#include "logging.h"
#include "pci.h"
#include "hostusb.h"
#include "storage_file.h"

#define VIR_FROM_THIS VIR_FROM_SECURITY
#define SECURITY_DAC_NAME "dac"

typedef struct _virSecurityDACData virSecurityDACData;
typedef virSecurityDACData *virSecurityDACDataPtr;

struct _virSecurityDACData {
    uid_t user;
    gid_t group;
    bool dynamicOwnership;
};

void virSecurityDACSetUser(virSecurityManagerPtr mgr,
                           uid_t user)
{
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);
    priv->user = user;
}

void virSecurityDACSetGroup(virSecurityManagerPtr mgr,
                            gid_t group)
{
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);
    priv->group = group;
}

void virSecurityDACSetDynamicOwnership(virSecurityManagerPtr mgr,
                                       bool dynamicOwnership)
{
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);
    priv->dynamicOwnership = dynamicOwnership;
}

static
int parseIds(const char *label, uid_t *uidPtr, gid_t *gidPtr)
{
    int rc = -1;
    uid_t theuid;
    gid_t thegid;
    char *tmp_label = NULL;
    char *sep = NULL;
    char *owner = NULL;
    char *group = NULL;

    tmp_label = strdup(label);
    if (tmp_label == NULL) {
        virReportOOMError();
        goto cleanup;
    }

    /* Split label */
    sep = strchr(tmp_label, ':');
    if (sep == NULL) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("Missing separator ':' in DAC label \"%s\""),
                       label);
        goto cleanup;
    }
    *sep = '\0';
    owner = tmp_label;
    group = sep + 1;

    /* Parse owner and group, error message is defined by
     * virGetUserID or virGetGroupID.
     */
    if (virGetUserID(owner, &theuid) < 0 ||
        virGetGroupID(group, &thegid) < 0)
        goto cleanup;

    if (uidPtr)
        *uidPtr = theuid;
    if (gidPtr)
        *gidPtr = thegid;

    rc = 0;

cleanup:
    VIR_FREE(tmp_label);

    return rc;
}

/* returns 1 if label isn't found, 0 on success, -1 on error */
static
int virSecurityDACParseIds(virDomainDefPtr def, uid_t *uidPtr, gid_t *gidPtr)
{
    uid_t uid;
    gid_t gid;
    virSecurityLabelDefPtr seclabel;

    if (def == NULL)
        return 1;

    seclabel = virDomainDefGetSecurityLabelDef(def, SECURITY_DAC_NAME);
    if (seclabel == NULL || seclabel->label == NULL) {
        VIR_DEBUG("DAC seclabel for domain '%s' wasn't found", def->name);
        return 1;
    }

    if (parseIds(seclabel->label, &uid, &gid) < 0)
        return -1;

    if (uidPtr)
        *uidPtr = uid;
    if (gidPtr)
        *gidPtr = gid;

    return 0;
}

static
int virSecurityDACGetIds(virDomainDefPtr def, virSecurityDACDataPtr priv,
                         uid_t *uidPtr, gid_t *gidPtr)
{
    int ret;

    if (!def && !priv) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Failed to determine default DAC seclabel "
                         "for an unknown object"));
        return -1;
    }

    if ((ret = virSecurityDACParseIds(def, uidPtr, gidPtr)) <= 0)
        return ret;

    if (!priv) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("DAC seclabel couldn't be determined "
                         "for domain '%s'"), def->name);
        return -1;
    }

    if (uidPtr)
        *uidPtr = priv->user;
    if (gidPtr)
        *gidPtr = priv->group;

    return 0;
}


/* returns 1 if label isn't found, 0 on success, -1 on error */
static
int virSecurityDACParseImageIds(virDomainDefPtr def,
                                uid_t *uidPtr, gid_t *gidPtr)
{
    uid_t uid;
    gid_t gid;
    virSecurityLabelDefPtr seclabel;

    if (def == NULL)
        return 1;

    seclabel = virDomainDefGetSecurityLabelDef(def, SECURITY_DAC_NAME);
    if (seclabel == NULL || seclabel->imagelabel == NULL) {
        VIR_DEBUG("DAC imagelabel for domain '%s' wasn't found", def->name);
        return 1;
    }

    if (parseIds(seclabel->imagelabel, &uid, &gid) < 0)
        return -1;

    if (uidPtr)
        *uidPtr = uid;
    if (gidPtr)
        *gidPtr = gid;

    return 0;
}

static
int virSecurityDACGetImageIds(virDomainDefPtr def, virSecurityDACDataPtr priv,
                         uid_t *uidPtr, gid_t *gidPtr)
{
    int ret;

    if (!def && !priv) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Failed to determine default DAC imagelabel "
                         "for an unknown object"));
        return -1;
    }

    if ((ret = virSecurityDACParseImageIds(def, uidPtr, gidPtr)) <= 0)
        return ret;

    if (!priv) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("DAC imagelabel couldn't be determined "
                         "for domain '%s'"), def->name);
        return -1;
    }

    if (uidPtr)
        *uidPtr = priv->user;
    if (gidPtr)
        *gidPtr = priv->group;

    return 0;
}


static virSecurityDriverStatus
virSecurityDACProbe(const char *virtDriver ATTRIBUTE_UNUSED)
{
    return SECURITY_DRIVER_ENABLE;
}

static int
virSecurityDACOpen(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED)
{
    return 0;
}

static int
virSecurityDACClose(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED)
{
    return 0;
}


static const char * virSecurityDACGetModel(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED)
{
    return SECURITY_DAC_NAME;
}

static const char * virSecurityDACGetDOI(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED)
{
    return "0";
}

static int
virSecurityDACSetOwnership(const char *path, uid_t uid, gid_t gid)
{
    VIR_INFO("Setting DAC user and group on '%s' to '%ld:%ld'",
             path, (long) uid, (long) gid);

    if (chown(path, uid, gid) < 0) {
        struct stat sb;
        int chown_errno = errno;

        if (stat(path, &sb) >= 0) {
            if (sb.st_uid == uid &&
                sb.st_gid == gid) {
                /* It's alright, there's nothing to change anyway. */
                return 0;
            }
        }

        if (chown_errno == EOPNOTSUPP || chown_errno == EINVAL) {
            VIR_INFO("Setting user and group to '%ld:%ld' on '%s' not "
                     "supported by filesystem",
                     (long) uid, (long) gid, path);
        } else if (chown_errno == EPERM) {
            VIR_INFO("Setting user and group to '%ld:%ld' on '%s' not "
                     "permitted",
                     (long) uid, (long) gid, path);
        } else if (chown_errno == EROFS) {
            VIR_INFO("Setting user and group to '%ld:%ld' on '%s' not "
                     "possible on readonly filesystem",
                     (long) uid, (long) gid, path);
        } else {
            virReportSystemError(chown_errno,
                                 _("unable to set user and group to '%ld:%ld' "
                                   "on '%s'"),
                                 (long) uid, (long) gid, path);
            return -1;
        }
    }
    return 0;
}

static int
virSecurityDACRestoreSecurityFileLabel(const char *path)
{
    struct stat buf;
    int rc = -1;
    char *newpath = NULL;

    VIR_INFO("Restoring DAC user and group on '%s'", path);

    if (virFileResolveLink(path, &newpath) < 0) {
        virReportSystemError(errno,
                             _("cannot resolve symlink %s"), path);
        goto err;
    }

    if (stat(newpath, &buf) != 0)
        goto err;

    /* XXX record previous ownership */
    rc = virSecurityDACSetOwnership(newpath, 0, 0);

err:
    VIR_FREE(newpath);
    return rc;
}


static int
virSecurityDACSetSecurityFileLabel(virDomainDiskDefPtr disk ATTRIBUTE_UNUSED,
                                   const char *path,
                                   size_t depth ATTRIBUTE_UNUSED,
                                   void *opaque)
{
    void **params = opaque;
    virSecurityManagerPtr mgr = params[0];
    virDomainDefPtr def = params[1];
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);
    uid_t user;
    gid_t group;

    if (virSecurityDACGetImageIds(def, priv, &user, &group))
        return -1;

    return virSecurityDACSetOwnership(path, user, group);
}


static int
virSecurityDACSetSecurityImageLabel(virSecurityManagerPtr mgr,
                                    virDomainDefPtr def ATTRIBUTE_UNUSED,
                                    virDomainDiskDefPtr disk)

{
    void *params[2];
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);

    if (!priv->dynamicOwnership)
        return 0;

    if (disk->type == VIR_DOMAIN_DISK_TYPE_NETWORK)
        return 0;

    params[0] = mgr;
    params[1] = def;
    return virDomainDiskDefForeachPath(disk,
                                       false,
                                       virSecurityDACSetSecurityFileLabel,
                                       params);
}


static int
virSecurityDACRestoreSecurityImageLabelInt(virSecurityManagerPtr mgr,
                                           virDomainDefPtr def ATTRIBUTE_UNUSED,
                                           virDomainDiskDefPtr disk,
                                           int migrated)
{
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);

    if (!priv->dynamicOwnership)
        return 0;

    if (disk->type == VIR_DOMAIN_DISK_TYPE_NETWORK)
        return 0;

    /* Don't restore labels on readoly/shared disks, because
     * other VMs may still be accessing these
     * Alternatively we could iterate over all running
     * domains and try to figure out if it is in use, but
     * this would not work for clustered filesystems, since
     * we can't see running VMs using the file on other nodes
     * Safest bet is thus to skip the restore step.
     */
    if (disk->readonly || disk->shared)
        return 0;

    if (!disk->src)
        return 0;

    /* If we have a shared FS & doing migrated, we must not
     * change ownership, because that kills access on the
     * destination host which is sub-optimal for the guest
     * VM's I/O attempts :-)
     */
    if (migrated) {
        int rc = virStorageFileIsSharedFS(disk->src);
        if (rc < 0)
            return -1;
        if (rc == 1) {
            VIR_DEBUG("Skipping image label restore on %s because FS is shared",
                      disk->src);
            return 0;
        }
    }

    return virSecurityDACRestoreSecurityFileLabel(disk->src);
}


static int
virSecurityDACRestoreSecurityImageLabel(virSecurityManagerPtr mgr,
                                        virDomainDefPtr def,
                                        virDomainDiskDefPtr disk)
{
    return virSecurityDACRestoreSecurityImageLabelInt(mgr, def, disk, 0);
}


static int
virSecurityDACSetSecurityPCILabel(pciDevice *dev ATTRIBUTE_UNUSED,
                                  const char *file,
                                  void *opaque)
{
    void **params = opaque;
    virSecurityManagerPtr mgr = params[0];
    virDomainDefPtr def = params[1];
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);
    uid_t user;
    gid_t group;

    if (virSecurityDACGetIds(def, priv, &user, &group))
        return -1;

    return virSecurityDACSetOwnership(file, user, group);
}


static int
virSecurityDACSetSecurityUSBLabel(usbDevice *dev ATTRIBUTE_UNUSED,
                                  const char *file,
                                  void *opaque)
{
    void **params = opaque;
    virSecurityManagerPtr mgr = params[0];
    virDomainDefPtr def = params[1];
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);
    uid_t user;
    gid_t group;

    if (virSecurityDACGetIds(def, priv, &user, &group))
        return -1;

    return virSecurityDACSetOwnership(file, user, group);
}


static int
virSecurityDACSetSecurityHostdevLabel(virSecurityManagerPtr mgr,
                                      virDomainDefPtr def,
                                      virDomainHostdevDefPtr dev)
{
    void *params[] = {mgr, def};
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);
    int ret = -1;

    if (!priv->dynamicOwnership)
        return 0;

    if (dev->mode != VIR_DOMAIN_HOSTDEV_MODE_SUBSYS)
        return 0;

    switch (dev->source.subsys.type) {
    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB: {
        usbDevice *usb;

        if (dev->missing)
            return 0;

        usb = usbGetDevice(dev->source.subsys.u.usb.bus,
                           dev->source.subsys.u.usb.device);
        if (!usb)
            goto done;

        ret = usbDeviceFileIterate(usb, virSecurityDACSetSecurityUSBLabel,
                                   params);
        usbFreeDevice(usb);
        break;
    }

    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI: {
        pciDevice *pci = pciGetDevice(dev->source.subsys.u.pci.domain,
                                      dev->source.subsys.u.pci.bus,
                                      dev->source.subsys.u.pci.slot,
                                      dev->source.subsys.u.pci.function);

        if (!pci)
            goto done;

        ret = pciDeviceFileIterate(pci, virSecurityDACSetSecurityPCILabel,
                                   params);
        pciFreeDevice(pci);

        break;
    }

    default:
        ret = 0;
        break;
    }

done:
    return ret;
}


static int
virSecurityDACRestoreSecurityPCILabel(pciDevice *dev ATTRIBUTE_UNUSED,
                                      const char *file,
                                      void *opaque ATTRIBUTE_UNUSED)
{
    return virSecurityDACRestoreSecurityFileLabel(file);
}


static int
virSecurityDACRestoreSecurityUSBLabel(usbDevice *dev ATTRIBUTE_UNUSED,
                                       const char *file,
                                       void *opaque ATTRIBUTE_UNUSED)
{
    return virSecurityDACRestoreSecurityFileLabel(file);
}


static int
virSecurityDACRestoreSecurityHostdevLabel(virSecurityManagerPtr mgr,
                                           virDomainDefPtr def ATTRIBUTE_UNUSED,
                                           virDomainHostdevDefPtr dev)

{
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);
    int ret = -1;

    if (!priv->dynamicOwnership)
        return 0;

    if (dev->mode != VIR_DOMAIN_HOSTDEV_MODE_SUBSYS)
        return 0;

    switch (dev->source.subsys.type) {
    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB: {
        usbDevice *usb;

        if (dev->missing)
            return 0;

        usb = usbGetDevice(dev->source.subsys.u.usb.bus,
                           dev->source.subsys.u.usb.device);
        if (!usb)
            goto done;

        ret = usbDeviceFileIterate(usb, virSecurityDACRestoreSecurityUSBLabel, mgr);
        usbFreeDevice(usb);

        break;
    }

    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI: {
        pciDevice *pci = pciGetDevice(dev->source.subsys.u.pci.domain,
                                      dev->source.subsys.u.pci.bus,
                                      dev->source.subsys.u.pci.slot,
                                      dev->source.subsys.u.pci.function);

        if (!pci)
            goto done;

        ret = pciDeviceFileIterate(pci, virSecurityDACRestoreSecurityPCILabel, mgr);
        pciFreeDevice(pci);

        break;
    }

    default:
        ret = 0;
        break;
    }

done:
    return ret;
}


static int
virSecurityDACSetChardevLabel(virSecurityManagerPtr mgr,
                              virDomainDefPtr def,
                              virDomainChrSourceDefPtr dev)

{
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);
    char *in = NULL, *out = NULL;
    int ret = -1;
    uid_t user;
    gid_t group;

    if (virSecurityDACGetIds(def, priv, &user, &group))
        return -1;

    switch (dev->type) {
    case VIR_DOMAIN_CHR_TYPE_DEV:
    case VIR_DOMAIN_CHR_TYPE_FILE:
        ret = virSecurityDACSetOwnership(dev->data.file.path, user, group);
        break;

    case VIR_DOMAIN_CHR_TYPE_PIPE:
        if ((virAsprintf(&in, "%s.in", dev->data.file.path) < 0) ||
            (virAsprintf(&out, "%s.out", dev->data.file.path) < 0)) {
            virReportOOMError();
            goto done;
        }
        if (virFileExists(in) && virFileExists(out)) {
            if ((virSecurityDACSetOwnership(in, user, group) < 0) ||
                (virSecurityDACSetOwnership(out, user, group) < 0)) {
                goto done;
            }
        } else if (virSecurityDACSetOwnership(dev->data.file.path,
                                              user, group) < 0) {
            goto done;
        }
        ret = 0;
        break;

    default:
        ret = 0;
        break;
    }

done:
    VIR_FREE(in);
    VIR_FREE(out);
    return ret;
}

static int
virSecurityDACRestoreChardevLabel(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                                  virDomainChrSourceDefPtr dev)
{
    char *in = NULL, *out = NULL;
    int ret = -1;

    switch (dev->type) {
    case VIR_DOMAIN_CHR_TYPE_DEV:
    case VIR_DOMAIN_CHR_TYPE_FILE:
        ret = virSecurityDACRestoreSecurityFileLabel(dev->data.file.path);
        break;

    case VIR_DOMAIN_CHR_TYPE_PIPE:
        if ((virAsprintf(&out, "%s.out", dev->data.file.path) < 0) ||
            (virAsprintf(&in, "%s.in", dev->data.file.path) < 0)) {
            virReportOOMError();
            goto done;
        }
        if (virFileExists(in) && virFileExists(out)) {
            if ((virSecurityDACRestoreSecurityFileLabel(out) < 0) ||
                (virSecurityDACRestoreSecurityFileLabel(in) < 0)) {
            goto done;
            }
        } else if (virSecurityDACRestoreSecurityFileLabel(dev->data.file.path) < 0) {
            goto done;
        }
        ret = 0;
        break;

    default:
        ret = 0;
        break;
    }

done:
    VIR_FREE(in);
    VIR_FREE(out);
    return ret;
}


static int
virSecurityDACRestoreChardevCallback(virDomainDefPtr def ATTRIBUTE_UNUSED,
                                     virDomainChrDefPtr dev,
                                     void *opaque)
{
    virSecurityManagerPtr mgr = opaque;

    return virSecurityDACRestoreChardevLabel(mgr, &dev->source);
}


static int
virSecurityDACRestoreSecurityAllLabel(virSecurityManagerPtr mgr,
                                      virDomainDefPtr def,
                                      int migrated)
{
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);
    int i;
    int rc = 0;

    if (!priv->dynamicOwnership)
        return 0;


    VIR_DEBUG("Restoring security label on %s migrated=%d",
              def->name, migrated);

    for (i = 0 ; i < def->nhostdevs ; i++) {
        if (virSecurityDACRestoreSecurityHostdevLabel(mgr,
                                                      def,
                                                      def->hostdevs[i]) < 0)
            rc = -1;
    }
    for (i = 0 ; i < def->ndisks ; i++) {
        if (virSecurityDACRestoreSecurityImageLabelInt(mgr,
                                                       def,
                                                       def->disks[i],
                                                       migrated) < 0)
            rc = -1;
    }

    if (virDomainChrDefForeach(def,
                               false,
                               virSecurityDACRestoreChardevCallback,
                               mgr) < 0)
        rc = -1;

    if (def->os.kernel &&
        virSecurityDACRestoreSecurityFileLabel(def->os.kernel) < 0)
        rc = -1;

    if (def->os.initrd &&
        virSecurityDACRestoreSecurityFileLabel(def->os.initrd) < 0)
        rc = -1;

    return rc;
}


static int
virSecurityDACSetChardevCallback(virDomainDefPtr def ATTRIBUTE_UNUSED,
                                 virDomainChrDefPtr dev,
                                 void *opaque)
{
    virSecurityManagerPtr mgr = opaque;

    return virSecurityDACSetChardevLabel(mgr, def, &dev->source);
}


static int
virSecurityDACSetSecurityAllLabel(virSecurityManagerPtr mgr,
                                  virDomainDefPtr def,
                                  const char *stdin_path ATTRIBUTE_UNUSED)
{
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);
    int i;
    uid_t user;
    gid_t group;

    if (!priv->dynamicOwnership)
        return 0;

    for (i = 0 ; i < def->ndisks ; i++) {
        /* XXX fixme - we need to recursively label the entire tree :-( */
        if (def->disks[i]->type == VIR_DOMAIN_DISK_TYPE_DIR)
            continue;
        if (virSecurityDACSetSecurityImageLabel(mgr,
                                                def,
                                                def->disks[i]) < 0)
            return -1;
    }
    for (i = 0 ; i < def->nhostdevs ; i++) {
        if (virSecurityDACSetSecurityHostdevLabel(mgr,
                                                  def,
                                                  def->hostdevs[i]) < 0)
            return -1;
    }

    if (virDomainChrDefForeach(def,
                               true,
                               virSecurityDACSetChardevCallback,
                               mgr) < 0)
        return -1;

    if (virSecurityDACGetImageIds(def, priv, &user, &group))
        return -1;

    if (def->os.kernel &&
        virSecurityDACSetOwnership(def->os.kernel, user, group) < 0)
        return -1;

    if (def->os.initrd &&
        virSecurityDACSetOwnership(def->os.initrd, user, group) < 0)
        return -1;

    return 0;
}


static int
virSecurityDACSetSavedStateLabel(virSecurityManagerPtr mgr,
                                 virDomainDefPtr def,
                                 const char *savefile)
{
    uid_t user;
    gid_t group;
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);

    if (virSecurityDACGetImageIds(def, priv, &user, &group))
        return -1;

    return virSecurityDACSetOwnership(savefile, user, group);
}


static int
virSecurityDACRestoreSavedStateLabel(virSecurityManagerPtr mgr,
                                     virDomainDefPtr def ATTRIBUTE_UNUSED,
                                     const char *savefile)
{
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);

    if (!priv->dynamicOwnership)
        return 0;

    return virSecurityDACRestoreSecurityFileLabel(savefile);
}


static int
virSecurityDACSetProcessLabel(virSecurityManagerPtr mgr,
                              virDomainDefPtr def ATTRIBUTE_UNUSED)
{
    uid_t user;
    gid_t group;
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);

    if (virSecurityDACGetIds(def, priv, &user, &group))
        return -1;

    VIR_DEBUG("Dropping privileges of DEF to %u:%u",
              (unsigned int) user, (unsigned int) group);

    if (virSetUIDGID(user, group) < 0)
        return -1;

    return 0;
}


static int
virSecurityDACVerify(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                     virDomainDefPtr def ATTRIBUTE_UNUSED)
{
    return 0;
}

static int
virSecurityDACGenLabel(virSecurityManagerPtr mgr,
                       virDomainDefPtr def)
{
    int rc = -1;
    virSecurityLabelDefPtr seclabel;
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);

    if (mgr == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("invalid security driver"));
        return rc;
    }

    seclabel = virDomainDefGetSecurityLabelDef(def, SECURITY_DAC_NAME);
    if (seclabel == NULL) {
        return rc;
    }

    if (seclabel->imagelabel) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("security image label already "
                         "defined for VM"));
        return rc;
    }

    if (seclabel->model
        && STRNEQ(seclabel->model, SECURITY_DAC_NAME)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("security label model %s is not supported "
                         "with selinux"),
                       seclabel->model);
            return rc;
    }

    switch (seclabel->type) {
    case VIR_DOMAIN_SECLABEL_STATIC:
        if (seclabel->label == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("missing label for static security "
                             "driver in domain %s"), def->name);
            return rc;
        }
        break;
    case VIR_DOMAIN_SECLABEL_DYNAMIC:
        if (virAsprintf(&seclabel->label, "%u:%u",
                        (unsigned int) priv->user,
                        (unsigned int) priv->group) < 0) {
            virReportOOMError();
            return rc;
        }
        if (seclabel->label == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("cannot generate dac user and group id "
                             "for domain %s"), def->name);
            return rc;
        }
        break;
    case VIR_DOMAIN_SECLABEL_NONE:
        /* no op */
        return 0;
    default:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected security label type '%s'"),
                       virDomainSeclabelTypeToString(seclabel->type));
        return rc;
    }

    if (!seclabel->norelabel) {
        if (seclabel->imagelabel == NULL && seclabel->label != NULL) {
            seclabel->imagelabel = strdup(seclabel->label);
            if (seclabel->imagelabel == NULL) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("cannot generate dac user and group id "
                                 "for domain %s"), def->name);
                VIR_FREE(seclabel->label);
                seclabel->label = NULL;
                return rc;
            }
        }
    }

    return 0;
}

static int
virSecurityDACReleaseLabel(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                           virDomainDefPtr def ATTRIBUTE_UNUSED)
{
    return 0;
}

static int
virSecurityDACReserveLabel(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                           virDomainDefPtr def ATTRIBUTE_UNUSED,
                           pid_t pid ATTRIBUTE_UNUSED)
{
    return 0;
}

static int
virSecurityDACGetProcessLabel(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                              virDomainDefPtr def ATTRIBUTE_UNUSED,
                              pid_t pid ATTRIBUTE_UNUSED,
                              virSecurityLabelPtr seclabel ATTRIBUTE_UNUSED)
{
    virSecurityLabelDefPtr secdef =
        virDomainDefGetSecurityLabelDef(def, SECURITY_DAC_NAME);

    if (!secdef || !seclabel)
        return -1;

    if (secdef->label)
        strcpy(seclabel->label, secdef->label);

    return 0;
}

static int
virSecurityDACSetDaemonSocketLabel(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                                   virDomainDefPtr vm ATTRIBUTE_UNUSED)
{
    return 0;
}


static int
virSecurityDACSetSocketLabel(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                             virDomainDefPtr def ATTRIBUTE_UNUSED)
{
    return 0;
}


static int
virSecurityDACClearSocketLabel(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                               virDomainDefPtr def ATTRIBUTE_UNUSED)
{
    return 0;
}

static int
virSecurityDACSetImageFDLabel(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                              virDomainDefPtr def ATTRIBUTE_UNUSED,
                              int fd ATTRIBUTE_UNUSED)
{
    return 0;
}

static int
virSecurityDACSetTapFDLabel(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                            virDomainDefPtr def ATTRIBUTE_UNUSED,
                            int fd ATTRIBUTE_UNUSED)
{
    return 0;
}

static char *virSecurityDACGetMountOptions(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                                           virDomainDefPtr vm ATTRIBUTE_UNUSED) {
    return NULL;
}

virSecurityDriver virSecurityDriverDAC = {
    .privateDataLen                     = sizeof(virSecurityDACData),
    .name                               = SECURITY_DAC_NAME,
    .probe                              = virSecurityDACProbe,
    .open                               = virSecurityDACOpen,
    .close                              = virSecurityDACClose,

    .getModel                           = virSecurityDACGetModel,
    .getDOI                             = virSecurityDACGetDOI,

    .domainSecurityVerify               = virSecurityDACVerify,

    .domainSetSecurityImageLabel        = virSecurityDACSetSecurityImageLabel,
    .domainRestoreSecurityImageLabel    = virSecurityDACRestoreSecurityImageLabel,

    .domainSetSecurityDaemonSocketLabel = virSecurityDACSetDaemonSocketLabel,
    .domainSetSecuritySocketLabel       = virSecurityDACSetSocketLabel,
    .domainClearSecuritySocketLabel     = virSecurityDACClearSocketLabel,

    .domainGenSecurityLabel             = virSecurityDACGenLabel,
    .domainReserveSecurityLabel         = virSecurityDACReserveLabel,
    .domainReleaseSecurityLabel         = virSecurityDACReleaseLabel,

    .domainGetSecurityProcessLabel      = virSecurityDACGetProcessLabel,
    .domainSetSecurityProcessLabel      = virSecurityDACSetProcessLabel,

    .domainSetSecurityAllLabel          = virSecurityDACSetSecurityAllLabel,
    .domainRestoreSecurityAllLabel      = virSecurityDACRestoreSecurityAllLabel,

    .domainSetSecurityHostdevLabel      = virSecurityDACSetSecurityHostdevLabel,
    .domainRestoreSecurityHostdevLabel  = virSecurityDACRestoreSecurityHostdevLabel,

    .domainSetSavedStateLabel           = virSecurityDACSetSavedStateLabel,
    .domainRestoreSavedStateLabel       = virSecurityDACRestoreSavedStateLabel,

    .domainSetSecurityImageFDLabel      = virSecurityDACSetImageFDLabel,
    .domainSetSecurityTapFDLabel        = virSecurityDACSetTapFDLabel,

    .domainGetSecurityMountOptions      = virSecurityDACGetMountOptions,
};
