#!/bin/bash
# ============================================
# Virtualpet Server 一键部署脚本
# 适用: Debian 11 + 宝塔面板
# 用法: bash deploy.sh
# ============================================
set -e

echo "========================================"
echo " Virtualpet Server 部署"
echo "========================================"

# 1. 检查 Docker
if ! command -v docker &> /dev/null; then
    echo "[1/5] Docker 未安装, 正在安装..."
    curl -fsSL https://get.docker.com | bash
    systemctl enable docker && systemctl start docker
else
    echo "[1/5] Docker 已安装 ✓"
fi

# 2. 检查 Docker Compose
if ! docker compose version &> /dev/null; then
    echo "[2/5] 安装 Docker Compose 插件..."
    apt-get update && apt-get install -y docker-compose-plugin
else
    echo "[2/5] Docker Compose 已安装 ✓"
fi

# 3. 配置 .env
if [ ! -f .env ]; then
    echo "[3/5] 创建 .env 配置文件..."
    cp .env.example .env
    echo "  ⚠ 请编辑 .env 文件, 填入API Key后重新运行此脚本"
    echo "  nano .env"
    exit 1
else
    echo "[3/5] .env 已存在 ✓"
fi

# 4. 创建必要目录
echo "[4/5] 创建数据目录..."
mkdir -p postgres_data

# 5. 启动
echo "[5/5] 启动服务..."
docker compose up -d

echo ""
echo "========================================"
echo " 部署完成!"
echo ""
echo " 查看日志: docker compose logs -f app"
echo " 查看状态: docker compose ps"
echo " API文档 : http://$(hostname -I | awk '{print $1}'):8000/docs"
echo "========================================"


