/*
 * qemu_process.h: QEMU process management
 *
 * Copyright (C) 2006-2012 Red Hat, Inc.
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
 */

#include <config.h>

#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <linux/capability.h>

#include "qemu_process.h"
#include "qemu_domain.h"
#include "qemu_cgroup.h"
#include "qemu_capabilities.h"
#include "qemu_monitor.h"
#include "qemu_command.h"
#include "qemu_hostdev.h"
#include "qemu_hotplug.h"
#include "qemu_bridge_filter.h"
#include "qemu_migration.h"

#if HAVE_NUMACTL
# define NUMA_VERSION1_COMPATIBILITY 1
# include <numa.h>
#endif

#include "datatypes.h"
#include "logging.h"
#include "virterror_internal.h"
#include "memory.h"
#include "hooks.h"
#include "virfile.h"
#include "virpidfile.h"
#include "util.h"
#include "c-ctype.h"
#include "nodeinfo.h"
#include "processinfo.h"
#include "domain_audit.h"
#include "domain_nwfilter.h"
#include "locking/domain_lock.h"
#include "network/bridge_driver.h"
#include "uuid.h"
#include "virprocess.h"
#include "virtime.h"
#include "virnetdevtap.h"
#include "bitmap.h"

#define VIR_FROM_THIS VIR_FROM_QEMU

#define START_POSTFIX ": starting up\n"
#define ATTACH_POSTFIX ": attaching\n"
#define SHUTDOWN_POSTFIX ": shutting down\n"

/**
 * qemuProcessRemoveDomainStatus
 *
 * remove all state files of a domain from statedir
 *
 * Returns 0 on success
 */
static int
qemuProcessRemoveDomainStatus(virQEMUDriverPtr driver,
                              virDomainObjPtr vm)
{
    char ebuf[1024];
    char *file = NULL;
    qemuDomainObjPrivatePtr priv = vm->privateData;

    if (virAsprintf(&file, "%s/%s.xml", driver->stateDir, vm->def->name) < 0) {
        virReportOOMError();
        return -1;
    }

    if (unlink(file) < 0 && errno != ENOENT && errno != ENOTDIR)
        VIR_WARN("Failed to remove domain XML for %s: %s",
                 vm->def->name, virStrerror(errno, ebuf, sizeof(ebuf)));
    VIR_FREE(file);

    if (priv->pidfile &&
        unlink(priv->pidfile) < 0 &&
        errno != ENOENT)
        VIR_WARN("Failed to remove PID file for %s: %s",
                 vm->def->name, virStrerror(errno, ebuf, sizeof(ebuf)));

    return 0;
}


/* XXX figure out how to remove this */
extern virQEMUDriverPtr qemu_driver;

/*
 * This is a callback registered with a qemuAgentPtr instance,
 * and to be invoked when the agent console hits an end of file
 * condition, or error, thus indicating VM shutdown should be
 * performed
 */
static void
qemuProcessHandleAgentEOF(qemuAgentPtr agent,
                          virDomainObjPtr vm)
{
    virQEMUDriverPtr driver = qemu_driver;
    qemuDomainObjPrivatePtr priv;

    VIR_DEBUG("Received EOF from agent on %p '%s'", vm, vm->def->name);

    qemuDriverLock(driver);
    virDomainObjLock(vm);

    priv = vm->privateData;
    if (priv->agent == agent)
        priv->agent = NULL;

    virDomainObjUnlock(vm);
    qemuDriverUnlock(driver);

    qemuAgentClose(agent);
}


/*
 * This is invoked when there is some kind of error
 * parsing data to/from the agent. The VM can continue
 * to run, but no further agent commands will be
 * allowed
 */
static void
qemuProcessHandleAgentError(qemuAgentPtr agent ATTRIBUTE_UNUSED,
                            virDomainObjPtr vm)
{
    virQEMUDriverPtr driver = qemu_driver;
    qemuDomainObjPrivatePtr priv;

    VIR_DEBUG("Received error from agent on %p '%s'", vm, vm->def->name);

    qemuDriverLock(driver);
    virDomainObjLock(vm);

    priv = vm->privateData;

    priv->agentError = true;

    virDomainObjUnlock(vm);
    qemuDriverUnlock(driver);
}

static void qemuProcessHandleAgentDestroy(qemuAgentPtr agent,
                                          virDomainObjPtr vm)
{
    VIR_DEBUG("Received destroy agent=%p vm=%p", agent, vm);

    virObjectUnref(vm);
}


static qemuAgentCallbacks agentCallbacks = {
    .destroy = qemuProcessHandleAgentDestroy,
    .eofNotify = qemuProcessHandleAgentEOF,
    .errorNotify = qemuProcessHandleAgentError,
};

static virDomainChrSourceDefPtr
qemuFindAgentConfig(virDomainDefPtr def)
{
    virDomainChrSourceDefPtr config = NULL;
    int i;

    for (i = 0 ; i < def->nchannels ; i++) {
        virDomainChrDefPtr channel = def->channels[i];

        if (channel->targetType != VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO)
            continue;

        if (STREQ_NULLABLE(channel->target.name, "org.qemu.guest_agent.0")) {
            config = &channel->source;
            break;
        }
    }

    return config;
}

static int
qemuConnectAgent(virQEMUDriverPtr driver, virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int ret = -1;
    qemuAgentPtr agent = NULL;
    virDomainChrSourceDefPtr config = qemuFindAgentConfig(vm->def);

    if (!config)
        return 0;

    if (virSecurityManagerSetDaemonSocketLabel(driver->securityManager,
                                               vm->def) < 0) {
        VIR_ERROR(_("Failed to set security context for agent for %s"),
                  vm->def->name);
        goto cleanup;
    }

    /* Hold an extra reference because we can't allow 'vm' to be
     * deleted while the agent is active */
    virObjectRef(vm);

    ignore_value(virTimeMillisNow(&priv->agentStart));
    virDomainObjUnlock(vm);
    qemuDriverUnlock(driver);

    agent = qemuAgentOpen(vm,
                          config,
                          &agentCallbacks);

    qemuDriverLock(driver);
    virDomainObjLock(vm);
    priv->agentStart = 0;

    if (virSecurityManagerClearSocketLabel(driver->securityManager,
                                           vm->def) < 0) {
        VIR_ERROR(_("Failed to clear security context for agent for %s"),
                  vm->def->name);
        goto cleanup;
    }

    if (agent == NULL)
        virObjectUnref(vm);

    if (!virDomainObjIsActive(vm)) {
        qemuAgentClose(agent);
        goto cleanup;
    }
    priv->agent = agent;

    if (priv->agent == NULL) {
        VIR_INFO("Failed to connect agent for %s", vm->def->name);
        goto cleanup;
    }

    ret = 0;

cleanup:
    return ret;
}


/*
 * This is a callback registered with a qemuMonitorPtr instance,
 * and to be invoked when the monitor console hits an end of file
 * condition, or error, thus indicating VM shutdown should be
 * performed
 */
static void
qemuProcessHandleMonitorEOF(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                            virDomainObjPtr vm)
{
    virQEMUDriverPtr driver = qemu_driver;
    virDomainEventPtr event = NULL;
    qemuDomainObjPrivatePtr priv;
    int eventReason = VIR_DOMAIN_EVENT_STOPPED_SHUTDOWN;
    int stopReason = VIR_DOMAIN_SHUTOFF_SHUTDOWN;
    const char *auditReason = "shutdown";

    VIR_DEBUG("Received EOF on %p '%s'", vm, vm->def->name);

    qemuDriverLock(driver);
    virDomainObjLock(vm);

    priv = vm->privateData;

    if (priv->beingDestroyed) {
        VIR_DEBUG("Domain is being destroyed, EOF is expected");
        goto unlock;
    }

    if (!virDomainObjIsActive(vm)) {
        VIR_DEBUG("Domain %p is not active, ignoring EOF", vm);
        goto unlock;
    }

    if (priv->monJSON && !priv->gotShutdown) {
        VIR_DEBUG("Monitor connection to '%s' closed without SHUTDOWN event; "
                  "assuming the domain crashed", vm->def->name);
        eventReason = VIR_DOMAIN_EVENT_STOPPED_FAILED;
        stopReason = VIR_DOMAIN_SHUTOFF_CRASHED;
        auditReason = "failed";
    }

    event = virDomainEventNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_STOPPED,
                                     eventReason);
    qemuProcessStop(driver, vm, stopReason, 0);
    virDomainAuditStop(vm, auditReason);

    if (!vm->persistent) {
        qemuDomainRemoveInactive(driver, vm);
        goto cleanup;
    }

unlock:
    virDomainObjUnlock(vm);

cleanup:
    if (event)
        qemuDomainEventQueue(driver, event);
    qemuDriverUnlock(driver);
}


/*
 * This is invoked when there is some kind of error
 * parsing data to/from the monitor. The VM can continue
 * to run, but no further monitor commands will be
 * allowed
 */
static void
qemuProcessHandleMonitorError(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                              virDomainObjPtr vm)
{
    virQEMUDriverPtr driver = qemu_driver;
    virDomainEventPtr event = NULL;

    VIR_DEBUG("Received error on %p '%s'", vm, vm->def->name);

    qemuDriverLock(driver);
    virDomainObjLock(vm);

    ((qemuDomainObjPrivatePtr) vm->privateData)->monError = true;
    event = virDomainEventControlErrorNewFromObj(vm);
    if (event)
        qemuDomainEventQueue(driver, event);

    virDomainObjUnlock(vm);
    qemuDriverUnlock(driver);
}


static virDomainDiskDefPtr
qemuProcessFindDomainDiskByPath(virDomainObjPtr vm,
                                const char *path)
{
    int i = virDomainDiskIndexByName(vm->def, path, true);

    if (i >= 0)
        return vm->def->disks[i];

    virReportError(VIR_ERR_INTERNAL_ERROR,
                   _("no disk found with path %s"),
                   path);
    return NULL;
}

static virDomainDiskDefPtr
qemuProcessFindDomainDiskByAlias(virDomainObjPtr vm,
                                 const char *alias)
{
    int i;

    if (STRPREFIX(alias, QEMU_DRIVE_HOST_PREFIX))
        alias += strlen(QEMU_DRIVE_HOST_PREFIX);

    for (i = 0; i < vm->def->ndisks; i++) {
        virDomainDiskDefPtr disk;

        disk = vm->def->disks[i];
        if (disk->info.alias != NULL && STREQ(disk->info.alias, alias))
            return disk;
    }

    virReportError(VIR_ERR_INTERNAL_ERROR,
                   _("no disk found with alias %s"),
                   alias);
    return NULL;
}

static int
qemuProcessGetVolumeQcowPassphrase(virConnectPtr conn,
                                   virDomainDiskDefPtr disk,
                                   char **secretRet,
                                   size_t *secretLen)
{
    virSecretPtr secret;
    char *passphrase;
    unsigned char *data;
    size_t size;
    int ret = -1;
    virStorageEncryptionPtr enc;

    if (!disk->encryption) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("disk %s does not have any encryption information"),
                       disk->src);
        return -1;
    }
    enc = disk->encryption;

    if (!conn) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("cannot find secrets without a connection"));
        goto cleanup;
    }

    if (conn->secretDriver == NULL ||
        conn->secretDriver->lookupByUUID == NULL ||
        conn->secretDriver->getValue == NULL) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("secret storage not supported"));
        goto cleanup;
    }

    if (enc->format != VIR_STORAGE_ENCRYPTION_FORMAT_QCOW ||
        enc->nsecrets != 1 ||
        enc->secrets[0]->type !=
        VIR_STORAGE_ENCRYPTION_SECRET_TYPE_PASSPHRASE) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("invalid <encryption> for volume %s"), disk->src);
        goto cleanup;
    }

    secret = conn->secretDriver->lookupByUUID(conn,
                                              enc->secrets[0]->uuid);
    if (secret == NULL)
        goto cleanup;
    data = conn->secretDriver->getValue(secret, &size, 0,
                                        VIR_SECRET_GET_VALUE_INTERNAL_CALL);
    virObjectUnref(secret);
    if (data == NULL)
        goto cleanup;

    if (memchr(data, '\0', size) != NULL) {
        memset(data, 0, size);
        VIR_FREE(data);
        virReportError(VIR_ERR_XML_ERROR,
                       _("format='qcow' passphrase for %s must not contain a "
                         "'\\0'"), disk->src);
        goto cleanup;
    }

    if (VIR_ALLOC_N(passphrase, size + 1) < 0) {
        memset(data, 0, size);
        VIR_FREE(data);
        virReportOOMError();
        goto cleanup;
    }
    memcpy(passphrase, data, size);
    passphrase[size] = '\0';

    memset(data, 0, size);
    VIR_FREE(data);

    *secretRet = passphrase;
    *secretLen = size;

    ret = 0;

cleanup:
    return ret;
}

static int
qemuProcessFindVolumeQcowPassphrase(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                                    virConnectPtr conn,
                                    virDomainObjPtr vm,
                                    const char *path,
                                    char **secretRet,
                                    size_t *secretLen)
{
    virDomainDiskDefPtr disk;
    int ret = -1;

    virDomainObjLock(vm);
    disk = qemuProcessFindDomainDiskByPath(vm, path);

    if (!disk)
        goto cleanup;

    ret = qemuProcessGetVolumeQcowPassphrase(conn, disk, secretRet, secretLen);

cleanup:
    virDomainObjUnlock(vm);
    return ret;
}


static int
qemuProcessHandleReset(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                       virDomainObjPtr vm)
{
    virQEMUDriverPtr driver = qemu_driver;
    virDomainEventPtr event;
    qemuDomainObjPrivatePtr priv;

    virDomainObjLock(vm);

    event = virDomainEventRebootNewFromObj(vm);
    priv = vm->privateData;
    if (priv->agent)
        qemuAgentNotifyEvent(priv->agent, QEMU_AGENT_EVENT_RESET);

    virDomainObjUnlock(vm);

    if (event) {
        qemuDriverLock(driver);
        qemuDomainEventQueue(driver, event);
        qemuDriverUnlock(driver);
    }

    return 0;
}


/*
 * Since we have the '-no-shutdown' flag set, the
 * QEMU process will currently have guest OS shutdown
 * and the CPUS stopped. To fake the reboot, we thus
 * want todo a reset of the virtual hardware, followed
 * by restart of the CPUs. This should result in the
 * guest OS booting up again
 */
static void
qemuProcessFakeReboot(void *opaque)
{
    virQEMUDriverPtr driver = qemu_driver;
    virDomainObjPtr vm = opaque;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainEventPtr event = NULL;
    int ret = -1;
    VIR_DEBUG("vm=%p", vm);
    qemuDriverLock(driver);
    virDomainObjLock(vm);
    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (!virDomainObjIsActive(vm)) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("guest unexpectedly quit"));
        goto endjob;
    }

    qemuDomainObjEnterMonitorWithDriver(driver, vm);
    if (qemuMonitorSystemReset(priv->mon) < 0) {
        qemuDomainObjExitMonitorWithDriver(driver, vm);
        goto endjob;
    }
    qemuDomainObjExitMonitorWithDriver(driver, vm);

    if (!virDomainObjIsActive(vm)) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("guest unexpectedly quit"));
        goto endjob;
    }

    if (qemuProcessStartCPUs(driver, vm, NULL,
                             VIR_DOMAIN_RUNNING_BOOTED,
                             QEMU_ASYNC_JOB_NONE) < 0) {
        if (virGetLastError() == NULL)
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("resume operation failed"));
        goto endjob;
    }
    priv->gotShutdown = false;
    event = virDomainEventNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_RESUMED,
                                     VIR_DOMAIN_EVENT_RESUMED_UNPAUSED);

    ret = 0;

endjob:
    if (!qemuDomainObjEndJob(driver, vm))
        vm = NULL;

cleanup:
    if (vm) {
        if (ret == -1) {
            ignore_value(qemuProcessKill(driver, vm,
                                         VIR_QEMU_PROCESS_KILL_FORCE));
        }
        if (virObjectUnref(vm))
            virDomainObjUnlock(vm);
    }
    if (event)
        qemuDomainEventQueue(driver, event);
    qemuDriverUnlock(driver);
}


static void
qemuProcessShutdownOrReboot(virQEMUDriverPtr driver,
                            virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;

    if (priv->fakeReboot) {
        qemuDomainSetFakeReboot(driver, vm, false);
        virObjectRef(vm);
        virThread th;
        if (virThreadCreate(&th,
                            false,
                            qemuProcessFakeReboot,
                            vm) < 0) {
            VIR_ERROR(_("Failed to create reboot thread, killing domain"));
            ignore_value(qemuProcessKill(driver, vm,
                                         VIR_QEMU_PROCESS_KILL_NOWAIT));
            virObjectUnref(vm);
        }
    } else {
        ignore_value(qemuProcessKill(driver, vm, VIR_QEMU_PROCESS_KILL_NOWAIT));
    }
}

static int
qemuProcessHandleShutdown(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                          virDomainObjPtr vm)
{
    virQEMUDriverPtr driver = qemu_driver;
    qemuDomainObjPrivatePtr priv;
    virDomainEventPtr event = NULL;

    VIR_DEBUG("vm=%p", vm);

    virDomainObjLock(vm);

    priv = vm->privateData;
    if (priv->gotShutdown) {
        VIR_DEBUG("Ignoring repeated SHUTDOWN event from domain %s",
                  vm->def->name);
        goto unlock;
    } else if (!virDomainObjIsActive(vm)) {
        VIR_DEBUG("Ignoring SHUTDOWN event from inactive domain %s",
                  vm->def->name);
        goto unlock;
    }
    priv->gotShutdown = true;

    VIR_DEBUG("Transitioned guest %s to shutdown state",
              vm->def->name);
    virDomainObjSetState(vm,
                         VIR_DOMAIN_SHUTDOWN,
                         VIR_DOMAIN_SHUTDOWN_UNKNOWN);
    event = virDomainEventNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_SHUTDOWN,
                                     VIR_DOMAIN_EVENT_SHUTDOWN_FINISHED);

    if (virDomainSaveStatus(driver->caps, driver->stateDir, vm) < 0) {
        VIR_WARN("Unable to save status on vm %s after state change",
                 vm->def->name);
    }

    if (priv->agent)
        qemuAgentNotifyEvent(priv->agent, QEMU_AGENT_EVENT_SHUTDOWN);

    qemuProcessShutdownOrReboot(driver, vm);

