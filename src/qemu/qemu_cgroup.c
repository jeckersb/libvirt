/*
 * qemu_cgroup.c: QEMU cgroup management
 *
 * Copyright (C) 2006-2012 Red Hat, Inc.
 * Copyright (C) 2006 Daniel P. Berrange
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
 * Author: Daniel P. Berrange <berrange@redhat.com>
 */

#include <config.h>

#include "qemu_cgroup.h"
#include "qemu_domain.h"
#include "qemu_process.h"
#include "cgroup.h"
#include "logging.h"
#include "memory.h"
#include "virterror_internal.h"
#include "util.h"
#include "domain_audit.h"

#define VIR_FROM_THIS VIR_FROM_QEMU

static const char *const defaultDeviceACL[] = {
    "/dev/null", "/dev/full", "/dev/zero",
    "/dev/random", "/dev/urandom",
    "/dev/ptmx", "/dev/kvm", "/dev/kqemu",
    "/dev/rtc", "/dev/hpet",
    NULL,
};
#define DEVICE_PTY_MAJOR 136
#define DEVICE_SND_MAJOR 116

bool qemuCgroupControllerActive(virQEMUDriverPtr driver,
                                int controller)
{
    if (driver->cgroup == NULL)
        return false;
    if (controller < 0 || controller >= VIR_CGROUP_CONTROLLER_LAST)
        return false;
    if (!virCgroupMounted(driver->cgroup, controller))
        return false;
    if (driver->cgroupControllers & (1 << controller))
        return true;
    return false;
}

static int
qemuSetupDiskPathAllow(virDomainDiskDefPtr disk,
                       const char *path,
                       size_t depth ATTRIBUTE_UNUSED,
                       void *opaque)
{
    qemuCgroupData *data = opaque;
    int rc;

    VIR_DEBUG("Process path %s for disk", path);
    rc = virCgroupAllowDevicePath(data->cgroup, path,
                                  (disk->readonly ? VIR_CGROUP_DEVICE_READ
                                   : VIR_CGROUP_DEVICE_RW));
    virDomainAuditCgroupPath(data->vm, data->cgroup, "allow", path,
                             disk->readonly ? "r" : "rw", rc);
    if (rc < 0) {
        if (rc == -EACCES) { /* Get this for root squash NFS */
            VIR_DEBUG("Ignoring EACCES for %s", path);
        } else {
            virReportSystemError(-rc,
                                 _("Unable to allow access for disk path %s"),
                                 path);
            return -1;
        }
    }
    return 0;
}


int qemuSetupDiskCgroup(virDomainObjPtr vm,
                        virCgroupPtr cgroup,
                        virDomainDiskDefPtr disk)
{
    qemuCgroupData data = { vm, cgroup };
    return virDomainDiskDefForeachPath(disk,
                                       true,
                                       qemuSetupDiskPathAllow,
                                       &data);
}


static int
qemuTeardownDiskPathDeny(virDomainDiskDefPtr disk ATTRIBUTE_UNUSED,
                         const char *path,
                         size_t depth ATTRIBUTE_UNUSED,
                         void *opaque)
{
    qemuCgroupData *data = opaque;
    int rc;

    VIR_DEBUG("Process path %s for disk", path);
    rc = virCgroupDenyDevicePath(data->cgroup, path,
                                 VIR_CGROUP_DEVICE_RWM);
    virDomainAuditCgroupPath(data->vm, data->cgroup, "deny", path, "rwm", rc);
    if (rc < 0) {
        if (rc == -EACCES) { /* Get this for root squash NFS */
            VIR_DEBUG("Ignoring EACCES for %s", path);
        } else {
            virReportSystemError(-rc,
                                 _("Unable to deny access for disk path %s"),
                                 path);
            return -1;
        }
    }
    return 0;
}


int qemuTeardownDiskCgroup(virDomainObjPtr vm,
                           virCgroupPtr cgroup,
                           virDomainDiskDefPtr disk)
{
    qemuCgroupData data = { vm, cgroup };
    return virDomainDiskDefForeachPath(disk,
                                       true,
                                       qemuTeardownDiskPathDeny,
                                       &data);
}


