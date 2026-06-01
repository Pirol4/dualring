#!/bin/bash
set -e

echo "==> Instalando nvm..."
curl -o- https://raw.githubusercontent.com/nvm-sh/nvm/v0.39.7/install.sh | bash

export NVM_DIR="$HOME/.nvm"
[ -s "$NVM_DIR/nvm.sh" ] && \. "$NVM_DIR/nvm.sh"

echo "==> Instalando Node.js 18..."
nvm install 18
nvm use 18

echo "==> Instalando Claude Code..."
npm install -g @anthropic-ai/claude-code

echo ""
echo "✅ Pronto! Rode 'claude' para autenticar."

## Como usar?
## curl -o- https://raw.githubusercontent.com/nvm-sh/nvm/v0.39.7/install.sh | bash  # já feito
## chmod +x setup.sh
## ./setup.sh