unlock:
    virDomainObjUnlock(vm);

    if (event) {
        qemuDriverLock(driver);
        qemuDomainEventQueue(driver, event);
        qemuDriverUnlock(driver);
    }

    return 0;
}


static int
qemuProcessHandleStop(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                      virDomainObjPtr vm)
{
    virQEMUDriverPtr driver = qemu_driver;
    virDomainEventPtr event = NULL;

    virDomainObjLock(vm);
    if (virDomainObjGetState(vm, NULL) == VIR_DOMAIN_RUNNING) {
        qemuDomainObjPrivatePtr priv = vm->privateData;

        if (priv->gotShutdown) {
            VIR_DEBUG("Ignoring STOP event after SHUTDOWN");
            goto unlock;
        }

        VIR_DEBUG("Transitioned guest %s to paused state",
                  vm->def->name);

        virDomainObjSetState(vm, VIR_DOMAIN_PAUSED, VIR_DOMAIN_PAUSED_UNKNOWN);
        event = virDomainEventNewFromObj(vm,
                                         VIR_DOMAIN_EVENT_SUSPENDED,
                                         VIR_DOMAIN_EVENT_SUSPENDED_PAUSED);

        VIR_FREE(priv->lockState);
        if (virDomainLockProcessPause(driver->lockManager, vm, &priv->lockState) < 0)
            VIR_WARN("Unable to release lease on %s", vm->def->name);
        VIR_DEBUG("Preserving lock state '%s'", NULLSTR(priv->lockState));

        if (virDomainSaveStatus(driver->caps, driver->stateDir, vm) < 0) {
            VIR_WARN("Unable to save status on vm %s after state change",
                     vm->def->name);
        }
    }

unlock:
    virDomainObjUnlock(vm);

    if (event) {
        qemuDriverLock(driver);
        qemuDomainEventQueue(driver, event);
        qemuDriverUnlock(driver);
    }

    return 0;
}


static int
qemuProcessHandleRTCChange(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                           virDomainObjPtr vm,
                           long long offset)
{
    virQEMUDriverPtr driver = qemu_driver;
    virDomainEventPtr event;

    virDomainObjLock(vm);
    event = virDomainEventRTCChangeNewFromObj(vm, offset);

    if (vm->def->clock.offset == VIR_DOMAIN_CLOCK_OFFSET_VARIABLE)
        vm->def->clock.data.variable.adjustment = offset;

    if (virDomainSaveStatus(driver->caps, driver->stateDir, vm) < 0)
        VIR_WARN("unable to save domain status with RTC change");

    virDomainObjUnlock(vm);

    if (event) {
        qemuDriverLock(driver);
        qemuDomainEventQueue(driver, event);
        qemuDriverUnlock(driver);
    }

    return 0;
}


static int
qemuProcessHandleWatchdog(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                          virDomainObjPtr vm,
                          int action)
{
    virQEMUDriverPtr driver = qemu_driver;
    virDomainEventPtr watchdogEvent = NULL;
    virDomainEventPtr lifecycleEvent = NULL;

    virDomainObjLock(vm);
    watchdogEvent = virDomainEventWatchdogNewFromObj(vm, action);

    if (action == VIR_DOMAIN_EVENT_WATCHDOG_PAUSE &&
        virDomainObjGetState(vm, NULL) == VIR_DOMAIN_RUNNING) {
        qemuDomainObjPrivatePtr priv = vm->privateData;
        VIR_DEBUG("Transitioned guest %s to paused state due to watchdog", vm->def->name);

        virDomainObjSetState(vm, VIR_DOMAIN_PAUSED, VIR_DOMAIN_PAUSED_WATCHDOG);
        lifecycleEvent = virDomainEventNewFromObj(vm,
                                                  VIR_DOMAIN_EVENT_SUSPENDED,
                                                  VIR_DOMAIN_EVENT_SUSPENDED_WATCHDOG);

        VIR_FREE(priv->lockState);
        if (virDomainLockProcessPause(driver->lockManager, vm, &priv->lockState) < 0)
            VIR_WARN("Unable to release lease on %s", vm->def->name);
        VIR_DEBUG("Preserving lock state '%s'", NULLSTR(priv->lockState));

        if (virDomainSaveStatus(driver->caps, driver->stateDir, vm) < 0) {
            VIR_WARN("Unable to save status on vm %s after watchdog event",
                     vm->def->name);
        }
    }

    if (vm->def->watchdog->action == VIR_DOMAIN_WATCHDOG_ACTION_DUMP) {
        struct qemuDomainWatchdogEvent *wdEvent;
        if (VIR_ALLOC(wdEvent) == 0) {
            wdEvent->action = VIR_DOMAIN_WATCHDOG_ACTION_DUMP;
            wdEvent->vm = vm;
            /* Hold an extra reference because we can't allow 'vm' to be
             * deleted before handling watchdog event is finished.
             */
            virObjectRef(vm);
            if (virThreadPoolSendJob(driver->workerPool, 0, wdEvent) < 0) {
                if (!virObjectUnref(vm))
                    vm = NULL;
                VIR_FREE(wdEvent);
            }
        } else {
            virReportOOMError();
        }
    }

    if (vm)
        virDomainObjUnlock(vm);

    if (watchdogEvent || lifecycleEvent) {
        qemuDriverLock(driver);
        if (watchdogEvent)
            qemuDomainEventQueue(driver, watchdogEvent);
        if (lifecycleEvent)
            qemuDomainEventQueue(driver, lifecycleEvent);
        qemuDriverUnlock(driver);
    }

    return 0;
}


static int
qemuProcessHandleIOError(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                         virDomainObjPtr vm,
                         const char *diskAlias,
                         int action,
                         const char *reason)
{
    virQEMUDriverPtr driver = qemu_driver;
    virDomainEventPtr ioErrorEvent = NULL;
    virDomainEventPtr ioErrorEvent2 = NULL;
    virDomainEventPtr lifecycleEvent = NULL;
    const char *srcPath;
    const char *devAlias;
    virDomainDiskDefPtr disk;

    virDomainObjLock(vm);
    disk = qemuProcessFindDomainDiskByAlias(vm, diskAlias);

    if (disk) {
        srcPath = disk->src;
        devAlias = disk->info.alias;
    } else {
        srcPath = "";
        devAlias = "";
    }

    ioErrorEvent = virDomainEventIOErrorNewFromObj(vm, srcPath, devAlias, action);
    ioErrorEvent2 = virDomainEventIOErrorReasonNewFromObj(vm, srcPath, devAlias, action, reason);

    if (action == VIR_DOMAIN_EVENT_IO_ERROR_PAUSE &&
        virDomainObjGetState(vm, NULL) == VIR_DOMAIN_RUNNING) {
        qemuDomainObjPrivatePtr priv = vm->privateData;
        VIR_DEBUG("Transitioned guest %s to paused state due to IO error", vm->def->name);

        virDomainObjSetState(vm, VIR_DOMAIN_PAUSED, VIR_DOMAIN_PAUSED_IOERROR);
        lifecycleEvent = virDomainEventNewFromObj(vm,
                                                  VIR_DOMAIN_EVENT_SUSPENDED,
                                                  VIR_DOMAIN_EVENT_SUSPENDED_IOERROR);

        VIR_FREE(priv->lockState);
        if (virDomainLockProcessPause(driver->lockManager, vm, &priv->lockState) < 0)
            VIR_WARN("Unable to release lease on %s", vm->def->name);
        VIR_DEBUG("Preserving lock state '%s'", NULLSTR(priv->lockState));

        if (virDomainSaveStatus(driver->caps, driver->stateDir, vm) < 0)
            VIR_WARN("Unable to save status on vm %s after IO error", vm->def->name);
    }
    virDomainObjUnlock(vm);

    if (ioErrorEvent || ioErrorEvent2 || lifecycleEvent) {
        qemuDriverLock(driver);
        if (ioErrorEvent)
            qemuDomainEventQueue(driver, ioErrorEvent);
        if (ioErrorEvent2)
            qemuDomainEventQueue(driver, ioErrorEvent2);
        if (lifecycleEvent)
            qemuDomainEventQueue(driver, lifecycleEvent);
        qemuDriverUnlock(driver);
    }

    return 0;
}

static int
qemuProcessHandleBlockJob(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                          virDomainObjPtr vm,
                          const char *diskAlias,
                          int type,
                          int status)
{
    virQEMUDriverPtr driver = qemu_driver;
    virDomainEventPtr event = NULL;
    const char *path;
    virDomainDiskDefPtr disk;

    virDomainObjLock(vm);
    disk = qemuProcessFindDomainDiskByAlias(vm, diskAlias);

    if (disk) {
        path = disk->src;
        event = virDomainEventBlockJobNewFromObj(vm, path, type, status);
        /* XXX If we completed a block pull or commit, then recompute
         * the cached backing chain to match.  Better would be storing
         * the chain ourselves rather than reprobing, but this
         * requires modifying domain_conf and our XML to fully track
         * the chain across libvirtd restarts.  For that matter, if
         * qemu gains support for committing the active layer, we have
         * to update disk->src.  */
        if ((type == VIR_DOMAIN_BLOCK_JOB_TYPE_PULL ||
             type == VIR_DOMAIN_BLOCK_JOB_TYPE_COMMIT) &&
            status == VIR_DOMAIN_BLOCK_JOB_COMPLETED)
            qemuDomainDetermineDiskChain(driver, disk, true);
        if (disk->mirror && type == VIR_DOMAIN_BLOCK_JOB_TYPE_COPY &&
            status == VIR_DOMAIN_BLOCK_JOB_READY)
            disk->mirroring = true;
    }

    virDomainObjUnlock(vm);

    if (event) {
        qemuDriverLock(driver);
        qemuDomainEventQueue(driver, event);
        qemuDriverUnlock(driver);
    }

    return 0;
}

static int
qemuProcessHandleGraphics(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                          virDomainObjPtr vm,
                          int phase,
                          int localFamily,
                          const char *localNode,
                          const char *localService,
                          int remoteFamily,
                          const char *remoteNode,
                          const char *remoteService,
                          const char *authScheme,
                          const char *x509dname,
                          const char *saslUsername)
{
    virQEMUDriverPtr driver = qemu_driver;
    virDomainEventPtr event;
    virDomainEventGraphicsAddressPtr localAddr = NULL;
    virDomainEventGraphicsAddressPtr remoteAddr = NULL;
    virDomainEventGraphicsSubjectPtr subject = NULL;
    int i;

    if (VIR_ALLOC(localAddr) < 0)
        goto no_memory;
    localAddr->family = localFamily;
    if (!(localAddr->service = strdup(localService)) ||
        !(localAddr->node = strdup(localNode)))
        goto no_memory;

    if (VIR_ALLOC(remoteAddr) < 0)
        goto no_memory;
    remoteAddr->family = remoteFamily;
    if (!(remoteAddr->service = strdup(remoteService)) ||
        !(remoteAddr->node = strdup(remoteNode)))
        goto no_memory;

    if (VIR_ALLOC(subject) < 0)
        goto no_memory;
    if (x509dname) {
        if (VIR_REALLOC_N(subject->identities, subject->nidentity+1) < 0)
            goto no_memory;
        subject->nidentity++;
        if (!(subject->identities[subject->nidentity-1].type = strdup("x509dname")) ||
            !(subject->identities[subject->nidentity-1].name = strdup(x509dname)))
            goto no_memory;
    }
    if (saslUsername) {
        if (VIR_REALLOC_N(subject->identities, subject->nidentity+1) < 0)
            goto no_memory;
        subject->nidentity++;
        if (!(subject->identities[subject->nidentity-1].type = strdup("saslUsername")) ||
            !(subject->identities[subject->nidentity-1].name = strdup(saslUsername)))
            goto no_memory;
    }

    virDomainObjLock(vm);
    event = virDomainEventGraphicsNewFromObj(vm, phase, localAddr, remoteAddr, authScheme, subject);
    virDomainObjUnlock(vm);

    if (event) {
        qemuDriverLock(driver);
        qemuDomainEventQueue(driver, event);
        qemuDriverUnlock(driver);
    }

    return 0;

no_memory:
    virReportOOMError();
    if (localAddr) {
        VIR_FREE(localAddr->service);
        VIR_FREE(localAddr->node);
        VIR_FREE(localAddr);
    }
    if (remoteAddr) {
        VIR_FREE(remoteAddr->service);
        VIR_FREE(remoteAddr->node);
        VIR_FREE(remoteAddr);
    }
    if (subject) {
        for (i = 0 ; i < subject->nidentity ; i++) {
            VIR_FREE(subject->identities[i].type);
            VIR_FREE(subject->identities[i].name);
        }
        VIR_FREE(subject->identities);
        VIR_FREE(subject);
    }

    return -1;
}


static void qemuProcessHandleMonitorDestroy(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                                            virDomainObjPtr vm)
{
    virObjectUnref(vm);
}

static int
qemuProcessHandleTrayChange(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                            virDomainObjPtr vm,
                            const char *devAlias,
                            int reason)
{
    virQEMUDriverPtr driver = qemu_driver;
    virDomainEventPtr event = NULL;
    virDomainDiskDefPtr disk;

    virDomainObjLock(vm);
    disk = qemuProcessFindDomainDiskByAlias(vm, devAlias);

    if (disk) {
        event = virDomainEventTrayChangeNewFromObj(vm,
                                                   devAlias,
                                                   reason);
        /* Update disk tray status */
        if (reason == VIR_DOMAIN_EVENT_TRAY_CHANGE_OPEN)
            disk->tray_status = VIR_DOMAIN_DISK_TRAY_OPEN;
        else if (reason == VIR_DOMAIN_EVENT_TRAY_CHANGE_CLOSE)
            disk->tray_status = VIR_DOMAIN_DISK_TRAY_CLOSED;

        if (virDomainSaveStatus(driver->caps, driver->stateDir, vm) < 0) {
            VIR_WARN("Unable to save status on vm %s after tray moved event",
                     vm->def->name);
        }
    }

    virDomainObjUnlock(vm);

    if (event) {
        qemuDriverLock(driver);
        qemuDomainEventQueue(driver, event);
        qemuDriverUnlock(driver);
    }

    return 0;
}

static int
qemuProcessHandlePMWakeup(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                          virDomainObjPtr vm)
{
    virQEMUDriverPtr driver = qemu_driver;
    virDomainEventPtr event = NULL;
    virDomainEventPtr lifecycleEvent = NULL;

    virDomainObjLock(vm);
    event = virDomainEventPMWakeupNewFromObj(vm);

    /* Don't set domain status back to running if it wasn't paused
     * from guest side, otherwise it can just cause confusion.
     */
    if (virDomainObjGetState(vm, NULL) == VIR_DOMAIN_PMSUSPENDED) {
        VIR_DEBUG("Transitioned guest %s from pmsuspended to running "
                  "state due to QMP wakeup event", vm->def->name);

        virDomainObjSetState(vm, VIR_DOMAIN_RUNNING,
                             VIR_DOMAIN_RUNNING_WAKEUP);
        lifecycleEvent = virDomainEventNewFromObj(vm,
                                                  VIR_DOMAIN_EVENT_STARTED,
                                                  VIR_DOMAIN_EVENT_STARTED_WAKEUP);

        if (virDomainSaveStatus(driver->caps, driver->stateDir, vm) < 0) {
            VIR_WARN("Unable to save status on vm %s after wakeup event",
                     vm->def->name);
        }
    }

    virDomainObjUnlock(vm);

    if (event || lifecycleEvent) {
        qemuDriverLock(driver);
        if (event)
            qemuDomainEventQueue(driver, event);
        if (lifecycleEvent)
            qemuDomainEventQueue(driver, lifecycleEvent);
        qemuDriverUnlock(driver);
    }

    return 0;
}

static int
qemuProcessHandlePMSuspend(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                           virDomainObjPtr vm)
{
    virQEMUDriverPtr driver = qemu_driver;
    virDomainEventPtr event = NULL;
    virDomainEventPtr lifecycleEvent = NULL;

    virDomainObjLock(vm);
    event = virDomainEventPMSuspendNewFromObj(vm);

    if (virDomainObjGetState(vm, NULL) == VIR_DOMAIN_RUNNING) {
        qemuDomainObjPrivatePtr priv = vm->privateData;
        VIR_DEBUG("Transitioned guest %s to pmsuspended state due to "
                  "QMP suspend event", vm->def->name);

        virDomainObjSetState(vm, VIR_DOMAIN_PMSUSPENDED,
                             VIR_DOMAIN_PMSUSPENDED_UNKNOWN);
        lifecycleEvent =
            virDomainEventNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_PMSUSPENDED,
                                     VIR_DOMAIN_EVENT_PMSUSPENDED_MEMORY);

        if (virDomainSaveStatus(driver->caps, driver->stateDir, vm) < 0) {
            VIR_WARN("Unable to save status on vm %s after suspend event",
                     vm->def->name);
        }

        if (priv->agent)
            qemuAgentNotifyEvent(priv->agent, QEMU_AGENT_EVENT_SUSPEND);
    }

    virDomainObjUnlock(vm);

    if (event || lifecycleEvent) {
        qemuDriverLock(driver);
        if (event)
            qemuDomainEventQueue(driver, event);
        if (lifecycleEvent)
            qemuDomainEventQueue(driver, lifecycleEvent);
        qemuDriverUnlock(driver);
    }

    return 0;
}

static int
qemuProcessHandleBalloonChange(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                               virDomainObjPtr vm,
                               unsigned long long actual)
{
    virQEMUDriverPtr driver = qemu_driver;
    virDomainEventPtr event;

    virDomainObjLock(vm);
    event = virDomainEventBalloonChangeNewFromObj(vm, actual);

    VIR_DEBUG("Updating balloon from %lld to %lld kb",
              vm->def->mem.cur_balloon, actual);
    vm->def->mem.cur_balloon = actual;

    if (virDomainSaveStatus(driver->caps, driver->stateDir, vm) < 0)
        VIR_WARN("unable to save domain status with balloon change");

    virDomainObjUnlock(vm);

    if (event) {
        qemuDriverLock(driver);
        qemuDomainEventQueue(driver, event);
        qemuDriverUnlock(driver);
    }

    return 0;
}

