"""Deploy server update to production."""
import paramiko
import tarfile
import io
import os
import sys
import time

HOST = os.environ.get("VPS_HOST", "virtualpet.top")
USER = os.environ.get("VPS_USER", "root")
PASS = os.environ.get("VPS_PASSWORD", "")
REMOTE_DIR = "/opt/virtualpet-server"
SERVER_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "server")
BUILD_LOG = "/tmp/vps_build.log"
BUILD_TIMEOUT_SEC = 1200   # 20 分钟

def run_ssh(ssh, cmd, desc="", timeout=600):
    print(f"\n>>> {desc or cmd}")
    stdin, stdout, stderr = ssh.exec_command(cmd, timeout=timeout)
    for line in stdout:
        print(f"    {line.rstrip()}")
    for line in stderr:
        print(f"    [stderr] {line.rstrip()}")
    exit_code = stdout.channel.recv_exit_status()
    if exit_code != 0:
        print(f"    !! exit code: {exit_code}")
        return False
    return True

def run_remote_background(ssh, cmd):
    """远端 nohup 后台执行 — 不依赖 SSH 通道存活, 本地超时不影响远端构建."""
    stdin, stdout, stderr = ssh.exec_command(
        f"nohup sh -c '{cmd}' > {BUILD_LOG} 2>&1 &", timeout=30)
    stdout.channel.recv_exit_status()
    time.sleep(2)

def poll_build_done(ssh):
    for i in range(BUILD_TIMEOUT_SEC // 20):
        time.sleep(20)
        _, out, _ = ssh.exec_command(f"tail -3 {BUILD_LOG}", timeout=20)
        tail = out.read().decode(errors="replace")
        if "BUILD_DONE" in tail:
            print("    远端构建完成")
            return True
        if "BUILD_FAILED" in tail:
            return False
    return False

# ── Create tar in memory ──
print("=== Creating archive...")
buf = io.BytesIO()
with tarfile.open(fileobj=buf, mode='w:gz') as tar:
    for root, dirs, files in os.walk(SERVER_DIR):
        for f in files:
            if f in ("api.json", ".env"):
                continue  # NEVER overwrite server's api.json / .env
            full = os.path.join(root, f)
            arcname = os.path.relpath(full, SERVER_DIR).replace('\\', '/')
            tar.add(full, arcname=arcname)
            print(f"  + {arcname}")
buf.seek(0)
print(f"Archive size: {len(buf.getvalue())} bytes")

# ── Connect ──
print(f"\n=== Connecting to {HOST}...")
ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh.connect(HOST, username=USER, password=PASS, timeout=15)
print("SSH connected.")

# ── Upload ──
print("\n>>> Uploading archive to /tmp/server-update.tar.gz ...")
sftp = ssh.open_sftp()
sftp.putfo(buf, "/tmp/server-update.tar.gz")
sftp.close()
print("Upload complete.")

# ── Extract ──
run_ssh(ssh, f"cd {REMOTE_DIR} && tar -xzf /tmp/server-update.tar.gz && echo 'Extracted'", "Extract archive")

# ── 远端后台构建 — 旧容器继续服务, 不停机 (构建约 5-10 分钟) ──
print(f"\n>>> 远端后台构建 (日志 {BUILD_LOG}, 旧容器保持运行)...")
run_remote_background(ssh,
    f"cd {REMOTE_DIR} && docker compose build --no-cache "
    f"&& echo BUILD_DONE >> {BUILD_LOG} || echo BUILD_FAILED >> {BUILD_LOG}")
if not poll_build_done(ssh):
    print("!! 构建失败或超时 — 旧容器仍在运行, 服务未中断。请检查远端 " + BUILD_LOG)
    ssh.close()
    sys.exit(1)

# ── 快速切换 (构建完成后才 down/up, 停机窗口最短) ──
run_ssh(ssh, f"cd {REMOTE_DIR} && docker compose down", "Stop containers")
run_ssh(ssh, f"cd {REMOTE_DIR} && docker compose up -d", "Start containers")

# ── Wait & Check ──
time.sleep(5)
run_ssh(ssh, f"cd {REMOTE_DIR} && docker compose ps", "Container status")

# ── Cleanup (维护约定: 部署后清理旧镜像与构建产物) ──
run_ssh(ssh, "docker image prune -f", "Clean dangling images")
run_ssh(ssh, "docker builder prune -f", "Clean build cache")

# ── Test LLM ──
print("\n=== Testing LLM service ===")
stdin, stdout, stderr = ssh.exec_command(
    f"cd {REMOTE_DIR} && docker compose exec -T app python -c "
    "\"from app.services.llm_service import chat; "
    "import asyncio; "
    "print('LLM Reply:', asyncio.run(chat('你好')))\""
)
for line in stdout:
    print(f"  {line.rstrip()}")
for line in stderr:
    print(f"  {line.rstrip()}")
exit_code = stdout.channel.recv_exit_status()
print(f"  exit: {exit_code}")

ssh.close()
print("\n=== Deploy complete ===")