static int
qemuSetupChardevCgroup(virDomainDefPtr def,
                       virDomainChrDefPtr dev,
                       void *opaque)
{
    qemuCgroupData *data = opaque;
    int rc;

    if (dev->source.type != VIR_DOMAIN_CHR_TYPE_DEV)
        return 0;


    VIR_DEBUG("Process path '%s' for disk", dev->source.data.file.path);
    rc = virCgroupAllowDevicePath(data->cgroup, dev->source.data.file.path,
                                  VIR_CGROUP_DEVICE_RW);
    virDomainAuditCgroupPath(data->vm, data->cgroup, "allow",
                             dev->source.data.file.path, "rw", rc);
    if (rc < 0) {
        virReportSystemError(-rc,
                             _("Unable to allow device %s for %s"),
                             dev->source.data.file.path, def->name);
        return -1;
    }

    return 0;
}


int qemuSetupHostUsbDeviceCgroup(usbDevice *dev ATTRIBUTE_UNUSED,
                                 const char *path,
                                 void *opaque)
{
    qemuCgroupData *data = opaque;
    int rc;

    VIR_DEBUG("Process path '%s' for USB device", path);
    rc = virCgroupAllowDevicePath(data->cgroup, path,
                                  VIR_CGROUP_DEVICE_RW);
    virDomainAuditCgroupPath(data->vm, data->cgroup, "allow", path, "rw", rc);
    if (rc < 0) {
        virReportSystemError(-rc,
                             _("Unable to allow device %s"),
                             path);
        return -1;
    }

    return 0;
}

