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
messages, and behaviour.

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
    LocaleString, LocaleRef, GuiBlock, GuiWidget, WidgetKind,
    GroupOrientation, Frame,
)
from installergen.model import Handler


locale = [
    LocaleString(
        "MSG_WELCOME",
        "\nWelcome to the installation of the Virtio9PFS filesystem "
        "handler.\n\n"
        "Virtio9PFS-handler mounts QEMU host-shared folders as native "
        "AmigaOS volumes via VirtIO 9P.  It is intended for AmigaOS "
        "4.1 Final Edition systems running inside QEMU and serves no "
        "purpose on real hardware.\n\n"
        "The following changes will be made to your system:\n\n"
        "    1.  Virtio9PFS-handler will be copied to \"L:\"\n\n"
        "    2.  The \"SHARED\" DOSDriver will be copied to "
        "\"DEVS:DOSDrivers\"\n\n"
        "A system restart is required to activate the SHARED: "
        "volume.\n\n"
        "Click \"View Readme\" below for manual installation details, "
        "the QEMU shared-folder setup, and general instructions on "
        "use.\n\n\n"
        "Press \"Next\" to continue."),
    LocaleString(
        "MSG_README_BUTTON",
        "View Readme..."),
    LocaleString(
        "MSG_FINISH",
        "\nThe installation completed successfully.\n\n"
        "Virtio9PFS-handler has been copied to \"L:\" and the "
        "\"SHARED\" DOSDriver to \"DEVS:DOSDrivers\".  The SHARED: "
        "volume will appear after the next system restart.\n\n"
        "Please ensure QEMU is started with a shared folder:\n\n"
        "    -virtfs local,path=/host/folder,mount_tag=SHARED,"
        "security_model=none,id=share0\n\n"
        "The mount_tag must match the DOSDriver name (SHARED).  If "
        "QEMU is started without a 9P device, the handler declines "
        "the mount silently and boot continues normally -- the "
        "DOSDriver can stay installed permanently.\n\n"
        "Please note: when restarting from within QEMU, the virtual "
        "machine may power off instead of restarting.  Should this "
        "occur, simply start QEMU again.\n\n\n"
        "Press \"Finish\" to exit the installation."),
    LocaleString(
        "MSG_REBOOT",
        "Restart the system now (required to activate the SHARED: volume)"),
]


# Welcome page is a GUI page (same rendered look as WELCOME) so it can
# carry a "View Readme" button -- U2's kicklayout-page button idiom:
# AddButton onclick handler launching NotePad on the bundled readme.
welcome_page = Page(
    var_name="welcomePage",
    kind=PageKind.GUI,
    on_click_handlers=[
        Handler(
            name="readmeLaunch",
            params=["page", "id"],
            body=(
                "amiga.system('notepad *>NIL: \"README\"')\n"
                "return True\n"
            ),
        ),
    ],
    gui=GuiBlock(
        orientation=GroupOrientation.VERTICAL,
        children=[
            GuiWidget(kind=WidgetKind.LABEL,
                      label=LocaleRef("MSG_WELCOME")),
            GuiBlock(
                orientation=GroupOrientation.HORIZONTAL,
                weight=0,
                children=[
                    GuiWidget(kind=WidgetKind.SPACE, weight=1),
                    GuiWidget(
                        kind=WidgetKind.BUTTON,
                        frame=Frame.BUTTON,
                        label=LocaleRef("MSG_README_BUTTON"),
                        onclick="readmeLaunch",
                        weight=10,
                    ),
                    GuiWidget(kind=WidgetKind.SPACE, weight=1),
                ],
            ),
            GuiWidget(kind=WidgetKind.SPACE),
        ],
    ),
)

install_page = Page(
    var_name="installPage",
    kind=PageKind.INSTALL,
)

finish_page = Page(
    var_name="finishPage",
    kind=PageKind.FINISH,
    strings={"message": LocaleRef("MSG_FINISH")},
)


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
    version="0.10.0",
    date="11.06.2026",
    locale_strings=locale,
    pages=[welcome_page, install_page, finish_page],
    packages=[handler_package, dosdriver_package],
    post_install_actions=[reboot_action],
)
