#!/bin/bash
# ============================================
# Script de instalação do Claude Code
# Ubuntu 20.04+ / Debian 10+
# ============================================

set -e

echo "🔧 Removendo repositórios com chave GPG inválida (ex: Grafana)..."
sudo rm -f /etc/apt/sources.list.d/grafana*.list

echo "📦 Instalando Node.js 20 LTS..."
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
sudo apt install -y nodejs

echo "✅ Versão do Node.js: $(node -v)"
echo "✅ Versão do npm: $(npm -v)"

echo "🤖 Instalando Claude Code..."
npm install -g @anthropic-ai/claude-code

echo "✅ Versão do Claude Code: $(claude --version)"
echo ""
echo "🚀 Instalação concluída! Execute 'claude' para iniciar."
