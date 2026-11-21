# Regras de segurança do console (OBRIGATÓRIO para todo agente)

Vale para Claude Code, Antigravity (agy), omp (oh-my-pi), Cursor e qualquer outro harness.
Este projeto roda código nativo num PS5 com jailbreak. Um erro pode travar, corromper ou inutilizar o console.
Estas regras têm prioridade sobre velocidade ou conveniência.

## 1. Nunca quebrar o console
- NUNCA escrever, apagar, renomear ou montar como escrita nada fora de `/data/` no console
  (ex.: `/system`, `/system_ex`, `/preinst`, `/update`, `/mnt/sandbox`, partições flash, registry, `/user/home` de outros apps).
- NUNCA alterar firmware, NVS/registry, bootloader, chaves, licenças, `param.sfo`/`param.json` de outros apps, nem disparar update/restore.
- Instalação/teste só em caminhos de desenvolvimento próprios (ex.: `/data/ps5term-dev/`).
- NUNCA executar no console comandos destrutivos (`rm -rf` amplo, `dd`, `newfs`, `mount -u -w`, `sysctl -w` de kernel) sem pedido explícito do usuário na mensagem atual.

## 2. Evitar ao máximo kernel panic
- Preferir sempre caminho em userland. Evitar kernel R/W, patches de kernel, `kexec`, hooks de syscall, escrita em memória do kernel, ucred/jail escapes novos.
- Se for indispensável tocar em kernel/memória privilegiada: explicar o risco, propor alternativa userland e pedir confirmação ANTES.
- Validar ponteiros, tamanhos e retornos de toda chamada nativa (`sceKernel*`, `ioctl`, `mmap`, `sysctl`); nunca ignorar erro.
- Sem loops apertados sem limite, alocações sem teto ou threads sem saída em payloads.
- Mudança arriscada: primeiro reproduzir/testar no host (build, testes, emulação); no console, um passo pequeno por vez, com log.

## 3. Evitar ao máximo mexer em arquivos nativos
- Não editar sem necessidade comprovada: `sce_sys/` (inclui `libc.prx` e seu `.sha256`), `third_party/`, `src/platform/`, `tooling/native/`, binários (`*.prx`, `*.sprx`, `*.elf`, `*.self`, `*.bin`), `.deps/`, `build/`, `dist/`.
- Preferir corrigir na camada mais alta possível (app, scripts, config) a alterar código nativo/vendorizado.
- Se a mudança nativa for inevitável: mudança mínima, justificar o motivo, e avisar o usuário explicitamente no resumo.
- NUNCA substituir ou regenerar binários nativos/assinados sem pedido explícito.

## Na dúvida
Pare e pergunte. Um console travado custa muito mais que uma pergunta.
