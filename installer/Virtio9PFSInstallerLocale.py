#
# Virtio9PFSInstallerLocale - locale wrapper
# Auto-generated -- do not edit; regenerate from the fixture module.
#

import catalog


class Virtio9PFSInstallerLocale:
    strings = {}
    cat = None

    MSG_WELCOME = 1
    MSG_README_BUTTON = 2
    MSG_FINISH = 3
    MSG_REBOOT = 4

    def __init__(self, language="", catalogName='Virtio9PFS.catalog', builtinLanguage='english'):
        self.strings[self.MSG_WELCOME] = '\nWelcome to the installation of the Virtio9PFS filesystem handler.\n\nVirtio9PFS-handler mounts QEMU host-shared folders as native AmigaOS volumes via VirtIO 9P.\n\nThe following changes will be made to your system:\n\n    1.  Virtio9PFS-handler will be copied to "L:"\n\n    2.  The "SHARED" DOSDriver will be copied to "DEVS:DOSDrivers"\n\nA system restart activates the SHARED: volume.  Click "View Readme" below for manual installation details, the QEMU shared-folder setup, and general instructions on use.\n\n\nPress "Next" to continue.'
        self.strings[self.MSG_README_BUTTON] = 'View Readme...'
        self.strings[self.MSG_FINISH] = '\nThe installation has finished.\n\nVirtio9PFS-handler has been copied to "L:" and the "SHARED" DOSDriver to "DEVS:DOSDrivers".  The SHARED: volume appears after the next system restart.\n\nQEMU must share a host folder whose mount_tag matches the DOSDriver name (SHARED); the exact -virtfs setup is in the README file in this drawer.  If no 9P device is present the handler declines the mount silently and boot continues normally.\n\n\nPress "Finish" to exit the installation.'
        self.strings[self.MSG_REBOOT] = 'Restart the system now (required to activate the SHARED: volume)'

        try:
            self.cat = catalog.OpenCatalog(catalogName, language, builtinLanguage)
        except:
            self.cat = None

    def GetString(self, id):
        if self.cat != None:
            return self.cat.GetString(id, self.strings[id])
        return self.strings[id]