int qemuSetupCgroup(virQEMUDriverPtr driver,
                    virDomainObjPtr vm,
                    virBitmapPtr nodemask)
{
    virCgroupPtr cgroup = NULL;
    int rc;
    unsigned int i;
    const char *const *deviceACL =
        driver->cgroupDeviceACL ?
        (const char *const *)driver->cgroupDeviceACL :
        defaultDeviceACL;

    if (driver->cgroup == NULL)
        return 0; /* Not supported, so claim success */

    rc = virCgroupForDomain(driver->cgroup, vm->def->name, &cgroup, 1);
    if (rc != 0) {
        virReportSystemError(-rc,
                             _("Unable to create cgroup for %s"),
                             vm->def->name);
        goto cleanup;
    }

    if (qemuCgroupControllerActive(driver, VIR_CGROUP_CONTROLLER_DEVICES)) {
        qemuCgroupData data = { vm, cgroup };
        rc = virCgroupDenyAllDevices(cgroup);
        virDomainAuditCgroup(vm, cgroup, "deny", "all", rc == 0);
        if (rc != 0) {
            if (rc == -EPERM) {
                VIR_WARN("Group devices ACL is not accessible, disabling whitelisting");
                goto done;
            }

            virReportSystemError(-rc,
                                 _("Unable to deny all devices for %s"), vm->def->name);
            goto cleanup;
        }

        for (i = 0; i < vm->def->ndisks ; i++) {
            if (qemuDomainDetermineDiskChain(driver, vm->def->disks[i],
                                             false) < 0 ||
                qemuSetupDiskCgroup(vm, cgroup, vm->def->disks[i]) < 0)
                goto cleanup;
        }

        rc = virCgroupAllowDeviceMajor(cgroup, 'c', DEVICE_PTY_MAJOR,
                                       VIR_CGROUP_DEVICE_RW);
        virDomainAuditCgroupMajor(vm, cgroup, "allow", DEVICE_PTY_MAJOR,
                                  "pty", "rw", rc == 0);
        if (rc != 0) {
            virReportSystemError(-rc, "%s",
                                 _("unable to allow /dev/pts/ devices"));
            goto cleanup;
        }

        if (vm->def->nsounds &&
            (!vm->def->ngraphics ||
             ((vm->def->graphics[0]->type == VIR_DOMAIN_GRAPHICS_TYPE_VNC &&
               driver->vncAllowHostAudio) ||
              (vm->def->graphics[0]->type == VIR_DOMAIN_GRAPHICS_TYPE_SDL)))) {
            rc = virCgroupAllowDeviceMajor(cgroup, 'c', DEVICE_SND_MAJOR,
                                           VIR_CGROUP_DEVICE_RW);
            virDomainAuditCgroupMajor(vm, cgroup, "allow", DEVICE_SND_MAJOR,
                                      "sound", "rw", rc == 0);
            if (rc != 0) {
                virReportSystemError(-rc, "%s",
                                     _("unable to allow /dev/snd/ devices"));
                goto cleanup;
            }
        }

        for (i = 0; deviceACL[i] != NULL ; i++) {
            rc = virCgroupAllowDevicePath(cgroup, deviceACL[i],
                                          VIR_CGROUP_DEVICE_RW);
            virDomainAuditCgroupPath(vm, cgroup, "allow", deviceACL[i], "rw", rc);
            if (rc < 0 &&
                rc != -ENOENT) {
                virReportSystemError(-rc,
                                     _("unable to allow device %s"),
                                     deviceACL[i]);
                goto cleanup;
            }
        }

        if (virDomainChrDefForeach(vm->def,
                                   true,
                                   qemuSetupChardevCgroup,
                                   &data) < 0)
            goto cleanup;

        for (i = 0; i < vm->def->nhostdevs; i++) {
            virDomainHostdevDefPtr hostdev = vm->def->hostdevs[i];
            usbDevice *usb;

            if (hostdev->mode != VIR_DOMAIN_HOSTDEV_MODE_SUBSYS)
                continue;
            if (hostdev->source.subsys.type != VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB)
                continue;
            if (hostdev->missing)
                continue;

            if ((usb = usbGetDevice(hostdev->source.subsys.u.usb.bus,
                                    hostdev->source.subsys.u.usb.device)) == NULL)
                goto cleanup;

            if (usbDeviceFileIterate(usb, qemuSetupHostUsbDeviceCgroup,
                                     &data) < 0)
                goto cleanup;
        }
    }

    if (vm->def->blkio.weight != 0) {
        if (qemuCgroupControllerActive(driver, VIR_CGROUP_CONTROLLER_BLKIO)) {
            rc = virCgroupSetBlkioWeight(cgroup, vm->def->blkio.weight);
            if (rc != 0) {
                virReportSystemError(-rc,
                                     _("Unable to set io weight for domain %s"),
                                     vm->def->name);
                goto cleanup;
            }
        } else {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Block I/O tuning is not available on this host"));
            goto cleanup;
        }
    }

    if (vm->def->blkio.ndevices) {
        if (qemuCgroupControllerActive(driver, VIR_CGROUP_CONTROLLER_BLKIO)) {
            for (i = 0; i < vm->def->blkio.ndevices; i++) {
                virBlkioDeviceWeightPtr dw = &vm->def->blkio.devices[i];
                if (!dw->weight)
                    continue;
                rc = virCgroupSetBlkioDeviceWeight(cgroup, dw->path,
                                                   dw->weight);
                if (rc != 0) {
                    virReportSystemError(-rc,
                                         _("Unable to set io device weight "
                                           "for domain %s"),
                                         vm->def->name);
                    goto cleanup;
                }
            }
        } else {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Block I/O tuning is not available on this host"));
            goto cleanup;
        }
    }

    if (qemuCgroupControllerActive(driver, VIR_CGROUP_CONTROLLER_MEMORY)) {
        unsigned long long hard_limit = vm->def->mem.hard_limit;

        if (!hard_limit) {
            /* If there is no hard_limit set, set a reasonable
             * one to avoid system trashing caused by exploited qemu.
             * As 'reasonable limit' has been chosen:
             *     (1 + k) * (domain memory + total video memory) + F
             * where k = 0.02 and F = 200MB. */
            hard_limit = vm->def->mem.max_balloon;
            for (i = 0; i < vm->def->nvideos; i++)
                hard_limit += vm->def->videos[i]->vram;
            hard_limit = hard_limit * 1.02 + 204800;
        }

        rc = virCgroupSetMemoryHardLimit(cgroup, hard_limit);
        if (rc != 0) {
            virReportSystemError(-rc,
                                 _("Unable to set memory hard limit for domain %s"),
                                 vm->def->name);
            goto cleanup;
        }
        if (vm->def->mem.soft_limit != 0) {
            rc = virCgroupSetMemorySoftLimit(cgroup, vm->def->mem.soft_limit);
            if (rc != 0) {
                virReportSystemError(-rc,
                                     _("Unable to set memory soft limit for domain %s"),
                                     vm->def->name);
                goto cleanup;
            }
        }

        if (vm->def->mem.swap_hard_limit != 0) {
            rc = virCgroupSetMemSwapHardLimit(cgroup, vm->def->mem.swap_hard_limit);
            if (rc != 0) {
                virReportSystemError(-rc,
                                     _("Unable to set swap hard limit for domain %s"),
                                     vm->def->name);
                goto cleanup;
            }
        }
    } else if (vm->def->mem.hard_limit != 0 ||
               vm->def->mem.soft_limit != 0 ||
               vm->def->mem.swap_hard_limit != 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Memory cgroup is not available on this host"));
    } else {
        VIR_WARN("Could not autoset a RSS limit for domain %s", vm->def->name);
    }

    if (vm->def->cputune.shares != 0) {
        if (qemuCgroupControllerActive(driver, VIR_CGROUP_CONTROLLER_CPU)) {
            rc = virCgroupSetCpuShares(cgroup, vm->def->cputune.shares);
            if (rc != 0) {
                virReportSystemError(-rc,
                                     _("Unable to set io cpu shares for domain %s"),
                                     vm->def->name);
                goto cleanup;
            }
        } else {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("CPU tuning is not available on this host"));
        }
    }

    if ((vm->def->numatune.memory.nodemask ||
         (vm->def->numatune.memory.placement_mode ==
          VIR_DOMAIN_NUMATUNE_MEM_PLACEMENT_MODE_AUTO)) &&
        vm->def->numatune.memory.mode == VIR_DOMAIN_NUMATUNE_MEM_STRICT &&
        qemuCgroupControllerActive(driver, VIR_CGROUP_CONTROLLER_CPUSET)) {
        char *mask = NULL;
        if (vm->def->numatune.memory.placement_mode ==
            VIR_DOMAIN_NUMATUNE_MEM_PLACEMENT_MODE_AUTO)
            mask = virBitmapFormat(nodemask);
        else
            mask = virBitmapFormat(vm->def->numatune.memory.nodemask);
        if (!mask) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("failed to convert memory nodemask"));
            goto cleanup;
        }

        rc = virCgroupSetCpusetMems(cgroup, mask);
        VIR_FREE(mask);
        if (rc != 0) {
            virReportSystemError(-rc,
                                 _("Unable to set cpuset.mems for domain %s"),
                                 vm->def->name);
            goto cleanup;
        }
    }