static int
qemuProcessHandlePMSuspendDisk(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                               virDomainObjPtr vm)
{
    virQEMUDriverPtr driver = qemu_driver;
    virDomainEventPtr event = NULL;
    virDomainEventPtr lifecycleEvent = NULL;

    virDomainObjLock(vm);
    event = virDomainEventPMSuspendDiskNewFromObj(vm);

    if (virDomainObjGetState(vm, NULL) == VIR_DOMAIN_RUNNING) {
        qemuDomainObjPrivatePtr priv = vm->privateData;
        VIR_DEBUG("Transitioned guest %s to pmsuspended state due to "
                  "QMP suspend_disk event", vm->def->name);

        virDomainObjSetState(vm, VIR_DOMAIN_PMSUSPENDED,
                             VIR_DOMAIN_PMSUSPENDED_UNKNOWN);
        lifecycleEvent =
            virDomainEventNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_PMSUSPENDED,
                                     VIR_DOMAIN_EVENT_PMSUSPENDED_DISK);

        if (virDomainSaveStatus(driver->caps, driver->stateDir, vm) < 0) {
            VIR_WARN("Unable to save status on vm %s after suspend event",
                     vm->def->name);
        }

        if (priv->agent)
            qemuAgentNotifyEvent(priv->agent, QEMU_AGENT_EVENT_SUSPEND);
    }

    virDomainObjUnlock(vm);

    if (event || lifecycleEvent) {
        qemuDriverLock(driver);
        if (event)
            qemuDomainEventQueue(driver, event);
        if (lifecycleEvent)
            qemuDomainEventQueue(driver, lifecycleEvent);
        qemuDriverUnlock(driver);
    }

    return 0;
}


static qemuMonitorCallbacks monitorCallbacks = {
    .destroy = qemuProcessHandleMonitorDestroy,
    .eofNotify = qemuProcessHandleMonitorEOF,
    .errorNotify = qemuProcessHandleMonitorError,
    .diskSecretLookup = qemuProcessFindVolumeQcowPassphrase,
    .domainShutdown = qemuProcessHandleShutdown,
    .domainStop = qemuProcessHandleStop,
    .domainReset = qemuProcessHandleReset,
    .domainRTCChange = qemuProcessHandleRTCChange,
    .domainWatchdog = qemuProcessHandleWatchdog,
    .domainIOError = qemuProcessHandleIOError,
    .domainGraphics = qemuProcessHandleGraphics,
    .domainBlockJob = qemuProcessHandleBlockJob,
    .domainTrayChange = qemuProcessHandleTrayChange,
    .domainPMWakeup = qemuProcessHandlePMWakeup,
    .domainPMSuspend = qemuProcessHandlePMSuspend,
    .domainBalloonChange = qemuProcessHandleBalloonChange,
    .domainPMSuspendDisk = qemuProcessHandlePMSuspendDisk,
};

static int
qemuConnectMonitor(virQEMUDriverPtr driver, virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int ret = -1;
    qemuMonitorPtr mon = NULL;

    if (virSecurityManagerSetDaemonSocketLabel(driver->securityManager,
                                               vm->def) < 0) {
        VIR_ERROR(_("Failed to set security context for monitor for %s"),
                  vm->def->name);
        goto error;
    }

    /* Hold an extra reference because we can't allow 'vm' to be
     * deleted while the monitor is active */
    virObjectRef(vm);

    ignore_value(virTimeMillisNow(&priv->monStart));
    virDomainObjUnlock(vm);
    qemuDriverUnlock(driver);

    mon = qemuMonitorOpen(vm,
                          priv->monConfig,
                          priv->monJSON,
                          &monitorCallbacks);

    qemuDriverLock(driver);
    virDomainObjLock(vm);
    priv->monStart = 0;

    if (mon == NULL) {
        virObjectUnref(vm);
    } else if (!virDomainObjIsActive(vm)) {
        qemuMonitorClose(mon);
        mon = NULL;
    }
    priv->mon = mon;

    if (virSecurityManagerClearSocketLabel(driver->securityManager, vm->def) < 0) {
        VIR_ERROR(_("Failed to clear security context for monitor for %s"),
                  vm->def->name);
        goto error;
    }

    if (priv->mon == NULL) {
        VIR_INFO("Failed to connect monitor for %s", vm->def->name);
        goto error;
    }


    qemuDomainObjEnterMonitorWithDriver(driver, vm);
    ret = qemuMonitorSetCapabilities(priv->mon);
    if (ret == 0 &&
        qemuCapsGet(priv->caps, QEMU_CAPS_MONITOR_JSON))
        ret = qemuCapsProbeQMP(priv->caps, priv->mon);
    qemuDomainObjExitMonitorWithDriver(driver, vm);

error:

    return ret;
}

typedef int qemuProcessLogHandleOutput(virDomainObjPtr vm,
                                       const char *output,
                                       int fd);

/*
 * Returns -1 for error, 0 on success
 */
static int
qemuProcessReadLogOutput(virDomainObjPtr vm,
                         int fd,
                         char *buf,
                         size_t buflen,
                         qemuProcessLogHandleOutput func,
                         const char *what,
                         int timeout)
{
    int retries = (timeout*10);
    int got = 0;
    char *debug = NULL;
    int ret = -1;
    char *filter_next = buf;

    buf[0] = '\0';

    /* This relies on log message format generated by virLogFormatString() and
     * might need to be modified when message format changes. */
    if (virAsprintf(&debug, ": %d: debug : ", vm->pid) < 0) {
        virReportOOMError();
        return -1;
    }

    while (retries) {
        ssize_t func_ret, bytes;
        int isdead = 0;
        char *eol;

        func_ret = func(vm, buf, fd);

        if (kill(vm->pid, 0) == -1 && errno == ESRCH)
            isdead = 1;

        /* Any failures should be detected before we read the log, so we
         * always have something useful to report on failure. */
        bytes = saferead(fd, buf+got, buflen-got-1);
        if (bytes < 0) {
            virReportSystemError(errno,
                                 _("Failure while reading %s log output"),
                                 what);
            goto cleanup;
        }

        got += bytes;
        buf[got] = '\0';

        /* Filter out debug messages from intermediate libvirt process */
        while ((eol = strchr(filter_next, '\n'))) {
            *eol = '\0';
            if (strstr(filter_next, debug)) {
                memmove(filter_next, eol + 1, got - (eol - buf));
                got -= eol + 1 - filter_next;
            } else {
                filter_next = eol + 1;
                *eol = '\n';
            }
        }

        if (got == buflen-1) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Out of space while reading %s log output: %s"),
                           what, buf);
            goto cleanup;
        }

        if (isdead) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Process exited while reading %s log output: %s"),
                           what, buf);
            goto cleanup;
        }

        if (func_ret <= 0) {
            ret = func_ret;
            goto cleanup;
        }

        usleep(100*1000);
        retries--;
    }

    virReportError(VIR_ERR_INTERNAL_ERROR,
                   _("Timed out while reading %s log output: %s"),
                   what, buf);

cleanup:
    VIR_FREE(debug);
    return ret;
}


/*
 * Look at a chunk of data from the QEMU stdout logs and try to
 * find a TTY device, as indicated by a line like
 *
 * char device redirected to /dev/pts/3
 *
 * Returns -1 for error, 0 success, 1 continue reading
 */
static int
qemuProcessExtractTTYPath(const char *haystack,
                          size_t *offset,
                          char **path)
{
    static const char needle[] = "char device redirected to";
    char *tmp, *dev;

    VIR_FREE(*path);
    /* First look for our magic string */
    if (!(tmp = strstr(haystack + *offset, needle))) {
        return 1;
    }
    tmp += sizeof(needle);
    dev = tmp;

    /*
     * And look for first whitespace character and nul terminate
     * to mark end of the pty path
     */
    while (*tmp) {
        if (c_isspace(*tmp)) {
            *path = strndup(dev, tmp-dev);
            if (*path == NULL) {
                virReportOOMError();
                return -1;
            }

            /* ... now further update offset till we get EOL */
            *offset = tmp - haystack;
            return 0;
        }
        tmp++;
    }

    /*
     * We found a path, but didn't find any whitespace,
     * so it must be still incomplete - we should at
     * least see a \n - indicate that we want to carry
     * on trying again
     */
    return 1;
}

static int
qemuProcessLookupPTYs(virDomainChrDefPtr *devices,
                      int count,
                      virHashTablePtr paths,
                      bool chardevfmt)
{
    int i;
    const char *prefix = chardevfmt ? "char" : "";

    for (i = 0 ; i < count ; i++) {
        virDomainChrDefPtr chr = devices[i];
        if (chr->source.type == VIR_DOMAIN_CHR_TYPE_PTY) {
            char id[32];
            const char *path;

            if (snprintf(id, sizeof(id), "%s%s",
                         prefix, chr->info.alias) >= sizeof(id))
                return -1;

            path = (const char *) virHashLookup(paths, id);
            if (path == NULL) {
                if (chr->source.data.file.path == NULL) {
                    /* neither the log output nor 'info chardev' had a
                     * pty path for this chardev, report an error
                     */
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   _("no assigned pty for device %s"), id);
                    return -1;
                } else {
                    /* 'info chardev' had no pty path for this chardev,
                     * but the log output had, so we're fine
                     */
                    continue;
                }
            }

            VIR_FREE(chr->source.data.file.path);
            chr->source.data.file.path = strdup(path);

            if (chr->source.data.file.path == NULL) {
                virReportOOMError();
                return -1;
            }
        }
    }

    return 0;
}

static int
qemuProcessFindCharDevicePTYsMonitor(virDomainObjPtr vm,
                                     qemuCapsPtr caps,
                                     virHashTablePtr paths)
{
    bool chardevfmt = qemuCapsGet(caps, QEMU_CAPS_CHARDEV);

    if (qemuProcessLookupPTYs(vm->def->serials, vm->def->nserials,
                              paths, chardevfmt) < 0)
        return -1;

    if (qemuProcessLookupPTYs(vm->def->parallels, vm->def->nparallels,
                              paths, chardevfmt) < 0)
        return -1;

    if (qemuProcessLookupPTYs(vm->def->channels, vm->def->nchannels,
                              paths, chardevfmt) < 0)
        return -1;

    if (qemuProcessLookupPTYs(vm->def->consoles, vm->def->nconsoles,
                              paths, chardevfmt) < 0)
        return -1;

    return 0;
}

static int
qemuProcessFindCharDevicePTYs(virDomainObjPtr vm,
                              const char *output,
                              int fd ATTRIBUTE_UNUSED)
{
    size_t offset = 0;
    int ret, i;

    /* The order in which QEMU prints out the PTY paths is
       the order in which it procsses its serial and parallel
       device args. This code must match that ordering.... */

    /* first comes the serial devices */
    for (i = 0 ; i < vm->def->nserials ; i++) {
        virDomainChrDefPtr chr = vm->def->serials[i];
        if (chr->source.type == VIR_DOMAIN_CHR_TYPE_PTY) {
            if ((ret = qemuProcessExtractTTYPath(output, &offset,
                                                 &chr->source.data.file.path)) != 0)
                return ret;
        }
    }

    /* then the parallel devices */
    for (i = 0 ; i < vm->def->nparallels ; i++) {
        virDomainChrDefPtr chr = vm->def->parallels[i];
        if (chr->source.type == VIR_DOMAIN_CHR_TYPE_PTY) {
            if ((ret = qemuProcessExtractTTYPath(output, &offset,
                                                 &chr->source.data.file.path)) != 0)
                return ret;
        }
    }

    /* then the channel devices */
    for (i = 0 ; i < vm->def->nchannels ; i++) {
        virDomainChrDefPtr chr = vm->def->channels[i];
        if (chr->source.type == VIR_DOMAIN_CHR_TYPE_PTY) {
            if ((ret = qemuProcessExtractTTYPath(output, &offset,
                                                 &chr->source.data.file.path)) != 0)
                return ret;
        }
    }

    for (i = 0 ; i < vm->def->nconsoles ; i++) {
        virDomainChrDefPtr chr = vm->def->consoles[i];
        /* For historical reasons, console[0] can be just an alias
         * for serial[0]; That's why we need to update it as well */
        if (i == 0 && vm->def->nserials &&
            chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE &&
            chr->targetType == VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SERIAL) {
            if ((ret = virDomainChrSourceDefCopy(&chr->source,
                                                 &((vm->def->serials[0])->source))) != 0)
                return ret;
        } else {
            if (chr->source.type == VIR_DOMAIN_CHR_TYPE_PTY &&
                chr->targetType == VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_VIRTIO) {
                if ((ret = qemuProcessExtractTTYPath(output, &offset,
                                                     &chr->source.data.file.path)) != 0)
                    return ret;
            }
        }
    }

    return 0;
}

static void qemuProcessFreePtyPath(void *payload, const void *name ATTRIBUTE_UNUSED)
{
    VIR_FREE(payload);
}

static void
qemuProcessReadLogFD(int logfd, char *buf, int maxlen, int off)
{
    int ret;
    char *tmpbuf = buf + off;

    ret = saferead(logfd, tmpbuf, maxlen - off - 1);
    if (ret < 0) {
        ret = 0;
    }

    tmpbuf[ret] = '\0';
}


static int
qemuProcessWaitForMonitor(virQEMUDriverPtr driver,
                          virDomainObjPtr vm,
                          qemuCapsPtr caps,
                          off_t pos)
{
    char *buf = NULL;
    size_t buf_size = 4096; /* Plenty of space to get startup greeting */
    int logfd = -1;
    int ret = -1;
    virHashTablePtr paths = NULL;
    qemuDomainObjPrivatePtr priv;

    if (pos != -1) {
        if ((logfd = qemuDomainOpenLog(driver, vm, pos)) < 0)
            return -1;

        if (VIR_ALLOC_N(buf, buf_size) < 0) {
            virReportOOMError();
            goto closelog;
        }

        if (qemuProcessReadLogOutput(vm, logfd, buf, buf_size,
                                     qemuProcessFindCharDevicePTYs,
                                     "console", 30) < 0)
            goto closelog;
    }

    VIR_DEBUG("Connect monitor to %p '%s'", vm, vm->def->name);
    if (qemuConnectMonitor(driver, vm) < 0) {
        goto cleanup;
    }

    /* Try to get the pty path mappings again via the monitor. This is much more
     * reliable if it's available.
     * Note that the monitor itself can be on a pty, so we still need to try the
     * log output method. */
    paths = virHashCreate(0, qemuProcessFreePtyPath);
    if (paths == NULL)
        goto cleanup;

    priv = vm->privateData;
    qemuDomainObjEnterMonitorWithDriver(driver, vm);
    ret = qemuMonitorGetPtyPaths(priv->mon, paths);
    qemuDomainObjExitMonitorWithDriver(driver, vm);

    VIR_DEBUG("qemuMonitorGetPtyPaths returned %i", ret);
    if (ret == 0)
        ret = qemuProcessFindCharDevicePTYsMonitor(vm, caps, paths);

cleanup:
    virHashFree(paths);

    if (pos != -1 && kill(vm->pid, 0) == -1 && errno == ESRCH) {
        /* VM is dead, any other error raised in the interim is probably
         * not as important as the qemu cmdline output */
        qemuProcessReadLogFD(logfd, buf, buf_size, strlen(buf));
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("process exited while connecting to monitor: %s"),
                       buf);
        ret = -1;
    }

closelog:
    if (VIR_CLOSE(logfd) < 0) {
        char ebuf[1024];
        VIR_WARN("Unable to close logfile: %s",
                 virStrerror(errno, ebuf, sizeof(ebuf)));
    }

    VIR_FREE(buf);

    return ret;
}

static int
qemuProcessDetectVcpuPIDs(virQEMUDriverPtr driver,
                          virDomainObjPtr vm)
{
    pid_t *cpupids = NULL;
    int ncpupids;
    qemuDomainObjPrivatePtr priv = vm->privateData;

    qemuDomainObjEnterMonitorWithDriver(driver, vm);
    /* failure to get the VCPU<-> PID mapping or to execute the query
     * command will not be treated fatal as some versions of qemu don't
     * support this command */
    if ((ncpupids = qemuMonitorGetCPUInfo(priv->mon, &cpupids)) <= 0) {
        qemuDomainObjExitMonitorWithDriver(driver, vm);
        virResetLastError();

        priv->nvcpupids = 1;
        if (VIR_ALLOC_N(priv->vcpupids, priv->nvcpupids) < 0) {
            virReportOOMError();
            return -1;
        }
        priv->vcpupids[0] = vm->pid;
        return 0;
    }
    qemuDomainObjExitMonitorWithDriver(driver, vm);

    if (ncpupids != vm->def->vcpus) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("got wrong number of vCPU pids from QEMU monitor. "
                         "got %d, wanted %d"),
                       ncpupids, vm->def->vcpus);
        VIR_FREE(cpupids);
        return -1;
    }

    priv->nvcpupids = ncpupids;
    priv->vcpupids = cpupids;
    return 0;
}


/*
 * Set NUMA memory policy for qemu process, to be run between
 * fork/exec of QEMU only.
 */
