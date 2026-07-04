"""Installer-script fixture for the Virtio9PFS-handler installer.

Produces an AmigaOS 4.1 FE `Installation Utility` script (Python 2.5)
that:

  1. copies Virtio9PFS-handler into L:
  2. copies the SHARED DOSDriver into DEVS:DOSDrivers/
  3. offers a reboot on the finish page (the SHARED: volume mounts at
     boot)

install.py + Virtio9PFSInstallerLocale.py are emitted from this
fixture by an in-house installer-script generator and committed, so
building the distribution archive needs no extra tooling.  This
fixture is the authoritative description of the installer's pages,
messages, and behaviour.  The page idioms are expanded from
`installergen.presets` -- the field-tested templates shared by all of
this author's driver installers.

Archive layout consumed by the script (see `make dist`):

    Virtio9PFS/
      install.py
      Virtio9PFSInstallerLocale.py
      content/Virtio9PFS-handler
      content/SHARED

IMPORTANT: the installer must be launched with the drawer as the
current directory (double-clicking the install.py icon does this; from
a shell, CD into the drawer first) -- the package uses drawer-relative
content/ paths, exactly like the OS's own update installers.
"""

from installergen import (
    Project, Page, PageKind, Package, PackageKind, PostInstallAction,
    LocaleString, LocaleRef, Handler,
)
from installergen.presets import (
    README_BUTTON_LOCALE, welcome_with_readme, finish_page,
)


# NOTE: the Installation Utility's page text is PLAIN TEXT only and the
# label does NOT scroll -- keep pages inside the ~20-rendered-line lint
# budget and defer detail to the bundled readme.  Formatting follows
# Hyperion's own Update installers: leading blank line, paragraph
# spacing, quoted file and button names, explicit navigation cue.
locale = [
    LocaleString(
        "MSG_WELCOME",
        "\nWelcome to the installation of the Virtio9PFS filesystem "
        "handler.\n\n"
        "Virtio9PFS-handler mounts QEMU host-shared folders as native "
        "AmigaOS volumes via VirtIO 9P.\n\n"
        "The following changes will be made to your system:\n\n"
        "    1.  Virtio9PFS-handler will be copied to \"L:\"\n\n"
        "    2.  The \"SHARED\" DOSDriver will be copied to "
        "\"DEVS:DOSDrivers\"\n\n"
        "A system restart activates the SHARED: volume.  Click "
        "\"View Readme\" below for manual installation details, the "
        "QEMU shared-folder setup, and general instructions on "
        "use.\n\n\n"
        "Press \"Next\" to continue."),
    README_BUTTON_LOCALE,
    LocaleString(
        "MSG_FINISH",
        "\nThe installation has finished.\n\n"
        "Virtio9PFS-handler has been copied to \"L:\" and the "
        "\"SHARED\" DOSDriver to \"DEVS:DOSDrivers\".  The SHARED: "
        "volume appears after the next system restart.\n\n"
        "QEMU must share a host folder whose mount_tag matches the "
        "DOSDriver name (SHARED); the exact -virtfs setup is in the "
        "README file in this drawer.  If no 9P device is present the "
        "handler declines the mount silently and boot continues "
        "normally.\n\n\n"
        "Press \"Finish\" to exit the installation."),
    LocaleString(
        "MSG_REBOOT",
        "Restart the system now (required to activate the SHARED: volume)"),
]


# Welcome page with the View Readme button (proven preset).
welcome_page = welcome_with_readme(LocaleRef("MSG_WELCOME"), "README")

install_page = Page(
    var_name="installPage",
    kind=PageKind.INSTALL,
)

finish = finish_page(LocaleRef("MSG_FINISH"))


handler_package = Package(
    name="Virtio9PFS filesystem handler",
    files=["content/Virtio9PFS-handler"],
    kind=PackageKind.FILEPACKAGE,
    # Fixed destinations: handlers belong in L: and DOSDrivers in
    # DEVS:DOSDrivers regardless of any user preference, so there is
    # no DESTINATION page.  Emitted verbatim, hence embedded quotes.
    alternatepath="\"L:\"",
    register_in="top",
)

dosdriver_package = Package(
    name="SHARED DOSDriver",
    files=["content/SHARED"],
    kind=PackageKind.FILEPACKAGE,
    alternatepath="\"DEVS:DOSDrivers\"",
    register_in="top",
)

reboot_action = PostInstallAction(
    name="Reboot",
    description=LocaleRef("MSG_REBOOT"),
    visible=True,
    default=True,
    callback=Handler(
        name="rebootHandler",
        params=[],
        body="amiga.system(\"reboot SYNC\")\nreturn True\n",
    ),
)


project = Project(
    name="Virtio9PFS Handler Install",
    short_name="Virtio9PFS",
    version="0.10.1",
    date="05.07.2026",
    locale_strings=locale,
    pages=[welcome_page, install_page, finish],
    packages=[handler_package, dosdriver_package],
    post_install_actions=[reboot_action],
)
