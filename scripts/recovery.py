#!/usr/bin/env python

# Copyright (c) 2010-2011 Stanford University
#
# Permission to use, copy, modify, and distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

"""Runs a recovery of a master."""

from common import *
import metrics
import os
import pprint
import re
import subprocess
import time

hosts = []
for i in range(1, 37):
    hosts.append(('rc%02d' % i,
                  '192.168.1.%d' % (100 + i)))

obj_path = '%s/%s' % (top_path, obj_dir)
coordinatorBin = '%s/coordinator' % obj_path
backupBin = '%s/backup' % obj_path
masterBin = '%s/server' % obj_path
clientBin = '%s/client' % obj_path
ensureHostsBin = '%s/ensureHosts' % obj_path

def recover(numBackups=1,
            numPartitions=1,
            objectSize=1024,
            numObjects=626012,
            replicas=1,
            disk=False,
            timeout=60,
            coordinatorArgs='',
            backupArgs='',
            oldMasterArgs='-m 2048',
            newMasterArgs='-m 2048',
            clientArgs=''):

    coordinatorHost = hosts[0]
    coordinatorLocator = 'infrc:host=%s,port=12246' % coordinatorHost[1]

    backupHosts = (hosts[1:] + [hosts[0]])[:numBackups]
    backupLocators = ['infrc:host=%s,port=12243' % host[1]
                      for host in backupHosts]

    oldMasterHost = hosts[0]
    oldMasterLocator = 'infrc:host=%s,port=12242' % oldMasterHost[1]

    newMasterHosts = (hosts[1:] + [hosts[0]])[:numPartitions]
    newMasterLocators = ['infrc:host=%s,port=12247' % host[1]
                         for host in newMasterHosts]

    clientHost = hosts[0]

    try:
        os.mkdir('recovery')
    except:
        pass
    datetime = time.strftime('%Y%m%d%H%M%S')
    run = 'recovery/%s' % datetime
    os.mkdir(run)
    try:
        os.remove('recovery/latest')
    except:
        pass
    os.symlink(datetime, 'recovery/latest')

    coordinator = None
    backups = []
    oldMaster = None
    newMasters = []
    client = None
    with Sandbox() as sandbox:
        def ensureHosts(qty):
            sandbox.checkFailures()
            try:
                sandbox.rsh(clientHost[0], '%s -C %s -n %d -l 1' %
                            (ensureHostsBin, coordinatorLocator, qty))
            except:
                # prefer exceptions from dead processes to timeout error
                sandbox.checkFailures()
                raise

        # start coordinator
        coordinator = sandbox.rsh(coordinatorHost[0],
                  ('%s -C %s %s' %
                   (coordinatorBin, coordinatorLocator, coordinatorArgs)),
                  bg=True, stderr=subprocess.STDOUT,
                  stdout=open(('%s/coordinator.%s.log' %
                               (run, coordinatorHost[0])), 'w'))
        ensureHosts(0)

        # start backups
        for i, (backupHost, backupLocator) in enumerate(zip(backupHosts,
                                                            backupLocators)):
            backups.append(sandbox.rsh(backupHost[0],
                       ('%s %s -C %s -L %s %s' %
                        (backupBin,
                         '-f %s' % disk if disk else '-m',
                         coordinatorLocator,
                         backupLocator,
                         backupArgs)),
                       bg=True, stderr=subprocess.STDOUT,
                       stdout=open('%s/backup.%s.log' % (run, backupHost[0]),
                                   'w')))
        ensureHosts(len(backups))

        # start dying master
        oldMaster = sandbox.rsh(oldMasterHost[0],
                        ('%s -r %d -C %s -L %s %s' %
                         (masterBin, replicas,
                          coordinatorLocator,
                          oldMasterLocator,
                          oldMasterArgs)),
                        bg=True, stderr=subprocess.STDOUT,
                        stdout=open(('%s/oldMaster.%s.log' %
                                     (run, oldMasterHost[0])),
                                    'w'))
        ensureHosts(len(backups) + 1)

        # start recovery masters
        for i, (newMasterHost,
                newMasterLocator) in enumerate(zip(newMasterHosts,
                                                   newMasterLocators)):
            newMasters.append(sandbox.rsh(newMasterHost[0],
                                  ('%s -r %d -C %s -L %s %s' %
                                   (masterBin,
                                    replicas,
                                    coordinatorLocator,
                                    newMasterLocator,
                                    newMasterArgs)),
                                  bg=True, stderr=subprocess.STDOUT,
                                  stdout=open(('%s/newMaster.%s.log' %
                                               (run, newMasterHost[0])),
                                              'w')))
        ensureHosts(len(backups) + 1 + len(newMasters))

        # start client
        client = sandbox.rsh(clientHost[0],
                     ('%s -d -C %s -n %d -s %d -t %d -k %d %s' %
                      (clientBin, coordinatorLocator, numObjects, objectSize,
                      numPartitions, numPartitions, clientArgs)),
                     bg=True, stderr=subprocess.STDOUT,
                     stdout=open('%s/client.%s.log' % (run, clientHost[0]),
                                 'w'))

        start = time.time()
        while client.returncode is None:
            sandbox.checkFailures()
            time.sleep(.1)
            if time.time() - start > timeout:
                raise Exception('timeout exceeded')

        stats = {}
        stats['metrics'] = metrics.parseRecovery(run)
        stats['run'] = run
        stats['count'] = numObjects
        stats['size'] = objectSize
        stats['ns'] = stats['metrics'].client.recoveryNs
        return stats

def insist(*args, **kwargs):
    """Keep insistly trying recoveries until the damn thing succeeds"""
    while True:
        try:
            return recover(*args, **kwargs)
        except subprocess.CalledProcessError, e:
            print 'Recovery failed:', e
            print 'Trying again...'
        except ValueError, e:
            print 'Recovery failed:', e
            print 'Trying again...'

if __name__ == '__main__':
    pprint.pprint(recover())