#if HAVE_NUMACTL
static int
qemuProcessInitNumaMemoryPolicy(virDomainObjPtr vm,
                                virBitmapPtr nodemask)
{
    nodemask_t mask;
    int mode = -1;
    int node = -1;
    int ret = -1;
    int i = 0;
    int maxnode = 0;
    bool warned = false;
    virDomainNumatuneDef numatune = vm->def->numatune;
    virBitmapPtr tmp_nodemask = NULL;

    if (numatune.memory.placement_mode ==
        VIR_DOMAIN_NUMATUNE_MEM_PLACEMENT_MODE_STATIC) {
        if (!numatune.memory.nodemask)
            return 0;
        VIR_DEBUG("Set NUMA memory policy with specified nodeset");
        tmp_nodemask = numatune.memory.nodemask;
    } else if (numatune.memory.placement_mode ==
               VIR_DOMAIN_NUMATUNE_MEM_PLACEMENT_MODE_AUTO) {
        VIR_DEBUG("Set NUMA memory policy with advisory nodeset from numad");
        tmp_nodemask = nodemask;
    } else {
        return 0;
    }

    if (numa_available() < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Host kernel is not aware of NUMA."));
        return -1;
    }

    maxnode = numa_max_node() + 1;
    /* Convert nodemask to NUMA bitmask. */
    nodemask_zero(&mask);
    i = -1;
    while ((i = virBitmapNextSetBit(tmp_nodemask, i)) >= 0) {
        if (i > NUMA_NUM_NODES) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Host cannot support NUMA node %d"), i);
            return -1;
        }
        if (i > maxnode && !warned) {
            VIR_WARN("nodeset is out of range, there is only %d NUMA "
                     "nodes on host", maxnode);
            warned = true;
        }
        nodemask_set(&mask, i);
    }

    mode = numatune.memory.mode;

    if (mode == VIR_DOMAIN_NUMATUNE_MEM_STRICT) {
        numa_set_bind_policy(1);
        numa_set_membind(&mask);
        numa_set_bind_policy(0);
    } else if (mode == VIR_DOMAIN_NUMATUNE_MEM_PREFERRED) {
        int nnodes = 0;
        for (i = 0; i < NUMA_NUM_NODES; i++) {
            if (nodemask_isset(&mask, i)) {
                node = i;
                nnodes++;
            }
        }

        if (nnodes != 1) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("NUMA memory tuning in 'preferred' mode "
                                   "only supports single node"));
            goto cleanup;
        }

        numa_set_bind_policy(0);
        numa_set_preferred(node);
    } else if (mode == VIR_DOMAIN_NUMATUNE_MEM_INTERLEAVE) {
        numa_set_interleave_mask(&mask);
    } else {
        /* XXX: Shouldn't go here, as we already do checking when
         * parsing domain XML.
         */
        virReportError(VIR_ERR_XML_ERROR,
                       "%s", _("Invalid mode for memory NUMA tuning."));
        goto cleanup;
    }

    ret = 0;

cleanup:
    return ret;
}
#else
static int
qemuProcessInitNumaMemoryPolicy(virDomainObjPtr vm,
                                virBitmapPtr nodemask ATTRIBUTE_UNUSED)
{
    if (vm->def->numatune.memory.nodemask) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("libvirt is compiled without NUMA tuning support"));

        return -1;
    }

    return 0;
}
#endif

#if HAVE_NUMAD
static char *
qemuGetNumadAdvice(virDomainDefPtr def)
{
    virCommandPtr cmd = NULL;
    char *output = NULL;

    cmd = virCommandNewArgList(NUMAD, "-w", NULL);
    virCommandAddArgFormat(cmd, "%d:%llu", def->vcpus,
                           VIR_DIV_UP(def->mem.cur_balloon, 1024));

    virCommandSetOutputBuffer(cmd, &output);

    if (virCommandRun(cmd, NULL) < 0)
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Failed to query numad for the "
                         "advisory nodeset"));

    virCommandFree(cmd);
    return output;
}
#else
static char *
qemuGetNumadAdvice(virDomainDefPtr def ATTRIBUTE_UNUSED)
{
    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                   _("numad is not available on this host"));
    return NULL;
}
#endif

/* Helper to prepare cpumap for affinity setting, convert
 * NUMA nodeset into cpuset if @nodemask is not NULL, otherwise
 * just return a new allocated bitmap.
 */
virBitmapPtr
qemuPrepareCpumap(virQEMUDriverPtr driver,
                  virBitmapPtr nodemask)
{
    int i, hostcpus, maxcpu = QEMUD_CPUMASK_LEN;
    virBitmapPtr cpumap = NULL;

    /* setaffinity fails if you set bits for CPUs which
     * aren't present, so we have to limit ourselves */
    if ((hostcpus = nodeGetCPUCount()) < 0)
        return NULL;

    if (maxcpu > hostcpus)
        maxcpu = hostcpus;

    if (!(cpumap = virBitmapNew(maxcpu))) {
        virReportOOMError();
        return NULL;
    }

    if (nodemask) {
        for (i = 0; i < driver->caps->host.nnumaCell; i++) {
            int j;
            int cur_ncpus = driver->caps->host.numaCell[i]->ncpus;
            bool result;
            if (virBitmapGetBit(nodemask, i, &result) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("Failed to convert nodeset to cpuset"));
                virBitmapFree(cpumap);
                return NULL;
            }
            if (result) {
                for (j = 0; j < cur_ncpus; j++)
                    ignore_value(virBitmapSetBit(cpumap,
                                                 driver->caps->host.numaCell[i]->cpus[j]));
            }
        }
    }

    return cpumap;
}

/*
 * To be run between fork/exec of QEMU only
 */
static int
qemuProcessInitCpuAffinity(virQEMUDriverPtr driver,
                           virDomainObjPtr vm,
                           virBitmapPtr nodemask)
{
    int ret = -1;
    virBitmapPtr cpumap = NULL;
    virBitmapPtr cpumapToSet = NULL;

    if (!(cpumap = qemuPrepareCpumap(driver, nodemask)))
        return -1;

    if (vm->def->placement_mode == VIR_DOMAIN_CPU_PLACEMENT_MODE_AUTO) {
        VIR_DEBUG("Set CPU affinity with advisory nodeset from numad");
        cpumapToSet = cpumap;
    } else {
        VIR_DEBUG("Set CPU affinity with specified cpuset");
        if (vm->def->cpumask) {
            cpumapToSet = vm->def->cpumask;
        } else {
            cpumapToSet = cpumap;
            /* You may think this is redundant, but we can't assume libvirtd
             * itself is running on all pCPUs, so we need to explicitly set
             * the spawned QEMU instance to all pCPUs if no map is given in
             * its config file */
            virBitmapSetAll(cpumap);
        }
    }

    /* We are pressuming we are running between fork/exec of QEMU
     * so use '0' to indicate our own process ID. No threads are
     * running at this point
     */
    if (virProcessInfoSetAffinity(0 /* Self */, cpumapToSet) < 0)
        goto cleanup;

    ret = 0;

cleanup:
    virBitmapFree(cpumap);
    return ret;
}

/* set link states to down on interfaces at qemu start */
static int
qemuProcessSetLinkStates(virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainDefPtr def = vm->def;
    int i;
    int ret = 0;

    for (i = 0; i < def->nnets; i++) {
        if (def->nets[i]->linkstate == VIR_DOMAIN_NET_INTERFACE_LINK_STATE_DOWN) {
            VIR_DEBUG("Setting link state: %s", def->nets[i]->info.alias);

            if (!qemuCapsGet(priv->caps, QEMU_CAPS_NETDEV)) {
                virReportError(VIR_ERR_NO_SUPPORT, "%s",
                               _("Setting of link state is not supported by this qemu"));
                return -1;
            }

            ret = qemuMonitorSetLink(priv->mon,
                                     def->nets[i]->info.alias,
                                     VIR_DOMAIN_NET_INTERFACE_LINK_STATE_DOWN);
            if (ret != 0) {
                virReportError(VIR_ERR_OPERATION_FAILED,
                               _("Couldn't set link state on interface: %s"), def->nets[i]->info.alias);
                break;
            }
        }
    }

    return ret;
}

/* Set CPU affinities for vcpus if vcpupin xml provided. */
static int
qemuProcessSetVcpuAffinites(virConnectPtr conn ATTRIBUTE_UNUSED,
                            virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainDefPtr def = vm->def;
    int vcpu, n;
    int ret = -1;

    if (!def->cputune.nvcpupin)
        return 0;

    if (priv->vcpupids == NULL) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("cpu affinity is not supported"));
        return -1;
    }

    for (n = 0; n < def->cputune.nvcpupin; n++) {
        vcpu = def->cputune.vcpupin[n]->vcpuid;

        if (virProcessInfoSetAffinity(priv->vcpupids[vcpu],
                                      def->cputune.vcpupin[n]->cpumask) < 0) {
            goto cleanup;
        }
    }

    ret = 0;
cleanup:
    return ret;
}

/* Set CPU affinities for emulator threads. */
static int
qemuProcessSetEmulatorAffinites(virConnectPtr conn ATTRIBUTE_UNUSED,
                                virDomainObjPtr vm)
{
    virBitmapPtr cpumask;
    virDomainDefPtr def = vm->def;
    int ret = -1;

    if (def->cputune.emulatorpin)
        cpumask = def->cputune.emulatorpin->cpumask;
    else if (def->cpumask)
        cpumask = def->cpumask;
    else
        return 0;

    ret = virProcessInfoSetAffinity(vm->pid, cpumask);
    return ret;
}

static int
qemuProcessInitPasswords(virConnectPtr conn,
                         virQEMUDriverPtr driver,
                         virDomainObjPtr vm)
{
    int ret = 0;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int i;

    for (i = 0 ; i < vm->def->ngraphics; ++i) {
        virDomainGraphicsDefPtr graphics = vm->def->graphics[i];
        if (graphics->type == VIR_DOMAIN_GRAPHICS_TYPE_VNC) {
            ret = qemuDomainChangeGraphicsPasswords(driver, vm,
                                                    VIR_DOMAIN_GRAPHICS_TYPE_VNC,
                                                    &graphics->data.vnc.auth,
                                                    driver->vncPassword);
        } else if (graphics->type == VIR_DOMAIN_GRAPHICS_TYPE_SPICE) {
            ret = qemuDomainChangeGraphicsPasswords(driver, vm,
                                                    VIR_DOMAIN_GRAPHICS_TYPE_SPICE,
                                                    &graphics->data.spice.auth,
                                                    driver->spicePassword);
        }
    }

    if (ret < 0)
        goto cleanup;

    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        for (i = 0 ; i < vm->def->ndisks ; i++) {
            char *secret;
            size_t secretLen;
            const char *alias;

            if (!vm->def->disks[i]->encryption ||
                !vm->def->disks[i]->src)
                continue;

            if (qemuProcessGetVolumeQcowPassphrase(conn,
                                                   vm->def->disks[i],
                                                   &secret, &secretLen) < 0)
                goto cleanup;

            alias = vm->def->disks[i]->info.alias;
            qemuDomainObjEnterMonitorWithDriver(driver, vm);
            ret = qemuMonitorSetDrivePassphrase(priv->mon, alias, secret);
            VIR_FREE(secret);
            qemuDomainObjExitMonitorWithDriver(driver, vm);
            if (ret < 0)
                goto cleanup;
        }
    }

cleanup:
    return ret;
}


#define QEMU_PCI_VENDOR_INTEL     0x8086
#define QEMU_PCI_VENDOR_LSI_LOGIC 0x1000
#define QEMU_PCI_VENDOR_REDHAT    0x1af4
#define QEMU_PCI_VENDOR_CIRRUS    0x1013
#define QEMU_PCI_VENDOR_REALTEK   0x10ec
#define QEMU_PCI_VENDOR_AMD       0x1022
#define QEMU_PCI_VENDOR_ENSONIQ   0x1274
#define QEMU_PCI_VENDOR_VMWARE    0x15ad
#define QEMU_PCI_VENDOR_QEMU      0x1234

#define QEMU_PCI_PRODUCT_DISK_VIRTIO 0x1001

#define QEMU_PCI_PRODUCT_BALLOON_VIRTIO 0x1002

#define QEMU_PCI_PRODUCT_NIC_NE2K     0x8029
#define QEMU_PCI_PRODUCT_NIC_PCNET    0x2000
#define QEMU_PCI_PRODUCT_NIC_RTL8139  0x8139
#define QEMU_PCI_PRODUCT_NIC_E1000    0x100E
#define QEMU_PCI_PRODUCT_NIC_VIRTIO   0x1000

#define QEMU_PCI_PRODUCT_VGA_CIRRUS 0x00b8
#define QEMU_PCI_PRODUCT_VGA_VMWARE 0x0405
#define QEMU_PCI_PRODUCT_VGA_STDVGA 0x1111

#define QEMU_PCI_PRODUCT_AUDIO_AC97    0x2415
#define QEMU_PCI_PRODUCT_AUDIO_ES1370  0x5000

#define QEMU_PCI_PRODUCT_CONTROLLER_PIIX 0x7010
#define QEMU_PCI_PRODUCT_CONTROLLER_LSI  0x0012

#define QEMU_PCI_PRODUCT_WATCHDOG_I63000ESB 0x25ab

static int
qemuProcessAssignNextPCIAddress(virDomainDeviceInfo *info,
                                int vendor,
                                int product,
                                qemuMonitorPCIAddress *addrs,
                                int naddrs)
{
    int found = 0;
    int i;

    VIR_DEBUG("Look for %x:%x out of %d", vendor, product, naddrs);

    for (i = 0 ; (i < naddrs) && !found; i++) {
        VIR_DEBUG("Maybe %x:%x", addrs[i].vendor, addrs[i].product);
        if (addrs[i].vendor == vendor &&
            addrs[i].product == product) {
            VIR_DEBUG("Match %d", i);
            found = 1;
            break;
        }
    }
    if (!found) {
        return -1;
    }

    /* Blank it out so this device isn't matched again */
    addrs[i].vendor = 0;
    addrs[i].product = 0;

    if (info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE)
        info->type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI;

    if (info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
        info->addr.pci.domain = addrs[i].addr.domain;
        info->addr.pci.bus = addrs[i].addr.bus;
        info->addr.pci.slot = addrs[i].addr.slot;
        info->addr.pci.function = addrs[i].addr.function;
    }

    return 0;
}

static int
qemuProcessGetPCIDiskVendorProduct(virDomainDiskDefPtr def,
                                   unsigned *vendor,
                                   unsigned *product)
{
    switch (def->bus) {
    case VIR_DOMAIN_DISK_BUS_VIRTIO:
        *vendor = QEMU_PCI_VENDOR_REDHAT;
        *product = QEMU_PCI_PRODUCT_DISK_VIRTIO;
        break;

    default:
        return -1;
    }

    return 0;
}

static int
qemuProcessGetPCINetVendorProduct(virDomainNetDefPtr def,
                                  unsigned *vendor,
                                  unsigned *product)
{
    if (!def->model)
        return -1;

    if (STREQ(def->model, "ne2k_pci")) {
        *vendor = QEMU_PCI_VENDOR_REALTEK;
        *product = QEMU_PCI_PRODUCT_NIC_NE2K;
    } else if (STREQ(def->model, "pcnet")) {
        *vendor = QEMU_PCI_VENDOR_AMD;
        *product = QEMU_PCI_PRODUCT_NIC_PCNET;
    } else if (STREQ(def->model, "rtl8139")) {
        *vendor = QEMU_PCI_VENDOR_REALTEK;
        *product = QEMU_PCI_PRODUCT_NIC_RTL8139;
    } else if (STREQ(def->model, "e1000")) {
        *vendor = QEMU_PCI_VENDOR_INTEL;
        *product = QEMU_PCI_PRODUCT_NIC_E1000;
    } else if (STREQ(def->model, "virtio")) {
        *vendor = QEMU_PCI_VENDOR_REDHAT;
        *product = QEMU_PCI_PRODUCT_NIC_VIRTIO;
    } else {
        VIR_INFO("Unexpected NIC model %s, cannot get PCI address",
                 def->model);
        return -1;
    }
    return 0;
}

static int
qemuProcessGetPCIControllerVendorProduct(virDomainControllerDefPtr def,
                                         unsigned *vendor,
                                         unsigned *product)
{
    switch (def->type) {
    case VIR_DOMAIN_CONTROLLER_TYPE_SCSI:
        *vendor = QEMU_PCI_VENDOR_LSI_LOGIC;
        *product = QEMU_PCI_PRODUCT_CONTROLLER_LSI;
        break;

    case VIR_DOMAIN_CONTROLLER_TYPE_FDC:
        /* XXX we could put in the ISA bridge address, but
           that's not technically the FDC's address */
        return -1;

    case VIR_DOMAIN_CONTROLLER_TYPE_IDE:
        *vendor = QEMU_PCI_VENDOR_INTEL;
        *product = QEMU_PCI_PRODUCT_CONTROLLER_PIIX;
        break;

    default:
        VIR_INFO("Unexpected controller type %s, cannot get PCI address",
                 virDomainControllerTypeToString(def->type));
        return -1;
    }

    return 0;
}

static int
qemuProcessGetPCIVideoVendorProduct(virDomainVideoDefPtr def,
                                    unsigned *vendor,
                                    unsigned *product)
{
    switch (def->type) {
    case VIR_DOMAIN_VIDEO_TYPE_CIRRUS:
        *vendor = QEMU_PCI_VENDOR_CIRRUS;
        *product = QEMU_PCI_PRODUCT_VGA_CIRRUS;
        break;

    case VIR_DOMAIN_VIDEO_TYPE_VGA:
        *vendor = QEMU_PCI_VENDOR_QEMU;
        *product = QEMU_PCI_PRODUCT_VGA_STDVGA;
        break;

    case VIR_DOMAIN_VIDEO_TYPE_VMVGA:
        *vendor = QEMU_PCI_VENDOR_VMWARE;
        *product = QEMU_PCI_PRODUCT_VGA_VMWARE;
        break;

    default:
        return -1;
    }
    return 0;
}

static int
qemuProcessGetPCISoundVendorProduct(virDomainSoundDefPtr def,
                                    unsigned *vendor,
                                    unsigned *product)
{
    switch (def->model) {
    case VIR_DOMAIN_SOUND_MODEL_ES1370:
        *vendor = QEMU_PCI_VENDOR_ENSONIQ;
        *product = QEMU_PCI_PRODUCT_AUDIO_ES1370;
        break;

    case VIR_DOMAIN_SOUND_MODEL_AC97:
        *vendor = QEMU_PCI_VENDOR_INTEL;
        *product = QEMU_PCI_PRODUCT_AUDIO_AC97;
        break;

    default:
        return -1;
    }

    return 0;
}

static int
qemuProcessGetPCIWatchdogVendorProduct(virDomainWatchdogDefPtr def,
                                       unsigned *vendor,
                                       unsigned *product)
{
    switch (def->model) {
    case VIR_DOMAIN_WATCHDOG_MODEL_I6300ESB:
        *vendor = QEMU_PCI_VENDOR_INTEL;
        *product = QEMU_PCI_PRODUCT_WATCHDOG_I63000ESB;
        break;

    default:
        return -1;
    }

    return 0;
}


