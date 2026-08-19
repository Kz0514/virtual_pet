"""Upload firmware to server and register in DB for OTA."""
import paramiko, sys, os, hashlib

HOST = os.environ.get("VPS_HOST", "virtualpet.top")
USER = os.environ.get("VPS_USER", "root")
PASS = os.environ.get("VPS_PASSWORD", "")
REMOTE_DIR = "/opt/virtualpet-server/firmware"
LOCAL_BIN = sys.argv[1] if len(sys.argv) > 1 else "build/Virtualpet.bin"

# Read version from version.txt in project root
ver_file = os.path.join(os.path.dirname(__file__), "..", "version.txt")
try:
    with open(ver_file) as f:
        version = f.read().strip()
except Exception:
    version = "1.0.0"

changelog = sys.argv[2] if len(sys.argv) > 2 else "OTA update"

# Compute SHA256
sha = hashlib.sha256()
with open(LOCAL_BIN, 'rb') as f:
    while True:
        chunk = f.read(65536)
        if not chunk: break
        sha.update(chunk)
sha256 = sha.hexdigest()
file_size = os.path.getsize(LOCAL_BIN)

print(f"Firmware: v{version}  size={file_size}  sha256={sha256[:16]}...")

# Upload
ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh.connect(HOST, username=USER, password=PASS, timeout=15)
sftp = ssh.open_sftp()
sftp.put(LOCAL_BIN, f"{REMOTE_DIR}/{version}.bin")
sftp.close()
print("Uploaded.")

# Register in DB — deactivate all first, then insert new
sql = f"""
UPDATE firmwares SET is_active = false;
INSERT INTO firmwares (version, changelog, file_url, file_size, sha256_hash, is_active, created_at)
VALUES ('{version}', '{changelog}', '/api/v1/ota/download/{version}', {file_size}, '{sha256}', true, NOW())
ON CONFLICT (version) DO UPDATE SET is_active=true, created_at=NOW(),
    changelog='{changelog}', file_size={file_size}, sha256_hash='{sha256}';
"""
cmd = f'docker exec virtualpet-server-postgres-1 psql -U virtualpet -d virtualpet -c "{sql}"'
stdin, stdout, stderr = ssh.exec_command(cmd)
print(stdout.read().decode())
err = stderr.read().decode()
if err:
    print("STDERR:", err)
ssh.close()
print(f"Done. ESP32 will detect v{version} on next OTA check.")


