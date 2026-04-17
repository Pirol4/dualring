# POC_DualRingProject
The codes for my conclusion project research

# Como rodar!

## 1. Compile
make

## 2. No servidor (máquina 1):
sudo ./server -l 0-1 -n 4 -- 

## 3. No cliente (máquina 2):
sudo ./client -l 0-1 -n 4 --

## Ajuste os endereços MAC em common.h antes de compilar!