static int
qemuProcessGetPCIMemballoonVendorProduct(virDomainMemballoonDefPtr def,
                                         unsigned *vendor,
                                         unsigned *product)
{
    switch (def->model) {
    case VIR_DOMAIN_MEMBALLOON_MODEL_VIRTIO:
        *vendor = QEMU_PCI_VENDOR_REDHAT;
        *product = QEMU_PCI_PRODUCT_BALLOON_VIRTIO;
        break;

    default:
        return -1;
    }

    return 0;
}


/*
 * This entire method assumes that PCI devices in 'info pci'
 * match ordering of devices specified on the command line
 * wrt to devices of matching vendor+product
 *
 * XXXX this might not be a valid assumption if we assign
 * some static addrs on CLI. Have to check that...
 */
static int
qemuProcessDetectPCIAddresses(virDomainObjPtr vm,
                              qemuMonitorPCIAddress *addrs,
                              int naddrs)
{
    unsigned int vendor = 0, product = 0;
    int i;

    /* XXX should all these vendor/product IDs be kept in the
     * actual device data structure instead ?
     */

    for (i = 0 ; i < vm->def->ndisks ; i++) {
        if (qemuProcessGetPCIDiskVendorProduct(vm->def->disks[i], &vendor, &product) < 0)
            continue;

        if (qemuProcessAssignNextPCIAddress(&(vm->def->disks[i]->info),
                                            vendor, product,
                                            addrs, naddrs) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("cannot find PCI address for VirtIO disk %s"),
                           vm->def->disks[i]->dst);
            return -1;
        }
    }

    for (i = 0 ; i < vm->def->nnets ; i++) {
        if (qemuProcessGetPCINetVendorProduct(vm->def->nets[i], &vendor, &product) < 0)
            continue;

        if (qemuProcessAssignNextPCIAddress(&(vm->def->nets[i]->info),
                                            vendor, product,
                                            addrs,  naddrs) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("cannot find PCI address for %s NIC"),
                           vm->def->nets[i]->model);
            return -1;
        }
    }

    for (i = 0 ; i < vm->def->ncontrollers ; i++) {
        if (qemuProcessGetPCIControllerVendorProduct(vm->def->controllers[i], &vendor, &product) < 0)
            continue;

        if (qemuProcessAssignNextPCIAddress(&(vm->def->controllers[i]->info),
                                            vendor, product,
                                            addrs,  naddrs) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("cannot find PCI address for controller %s"),
                           virDomainControllerTypeToString(vm->def->controllers[i]->type));
            return -1;
        }
    }

    for (i = 0 ; i < vm->def->nvideos ; i++) {
        if (qemuProcessGetPCIVideoVendorProduct(vm->def->videos[i], &vendor, &product) < 0)
            continue;

        if (qemuProcessAssignNextPCIAddress(&(vm->def->videos[i]->info),
                                            vendor, product,
                                            addrs,  naddrs) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("cannot find PCI address for video adapter %s"),
                           virDomainVideoTypeToString(vm->def->videos[i]->type));
            return -1;
        }
    }

    for (i = 0 ; i < vm->def->nsounds ; i++) {
        if (qemuProcessGetPCISoundVendorProduct(vm->def->sounds[i], &vendor, &product) < 0)
            continue;

        if (qemuProcessAssignNextPCIAddress(&(vm->def->sounds[i]->info),
                                    vendor, product,
                                     addrs,  naddrs) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("cannot find PCI address for sound adapter %s"),
                           virDomainSoundModelTypeToString(vm->def->sounds[i]->model));
            return -1;
        }
    }


    if (vm->def->watchdog &&
        qemuProcessGetPCIWatchdogVendorProduct(vm->def->watchdog, &vendor, &product) == 0) {
        if (qemuProcessAssignNextPCIAddress(&(vm->def->watchdog->info),
                                            vendor, product,
                                            addrs,  naddrs) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("cannot find PCI address for watchdog %s"),
                           virDomainWatchdogModelTypeToString(vm->def->watchdog->model));
            return -1;
        }
    }

    if (vm->def->memballoon &&
        qemuProcessGetPCIMemballoonVendorProduct(vm->def->memballoon, &vendor, &product) == 0) {
        if (qemuProcessAssignNextPCIAddress(&(vm->def->memballoon->info),
                                            vendor, product,
                                            addrs, naddrs) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("cannot find PCI address for balloon %s"),
                           virDomainMemballoonModelTypeToString(vm->def->memballoon->model));
            return -1;
        }
    }

    /* XXX console (virtio) */


    /* ... and now things we don't have in our xml */

    /* XXX USB controller ? */

    /* XXX what about other PCI devices (ie bridges) */

    return 0;
}

static int
qemuProcessInitPCIAddresses(virQEMUDriverPtr driver,
                            virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int naddrs;
    int ret;
    qemuMonitorPCIAddress *addrs = NULL;

    qemuDomainObjEnterMonitorWithDriver(driver, vm);
    naddrs = qemuMonitorGetAllPCIAddresses(priv->mon,
                                           &addrs);
    qemuDomainObjExitMonitorWithDriver(driver, vm);

    ret = qemuProcessDetectPCIAddresses(vm, addrs, naddrs);

    VIR_FREE(addrs);

    return ret;
}


static int qemuProcessNextFreePort(virQEMUDriverPtr driver,
                                   int startPort)
{
    int i;

    for (i = startPort ; i < driver->remotePortMax; i++) {
        int fd;
        int reuse = 1;
        struct sockaddr_in addr;
        bool used = false;

        if (virBitmapGetBit(driver->reservedRemotePorts,
                            i - driver->remotePortMin, &used) < 0)
            VIR_DEBUG("virBitmapGetBit failed on bit %d", i - driver->remotePortMin);

        if (used)
            continue;

        addr.sin_family = AF_INET;
        addr.sin_port = htons(i);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        fd = socket(PF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;

        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void*)&reuse, sizeof(reuse)) < 0) {
            VIR_FORCE_CLOSE(fd);
            break;
        }

        if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            /* Not in use, lets grab it */
            VIR_FORCE_CLOSE(fd);
            /* Add port to bitmap of reserved ports */
            if (virBitmapSetBit(driver->reservedRemotePorts,
                                i - driver->remotePortMin) < 0) {
                VIR_DEBUG("virBitmapSetBit failed on bit %d",
                          i - driver->remotePortMin);
            }
            return i;
        }
        VIR_FORCE_CLOSE(fd);

        if (errno == EADDRINUSE) {
            /* In use, try next */
            continue;
        }
        /* Some other bad failure, get out.. */
        break;
    }
    return -1;
}


static void
qemuProcessReturnPort(virQEMUDriverPtr driver,
                      int port)
{
    if (port < driver->remotePortMin)
        return;

    if (virBitmapClearBit(driver->reservedRemotePorts,
                          port - driver->remotePortMin) < 0)
        VIR_DEBUG("Could not mark port %d as unused", port);
}


static int
qemuProcessPrepareChardevDevice(virDomainDefPtr def ATTRIBUTE_UNUSED,
                                virDomainChrDefPtr dev,
                                void *opaque ATTRIBUTE_UNUSED)
{
    int fd;
    if (dev->source.type != VIR_DOMAIN_CHR_TYPE_FILE)
        return 0;

    if ((fd = open(dev->source.data.file.path,
                   O_CREAT | O_APPEND, S_IRUSR|S_IWUSR)) < 0) {
        virReportSystemError(errno,
                             _("Unable to pre-create chardev file '%s'"),
                             dev->source.data.file.path);
        return -1;
    }

    VIR_FORCE_CLOSE(fd);

    return 0;
}


static int
qemuProcessLimits(virQEMUDriverPtr driver)
{
    struct rlimit rlim;

    if (driver->maxProcesses > 0) {
        rlim.rlim_cur = rlim.rlim_max = driver->maxProcesses;
        if (setrlimit(RLIMIT_NPROC, &rlim) < 0) {
            virReportSystemError(errno,
                                 _("cannot limit number of processes to %d"),
                                 driver->maxProcesses);
            return -1;
        }
    }

    if (driver->maxFiles > 0) {
        /* Max number of opened files is one greater than
         * actual limit. See man setrlimit */
        rlim.rlim_cur = rlim.rlim_max = driver->maxFiles + 1;
        if (setrlimit(RLIMIT_NOFILE, &rlim) < 0) {
            virReportSystemError(errno,
                                 _("cannot set max opened files to %d"),
                                 driver->maxFiles);
            return -1;
        }
    }

    return 0;
}


struct qemuProcessHookData {
    virConnectPtr conn;
    virDomainObjPtr vm;
    virQEMUDriverPtr driver;
    virBitmapPtr nodemask;
};

static int qemuProcessHook(void *data)
{
    struct qemuProcessHookData *h = data;
    int ret = -1;
    int fd;

    /* Some later calls want pid present */
    h->vm->pid = getpid();

    VIR_DEBUG("Obtaining domain lock");
    /*
     * Since we're going to leak the returned FD to QEMU,
     * we need to make sure it gets a sensible label.
     * This mildly sucks, because there could be other
     * sockets the lock driver opens that we don't want
     * labelled. So far we're ok though.
     */
    if (virSecurityManagerSetSocketLabel(h->driver->securityManager, h->vm->def) < 0)
        goto cleanup;
    if (virDomainLockProcessStart(h->driver->lockManager,
                                  h->driver->uri,
                                  h->vm,
                                  /* QEMU is always paused initially */
                                  true,
                                  &fd) < 0)
        goto cleanup;
    if (virSecurityManagerClearSocketLabel(h->driver->securityManager, h->vm->def) < 0)
        goto cleanup;

    if (qemuProcessLimits(h->driver) < 0)
        goto cleanup;

    /* This must take place before exec(), so that all QEMU
     * memory allocation is on the correct NUMA node
     */
    VIR_DEBUG("Moving process to cgroup");
    if (qemuAddToCgroup(h->driver, h->vm->def) < 0)
        goto cleanup;

    /* This must be done after cgroup placement to avoid resetting CPU
     * affinity */
    if (!h->vm->def->cputune.emulatorpin &&
        qemuProcessInitCpuAffinity(h->driver, h->vm, h->nodemask) < 0)
        goto cleanup;

    if (qemuProcessInitNumaMemoryPolicy(h->vm, h->nodemask) < 0)
        goto cleanup;

    VIR_DEBUG("Setting up security labelling");
    if (virSecurityManagerSetProcessLabel(h->driver->securityManager, h->vm->def) < 0)
        goto cleanup;

    ret = 0;

cleanup:
    VIR_DEBUG("Hook complete ret=%d", ret);
    return ret;
}

int
qemuProcessPrepareMonitorChr(virQEMUDriverPtr driver,
                             virDomainChrSourceDefPtr monConfig,
                             const char *vm)
{
    monConfig->type = VIR_DOMAIN_CHR_TYPE_UNIX;
    monConfig->data.nix.listen = true;

    if (virAsprintf(&monConfig->data.nix.path, "%s/%s.monitor",
                    driver->libDir, vm) < 0) {
        virReportOOMError();
        return -1;
    }

    return 0;
}


/*
 * Precondition: Both driver and vm must be locked,
 * and a job must be active. This method will call
 * {Enter,Exit}MonitorWithDriver
 */
int
qemuProcessStartCPUs(virQEMUDriverPtr driver, virDomainObjPtr vm,
                     virConnectPtr conn, virDomainRunningReason reason,
                     enum qemuDomainAsyncJob asyncJob)
{
    int ret;
    qemuDomainObjPrivatePtr priv = vm->privateData;

    VIR_DEBUG("Using lock state '%s'", NULLSTR(priv->lockState));
    if (virDomainLockProcessResume(driver->lockManager, driver->uri,
                                   vm, priv->lockState) < 0) {
        /* Don't free priv->lockState on error, because we need
         * to make sure we have state still present if the user
         * tries to resume again
         */
        return -1;
    }
    VIR_FREE(priv->lockState);

    ret = qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob);
    if (ret == 0) {
        ret = qemuMonitorStartCPUs(priv->mon, conn);
        qemuDomainObjExitMonitorWithDriver(driver, vm);
    }

    if (ret == 0) {
        virDomainObjSetState(vm, VIR_DOMAIN_RUNNING, reason);
    } else {
        if (virDomainLockProcessPause(driver->lockManager, vm, &priv->lockState) < 0)
            VIR_WARN("Unable to release lease on %s", vm->def->name);
        VIR_DEBUG("Preserving lock state '%s'", NULLSTR(priv->lockState));
    }

    return ret;
}


int qemuProcessStopCPUs(virQEMUDriverPtr driver, virDomainObjPtr vm,
                        virDomainPausedReason reason,
                        enum qemuDomainAsyncJob asyncJob)
{
    int ret;
    qemuDomainObjPrivatePtr priv = vm->privateData;

    VIR_FREE(priv->lockState);

    ret = qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob);
    if (ret == 0) {
        ret = qemuMonitorStopCPUs(priv->mon);
        qemuDomainObjExitMonitorWithDriver(driver, vm);
    }

    if (ret == 0) {
        virDomainObjSetState(vm, VIR_DOMAIN_PAUSED, reason);
        if (virDomainLockProcessPause(driver->lockManager, vm, &priv->lockState) < 0)
            VIR_WARN("Unable to release lease on %s", vm->def->name);
        VIR_DEBUG("Preserving lock state '%s'", NULLSTR(priv->lockState));
    }

    return ret;
}



static int
qemuProcessNotifyNets(virDomainDefPtr def)
{
    int ii;

    for (ii = 0 ; ii < def->nnets ; ii++) {
        virDomainNetDefPtr net = def->nets[ii];
        if (networkNotifyActualDevice(net) < 0)
            return -1;
    }
    return 0;
}

static int
qemuProcessFiltersInstantiate(virConnectPtr conn,
                              virDomainDefPtr def)
{
    int err = 0;
    int i;

    if (!conn)
        return 1;

    for (i = 0 ; i < def->nnets ; i++) {
        virDomainNetDefPtr net = def->nets[i];
        if ((net->filter) && (net->ifname)) {
           if (virDomainConfNWFilterInstantiate(conn, def->uuid, net) < 0) {
                err = 1;
                break;
            }
        }
    }

    return err;
}

static int
qemuProcessUpdateState(virQEMUDriverPtr driver, virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainState state;
    virDomainPausedReason reason;
    virDomainState newState = VIR_DOMAIN_NOSTATE;
    int newReason;
    bool running;
    char *msg = NULL;
    int ret;

    qemuDomainObjEnterMonitorWithDriver(driver, vm);
    ret = qemuMonitorGetStatus(priv->mon, &running, &reason);
    qemuDomainObjExitMonitorWithDriver(driver, vm);

    if (ret < 0 || !virDomainObjIsActive(vm))
        return -1;

    state = virDomainObjGetState(vm, NULL);

    if (state == VIR_DOMAIN_PAUSED && running) {
        newState = VIR_DOMAIN_RUNNING;
        newReason = VIR_DOMAIN_RUNNING_UNPAUSED;
        msg = strdup("was unpaused");
    } else if (state == VIR_DOMAIN_RUNNING && !running) {
        if (reason == VIR_DOMAIN_PAUSED_SHUTTING_DOWN) {
            newState = VIR_DOMAIN_SHUTDOWN;
            newReason = VIR_DOMAIN_SHUTDOWN_UNKNOWN;
            msg = strdup("shutdown");
        } else {
            newState = VIR_DOMAIN_PAUSED;
            newReason = reason;
            ignore_value(virAsprintf(&msg, "was paused (%s)",
                                 virDomainPausedReasonTypeToString(reason)));
        }
    } else if (state == VIR_DOMAIN_SHUTOFF && running) {
        newState = VIR_DOMAIN_RUNNING;
        newReason = VIR_DOMAIN_RUNNING_BOOTED;
        msg = strdup("finished booting");
    }

    if (newState != VIR_DOMAIN_NOSTATE) {
        if (!msg) {
            virReportOOMError();
            return -1;
        }

        VIR_DEBUG("Domain %s %s while its monitor was disconnected;"
                  " changing state to %s (%s)",
                  vm->def->name,
                  msg,
                  virDomainStateTypeToString(newState),
                  virDomainStateReasonToString(newState, newReason));
        VIR_FREE(msg);
        virDomainObjSetState(vm, newState, newReason);
    }

    return 0;
}

