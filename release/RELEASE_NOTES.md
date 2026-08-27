# AmethystTool v1.0.0

## O que é
Fork independente do BetterSteamTools, focado em privacidade, segurança e
controle do usuário. Renomeado para evitar conflitos e dependências do upstream.

## Mudanças principais
- **Renomeação completa:** OpenSteamTool → AmethystTool (arquivos, DLL, pastas, scheme URI)
- **Auto-update desativado:** não há mais risco de RCE via infraestrutura upstream
- **Telemetria desativada:** não envia mais dados de jogos para servidores do projeto original
- **RemoteToml cache-first:** não faz phone-home a cada inicialização
- **Protocol handler condicionado:** `amethysttool://` só é registrado se o servidor de código for configurado
- **Build hardening:** `/W4 /permissive- /w14062`, warnings reduzidos de 141 → 11
- **39 testes unitários:** cobrindo parsers críticos de entrada externa
- **Migração automática de config:** `opensteamtool.toml` → `amethysttool.toml`

## Requisitos
- Windows 10/11 64-bit
- Steam (qualquer versão recente)
- Visual C++ Runtime **não é necessário** (CRT estático incluído)

## Instalação
Veja `INSTALL.txt` no pacote zip ou `README.md` no repositório.

## Aviso
Use apenas em conta Steam descartável. Este plugin manipula DRM e pode
resultar em banimento da conta. O autor não se responsabiliza por
consequências do uso.

## Build
- **Release:** 11 warnings (todos C4100 pré-existentes)
- **Debug:** 10 warnings
- **Testes:** 39/39 passando
- **ABI:** preservada (`TokeerUri` ordinal 1)
- **CRT:** estático (`/MT`)
