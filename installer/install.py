#
# Virtio9PFS Handler Install install.py
# $VER: Virtio9PFS Handler Install 0.10.0 (11.06.2026)
# Auto-generated -- do not edit; regenerate from the fixture module.
#

from installer import *
from Virtio9PFSInstallerLocale import *
import amiga
import os

loc = Virtio9PFSInstallerLocale()

##############################################
# welcomePage
welcomePage = NewPage(WELCOME)
SetString(welcomePage, 'message', loc.GetString(loc.MSG_WELCOME))

##############################################
# installPage
installPage = NewPage(INSTALL)

##############################################
# Post-install actions

def rebootHandler():
    amiga.system("reboot SYNC")
    return True

AddPostInstallAction(
    name='Reboot',
    description=loc.GetString(loc.MSG_REBOOT),
    visible=True,
    default=True,
    callback=rebootHandler,
    )

##############################################
# finishPage
finishPage = NewPage(FINISH)
SetString(finishPage, 'message', loc.GetString(loc.MSG_FINISH))

##############################################
# Top-level packages (always registered)

_pkg = AddPackage(FILEPACKAGE,
    name='Virtio9PFS filesystem handler',
    files=['content/Virtio9PFS-handler'],
    alternatepath="L:"
    )

_pkg = AddPackage(FILEPACKAGE,
    name='SHARED DOSDriver',
    files=['content/SHARED'],
    alternatepath="DEVS:DOSDrivers"
    )

##############################################
# Run the installer
RunInstaller()