static int
qemuProcessRecoverMigration(virQEMUDriverPtr driver,
                            virDomainObjPtr vm,
                            virConnectPtr conn,
                            enum qemuDomainAsyncJob job,
                            enum qemuMigrationJobPhase phase,
                            virDomainState state,
                            int reason)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;

    if (job == QEMU_ASYNC_JOB_MIGRATION_IN) {
        switch (phase) {
        case QEMU_MIGRATION_PHASE_NONE:
        case QEMU_MIGRATION_PHASE_PERFORM2:
        case QEMU_MIGRATION_PHASE_BEGIN3:
        case QEMU_MIGRATION_PHASE_PERFORM3:
        case QEMU_MIGRATION_PHASE_PERFORM3_DONE:
        case QEMU_MIGRATION_PHASE_CONFIRM3_CANCELLED:
        case QEMU_MIGRATION_PHASE_CONFIRM3:
        case QEMU_MIGRATION_PHASE_LAST:
            break;

        case QEMU_MIGRATION_PHASE_PREPARE:
            VIR_DEBUG("Killing unfinished incoming migration for domain %s",
                      vm->def->name);
            return -1;

        case QEMU_MIGRATION_PHASE_FINISH2:
            /* source domain is already killed so let's just resume the domain
             * and hope we are all set */
            VIR_DEBUG("Incoming migration finished, resuming domain %s",
                      vm->def->name);
            if (qemuProcessStartCPUs(driver, vm, conn,
                                     VIR_DOMAIN_RUNNING_UNPAUSED,
                                     QEMU_ASYNC_JOB_NONE) < 0) {
                VIR_WARN("Could not resume domain %s", vm->def->name);
            }
            break;

        case QEMU_MIGRATION_PHASE_FINISH3:
            /* migration finished, we started resuming the domain but didn't
             * confirm success or failure yet; killing it seems safest */
            VIR_DEBUG("Killing migrated domain %s", vm->def->name);
            return -1;
        }
    } else if (job == QEMU_ASYNC_JOB_MIGRATION_OUT) {
        switch (phase) {
        case QEMU_MIGRATION_PHASE_NONE:
        case QEMU_MIGRATION_PHASE_PREPARE:
        case QEMU_MIGRATION_PHASE_FINISH2:
        case QEMU_MIGRATION_PHASE_FINISH3:
        case QEMU_MIGRATION_PHASE_LAST:
            break;

        case QEMU_MIGRATION_PHASE_BEGIN3:
            /* nothing happen so far, just forget we were about to migrate the
             * domain */
            break;

        case QEMU_MIGRATION_PHASE_PERFORM2:
        case QEMU_MIGRATION_PHASE_PERFORM3:
            /* migration is still in progress, let's cancel it and resume the
             * domain */
            VIR_DEBUG("Canceling unfinished outgoing migration of domain %s",
                      vm->def->name);
            qemuDomainObjEnterMonitor(driver, vm);
            ignore_value(qemuMonitorMigrateCancel(priv->mon));
            qemuDomainObjExitMonitor(driver, vm);
            /* resume the domain but only if it was paused as a result of
             * migration */
            if (state == VIR_DOMAIN_PAUSED &&
                (reason == VIR_DOMAIN_PAUSED_MIGRATION ||
                 reason == VIR_DOMAIN_PAUSED_UNKNOWN)) {
                if (qemuProcessStartCPUs(driver, vm, conn,
                                         VIR_DOMAIN_RUNNING_UNPAUSED,
                                         QEMU_ASYNC_JOB_NONE) < 0) {
                    VIR_WARN("Could not resume domain %s", vm->def->name);
                }
            }
            break;

        case QEMU_MIGRATION_PHASE_PERFORM3_DONE:
            /* migration finished but we didn't have a chance to get the result
             * of Finish3 step; third party needs to check what to do next
             */
            break;

        case QEMU_MIGRATION_PHASE_CONFIRM3_CANCELLED:
            /* Finish3 failed, we need to resume the domain */
            VIR_DEBUG("Resuming domain %s after failed migration",
                      vm->def->name);
            if (state == VIR_DOMAIN_PAUSED &&
                (reason == VIR_DOMAIN_PAUSED_MIGRATION ||
                 reason == VIR_DOMAIN_PAUSED_UNKNOWN)) {
                if (qemuProcessStartCPUs(driver, vm, conn,
                                         VIR_DOMAIN_RUNNING_UNPAUSED,
                                         QEMU_ASYNC_JOB_NONE) < 0) {
                    VIR_WARN("Could not resume domain %s", vm->def->name);
                }
            }
            break;

        case QEMU_MIGRATION_PHASE_CONFIRM3:
            /* migration completed, we need to kill the domain here */
            return -1;
        }
    }

    return 0;
}

static int
qemuProcessRecoverJob(virQEMUDriverPtr driver,
                      virDomainObjPtr vm,
                      virConnectPtr conn,
                      const struct qemuDomainJobObj *job)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainState state;
    int reason;

    state = virDomainObjGetState(vm, &reason);

    switch (job->asyncJob) {
    case QEMU_ASYNC_JOB_MIGRATION_OUT:
    case QEMU_ASYNC_JOB_MIGRATION_IN:
        if (qemuProcessRecoverMigration(driver, vm, conn, job->asyncJob,
                                        job->phase, state, reason) < 0)
            return -1;
        break;

    case QEMU_ASYNC_JOB_SAVE:
    case QEMU_ASYNC_JOB_DUMP:
    case QEMU_ASYNC_JOB_SNAPSHOT:
        qemuDomainObjEnterMonitor(driver, vm);
        ignore_value(qemuMonitorMigrateCancel(priv->mon));
        qemuDomainObjExitMonitor(driver, vm);
        /* resume the domain but only if it was paused as a result of
         * running a migration-to-file operation.  Although we are
         * recovering an async job, this function is run at startup
         * and must resume things using sync monitor connections.  */
         if (state == VIR_DOMAIN_PAUSED &&
             ((job->asyncJob == QEMU_ASYNC_JOB_DUMP &&
               reason == VIR_DOMAIN_PAUSED_DUMP) ||
              (job->asyncJob == QEMU_ASYNC_JOB_SAVE &&
               reason == VIR_DOMAIN_PAUSED_SAVE) ||
              (job->asyncJob == QEMU_ASYNC_JOB_SNAPSHOT &&
               reason == VIR_DOMAIN_PAUSED_SNAPSHOT) ||
              reason == VIR_DOMAIN_PAUSED_UNKNOWN)) {
             if (qemuProcessStartCPUs(driver, vm, conn,
                                      VIR_DOMAIN_RUNNING_UNPAUSED,
                                      QEMU_ASYNC_JOB_NONE) < 0) {
                 VIR_WARN("Could not resume domain '%s' after migration to file",
                          vm->def->name);
            }
        }
        break;

    case QEMU_ASYNC_JOB_NONE:
    case QEMU_ASYNC_JOB_LAST:
        break;
    }

    if (!virDomainObjIsActive(vm))
        return -1;

    /* In case any special handling is added for job type that has been ignored
     * before, QEMU_DOMAIN_TRACK_JOBS (from qemu_domain.h) needs to be updated
     * for the job to be properly tracked in domain state XML.
     */
    switch (job->active) {
    case QEMU_JOB_QUERY:
        /* harmless */
        break;

    case QEMU_JOB_DESTROY:
        VIR_DEBUG("Domain %s should have already been destroyed",
                  vm->def->name);
        return -1;

    case QEMU_JOB_SUSPEND:
        /* mostly harmless */
        break;

    case QEMU_JOB_MODIFY:
        /* XXX depending on the command we may be in an inconsistent state and
         * we should probably fall back to "monitor error" state and refuse to
         */
        break;

    case QEMU_JOB_MIGRATION_OP:
    case QEMU_JOB_ABORT:
    case QEMU_JOB_ASYNC:
    case QEMU_JOB_ASYNC_NESTED:
        /* async job was already handled above */
    case QEMU_JOB_NONE:
    case QEMU_JOB_LAST:
        break;
    }

    return 0;
}

struct qemuProcessReconnectData {
    virConnectPtr conn;
    virQEMUDriverPtr driver;
    void *payload;
    struct qemuDomainJobObj oldjob;
};
/*
 * Open an existing VM's monitor, re-detect VCPU threads
 * and re-reserve the security labels in use
 *
 * We own the virConnectPtr we are passed here - whoever started
 * this thread function has increased the reference counter to it
 * so that we now have to close it.
 */
static void
qemuProcessReconnect(void *opaque)
{
    struct qemuProcessReconnectData *data = opaque;
    virQEMUDriverPtr driver = data->driver;
    virDomainObjPtr obj = data->payload;
    qemuDomainObjPrivatePtr priv;
    virConnectPtr conn = data->conn;
    struct qemuDomainJobObj oldjob;
    int state;
    int reason;

    memcpy(&oldjob, &data->oldjob, sizeof(oldjob));

    VIR_FREE(data);

    qemuDriverLock(driver);
    virDomainObjLock(obj);


    VIR_DEBUG("Reconnect monitor to %p '%s'", obj, obj->def->name);

    priv = obj->privateData;

    /* Job was started by the caller for us */
    qemuDomainObjTransferJob(obj);

    /* Hold an extra reference because we can't allow 'vm' to be
     * deleted if qemuConnectMonitor() failed */
    virObjectRef(obj);

    /* XXX check PID liveliness & EXE path */
    if (qemuConnectMonitor(driver, obj) < 0)
        goto error;

    /* Failure to connect to agent shouldn't be fatal */
    if (qemuConnectAgent(driver, obj) < 0) {
        VIR_WARN("Cannot connect to QEMU guest agent for %s",
                 obj->def->name);
        virResetLastError();
        priv->agentError = true;
    }

    if (qemuUpdateActivePciHostdevs(driver, obj->def) < 0) {
        goto error;
    }

    if (qemuUpdateActiveUsbHostdevs(driver, obj->def) < 0)
        goto error;

    if (qemuProcessUpdateState(driver, obj) < 0)
        goto error;

    state = virDomainObjGetState(obj, &reason);
    if (state == VIR_DOMAIN_SHUTOFF) {
        VIR_DEBUG("Domain '%s' wasn't fully started yet, killing it",
                  obj->def->name);
        goto error;
    }

    /* If upgrading from old libvirtd we won't have found any
     * caps in the domain status, so re-query them
     */
    if (!priv->caps &&
        !(priv->caps = qemuCapsCacheLookupCopy(driver->capsCache,
                                               obj->def->emulator)))
        goto error;

    /* In case the domain shutdown while we were not running,
     * we need to finish the shutdown process. And we need to do it after
     * we have qemuCaps filled in.
     */
    if (state == VIR_DOMAIN_SHUTDOWN ||
        (state == VIR_DOMAIN_PAUSED &&
         reason == VIR_DOMAIN_PAUSED_SHUTTING_DOWN)) {
        VIR_DEBUG("Finishing shutdown sequence for domain %s",
                  obj->def->name);
        qemuProcessShutdownOrReboot(driver, obj);
        goto endjob;
    }

    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE))
        if ((qemuDomainAssignAddresses(obj->def, priv->caps, obj)) < 0)
            goto error;

    if (virSecurityManagerReserveLabel(driver->securityManager, obj->def, obj->pid) < 0)
        goto error;

    if (qemuProcessNotifyNets(obj->def) < 0)
        goto error;

    if (qemuProcessFiltersInstantiate(conn, obj->def))
        goto error;

    if (qemuDomainCheckEjectableMedia(driver, obj, QEMU_ASYNC_JOB_NONE) < 0)
        goto error;

    if (qemuProcessRecoverJob(driver, obj, conn, &oldjob) < 0)
        goto error;

    /* update domain state XML with possibly updated state in virDomainObj */
    if (virDomainSaveStatus(driver->caps, driver->stateDir, obj) < 0)
        goto error;

    /* Run an hook to allow admins to do some magic */
    if (virHookPresent(VIR_HOOK_DRIVER_QEMU)) {
        char *xml = qemuDomainDefFormatXML(driver, obj->def, 0);
        int hookret;

        hookret = virHookCall(VIR_HOOK_DRIVER_QEMU, obj->def->name,
                              VIR_HOOK_QEMU_OP_RECONNECT, VIR_HOOK_SUBOP_BEGIN,
                              NULL, xml, NULL);
        VIR_FREE(xml);

        /*
         * If the script raised an error abort the launch
         */
        if (hookret < 0)
            goto error;
    }

    if (!driver->nactive && driver->inhibitCallback)
        driver->inhibitCallback(true, driver->inhibitOpaque);
    driver->nactive++;

endjob:
    if (!qemuDomainObjEndJob(driver, obj))
        obj = NULL;

    if (obj && virObjectUnref(obj))
        virDomainObjUnlock(obj);

    qemuDriverUnlock(driver);

    virConnectClose(conn);

    return;

error:
    if (!qemuDomainObjEndJob(driver, obj))
        obj = NULL;

    if (obj) {
        if (!virDomainObjIsActive(obj)) {
            if (virObjectUnref(obj))
                virDomainObjUnlock(obj);
            qemuDriverUnlock(driver);
            return;
        }

        if (virObjectUnref(obj)) {
            /* We can't get the monitor back, so must kill the VM
             * to remove danger of it ending up running twice if
             * user tries to start it again later
             */
            if (qemuCapsGet(priv->caps, QEMU_CAPS_NO_SHUTDOWN)) {
                /* If we couldn't get the monitor and qemu supports
                 * no-shutdown, we can safely say that the domain
                 * crashed ... */
                state = VIR_DOMAIN_SHUTOFF_CRASHED;
            } else {
                /* ... but if it doesn't we can't say what the state
                 * really is and FAILED means "failed to start" */
                state = VIR_DOMAIN_SHUTOFF_UNKNOWN;
            }
            qemuProcessStop(driver, obj, state, 0);
            if (!obj->persistent)
                qemuDomainRemoveInactive(driver, obj);
            else
                virDomainObjUnlock(obj);
        }
    }
    qemuDriverUnlock(driver);

    virConnectClose(conn);
}

static void
qemuProcessReconnectHelper(void *payload,
                           const void *name ATTRIBUTE_UNUSED,
                           void *opaque)
{
    virThread thread;
    struct qemuProcessReconnectData *src = opaque;
    struct qemuProcessReconnectData *data;
    virDomainObjPtr obj = payload;

    if (VIR_ALLOC(data) < 0) {
        virReportOOMError();
        return;
    }

    memcpy(data, src, sizeof(*data));
    data->payload = payload;

    /* This iterator is called with driver being locked.
     * We create a separate thread to run qemuProcessReconnect in it.
     * However, qemuProcessReconnect needs to:
     * 1. lock driver
     * 2. just before monitor reconnect do lightweight MonitorEnter
     *    (increase VM refcount, unlock VM & driver)
     * 3. reconnect to monitor
     * 4. do lightweight MonitorExit (lock driver & VM)
     * 5. continue reconnect process
     * 6. EndJob
     * 7. unlock driver
     *
     * It is necessary to NOT hold driver lock for the entire run
     * of reconnect, otherwise we will get blocked if there is
     * unresponsive qemu.
     * However, iterating over hash table MUST be done on locked
     * driver.
     *
     * NB, we can't do normal MonitorEnter & MonitorExit because
     * these two lock the monitor lock, which does not exists in
     * this early phase.
     */

    virDomainObjLock(obj);

    qemuDomainObjRestoreJob(obj, &data->oldjob);

    if (qemuDomainObjBeginJobWithDriver(src->driver, obj, QEMU_JOB_MODIFY) < 0)
        goto error;

    /* Since we close the connection later on, we have to make sure
     * that the threads we start see a valid connection throughout their
     * lifetime. We simply increase the reference counter here.
     */
    virConnectRef(data->conn);

    if (virThreadCreate(&thread, true, qemuProcessReconnect, data) < 0) {

        virConnectClose(data->conn);

        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not create thread. QEMU initialization "
                         "might be incomplete"));
        if (!qemuDomainObjEndJob(src->driver, obj)) {
            obj = NULL;
        } else if (virObjectUnref(obj)) {
           /* We can't spawn a thread and thus connect to monitor.
            * Kill qemu */
            qemuProcessStop(src->driver, obj, VIR_DOMAIN_SHUTOFF_FAILED, 0);
            if (!obj->persistent)
                qemuDomainRemoveInactive(src->driver, obj);
            else
                virDomainObjUnlock(obj);
        }
        goto error;
    }

    virDomainObjUnlock(obj);

    return;

error:
    VIR_FREE(data);
}

/**
 * qemuProcessReconnectAll
 *
 * Try to re-open the resources for live VMs that we care
 * about.
 */
void
qemuProcessReconnectAll(virConnectPtr conn, virQEMUDriverPtr driver)
{
    struct qemuProcessReconnectData data = {.conn = conn, .driver = driver};
    virHashForEach(driver->domains.objs, qemuProcessReconnectHelper, &data);
}