done:
    virCgroupFree(&cgroup);
    return 0;

cleanup:
    if (cgroup) {
        virCgroupRemove(cgroup);
        virCgroupFree(&cgroup);
    }
    return -1;
}

int qemuSetupCgroupVcpuBW(virCgroupPtr cgroup, unsigned long long period,
                          long long quota)
{
    int rc;
    unsigned long long old_period;

    if (period == 0 && quota == 0)
        return 0;

    if (period) {
        /* get old period, and we can rollback if set quota failed */
        rc = virCgroupGetCpuCfsPeriod(cgroup, &old_period);
        if (rc < 0) {
            virReportSystemError(-rc,
                                 "%s", _("Unable to get cpu bandwidth period"));
            return -1;
        }

        rc = virCgroupSetCpuCfsPeriod(cgroup, period);
        if (rc < 0) {
            virReportSystemError(-rc,
                                 "%s", _("Unable to set cpu bandwidth period"));
            return -1;
        }
    }

    if (quota) {
        rc = virCgroupSetCpuCfsQuota(cgroup, quota);
        if (rc < 0) {
            virReportSystemError(-rc,
                                 "%s", _("Unable to set cpu bandwidth quota"));
            goto cleanup;
        }
    }

    return 0;

cleanup:
    if (period) {
        rc = virCgroupSetCpuCfsPeriod(cgroup, old_period);
        if (rc < 0)
            virReportSystemError(-rc, "%s",
                                 _("Unable to rollback cpu bandwidth period"));
    }

    return -1;
}

int qemuSetupCgroupVcpuPin(virCgroupPtr cgroup,
                           virDomainVcpuPinDefPtr *vcpupin,
                           int nvcpupin,
                           int vcpuid)
{
    int i;

    for (i = 0; i < nvcpupin; i++) {
        if (vcpuid == vcpupin[i]->vcpuid) {
            return qemuSetupCgroupEmulatorPin(cgroup, vcpupin[i]->cpumask);
        }
    }

    return -1;
}

int qemuSetupCgroupEmulatorPin(virCgroupPtr cgroup,
                               virBitmapPtr cpumask)
{
    int rc = 0;
    char *new_cpus = NULL;

    new_cpus = virBitmapFormat(cpumask);
    if (!new_cpus) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("failed to convert cpu mask"));
        rc = -1;
        goto cleanup;
    }

    rc = virCgroupSetCpusetCpus(cgroup, new_cpus);
    if (rc < 0) {
        virReportSystemError(-rc,
                             "%s",
                             _("Unable to set cpuset.cpus"));
        goto cleanup;
    }

