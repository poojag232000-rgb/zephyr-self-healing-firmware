# Development signing key

Generate a local ECDSA-P256 development key before the MCUboot build:

```powershell
.\.venv\Scripts\python.exe bootloader\mcuboot\scripts\imgtool.py keygen `
  -k keys\dev-signing.pem -t ecdsa-p256
```

The private key is intentionally ignored by Git. Production signing keys must
be held outside the source repository and protected by an appropriate key
management system.