int qemuProcessStart(virConnectPtr conn,
                     virQEMUDriverPtr driver,
                     virDomainObjPtr vm,
                     const char *migrateFrom,
                     int stdin_fd,
                     const char *stdin_path,
                     virDomainSnapshotObjPtr snapshot,
                     enum virNetDevVPortProfileOp vmop,
                     unsigned int flags)
{
    int ret;
    off_t pos = -1;
    char ebuf[1024];
    int logfile = -1;
    char *timestamp;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virCommandPtr cmd = NULL;
    struct qemuProcessHookData hookData;
    unsigned long cur_balloon;
    int i;
    char *nodeset = NULL;
    virBitmapPtr nodemask = NULL;
    unsigned int stop_flags;

    /* Okay, these are just internal flags,
     * but doesn't hurt to check */
    virCheckFlags(VIR_QEMU_PROCESS_START_COLD |
                  VIR_QEMU_PROCESS_START_PAUSED |
                  VIR_QEMU_PROCESS_START_AUTODESROY, -1);

    /* From now on until domain security labeling is done:
     * if any operation fails and we goto cleanup, we must not
     * restore any security label as we would overwrite labels
     * we did not set. */
    stop_flags = VIR_QEMU_PROCESS_STOP_NO_RELABEL;

    hookData.conn = conn;
    hookData.vm = vm;
    hookData.driver = driver;

    VIR_DEBUG("Beginning VM startup process");

    if (virDomainObjIsActive(vm)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("VM is already active"));
        return -1;
    }

    /* Do this upfront, so any part of the startup process can add
     * runtime state to vm->def that won't be persisted. This let's us
     * report implicit runtime defaults in the XML, like vnc listen/socket
     */
    VIR_DEBUG("Setting current domain def as transient");
    if (virDomainObjSetDefTransient(driver->caps, vm, true) < 0)
        goto cleanup;

    vm->def->id = driver->nextvmid++;
    qemuDomainSetFakeReboot(driver, vm, false);
    virDomainObjSetState(vm, VIR_DOMAIN_SHUTOFF, VIR_DOMAIN_SHUTOFF_UNKNOWN);

    if (!driver->nactive && driver->inhibitCallback)
        driver->inhibitCallback(true, driver->inhibitOpaque);
    driver->nactive++;

    /* Run an early hook to set-up missing devices */
    if (virHookPresent(VIR_HOOK_DRIVER_QEMU)) {
        char *xml = qemuDomainDefFormatXML(driver, vm->def, 0);
        int hookret;

        hookret = virHookCall(VIR_HOOK_DRIVER_QEMU, vm->def->name,
                              VIR_HOOK_QEMU_OP_PREPARE, VIR_HOOK_SUBOP_BEGIN,
                              NULL, xml, NULL);
        VIR_FREE(xml);

        /*
         * If the script raised an error abort the launch
         */
        if (hookret < 0)
            goto cleanup;
    }

    /* Must be run before security labelling */
    VIR_DEBUG("Preparing host devices");
    if (qemuPrepareHostDevices(driver, vm->def, !migrateFrom) < 0)
        goto cleanup;

    VIR_DEBUG("Preparing chr devices");
    if (virDomainChrDefForeach(vm->def,
                               true,
                               qemuProcessPrepareChardevDevice,
                               NULL) < 0)
        goto cleanup;

    /* If you are using a SecurityDriver with dynamic labelling,
       then generate a security label for isolation */
    VIR_DEBUG("Generating domain security label (if required)");
    if (virSecurityManagerGenLabel(driver->securityManager, vm->def) < 0) {
        virDomainAuditSecurityLabel(vm, false);
        goto cleanup;
    }
    virDomainAuditSecurityLabel(vm, true);

    /* Ensure no historical cgroup for this VM is lying around bogus
     * settings */
    VIR_DEBUG("Ensuring no historical cgroup is lying around");
    qemuRemoveCgroup(driver, vm, 1);

    for (i = 0 ; i < vm->def->ngraphics; ++i) {
        virDomainGraphicsDefPtr graphics = vm->def->graphics[i];
        if (graphics->type == VIR_DOMAIN_GRAPHICS_TYPE_VNC &&
            !graphics->data.vnc.socket &&
            graphics->data.vnc.autoport) {
            int port = qemuProcessNextFreePort(driver, driver->remotePortMin);
            if (port < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               "%s", _("Unable to find an unused port for VNC"));
                goto cleanup;
            }
            graphics->data.vnc.port = port;
        } else if (graphics->type == VIR_DOMAIN_GRAPHICS_TYPE_SPICE) {
            int port = -1;
            if (graphics->data.spice.autoport ||
                graphics->data.spice.port == -1) {
                port = qemuProcessNextFreePort(driver, driver->remotePortMin);

                if (port < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   "%s", _("Unable to find an unused port for SPICE"));
                    goto cleanup;
                }

                graphics->data.spice.port = port;
            }
            if (driver->spiceTLS &&
                (graphics->data.spice.autoport ||
                 graphics->data.spice.tlsPort == -1)) {
                int tlsPort = qemuProcessNextFreePort(driver,
                                                      graphics->data.spice.port + 1);
                if (tlsPort < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   "%s", _("Unable to find an unused port for SPICE TLS"));
                    qemuProcessReturnPort(driver, port);
                    goto cleanup;
                }

                graphics->data.spice.tlsPort = tlsPort;
            }
        }

        if (graphics->type == VIR_DOMAIN_GRAPHICS_TYPE_VNC ||
            graphics->type == VIR_DOMAIN_GRAPHICS_TYPE_SPICE) {
            if (graphics->nListens == 0) {
                if (VIR_EXPAND_N(graphics->listens, graphics->nListens, 1) < 0) {
                    virReportOOMError();
                    goto cleanup;
                }
                graphics->listens[0].type = VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_ADDRESS;
                if (graphics->type == VIR_DOMAIN_GRAPHICS_TYPE_VNC)
                    graphics->listens[0].address = strdup(driver->vncListen);
                else
                    graphics->listens[0].address = strdup(driver->spiceListen);
                if (!graphics->listens[0].address) {
                    VIR_SHRINK_N(graphics->listens, graphics->nListens, 1);
                    virReportOOMError();
                    goto cleanup;
                }
            }
        }
    }

    if (virFileMakePath(driver->logDir) < 0) {
        virReportSystemError(errno,
                             _("cannot create log directory %s"),
                             driver->logDir);
        goto cleanup;
    }

    VIR_DEBUG("Creating domain log file");
    if ((logfile = qemuDomainCreateLog(driver, vm, false)) < 0)
        goto cleanup;

    if (vm->def->virtType == VIR_DOMAIN_VIRT_KVM) {
        VIR_DEBUG("Checking for KVM availability");
        if (access("/dev/kvm", F_OK) != 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Domain requires KVM, but it is not available. "
                             "Check that virtualization is enabled in the host BIOS, "
                             "and host configuration is setup to load the kvm modules."));
            goto cleanup;
        }
    }

    VIR_DEBUG("Determining emulator version");
    virObjectUnref(priv->caps);
    if (!(priv->caps = qemuCapsCacheLookupCopy(driver->capsCache,
                                               vm->def->emulator)))
        goto cleanup;

    if (qemuAssignDeviceAliases(vm->def, priv->caps) < 0)
        goto cleanup;

    VIR_DEBUG("Checking for CDROM and floppy presence");
    if (qemuDomainCheckDiskPresence(driver, vm,
                                    flags & VIR_QEMU_PROCESS_START_COLD) < 0)
        goto cleanup;

    /* Get the advisory nodeset from numad if 'placement' of
     * either <vcpu> or <numatune> is 'auto'.
     */
    if ((vm->def->placement_mode ==
         VIR_DOMAIN_CPU_PLACEMENT_MODE_AUTO) ||
        (vm->def->numatune.memory.placement_mode ==
         VIR_DOMAIN_NUMATUNE_MEM_PLACEMENT_MODE_AUTO)) {
        nodeset = qemuGetNumadAdvice(vm->def);
        if (!nodeset)
            goto cleanup;

        VIR_DEBUG("Nodeset returned from numad: %s", nodeset);

        if (virBitmapParse(nodeset, 0, &nodemask,
                           VIR_DOMAIN_CPUMASK_LEN) < 0)
            goto cleanup;
    }
    hookData.nodemask = nodemask;

    VIR_DEBUG("Setting up domain cgroup (if required)");
    if (qemuSetupCgroup(driver, vm, nodemask) < 0)
        goto cleanup;

    if (VIR_ALLOC(priv->monConfig) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    VIR_DEBUG("Preparing monitor state");
    if (qemuProcessPrepareMonitorChr(driver, priv->monConfig, vm->def->name) < 0)
        goto cleanup;

    if (qemuCapsGet(priv->caps, QEMU_CAPS_MONITOR_JSON))
        priv->monJSON = 1;
    else
        priv->monJSON = 0;

    priv->monError = false;
    priv->monStart = 0;
    priv->gotShutdown = false;

    VIR_FREE(priv->pidfile);
    if (!(priv->pidfile = virPidFileBuildPath(driver->stateDir, vm->def->name))) {
        virReportSystemError(errno,
                             "%s", _("Failed to build pidfile path."));
        goto cleanup;
    }

    if (unlink(priv->pidfile) < 0 &&
        errno != ENOENT) {
        virReportSystemError(errno,
                             _("Cannot remove stale PID file %s"),
                             priv->pidfile);
        goto cleanup;
    }

    /*
     * Normally PCI addresses are assigned in the virDomainCreate
     * or virDomainDefine methods. We might still need to assign
     * some here to cope with the question of upgrades. Regardless
     * we also need to populate the PCi address set cache for later
     * use in hotplug
     */
    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        VIR_DEBUG("Assigning domain PCI addresses");
        if ((qemuDomainAssignAddresses(vm->def, priv->caps, vm)) < 0)
            goto cleanup;
    }

    VIR_DEBUG("Building emulator command line");
    if (!(cmd = qemuBuildCommandLine(conn, driver, vm->def, priv->monConfig,
                                     priv->monJSON != 0, priv->caps,
                                     migrateFrom, stdin_fd, snapshot, vmop)))
        goto cleanup;

    /* now that we know it is about to start call the hook if present */
    if (virHookPresent(VIR_HOOK_DRIVER_QEMU)) {
        char *xml = qemuDomainDefFormatXML(driver, vm->def, 0);
        int hookret;

        hookret = virHookCall(VIR_HOOK_DRIVER_QEMU, vm->def->name,
                              VIR_HOOK_QEMU_OP_START, VIR_HOOK_SUBOP_BEGIN,
                              NULL, xml, NULL);
        VIR_FREE(xml);

        /*
         * If the script raised an error abort the launch
         */
        if (hookret < 0)
            goto cleanup;
    }

    if ((timestamp = virTimeStringNow()) == NULL) {
        virReportOOMError();
        goto cleanup;
    } else {
        if (safewrite(logfile, timestamp, strlen(timestamp)) < 0 ||
            safewrite(logfile, START_POSTFIX, strlen(START_POSTFIX)) < 0) {
            VIR_WARN("Unable to write timestamp to logfile: %s",
                     virStrerror(errno, ebuf, sizeof(ebuf)));
        }

        VIR_FREE(timestamp);
    }

    virCommandWriteArgLog(cmd, logfile);

    qemuDomainObjCheckTaint(driver, vm, logfile);

    if ((pos = lseek(logfile, 0, SEEK_END)) < 0)
        VIR_WARN("Unable to seek to end of logfile: %s",
                 virStrerror(errno, ebuf, sizeof(ebuf)));

    VIR_DEBUG("Clear emulator capabilities: %d",
              driver->clearEmulatorCapabilities);
    if (driver->clearEmulatorCapabilities)
        virCommandClearCaps(cmd);

    /* in case a certain disk is desirous of CAP_SYS_RAWIO, add this */
    for (i = 0; i < vm->def->ndisks; i++) {
        if (vm->def->disks[i]->rawio == 1)
            virCommandAllowCap(cmd, CAP_SYS_RAWIO);
    }

    virCommandSetPreExecHook(cmd, qemuProcessHook, &hookData);

    virCommandSetOutputFD(cmd, &logfile);
    virCommandSetErrorFD(cmd, &logfile);
    virCommandNonblockingFDs(cmd);
    virCommandSetPidFile(cmd, priv->pidfile);
    virCommandDaemonize(cmd);
    virCommandRequireHandshake(cmd);

    ret = virCommandRun(cmd, NULL);

    /* wait for qemu process to show up */
    if (ret == 0) {
        if (virPidFileReadPath(priv->pidfile, &vm->pid) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Domain %s didn't show up"), vm->def->name);
            ret = -1;
        }
#if 0
    } else if (ret == -2) {
        /*
         * XXX this is bogus. It isn't safe to set vm->pid = child
         * because the child no longer exists.
         */

        /* The virCommand process that launches the daemon failed. Pending on
         * when it failed (we can't determine for sure), there may be
         * extra info in the domain log (if the hook failed for example).
         *
         * Pretend like things succeeded, and let 'WaitForMonitor' report
         * the log contents for us.
         */
        vm->pid = child;
        ret = 0;
#endif
    }

    VIR_DEBUG("Writing early domain status to disk");
    if (virDomainSaveStatus(driver->caps, driver->stateDir, vm) < 0) {
        goto cleanup;
    }

    VIR_DEBUG("Waiting for handshake from child");
    if (virCommandHandshakeWait(cmd) < 0) {
        goto cleanup;
    }

    VIR_DEBUG("Setting domain security labels");
    if (virSecurityManagerSetAllLabel(driver->securityManager,
                                      vm->def, stdin_path) < 0)
        goto cleanup;

    /* Security manager labeled all devices, therefore
     * if any operation from now on fails and we goto cleanup,
     * where virSecurityManagerRestoreAllLabel() is called
     * (hidden under qemuProcessStop) we need to restore labels. */
    stop_flags &= ~VIR_QEMU_PROCESS_STOP_NO_RELABEL;

    if (stdin_fd != -1) {
        /* if there's an fd to migrate from, and it's a pipe, put the
         * proper security label on it
         */
        struct stat stdin_sb;

        VIR_DEBUG("setting security label on pipe used for migration");

        if (fstat(stdin_fd, &stdin_sb) < 0) {
            virReportSystemError(errno,
                                 _("cannot stat fd %d"), stdin_fd);
            goto cleanup;
        }
        if (S_ISFIFO(stdin_sb.st_mode) &&
            virSecurityManagerSetImageFDLabel(driver->securityManager, vm->def, stdin_fd) < 0)
            goto cleanup;
    }

    VIR_DEBUG("Labelling done, completing handshake to child");
    if (virCommandHandshakeNotify(cmd) < 0) {
        goto cleanup;
    }
    VIR_DEBUG("Handshake complete, child running");

    if (migrateFrom)
        flags |= VIR_QEMU_PROCESS_START_PAUSED;

    if (ret == -1) /* The VM failed to start; tear filters before taps */
        virDomainConfVMNWFilterTeardown(vm);

    if (ret == -1) /* The VM failed to start */
        goto cleanup;

    VIR_DEBUG("Waiting for monitor to show up");
    if (qemuProcessWaitForMonitor(driver, vm, priv->caps, pos) < 0)
        goto cleanup;

    /* Failure to connect to agent shouldn't be fatal */
    if (qemuConnectAgent(driver, vm) < 0) {
        VIR_WARN("Cannot connect to QEMU guest agent for %s",
                 vm->def->name);
        virResetLastError();
        priv->agentError = true;
    }

    VIR_DEBUG("Detecting VCPU PIDs");
    if (qemuProcessDetectVcpuPIDs(driver, vm) < 0)
        goto cleanup;

    VIR_DEBUG("Setting cgroup for each VCPU (if required)");
    if (qemuSetupCgroupForVcpu(driver, vm) < 0)
        goto cleanup;

    VIR_DEBUG("Setting cgroup for emulator (if required)");
    if (qemuSetupCgroupForEmulator(driver, vm, nodemask) < 0)
        goto cleanup;

    VIR_DEBUG("Setting VCPU affinities");
    if (qemuProcessSetVcpuAffinites(conn, vm) < 0)
        goto cleanup;

    VIR_DEBUG("Setting affinity of emulator threads");
    if (qemuProcessSetEmulatorAffinites(conn, vm) < 0)
        goto cleanup;

    VIR_DEBUG("Setting any required VM passwords");
    if (qemuProcessInitPasswords(conn, driver, vm) < 0)
        goto cleanup;

    /* If we have -device, then addresses are assigned explicitly.
     * If not, then we have to detect dynamic ones here */
    if (!qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        VIR_DEBUG("Determining domain device PCI addresses");
        if (qemuProcessInitPCIAddresses(driver, vm) < 0)
            goto cleanup;
    }

    /* set default link states */
    /* qemu doesn't support setting this on the command line, so
     * enter the monitor */
    VIR_DEBUG("Setting network link states");
    qemuDomainObjEnterMonitorWithDriver(driver, vm);
    if (qemuProcessSetLinkStates(vm) < 0) {
        qemuDomainObjExitMonitorWithDriver(driver, vm);
        goto cleanup;
    }

    qemuDomainObjExitMonitorWithDriver(driver, vm);

    /* Technically, qemuProcessStart can be called from inside
     * QEMU_ASYNC_JOB_MIGRATION_IN, but we are okay treating this like
     * a sync job since no other job can call into the domain until
     * migration completes.  */
    VIR_DEBUG("Setting initial memory amount");
    cur_balloon = vm->def->mem.cur_balloon;
    if (cur_balloon != vm->def->mem.cur_balloon) {
        virReportError(VIR_ERR_OVERFLOW,
                       _("unable to set balloon to %lld"),
                       vm->def->mem.cur_balloon);
        goto cleanup;
    }
    qemuDomainObjEnterMonitorWithDriver(driver, vm);
    if (qemuMonitorSetBalloon(priv->mon, cur_balloon) < 0) {
        qemuDomainObjExitMonitorWithDriver(driver, vm);
        goto cleanup;
    }
    qemuDomainObjExitMonitorWithDriver(driver, vm);

    if (!(flags & VIR_QEMU_PROCESS_START_PAUSED)) {
        VIR_DEBUG("Starting domain CPUs");
        /* Allow the CPUS to start executing */
        if (qemuProcessStartCPUs(driver, vm, conn,
                                 VIR_DOMAIN_RUNNING_BOOTED,
                                 QEMU_ASYNC_JOB_NONE) < 0) {
            if (virGetLastError() == NULL)
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               "%s", _("resume operation failed"));
            goto cleanup;
        }
    } else {
        virDomainObjSetState(vm, VIR_DOMAIN_PAUSED,
                             migrateFrom ?
                             VIR_DOMAIN_PAUSED_MIGRATION :
                             VIR_DOMAIN_PAUSED_USER);
    }

    if (flags & VIR_QEMU_PROCESS_START_AUTODESROY &&
        qemuProcessAutoDestroyAdd(driver, vm, conn) < 0)
        goto cleanup;

    VIR_DEBUG("Writing domain status to disk");
    if (virDomainSaveStatus(driver->caps, driver->stateDir, vm) < 0)
        goto cleanup;

    /* finally we can call the 'started' hook script if any */
    if (virHookPresent(VIR_HOOK_DRIVER_QEMU)) {
        char *xml = qemuDomainDefFormatXML(driver, vm->def, 0);
        int hookret;

        hookret = virHookCall(VIR_HOOK_DRIVER_QEMU, vm->def->name,
                              VIR_HOOK_QEMU_OP_STARTED, VIR_HOOK_SUBOP_BEGIN,
                              NULL, xml, NULL);
        VIR_FREE(xml);

        /*
         * If the script raised an error abort the launch
         */
        if (hookret < 0)
            goto cleanup;
    }

    virCommandFree(cmd);
    VIR_FORCE_CLOSE(logfile);

    return 0;

cleanup:
    /* We jump here if we failed to start the VM for any reason, or
     * if we failed to initialize the now running VM. kill it off and
     * pretend we never started it */
    VIR_FREE(nodeset);
    virBitmapFree(nodemask);
    virCommandFree(cmd);
    VIR_FORCE_CLOSE(logfile);
    qemuProcessStop(driver, vm, VIR_DOMAIN_SHUTOFF_FAILED, stop_flags);

    return -1;
}


int
qemuProcessKill(virQEMUDriverPtr driver,
                virDomainObjPtr vm, unsigned int flags)
{
    int ret;

    VIR_DEBUG("vm=%s pid=%d flags=%x",
              vm->def->name, vm->pid, flags);

    if (!(flags & VIR_QEMU_PROCESS_KILL_NOCHECK)) {
        if (!virDomainObjIsActive(vm)) {
            VIR_DEBUG("VM '%s' not active", vm->def->name);
            return 0;
        }
    }

    if ((flags & VIR_QEMU_PROCESS_KILL_NOWAIT)) {
        virProcessKill(vm->pid,
                       (flags & VIR_QEMU_PROCESS_KILL_FORCE) ?
                       SIGKILL : SIGTERM);
        return 0;
    }

    if (driver)
        qemuDriverUnlock(driver);

    ret = virProcessKillPainfully(vm->pid,
                                  !!(flags & VIR_QEMU_PROCESS_KILL_FORCE));

    if (driver) {
        virObjectRef(vm);
        virDomainObjUnlock(vm);
        qemuDriverLock(driver);
        virDomainObjLock(vm);
        virObjectUnref(vm);
    }

    return ret;
}


