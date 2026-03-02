.KEY DRIVE/K
.BRA {
.KET }

; Virtio9PFS-handler installer script
; Run from AmigaOS Shell: execute USB0:install.sh
; Or with a different source: execute USB0:install.sh DRIVE=USB1:

IF "{DRIVE}" EQ ""
  SET SOURCE "USB0:"
ELSE
  SET SOURCE "{DRIVE}"
ENDIF

echo "Installing Virtio9PFS-handler from $SOURCE"

IF EXISTS ${SOURCE}Virtio9PFS-handler
  copy ${SOURCE}Virtio9PFS-handler L:Virtio9PFS-handler QUIET
  echo "  Copied handler to L:"
ELSE
  echo "  ERROR: ${SOURCE}Virtio9PFS-handler not found!"
  QUIT 20
ENDIF

IF EXISTS ${SOURCE}SHARED.DOSDriver
  copy ${SOURCE}SHARED.DOSDriver DEVS:DOSDrivers/SHARED QUIET
  echo "  Copied DOSDriver to DEVS:DOSDrivers/"
ELSE
  echo "  WARNING: ${SOURCE}SHARED.DOSDriver not found, skipping."
ENDIF

echo "Done. Reboot to activate the handler."
