#!/bin/bash
# binfmt-guard: auto-heals if the FEX-x86_64 binfmt gets corrupted to catch NATIVE
# aarch64 ELF (a truncated magic missing the x86-64 e_machine 0x3E) -- which breaks
# ALL native exec system-wide and is otherwise unrecoverable from a normal shell
# (sudo itself won't exec). The DETECT+REMOVE path uses ONLY bash builtins
# ($(<file), [[ ]], echo>file) so it works even when exec is globally broken.
# Re-registration delegates to the official null-safe fex-binfmt.service.
# NEVER shell-printf a magic containing \x00 -- printf turns it into a real null
# that truncates the magic: that truncation IS the root cause of the break.
ENTRY=/proc/sys/fs/binfmt_misc/FEX-x86_64
LOG=/var/log/binfmt-guard.log
while : ; do
  if [[ -e "$ENTRY" ]]; then
    cur=$(<"$ENTRY")
    if [[ "$cur" != *3e00* ]]; then                      # magic doesn't constrain e_machine=x86-64 -> DANGER
      echo -1 > "$ENTRY" 2>/dev/null                     # 1) remove bad entry (builtin) -> native exec restored
      { echo "[+${SECONDS}s] removed DANGEROUS FEX-x86_64: ${cur//$'\n'/ }"; } >> "$LOG" 2>/dev/null
      systemctl restart fex-binfmt.service 2>/dev/null   # 2) re-register via official null-safe path
    fi
  fi
  sleep 2
done