cleanup:
    VIR_FREE(new_cpus);
    return rc;
}

int qemuSetupCgroupForVcpu(virQEMUDriverPtr driver, virDomainObjPtr vm)
{
    virCgroupPtr cgroup = NULL;
    virCgroupPtr cgroup_vcpu = NULL;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainDefPtr def = vm->def;
    int rc;
    unsigned int i, j;
    unsigned long long period = vm->def->cputune.period;
    long long quota = vm->def->cputune.quota;

    if ((period || quota) &&
        (!driver->cgroup ||
         !qemuCgroupControllerActive(driver, VIR_CGROUP_CONTROLLER_CPU))) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("cgroup cpu is required for scheduler tuning"));
        return -1;
    }

    /* We are trying to setup cgroups for CPU pinning, which can also be done
     * with virProcessInfoSetAffinity, thus the lack of cgroups is not fatal
     * here.
     */
    if (driver->cgroup == NULL)
        return 0;

    rc = virCgroupForDomain(driver->cgroup, vm->def->name, &cgroup, 0);
    if (rc != 0) {
        virReportSystemError(-rc,
                             _("Unable to find cgroup for %s"),
                             vm->def->name);
        goto cleanup;
    }

    if (priv->nvcpupids == 0 || priv->vcpupids[0] == vm->pid) {
        /* If we don't know VCPU<->PID mapping or all vcpu runs in the same
         * thread, we cannot control each vcpu.
         */
        VIR_WARN("Unable to get vcpus' pids.");
        virCgroupFree(&cgroup);
        return 0;
    }

    for (i = 0; i < priv->nvcpupids; i++) {
        rc = virCgroupForVcpu(cgroup, i, &cgroup_vcpu, 1);
        if (rc < 0) {
            virReportSystemError(-rc,
                                 _("Unable to create vcpu cgroup for %s(vcpu:"
                                   " %d)"),
                                 vm->def->name, i);
            goto cleanup;
        }

        /* move the thread for vcpu to sub dir */
        rc = virCgroupAddTask(cgroup_vcpu, priv->vcpupids[i]);
        if (rc < 0) {
            virReportSystemError(-rc,
                                 _("unable to add vcpu %d task %d to cgroup"),
                                 i, priv->vcpupids[i]);
            goto cleanup;
        }

        if (period || quota) {
            if (qemuSetupCgroupVcpuBW(cgroup_vcpu, period, quota) < 0)
                goto cleanup;
        }

        /* Set vcpupin in cgroup if vcpupin xml is provided */
        if (qemuCgroupControllerActive(driver, VIR_CGROUP_CONTROLLER_CPUSET)) {
            /* find the right CPU to pin, otherwise
             * qemuSetupCgroupVcpuPin will fail. */
            for (j = 0; j < def->cputune.nvcpupin; j++) {
                if (def->cputune.vcpupin[j]->vcpuid != i)
                    continue;

                if (qemuSetupCgroupVcpuPin(cgroup_vcpu,
                                           def->cputune.vcpupin,
                                           def->cputune.nvcpupin,
                                           i) < 0)
                    goto cleanup;

                break;
            }
        }

        virCgroupFree(&cgroup_vcpu);
    }

    virCgroupFree(&cgroup);
    return 0;

cleanup:
    if (cgroup_vcpu) {
        virCgroupRemove(cgroup_vcpu);
        virCgroupFree(&cgroup_vcpu);
    }

    if (cgroup) {
        virCgroupRemove(cgroup);
        virCgroupFree(&cgroup);
    }

    return -1;
}

