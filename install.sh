; Virtio9PFS-handler installer
; Run from AmigaOS Shell: execute install.sh
; (cd to the directory containing this script first)

echo "Installing Virtio9PFS-handler..."

IF EXISTS Virtio9PFS-handler
  copy Virtio9PFS-handler L:Virtio9PFS-handler QUIET
  echo "  Copied handler to L:"
ELSE
  echo "  ERROR: Virtio9PFS-handler not found in current directory!"
  QUIT 20
ENDIF

IF EXISTS SHARED
  copy SHARED DEVS:DOSDrivers/SHARED QUIET
  echo "  Copied DOSDriver to DEVS:DOSDrivers/"
ELSE
  echo "  WARNING: SHARED DOSDriver not found, skipping."
ENDIF

echo ""
echo "Done. Reboot to activate the SHARED: volume."