void qemuProcessStop(virQEMUDriverPtr driver,
                     virDomainObjPtr vm,
                     virDomainShutoffReason reason,
                     unsigned int flags)
{
    int ret;
    int retries = 0;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virErrorPtr orig_err;
    virDomainDefPtr def;
    virNetDevVPortProfilePtr vport = NULL;
    int i;
    int logfile = -1;
    char *timestamp;
    char ebuf[1024];

    VIR_DEBUG("Shutting down VM '%s' pid=%d flags=%x",
              vm->def->name, vm->pid, flags);

    if (!virDomainObjIsActive(vm)) {
        VIR_DEBUG("VM '%s' not active", vm->def->name);
        return;
    }

    /*
     * We may unlock the driver and vm in qemuProcessKill(), and another thread
     * can lock driver and vm, and then call qemuProcessStop(). So we should
     * set vm->def->id to -1 here to avoid qemuProcessStop() to be called twice.
     */
    vm->def->id = -1;

    driver->nactive--;
    if (!driver->nactive && driver->inhibitCallback)
        driver->inhibitCallback(false, driver->inhibitOpaque);

    if ((logfile = qemuDomainCreateLog(driver, vm, true)) < 0) {
        /* To not break the normal domain shutdown process, skip the
         * timestamp log writing if failed on opening log file. */
        VIR_WARN("Unable to open logfile: %s",
                  virStrerror(errno, ebuf, sizeof(ebuf)));
    } else {
        if ((timestamp = virTimeStringNow()) == NULL) {
            virReportOOMError();
        } else {
            if (safewrite(logfile, timestamp, strlen(timestamp)) < 0 ||
                safewrite(logfile, SHUTDOWN_POSTFIX,
                          strlen(SHUTDOWN_POSTFIX)) < 0) {
                VIR_WARN("Unable to write timestamp to logfile: %s",
                         virStrerror(errno, ebuf, sizeof(ebuf)));
            }

            VIR_FREE(timestamp);
        }

        if (VIR_CLOSE(logfile) < 0)
             VIR_WARN("Unable to close logfile: %s",
                      virStrerror(errno, ebuf, sizeof(ebuf)));
    }

    /* This method is routinely used in clean up paths. Disable error
     * reporting so we don't squash a legit error. */
    orig_err = virSaveLastError();

    virDomainConfVMNWFilterTeardown(vm);

    if (driver->macFilter) {
        def = vm->def;
        for (i = 0 ; i < def->nnets ; i++) {
            virDomainNetDefPtr net = def->nets[i];
            if (net->ifname == NULL)
                continue;
            if ((errno = networkDisallowMacOnPort(driver, net->ifname,
                                                  &net->mac))) {
                virReportSystemError(errno,
             _("failed to remove ebtables rule to allow MAC address on '%s'"),
                                     net->ifname);
            }
        }
    }

    if (priv->agent) {
        qemuAgentClose(priv->agent);
        priv->agent = NULL;
        priv->agentError = false;
    }

    if (priv->mon) {
        qemuMonitorClose(priv->mon);
        priv->mon = NULL;
    }

    if (priv->monConfig) {
        if (priv->monConfig->type == VIR_DOMAIN_CHR_TYPE_UNIX)
            unlink(priv->monConfig->data.nix.path);
        virDomainChrSourceDefFree(priv->monConfig);
        priv->monConfig = NULL;
    }

    /* shut it off for sure */
    ignore_value(qemuProcessKill(driver, vm, VIR_QEMU_PROCESS_KILL_FORCE|
                                             VIR_QEMU_PROCESS_KILL_NOCHECK));

    qemuDomainCleanupRun(driver, vm);

    /* Stop autodestroy in case guest is restarted */
    qemuProcessAutoDestroyRemove(driver, vm);

    /* now that we know it's stopped call the hook if present */
    if (virHookPresent(VIR_HOOK_DRIVER_QEMU)) {
        char *xml = qemuDomainDefFormatXML(driver, vm->def, 0);

        /* we can't stop the operation even if the script raised an error */
        virHookCall(VIR_HOOK_DRIVER_QEMU, vm->def->name,
                    VIR_HOOK_QEMU_OP_STOPPED, VIR_HOOK_SUBOP_END,
                    NULL, xml, NULL);
        VIR_FREE(xml);
    }

    /* Reset Security Labels unless caller don't want us to */
    if (!(flags & VIR_QEMU_PROCESS_STOP_NO_RELABEL))
        virSecurityManagerRestoreAllLabel(driver->securityManager,
                                          vm->def,
                                          flags & VIR_QEMU_PROCESS_STOP_MIGRATED);
    virSecurityManagerReleaseLabel(driver->securityManager, vm->def);

    /* Clear out dynamically assigned labels */
    for (i = 0; i < vm->def->nseclabels; i++) {
        if (vm->def->seclabels[i]->type == VIR_DOMAIN_SECLABEL_DYNAMIC) {
            VIR_FREE(vm->def->seclabels[i]->label);
        }
        VIR_FREE(vm->def->seclabels[i]->imagelabel);
    }

    virDomainDefClearDeviceAliases(vm->def);
    if (!priv->persistentAddrs) {
        virDomainDefClearPCIAddresses(vm->def);
        qemuDomainPCIAddressSetFree(priv->pciaddrs);
        priv->pciaddrs = NULL;
    }

    qemuDomainReAttachHostDevices(driver, vm->def);

    def = vm->def;
    for (i = 0; i < def->nnets; i++) {
        virDomainNetDefPtr net = def->nets[i];
        if (virDomainNetGetActualType(net) == VIR_DOMAIN_NET_TYPE_DIRECT) {
            ignore_value(virNetDevMacVLanDeleteWithVPortProfile(
                             net->ifname, &net->mac,
                             virDomainNetGetActualDirectDev(net),
                             virDomainNetGetActualDirectMode(net),
                             virDomainNetGetActualVirtPortProfile(net),
                             driver->stateDir));
            VIR_FREE(net->ifname);
        }
        /* release the physical device (or any other resources used by
         * this interface in the network driver
         */
        vport = virDomainNetGetActualVirtPortProfile(net);
        if (vport && vport->virtPortType == VIR_NETDEV_VPORT_PROFILE_OPENVSWITCH)
            ignore_value(virNetDevOpenvswitchRemovePort(
                                       virDomainNetGetActualBridgeName(net),
                                       net->ifname));

        networkReleaseActualDevice(net);
    }

retry:
    if ((ret = qemuRemoveCgroup(driver, vm, 0)) < 0) {
        if (ret == -EBUSY && (retries++ < 5)) {
            usleep(200*1000);
            goto retry;
        }
        VIR_WARN("Failed to remove cgroup for %s",
                 vm->def->name);
    }

    qemuProcessRemoveDomainStatus(driver, vm);

    /* Remove VNC and Spice ports from port reservation bitmap, but only if
       they were reserved by the driver (autoport=yes)
    */
    for (i = 0 ; i < vm->def->ngraphics; ++i) {
        virDomainGraphicsDefPtr graphics = vm->def->graphics[i];
        if (graphics->type == VIR_DOMAIN_GRAPHICS_TYPE_VNC &&
            graphics->data.vnc.autoport) {
            qemuProcessReturnPort(driver, graphics->data.vnc.port);
        }
        if (graphics->type == VIR_DOMAIN_GRAPHICS_TYPE_SPICE &&
            graphics->data.spice.autoport) {
            qemuProcessReturnPort(driver, graphics->data.spice.port);
            qemuProcessReturnPort(driver, graphics->data.spice.tlsPort);
        }
    }

    vm->taint = 0;
    vm->pid = -1;
    virDomainObjSetState(vm, VIR_DOMAIN_SHUTOFF, reason);
    VIR_FREE(priv->vcpupids);
    priv->nvcpupids = 0;
    virObjectUnref(priv->caps);
    priv->caps = NULL;
    VIR_FREE(priv->pidfile);

    /* The "release" hook cleans up additional resources */
    if (virHookPresent(VIR_HOOK_DRIVER_QEMU)) {
        char *xml = qemuDomainDefFormatXML(driver, vm->def, 0);

        /* we can't stop the operation even if the script raised an error */
        virHookCall(VIR_HOOK_DRIVER_QEMU, vm->def->name,
                    VIR_HOOK_QEMU_OP_RELEASE, VIR_HOOK_SUBOP_END,
                    NULL, xml, NULL);
        VIR_FREE(xml);
    }

    if (vm->newDef) {
        virDomainDefFree(vm->def);
        vm->def = vm->newDef;
        vm->def->id = -1;
        vm->newDef = NULL;
    }

    if (orig_err) {
        virSetError(orig_err);
        virFreeError(orig_err);
    }
}


int qemuProcessAttach(virConnectPtr conn ATTRIBUTE_UNUSED,
                      virQEMUDriverPtr driver,
                      virDomainObjPtr vm,
                      pid_t pid,
                      const char *pidfile,
                      virDomainChrSourceDefPtr monConfig,
                      bool monJSON)
{
    size_t i;
    char ebuf[1024];
    int logfile = -1;
    char *timestamp;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    bool running = true;
    virDomainPausedReason reason;
    virSecurityLabelPtr seclabel = NULL;
    virSecurityLabelDefPtr seclabeldef = NULL;
    virSecurityManagerPtr* sec_managers = NULL;
    const char *model;

    VIR_DEBUG("Beginning VM attach process");

    if (virDomainObjIsActive(vm)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("VM is already active"));
        return -1;
    }

    /* Do this upfront, so any part of the startup process can add
     * runtime state to vm->def that won't be persisted. This let's us
     * report implicit runtime defaults in the XML, like vnc listen/socket
     */
    VIR_DEBUG("Setting current domain def as transient");
    if (virDomainObjSetDefTransient(driver->caps, vm, true) < 0)
        goto cleanup;

    vm->def->id = driver->nextvmid++;

    if (!driver->nactive && driver->inhibitCallback)
        driver->inhibitCallback(true, driver->inhibitOpaque);
    driver->nactive++;

    if (virFileMakePath(driver->logDir) < 0) {
        virReportSystemError(errno,
                             _("cannot create log directory %s"),
                             driver->logDir);
        goto cleanup;
    }

    VIR_FREE(priv->pidfile);
    if (pidfile &&
        !(priv->pidfile = strdup(pidfile)))
        goto no_memory;

    VIR_DEBUG("Detect security driver config");
    sec_managers = virSecurityManagerGetNested(driver->securityManager);
    if (sec_managers == NULL) {
        goto cleanup;
    }

    for (i = 0; sec_managers[i]; i++) {
        model = virSecurityManagerGetModel(sec_managers[i]);
        seclabeldef = virDomainDefGetSecurityLabelDef(vm->def, model);
        if (seclabeldef == NULL) {
            goto cleanup;
        }
        seclabeldef->type = VIR_DOMAIN_SECLABEL_STATIC;
        if (VIR_ALLOC(seclabel) < 0)
            goto no_memory;
        if (virSecurityManagerGetProcessLabel(driver->securityManager,
                                              vm->def, vm->pid, seclabel) < 0)
            goto cleanup;

        if (!(seclabeldef->model = strdup(model)))
            goto no_memory;

        if (!(seclabeldef->label = strdup(seclabel->label)))
            goto no_memory;
        VIR_FREE(seclabel);
    }

    VIR_DEBUG("Creating domain log file");
    if ((logfile = qemuDomainCreateLog(driver, vm, false)) < 0)
        goto cleanup;

    VIR_DEBUG("Determining emulator version");
    virObjectUnref(priv->caps);
    if (!(priv->caps = qemuCapsCacheLookupCopy(driver->capsCache,
                                               vm->def->emulator)))
        goto cleanup;

    VIR_DEBUG("Preparing monitor state");
    priv->monConfig = monConfig;
    monConfig = NULL;
    priv->monJSON = monJSON;

    priv->gotShutdown = false;

    /*
     * Normally PCI addresses are assigned in the virDomainCreate
     * or virDomainDefine methods. We might still need to assign
     * some here to cope with the question of upgrades. Regardless
     * we also need to populate the PCi address set cache for later
     * use in hotplug
     */
    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        VIR_DEBUG("Assigning domain PCI addresses");
        if ((qemuDomainAssignAddresses(vm->def, priv->caps, vm)) < 0)
            goto cleanup;
    }

    if ((timestamp = virTimeStringNow()) == NULL) {
        virReportOOMError();
        goto cleanup;
    } else {
        if (safewrite(logfile, timestamp, strlen(timestamp)) < 0 ||
            safewrite(logfile, ATTACH_POSTFIX, strlen(ATTACH_POSTFIX)) < 0) {
            VIR_WARN("Unable to write timestamp to logfile: %s",
                     virStrerror(errno, ebuf, sizeof(ebuf)));
        }

        VIR_FREE(timestamp);
    }

    qemuDomainObjTaint(driver, vm, VIR_DOMAIN_TAINT_EXTERNAL_LAUNCH, logfile);

    vm->pid = pid;

    VIR_DEBUG("Waiting for monitor to show up");
    if (qemuProcessWaitForMonitor(driver, vm, priv->caps, -1) < 0)
        goto cleanup;

    /* Failure to connect to agent shouldn't be fatal */
    if (qemuConnectAgent(driver, vm) < 0) {
        VIR_WARN("Cannot connect to QEMU guest agent for %s",
                 vm->def->name);
        virResetLastError();
        priv->agentError = true;
    }

    VIR_DEBUG("Detecting VCPU PIDs");
    if (qemuProcessDetectVcpuPIDs(driver, vm) < 0)
        goto cleanup;

    /* If we have -device, then addresses are assigned explicitly.
     * If not, then we have to detect dynamic ones here */
    if (!qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        VIR_DEBUG("Determining domain device PCI addresses");
        if (qemuProcessInitPCIAddresses(driver, vm) < 0)
            goto cleanup;
    }

    VIR_DEBUG("Getting initial memory amount");
    qemuDomainObjEnterMonitorWithDriver(driver, vm);
    if (qemuMonitorGetBalloonInfo(priv->mon, &vm->def->mem.cur_balloon) < 0) {
        qemuDomainObjExitMonitorWithDriver(driver, vm);
        goto cleanup;
    }
    if (qemuMonitorGetStatus(priv->mon, &running, &reason) < 0) {
        qemuDomainObjExitMonitorWithDriver(driver, vm);
        goto cleanup;
    }
    if (qemuMonitorGetVirtType(priv->mon, &vm->def->virtType) < 0) {
        qemuDomainObjExitMonitorWithDriver(driver, vm);
        goto cleanup;
    }
    qemuDomainObjExitMonitorWithDriver(driver, vm);

    if (!virDomainObjIsActive(vm))
        goto cleanup;

    if (running)
        virDomainObjSetState(vm, VIR_DOMAIN_RUNNING,
                             VIR_DOMAIN_RUNNING_UNPAUSED);
    else
        virDomainObjSetState(vm, VIR_DOMAIN_PAUSED, reason);

    VIR_DEBUG("Writing domain status to disk");
    if (virDomainSaveStatus(driver->caps, driver->stateDir, vm) < 0)
        goto cleanup;

    /* Run an hook to allow admins to do some magic */
    if (virHookPresent(VIR_HOOK_DRIVER_QEMU)) {
        char *xml = qemuDomainDefFormatXML(driver, vm->def, 0);
        int hookret;

        hookret = virHookCall(VIR_HOOK_DRIVER_QEMU, vm->def->name,
                              VIR_HOOK_QEMU_OP_ATTACH, VIR_HOOK_SUBOP_BEGIN,
                              NULL, xml, NULL);
        VIR_FREE(xml);

        /*
         * If the script raised an error abort the launch
         */
        if (hookret < 0)
            goto cleanup;
    }

    VIR_FORCE_CLOSE(logfile);
    VIR_FREE(seclabel);
    VIR_FREE(sec_managers);

    return 0;

no_memory:
    virReportOOMError();
cleanup:
    /* We jump here if we failed to start the VM for any reason, or
     * if we failed to initialize the now running VM. kill it off and
     * pretend we never started it */
    VIR_FORCE_CLOSE(logfile);
    VIR_FREE(seclabel);
    VIR_FREE(sec_managers);
    virDomainChrSourceDefFree(monConfig);
    return -1;
}


static virDomainObjPtr
qemuProcessAutoDestroy(virQEMUDriverPtr driver,
                       virDomainObjPtr dom,
                       virConnectPtr conn)
{
    qemuDomainObjPrivatePtr priv = dom->privateData;
    virDomainEventPtr event = NULL;

    VIR_DEBUG("vm=%s, conn=%p", dom->def->name, conn);

    if (priv->job.asyncJob) {
        VIR_DEBUG("vm=%s has long-term job active, cancelling",
                  dom->def->name);
        qemuDomainObjDiscardAsyncJob(driver, dom);
    }

    if (qemuDomainObjBeginJobWithDriver(driver, dom,
                                        QEMU_JOB_DESTROY) < 0)
        goto cleanup;

    VIR_DEBUG("Killing domain");
    qemuProcessStop(driver, dom, VIR_DOMAIN_SHUTOFF_DESTROYED,
                    VIR_QEMU_PROCESS_STOP_MIGRATED);
    virDomainAuditStop(dom, "destroyed");
    event = virDomainEventNewFromObj(dom,
                                     VIR_DOMAIN_EVENT_STOPPED,
                                     VIR_DOMAIN_EVENT_STOPPED_DESTROYED);

    if (!qemuDomainObjEndJob(driver, dom))
        dom = NULL;
    if (dom && !dom->persistent)
        qemuDomainRemoveInactive(driver, dom);
    if (event)
        qemuDomainEventQueue(driver, event);

cleanup:
    return dom;
}

int qemuProcessAutoDestroyAdd(virQEMUDriverPtr driver,
                              virDomainObjPtr vm,
                              virConnectPtr conn)
{
    VIR_DEBUG("vm=%s, conn=%p", vm->def->name, conn);
    return qemuDriverCloseCallbackSet(driver, vm, conn,
                                      qemuProcessAutoDestroy);
}

int qemuProcessAutoDestroyRemove(virQEMUDriverPtr driver,
                                 virDomainObjPtr vm)
{
    VIR_DEBUG("vm=%s", vm->def->name);
    return qemuDriverCloseCallbackUnset(driver, vm, qemuProcessAutoDestroy);
}

bool qemuProcessAutoDestroyActive(virQEMUDriverPtr driver,
                                  virDomainObjPtr vm)
{
    qemuDriverCloseCallback cb;
    VIR_DEBUG("vm=%s", vm->def->name);
    cb = qemuDriverCloseCallbackGet(driver, vm, NULL);
    return cb == qemuProcessAutoDestroy;
}