int qemuSetupCgroupForEmulator(virQEMUDriverPtr driver,
                               virDomainObjPtr vm,
                               virBitmapPtr nodemask)
{
    virBitmapPtr cpumask = NULL;
    virBitmapPtr cpumap = NULL;
    virCgroupPtr cgroup = NULL;
    virCgroupPtr cgroup_emulator = NULL;
    virDomainDefPtr def = vm->def;
    unsigned long long period = vm->def->cputune.emulator_period;
    long long quota = vm->def->cputune.emulator_quota;
    int rc, i;

    if ((period || quota) &&
        (!driver->cgroup ||
         !qemuCgroupControllerActive(driver, VIR_CGROUP_CONTROLLER_CPU))) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("cgroup cpu is required for scheduler tuning"));
        return -1;
    }

    if (driver->cgroup == NULL)
        return 0; /* Not supported, so claim success */

    rc = virCgroupForDomain(driver->cgroup, vm->def->name, &cgroup, 0);
    if (rc != 0) {
        virReportSystemError(-rc,
                             _("Unable to find cgroup for %s"),
                             vm->def->name);
        goto cleanup;
    }

    rc = virCgroupForEmulator(cgroup, &cgroup_emulator, 1);
    if (rc < 0) {
        virReportSystemError(-rc,
                             _("Unable to create emulator cgroup for %s"),
                             vm->def->name);
        goto cleanup;
    }

    for (i = 0; i < VIR_CGROUP_CONTROLLER_LAST; i++) {
        if (!qemuCgroupControllerActive(driver, i))
            continue;
        rc = virCgroupMoveTask(cgroup, cgroup_emulator, i);
        if (rc < 0) {
            virReportSystemError(-rc,
                                 _("Unable to move tasks from domain cgroup to "
                                   "emulator cgroup in controller %d for %s"),
                                 i, vm->def->name);
            goto cleanup;
        }
    }

    if (def->placement_mode == VIR_DOMAIN_CPU_PLACEMENT_MODE_AUTO) {
        if (!(cpumap = qemuPrepareCpumap(driver, nodemask)))
            goto cleanup;
        cpumask = cpumap;
    } else if (def->cputune.emulatorpin) {
        cpumask = def->cputune.emulatorpin->cpumask;
    } else if (def->cpumask) {
        cpumask = def->cpumask;
    }

    if (cpumask) {
        if (qemuCgroupControllerActive(driver, VIR_CGROUP_CONTROLLER_CPUSET)) {
            rc = qemuSetupCgroupEmulatorPin(cgroup_emulator, cpumask);
            if (rc < 0)
                goto cleanup;
        }
        cpumask = NULL; /* sanity */
    }

    if (period || quota) {
        if (qemuCgroupControllerActive(driver, VIR_CGROUP_CONTROLLER_CPU)) {
            if ((rc = qemuSetupCgroupVcpuBW(cgroup_emulator, period,
                                            quota)) < 0)
                goto cleanup;
        }
    }

    virCgroupFree(&cgroup_emulator);
    virCgroupFree(&cgroup);
    virBitmapFree(cpumap);
    return 0;

cleanup:
    virBitmapFree(cpumap);

    if (cgroup_emulator) {
        virCgroupRemove(cgroup_emulator);
        virCgroupFree(&cgroup_emulator);
    }

    if (cgroup) {
        virCgroupRemove(cgroup);
        virCgroupFree(&cgroup);
    }

    return rc;
}

int qemuRemoveCgroup(virQEMUDriverPtr driver,
                     virDomainObjPtr vm,
                     int quiet)
{
    virCgroupPtr cgroup;
    int rc;

    if (driver->cgroup == NULL)
        return 0; /* Not supported, so claim success */

    rc = virCgroupForDomain(driver->cgroup, vm->def->name, &cgroup, 0);
    if (rc != 0) {
        if (!quiet)
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unable to find cgroup for %s"),
                           vm->def->name);
        return rc;
    }

    rc = virCgroupRemove(cgroup);
    virCgroupFree(&cgroup);
    return rc;
}

int qemuAddToCgroup(virQEMUDriverPtr driver,
                    virDomainDefPtr def)
{
    virCgroupPtr cgroup = NULL;
    int ret = -1;
    int rc;

    if (driver->cgroup == NULL)
        return 0; /* Not supported, so claim success */

    rc = virCgroupForDomain(driver->cgroup, def->name, &cgroup, 0);
    if (rc != 0) {
        virReportSystemError(-rc,
                             _("unable to find cgroup for domain %s"),
                             def->name);
        goto cleanup;
    }

    rc = virCgroupAddTask(cgroup, getpid());
    if (rc != 0) {
        virReportSystemError(-rc,
                             _("unable to add domain %s task %d to cgroup"),
                             def->name, getpid());
        goto cleanup;
    }

    ret = 0;

cleanup:
    virCgroupFree(&cgroup);
    return ret;
}
